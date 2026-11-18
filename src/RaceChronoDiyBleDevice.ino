#include <NimBLEDevice.h>
#include <RaceChrono.h>

#include "can_bus_reader.h"
#include "config.h"
#include "packed_frames.h"
#include "web_debug.h"

// This board (LOLIN S3 Pro) uses the ESP32-S3's native TWAI controller
// through a bare SN65HVD230 transceiver, not an MCP2515 SPI CAN controller
// board -- so there's no CS/IRQ/quartz/SPI-frequency setup here. See
// can_bus_reader.h/.cpp for that, and docs/D29-CAN-Bus-Power-Wiring.md
// section 5 for why these two GPIOs specifically (picked to avoid this
// board's strapping pins and its I2C/SPI/RGB-LED/UART0 pin assignments).
// Never use GPIO43/44 (TX0/RX0) here: the ROM bootloader prints its boot log
// on GPIO43 before this firmware runs, which would go straight onto the car's
// CAN bus through CTX.
const int CAN_TX_PIN = 4;  // -> SN65HVD230 CTX
const int CAN_RX_PIN = 5;  // <- SN65HVD230 CRX

// Where the CAN feed is going, if anywhere. RaceChrono always wins: if it
// connects while a web debug session is streaming, the web session is dropped
// and Wi-Fi is switched off for the whole RaceChrono session. The ESP32-S3
// time-slices a single 2.4 GHz radio between Wi-Fi and BLE, and a SoftAP
// costs BLE airtime even with nobody connected to it.
enum class Mode {
  IDLE,        // BLE advertising + debug AP up, CAN reader off.
  RACECHRONO,  // CAN -> BLE. Wi-Fi off.
  WEB_DEBUG,   // CAN -> debug web page. BLE still advertising.
};
Mode mode = Mode::IDLE;

bool isCanBusReaderActive = false;
long lastCanMessageReceivedMs;

// Only throttles retries after the TWAI driver refused to start; a restart
// after a bus timeout is still immediate.
bool lastCanStartFailed = false;
uint32_t lastCanStartFailedMs = 0;

struct PidExtra {
  // Only send one out of |updateRateDivider| packets per PID.
  uint8_t updateRateDivider = DEFAULT_UPDATE_RATE_DIVIDER;

  // Varies between 0 and |updateRateDivider - 1|.
  uint8_t skippedUpdates = 0;
};
RaceChronoPidMap<PidExtra> pidMap;

// Builds the virtual 0x7A0/0x7A1 frames RaceChrono can ask for instead of the
// raw source IDs. See packed_frames.h for the layout.
PackedFrameBuilder packedFrames;

uint32_t loop_iteration = 0;

uint32_t last_time_num_can_bus_timeouts_sent_ms = 0;
uint16_t num_can_bus_timeouts = 0;

// Reported on the debug web page, and to RaceChrono in the PID 0x778
// diagnostics message (see sendBleDiagnostics()).
uint32_t num_buffer_overflows = 0;
uint32_t last_can_status_published_ms = 0;

// BLE throughput diagnostics. NimBLE reports the outcome of every queued
// notification from its host task via onStatus(), so these are volatile.
// A failed notification (usually: the stack ran out of buffers because the
// connection can't carry frames as fast as we queue them) is a CAN frame
// RaceChrono never saw.
volatile uint32_t num_notify_ok = 0;
volatile uint32_t num_notify_failed = 0;
uint32_t last_ble_diagnostics_sent_ms = 0;
uint32_t num_notify_ok_at_last_diagnostics = 0;

class NotifyStatusCounter : public NimBLECharacteristicCallbacks {
  void onStatus(NimBLECharacteristic *characteristic, Status s, int code) {
    if (s == SUCCESS_NOTIFY) {
      num_notify_ok++;
    } else {
      num_notify_failed++;
    }
  }
} notifyStatusCounter;

// Values from RaceChrono.h (RaceChrono DIY BLE API).
const uint16_t RC_SERVICE_UUID = 0x1ff8;
const uint16_t RC_CAN_BUS_CHARACTERISTIC_UUID = 0x0001;

