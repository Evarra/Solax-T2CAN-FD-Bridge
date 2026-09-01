# SolaX SOC / charge-current bridge for LILYGO T-2CAN-FD

This firmware places a **LILYGO T-2CAN-FD V1.0 (MCP2518FD)** between a SolaX battery and a SolaX inverter. It transparently forwards Classic CAN data frames in both directions and changes only the battery-to-inverter SOC and maximum charge-current fields. The MCP2518FD can receive FD frames, but all SolaX frames are transmitted as Classic CAN.

## What it does

The defaults in `src/config.h` implement this curve:

| Real battery SOC | SOC sent to inverter | Battery charge-current limit passed through |
|---:|---:|---:|
| 89% or lower | unchanged | 100% of the battery's own limit |
| 90% | 90% | 100% |
| 91% | 92% | 80% |
| 92% | 94% | 60% |
| 93% | 96% | 40% |
| 94% | 98% | 20% |
| 95% or higher | 100% | 0% (0 A) |

Example: if the battery reports 100.0 A as its maximum charge current at a real SOC of 93%, the bridge sends 40.0 A and 96% SOC to the inverter.

The modified SolaX frames are:

| CAN ID | Direction | Modified field |
|---:|---|---|
| `0x1872` extended | Battery -> inverter | Bytes 4-5, little-endian maximum charge current, 0.1 A/unit |
| `0x1873` extended | Battery -> inverter | Byte 4, SOC in whole percent |
| `0x187E` extended | Battery -> inverter | Byte 5, duplicate SOC used by SolaX Ultra |

The discharge-current limit in `0x1872` bytes 6-7 is never changed. All inverter-to-battery frames, including the `0x1871` polling/contactor requests, are forwarded unchanged.

If no valid SOC has been received yet, or SOC is older than 2 seconds, the next forwarded maximum charge-current limit is forced to **0 A**. This avoids allowing full charge current from a stale or unknown SOC value.

## Connections

| T-2CAN port | Connect to |
|---|---|
| CAN A (MCP2518FD) | SolaX battery CAN |
| CAN B (ESP32 native CAN) | SolaX inverter BMS CAN |

Connect CAN-H to CAN-H and CAN-L to CAN-L. Also follow the equipment pinout for the CAN reference/GND. Do not assume an RJ45 pinout: verify it for the exact inverter and battery model.

The bridge splits the original bus into two separate CAN segments. Each segment must have correct termination. With power off, a correctly terminated segment normally measures about 60 ohms between CAN-H and CAN-L. Check the actual T-2CAN board termination arrangement before connecting it.

## Build and upload

1. Install VS Code and PlatformIO.
2. Open this folder as a PlatformIO project.
3. Connect the T-2CAN by USB.
4. Build and upload the `lilygo_t2can_fd` environment.
5. Open the serial monitor at 115200 baud.

The serial monitor shows real SOC, SOC sent to the inverter, original/limited charge current, forwarded frame counters, drops, and CAN errors.

## Build a binary with GitHub Actions

The project contains `.github/workflows/build-firmware.yml`. GitHub automatically builds the firmware after every push, pull request, or manual workflow run.

1. Create an empty GitHub repository.
2. Unzip this project and upload/push the **contents** of the `Solax-T2CAN-FD-Bridge` folder. `platformio.ini` must be in the repository root; do not upload only the ZIP file.
3. Open the repository's **Actions** tab.
4. Select **Build T-2CAN-FD firmware**. It starts automatically after the first push, or choose **Run workflow**.
5. Open the completed run and download the `solax-t2can-fd-firmware` artifact.

The artifact contains:

- `t2can-fd-full-flash.bin` — complete first-install image, flashed at offset `0x0`
- `t2can-fd-firmware.bin` — application-only image, flashed at `0x10000`
- bootloader and partition binaries
- `FLASHING.txt` with commands
- SHA-256 checksums

For a new board, use `t2can-fd-full-flash.bin`. The application-only file is intended for later updates after a compatible full image is already installed.

## Configuration

Edit `src/config.h` to change:

- `TAPER_START_SOC_PCT` (default 90)
- `TAPER_STOP_SOC_PCT` (default 95)
- `SOC_FRESHNESS_TIMEOUT_MS` (default 2000 ms)

Current limiting is proportional to the battery's own reported maximum charge current, so the bridge can only reduce the battery's limit; it never increases it.

## Important checks before live use

- This project targets **T-2CAN-FD V1.0 with MCP2518FD**. It does not target the older MCP2515 board.
- Confirm the real bus uses 500 kbit/s and extended SolaX frames.
- First test with the inverter unable to energize the high-voltage system, and verify the serial counters and captured CAN frames.
- Verify `0x1873` byte 4 and `0x1872` bytes 4-5 against your battery capture before enabling charging.
- Confirm the inverter really obeys a 0 A maximum-charge-current command at the top of charge.
- Keep the battery BMS's own voltage/current protections active. This bridge is an additional control layer, not a replacement for BMS protection.

## Source note

The SolaX frame definitions, LILYGO T-2CAN-FD pin mapping, and the vendored `ACAN2517FD` driver were taken from the supplied Battery Emulator source. See `THIRD_PARTY_NOTICES.md` and `LICENSE`.
