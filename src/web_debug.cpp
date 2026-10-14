#include "web_debug.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "web_debug_page.h"

namespace {

// Up to this many distinct CAN IDs are tracked; frames for any further IDs
// are only counted. The FT86 gen1 bus carries a few dozen IDs, so this is
// plenty of headroom.
const uint8_t MAX_TRACKED_IDS = 128;

// How often to re-check whether the web client is still there.
const uint32_t CLIENT_CHECK_INTERVAL_MS = 250;

// Back-off before retrying if WiFi.softAP() fails.
const uint32_t AP_START_RETRY_MS = 5000;

const char *apSsid;
const char *apPassword;
uint8_t apChannel;
IPAddress apIp;
uint32_t clientTimeoutMs;

// Shared between loop() on core 1 and the web task on core 0.
volatile bool apWanted = false;
volatile bool streamRequested = false;

struct FrameStats {
  uint32_t id;
  uint8_t length;
  uint8_t data[8];
  uint32_t count;
  uint32_t lastSeenMs;
};

// Guards everything below it. Held only for short copies, never across I/O.
portMUX_TYPE statsLock = portMUX_INITIALIZER_UNLOCKED;
FrameStats stats[MAX_TRACKED_IDS];
uint8_t numTrackedIds = 0;
uint32_t totalFrames = 0;
uint32_t untrackedFrames = 0;
WebDebugCanStatus canStatus = {};

// Only touched from the web task.
WebServer server(80);
bool apUp = false;
uint32_t lastClientRequestMs = 0;

void resetStats() {
  portENTER_CRITICAL(&statsLock);
  numTrackedIds = 0;
  totalFrames = 0;
  untrackedFrames = 0;
  portEXIT_CRITICAL(&statsLock);
}

void handlePage() {
  server.send(200, "text/html", WEB_DEBUG_PAGE);
}

// Returns the status plus a snapshot of the per-ID table as compact JSON:
// frames are [id, length, "hex payload", count, ms since last seen].
void handleState() {
  lastClientRequestMs = millis();

  static FrameStats snapshot[MAX_TRACKED_IDS];
  portENTER_CRITICAL(&statsLock);
  uint8_t numIds = numTrackedIds;
  memcpy(snapshot, stats, numIds * sizeof(FrameStats));
  uint32_t total = totalFrames;
  uint32_t untracked = untrackedFrames;
  WebDebugCanStatus can = canStatus;
  portEXIT_CRITICAL(&statsLock);

  uint32_t nowMs = millis();

  String json;
  json.reserve(400 + numIds * 56);
  json += "{\"streaming\":";
  json += streamRequested ? "true" : "false";
  json += ",\"can\":";
  json += can.readerActive ? "true" : "false";
  json += ",\"diag\":{\"startFailed\":";
  json += can.startFailed ? "true" : "false";
  json += ",\"busOff\":";
  json += can.busOff ? "true" : "false";
  json += ",\"bitRate\":";
  json += can.bitRate;
  json += ",\"rec\":";
  json += can.rxErrorCounter;
  json += ",\"busErrors\":";
  json += can.busErrors;
  json += ",\"rxMissed\":";
  json += can.rxMissed;
  json += ",\"rxOverrun\":";
  json += can.rxOverrun;
  json += ",\"timeouts\":";
  json += can.timeouts;
  json += ",\"bufferOverflows\":";
  json += can.bufferOverflows;
  json += "},\"total\":";
  json += total;
  json += ",\"untracked\":";
  json += untracked;
  json += ",\"uptimeMs\":";
  json += nowMs;
  json += ",\"clientTimeoutMs\":";
  json += clientTimeoutMs;
  json += ",\"frames\":[";

  char hex[17];
  char entry[72];
  for (uint8_t i = 0; i < numIds; i++) {
    const FrameStats &s = snapshot[i];
    for (uint8_t b = 0; b < s.length; b++) {
      snprintf(hex + b * 2, 3, "%02X", s.data[b]);
    }
    hex[s.length * 2] = '\0';

    snprintf(entry, sizeof(entry), "%s[%lu,%u,\"%s\",%lu,%lu]",
             i == 0 ? "" : ",", (unsigned long)s.id, s.length, hex,
             (unsigned long)s.count, (unsigned long)(nowMs - s.lastSeenMs));
    json += entry;
  }
  json += "]}";

  server.send(200, "application/json", json);
}

void handleStream() {
  lastClientRequestMs = millis();

  bool on = server.arg("on") == "1";
  if (on && !streamRequested) {
    resetStats();
    Serial.println("Web debug: streaming turned on by the web page.");
  } else if (!on && streamRequested) {
    Serial.println("Web debug: streaming turned off by the web page.");
  }
  streamRequested = on;

  handleState();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

bool startAp() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(apSsid, apPassword, apChannel, /*ssid_hidden=*/0,
                   /*max_connection=*/2)) {
    Serial.println("Web debug: ERROR: unable to start the Wi-Fi AP.");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  // Same order as bonogps: give the AP a moment to finish starting, then
  // move it off the 192.168.4.1 default.
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!WiFi.softAPConfig(apIp, /*gateway=*/apIp, IPAddress(255, 255, 255, 0))) {
    Serial.println("Web debug: WARNING: unable to set the AP's IP address, "
                   "keeping the default.");
  }