// The library's CAN characteristic, looked up in setup(). sendCan() writes to
// it directly because the library's sendCanData() truncates payloads to 8
// bytes, while the RaceChrono API allows 16 (the packed frames need 15).
NimBLECharacteristic *canCharacteristic = nullptr;

// BLE link setup after RaceChrono connects (see serviceLinkSetup()). The PHY
// and connection-interval changes are separate link-layer procedures, and a
// link runs only one at a time, so they're spaced out instead of requested
// back to back (which is likely why 2M never took effect before).
uint32_t link_connected_ms = 0;
uint8_t link_setup_stage = 0;

// Last link state read back from the stack, for diagnostics and retries.
uint16_t link_interval_units = 0;  // 1.25 ms units
uint8_t link_tx_phy = 0;
uint8_t link_rx_phy = 0;
int8_t link_rssi = 0;
bool link_rssi_valid = false;

// Forward declarations to help put code in a natural reading order.
void enterMode(Mode newMode);
void bufferNewPacket(uint32_t pid, const uint8_t *data, uint8_t data_length);
void handleOneBufferedPacket();
void handleAllBufferedPackets();
void flushBufferedPackets();
void sendNumCanBusTimeouts();
void sendBleDiagnostics();
void serviceLinkSetup();
void readLinkState();
void sendCan(uint32_t pid, const uint8_t *data, uint8_t length);
void resetSkippedUpdatesCounters();

void dumpMapToSerial() {
  Serial.println("Current state of the PID map:");

  uint16_t updateIntervalForAllEntries;
  bool areAllPidsAllowed =
      pidMap.areAllPidsAllowed(&updateIntervalForAllEntries);
  if (areAllPidsAllowed) {
    Serial.println("  All PIDs are allowed.");
  }

  if (pidMap.isEmpty()) {
    if (areAllPidsAllowed) {
      Serial.println("  Map is empty.");
      Serial.println("");
    } else {
      Serial.println("  No PIDs are allowed.");
      Serial.println("");
    }
    return;
  }

  struct {
    void operator() (void *entry) {
      uint32_t pid = pidMap.getPid(entry);
      const PidExtra *extra = pidMap.getExtra(entry);

      Serial.print("  ");
      Serial.print(pid);
      Serial.print(" (0x");
      Serial.print(pid, HEX);
      Serial.print("), sending 1 out of ");
      Serial.print(extra->updateRateDivider);
      Serial.println(" messages.");
    }
  } dumpEntry;
  pidMap.forEach(dumpEntry);

  Serial.println("");
}

class UpdateMapOnRaceChronoCommands : public RaceChronoBleCanHandler {
public:
  void allowAllPids(uint16_t updateIntervalMs) {
    Serial.print("Command: ALLOW ALL PIDS, update interval: ");
    Serial.print(updateIntervalMs);
    Serial.println(" ms.");

    pidMap.allowAllPids(updateIntervalMs);
    // "Allow all" means RaceChrono wants everything, packed frames included.
    packedFrames.setFastRequested(true);
    packedFrames.setChassisRequested(true);

    dumpMapToSerial();
  }

  void denyAllPids() {
    Serial.println("Command: DENY ALL PIDS.");

    pidMap.reset();
    packedFrames.reset();

    dumpMapToSerial();
  }

  void allowPid(uint32_t pid, uint16_t updateIntervalMs) {
    Serial.print("Command: ALLOW PID ");
    Serial.print(pid);
    Serial.print(" (0x");
    Serial.print(pid, HEX);
    Serial.print("), requested update interval: ");
    Serial.print(updateIntervalMs);
    Serial.println(" ms.");

    if (!pidMap.allowOnePid(pid, updateIntervalMs)) {
      Serial.println("WARNING: unable to handle this request!");
    }

    void *entry = pidMap.getEntryId(pid);
    if (entry != nullptr) {
      PidExtra *pidExtra = pidMap.getExtra(entry);
      pidExtra->skippedUpdates = 0;
      pidExtra->updateRateDivider = getUpdateRateDivider(pid);
    }

    // Virtual packed IDs aren't on the bus: asking for one turns on building
    // it from its source frames (see packed_frames.h).
    if (pid == PACKED_FAST_PID) {
      packedFrames.setFastRequested(true);
    } else if (pid == PACKED_CHASSIS_PID) {
      packedFrames.setChassisRequested(true);
    }

    dumpMapToSerial();
  }

