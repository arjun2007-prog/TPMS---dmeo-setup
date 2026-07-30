# TPMS — Tire Pressure Monitoring System

A low-cost, cellular-connected Tire Pressure Monitoring System built around BLE sensor advertisements, a microcontroller decode/upload pipeline, and a live web dashboard.

## Repository Structure

This repo contains three standalone source files — it is **not** a full buildable project on its own. Each file is meant to be dropped into its respective toolchain (see the setup section for each below).

```
.
├── main.c                # nRF52810 BLE observer firmware
├── tpms_mega.ino          # Arduino Mega decode + upload firmware
├── tpms_dashboard.html    # Live web dashboard
└── README.md
```

## Architecture

```
[BLE Sensor / Beacon Simulator]
        │  (iBeacon advertisement: UUID + Major + Minor)
        ▼
[nRF52810 — BLE Observer]
        │  (filters advertisements, forwards MAC + raw hex over UART)
        ▼
[Arduino Mega]
        │  (parses iBeacon payload, decodes pressure/temperature,
        │   sends AT commands to GSM module)
        ▼
[SIMCom A7672S — Cellular Module]
        │  (plain HTTP GET upload)
        ▼
[ThingSpeak Cloud Channel]
        │  (stores + serves readings via REST API)
        ▼
[HTML Dashboard]
        (polls ThingSpeak, renders live tire pressure/temperature)
```

---

## 1. nRF52810 — BLE Observer Firmware

