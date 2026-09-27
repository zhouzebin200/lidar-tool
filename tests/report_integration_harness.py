"""Manual UI test harness: synthetic frames, isolated config, no network capture.

Run from project root: python tests/report_integration_harness.py
The production build does not include this harness or its synthetic data.
"""
from pathlib import Path
import math
import sys
import tempfile
import threading
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from ulv.server import Application, ViewerHTTPServer
from ulv.detection import Detection
from ulv.protocol import Point, encode_frame

if __name__=='__main__':
    with tempfile.TemporaryDirectory(prefix='lijian-report-qa-') as directory:
        config=Path(directory)
        app=Application('127.0.0.1',(),(),config/'runtime.json',config/'no-plugins',auto_discovery=False,auto_arp_assist=False,shutdown_when_browser_closes=False)
        server=ViewerHTTPServer(('127.0.0.1',17889),app,ROOT/'web')
        def feed():
            points=[Point(8*math.cos(i/40),8*math.sin(i/40),j/5-3,100,j) for i in range(252) for j in range(30)]
            sequence=0
            while True:
                device=app.registry.observe('192.0.2.1',2368,2368,1206,Detection('TEST ONLY','SYNTHETIC CLOUD',1.0,'Synthetic QA data; not a physical lidar'))
                app.hub.broadcast_binary(encode_frame(device.id,sequence,points))
                sequence+=1
                time.sleep(.1)
        threading.Thread(target=feed,daemon=True).start()
        print('Synthetic viewer QA: http://127.0.0.1:17889',flush=True)
        server.serve_forever()
