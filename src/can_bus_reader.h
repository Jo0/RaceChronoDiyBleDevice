#pragma once

#include <stdint.h>

// Thin wrapper around the ESP32-S3's native TWAI (CAN 2.0) controller.
//
// This replaces arduino-CAN's MCP2515/ESP32SJA1000 backends: MCP2515 needs
// an external SPI CAN controller chip we don't have (SN65HVD230 is a bare
// transceiver, not a controller), and ESP32SJA1000 pokes register addresses
// that only exist on the original/classic ESP32, not the S3.
//
// The peripheral is normally started in listen-only mode: it can receive and
// observe bus traffic but is physically incapable of driving CANH/CANL, even
// if firmware misbehaves. See docs/D29-CAN-Bus-Power-Wiring.md section 6 for
// why that's the load-bearing safety mechanism for this whole project.

// Starts the TWAI peripheral on the given GPIOs at the given bit rate (e.g.
// 500000 for 500 kbit/s, matching the FT86 gen1 CAN bus). Returns false if
// the bit rate isn't one of the cases handled below, or the driver failed to
// install/start.
//
// selfTest starts the peripheral in TWAI_MODE_NO_ACK instead of
// TWAI_MODE_LISTEN_ONLY, which allows canBusReaderSendTestFrame() to work.
// This is for the bench smoke test only (see docs/D29-CAN-Bus-Power-Wiring.md
// section 8) -- NEVER pass true for an in-car build, since a mode that can
// transmit defeats the whole listen-only safety design.
bool canBusReaderBegin(int rxPin, int txPin, long bitRate, bool selfTest = false);

// Stops and uninstalls the TWAI driver, freeing it for a later
// canBusReaderBegin() call.
void canBusReaderEnd();

// Non-blocking: returns false immediately if no frame is queued. On true,
// fills in the CAN ID, up to 8 bytes of payload, and the payload length.
// RTR (remote-request) frames carry no data and are reported as false, same
// as the original MCP2515-based loop treated them.
bool canBusReaderReceive(uint32_t *id, uint8_t *data, uint8_t *length);

struct CanBusReaderStats {
  // The driver is installed and the controller is in its running state.
  bool running;
  // The controller went bus-off (or is recovering from it).
  bool busOff;
  // Current Receive Error Counter (REC) of the running driver instance.
  uint32_t rxErrorCounter;
  // The counters below are totals since boot. The TWAI driver resets its own
  // counters on every canBusReaderEnd()/canBusReaderBegin() cycle (which the
  // main sketch does on every 100 ms bus timeout), so they're folded into
  // running totals here.
  //
  // Bit/stuff/CRC/form errors seen on the wire. Note that in listen-only mode
  // the controller never sends error frames, and may not report every error
  // a normal node would, so a miswired bus can also look merely silent.
  uint32_t busErrors;
  // Frames lost because the driver's RX queue (rx_queue_len) was full.
  uint32_t rxMissed;
  // Frames lost in the controller's hardware RX FIFO.
  uint32_t rxOverrun;
};

// Safe to call whether or not the reader is currently running.
void canBusReaderGetStats(CanBusReaderStats *stats);

// Only valid after canBusReaderBegin(..., /*selfTest=*/true). Transmits one
// frame using TWAI's "Self Reception Request" flag, which the driver queues
// into our own RX queue on successful transmission -- per Espressif's own
// docs, this needs no external transceiver or second node on the bus, which
// is what makes the self-test build usable with zero extra bench hardware.
// Returns false if the driver's TX queue rejected it.
bool canBusReaderSendTestFrame(uint32_t id, const uint8_t *data, uint8_t length);
