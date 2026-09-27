import unittest

from ulv.protocol import HEADER, POINT, Point, decode_header, encode_frame
from ulv.server import WebSocketHub, websocket_frame


class ProtocolTests(unittest.TestCase):
    def test_frame_round_trip(self):
        frame = encode_frame(42, 7, [Point(1, 2, 3, 0.5, 4)], timestamp_ns=123)
        self.assertEqual(decode_header(frame), (42, 7, 1, 123))
        self.assertEqual(len(frame), HEADER.size + POINT.size)

    def test_rejects_truncated_frame(self):
        frame = encode_frame(1, 1, [Point(0, 0, 0, 0)])
        with self.assertRaises(ValueError):
            decode_header(frame[:-1])

    def test_websocket_lengths(self):
        self.assertEqual(websocket_frame(b"abc")[:2], bytes((0x82, 3)))
        self.assertEqual(websocket_frame(bytes(126))[1], 126)
        self.assertEqual(websocket_frame(bytes(70000))[1], 127)

    def test_hub_replays_latest_point_frame_to_late_browser(self):
        class Client:
            def __init__(self):
                self.payloads = []

            def send(self, payload, opcode=2):
                self.payloads.append((payload, opcode))
                return True

        hub = WebSocketHub()
        hub.broadcast_binary(b"latest-point-frame")
        client = Client()
        hub.add(client)
        self.assertEqual(client.payloads, [(b"latest-point-frame", 2)])


if __name__ == "__main__":
    unittest.main()
