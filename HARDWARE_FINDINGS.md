# HP OmniBook X Flip 14-fm0xxx (Board `8D9F` / Lunar Lake) — Hardware & Firmware Findings

This document records the reverse-engineered hardware, firmware, ACPI/EC, IIO sensor, and DRM/HDR specifications for the **HP OmniBook X Flip Laptop 14-fm0xxx** (`sys_vendor = HP`, `board_name = 8D9F`, Intel Core Ultra 200V Lunar Lake).

---

## 1. Embedded Controller (EC) Keyboard Backlight & Timeout Control

On the HP OmniBook X Flip 14-fm0xxx, the keyboard backlight is **not** exposed via standard Linux `/sys/class/leds/*::kbd_backlight` or `hp-wmi` LED classes. Decompiling the system's ACPI `DSDT` and `SSDT20` tables with `iasl` revealed the exact Embedded Controller (`OperationRegion (RAM, EmbeddedControl, 0x00, 0xFF)`) registers and Windows WMI methods.

### 1.1 ACPI DSDT & SSDT20 Definitions

In `DSDT` (`\_SB.PC00.LPCB.EC0.RAM`):
```asl
Offset (0x94),
FSW1,   8,     // Offset 0x94 (148)
FAN1,   8,     // Offset 0x95 (149)
HKBT,   3,     // Offset 0x96 (150), bits 0..2: HP Keyboard Backlight Timeout
Offset (0x97),
HKBL,   8,     // Offset 0x97 (151), bits 0..7: HP Keyboard Backlight Level
```

#### Why Long-Press `F4` Only Worked on Windows
When `F4` is long-pressed, the EC does not change `HKBT` on its own. Instead, it triggers ACPI EC Query `_Q9B` (`DSDT`), which emits WMI event `0x1D` (`BUF0 = 0x21AA | (level << 16) | (timeout << 24)`):
```asl
Method (_Q9B, 0, Serialized)
{
    BUF0 = 0x21AA
    Switch ((HKBL & 0x03))
    {
        Case (Zero) { Local0 = Zero }  // Off (0%)
        Case (0x02) { Local0 = 0x32 }  // Low (50%)
        Case (One)  { Local0 = 0x64 }  // High (100%)
    }
    Switch (HKBT)
    {
        Case (Zero) { Local1 = Zero }  // 0 * 10s = Always On
        Case (One)  { Local1 = 0x03 }  // 3 * 10s = 30 seconds
        Case (0x03) { Local1 = 0x12 }  // 18 * 10s = 180 seconds (3 minutes)
    }
    Local2 = ((Local0 << 0x10) | (Local1 << 0x18))
    BUF0 |= Local2
    ^^^^WMID.GWET (0x1D, BUF0)
}
```
On Windows, the HP background service catches WMI event `0x1D` and invokes WMI method `SBKT` (**S**et **B**acklight **K**eyboard **T**imeout, WMI command ID `0x34` in `SSDT20`), which writes `HKBT` at EC offset `0x96`:
```asl
Method (SBKT, 1, Serialized)
{
    ...
    Switch (HWBF)
    {
        Case (One)  { \_SB.PC00.LPCB.EC0.HKBT = Zero } // Always On
        Case (0x02) { \_SB.PC00.LPCB.EC0.HKBT = One  } // 30 Seconds
        Case (0x03) { \_SB.PC00.LPCB.EC0.HKBT = 0x03 } // 3 Minutes
    }
}
```

### 1.2 EC Register Map (`/sys/kernel/debug/ec/ec0/io`)

Requires the `ec_sys` kernel module loaded with write support:
```bash
sudo modprobe ec_sys write_support=1
```

