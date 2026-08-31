#include "core/network/softApTransfer.h"

#if NOTUA_SOFTAP_HTTP_SPIKE
#include <WiFi.h>
#include "core/diag/log.h"
#include "core/power/watchdog.h"
#include "core/storage/imageStorage.h"
#include "core/storage/imageCatalog.h"
#include "core/storage/playlistStore.h"
#include "core/storage/transferSession.h"

namespace {
constexpr char PASSWORD[] = "notua-dev-2026"; // Development only; not a production secret.
constexpr uint16_t PORT = 80;
constexpr uint32_t IDLE_MS = 120000;
constexpr uint32_t LOG_BYTES = 256 * 1024;
constexpr size_t IMAGE_BYTES = 1920000;
WiFiServer server(PORT);
WiFiClient client;
notua::storage::ImageStorage storage;
bool requested = false, running = false, uploading = false;
uint32_t lastActivity = 0, started = 0, lastLogged = 0, expectedCrc = 0;
size_t remaining = 0;
String ssid;

int unusedCandidate(String* activeJson = nullptr) {
  notua::storage::CatalogEntry catalog[notua::storage::MAX_IMAGES];
  notua::storage::Playlist active{}; notua::storage::PlaylistStore playlists;
  if (!notua::storage::scanFixedCatalog(LittleFS, catalog) || !playlists.begin()
      || !playlists.loadActiveValidated(catalog, active)) { playlists.end(); return -1; }
  playlists.end(); bool used[notua::storage::MAX_IMAGES]{}; String json = "[";
  for (uint8_t i = 0; i < active.count; ++i) {
    used[active.slots[i]] = true; if (i) json += ','; json += active.slots[i];
  }
  json += ']'; if (activeJson) *activeJson = json;
  for (uint8_t slot = 0; slot < notua::storage::MAX_IMAGES; ++slot) if (!used[slot]) return slot;
  return -1;
}

void reply(int code, const String& json) {
  if (!client) return;
  client.printf("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
      code, code == 200 ? "OK" : "Error", json.length());
  client.print(json); client.flush();
}

void fail(int code, const char* reason) {
  const size_t received = storage.offset();
  if (storage.active()) storage.abort();
  uploading = false;
  notua::storage::releaseTransferSession(notua::storage::TransferOwner::softAp);
  reply(code, String("{\"ok\":false,\"bytesReceived\":") + received
      + ",\"crcMatch\":false,\"error\":\"" + reason + "\"}");
  logWarn("SOFTAP", "upload aborted: reason=%s bytes=%u", reason, received);
  client.stop();
}

bool readLine(String& line, uint32_t timeoutMs = 3000) {
  line = ""; const uint32_t waitStart = millis();
  while (millis() - waitStart < timeoutMs) {
    feedWatchdog();
    while (client.available()) {
      const char c = client.read(); lastActivity = millis();
      if (c == '\n') { line.trim(); return true; }
      if (c != '\r' && line.length() < 512) line += c;
    }
    if (!client.connected()) return false;
    delay(1);
  }
  return false;
}

void acceptRequest() {
  String line;
  if (!readLine(line)) { fail(400, "request-line"); return; }
  if (line == "GET /health HTTP/1.1") {
    while (readLine(line) && line.length()) {}
    reply(200, "{\"ok\":true}"); client.stop(); return;
  }
  int slot = -1;
  if (line.startsWith("PUT /images/") && line.endsWith(" HTTP/1.1"))
    slot = line.substring(12, line.length() - 9).toInt();
  size_t length = 0; bool haveLength = false, haveCrc = false;
  while (readLine(line) && line.length()) {
    const int colon = line.indexOf(':');
    if (colon < 0) continue;
    String name = line.substring(0, colon), value = line.substring(colon + 1); name.toLowerCase(); value.trim();
    if (name == "content-length") { length = strtoul(value.c_str(), nullptr, 10); haveLength = true; }
    if (name == "x-notua-crc32") { expectedCrc = strtoul(value.c_str(), nullptr, 16); haveCrc = true; }
  }
  if (slot < 0 || slot >= notua::storage::MAX_IMAGES) { fail(400, "slot"); return; }
  if (!haveLength || length != IMAGE_BYTES) { fail(411, "content-length"); return; }
  if (!haveCrc) { fail(400, "crc-header"); return; }
  const int candidate = unusedCandidate();
  if (candidate < 0) { fail(409, "all-slots-active"); return; }
  if (slot != candidate) { fail(409, "active-or-stale-slot"); return; }
  if (!notua::storage::acquireTransferSession(notua::storage::TransferOwner::softAp)) { fail(409, "busy"); return; }
  if (storage.startSpike(length, expectedCrc) != notua::storage::StartResult::ok) { fail(507, "storage-start"); return; }
  remaining = length; uploading = true; started = lastActivity = millis(); lastLogged = 0;
  logInfo("SOFTAP", "HTTP upload start: slot=%d bytes=%u crc=%08lx", slot, length, expectedCrc);
}
}

