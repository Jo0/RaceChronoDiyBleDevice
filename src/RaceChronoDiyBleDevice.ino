#include <RaceChrono.h>

#include "can_bus_reader.h"
#include "config.h"

// This board (LOLIN S3 Pro) uses the ESP32-S3's native TWAI controller
// through a bare SN65HVD230 transceiver, not an MCP2515 SPI CAN controller
// board -- so there's no CS/IRQ/quartz/SPI-frequency setup here. See
// can_bus_reader.h/.cpp for that, and docs/D29-CAN-Bus-Power-Wiring.md
// section 5 for why these two GPIOs specifically (picked to avoid this
// board's strapping pins and its I2C/SPI/RGB-LED/UART0 pin assignments).
const int CAN_TX_PIN = 4;  // -> SN65HVD230 CTX
const int CAN_RX_PIN = 5;  // <- SN65HVD230 CRX

bool isCanBusReaderActive = false;
long lastCanMessageReceivedMs;

struct PidExtra {
  // Only send one out of |updateRateDivider| packets per PID.
  uint8_t updateRateDivider = DEFAULT_UPDATE_RATE_DIVIDER;

  // Varies between 0 and |updateRateDivider - 1|.
  uint8_t skippedUpdates = 0;
};
RaceChronoPidMap<PidExtra> pidMap;

uint32_t loop_iteration = 0;

uint32_t last_time_num_can_bus_timeouts_sent_ms = 0;
uint16_t num_can_bus_timeouts = 0;

// Forward declarations to help put code in a natural reading order.
void waitForConnection();
void bufferNewPacket(uint32_t pid, uint8_t *data, uint8_t data_length);
void handleOneBufferedPacket();
void flushBufferedPackets();
void sendNumCanBusTimeouts();
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

    dumpMapToSerial();
  }

  void denyAllPids() {
    Serial.println("Command: DENY ALL PIDS.");

    pidMap.reset();

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

    dumpMapToSerial();
  }

  void handleDisconnect() {
    Serial.println("Resetting the map.");

    pidMap.reset();

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
  RaceChronoBle.startAdvertising();

  Serial.println("BLE is set up, waiting for an incoming connection.");
  waitForConnection();
}

void waitForConnection() {
  uint32_t iteration = 0;
  bool lastPrintHadNewline = false;
  while (!RaceChronoBle.waitForConnection(1000)) {
    Serial.print(".");
    if ((++iteration) % 10 == 0) {
      lastPrintHadNewline = true;
      Serial.println();
    } else {
      lastPrintHadNewline = false;
    }
  }

  if (!lastPrintHadNewline) {
    Serial.println();
  }

  Serial.println("Connected.");
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

void loop() {
  loop_iteration++;

  // First, verify that we have both Bluetooth and CAN up and running.

  // Not clear how heavy is the isConnected() call. Only check the connectivity
  // every 100 iterations to avoid stalling the CAN bus loop.
  if ((loop_iteration % 100) == 0 && !RaceChronoBle.isConnected()) {
    Serial.println("RaceChrono disconnected!");
    raceChronoHandler.handleDisconnect();
    stopCanBusReader();

    Serial.println("Waiting for a new connection.");
    waitForConnection();
    sendNumCanBusTimeouts();
  }

  while (!isCanBusReaderActive) {
    if (startCanBusReader()) {
      flushBufferedPackets();
      resetSkippedUpdatesCounters();
      lastCanMessageReceivedMs = millis();
      break;
    }
    delay(1000);
  }

  uint32_t timeNowMs = millis();

  // Bail out and restart the peripheral if the bus goes quiet for too long
  // (e.g. it lands in an error-passive/bus-off state). The timeout might
  // need to be tweaked for some cars.
  if (timeNowMs - lastCanMessageReceivedMs > 100) {
    Serial.println("ERROR: CAN bus timeout, aborting.");
    num_can_bus_timeouts++;
    sendNumCanBusTimeouts();
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

    bufferNewPacket(pid, data, data_length);
    lastCanMessageReceivedMs = millis();
  }

  handleOneBufferedPacket();

  if (millis() - last_time_num_can_bus_timeouts_sent_ms > 2000) {
    sendNumCanBusTimeouts();
  }
}

struct BufferedMessage {
  uint32_t pid;
  uint8_t data[8];
  uint8_t length;
};

// Circular buffer to put received messages, used to buffer messages in memory
// (which is relatively abundant) instead of relying on the very limited
// buffering ability of the MCP2515.
const uint8_t NUM_BUFFERS = 16;  // Must be a power of 2, but less than 256.
BufferedMessage buffers[NUM_BUFFERS];
uint8_t bufferToWriteTo = 0;
uint8_t bufferToReadFrom = 0;

void bufferNewPacket(uint32_t pid, uint8_t *data, uint8_t data_length) {
  if (bufferToWriteTo - bufferToReadFrom == NUM_BUFFERS) {
    Serial.println("WARNING: Receive buffer overflow, dropping one message.");

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
  if (entry != nullptr) {
    // TODO: we could do something smart here. For example, if there are more
    // messages pending with the same PID, we could count them towards
    // |skippedUpdates| in a way that we only send the latest one, but maintain
    // roughly the desired rate.
    PidExtra *extra = pidMap.getExtra(entry);
    if (extra->skippedUpdates == 0) {
      RaceChronoBle.sendCanData(pid, message->data, message->length);
    }

    extra->skippedUpdates++;
    if (extra->skippedUpdates >= extra->updateRateDivider) {
      // The next message with this PID will be sent.
      extra->skippedUpdates = 0;
    }
  }

  bufferToReadFrom++;
}

void flushBufferedPackets() {
  bufferToWriteTo = 0;
  bufferToReadFrom = 0;
}

void sendNumCanBusTimeouts() {
  // Send the count of timeouts to RaceChrono in a "fake" PID=0x777 message.
  uint8_t data[2];
  data[0] = num_can_bus_timeouts & 0xff;
  data[1] = num_can_bus_timeouts >> 8;
  RaceChronoBle.sendCanData(0x777, data, 2);

  last_time_num_can_bus_timeouts_sent_ms = millis();
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
