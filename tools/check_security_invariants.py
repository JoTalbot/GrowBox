#!/usr/bin/env python3
"""Static regression checks for GrowBox transport and recovery-AP security invariants.

These checks complement (but do not replace) ESP32 hardware certificate/OTA tests.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
REMOTE = ROOT / "firmware/Growbox_CriticalKush_V6/RemoteAgent.ino"
SERVICE = ROOT / "firmware/Growbox_OTA_Service/Growbox_OTA_Service.ino"


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


remote = REMOTE.read_text(encoding="utf-8")
service = SERVICE.read_text(encoding="utf-8")

require("setInsecure(" not in remote, "remote agent does not disable TLS verification")
require("setCACertBundle(x509_crt_bundle)" in remote,
        "remote HTTPS configures the maintained CA bundle")
require("if (now < 1735689600L) return false;" in remote,
        "TLS setup fails closed when system time is unavailable")
require("if (!configureVerifiedTls(secureClient))" in remote,
        "OTA refuses to start when verified TLS setup fails")
require('url.startsWith("https://")' in remote,
        "remote download paths require HTTPS")
require("String serviceApPassword()" in service and "ESP.getEfuseMac()" in service,
        "service AP password is derived per device")
require('WiFi.softAP(apSsid.c_str(), apPassword.c_str())' in service,
        "recovery AP uses the derived SSID/password")
require('"12345678"' not in service,
        "service AP has no universal default password")

print("Static security regression checks passed; hardware acceptance remains required.")
