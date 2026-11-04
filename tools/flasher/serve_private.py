#!/usr/bin/env python3
"""Serve one immutable private ESP Web Tools release behind a capability path."""

from __future__ import annotations

import argparse
import json
import mimetypes
import re
import stat
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

TOKEN_PATTERN = re.compile(r"[A-Za-z0-9_-]{43}\Z")


def _read_stable_regular_file(path: Path) -> bytes:
    before = path.lstat()
    if path.is_symlink() or not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        raise SystemExit(f"release entry is not a single-link regular file: {path.name}")
    content = path.read_bytes()
    after = path.lstat()
    identity_before = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
    identity_after = (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
    if identity_after != identity_before or len(content) != before.st_size:
        raise SystemExit(f"release entry changed while loading: {path.name}")
    return content


def _release_snapshot(root: Path) -> dict[str, tuple[bytes, str]]:
    root = root.resolve(strict=True)
    candidates = [root / "index.html", root / "manifest.json", root / "release.json"]
    for directory_name, suffix in (("firmware", ".bin"), ("vendor", None)):
        directory = root / directory_name
        if directory.is_symlink() or not directory.is_dir():
            raise SystemExit(f"missing release directory: {directory_name}")
        candidates.extend(
            path for path in sorted(directory.rglob("*"))
            if path.is_file() and (suffix is None or path.suffix == suffix)
        )

    snapshot: dict[str, tuple[bytes, str]] = {}
    for path in candidates:
        relative = path.relative_to(root).as_posix()
        media_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        snapshot[relative] = (_read_stable_regular_file(path), media_type)
    return snapshot


def make_handler(root: Path, path_token: str) -> type[BaseHTTPRequestHandler]:
    if TOKEN_PATTERN.fullmatch(path_token) is None:
        raise ValueError("path token must be 43 base64url characters")
    snapshot = _release_snapshot(root)
    prefix = f"/install/{path_token}"
    routes = {f"{prefix}/{relative}": artifact for relative, artifact in snapshot.items()}
    routes[f"{prefix}/"] = snapshot["index.html"]

    class PrivateFlasherHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "LibreDeskInstaller/1"
        sys_version = ""

        def do_GET(self) -> None:
            self._serve(include_body=True)

        def do_HEAD(self) -> None:
            self._serve(include_body=False)

        def _serve(self, *, include_body: bool) -> None:
            try:
                path = unquote(urlsplit(self.path).path, errors="strict")
            except (UnicodeDecodeError, ValueError):
                self._send_not_found()
                return
            if path == prefix:
                self.send_response(308)
                self.send_header("Location", prefix + "/")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            artifact = routes.get(path)
            if artifact is None:
                self._send_not_found()
                return
            body, media_type = artifact
            self.send_response(200)
            self.send_header("Content-Type", media_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()
            if include_body:
                self.wfile.write(body)

        def _send_not_found(self) -> None:
            self.send_response(404)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, format: str, *args: object) -> None:
            del format, args

    return PrivateFlasherHandler


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--device-config", type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    if args.port < 1 or args.port > 65535:
        raise SystemExit("port must be in 1..65535")
    device = json.loads(args.device_config.read_text(encoding="utf-8"))
    token = device.get("installer_path_token")
    if not isinstance(token, str):
        raise SystemExit("device config has no installer_path_token")
    handler = make_handler(args.root, token)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(json.dumps({"status": "ready", "port": args.port}), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
