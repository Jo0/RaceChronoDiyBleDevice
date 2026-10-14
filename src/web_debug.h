#pragma once

#include <IPAddress.h>
#include <stdint.h>

// Bench/debug-only view of the CAN feed over Wi-Fi, for checking the module
// works without RaceChrono.
//
// The board runs a Wi-Fi SoftAP serving one web page. Connecting to the AP
// does nothing on its own: the page has to explicitly turn streaming on, and
// only then does the main sketch start the CAN reader and feed frames in here
// via webDebugRecordFrame(). Streaming turns itself back off as soon as no
// station is associated with the AP, or the page stops polling for
// |clientTimeoutMs| (tab closed, phone locked, walked out of range).
//
// The ESP32-S3 time-slices one 2.4 GHz radio between Wi-Fi and BLE, and a
// SoftAP costs BLE airtime even with nobody connected to it (it has to keep
// beaconing). So the main sketch turns the AP off entirely for as long as
// RaceChrono is connected -- see webDebugSetApEnabled().
//
// All Wi-Fi and HTTP work runs in its own FreeRTOS task pinned to core 0
// (alongside the Wi-Fi/BLE stacks), so serving the page never blocks the CAN
// drain loop in loop() on core 1.

// Registers the HTTP routes and starts the background task. The AP itself is
// not brought up until webDebugSetApEnabled(true). The string arguments must
// outlive the program (pass string literals). |password| must be 8-63
// characters for WPA2. The AP serves a /24 subnet with |ip| as both the
// board's address and the gateway; DHCP hands out the rest.
void webDebugBegin(const char *ssid, const char *password, uint8_t channel,
                   const IPAddress &ip, uint32_t clientTimeoutMs);

// Asks the background task to bring the AP up or tear it down (Wi-Fi fully
// off). Takes effect asynchronously, within a few tens of milliseconds.
// Turning the AP off also ends any streaming session.
void webDebugSetApEnabled(bool enabled);

// True while a web client has streaming turned on and is still polling.
bool webDebugIsStreaming();

// Called from loop() for every received CAN frame while streaming. Keeps the
// latest payload, a running count and the last-seen time per CAN ID; the page
// polls a snapshot of that table rather than receiving every frame, so Wi-Fi
// load stays fixed no matter how busy the bus is.
void webDebugRecordFrame(uint32_t id, const uint8_t *data, uint8_t length);

// CAN health shown on the page, which turns it into a plain-language
// diagnosis (silent bus vs. bus errors vs. dropped frames, ...). Counters are
// totals since boot, so they also cover RaceChrono sessions that ran while
// Wi-Fi was off.
struct WebDebugCanStatus {
  bool readerActive;         // TWAI driver installed and started.
  bool startFailed;          // Last attempt to start the TWAI driver failed.
  bool busOff;
  uint32_t bitRate;
  uint32_t rxErrorCounter;   // Current REC.
  uint32_t busErrors;
  uint32_t rxMissed;         // Lost to a full driver RX queue.
  uint32_t rxOverrun;        // Lost in the controller's hardware FIFO.
  uint32_t timeouts;         // Times the bus went quiet for >100 ms.
  uint32_t bufferOverflows;  // Lost in the sketch's own buffer (BLE path).
};
void webDebugSetCanStatus(const WebDebugCanStatus &status);
