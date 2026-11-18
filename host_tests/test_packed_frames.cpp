// Host-side tests for src/packed_frames.cpp. No Arduino needed:
//   g++ -std=c++11 -Wall -I../src test_packed_frames.cpp ../src/packed_frames.cpp -o t && ./t
// (run from this directory; on Windows, e.g. through WSL)

#include "packed_frames.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      failures++;                                                     \
    }                                                                 \
  } while (0)

// Frames with recognizable bytes: byte i of CAN ID x is (x & 0xF0) + i.
static void frameFor(uint32_t pid, uint8_t *out) {
  for (int i = 0; i < 8; i++) out[i] = (uint8_t)((pid & 0xF0) + i);
}

static int feed(PackedFrameBuilder &b, uint32_t pid, PackedFrame *out) {
  uint8_t d[8];
  frameFor(pid, d);
  return b.onCanFrame(pid, d, 8, out);
}

// Bit extraction as RaceChrono's bitsToUIntLe() does it.
static uint32_t bitsToUIntLe(const uint8_t *raw, int bitOffset, int bits) {
  uint32_t v = 0;
  for (int i = 0; i < bits; i++) {
    int b = bitOffset + i;
    v |= (uint32_t)((raw[b / 8] >> (b % 8)) & 1) << i;
  }
  return v;
}

static void testNothingUntilRequested() {
  PackedFrameBuilder b;
  PackedFrame out[2];
  CHECK(feed(b, 0x140, out) == 0);
  CHECK(feed(b, 0xD0, out) == 0);
  CHECK(!b.anyRequested());
}

static void testFastWaitsForAllSources() {
  PackedFrameBuilder b;
  b.setFastRequested(true);
  PackedFrame out[2];
  CHECK(feed(b, 0x140, out) == 0);  // no 0xD1 / 0x360 yet
  CHECK(feed(b, 0xD1, out) == 0);
  CHECK(feed(b, 0x140, out) == 0);  // still no 0x360
  CHECK(feed(b, 0x360, out) == 0);  // 0x360 doesn't trigger by itself
  CHECK(feed(b, 0x140, out) == 1);
  CHECK(out[0].pid == PACKED_FAST_PID);
  CHECK(out[0].length == PACKED_FAST_LENGTH);
  // RPM: 0x140[2..3], accel 0x140[0], speed 0xD1[0..1], oil/coolant 0x360[2..3]
  const uint8_t expected[7] = {0x42, 0x43, 0x40, 0xD0, 0xD1, 0x62, 0x63};
  CHECK(memcmp(out[0].data, expected, 7) == 0);
}

static void testRpmEquationMatchesOriginal() {
  // Original: bitsToUIntLe(raw, 16, 14) on 0x140. Packed: bitsToUIntLe(raw, 0, 14).
  PackedFrameBuilder b;
  b.setFastRequested(true);
  PackedFrame out[2];
  uint8_t f140[8] = {0x80, 0, 0x34, 0xD2, 0, 0, 0, 0};  // top 2 bits of [3] are flags
  uint8_t other[8] = {0};
  b.onCanFrame(0xD1, other, 8, out);
  b.onCanFrame(0x360, other, 8, out);
  CHECK(b.onCanFrame(0x140, f140, 8, out) == 1);
  CHECK(bitsToUIntLe(out[0].data, 0, 14) == bitsToUIntLe(f140, 16, 14));
  CHECK(bitsToUIntLe(out[0].data, 0, 14) == 0x1234);
}

static void testChassisOncePerCycle() {
  PackedFrameBuilder b;
  b.setChassisRequested(true);
  PackedFrame out[2];
  for (int cycle = 0; cycle < 3; cycle++) {
    CHECK(feed(b, 0xD0, out) == 0);
    CHECK(feed(b, 0xD1, out) == 0);
    CHECK(feed(b, 0xD4, out) == 1);
    CHECK(out[0].pid == PACKED_CHASSIS_PID);
    CHECK(out[0].length == PACKED_CHASSIS_LENGTH);
  }
  const uint8_t expected[15] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD6, 0xD7, 0xD2,
                                0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
  CHECK(memcmp(out[0].data, expected, 15) == 0);
}

