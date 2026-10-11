#!/usr/bin/env python3
"""Static regression checks for GrowBox relay mapping and Telegram keyboards."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW_PATH = ROOT / "firmware/Growbox_CriticalKush_V6/Growbox_CriticalKush_V6.ino"
fw = FW_PATH.read_text(encoding="utf-8")
failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def keyboard(name: str) -> dict:
    match = re.search(rf"String {re.escape(name)}\(\)\s*\{{", fw)
    if not match:
        raise AssertionError(f"missing keyboard function: {name}")
    end = fw.find("\n}", match.end())
    if end < 0:
        raise AssertionError(f"unterminated keyboard function: {name}")
    block = fw[match.start():end]
    fragments = []
    for line in block.splitlines():
        literal = re.search(r'"(?:\\.|[^"\\])*"', line)
        if literal:
            fragments.append(json.loads(literal.group(0)))
    return json.loads("".join(fragments))


# Relay mapping and safety invariants.
check(bool(re.search(r"#define RELAY_LIGHT\s+16\b", fw)), "legacy light relay must be GPIO16")
check(bool(re.search(r"#define RELAY_LIGHT_CONTROL\s+19\b", fw)), "controllable light relay must be GPIO19")
check("setRelay(RELAY_LIGHT_CONTROL, stateLight)" in fw, "GPIO19 must follow the logical light state")
check(fw.count("setRelay(RELAY_LIGHT, true)") >= 3, "GPIO16 must be energized during boot and control updates")
check(not re.search(r"setRelay\(RELAY_LIGHT,\s*false\)", fw), "GPIO16 must never be switched off by runtime logic")
check("GPIO21" not in fw, "removed humidifier GPIO21 must not be referenced")
release_meta = json.loads((ROOT / "firmware/remote/release-version.json").read_text(encoding="utf-8"))
check('#define FIRMWARE_VERSION  "' + release_meta.get("version", "") + '"' in fw,
      "normal firmware version must match release metadata")
check('#define FIRMWARE_VERSION  "' + release_meta.get("version", "") + '-dry"' in fw,
      "dry firmware version must match release metadata")

legacy_symbols = re.compile(
    r"\b(RELAY_FAN|RELAY_HUMIDIFIER|stateFan|modeFan|stateHumid|modeHumid|"
    r"enableHumidifier|computeHumidAuto|windOnMs|windOffMs|windTimer|windState)\b|"
    r"humidEn|mode:fan:|mode:humid:|/fan\b|/humid\b",
    re.IGNORECASE,
)
check(not legacy_symbols.search(fw), "obsolete fan/humidifier firmware symbols or commands remain")

# Every inline keyboard must be valid JSON and every callback must have a handler.
keyboard_names = [
    "tgMainKeyboard", "tgSettingsInlineKeyboard", "tgStatusInlineKeyboard",
    "tgControlInlineKeyboard", "tgWaterInlineKeyboard", "tgStageInlineKeyboard",
    "tgSensorsInlineKeyboard", "tgUpdateInlineKeyboard", "tgServiceInlineKeyboard",
    "tgCameraInlineKeyboard",
]
payloads = {}
try:
    payloads = {name: keyboard(name) for name in keyboard_names}
except (AssertionError, json.JSONDecodeError) as exc:
    failures.append(f"invalid Telegram keyboard JSON: {exc}")

main_buttons = [
    button for row in payloads.get("tgMainKeyboard", {}).get("keyboard", [])
    for button in row
]
check(len(main_buttons) == 2, "main Telegram menu should contain only Status and Settings")
check(not any(re.search(r"управление|камера", label, re.IGNORECASE) for label in main_labels),
      "Control and Camera must not appear in the main Telegram menu")
main_labels = [str(button) for button in main_buttons]
check(any("Статус" in label for label in main_labels), "main menu must expose overall status")
check(any("Настройки" in label for label in main_labels), "main menu must expose settings")
check(not any(re.search(r"полив|всё auto|датчики|обновления|сервис|режим", label, re.IGNORECASE) for label in main_labels),
      "secondary sections must be nested under Settings and the watering menu removed")
check("settings:stage" in str(payloads.get("tgSettingsInlineKeyboard", {})), "settings must contain the grow-mode route")
check("settings:sensors" in str(payloads.get("tgSettingsInlineKeyboard", {})), "settings must contain the sensor route")
check("settings:update" in str(payloads.get("tgSettingsInlineKeyboard", {})), "settings must contain the update route")
check("settings:service" in str(payloads.get("tgSettingsInlineKeyboard", {})), "settings must contain the service route")
control_buttons = [button for row in payloads.get("tgControlInlineKeyboard", {}).get("inline_keyboard", []) for button in row]
check(not any(re.search(r"всё auto|статус|главное меню", str(button), re.IGNORECASE) for button in control_buttons),
      "device controls must not contain All AUTO, Status or Main Menu buttons")
check("String tgControlText()" in fw and "relayModeStatus(" in fw,
      "device controls must show current relay mode and active state")
check('if (cmd == "📊 статус") { sendTelegramInline(tgStatusText(), tgStatusInlineKeyboard()); return; }' in fw,
      "overall status must show watering and relay controls inline")
check('editTelegramMessage(callbackMsgId, tgStatusText(), tgStatusInlineKeyboard())' in fw,
      "relay and watering callbacks must refresh the status message in-place")
status_start = fw.find("String tgStatusText() {")
status_end = fw.find("String tgControlText() {", status_start)
status_block = fw[status_start:status_end]
check('🌱 Стадия: <b>' in status_block and status_block.find('🌱 Стадия: <b>') < status_block.find('🌡️ Температура:'),
      "grow stage must appear before temperature in status")
check('🪴 Почва: ' in status_block and 'Зона ' not in status_block,
      "soil moisture zones must be compact and use numeric labels only")
check('🌡️ <b>Климат</b>' not in status_block and 'Реле и режимы' not in status_block,
      "status must omit Climate and relay/mode headings")

# Main reply-keyboard sections must all reach a real route.
for route in [
    'if (cmd == "📊 статус")',
    'if (cmd == "⚙️ настройки"',
    'callbackData == "settings:stage"',
    'callbackData == "settings:sensors"',
    'callbackData == "settings:update"',
    'callbackData == "settings:service"',
]:
    check(route in fw, f"main menu route missing: {route}")
check('sendTelegramInline(tgControlText(), tgControlInlineKeyboard())' in fw,
      "relay menu must open the real device controls with current state")
check('if (dev == "light" && mode == MODE_ON && thermalShutdown)' in fw,
      "legacy text controls must respect the light thermal interlock")

callback_values = []
for name, payload in payloads.items():
    for row in payload.get("inline_keyboard", []):
        for button in row:
            if "callback_data" in button:
                callback_values.append(button["callback_data"])

start = fw.find("int callbackIdx = update.indexOf")
end = fw.find("int chatIdx = update.indexOf", start)
handler = fw[start:end] if start >= 0 and end > start else ""
check(bool(handler), "Telegram callback handler region not found")
missing = []
for value in callback_values:
    if value.startswith("water:"):
        supported = f'callbackData == "{value}"' in handler
    elif value.startswith("mode:"):
        supported = 'callbackData.startsWith("mode:")' in handler
    elif value.startswith("stage:"):
        supported = 'callbackData.startsWith("stage:")' in handler
    else:
        supported = f'callbackData == "{value}"' in handler
    if not supported:
        missing.append(value)
check(not missing, "unhandled Telegram callback_data: " + ", ".join(sorted(set(missing))))
check(len(callback_values) >= 25, "expected inline Telegram controls were not discovered")
check('dev == "heater" && mode == MODE_ON && !heaterSafetyOk()' in handler,
      "Telegram heater button must report safety interlock")
check('dev == "light" && mode == MODE_ON && thermalShutdown' in handler,
      "Telegram light button must report thermal interlock")

# Keep removed controls out of all active UI/telemetry surfaces.
surfaces = {
    "firmware/Growbox_CriticalKush_V6/Admin.ino": r"stateFan|modeFan|stateHumid|modeHumid|d=fan|d=humid|увлажнитель|обдув",
    "firmware/Growbox_CriticalKush_V6/RemoteAgent.ino": r'"fan"|"humid"|stateFan|stateHumid|modeFan|modeHumid',
    "firmware/Growbox_CriticalKush_V6/MqttHa.ino": r"Обдув|Увлажнитель|fan_mode|humid_mode|stateFan|stateHumid|modeFan|modeHumid",
    "phone/index.html": r"pFan|mdFan|j\.fan|mFan|Обдув|Увлажнитель",
    "docs/remote.html": r"pFan|mdFan|j\.fan|mFan|Обдув|Увлажнитель",
    "homeassistant/growbox.yaml": r"GrowBox обдув|value_json\.fan|growbox_fan",
}
for relative, pattern in surfaces.items():
    text = (ROOT / relative).read_text(encoding="utf-8")
    check(not re.search(pattern, text, re.IGNORECASE), f"removed controls remain in {relative}")

try:
    manifest = json.loads((ROOT / "firmware/remote/version.json").read_text(encoding="utf-8"))
    release_meta = json.loads((ROOT / "firmware/remote/release-version.json").read_text(encoding="utf-8"))
    check(bool(manifest.get("version")), "OTA manifest version is missing")
    check("v" + manifest.get("version", "") + "/" in manifest.get("url", ""),
          "OTA manifest URL must match its published version")
    check(release_meta.get("version") == "6.5.27", "release metadata version mismatch")
    check(release_meta.get("version") != manifest.get("version") or
          "v" + release_meta.get("version", "") + "/" in manifest.get("url", ""),
          "new OTA manifest can only point at the release matching its version")
except (OSError, json.JSONDecodeError) as exc:
    failures.append(f"invalid OTA manifest: {exc}")

if failures:
    print("GrowBox static checks FAILED:")
    for failure in failures:
        print(f" - {failure}")
    sys.exit(1)

print(f"GrowBox static checks passed: {len(main_buttons)} main-menu buttons, "
      f"{len(set(callback_values))} unique inline callbacks, relay mapping and removed controls verified.")
