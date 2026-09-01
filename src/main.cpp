#include <Arduino.h>
#include <SPI.h>
#include <driver/twai.h>

#include <ACAN2517FD.h>

#include "config.h"
#include "solax_bridge_logic.h"

namespace {

SPIClass canASpi(FSPI);
ACAN2517FD canA(bridge_config::CAN_A_CS_PIN, canASpi, bridge_config::CAN_A_INT_PIN);
SolaxBridgeLogic solaxLogic;

struct BridgeStats {
  uint32_t batteryToInverter = 0;
  uint32_t inverterToBattery = 0;
  uint32_t droppedToInverter = 0;
  uint32_t droppedToBattery = 0;
  uint32_t unsupportedRemoteFrames = 0;
  uint32_t unsupportedFdFrames = 0;
  uint32_t canAErrors = 0;
  uint32_t canBErrors = 0;
} stats;

uint32_t lastStatusMs = 0;
bool canBRecovering = false;

[[noreturn]] void stopWithError(const char* message) {
  Serial.println(message);
  while (true) {
    delay(1000);
  }
}

void initializeCanA() {
  canASpi.begin(bridge_config::CAN_A_SCK_PIN, bridge_config::CAN_A_MISO_PIN,
                bridge_config::CAN_A_MOSI_PIN, bridge_config::CAN_A_CS_PIN);

  ACAN2517FDSettings settings(ACAN2517FDSettings::OSC_40MHz, bridge_config::CAN_BITRATE,
                              DataBitRateFactor::x4);
  // NormalFD receives both Classic CAN and CAN-FD. SolaX traffic is always
  // transmitted below as Classic CAN data frames.
  settings.mRequestedMode = ACAN2517FDSettings::NormalFD;
  settings.mControllerTransmitFIFOPayload = ACAN2517FDSettings::PAYLOAD_8;
  settings.mControllerReceiveFIFOPayload = ACAN2517FDSettings::PAYLOAD_8;
  settings.mControllerTXQBufferPayload = ACAN2517FDSettings::PAYLOAD_8;

  const uint32_t errorCode = canA.begin(settings, [] { canA.isr(); });
  canA.poll();
  if (errorCode != 0) {
    Serial.printf("MCP2518FD configuration error: 0x%08lX\n", static_cast<unsigned long>(errorCode));
    stopWithError("ERROR: CAN A / MCP2518FD initialization failed");
  }
  Serial.println("CAN A ready: battery side, MCP2518FD, Classic CAN at 500 kbit/s");
}

void initializeCanB() {
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(static_cast<gpio_num_t>(bridge_config::CAN_B_TX_PIN),
                                  static_cast<gpio_num_t>(bridge_config::CAN_B_RX_PIN), TWAI_MODE_NORMAL);
  general.tx_queue_len = 32;
  general.rx_queue_len = 64;
  general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_RX_QUEUE_FULL |
                           TWAI_ALERT_TX_FAILED;

  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&general, &timing, &filter) != ESP_OK) {
    stopWithError("ERROR: CAN B / native TWAI driver installation failed");
  }
  if (twai_start() != ESP_OK) {
    stopWithError("ERROR: CAN B / native TWAI start failed");
  }
  Serial.println("CAN B ready: inverter side, native TWAI, 500 kbit/s");
}

bool sendToInverter(const CanFrame& source) {
  twai_message_t target = {};
  target.identifier = source.id;
  target.extd = source.extended ? 1 : 0;
  target.rtr = 0;
  target.data_length_code = source.length > 8 ? 8 : source.length;
  memcpy(target.data, source.data, target.data_length_code);
  return twai_transmit(&target, 0) == ESP_OK;
}

bool sendToBattery(const twai_message_t& source) {
  if (source.rtr) {
    ++stats.unsupportedRemoteFrames;
    return false;
  }

  CANFDMessage target;
  target.type = CANFDMessage::CAN_DATA;
  target.ext = source.extd;
  target.id = source.identifier;
  target.len = source.data_length_code > 8 ? 8 : source.data_length_code;
  memcpy(target.data, source.data, target.len);
  return canA.tryToSend(target);
}

