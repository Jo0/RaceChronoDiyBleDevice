#pragma once

// Packed "virtual" CAN frames for FT86 gen1 (2013-2020 BRZ / 86 / FR-S).
//
// Instead of forwarding five real CAN IDs raw (~270 BLE notifications/s at
// native rates), the bytes RaceChrono's channels actually use are copied into
// two virtual CAN IDs (~150 notifications/s), with no loss of update rate:
//
//   0x7A0 (1952), 7 bytes, 100 Hz -- sent on every 0x140 frame:
//     0-1  RPM          <- 0x140[2..3]  bitsToUIntLe(raw, 0, 14)
//     2    accelerator  <- 0x140[0]     bytesToUIntLe(raw, 2, 1) / 2.55
//     3-4  speed        <- 0xD1[0..1]   bytesToIntLe(raw, 3, 2) * 0.015694
//     5    oil temp     <- 0x360[2]     bytesToUIntLe(raw, 5, 1) - 40
//     6    coolant temp <- 0x360[3]     bytesToUIntLe(raw, 6, 1) - 40
//
//   0x7A1 (1953), 15 bytes, 50 Hz -- sent once 0xD0, 0xD1 and 0xD4 have all
//   updated (they're sent together, 50 times a second):
//     0-1   steering     <- 0xD0[0..1]
//     2-3   yaw rate     <- 0xD0[2..3]
//     4     lateral G    <- 0xD0[6]
//     5     long. G      <- 0xD0[7]
//     6     brake        <- 0xD1[2]
//     7-14  wheel speeds <- 0xD4[0..7]  FL, FR, RL, RR
//
// Speed and the temperatures ride in the 100 Hz frame because the 50 Hz one
// is full (0xD0 + 0xD1 + 0xD4 would be 17 bytes, the API allows 16); they're
// simply repeated between bus updates. The full RaceChrono equations are in
// can_db/ft86.md.
//
// A virtual frame is only built once every source frame it needs has been
// seen at least once, so RaceChrono never sees placeholder zeros (e.g. a
// coolant temperature of -40 C right after connecting).
//
// Plain C++ with no Arduino or BLE dependency, so it can be tested on a PC.

#include <stdint.h>

const uint32_t PACKED_FAST_PID = 0x7A0;     // 1952
const uint32_t PACKED_CHASSIS_PID = 0x7A1;  // 1953

const uint8_t PACKED_FAST_LENGTH = 7;
const uint8_t PACKED_CHASSIS_LENGTH = 15;
const uint8_t PACKED_MAX_LENGTH = 16;  // RaceChrono DIY API payload limit.

struct PackedFrame {
  uint32_t pid;
  uint8_t data[PACKED_MAX_LENGTH];
  uint8_t length;
};

class PackedFrameBuilder {
public:
  PackedFrameBuilder() { reset(); }

  // Forgets all cached data and which frames were requested.
  void reset();

  // Which virtual frames RaceChrono asked for. Only requested frames are
  // built, and only their source IDs count as needed.
  void setFastRequested(bool requested) { fastRequested = requested; }
  void setChassisRequested(bool requested) { chassisRequested = requested; }
  bool anyRequested() const { return fastRequested || chassisRequested; }

  // True if this is the virtual PID of a packed frame.
  static bool isPackedPid(uint32_t pid) {
    return pid == PACKED_FAST_PID || pid == PACKED_CHASSIS_PID;
  }

  // Feed every CAN frame received from the bus. Writes 0, 1 or 2 finished
  // virtual frames to |out| (which must hold 2) and returns how many.
  int onCanFrame(uint32_t pid, const uint8_t *data, uint8_t length,
                 PackedFrame *out);

private:
  enum Source { SRC_D0, SRC_D1, SRC_D4, SRC_140, SRC_360, NUM_SOURCES };

  // Latest payload of each source ID, and whether it was ever received.
  uint8_t latest[NUM_SOURCES][8];
  bool seen[NUM_SOURCES];

  // For the chassis frame: which of 0xD0/0xD1/0xD4 changed since it was
  // last sent.
  bool chassisPending[3];

  bool fastRequested;
  bool chassisRequested;

  void buildFast(PackedFrame *out) const;
  void buildChassis(PackedFrame *out);
};
