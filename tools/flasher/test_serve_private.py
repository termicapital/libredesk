from __future__ import annotations

import importlib.util
import tempfile
import threading
import unittest
from pathlib import Path
from typing import Any
from urllib.error import HTTPError
from urllib.request import Request, urlopen


SCRIPT = Path(__file__).with_name("serve_private.py")
SPEC = importlib.util.spec_from_file_location("flasher_serve_private", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load serve_private.py")
serve_private = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(serve_private)


class PrivateFlasherServerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "firmware").mkdir()
        (self.root / "vendor").mkdir()
        (self.root / "index.html").write_text("<title>private installer</title>", encoding="utf-8")
        (self.root / "manifest.json").write_text("{}", encoding="utf-8")
        (self.root / "release.json").write_text("{}", encoding="utf-8")
        (self.root / "firmware" / "app.bin").write_bytes(b"firmware")
        (self.root / "vendor" / "install.js").write_text("export {};", encoding="utf-8")
        (self.root / "not-public.txt").write_text("private", encoding="utf-8")
        self.token = "A" * 43
        handler = serve_private.make_handler(self.root, self.token)
        self.server = serve_private.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temporary.cleanup()

    def fetch(self, path: str, method: str = "GET") -> tuple[int, bytes, Any]:
        request = Request(self.base + path, method=method)
        try:
            with urlopen(request, timeout=2) as response:
                return response.status, response.read(), response.headers
        except HTTPError as error:
            return error.code, error.read(), error.headers

    def test_serves_only_the_capability_scoped_release_artifacts(self) -> None:
        prefix = f"/install/{self.token}"
        status, body, headers = self.fetch(prefix + "/")
        self.assertEqual((status, body), (200, b"<title>private installer</title>"))
        self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertEqual(headers["X-Content-Type-Options"], "nosniff")

        self.assertEqual(self.fetch(prefix + "/manifest.json")[:2], (200, b"{}"))
        self.assertEqual(self.fetch(prefix + "/firmware/app.bin")[:2], (200, b"firmware"))
        self.assertEqual(self.fetch(prefix + "/vendor/install.js")[:2], (200, b"export {};"))
        self.assertEqual(self.fetch(prefix + "/not-public.txt")[0], 404)
        self.assertEqual(self.fetch("/")[0], 404)
        self.assertEqual(self.fetch(prefix + "/%2e%2e/not-public.txt")[0], 404)

        head_status, head_body, head_headers = self.fetch(prefix + "/firmware/app.bin", "HEAD")
        self.assertEqual((head_status, head_body), (200, b""))
        self.assertEqual(head_headers["Content-Length"], str(len(b"firmware")))


if __name__ == "__main__":
    unittest.main()
