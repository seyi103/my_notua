"""Guard the minimal firmware's local includes and offline dependency boundary."""

from pathlib import Path
import re

Import("env")

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
SOURCE_ROOTS = (PROJECT_DIR / "src", PROJECT_DIR / "include")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}

# IP network/application transports remain outside the firmware boundary. BLE is
# intentionally allowed through the single, pinned NimBLE dependency.
FORBIDDEN_INCLUDES = {
    "ArduinoOTA.h",
    "AsyncMqttClient.h",
    "ESPAsyncWebServer.h",
    "HTTPClient.h",
    "MQTT.h",
    "PubSubClient.h",
    "WebServer.h",
    "WiFi.h",
    "WiFiClient.h",
    "espMqttClient.h",
}
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)


def project_include_exists(source: Path, include: str) -> bool:
    candidates = (source.parent / include, *(root / include for root in SOURCE_ROOTS))
    return any(candidate.is_file() for candidate in candidates)


errors = []
for source in sorted((PROJECT_DIR / "src").rglob("*")):
    if source.suffix not in SOURCE_SUFFIXES:
        continue
    contents = source.read_text(encoding="utf-8")
    for delimiter, include in INCLUDE_PATTERN.findall(contents):
        if include in FORBIDDEN_INCLUDES:
            errors.append(f"{source.relative_to(PROJECT_DIR)}: forbidden network include <{include}>")
        if delimiter == '"' and not project_include_exists(source, include):
            errors.append(f'{source.relative_to(PROJECT_DIR)}: unresolved local include "{include}"')

if errors:
    print("Offline dependency check failed:")
    for error in errors:
        print(f"  - {error}")
    env.Exit(1)

print("Core dependency check: local includes resolve; WiFi/MQTT/HTTP/OTA absent")
