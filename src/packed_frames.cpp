#include "packed_frames.h"

#include <string.h>

void PackedFrameBuilder::reset() {
  memset(latest, 0, sizeof(latest));
  memset(seen, 0, sizeof(seen));
  memset(chassisPending, 0, sizeof(chassisPending));
  fastRequested = false;
  chassisRequested = false;
}

int PackedFrameBuilder::onCanFrame(uint32_t pid, const uint8_t *data,
                                   uint8_t length, PackedFrame *out) {
  // |needed| is how many leading bytes of the frame get packed. Not every
  // source is a full 8-byte frame on the car (0xD1, for one, is shorter), so
  // only those bytes are required; a frame too short to hold them would leave
  // stale bytes in the cache, so it's ignored rather than packing garbage.
  int source;
  uint8_t needed;
  switch (pid) {
    case 0xD0: source = SRC_D0; needed = 8; break;    // bytes 0-3, 6, 7
    case 0xD1: source = SRC_D1; needed = 3; break;    // bytes 0-2
    case 0xD4: source = SRC_D4; needed = 8; break;    // bytes 0-7
    case 0x140: source = SRC_140; needed = 4; break;  // bytes 0, 2, 3
    case 0x360: source = SRC_360; needed = 4; break;  // bytes 2, 3
    default: return 0;
  }
  if (length < needed) {
    return 0;
  }
  if (length > 8) {
    length = 8;
  }

  int count = 0;
  bool isChassisSource =
      source == SRC_D0 || source == SRC_D1 || source == SRC_D4;

  // A chassis source arriving again before the frame went out means a
  // sibling frame was missed on the bus. Send what we have now rather than
  // overwrite an update RaceChrono hasn't seen.
  if (chassisRequested && isChassisSource && chassisPending[source] &&
      seen[SRC_D0] && seen[SRC_D1] && seen[SRC_D4]) {
    buildChassis(&out[count++]);
  }

  memset(latest[source], 0, 8);
  memcpy(latest[source], data, length);
  seen[source] = true;

  if (chassisRequested && isChassisSource) {
    chassisPending[source] = true;
    if (chassisPending[SRC_D0] && chassisPending[SRC_D1] &&
        chassisPending[SRC_D4] && count == 0) {
      buildChassis(&out[count++]);
    }
  }

  if (fastRequested && source == SRC_140 && seen[SRC_140] && seen[SRC_D1] &&
      seen[SRC_360]) {
    buildFast(&out[count++]);
  }

  return count;
}

void PackedFrameBuilder::buildFast(PackedFrame *out) const {
  const uint8_t *f140 = latest[SRC_140];
  const uint8_t *d1 = latest[SRC_D1];
  const uint8_t *f360 = latest[SRC_360];

  out->pid = PACKED_FAST_PID;
  out->length = PACKED_FAST_LENGTH;
  memset(out->data, 0, sizeof(out->data));
  out->data[0] = f140[2];  // RPM, 14 bits LE
  out->data[1] = f140[3];
  out->data[2] = f140[0];  // accelerator
  out->data[3] = d1[0];    // speed
  out->data[4] = d1[1];
  out->data[5] = f360[2];  // oil temp
  out->data[6] = f360[3];  // coolant temp
}

void PackedFrameBuilder::buildChassis(PackedFrame *out) {
  const uint8_t *d0 = latest[SRC_D0];
  const uint8_t *d1 = latest[SRC_D1];
  const uint8_t *d4 = latest[SRC_D4];

  out->pid = PACKED_CHASSIS_PID;
  out->length = PACKED_CHASSIS_LENGTH;
  memset(out->data, 0, sizeof(out->data));
  memcpy(out->data, d0, 4);  // steering, yaw rate
  out->data[4] = d0[6];      // lateral G
  out->data[5] = d0[7];      // longitudinal G
  out->data[6] = d1[2];      // brake
  memcpy(out->data + 7, d4, 8);  // wheel speeds

  chassisPending[SRC_D0] = false;
  chassisPending[SRC_D1] = false;
  chassisPending[SRC_D4] = false;
}