| EC Offset (Hex) | EC Offset (Dec) | ACPI Symbol | Values | Meaning |
| :---: | :---: | :---: | :--- | :--- |
| **`0x96`** | **`150`** | `HKBT` | `0x00`<br>`0x01`<br>`0x03` | **Always On (Never Timeout)**<br>**30 Seconds Auto-Off Timeout**<br>**3 Minutes (180s) Auto-Off Timeout** |
| **`0x97`** | **`151`** | `HKBL` | `0x00`<br>`0x02`<br>`0x01` | **Backlight Status / Wake Register**:<br>• Readback: `0x00` = Off, `0x02` = Low (50%), `0x01` = High (100%).<br>• Write: Writing non-zero (`0x01` / `0x02`) when `0x97 == 0x00` wakes/turns on the keyboard backlight (`0x0C` $\rightarrow$ `0x11`). The 50% vs 100% PWM step itself is cycled by the physical `F4` key in the EC scan matrix. |
| **`0x0C`** | **`12`** | `WAKS` | `0x10` (bit 0 = `0`)<br>`0x11` (bit 0 = `1`) | Readback status bit: `0` when LED is off, `1` when LED is active |

### 1.3 Direct Shell Commands

```bash
# Set Keyboard Backlight Timeout to ALWAYS ON (Never turn off automatically):
printf '\x00' | sudo dd of=/sys/kernel/debug/ec/ec0/io bs=1 seek=150 count=1 conv=notrunc

# Set Keyboard Backlight Timeout to 30 Seconds:
printf '\x01' | sudo dd of=/sys/kernel/debug/ec/ec0/io bs=1 seek=150 count=1 conv=notrunc

# Set Keyboard Backlight Timeout to 3 Minutes:
printf '\x03' | sudo dd of=/sys/kernel/debug/ec/ec0/io bs=1 seek=150 count=1 conv=notrunc

# Set Keyboard Backlight Brightness (0x00 = Off, 0x01 = Low, 0x02 = High):
printf '\x02' | sudo dd of=/sys/kernel/debug/ec/ec0/io bs=1 seek=151 count=1 conv=notrunc
```

---

## 2. Intel Integrated Sensor Hub (ISH) Firmware & 360° Sensors

### 2.1 ISH Firmware Extraction & Naming Recipe
- **PCI Device**: `0000:00:12.0` (`[8086:a845]`, Subsystem `103c:8d9f`), driver `intel_ish_ipc`.
- **Why Custom Firmware Was Needed**: Lunar Lake (`lnlm`) ISH loads its sensor routing and calibration firmware from the host OS at boot. The generic `ish_lnlm.bin.zst` in `linux-firmware-intel` lacks HP board `8D9F`'s sensor tables.
- **DMI CRC32 Firmware Filename Calculation**:
  `intel_ish_ipc` hashes `/sys/class/dmi/id/sys_vendor` (`12128606`) and `/sys/class/dmi/id/product_name` (`e4cc03e5`):
  ```bash
  python3 -c "import zlib; print(f'ish_lnlm_{zlib.crc32(open(\"/sys/class/dmi/id/sys_vendor\", \"rb\").read().strip()) & 0xFFFFFFFF:08x}_{zlib.crc32(open(\"/sys/class/dmi/id/product_name\", \"rb\").read().strip()) & 0xFFFFFFFF:08x}.bin.zst')"
  # Output: ish_lnlm_12128606_e4cc03e5.bin.zst
  ```
- **Installation**:
  1. Extracted `ishS_SI_5.8.0.7718.bin` (645,632 bytes, FW base `5.8.0.7718`, project `1.0.0.12584`) from the Windows driver store.
  2. Compressed with `zstd` and installed to `/usr/lib/firmware/intel/ish/ish_lnlm_12128606_e4cc03e5.bin.zst` (SHA-256: `260c35318ff2f07b05c0dae511fb0aa99e07147d30e354d2bf4ea0cfa83233ac`).
  3. Backed up to `/home/mikolas/ish_lnlm_12128606_e4cc03e5.bin.zst`.

### 2.2 Enumerated IIO & Input Sensors

