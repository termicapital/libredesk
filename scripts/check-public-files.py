#!/usr/bin/env python3
"""Reject private configuration and generated sensitive artifacts in Git's index."""

from pathlib import PurePosixPath
import subprocess
import sys

PRIVATE_NAMES = {
    "credentials.json", "device.json", "vercel-push.json", "mvp_secrets.h",
    "sdkconfig", "sdkconfig.old",
}
PRIVATE_DIRS = {
    ".vercel", "node_modules", "private", "secrets", ".secure", "coredumps",
    "coverage", "dist", "managed_components", "__pycache__",
}
PRIVATE_SUFFIXES = {
    ".bin", ".elf", ".hex", ".uf2", ".map", ".zip", ".tar", ".tgz",
    ".pem", ".key", ".p12", ".pfx", ".log", ".jsonl", ".db", ".sqlite",
    ".sqlite3", ".pyc", ".pyo",
}


def rejection(path: str) -> str | None:
    p = PurePosixPath(path)
    name = p.name.lower()
    if name in PRIVATE_NAMES or name.endswith("secrets.json"):
        return "private configuration"
    if any(part.lower() in PRIVATE_DIRS for part in p.parts):
        return "private or generated directory"
    if "build" in p.parts and p.parts[0] == "firmware":
        return "firmware build output"
    if name == ".env" or name.startswith(".env."):
        if name != ".env.example":
            return "environment configuration"
    if name.endswith(".env") or p.suffix.lower() in PRIVATE_SUFFIXES or name.endswith(".tar.gz"):
        return "private configuration or generated artifact"
    if path == "deploy/cloudflared/config.yml" or (
        path.startswith("deploy/cloudflared/") and p.suffix.lower() == ".json"
    ):
        return "tunnel configuration"
    return None


def main() -> int:
    paths = subprocess.check_output(["git", "ls-files", "--cached", "-z"]).decode().split("\0")
    failures = [(path, reason) for path in paths if path and (reason := rejection(path))]
    for path, reason in failures:
        print(f"Refusing to publish {path}: {reason}", file=sys.stderr)
    if failures:
        return 1
    print("Tracked-file publication boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
