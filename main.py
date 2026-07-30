import asyncio
import random
import uuid
from winrt.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementPublisher,
    BluetoothLEAdvertisement,
    BluetoothLEManufacturerData,
)
from winrt.windows.storage.streams import DataWriter

# ---- Configuration ----
BEACON_UUID = "13EA2EE5-F292-4B6A-B83C-14A3280B4EEC"
MAJOR = 1            # e.g. 1 = front-left tire
COMPANY_ID = 0x004C  # Apple's company ID - required for standard iBeacon format
TX_POWER = -59
UPDATE_INTERVAL_SEC = 45

# ── Pressure byte range ──────────────────────────────────────────────────────
# Formula:  psi = byte × 2.5 × 0.145038
# Inverse:  byte = psi ÷ (2.5 × 0.145038)  =  psi ÷ 0.36260
#
#   30 psi → byte = 30 ÷ 0.36260 ≈ 82.74  → use 83 (gives 30.1 psi)
#   40 psi → byte = 40 ÷ 0.36260 ≈ 110.32 → use 110 (gives 39.9 psi)
#
# Step of ±3 bytes ≈ ±1.09 psi per cycle — closest whole-byte step to 1 psi
# Formula: 1 psi ÷ 0.36260 = 2.758 bytes → round to 3
#
PRESSURE_BYTE_MIN = 83    # ≈ 30.1 psi
PRESSURE_BYTE_MAX = 110   # ≈ 39.9 psi
PRESSURE_STEP     = 3     # ≈ 1.09 psi per cycle

# ── Temperature byte range ───────────────────────────────────────────────────
# Formula:  °C = byte − 40
# Inverse:  byte = °C + 40
#
#   34°C → byte = 74
#   37°C → byte = 77
#
TEMP_BYTE_MIN = 74   # 34°C
TEMP_BYTE_MAX = 77   # 37°C
TEMP_STEP     = 1    # 1°C per cycle

# ── Starting values — mid-range on both axes ─────────────────────────────────
# Pressure mid: (83 + 110) ÷ 2 ≈ 96  → 96 × 0.36260 ≈ 34.8 psi
# Temp mid:     (74 + 77)  ÷ 2 ≈ 75  → 75 − 40 = 35°C
pressure_byte = 96
temp_byte     = 75


def build_ibeacon_payload(uuid_str, major, minor, tx_power):
    u = uuid.UUID(uuid_str)
    writer = DataWriter()
    writer.write_byte(0x02)   # iBeacon type
    writer.write_byte(0x15)   # payload length (21 bytes follow)
    for b in u.bytes:
        writer.write_byte(b)
    writer.write_byte((major >> 8) & 0xFF)
    writer.write_byte(major & 0xFF)
    writer.write_byte((minor >> 8) & 0xFF)
    writer.write_byte(minor & 0xFF)
    writer.write_byte(tx_power & 0xFF)
    return writer.detach_buffer()


def make_publisher(minor):
    advertisement = BluetoothLEAdvertisement()
    mfg_data = BluetoothLEManufacturerData()
    mfg_data.company_id = COMPANY_ID
    mfg_data.data = build_ibeacon_payload(BEACON_UUID, MAJOR, minor, TX_POWER)
    advertisement.manufacturer_data.append(mfg_data)
    return BluetoothLEAdvertisementPublisher(advertisement)


async def main():
    global pressure_byte, temp_byte
    publisher = None

    print("Starting iBeacon simulator")
    print(f"UUID               : {BEACON_UUID}")
    print(f"Major              : {MAJOR}")
    print(f"Pressure range     : 30 – 40 psi  "
          f"(bytes {PRESSURE_BYTE_MIN}–{PRESSURE_BYTE_MAX})")
    print(f"Pressure drift     : ±{PRESSURE_STEP} bytes "
          f"≈ ±{PRESSURE_STEP * 2.5 * 0.145038:.2f} psi per cycle")
    print(f"Temp range         : 34 – 37 °C   "
          f"(bytes {TEMP_BYTE_MIN}–{TEMP_BYTE_MAX})")
    print(f"Temp drift         : ±{TEMP_STEP} byte = ±{TEMP_STEP}°C per cycle")
    print(f"Broadcast interval : {UPDATE_INTERVAL_SEC}s")
    print("Press Ctrl+C to stop.\n")

    while True:
        # ── Pressure random walk ─────────────────────────────────────────────
        # ±3 bytes ≈ ±1.09 psi; clamped hard to 30–40 psi byte window
        pressure_byte = max(
            PRESSURE_BYTE_MIN,
            min(PRESSURE_BYTE_MAX,
                pressure_byte + random.randint(-PRESSURE_STEP, PRESSURE_STEP))
        )

        # ── Temperature random walk ──────────────────────────────────────────
        # ±1 byte = ±1°C; clamped hard to 34–37°C byte window
        temp_byte = max(
            TEMP_BYTE_MIN,
            min(TEMP_BYTE_MAX,
                temp_byte + random.randint(-TEMP_STEP, TEMP_STEP))
        )

        minor = (pressure_byte << 8) | temp_byte

        # Decode back — confirms exactly what the receiver will see
        pressure_psi = pressure_byte * 2.5 * 0.145038
        temp_c       = temp_byte - 40

        # Windows BLE stack requires full stop/start to update payload
        if publisher is not None:
            try:
                publisher.stop()
            except Exception:
                pass

        publisher = make_publisher(minor)
        publisher.start()

        print(
            f"Minor={minor:6d} (0x{minor:04X}) | "
            f"Pressure byte={pressure_byte:3d} → {pressure_psi:5.2f} psi | "
            f"Temp byte={temp_byte:3d} → {temp_c}°C"
        )

        await asyncio.sleep(UPDATE_INTERVAL_SEC)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")