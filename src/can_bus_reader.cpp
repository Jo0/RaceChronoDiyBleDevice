#include "can_bus_reader.h"

#include <string.h>

#include "driver/twai.h"

bool canBusReaderBegin(int rxPin, int txPin, long bitRate, bool selfTest) {
  twai_mode_t mode = selfTest ? TWAI_MODE_NO_ACK : TWAI_MODE_LISTEN_ONLY;
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)txPin, (gpio_num_t)rxPin, mode);

  // The macro's default rx_queue_len (5 in current IDF versions) is sized
  // for a generic example, not for bursts while a BLE notify is in flight.
  // Our own software ring buffer downstream (see the main sketch) is 16
  // deep; give the hardware queue similar headroom so we don't drop frames
  // between the ISR and the next loop() iteration.
  g_config.rx_queue_len = 32;

  twai_timing_config_t t_config;
  switch (bitRate) {
    case 1000000: t_config = TWAI_TIMING_CONFIG_1MBITS();   break;
    case 500000:  t_config = TWAI_TIMING_CONFIG_500KBITS(); break;
    case 250000:  t_config = TWAI_TIMING_CONFIG_250KBITS(); break;
    case 125000:  t_config = TWAI_TIMING_CONFIG_125KBITS(); break;
    case 100000:  t_config = TWAI_TIMING_CONFIG_100KBITS(); break;
    case 50000:   t_config = TWAI_TIMING_CONFIG_50KBITS();  break;
    default:
      // Unsupported bit rate -- add a TWAI_TIMING_CONFIG_*BITS() case above
      // if a car needs one (see driver/twai.h for the full list).
      return false;
  }

  // RaceChrono itself decides which PIDs it wants via BLE (see
  // RaceChronoBleCanHandler::allowPid() in the main sketch); the hardware
  // filter here just needs to admit everything.
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
    return false;
  }

  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }

  return true;
}

void canBusReaderEnd() {
  twai_stop();
  twai_driver_uninstall();
}

bool canBusReaderReceive(uint32_t *id, uint8_t *data, uint8_t *length) {
  twai_message_t message;
  // ticks_to_wait = 0: poll only, never block the caller's loop().
  if (twai_receive(&message, 0) != ESP_OK) {
    return false;
  }

  if (message.rtr) {
    // Remote-request frames carry no data; nothing to relay.
    return false;
  }

  *id = message.identifier;
  *length = message.data_length_code;
  memcpy(data, message.data, message.data_length_code);
  return true;
}

bool canBusReaderSendTestFrame(uint32_t id, const uint8_t *data, uint8_t length) {
  twai_message_t message = {};
  message.identifier = id;
  message.data_length_code = length;
  memcpy(message.data, data, length);
  // Self Reception Request: the driver queues this frame into our own RX
  // queue as soon as it's transmitted, without requiring an external
  // transceiver or another node on the bus to see it. See canBusReaderBegin's
  // selfTest parameter -- this only works when TWAI_MODE_NO_ACK was used.
  message.self = 1;

  return twai_transmit(&message, pdMS_TO_TICKS(10)) == ESP_OK;
}
