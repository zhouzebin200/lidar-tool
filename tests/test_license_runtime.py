from http.client import HTTPConnection
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from ulv.licensing import LicenseResult
from ulv.native_plugins import NativePluginManager
from ulv.server import ViewerHTTPServer


class LicenseRuntimeTests(unittest.TestCase):
    def test_decoder_and_queued_frames_stop_when_license_is_revoked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "decoder.dll").touch()
            authorize = Mock(return_value=True)
            emit = Mock()
            plugin = Mock(vendor="Hesai")
            with patch("ulv.native_plugins.LoadedPlugin", return_value=plugin) as load:
                manager = NativePluginManager(root, emit, calibration_cache_directory=root, authorize=authorize)
            callback = load.call_args.args[1]
            device = SimpleNamespace(vendor="Hesai")
            manager.feed(device, b"packet", 0, 1, 2)
            plugin.feed.assert_called_once()
            callback(b"frame")
            emit.assert_called_once_with(b"frame")
            authorize.return_value = False
            plugin.feed.reset_mock()
            emit.reset_mock()
            manager.feed(device, b"packet", 0, 1, 2)
            callback(b"queued-frame")
            plugin.feed.assert_not_called()
            emit.assert_not_called()
            manager.close()

    def test_http_rejects_static_api_and_replay_after_revocation(self):
        session = Mock()
        session.valid.return_value = False
        session.result = LicenseResult(False, "expired")
        app = SimpleNamespace(license_session=session)
        with tempfile.TemporaryDirectory() as directory:
            server = ViewerHTTPServer(("127.0.0.1", 0), app, Path(directory))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                for method, path in (("GET", "/"), ("GET", "/api/status"), ("GET", "/ws/points"), ("POST", "/api/pcap/replay")):
                    connection = HTTPConnection(*server.server_address, timeout=3)
                    connection.request(method, path)
                    response = connection.getresponse()
                    self.assertEqual(response.status, 403)
                    response.read()
                    connection.close()
            finally:
                server.shutdown()
                server.server_close()
                thread.join(3)


if __name__ == "__main__":
    unittest.main()