  void handleDisconnect() {
    Serial.println("Resetting the map.");

    pidMap.reset();
    packedFrames.reset();

    dumpMapToSerial();
  }
} raceChronoHandler;


void setup() {
  uint32_t startTimeMs = millis();
  Serial.begin(115200);
  while (!Serial && millis() - startTimeMs < 5000) {
  }

  Serial.println("Setting up BLE...");
  RaceChronoBle.setUp(DEVICE_NAME, &raceChronoHandler);

  // RaceChrono itself never pairs, but Android will if the device is ever
  // tapped in Bluetooth settings, and Android 16+ re-pairs on its own when
  // a stored bond's key is missing. NimBLE-Arduino defaults to pairing
  // *without* bonding, so the phone kept an LTK this board never stored:
  // every reconnect then failed encryption ("key missing"), kicked off an
  // automatic re-pair, and the link dropped 5-30 s later. Bonding (stored in
  // NVS, survives power cycles) makes any pairing stick on both ends.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);

  // The RaceChrono library sets no callbacks on its CAN characteristic, so we
  // can attach our own to count notification successes/failures.
  NimBLEService *rcService =
      NimBLEDevice::getServer()->getServiceByUUID(NimBLEUUID(RC_SERVICE_UUID));
  canCharacteristic =
      rcService == nullptr
          ? nullptr
          : rcService->getCharacteristic(
                NimBLEUUID(RC_CAN_BUS_CHARACTERISTIC_UUID));
  if (canCharacteristic != nullptr) {
    canCharacteristic->setCallbacks(&notifyStatusCounter);
  } else {
    Serial.println("WARNING: CAN characteristic not found, no BLE diagnostics.");
  }

  // Default is +3 dBm. A stronger signal means fewer link-layer resends,
  // which otherwise eat the slots new frames would use -- this matters when
  // the board is mounted somewhere enclosed, like a glove box. +18 dBm is within the ESP32-S3's rated
  // BLE output (max +20). It only strengthens the board -> phone direction,
  // which is the one carrying all the data.
  NimBLEDevice::setPower(ESP_PWR_LVL_P18, ESP_BLE_PWR_TYPE_DEFAULT);
  NimBLEDevice::setPower(ESP_PWR_LVL_P18, ESP_BLE_PWR_TYPE_ADV);
  Serial.printf("BLE TX power: %d dBm\n", NimBLEDevice::getPower());

  RaceChronoBle.startAdvertising();

  Serial.println("Setting up the Wi-Fi debug page...");
  webDebugBegin(WEB_DEBUG_AP_SSID, WEB_DEBUG_AP_PASSWORD, WEB_DEBUG_AP_CHANNEL,
                WEB_DEBUG_AP_IP, WEB_DEBUG_CLIENT_TIMEOUT_MS);

  enterMode(Mode::IDLE);
}

const char *modeName(Mode m) {
  switch (m) {
    case Mode::IDLE: return "IDLE (waiting for RaceChrono over BLE, or the debug web page)";
    case Mode::RACECHRONO: return "RACECHRONO (CAN -> BLE, Wi-Fi off)";
    case Mode::WEB_DEBUG: return "WEB_DEBUG (CAN -> debug web page)";
  }
  return "?";
}

Mode desiredMode() {
  if (RaceChronoBle.isConnected()) {
    return Mode::RACECHRONO;
  }
  if (webDebugIsStreaming()) {
    return Mode::WEB_DEBUG;
  }
  return Mode::IDLE;
}

void enterMode(Mode newMode) {
  if (mode == Mode::RACECHRONO && newMode != Mode::RACECHRONO) {
    Serial.println("RaceChrono disconnected!");
    raceChronoHandler.handleDisconnect();
  }

  // Every mode change restarts the CAN reader from scratch (or leaves it off
  // for IDLE), so no buffered frames leak from one consumer to the other.
  if (isCanBusReaderActive) {
    stopCanBusReader();
  }
  lastCanStartFailed = false;

  mode = newMode;
  webDebugSetApEnabled(newMode != Mode::RACECHRONO);

  Serial.print("Mode: ");
  Serial.println(modeName(newMode));

  if (newMode == Mode::RACECHRONO) {
    Serial.println("Connected.");
    link_connected_ms = millis();
    link_setup_stage = 0;
    link_tx_phy = link_rx_phy = 0;
    link_interval_units = 0;
    link_rssi_valid = false;
    num_notify_ok = 0;
    num_notify_failed = 0;
    num_notify_ok_at_last_diagnostics = 0;
    num_buffer_overflows = 0;
    last_ble_diagnostics_sent_ms = millis();
    sendNumCanBusTimeouts();
  }
}