| Sensor / Device | Interface / Path | Channels & Behavior |
| :--- | :--- | :--- |
| **360° Hinge Sensor** | `/sys/bus/iio/devices/iio:device*` (`name = hinge`)<br>`HID-SENSOR-INT-020b` | `in_angl0_raw` (`hinge`): `0°–360°` opening angle<br>`in_angl1_raw` (`screen`): Screen pitch angle (`deg`)<br>`in_angl2_raw` (`keyboard`): Keyboard base angle (`deg`) |
| **3D Accelerometer** | `/sys/bus/iio/devices/iio:device*` (`name = accel_3d`)<br>`HID-SENSOR-200073` | `in_accel_x_raw`, `in_accel_y_raw`, `in_accel_z_raw` (`scale = 0.000009806` m/s²)<br>Exposed on D-Bus via `net.hadess.SensorProxy` (`AccelerometerOrientation`) |
| **3D Gyroscope** | `/sys/bus/iio/devices/iio:device*` (`name = gyro_3d`)<br>`HID-SENSOR-200076` | `in_anglvel_x_raw`, `in_anglvel_y_raw`, `in_anglvel_z_raw` (`scale = 0.000000174` rad/s) |
| **Human Presence** | `/sys/bus/iio/devices/iio:device*` (`name = prox`)<br>USB Camera `04F2:B7FE` | `in_attention_input`, `in_proximity0_raw`, `in_proximity1_raw` |
| **Tablet Switches** | `HP WMI hotkeys` (`input6`) & `Intel HID switches` (`INTC107B:00`) | Emit `EV_SW` `SW_TABLET_MODE` (`SW=2`) when folded |

### 2.3 Input Device Kernel Inhibition (`/sys/class/input/input*/inhibited`)

Writing `1` (inhibit) or `0` (enable) to `/sys/class/input/input*/inhibited` disables/enables input devices inside the Linux input core before `keyd` or `libinput` receives events:
- **Built-in Keyboard**: `AT Translated Set 2 keyboard` (`phys = isa0060/serio0/input0`)
- **Built-in Touchpad**: `SYNA3502:00 06CB:CFC5 Touchpad`
- **Touchscreen**: `ELAN2514:00 04F3:4428` (`PROP=2` / `INPUT_PROP_DIRECT`, distinct from `ELAN2514:00 04F3:4428 Stylus`)

---

## 3. Built-in OLED Display HDR10 (`eDP-1`)

- **Panel**: `Samsung Display Corp. 0x41AA` (Model `16810`, 2880×1800 @ 120Hz OLED, 12 bpc native / 10 bpc link).
- **EDID DisplayID 2.0 & CTA-861 HDR Capabilities**:
  - Peak Luminance (10% window): **`616.0 cd/m²`** (`Desired content max luminance: 116 = 616.884 cd/m²`)
  - Full-Frame Sustained Luminance: **`400.0 cd/m²`** (`Desired content max frame-average: 96 = 400.0 cd/m²`)
  - Minimum Luminance: **`0.0005 cd/m²`**
  - Colorimetry & EOTF: `BT2020RGB`, `DCI-P3`, `SMPTE ST 2084` (HDR10 PQ)
- **Linux Kernel (`xe` driver on `/dev/dri/card0`)**:
  - Exposes `HDR_OUTPUT_METADATA` (prop `8`), `Colorspace` (prop `403`, including `BT2020_RGB=9`), and `max bpc = 12` (prop `402`) on `eDP-1`.
- **Niri Compositor Status (`niri 26.04`)**:
  - In `/home/mikolas/src/niri/src/backend/tty.rs`, `ConnectorProperties::reset_hdr()` explicitly zeroes `HDR_OUTPUT_METADATA` and sets `Colorspace = 0` (`Default`) on startup and VT resume because Niri does not yet implement an sRGB-to-BT.2020/PQ output compositing shader or `wp-color-management-v1`.
