#pragma once

#include <stdint.h>

namespace bridge_config {

// SolaX Triple Power CAN uses 500 kbit/s in the Battery Emulator implementation.
constexpr uint32_t CAN_BITRATE = 500000;

// Real battery SOC range that is expanded for the inverter.
// 90% real -> 90% reported, 95% real -> 100% reported.
constexpr uint8_t TAPER_START_SOC_PCT = 90;
constexpr uint8_t TAPER_STOP_SOC_PCT = 95;

// A limit frame received without a recent valid SOC is made fail-safe (0 A charge).
constexpr uint32_t SOC_FRESHNESS_TIMEOUT_MS = 2000;

// LILYGO T-2CAN-FD V1.0 (MCP2518FD) CAN A pinout.
constexpr int CAN_A_SCK_PIN = 12;
constexpr int CAN_A_MOSI_PIN = 11;
constexpr int CAN_A_MISO_PIN = 13;
constexpr int CAN_A_CS_PIN = 10;
constexpr int CAN_A_INT_PIN = 8;

constexpr int CAN_B_TX_PIN = 7;
constexpr int CAN_B_RX_PIN = 6;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t STATUS_INTERVAL_MS = 1000;

static_assert(TAPER_START_SOC_PCT < TAPER_STOP_SOC_PCT, "SOC taper range must be increasing");
static_assert(TAPER_STOP_SOC_PCT <= 100, "SOC taper stop must be <= 100");

}  // namespace bridge_config