bool startCanBusReader() {
  Serial.println("Connecting to the CAN bus...");

#if defined(CAN_BUS_SELF_TEST)
  Serial.println(
      "*** CAN_BUS_SELF_TEST build -- transmits fake frames onto the bus. "
      "DO NOT install this build in the car. ***");
  bool started = canBusReaderBegin(
      CAN_RX_PIN, CAN_TX_PIN, BAUD_RATE, /*selfTest=*/true);
#else
  bool started = canBusReaderBegin(CAN_RX_PIN, CAN_TX_PIN, BAUD_RATE);
#endif

  if (!started) {
    Serial.println("ERROR: Unable to start the CAN bus listener.");
    return false;
  }

  Serial.println("Success!");

  isCanBusReaderActive = true;
  return true;
}

void stopCanBusReader() {
  canBusReaderEnd();
  isCanBusReaderActive = false;
}

void publishCanStatusToWebDebug() {
  CanBusReaderStats stats;
  canBusReaderGetStats(&stats);

  WebDebugCanStatus status;
  status.readerActive = isCanBusReaderActive && stats.running;
  status.startFailed = lastCanStartFailed;
  status.busOff = stats.busOff;
  status.bitRate = BAUD_RATE;
  status.rxErrorCounter = stats.rxErrorCounter;
  status.busErrors = stats.busErrors;
  status.rxMissed = stats.rxMissed;
  status.rxOverrun = stats.rxOverrun;
  status.timeouts = num_can_bus_timeouts;
  status.bufferOverflows = num_buffer_overflows;
  webDebugSetCanStatus(status);
}

