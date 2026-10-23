import unittest
from ulv.detection import detect_packet
from ulv.registry import DeviceRegistry

class E2DetectionTests(unittest.TestCase):
    def test_msop_on_custom_port(self):
        result = detect_packet(bytes.fromhex('55aa5aa5') + bytes(840), 45678)
        self.assertEqual((result.vendor, result.model), ('RoboSense', 'RSE2'))

    def test_calibration_on_custom_port_shares_device(self):
        data = bytes.fromhex('a5ff00ae') + bytes(1292)
        result = detect_packet(data, 45679)
        self.assertIsNotNone(result)
        self.assertEqual((result.vendor, result.model), ('RoboSense', 'RSE2'))
        registry = DeviceRegistry()
        a = registry.observe('10.0.0.2', 6699, 45678, 844, detect_packet(bytes.fromhex('55aa5aa5') + bytes(840),45678))
        b = registry.observe('10.0.0.2', 7766, 45679, 1296, result)
        self.assertEqual(a.id, b.id)

    def test_em4_calibration_is_not_e2(self):
        result = detect_packet(bytes.fromhex('a5ff00ae') + bytes(1158), 45679)
        self.assertTrue(result is None or result.model != 'RSE2')

from pathlib import Path
import tempfile
import threading
from ulv.native_plugins import NativePluginManager, CPoint
from ulv.protocol import decode_header, HEADER

ROOT = Path(__file__).resolve().parents[1]
E2_PLUGINS = [p for p in (ROOT/'plugins').glob('*robosense*') if p.suffix in ('.so','.dylib','.dll')]

def e2_packet(frame, dual=False):
    p = bytearray(844)
    p[:4] = bytes.fromhex('55aa5aa5')
    p[4:6] = (1).to_bytes(2,'big')
    p[6] = 0 if dual else 1
    p[13] = 12
    p[20:23] = bytes((frame,10,10))
    for i in range(200):
        p[32+i*4:34+i*4] = (4000 if dual and i%2 else 2000).to_bytes(2,'big')
        p[34+i*4] = 128
    return bytes(p)

@unittest.skipUnless(E2_PLUGINS, 'build the RoboSense native plugin to run E2 integration')
class E2PluginTests(unittest.TestCase):
    def test_plugin_to_browser_wire_protocol(self):
        import shutil
        frames=[]
        event=threading.Event()
        def emit(f):
            frames.append(f)
            event.set()
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            shutil.copyfile(E2_PLUGINS[0],directory/E2_PLUGINS[0].name)
            manager=NativePluginManager(directory,emit,calibration_cache_directory=directory/'cache')
            registry=DeviceRegistry()
            try:
                self.assertEqual(manager.errors,[])
                for frame in range(2):
                    payload=e2_packet(frame,True)
                    device=registry.observe('10.0.0.2',6699,45678,len(payload),detect_packet(payload,45678))
                    manager.feed(device,payload,12000000000,6699,45678)
                self.assertTrue(event.wait(3),manager.status())
                _,_,count,ts=decode_header(frames[0])
                self.assertEqual(count,200)
                self.assertEqual(ts,12000000000)
                points=(CPoint*count).from_buffer_copy(frames[0][HEADER.size:])
                self.assertAlmostEqual(points[1].x,2*points[0].x,places=4)
                self.assertEqual((points[0].return_type,points[1].return_type),(1,2))
            finally:
                manager.close()

    def test_pcap_replay_to_points(self):
        import shutil
        import struct
        import time
        from ulv.pcap_replay import PcapReplay
        frames=[]
        event=threading.Event()
        def emit(f):
            frames.append(f)
            event.set()
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            shutil.copyfile(E2_PLUGINS[0],directory/E2_PLUGINS[0].name)
            manager=NativePluginManager(directory,emit,calibration_cache_directory=directory/'cache')
            registry=DeviceRegistry()
            replay=PcapReplay(registry,manager.feed)
            content=bytearray(struct.pack('<IHHIIII',0xa1b2c3d4,2,4,0,0,65535,1))
            for index in range(2):
                payload=e2_packet(index)
                ethernet=bytes.fromhex('00112233445566778899aabb0800')
                ip=struct.pack('!BBHHHBBH4s4s',0x45,0,28+len(payload),0,0,64,17,0,bytes((10,0,0,2)),bytes((10,0,0,1)))
                udp=struct.pack('!HHHH',6699,45678,len(payload)+8,0)
                raw=ethernet+ip+udp+payload
                content+=struct.pack('<IIII',12,index*1000,len(raw),len(raw))+raw
            path=directory/'synthetic-e2.pcap'
            path.write_bytes(content)
            try:
                replay.start(path,path.name,loop=False)
                self.assertTrue(event.wait(3))
                deadline=time.monotonic()+1
                while replay.status()['active'] and time.monotonic()<deadline:
                    time.sleep(.005)
                self.assertEqual(replay.status()['state'],'completed')
                self.assertEqual(replay.status()['accepted_packets'],2)
                self.assertEqual(decode_header(frames[0])[2],200)
                self.assertEqual(registry.list_public()[0]['model'],'RSE2')
            finally:
                replay.stop()
                manager.close()