  server.begin();
  apUp = true;

  Serial.printf("Web debug: AP \"%s\" up on channel %u, browse to http://%s/\n",
                apSsid, apChannel, WiFi.softAPIP().toString().c_str());
  return true;
}

void stopAp() {
  if (streamRequested) {
    Serial.println("Web debug: streaming stopped, AP is shutting down.");
  }
  streamRequested = false;

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apUp = false;

  Serial.println("Web debug: AP down, Wi-Fi off.");
}

// Ends the streaming session once nobody is around to watch it, which in
// turn lets loop() stop the CAN reader and go back to waiting for RaceChrono.
void checkClientStillThere() {
  if (!streamRequested) {
    return;
  }

  const char *reason = nullptr;
  if (WiFi.softAPgetStationNum() == 0) {
    reason = "no devices connected to the AP";
  } else if (millis() - lastClientRequestMs > clientTimeoutMs) {
    reason = "web page stopped polling";
  }

  if (reason != nullptr) {
    streamRequested = false;
    Serial.printf("Web debug: streaming stopped (%s).\n", reason);
  }
}

void webDebugTask(void *) {
  uint32_t lastApStartAttemptMs = millis() - AP_START_RETRY_MS;
  uint32_t lastClientCheckMs = 0;

  for (;;) {
    bool wanted = apWanted;
    if (wanted && !apUp &&
        millis() - lastApStartAttemptMs >= AP_START_RETRY_MS) {
      lastApStartAttemptMs = millis();
      startAp();
    } else if (!wanted && apUp) {
      stopAp();
    }

    if (apUp) {
      server.handleClient();

      if (millis() - lastClientCheckMs >= CLIENT_CHECK_INTERVAL_MS) {
        checkClientStillThere();
        lastClientCheckMs = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(apUp ? 2 : 50));
  }
}

}  // namespace

void webDebugBegin(const char *ssid, const char *password, uint8_t channel,
                   const IPAddress &ip, uint32_t timeoutMs) {
  apSsid = ssid;
  apPassword = password;
  apChannel = channel;
  apIp = ip;
  clientTimeoutMs = timeoutMs;

  server.on("/", HTTP_GET, handlePage);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/stream", HTTP_POST, handleStream);
  server.onNotFound(handleNotFound);

  // Core 0 is where the Wi-Fi and NimBLE host tasks already live; loop() runs
  // on core 1. Priority 1 keeps this below both radio stacks.
  xTaskCreatePinnedToCore(webDebugTask, "webDebug", 8192, nullptr,
                          /*priority=*/1, nullptr, /*core=*/0);
}

void webDebugSetApEnabled(bool enabled) {
  apWanted = enabled;
}

bool webDebugIsStreaming() {
  return streamRequested;
}

void webDebugRecordFrame(uint32_t id, const uint8_t *data, uint8_t length) {
  if (length > 8) {
    length = 8;
  }
  uint32_t nowMs = millis();

  portENTER_CRITICAL(&statsLock);
  totalFrames++;

  FrameStats *entry = nullptr;
  for (uint8_t i = 0; i < numTrackedIds; i++) {
    if (stats[i].id == id) {
      entry = &stats[i];
      break;
    }
  }
  if (entry == nullptr && numTrackedIds < MAX_TRACKED_IDS) {
    entry = &stats[numTrackedIds++];
    entry->id = id;
    entry->count = 0;
  }

  if (entry != nullptr) {
    entry->length = length;
    memcpy(entry->data, data, length);
    entry->count++;
    entry->lastSeenMs = nowMs;
  } else {
    untrackedFrames++;
  }
  portEXIT_CRITICAL(&statsLock);
}

void webDebugSetCanStatus(const WebDebugCanStatus &status) {
  portENTER_CRITICAL(&statsLock);
  canStatus = status;
  portEXIT_CRITICAL(&statsLock);
}