void loop() {
  loop_iteration++;

  // First, work out who (if anyone) the CAN feed is for: RaceChrono over BLE,
  // the debug web page over Wi-Fi, or nobody.

  // Not clear how heavy is the isConnected() call. Only check the connectivity
  // every 100 iterations to avoid stalling the CAN bus loop. IDLE has no CAN
  // loop to stall, so it checks on every pass.
  if (mode == Mode::IDLE || (loop_iteration % 100) == 0) {
    Mode wanted = desiredMode();
    if (wanted != mode) {
      enterMode(wanted);
    }
  }

  if (millis() - last_can_status_published_ms >= 100) {
    publishCanStatusToWebDebug();
    last_can_status_published_ms = millis();
  }

  if (mode == Mode::IDLE) {
    delay(10);
    return;
  }

  if (!isCanBusReaderActive) {
    // Never block here (the old code sat in a delay(1000) loop): mode changes
    // like the web page going away must still be noticed.
    if (lastCanStartFailed && millis() - lastCanStartFailedMs < 1000) {
      return;
    }
    if (!startCanBusReader()) {
      lastCanStartFailed = true;
      lastCanStartFailedMs = millis();
      return;
    }
    lastCanStartFailed = false;
    flushBufferedPackets();
    resetSkippedUpdatesCounters();
    lastCanMessageReceivedMs = millis();
  }

  uint32_t timeNowMs = millis();

  // Bail out and restart the peripheral if the bus goes quiet for too long
  // (e.g. it lands in an error-passive/bus-off state). The timeout might
  // need to be tweaked for some cars.
  if (timeNowMs - lastCanMessageReceivedMs > 100) {
    Serial.println("ERROR: CAN bus timeout, aborting.");
    num_can_bus_timeouts++;
    if (mode == Mode::RACECHRONO) {
      sendNumCanBusTimeouts();
    }
    stopCanBusReader();
    return;
  }

#if defined(CAN_BUS_SELF_TEST)
  // Bench smoke test: manufacture a plausible "Engine RPM" frame on CAN ID
  // 0x140 every 100ms, sweeping 800-7000 RPM, using the same bit layout as
  // the "Engine RPM" equation in docs/D29-CAN-Bus-Power-Wiring.md section 7
  // (bitsToUIntLe(raw, 16, 14) -- i.e. a 14-bit little-endian field starting
  // at byte 2). Configure that channel in RaceChrono and you should see a
  // sawtooth RPM sweep with zero bench hardware beyond this one board.
  static uint32_t last_test_frame_sent_ms = 0;
  if (timeNowMs - last_test_frame_sent_ms >= 100) {
    static uint16_t fake_rpm = 800;
    fake_rpm = (fake_rpm >= 7000) ? 800 : fake_rpm + 40;

    uint8_t data[8] = {0};
    data[2] = fake_rpm & 0xff;
    data[3] = (fake_rpm >> 8) & 0x3f;
    canBusReaderSendTestFrame(0x140, data, sizeof(data));

    last_test_frame_sent_ms = timeNowMs;
  }
#endif

  // Sending data over Bluetooth takes time, so we drain the TWAI driver's
  // own RX queue aggressively into our own (larger) software ring buffer
  // rather than letting frames pile up and get dropped upstream.
  uint32_t pid;
  uint8_t data[8];
  uint8_t data_length;
  while (canBusReaderReceive(&pid, data, &data_length)) {
    if (data_length == 0) {
      // Nothing to send here. Can this even happen?
      continue;
    }

    if (mode == Mode::RACECHRONO) {
      // Packed frames first: a source frame may complete one or two of them.
      PackedFrame packed[2];
      int numPacked = packedFrames.onCanFrame(pid, data, data_length, packed);
      for (int i = 0; i < numPacked; i++) {
        bufferNewPacket(packed[i].pid, packed[i].data, packed[i].length);
      }
      // Then the raw frame, if RaceChrono also asked for this ID itself.
      if (pidMap.getEntryId(pid) != nullptr) {
        bufferNewPacket(pid, data, data_length);
      }
    } else {
      webDebugRecordFrame(pid, data, data_length);
    }
    lastCanMessageReceivedMs = millis();
  }

  if (mode != Mode::RACECHRONO) {
    return;
  }

  // Drain everything buffered: at native rates, sending one frame per loop
  // pass can fall behind the bus. Queuing a notification doesn't block; if
  // BLE can't keep up, NimBLE fails it and num_notify_failed counts it.
  handleAllBufferedPackets();

  serviceLinkSetup();

  if (millis() - last_time_num_can_bus_timeouts_sent_ms > 2000) {
    sendNumCanBusTimeouts();
  }

  if (millis() - last_ble_diagnostics_sent_ms >= 1000) {
    sendBleDiagnostics();
  }
}

struct BufferedMessage {
  uint32_t pid;
  uint8_t data[PACKED_MAX_LENGTH];  // 8 for raw CAN frames, up to 16 packed.
  uint8_t length;
};

// Circular buffer to put received messages, used to buffer messages in memory
// (which is relatively abundant) between draining the TWAI driver queue and
// sending over BLE.
const uint8_t NUM_BUFFERS = 64;  // Must be a power of 2, but less than 256.
BufferedMessage buffers[NUM_BUFFERS];
uint8_t bufferToWriteTo = 0;
uint8_t bufferToReadFrom = 0;

// Callers only pass frames RaceChrono asked for (a requested raw ID, or a
// requested packed frame). Filtering before buffering matters: previously
// every frame on the bus went into the buffer and was filtered on the way
// out, so the many IDs nobody wants could push wanted frames out of it.
void bufferNewPacket(uint32_t pid, const uint8_t *data, uint8_t data_length) {
  if (data_length > PACKED_MAX_LENGTH) {
    data_length = PACKED_MAX_LENGTH;
  }

  if (bufferToWriteTo - bufferToReadFrom == NUM_BUFFERS) {
    Serial.println("WARNING: Receive buffer overflow, dropping one message.");
    num_buffer_overflows++;

    // In case of a buffer overflow, drop the oldest message in the buffer, as
    // it's likely less useful than the newest one.
    bufferToReadFrom++;
  }

  BufferedMessage *message = &buffers[bufferToWriteTo % NUM_BUFFERS];
  message->pid = pid;
  memcpy(message->data, data, data_length);
  message->length = data_length;
  bufferToWriteTo++;
}