void forwardBatteryToInverter(uint32_t nowMs) {
  CANFDMessage incoming;
  uint8_t processed = 0;
  while (processed++ < 64 && canA.available() && canA.receive(incoming)) {
    if (incoming.type == CANFDMessage::CAN_REMOTE) {
      ++stats.unsupportedRemoteFrames;
      ++stats.droppedToInverter;
      continue;
    }
    if (incoming.type != CANFDMessage::CAN_DATA || incoming.len > 8) {
      // Do not accidentally convert an FD frame into a Classic CAN frame.
      ++stats.unsupportedFdFrames;
      ++stats.droppedToInverter;
      continue;
    }

    CanFrame frame;
    frame.id = incoming.id;
    frame.extended = incoming.ext;
    frame.length = incoming.len;
    memcpy(frame.data, incoming.data, frame.length);

    solaxLogic.processBatteryToInverter(frame, nowMs);
    if (sendToInverter(frame)) {
      ++stats.batteryToInverter;
    } else {
      ++stats.droppedToInverter;
    }
  }
}

void forwardInverterToBattery() {
  twai_message_t incoming = {};
  uint8_t processed = 0;
  while (processed++ < 64 && twai_receive(&incoming, 0) == ESP_OK) {
    if (sendToBattery(incoming)) {
      ++stats.inverterToBattery;
    } else {
      ++stats.droppedToBattery;
    }
  }
}

void serviceCanErrors() {
  if (canA.hasCanErrors()) {
    ++stats.canAErrors;
    const bool recovered = canA.recoverFromRestrictedOperationMode();
    Serial.printf("WARNING: CAN A controller reported an error%s\n", recovered ? "; recovered" : "");
  }

  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) != ESP_OK) {
    return;
  }

  if (alerts & (TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_TX_FAILED)) {
    ++stats.canBErrors;
  }
  if (alerts & TWAI_ALERT_BUS_OFF) {
    ++stats.canBErrors;
    canBRecovering = (twai_initiate_recovery() == ESP_OK);
    Serial.println("WARNING: CAN B bus-off; recovery started");
  }
  if ((alerts & TWAI_ALERT_BUS_RECOVERED) && canBRecovering) {
    canBRecovering = false;
    if (twai_start() == ESP_OK) {
      Serial.println("CAN B recovered");
    } else {
      Serial.println("ERROR: CAN B could not restart after recovery");
    }
  }
}

void printStatus(uint32_t nowMs) {
  if (static_cast<uint32_t>(nowMs - lastStatusMs) < bridge_config::STATUS_INTERVAL_MS) {
    return;
  }
  lastStatusMs = nowMs;

  Serial.printf("SOC real=%s", solaxLogic.haveSoc() ? "" : "N/A");
  if (solaxLogic.haveSoc()) {
    Serial.printf("%u%% inverter=%u%% fresh=%s", solaxLogic.actualSocPct(), solaxLogic.inverterSocPct(),
                  solaxLogic.socIsFresh(nowMs) ? "yes" : "no");
  }
  Serial.printf(" | charge limit %.1fA -> %.1fA | frames A>B=%lu B>A=%lu | drops=%lu/%lu | errors=%lu/%lu | FD ignored=%lu\n",
                solaxLogic.lastOriginalChargeCurrentdA() / 10.0,
                solaxLogic.lastLimitedChargeCurrentdA() / 10.0,
                static_cast<unsigned long>(stats.batteryToInverter),
                static_cast<unsigned long>(stats.inverterToBattery),
                static_cast<unsigned long>(stats.droppedToInverter),
                static_cast<unsigned long>(stats.droppedToBattery), static_cast<unsigned long>(stats.canAErrors),
                static_cast<unsigned long>(stats.canBErrors),
                static_cast<unsigned long>(stats.unsupportedFdFrames));
}

}  // namespace

void setup() {
  Serial.begin(bridge_config::SERIAL_BAUD);
  delay(500);
  Serial.println();
  Serial.println("SolaX T-2CAN-FD SOC/charge-current bridge starting");
  initializeCanA();
  initializeCanB();
  Serial.println("Bridge active. Unknown/stale SOC makes the forwarded charge limit 0 A.");
}

void loop() {
  const uint32_t nowMs = millis();
  forwardBatteryToInverter(nowMs);
  forwardInverterToBattery();
  serviceCanErrors();
  printStatus(nowMs);
  delay(1);
}
