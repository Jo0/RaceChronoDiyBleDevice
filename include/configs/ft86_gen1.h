// Customizations for 1st generation Subaru BRZ / Toyota 86 / Scion FR-S
// (2013-2020 model years).

#if !defined(RACECHRONO_BIY_BLE_DEVICE_FT86_GEN1_H)
#define RACECHRONO_BIY_BLE_DEVICE_FT86_GEN1_H

const long BAUD_RATE = 500 * 1e3;  // 500k.

// Every requested CAN ID is forwarded at its native bus rate. The dividers
// inherited from upstream (25 Hz targets) were sized for an nRF52840 polling
// an MCP2515 over SPI; this board reads CAN through the ESP32-S3's own TWAI
// controller, so there's no reason to throw data away up front. Only IDs
// RaceChrono asked for are forwarded at all (see bufferNewPacket()).
//
// Native rates of the IDs in can_db/ft86.md, for reference:
//   0x140, 0x141, 0x142, 0x18                        100 Hz
//   0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0x144, 0x152,
//   0x156, 0x280                                      50 Hz
//   0x360                                             20 Hz
//
// To throttle a specific ID again (e.g. if BLE can't keep up), return N > 1
// for it here: only every Nth frame of that ID is then sent.
uint8_t getUpdateRateDivider(uint32_t can_id) {
  (void)can_id;
  return 1;
}

#endif // RACECHRONO_BIY_BLE_DEVICE_FT86_GEN1_H
