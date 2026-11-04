# BLE Protocol Specifications & Reverse-Engineering Reference

This document details the low-level Bluetooth Low Energy (BLE) GATT topologies, byte-level framing, checksum algorithms, and connection state machines implemented in the **Timemore Black Mirror Dot BLE Bridge**.

---

## 1. Input Protocol: Timemore Black Mirror Dot (BLE Central Role)

The **Timemore Black Mirror Dot** uses an asymmetric dual-characteristic GATT architecture under service `0x181A` (Environmental Sensing):

| GATT Entity | UUID | Properties | Purpose |
| :--- | :---: | :---: | :--- |
| **Primary Service** | `0x181A` (`0000181a-0000-1000-8000-00805f9b34fb`) | Primary | Main scale telemetry and control service |
| **Telemetry Characteristic (RX)** | `0x2A6D` (`00002a6d-0000-1000-8000-00805f9b34fb`) | `NOTIFY`, `READ` | Streams real-time weight frames from scale to bridge |
| **CCCD Descriptor** | `0x2902` | `READ`, `WRITE` | Writing `0x01 0x00` enables notifications |
| **Command Characteristic (TX)** | `0x2A6E` (`00002a6e-0000-1000-8000-00805f9b34fb`) | `WRITE`, `WRITE_NR` | Receives Tare, Timer, and Keep-Alive commands |

### 1.1. Wake-on-Power & Anonymous Advertising Behavior
- **Pairing Mode:** When the physical pairing button on the scale is held, the scale advertises its ASCII Local Name (`"Timemore Scale"`, `"Black Mirror"`, or `"TIMEMORE"`).
- **Normal Power-On (After Reset/Sleep):** The scale advertises **anonymously (`Name = ""`)** using `ADV_IND` (`advType = 0`, connectable) with its static/random MAC address (`e.g., a4:6d:33:91:a3:41`).
- **Connection Requirement (`err=574` mitigation):** Calling `pScaleClient->connect()` blind without receiving a live advertising packet in the controller's scan cache results in NimBLE error `574` (`BLE_HS_ENOTCONN` / `0x3E` Connection Failed to be Established). Therefore, the bridge runs a non-blocking background scan (`25%` duty cycle) and initiates `connect()` immediately after capturing a fresh advertisement from the saved MAC address.

### 1.2. Weight Notification Frame (`0x2A6D`, 9 Bytes)
Weight is encoded in **32-bit Little-Endian** milligrams starting at byte offset `1`:

| Byte Offset | Hex Example | Field | Description |
| :---: | :---: | :--- | :--- |
| **`[0]`** | `0x00` | Stability / Flags | `0x00` = Stable reading; non-zero = Unstable / in-motion |
| **`[1..4]`** | `E4 2A 00 00` | Signed Weight (`int32_t` LE) | Weight in **milligrams (mg)**. Divide by `1000.0f` for grams (`0x00002AE4` = `10,980 mg` = `10.98 g` -> `11.0 g`) |
| **`[5..8]`** | `00 00 00 00` | Tare / Auxiliary Data | Auxiliary state bytes |

### 1.3. Outgoing Control Commands (`0x2A6E`)
- **Tare (`0x00`):** Single byte `{ 0x00 }` written to `0x2A6E` resets the scale display to zero.
- **Keep-Alive (`0x20 0x00`):** Sent every `25,000 ms` (`{ 0x20, 0x00 }`) using Write Without Response to prevent the scale's auto-sleep timer from powering off the unit during a session.
- **Timer Controls:**
  - Start Timer: `{ 0x08 }`
  - Stop / Pause Timer: `{ 0x09 }`
  - Reset Timer: `{ 0x0A }`

---

## 2. Output Protocol A: BooKoo Themis Ultra (`#define EMULATE_BOOKOO`)

Designed for native compatibility with **Wendougee Espresso Machines (Data-S / Lita via E-Bar & GeeFlow)**, **Decent Espresso (`de1app`)**, **Beanconqueror**, and **GaggiMate**.

