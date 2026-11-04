#!/usr/bin/env python3
"""Portable structural checks for deployment templates."""

from __future__ import annotations

import configparser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVICE = ROOT / "deploy/systemd/libre-desk-bridge.service"
TUNNEL = ROOT / "deploy/cloudflared/config.yml.example"
TUNNEL_ENV = ROOT / "deploy/cloudflared/cloudflared.env.example"
COMPOSE_TUNNEL = ROOT / "deploy/docker-compose.tunnel.yml"


def check_service() -> None:
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    with SERVICE.open(encoding="utf-8") as handle:
        parser.read_file(handle)
    assert parser.sections() == ["Unit", "Service", "Install"]
    service = parser["Service"]
    assert service["User"] == "libre-desk"
    assert service["Group"] == "libre-desk"
    assert service["WorkingDirectory"] == "/opt/libre-desk"
    assert service["EnvironmentFile"] == "/etc/libre-desk/bridge.env"
    assert service["ExecStart"] == "/usr/bin/node /opt/libre-desk/dist/bridge/src/main.js"
    assert service["Restart"] == "always"
    assert service["NoNewPrivileges"] == "true"
    assert service["ProtectSystem"] == "strict"
    assert service["ProtectHome"] == "true"
    assert service["CapabilityBoundingSet"] == ""
    assert parser["Install"]["WantedBy"] == "multi-user.target"


def check_tunnel() -> None:
    text = TUNNEL.read_text(encoding="utf-8")
    assert text.count("REPLACE_WITH_TUNNEL_UUID") == 2
    assert "hostname: REPLACE_WITH_STABLE_HOSTNAME" in text
    assert "service: http://127.0.0.1:18080" in text
    assert "service: http_status:404" in text
    assert text.index("service: http://127.0.0.1:18080") < text.index("service: http_status:404")
    assert "trycloudflare.com" not in text
    assert "libre.example.com" not in text

    environment = TUNNEL_ENV.read_text(encoding="utf-8")
    assert "TUNNEL_TOKEN=replace-with-cloudflare-tunnel-token" in environment
    assert "TUNNEL_URL=http://bridge:8080" in environment

    compose = COMPOSE_TUNNEL.read_text(encoding="utf-8")
    assert "cloudflare/cloudflared:2026.6.1@sha256:" in compose
    assert compose.count("restart: unless-stopped") == 2
    assert "CLOUDFLARED_ENV_FILE" in compose
    assert "command: [\"tunnel\", \"--no-autoupdate\", \"run\"]" in compose
    assert "ports:" not in compose


if __name__ == "__main__":
    check_service()
    check_tunnel()
    print("deployment template tests: PASS")