void handleOneBufferedPacket() {
  if (bufferToReadFrom == bufferToWriteTo) {
    // No buffered messages.
    return;
  }

  BufferedMessage *message = &buffers[bufferToReadFrom % NUM_BUFFERS];
  uint32_t pid = message->pid;
  void *entry = pidMap.getEntryId(pid);
  if (PackedFrameBuilder::isPackedPid(pid)) {
    // Built at the source frames' native rate; no divider applies.
    sendCan(pid, message->data, message->length);
  } else if (entry != nullptr) {
    // TODO: we could do something smart here. For example, if there are more
    // messages pending with the same PID, we could count them towards
    // |skippedUpdates| in a way that we only send the latest one, but maintain
    // roughly the desired rate.
    PidExtra *extra = pidMap.getExtra(entry);
    if (extra->skippedUpdates == 0) {
      sendCan(pid, message->data, message->length);
    }

    extra->skippedUpdates++;
    if (extra->skippedUpdates >= extra->updateRateDivider) {
      // The next message with this PID will be sent.
      extra->skippedUpdates = 0;
    }
  }

  bufferToReadFrom++;
}

void handleAllBufferedPackets() {
  while (bufferToReadFrom != bufferToWriteTo) {
    handleOneBufferedPacket();
  }
}

void flushBufferedPackets() {
  bufferToWriteTo = 0;
  bufferToReadFrom = 0;
}

// Same packet format as the library's sendCanData() (4-byte little-endian
// PID, then the payload), but allows the full 16-byte payload.
void sendCan(uint32_t pid, const uint8_t *data, uint8_t length) {
  if (canCharacteristic == nullptr) {
    RaceChronoBle.sendCanData(pid, data, length);  // Truncates to 8 bytes.
    return;
  }
  if (length > PACKED_MAX_LENGTH) {
    length = PACKED_MAX_LENGTH;
  }
  uint8_t buffer[4 + PACKED_MAX_LENGTH];
  buffer[0] = pid & 0xFF;
  buffer[1] = (pid >> 8) & 0xFF;
  buffer[2] = (pid >> 16) & 0xFF;
  buffer[3] = (pid >> 24) & 0xFF;
  memcpy(buffer + 4, data, length);
  canCharacteristic->setValue(buffer, 4 + length);
  canCharacteristic->notify();
}

void sendNumCanBusTimeouts() {
  // Send the count of timeouts to RaceChrono in a "fake" PID=0x777 message.
  uint8_t data[2];
  data[0] = num_can_bus_timeouts & 0xff;
  data[1] = num_can_bus_timeouts >> 8;
  sendCan(0x777, data, 2);

  last_time_num_can_bus_timeouts_sent_ms = millis();
}

// Once a second: BLE throughput diagnostics, to Serial and to RaceChrono as a
// "fake" PID=0x778 message (1912 decimal). Add channels for it in RaceChrono
// to log whether BLE keeps up during a session:
//   Notify failures (total):   bytesToUIntLe(raw, 0, 2)
//   Buffer overflows (total):  bytesToUIntLe(raw, 2, 2)
//   Frames sent per second:    bytesToUIntLe(raw, 4, 2)
//   Connection interval (ms):  bytesToUIntLe(raw, 6, 1) * 1.25
//   TX PHY (1=1M, 2=2M, 3=Coded, 0=unknown): bytesToUIntLe(raw, 7, 1)
//   Signal strength (dBm):     bytesToIntLe(raw, 8, 1)   (0 = unknown)
// The counters reset on every new RaceChrono connection; the rest is what
// the link is actually running at, read back from the BLE stack each time.
void sendBleDiagnostics() {
  uint32_t now = millis();
  uint32_t elapsed = now - last_ble_diagnostics_sent_ms;
  uint32_t ok = num_notify_ok;
  uint32_t failed = num_notify_failed;
  uint32_t sentPerSecond =
      elapsed == 0 ? 0 : (ok - num_notify_ok_at_last_diagnostics) * 1000 / elapsed;
  num_notify_ok_at_last_diagnostics = ok;
  last_ble_diagnostics_sent_ms = now;

  uint16_t failed16 = failed > 0xffff ? 0xffff : failed;
  uint16_t overflows16 =
      num_buffer_overflows > 0xffff ? 0xffff : num_buffer_overflows;
  uint16_t rate16 = sentPerSecond > 0xffff ? 0xffff : sentPerSecond;

  readLinkState();

  uint8_t data[9];
  data[0] = failed16 & 0xff;
  data[1] = failed16 >> 8;
  data[2] = overflows16 & 0xff;
  data[3] = overflows16 >> 8;
  data[4] = rate16 & 0xff;
  data[5] = rate16 >> 8;
  data[6] = link_interval_units > 0xff ? 0xff : link_interval_units;
  data[7] = link_tx_phy;
  data[8] = link_rssi_valid ? (uint8_t)link_rssi : 0;
  sendCan(0x778, data, sizeof(data));

  Serial.printf("BLE: %lu frames/s sent, %lu notify failures, %lu buffer overflows (since connect), "
                "interval %.2f ms, PHY tx %u rx %u, RSSI %d dBm\n",
                (unsigned long)sentPerSecond, (unsigned long)failed,
                (unsigned long)num_buffer_overflows, link_interval_units * 1.25f,
                link_tx_phy, link_rx_phy, link_rssi_valid ? link_rssi : 0);
}