### 2.1. Wendougee / GeeFlow Discovery Architecture
Reverse engineering of the official **Wendougee E-Bar** Android app and **[`drobekk/GeeFlow`](https://github.com/drobekk/GeeFlow)** revealed:
1. **The Android tablet does NOT connect directly to the BLE scale.** The tablet only connects to the espresso machine's internal BLE module (`Wendougee_AABBCCDDEEFF`, using `0x2B10` for Modbus RTU telemetry and `0x2C10` for proprietary `FF 55 FF FF ...` frames).
2. **Scale scanning is performed by the espresso machine's internal BLE controller:**
   - Enabling Smart Scale in the app sends `FF 55 FF FF 9A 00 01 [flags | 0x04] [cksum]` (`INTEGRATION_FLAGS_COMMAND = 0x9A`) to the machine.
   - Tapping *Scan* sends `FF 55 FF FF 82 00 01 01 D7` (`CMD_SCALE_SCAN = 0x82`) to the machine.
   - The machine's internal BLE Central scans for scales and reports matches back to the tablet via `0x81` (`SCALE_FOUND`: `FF 55 FF FF 81 [len_hi] [len_lo] [ASCII_NAME] [cksum]`).
   - Whitelisted scale prefixes in `WendougeeScaleParser.kt` (`GlobalBleState`):
     ```kotlin
     SCALE_NAME_PREFIXES = listOf("BOOKOO", "TFY_", "AiLink_", "ECLAIR", "wiseda")
     ```
   - Real hardware capture from `WendougeeSmartScaleTest.kt` (`ff55ffff860012424f4f4b4f4f5f53435f5520383936303538c1`) shows the machine expects the exact 18-character format **`"BOOKOO_SC_U XXXXXX"`** (`"BOOKOO_SC_U "` + 6 hex digits of the BLE MAC).
   - Once connected, the espresso machine parses the 20-byte `0x03 0x0B` frames on `0x0FFE -> 0xFF11` and populates **Modbus Holding Registers 1415 (`WEIGHT`, offset 22) and 1423 (`WEIGHT_RATE`, offset 38)**.

### 2.2. Advertising PDU Layout (`ADV_IND` = 27 Bytes)
| Offset | Length | AD Type | Hex Bytes | Description |
| :---: | :---: | :---: | :--- | :--- |
| `0..2` | **3 B** | `0x01` | `02 01 06` | **Flags:** `LE General Discoverable` + `BR/EDR Not Supported` |
| `3..6` | **4 B** | `0x03` | `03 03 FE 0F` | **Complete 16-bit Service UUID:** `0x0FFE` |
| `7..26` | **20 B** | `0x09` | `13 09 42 4F 4F 4B 4F 4F 5F 53 43 5F 55 20 XX XX XX XX XX XX` | **Complete Local Name:** `"BOOKOO_SC_U XXXXXX"` (18 ASCII chars) |

### 2.3. 20-Byte Weight & Flow Telemetry Frame (`0x0FFE -> 0xFF11`)
Transmitted at **10 Hz** (`100 ms` intervals) per [`BooKooCode/OpenSource/bookoo_ultra_scale/protocols.md`](https://github.com/BooKooCode/OpenSource/blob/main/bookoo_ultra_scale/protocols.md):

| Byte | Value / Encoding | Description |
| :---: | :---: | :--- |
| **`0`** | `0x03` | Header Byte 1 (Product Number) |
| **`1`** | `0x0B` | Header Byte 2 (Weight Telemetry Type) |
| **`2..4`** | `uint24_t` BE | Elapsed timer in milliseconds (`ms`) |
| **`5`** | `0x01` | Weight Unit (`0x01` = Grams) |
| **`6`** | `0x2B` / `0x2D` | Weight Sign (`0x2B` = `'+'`, `0x2D` = `'-'`) |
| **`7..9`** | `uint24_t` BE | Absolute weight in **hundredths of a gram** (`|weight_g| * 100`) |
| **`10`** | `0x2B` / `0x2D` | Flow Rate Sign (`0x2B` = `'+'`, `0x2D` = `'-'`) |
| **`11..12`** | `uint16_t` BE | EMA-smoothed flow rate in **hundredths of g/s** (`|flow_gps| * 100`) |
| **`13`** | `0..100` | Battery percentage (`%`) |
| **`14..15`** | `0x0064` (`uint16_t` BE) | Standby time (`100` minutes) |
| **`16`** | `0x01` | Buzzer gear (`1`) |
| **`17`** | `0x01` | Flow rate smoothing switch (`1` = Enabled) |
| **`18`** | `0x00` | Reserved |
| **`19`** | `XOR(pkt[0..18])` | 8-bit XOR Checksum over bytes `0..18` |

### 2.4. Incoming Commands (`0x0FFE -> 0xFF12`)
All BooKoo commands follow `[0x03, 0x0A, CmdID, Data1, Data2, ChecksumXOR]`:
- **Tare (`0x01`):** `03 0A 01 00 00 08`
- **Start Timer (`0x04`):** `03 0A 04 00 00 0D`
- **Stop Timer (`0x05`):** `03 0A 05 00 00 0C`
- **Reset Timer (`0x06`):** `03 0A 06 00 00 0F`
- **Tare + Start Timer (`0x07`):** `03 0A 07 00 00 0E`

---

## 3. Output Protocol B: Acaia Lunar (`#define EMULATE_BOOKOO` Commented Out)

Emulates both the modern **Acaia Lunar Pyxis (`0xFF08`)** and **Legacy Microchip ISSC (`49535343-fe7d-4ae5-8fa9-9fafd205e455`)** services simultaneously.

### 3.1. Frame Structure
Every Acaia packet begins with `0xEF 0xDD` and ends with a 2-byte dual-parity checksum (`cksum1` = sum of even payload indices, `cksum2` = sum of odd payload indices):
```text
[ 0xEF | 0xDD | MsgType | Length | Payload[0 ... Length-1] | cksum1 | cksum2 ]
```

### 3.2. Supported Message Types
- **`0x00` (Heartbeat):** Bidirectional keep-alive every 3 seconds; echoed with `{0xEF, 0xDD, 0x00, 0x02, 0x00, 0x02, 0x00}` + Status (`0x08`).
- **`0x04` (Tare):** Triggers hardware tare on the Timemore Dot + instant software tare offset.
- **`0x07` (Info Packet):** Firmware version `2.0.08` sent during handshake.
- **`0x08` (Status / Battery):** Reports battery level (`90%`), units (`Grams = 2`), auto-off, and beep state.
- **`0x0B` (Identify Handshake):** Responds immediately with `0x07` (Info) + `0x08` (Status).
- **`0x0C` (Notification Config / Event Stream):** Streams `ACAIA_EVENT_WEIGHT (0x05)` (`6 bytes`, `0.1g` resolution with exponent `0x01`), `ACAIA_EVENT_TIMER (0x07)`, and `ACAIA_EVENT_ACK (0x0B)`.
- **`0x0D` (Stopwatch Action):** Handles Start (`0x00`), Stop (`0x01`), and Reset (`0x02`).