**File:** `main.c`
**SDK:** nRF5 SDK 17.1.0, S132 SoftDevice (required to build — not included in this repo, see [Build Requirements](#build-requirements))
**Role:** Continuously scans for BLE advertisements in Observer-only mode (no connection made), filters for advertisements carrying the project's chosen iBeacon Proximity UUID, and forwards each matching advertisement's MAC address and raw advertisement bytes over UART to the Arduino Mega for decoding.

### Build Requirements
This file is designed to be dropped into an existing nRF5 SDK 17.1.0 example project (originally based on `ble_app_uart_c`), replacing the example's default `main.c`. The SDK itself, linker scripts, and Makefile are not included in this repo — download SDK 17.1.0 from Nordic Semiconductor and place this file inside:
```
<nRF5_SDK_17.1.0>/examples/ble_central/ble_app_uart_c/<your_board_folder>/s132/armgcc/main.c
```

**SoftDevice: S112 → S132.** The nRF52810 on the ISC_DEVKIT ships configured for the **S112** SoftDevice, which only supports the Peripheral BLE role. This project requires **BLE Central/Observer scanning**, which S112 does not support. **S132** must be flashed in its place — it supports both Central and Peripheral roles and is required for the scanning logic in `main.c` to function. Flash S132 before flashing this application:
```bash
nrfjprog --eraseall
nrfjprog --program s132_nrf52_7.2.0_softdevice.hex --sectorerase
```
(Adjust the S132 hex filename/version to match the SoftDevice release bundled with SDK 17.1.0.)

**Linker memory layout.** The nRF52810 has 192KB flash (0x30000) and 24KB RAM (0x6000) total. S132 reserves a larger portion of both than S112 for its own BLE stack, so the application's linker script (`<board>_gcc_nrf52.ld`) must place the `FLASH` and `RAM` regions *above* S132's reserved space rather than reusing the original S112-based boundaries from the example project. The values used in this project:

```ld
MEMORY
{
  FLASH (rx) : ORIGIN = 0x26000, LENGTH = 0xA000
  RAM (rwx)  : ORIGIN = 0x20003400, LENGTH = 0x2C00
}
```

- `FLASH ORIGIN = 0x26000` — start of application flash, immediately after S132's reserved flash region
- `RAM ORIGIN = 0x20003400` — start of application RAM, set with headroom above S132's reserved RAM to avoid the application's variables overlapping SoftDevice-owned memory
- `RAM LENGTH = 0x2C00` — sized so the region still ends at the chip's real RAM ceiling (`0x20006000`)

These exact `ORIGIN`/`LENGTH` values are specific to this application's BLE configuration (Observer-only, no peripheral/connectable role, minimal GATT usage). If you change the BLE configuration (add peripheral role, more GATT services/characteristics, larger buffers, etc.), **recalculate the real RAM boundary S132 needs** via the value `nrf_sdh_ble_default_cfg_set()` reports at runtime, rather than reusing these numbers as-is. Confirm your build fits within the remaining space using:
```bash
make --print-memory-usage
```
Mismatched boundaries here will cause the application to silently corrupt its own memory or HardFault shortly after boot — this is the most common failure mode when porting BLE Central examples onto boards originally configured for S112.

### Build & Flash
```bash
cd <path_to_your_example_folder>/armgcc
make
openocd -f interface/ch347.cfg -f target/nrf52.cfg -c "program _build/*.hex verify reset exit"
```

### Configuration
- Set the target Proximity UUID to match your beacon/sensor source (see scan filter near the top of `main.c`)
- UART output format sent to the Mega: `MAC,RAWHEXDATA\n`

### Hardware
- Chip: nRF52810 (192KB flash / 24KB RAM)
- Board: ISC_DEVKIT_nRF52810
- Debugger: CH347-based USB-to-JTAG/SWD adapter

---

## 2. Arduino Mega — Decode & Upload Firmware

**File:** `tpms_mega.ino`
**Role:** Receives raw BLE payloads from the nRF52810 over `Serial3`, validates and parses the iBeacon structure to extract Major/Minor, decodes pressure (psi) and temperature (°C) from the Minor value, and uploads readings to ThingSpeak via the A7672S cellular module using plain HTTP (no TLS — see [Notes](#notes--known-limitations)).

### Wiring
| Signal | Mega Pin | Connects to |
|---|---|---|
| USB Serial | `Serial` | PC (debug output) |
| GSM UART | `Serial1` (18/19) | SIMCom A7672S |
| BLE UART | `Serial3` (14/15) | nRF52810 |
| GSM PWRKEY | Pin 3 | A7672S PWRKEY |
| GSM RESET | Pin 2 | A7672S RESET |

### Decode logic
The BLE Minor value is split into two bytes:
```
pressure_byte = high byte of Minor  →  pressure_kPa = pressure_byte × 2.5
temperature_C = low byte of Minor − 40
```

### Configuration
Set the following near the top of the sketch before flashing:
```cpp
const char* TS_API_KEY = "YOUR_THINGSPEAK_WRITE_KEY";
const char* TS_HOST    = "api.thingspeak.com";
```


### ThingSpeak field mapping
| Field | Data |
|---|---|
| field1 | Pressure (psi) |
| field2 | Temperature (°C) |
| field3 | Sensor tag (Minor value) |

---

## 3. HTML Dashboard

**File:** `tpms_dashboard.html`
**Role:** A single-file, no-backend static dashboard that polls the ThingSpeak REST API directly from the browser and renders a live instrument-cluster-style view of tire readings.

### How it works
- Fetches `https://api.thingspeak.com/channels/<CHANNEL_ID>/feeds/last.json?api_key=<READ_KEY>` every 15 seconds
- Displays a top-down car visualization with four tire panels
- One panel reflects live sensor data (green, pulsing "LIVE" indicator); the remaining three are placeholders for future sensor positions (grey, "NO SENSOR")

### Configuration
Set these constants near the bottom of the file:
```javascript
const CHANNEL_ID = "YOUR_CHANNEL_ID";
const READ_API_KEY = "YOUR_THINGSPEAK_READ_KEY";
```


### Hosting
This is a static file with no server dependency. Deploy via any static host:
- Netlify Drop (drag-and-drop, instant URL)
- GitHub Pages
- Vercel

No build step, no backend, no environment variables required.

---

## Notes / Known Limitations

- **HTTP, not HTTPS, for cloud upload.** The A7672S module's current firmware (`A011B01A7672M7_F`) fails TLS handshakes (`+HTTPACTION: 0,715,0`) against modern cloud edges (Railway, Cloudflare-class TLS). ThingSpeak's plain-HTTP endpoint is used as a reliable workaround. A firmware update or a TLS-terminating proxy would be required to restore HTTPS support.
- **Simulated sensor input.** Pressure/temperature values currently originate from a BLE beacon simulator (phone app or a Python-based Windows advertiser script), not a physical MEMS pressure sensor. The decode pipeline is designed to accept real sensor hardware with no changes once available.
- **Single active tire channel.** The dashboard and current wiring support one live sensor position; extending to four requires additional ThingSpeak fields (or channels) and corresponding dashboard panel wiring.

