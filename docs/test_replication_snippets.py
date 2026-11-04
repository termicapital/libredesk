#!/usr/bin/env python3
"""Execute the private-material bootstrap embedded in SECOND_DEVICE.md with fixtures."""

from __future__ import annotations

import getpass
import hashlib
import io
import json
import os
import shlex
import stat
import tempfile
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/SECOND_DEVICE.md"
START = "python3 - <<'PY'\n"
END = "\nPY\n```"


def extract_snippet() -> str:
    text = DOC.read_text(encoding="utf-8")
    start = text.index(START) + len(START)
    end = text.index(END, start)
    return text[start:end]


def parse_environment(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        parsed = shlex.split(line, posix=True)
        assert len(parsed) == 1 and "=" in parsed[0]
        key, value = parsed[0].split("=", 1)
        values[key] = value
    return values


def main() -> None:
    snippet = extract_snippet()
    compile(snippet, str(DOC), "exec")
    follower_email = "follower@example.invalid"
    libre_password = 'fixture-$-"quote"-\\slash'
    ssid = "fixture-ssid"
    wifi_password = 'wifi-$-"quote"-\\slash'
    origin = "https://libre.example.invalid"

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory) / "checkout"
        secure = Path(directory) / "secure"
        (workspace / "firmware/main").mkdir(parents=True)
        previous_cwd = Path.cwd()
        previous_secure = os.environ.get("LIBRE_DESK_SECURE_DIR")
        output = io.StringIO()
        try:
            os.chdir(workspace)
            os.environ["LIBRE_DESK_SECURE_DIR"] = str(secure)
            with (
                patch("builtins.input", side_effect=[follower_email, ssid, origin]),
                patch.object(getpass, "getpass", side_effect=[libre_password, wifi_password]),
                redirect_stdout(output),
            ):
                exec(compile(snippet, str(DOC), "exec"), {"__name__": "__main__"})
        finally:
            os.chdir(previous_cwd)
            if previous_secure is None:
                os.environ.pop("LIBRE_DESK_SECURE_DIR", None)
            else:
                os.environ["LIBRE_DESK_SECURE_DIR"] = previous_secure

        credentials = json.loads((secure / "credentials.json").read_text(encoding="utf-8"))
        device = json.loads((secure / "device.json").read_text(encoding="utf-8"))
        environment = parse_environment(secure / "bridge.env")
        header = (workspace / "firmware/main/mvp_secrets.h").read_text(encoding="utf-8")

        assert credentials == {"libre_email": follower_email, "libre_password": libre_password}
        assert environment["LIBRE_EMAIL"] == follower_email
        assert environment["LIBRE_PASSWORD"] == libre_password
        assert environment["DEVICE_TOKEN_HASHES"] == device["device_token_sha256"]
        assert len(device["device_token"]) == 43
        assert len(device["installer_path_token"]) == 43
        assert hashlib.sha256(device["device_token"].encode("ascii")).hexdigest() == device["device_token_sha256"]
        assert origin in header and ssid in header
        assert wifi_password.replace("\\", "\\\\").replace('"', '\\"') in header
        for path in (*secure.iterdir(), workspace / "firmware/main/mvp_secrets.h"):
            assert stat.S_IMODE(path.stat().st_mode) == 0o600
        rendered = output.getvalue()
        for private_value in (follower_email, libre_password, ssid, wifi_password, origin, device["device_token"]):
            assert private_value not in rendered

    print("replication documentation snippet: PASS")


if __name__ == "__main__":
    main()