// Reads what the link is actually running at. The phone has the final say on
// both the PHY and the connection interval.
void readLinkState() {
  std::vector<uint16_t> peers = NimBLEDevice::getServer()->getPeerDevices();
  if (peers.empty()) {
    return;
  }
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(peers[0], &desc) == 0) {
    link_interval_units = desc.conn_itvl;
  }
  if (ble_gap_read_le_phy(peers[0], &link_tx_phy, &link_rx_phy) != 0) {
    link_tx_phy = link_rx_phy = 0;
  }
  link_rssi_valid = ble_gap_conn_rssi(peers[0], &link_rssi) == 0;
}

// Asks the phone for a faster link, one link-layer procedure at a time:
//   +0.5 s  2M PHY: each packet takes about half the airtime of 1M, so more
//           fit into each connection event -- and into the radio time the
//           phone leaves Bluetooth when it's also using Wi-Fi.
//   +2.0 s  7.5-15 ms connection interval: more connection events per second
//           means more notifications per second, and tighter timestamps on
//           the phone side (RaceChrono stamps CAN data on arrival).
//   +5 s, +8 s  ask for 2M again if the link is still on 1M.
// sendBleDiagnostics() reports what the phone actually granted.
void serviceLinkSetup() {
  static const uint32_t kStageAtMs[] = {500, 2000, 5000, 8000};
  if (link_setup_stage >= sizeof(kStageAtMs) / sizeof(kStageAtMs[0]) ||
      millis() - link_connected_ms < kStageAtMs[link_setup_stage]) {
    return;
  }
  uint8_t stage = link_setup_stage++;

  NimBLEServer *server = NimBLEDevice::getServer();
  for (uint16_t connHandle : server->getPeerDevices()) {
    if (stage == 1) {
      // Units: interval 1.25 ms (6 = 7.5 ms, 12 = 15 ms), timeout 10 ms (4 s).
      server->updateConnParams(connHandle, 6, 12, 0, 400);
      Serial.println("Link: requested 7.5-15 ms connection interval.");
      continue;
    }

    if (stage >= 2) {
      readLinkState();
      if (link_tx_phy == BLE_HCI_LE_PHY_2M) {
        continue;
      }
    }
    int rc = ble_gap_set_prefered_le_phy(connHandle, BLE_GAP_LE_PHY_2M_MASK,
                                         BLE_GAP_LE_PHY_2M_MASK,
                                         BLE_GAP_LE_PHY_CODED_ANY);
    Serial.printf("Link: requested 2M PHY (attempt %u), rc=%d\n",
                  stage == 0 ? 1 : stage, rc);
  }
}

void resetSkippedUpdatesCounters() {
  struct {
    void operator() (void *entry) {
      PidExtra *extra = pidMap.getExtra(entry);
      extra->skippedUpdates = 0;
    }
  } resetSkippedUpdatesCounter;
  pidMap.forEach(resetSkippedUpdatesCounter);
}