void requestSoftApStart() { requested = true; }

String softApConnectionInfo() {
  if (!ssid.length()) {
    const uint64_t id = ESP.getEfuseMac();
    char value[24]; snprintf(value, sizeof(value), "NOTUA-%06llX", id & 0xffffffULL); ssid = value;
  }
  String activeSlots; const int candidate = unusedCandidate(&activeSlots);
  return String("{\"ssid\":\"") + ssid + "\",\"password\":\"" + PASSWORD
      + "\",\"ip\":\"192.168.4.1\",\"port\":80,\"activeSlots\":" + activeSlots
      + ",\"candidateSlot\":" + candidate + ",\"developmentOnly\":true}";
}

bool softApTransferOwnsLifecycle() { return requested || running; }

void stopSoftApTransfer(const char* reason) {
  if (!running) return;
  if (storage.active()) storage.abort();
  notua::storage::releaseTransferSession(notua::storage::TransferOwner::softAp);
  client.stop(); server.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
  running = uploading = false;
  logInfo("SOFTAP", "stopped: reason=%s", reason);
}

void pollSoftApTransfer() {
  if (requested && !running) {
    requested = false; softApConnectionInfo(); WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(ssid.c_str(), PASSWORD)) { logError("SOFTAP", "start failed"); return; }
    server.begin(); running = true; lastActivity = millis();
    logInfo("SOFTAP", "started: ssid=%s ip=%s port=%u development_password=true", ssid.c_str(), WiFi.softAPIP().toString().c_str(), PORT);
  }
  if (!running) return;
  if (!client) {
    client = server.available();
    if (client) { lastActivity = millis(); logInfo("SOFTAP", "client connected"); acceptRequest(); }
  }
  if (uploading) {
    uint8_t buffer[4096];
    while (client.available() && remaining) {
      const size_t count = client.read(buffer, min(sizeof(buffer), remaining));
      if (!count || !storage.append(buffer, count)) { fail(500, "write"); return; }
      remaining -= count; lastActivity = millis(); feedWatchdog();
      if (storage.offset() - lastLogged >= LOG_BYTES) {
        lastLogged = storage.offset();
        const float rate = storage.offset() * 1000.0f / max(1UL, millis() - started) / 1024.0f;
        logInfo("SOFTAP", "persisted=%u average_kib_s=%.1f", storage.offset(), rate);
      }
    }
    if (!remaining) {
      uint32_t detail = 0; const auto result = storage.finish(detail); const uint32_t elapsed = millis() - started;
      const bool ok = result == notua::storage::CommitResult::committed && storage.finalizeCommit() == notua::storage::CleanupResult::ok;
      notua::storage::releaseTransferSession(notua::storage::TransferOwner::softAp);
      const float rate = IMAGE_BYTES * 1000.0f / max(static_cast<uint32_t>(1), elapsed) / 1024.0f;
      reply(ok ? 200 : 422, String("{\"ok\":") + (ok ? "true" : "false") + ",\"bytesReceived\":"
          + IMAGE_BYTES + ",\"crcMatch\":" + (ok ? "true" : "false") + ",\"elapsedMs\":" + elapsed
          + ",\"averageKiBs\":" + String(rate, 1) + ",\"crc32\":\"" + String(detail, HEX) + "\"}");
      logInfo("SOFTAP", "upload result: committed=%s crc=%s elapsed_ms=%u average_kib_s=%.1f", ok ? "true" : "false", ok ? "match" : "mismatch", elapsed, IMAGE_BYTES * 1000.0f / max(static_cast<uint32_t>(1), elapsed) / 1024.0f);
      client.stop(); uploading = false; delay(100); stopSoftApTransfer("completed"); return;
    }
    if (!client.connected()) { fail(400, "disconnect"); return; }
  }
  if (millis() - lastActivity > IDLE_MS) stopSoftApTransfer(uploading ? "upload-timeout" : "idle-timeout");
}
#endif
