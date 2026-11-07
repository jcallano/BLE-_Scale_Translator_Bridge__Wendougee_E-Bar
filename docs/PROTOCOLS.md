# BLE Protocol Specifications & Reverse-Engineering Reference

This document details the low-level Bluetooth Low Energy (BLE) GATT topologies, byte-level framing, checksum algorithms, and connection state machines implemented in the **Timemore Black Mirror Dot BLE Bridge**.

---

## 1. Input Protocol: Timemore Black Mirror Dot (BLE Central Role)

The Dot protocol below follows the **native Timemore Dot driver in de1app** ([decentespresso/de1app PR #345](https://github.com/decentespresso/de1app/pull/345)), which was reverse-engineered from HCI snoop logs of the official Timemore app and tested on real hardware. The bridge sends those frames **byte-for-byte**.

| GATT Entity | UUID | Properties | Purpose |
| :--- | :---: | :---: | :--- |
| **Primary Service** | `0xFFF0` | Primary | Timemore Dot scale service |
| **Telemetry Characteristic (RX)** | `0xFFF1` | `NOTIFY` | Weight frames and command responses (scale → bridge) |
| **Command Characteristic (TX)** | `0xFFF2` | `WRITE`, `WRITE_NR` | Init, poll, tare and timer commands (bridge → scale) |

Discovery order in `discoverAndSubscribeTimemore()`: `0xFFF0` (Dot) first, then `0xFFE0` (UART variant: `0xFFE1`/`0xFFE2`), then `0x181D` (SIG Weight Scale: `0x2A9D`). Only `0xFFF0` uses the frames below; the other variants keep the legacy generic handling.

### 1.1. Wake-on-Power & Anonymous Advertising Behavior
- **Pairing Mode:** the scale answers `SCAN_REQ` with its name (`"TIMEMORE..."`) and accepts connections from any central.
- **Normal Power-On:** the scale (Phyplus `PHY6222` SoC, MAC prefix `a4:6d:33`) advertises **anonymously** (`ADV_IND`, connectable, ~1 s period) and only accepts centrals that completed SMP bonding once in pairing mode (White List). Otherwise `connect()` fails with NimBLE error `574` (`0x3E`).
- **Mitigation:** the bridge bonds (Just-Works, `secureConnection()` after GATT discovery), persists the MAC in NVS, and only calls `connect()` right after capturing a fresh connectable advertisement.

### 1.2. Weight Notification Frame (`0xFFF1`)

```text
[0]  [1]  [2]   [3]   [4] [5] [6] [7] [8] [9] ...
A5   5A   Type  Stat  ..  ..  ..  ..  WH  WL  ...
```

| Byte(s) | Field | Description |
| :---: | :--- | :--- |
| `[0..1]` | Header | Always `A5 5A` |
| `[2]` | Type | `0x01` = weight data; `0x02` = command response; `0x03` = ACK (no weight) |
| `[8..9]` | Weight | **Signed 16-bit big-endian**, in **0.1 g** units → `grams = int16(WH, WL) / 10.0` |

Frames shorter than 10 bytes or with a type other than `0x01` are ignored for weight. The scale streams at **10 Hz**. On every scale connection the bridge logs the first 3 raw frames with the decoded weight (`[TIMEMORE-RX] ...`) so the decoding can be checked against the display.

> **Note:** the response to the battery query (`A5 5A 02 05 ...`) does **not** carry a battery percentage (a full Dot reports `3`), so it is ignored.

### 1.3. Outgoing Commands (`0xFFF2`)

All frames are sent **verbatim** (they are not rebuilt with a CRC, since their length/trailer does not match a simple `len + CRC-16` layout).

| Action | Frame (hex) | When |
| :--- | :--- | :--- |
| **Init sequence** (official app) | `A55A021300000000`, `A55A020800000000`, `A55A020500000000`, `A55A020200000000`, `A55A020600000000`, `A55A020C00000000` | After connect: sent twice, 150 ms apart, 300 ms between passes |
| **Poll** (keep-alive) | `A55A020800000000`, then 100 ms later `A55A030800020100000025` | After init, then every **10 s** |
| **Tare** | Poll, then `A55A030D000200000071` | On tare request (after the instant software tare) |
| **Timer Start** | `A55A03020001010020` | Timer start from the app |
| **Timer Stop** | `A55A030200010200FFD0` | Timer stop from the app |
| **Timer Reset** | `A55A030200010300FF81` | Timer reset from the app |

### 1.4. Hybrid Tare
When a tare is requested, `bridge_tx` immediately sets `softwareTareOffset = rawWeight`, so the app sees `0.0 g` on the very next packet. The physical tare frame is then sent by `scale_link`. When the scale's own reading returns to ~0 g, the software offset is cleared automatically.

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
Transmitted once per scale sample (the Dot streams at 10 Hz), with a filler packet if the scale is silent for 250 ms, per [`BooKooCode/OpenSource/bookoo_ultra_scale/protocols.md`](https://github.com/BooKooCode/OpenSource/blob/main/bookoo_ultra_scale/protocols.md):

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