static void testChassisAnyArrivalOrder() {
  PackedFrameBuilder b;
  b.setChassisRequested(true);
  PackedFrame out[2];
  int sent = 0;
  const uint32_t order[] = {0xD4, 0xD0, 0xD1};
  for (int cycle = 0; cycle < 10; cycle++)
    for (uint32_t pid : order) sent += feed(b, pid, out);
  CHECK(sent == 10);  // exactly one per 50 Hz cycle
}

static void testChassisMissedFrameDoesNotStall() {
  PackedFrameBuilder b;
  b.setChassisRequested(true);
  PackedFrame out[2];
  feed(b, 0xD0, out);
  feed(b, 0xD1, out);
  CHECK(feed(b, 0xD4, out) == 1);
  // Next cycle loses 0xD1 on the bus.
  CHECK(feed(b, 0xD0, out) == 0);
  CHECK(feed(b, 0xD4, out) == 0);
  // The cycle after: 0xD0 again before the frame went out -> send what we
  // have instead of overwriting the unsent 0xD0 update.
  CHECK(feed(b, 0xD0, out) == 1);
  CHECK(out[0].pid == PACKED_CHASSIS_PID);
  CHECK(feed(b, 0xD1, out) == 0);
  CHECK(feed(b, 0xD4, out) == 1);  // back in step
}

static void testShortFramesIgnored() {
  PackedFrameBuilder b;
  b.setFastRequested(true);
  PackedFrame out[2];
  uint8_t d[8] = {0};
  b.onCanFrame(0xD1, d, 8, out);
  b.onCanFrame(0x360, d, 8, out);
  CHECK(b.onCanFrame(0x140, d, 3, out) == 0);  // RPM needs bytes 2-3
}

// Regression: 0xD1 is shorter than 8 bytes on the car. Requiring 8 bytes
// from every source meant neither packed frame was ever sent.
static void testShortD1Accepted() {
  PackedFrameBuilder b;
  b.setFastRequested(true);
  b.setChassisRequested(true);
  PackedFrame out[2];
  uint8_t d1[4] = {0x11, 0x22, 0x33, 0x44};
  feed(b, 0x360, out);
  feed(b, 0xD0, out);
  CHECK(b.onCanFrame(0xD1, d1, 4, out) == 0);
  CHECK(feed(b, 0xD4, out) == 1 && out[0].pid == PACKED_CHASSIS_PID);
  CHECK(out[0].data[6] == 0x33);  // brake
  CHECK(feed(b, 0x140, out) == 1 && out[0].pid == PACKED_FAST_PID);
  CHECK(out[0].data[3] == 0x11 && out[0].data[4] == 0x22);  // speed
  CHECK(b.onCanFrame(0xD1, d1, 2, out) == 0);  // too short for brake: ignored
}

static void testBothFramesAtOnce() {
  PackedFrameBuilder b;
  b.setFastRequested(true);
  b.setChassisRequested(true);
  PackedFrame out[2];
  feed(b, 0x360, out);
  feed(b, 0xD0, out);
  feed(b, 0xD1, out);
  feed(b, 0x140, out);  // fast frame
  int n = feed(b, 0xD4, out);
  CHECK(n == 1 && out[0].pid == PACKED_CHASSIS_PID);
  CHECK(feed(b, 0x140, out) == 1 && out[0].pid == PACKED_FAST_PID);
}

static void testResetForgetsEverything() {
  PackedFrameBuilder b;
  b.setFastRequested(true);
  PackedFrame out[2];
  feed(b, 0xD1, out);
  feed(b, 0x360, out);
  CHECK(feed(b, 0x140, out) == 1);
  b.reset();
  CHECK(!b.anyRequested());
  b.setFastRequested(true);
  CHECK(feed(b, 0x140, out) == 0);  // cache was cleared too
}

int main() {
  testNothingUntilRequested();
  testFastWaitsForAllSources();
  testRpmEquationMatchesOriginal();
  testChassisOncePerCycle();
  testChassisAnyArrivalOrder();
  testChassisMissedFrameDoesNotStall();
  testShortFramesIgnored();
  testShortD1Accepted();
  testBothFramesAtOnce();
  testResetForgetsEverything();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("All packed frame tests passed\n");
  return 0;
}
