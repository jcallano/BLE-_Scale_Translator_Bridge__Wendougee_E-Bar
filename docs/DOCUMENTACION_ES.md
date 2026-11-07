# Documentación Técnica: Puente BLE Timemore Dot a Emulador Acaia Lunar (ESP32-S3-Zero)

## 1. Descripción General
Este firmware convierte una placa **Waveshare ESP32-S3-Zero** en un puente Bluetooth Low Energy (BLE) transparente (**Man-in-the-Middle / MitM**) que opera simultáneamente en modo **Cliente BLE** y **Servidor BLE** utilizando la pila `NimBLE-Arduino`:

1. **Entrada (Rol Cliente BLE):** Se conecta automáticamente a una balanza **Timemore Black Mirror Dot** (compatible también con Timemore Basic / Plus / Nano), recibe las notificaciones de peso en tiempo real, mantiene activa la báscula para evitar su apagado automático y permite enviarle comandos de **Tara** y **Cronómetro**.
2. **Salida (Rol Servidor BLE):** Emula una balanza **BooKoo Themis Ultra** (`BOOKOO_SC_U XXXXXX`, modo por defecto con `#define EMULATE_BOOKOO`) o una **Acaia Lunar** (`ACAIA_L_XXXX`, servicios `0xFF08` / `0x1820` / ISSC), para ser detectada nativamente por máquinas y aplicaciones de espresso como **Wendougee (E-Bar / GeeFlow), Decent Espresso (de1app / Decaid), Smart Espresso Profiler y Beanconqueror**.
3. **Baja latencia:** cada muestra de la balanza (10 Hz) se reenvía en cuanto llega, mediante una arquitectura que usa explícitamente los dos núcleos del ESP32-S3 (ver la [sección 7](#7-arquitectura-multinúcleo-y-baja-latencia)). Latencia interna medida: **~0.33 ms de media, < 0.7 ms en el peor caso**.

---

## 2. Códigos de Colores del LED RGB Integrado (WS2812 - `GPIO 21`)

El LED RGB WS2812 integrado en la placa **Waveshare ESP32-S3-Zero** (`GPIO 21`) indica en todo momento el estado operativo del puente BLE:

| Color / Patrón del LED | Valores RGB (Base) | Estado del Sistema | Descripción Detallada |
| :--- | :---: | :--- | :--- |
| 🔵 **Azul Parpadeante** *(cada 450 ms)* | `(0, 70, 255)` | **Buscando Balanza** | Ni la Timemore Dot ni la App están conectadas. El ESP32-S3 está escaneando activamente en busca de la Timemore Dot. |
| 🩵 **Cian Fijo** | `(0, 180, 255)` | **Conectado a Timemore Dot** | La balanza física Timemore Dot ya está enlazada y enviando peso. Esperando a que la App móvil o máquina de espresso se conecte al servidor `LUNAR-XXXX`. |
| 🟠 **Ámbar / Naranja Fijo** | `(255, 140, 0)` | **Conectado solo a la App** | La App de espresso está conectada al emulador Acaia Lunar, pero la balanza física Timemore Dot está apagada o fuera de rango (el ESP32 sigue buscándola en segundo plano). |
| 🟢 **Verde Fijo** | `(0, 255, 40)` | **Puente Activo (100% Operativo)** | Tanto la **Timemore Dot** como la **App móvil** están conectadas. El peso se traduce y transmite en tiempo real a 10 Hz. |
| ⚪ **Destello Blanco** *(180 ms)* | `(255, 255, 255)` | **Comando de TARA** | Destello breve que confirma que se acaba de ejecutar una orden de **Tara** (solicitada desde la App, botón `BOOT` o Monitor Serie). |
| 🩷 **Rosa / Magenta Fijo** | `(255, 0, 128)` | **Botón BOOT Presionado** | Indica que el usuario está manteniendo presionado el botón `BOOT` (`GPIO 0`) rumbo a los 3 segundos para resetear la memoria. |
| 🟣 **Magenta Parpadeante Rápido** *(6 destellos)* | `(255, 0, 180)` | **Reset de Emparejamiento (NVS)** | Confirma que la dirección MAC guardada en memoria Flash NVS fue borrada y el dispositivo vuelve al modo de búsqueda abierta por mejor señal (RSSI). |

> **Nota técnica sobre el driver LED:** El firmware implementa un sistema de caché RGB (`writeRgbPixelSafe()`) que solo invoca al periférico `RMT` (`rgbLedWrite`) cuando el color realmente cambia, evitando interferencias de hardware o sobrecarga de interrupciones con el radio BLE a 2.4 GHz.

---

## 3. Funciones del Botón Físico `BOOT` (`GPIO 0`)

El botón integrado `BOOT` (`GPIO 0`) cumple dos funciones sin necesidad de interfaces web ni menús externos:

1. **Pulsación Corta (`50 ms` a `1.2 s`):**
   - Ejecuta inmediatamente un comando de **TARA (Puesta a cero)** tanto hacia la balanza Timemore Dot como en el acumulador lógico de respaldo, produciendo un destello blanco en el LED RGB.
2. **Pulsación Larga (`>= 3 segundos`, al encender o en cualquier momento):**
   - Borra la dirección MAC almacenada en la partición NVS (`Preferences.h`).
   - Desconecta la balanza actual (si estaba conectada).
   - Parpadea 6 veces en color **Magenta**.
   - Reinicia el escáner BLE en modo **Auto-Discovery**, conectándose automáticamente a la balanza Timemore encendida con mejor señal (RSSI).

---

## 4. Lógica Interna y Arquitectura del Programa

### Diagrama de Flujo del Sistema

```mermaid
flowchart TD
    A["Encendido / Reset ESP32-S3"] --> B["Ventana USB Serial (5s) + Diagnóstico de Reset"]
    B --> C{"¿Botón BOOT (GPIO 0)\npresionado 3s?"}
    C -- Sí --> D["Borrar MAC en NVS\n(6 destellos Magenta)"]
    C -- No --> E["Leer MAC guardada en NVS (Preferences.h)"]
    D --> E
    E --> F["Inicializar NimBLE Dual\n(BOOKOO_SC_U XXXXXX o ACAIA_L_XXXX)"]
    F --> T1["Crear tarea bridge_tx (core 0)"]
    T1 --> G["Iniciar Servidor BooKoo / Acaia\n(Advertising siempre visible)"]
    G --> T2["Crear tarea scale_link (core 1)"]
    T2 --> H{"¿Existe MAC\nguardada en NVS?"}
    H -- Sí --> I["Conexión directa rápida\na la MAC guardada"]
    H -- No --> J["Auto-Discovery:\nFiltrar por UUID 0xFFF0/0xFFE0/0x181D o nombre\nTIMEMORE / DOT / MACARON / BLACK MIRROR"]
    I --> K["Conectar y suscribir Notificaciones\n(0xFFF1 / 0xFFE1 / 0x2A9D)"]
    J --> K
    K --> L["Guardar MAC en NVS +\nsecuencia de inicio de la Dot"]
    L --> M["Funcionamiento por tareas"]
    M --> N["bridge_tx (core 0): muestra nueva ->\ncaudal -> trama BooKoo/Acaia al instante"]
    M --> O["scale_link (core 1): sondeo cada 10 s,\ntara/timer físicos, reconexión"]
    M --> P["bridge_tx: Handshake Acaia/BooKoo\n(IDENTIFY, HEARTBEAT, TARE, TIMER)"]
    M --> Q["loop (core 1): LED, botón BOOT,\nSerial y estadísticas"]
```

---

### 4.1. Lógica del Rol Cliente BLE y Anatomía del Protocolo Timemore Dot

#### A. Descubrimiento, Emparejamiento y Reconexión Instantánea (`isTimemoreAdvertisedDevice`, `evaluateScannedDevice` y `connectToTimemoreScale`)

1. **Diagnóstico del Error `err=574 (0x23E / 0x3E = BLE_ERR_CONN_ESTABLISHMENT)` y White List del SoC Phyplus (`A4:6D:33`):**
   - El prefijo MAC `a4:6d:33` corresponde al SoC BLE **Phyplus Technology (`PHY6222` / `PHY6252`)** integrado en la Timemore Dot.
   - Su pila BLE (`GAPBondMgr` con `GAPBOND_AUTO_SYNC_WL`) opera en dos estados de anuncio (`Adv_Filter_Policy`):
     - **Modo Emparejamiento Manual (`Adv_Filter_Policy = 0x00`):** Responde a solicitudes `SCAN_REQ` (enviando su nombre `"TIMEMORE..."` en `SCAN_RSP`) y acepta `CONNECT_IND` de cualquier dispositivo central.
     - **Encendido Normal / Reconexión (`Adv_Filter_Policy = 0x03 - White List Only`):** Emite paquetes `ADV_IND` (`advType=0, conn=1`) cada `1000 ms`, pero su capa de enlace ignora tanto `SCAN_REQ` (por eso aparece `Nombre='(Sin Nombre)'`) como `CONNECT_IND` (devolviendo **`err=574 / 0x3E`** tras 6 eventos de conexión) de cualquier dispositivo que **no haya completado un enlace SMP Bonding (`Just-Works`)** durante el modo emparejamiento.
   - **Soluciones implementadas en el firmware:**
     1. **Establecimiento Limpio de Enlace (`exchangeMTU = false`, 1M PHY y Pausa de 60 ms):**
        - Al detectar la balanza en `onResult()` (con `setScanResponseTimeout(150)` para no cortar el intercambio `SCAN_REQ -> SCAN_RSP` en el aire), el ESP32-S3 detiene el escáner y el advertising local, espera **60 ms** para estabilizar el estado del radio, fuerza **1M PHY** (`BLE_GAP_LE_PHY_1M_MASK`), usa parámetros estándar (`24..40`, `30–50 ms`) e invoca `pScaleClient->connect(targetAddr, true, false, false)` (`exchangeMTU = false`) para no inyectar paquetes `ATT_EXCHANGE_MTU_REQ` durante los primeros 6 eventos críticos de establecimiento del enlace.
     2. **Bonding BLE Asíncrono Post-GATT (`NimBLEDevice::setSecurityAuth(true, false, false)`):**
        - Configura `Just-Works Legacy Bonding` con distribución de claves `ENC | ID` (`BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID`).
        - **Después** de completar `discoverAndSubscribeTimemore()` (para no colisionar con el semáforo interno `m_pTaskData` de `NimBLEClient`), ejecuta `pScaleClient->secureConnection(true)`. De este modo, al emparejar la Timemore Dot **una vez** en modo emparejamiento con este firmware, el SoC Phyplus registra la MAC del ESP32-S3 en su **White List** interna.
     3. **Trazas `[SCAN-RAW]` en Monitor Serie:** Imprime en hexadecimal la carga útil cruda de cada paquete de anuncio de la balanza y el estado del enlace SMP (`encrypted`, `bonded`).

---

#### B. Servicios y Características GATT de la Timemore Dot

Dependiendo de la revisión de hardware/firmware de la familia **Timemore Black Mirror (Dot / Basic Pro / Nano / Macaron)**, la balanza expone uno de los siguientes tres perfiles GATT (todos detectados y suscritos automáticamente por `discoverAndSubscribeTimemore()`):

| Perfil / Variante | UUID de Servicio | Característica **NOTIFY** (Balanza → ESP32-S3) | Característica **WRITE** (ESP32-S3 → Balanza) |
| :--- | :--- | :--- | :--- |
| **1. Timemore Open Scale (Principal en Dot / Basic Pro)** | `0xFFF0` | **`0xFFF1`** (`Notify` / `Read`) | **`0xFFF2`** (`Write` / `Write No Response`) |
| **2. Timemore UART Transparente (Telink / Tuya BLE)** | `0xFFE0` | **`0xFFE1`** (`Notify` / `Read`) | **`0xFFE2`** o **`0xFFE1`** (`Write`) |
| **3. Bluetooth SIG Weight Scale (Estándar)** | `0x181D` | **`0x2A9D`** (`Notify` / `Indicate`) | Escaneo automático en el servicio |

---

#### C. Protocolo de la Timemore Dot (servicio `0xFFF0`)

El protocolo implementado sigue el **driver nativo de la Timemore Dot en de1app** ([decentespresso/de1app PR #345](https://github.com/decentespresso/de1app/pull/345)). Ese driver se obtuvo de capturas HCI de la app oficial de Timemore y está probado con hardware real. El puente envía esas tramas **byte a byte, sin recalcularlas**: su longitud y su cola no encajan con un formato simple `longitud + CRC-16`, así que no se reconstruyen con `calculateTimemoreCRC16()`.

> [!NOTE]
> Versiones anteriores de esta documentación describían un formato con CRC-16/IBM y comandos `0x09`/`0x0A`/`0x0B` para el timer. Ese formato no coincide con el de la Dot real, así que se reemplazó.

---

#### D. Detalle Byte por Byte de las Tramas de la Timemore Dot

1. **Notificación de peso (`0xFFF1`):**
   ```text
   [0]  [1]  [2]   [3]   [4] [5] [6] [7] [8] [9] ...
   A5   5A   Tipo  Est.  ..  ..  ..  ..  WH  WL  ...
   ```
   - `Bytes 0..1`: cabecera fija `A5 5A`.
   - `Byte 2 (Tipo)`: `0x01` = peso; `0x02` = respuesta a comando; `0x03` = ACK (sin peso).
   - `Bytes 8..9 (WH, WL)`: entero de 16 bits **con signo**, **big-endian**, en décimas de gramo: $\text{Peso (g)} = \frac{\text{int16\_be}(W_H, W_L)}{10.0}$.
   - Las tramas de menos de 10 bytes o de tipo distinto de `0x01` se ignoran para el peso. La balanza envía a **10 Hz**.
   - **Verificación:** en cada conexión, el Monitor Serie muestra las 3 primeras tramas crudas con su peso interpretado (`[TIMEMORE-RX] Trama de peso 1/3: A5 5A 01 ... -> 12.3 g`), para compararlas con la pantalla de la Dot.
   - La respuesta a la consulta de batería (`A5 5A 02 05 ...`) **no** es un porcentaje (con batería llena reporta `3`), por lo que se ignora.

2. **Variante UART (`0xFFE0`: `0xFFE1` / `0xFFE2`, otros modelos):** se mantiene el manejo genérico anterior (tramas con cabecera `0xAA`/`0x55`, tara `AA 02 43 00 45`). Solo se usa si la balanza no expone `0xFFF0`.

3. **Comandos enviados por el ESP32-S3 a la Timemore Dot (`0xFFF2`):**

   | Acción | Trama hexadecimal (exacta, de de1app) | Cuándo se envía |
   | :--- | :--- | :--- |
   | **Secuencia de inicio** (app oficial) | `A55A021300000000`, `A55A020800000000`, `A55A020500000000`, `A55A020200000000`, `A55A020600000000`, `A55A020C00000000` | Al conectar: dos pasadas, 150 ms entre comandos y 300 ms entre pasadas (~2.5 s) |
   | **Sondeo / keep-alive** | `A55A020800000000` y, 100 ms después, `A55A030800020100000025` | Tras la secuencia de inicio y luego **cada 10 s** |
   | **Tara** | Sondeo y luego `A55A030D000200000071` | Al pedir tara (después de la tara por software) |
   | **Timer Start** | `A55A03020001010020` | Orden de iniciar el timer desde la App |
   | **Timer Stop** | `A55A030200010200FFD0` | Orden de detener el timer |
   | **Timer Reset** | `A55A030200010300FF81` | Orden de reiniciar el timer |

   > [!TIP]
   > **Tara híbrida instantánea:** al pedir una tara, `bridge_tx` (core 0) aplica en el acto `softwareTareOffset = rawWeightGrams`, de modo que la App recibe `0.0 g` en el siguiente paquete. La trama física de tara la envía después `scale_link` (core 1). Cuando el peso crudo de la balanza vuelve a ~0 g, el offset por software se borra solo.

---

### 4.2. Lógica del Rol Servidor BLE y Protocolo Acaia Lunar (2021 / Pyxis)

#### A. Publicidad BLE (`Advertising`) y Servicios GATT
1. **Paquete de Advertising Primario y Scan Response (`setupAcaiaServer`):**
   - **Nombre BLE:** `LUNAR-XXXX` (donde `XXXX` son los últimos 2 bytes en hexadecimal de la MAC de fábrica del ESP32-S3, por ejemplo `LUNAR-5710`).
   - **Paquete Primario (19 bytes `<= 31 bytes`):** Incluye Flags BLE, el UUID público de 16 bits **`0xFF08`** (`AD Type 0x03`) requerido por filtros de escaneo de Wendougee/Profitec y el nombre completo `LUNAR-XXXX`.
   - **Scan Response (18 bytes `<= 31 bytes`):** Incluye el UUID de 128 bits **`49535343-fe7d-4ae5-8fa9-9fafd205e455`** (Microchip/ISSC Transparent UART) propio de la **Acaia Lunar 2021**.
   - **Sin condición de bloqueo:** El servidor Acaia nunca detiene su anuncio ni desconecta a la App aunque la Timemore Dot esté apagada o se reinicie.

2. **Perfiles GATT Expuestos Simultáneamente:**
   - **Servicio Acaia Lunar 2021 / Pyxis (`49535343-fe7d-4ae5-8fa9-9fafd205e455` - ISSC Transparent UART):**
     - `49535343-8841-43f4-a8d4-ecbe34729bb3` (`Write` / `Write No Response`): Canal **RX** donde la App escribe los comandos (`IDENTIFY`, `NOTIFICATION_REQUEST`, `HEARTBEAT`, `TARE`, `TIMER`).
     - `49535343-1e4d-4bd9-ba61-23c647249616` (`Notify` / `Read`): Canal **TX** donde la balanza notifica peso a 10 Hz, estado de batería y ACKs.
   - **Servicio Primario `0xFF08` (Especificación / Wendougee):**
     - `0xFF10` (`Write` / `Write No Response`): Canal RX de comandos.
     - `0xFF11` (`Notify` / `Read`): Canal TX de notificaciones de peso.
   - *(Nota de diseño crítica: No se expone el servicio pre-2021 `0x1820 / 0x2A80` para evitar que librerías como `AcaiaArduinoBLE` entren en el modo legacy `OLD` de 10 bytes e ignoren las tramas modernas de 13 bytes de la Lunar 2021).*

---

#### B. Anatomía del Protocolo Acaia (`0xEF, 0xDD`), Regla de Longitud `N + 1` y Checksum Dual

Toda trama del protocolo Acaia Lunar —tanto las enviadas por la App como las respondidas por la balanza— comparte el siguiente formato binario:

```text
+--------+--------+---------+--------------------+--------+--------+
| Byte 0 | Byte 1 | Byte 2  | Bytes 3 .. (3+M-1) |  End-1 |   End  |
+--------+--------+---------+--------------------+--------+--------+
|  0xEF  |  0xDD  | msgType |   Payload (M B)    | cksum1 | cksum2 |
+--------+--------+---------+--------------------+--------+--------+
```

1. **Cabecera Mágica (`Bytes 0 y 1`):** Siempre fija en `0xEF, 0xDD`.
2. **Tipo de Mensaje (`Byte 2 - msgType`):**
   - `0x00` (`ACAIA_MSG_SYSTEM`): Latido de conexión (`Heartbeat`).
   - `0x04` (`ACAIA_MSG_TARE`): Orden de puesta a cero (`Tare`).
   - `0x07` (`ACAIA_MSG_INFO`): Información de versión de firmware (`v2.0.08` / `"LUNA"`).
   - `0x08` (`ACAIA_MSG_STATUS`): Estado de la balanza (batería, unidades en gramos, sonido, auto-off).
   - `0x0B` (`ACAIA_MSG_IDENTIFY`): Handshake de autenticación inicial de la App.
   - `0x0C` (`ACAIA_MSG_EVENT`): Solicitud de notificaciones (App → Balanza) o flujo de eventos de Peso/Timer/ACK (Balanza → App).
   - `0x0D` (`ACAIA_MSG_TIMER`): Control del cronómetro (`0 = Start`, `1 = Stop`, `2 = Reset`).
3. **Regla Crítica del Byte de Longitud (`Byte 3 = N_datos + 1`):**
   - En los mensajes estructurados con longitud (`0x08`, `0x07`, `0x0C`), el primer byte del payload (`Byte 3` del paquete) indica la longitud del bloque **contándose a sí mismo** (`Longitud = N_datos + 1 = sizeof(payload)`).
   - *Ejemplo real de la App (`NOTIFICATION_REQUEST`, 14 bytes):*
     `EF DD 0C [09] 00 01 01 02 02 05 03 04 [15 06]`
     Contiene **8 bytes de configuración** (`00 01 01 02 02 05 03 04`), por lo que el `Byte 3` vale **`0x09` (`8 + 1 = 9`)**.
   - *Ejemplo de nuestro paquete de Peso (`EVENT_WEIGHT`, 13 bytes):*
     `EF DD 0C [08] 05 W0 W1 W2 W3 Unit Flags [cksum1 cksum2]`
     Contiene **7 bytes de evento** (`05` + 4 bytes de peso + `Unit` + `Flags`), por lo que el `Byte 3` debe valer exactamente **`0x08` (`7 + 1 = 8`)**. Si se envía `0x07`, el decodificador de la App desplaza la lectura del checksum 1 byte antes y descarta el paquete.
4. **Cálculo del Doble Checksum Par / Impar (`cksum1` y `cksum2`):**
   Se calcula iterando únicamente sobre los `M` bytes del `Payload` (desde `Byte 3` hasta antes del checksum):
   - **`cksum1` (Suma de índices pares del payload: `0, 2, 4, ...`):**
     $$\text{cksum1} = \left( \sum_{k=0, 2, 4\dots} \text{Payload}[k] \right) \ \& \ \text{0xFF}$$
   - **`cksum2` (Suma de índices impares del payload: `1, 3, 5, ...`):**
     $$\text{cksum2} = \left( \sum_{k=1, 3, 5\dots} \text{Payload}[k] \right) \ \& \ \text{0xFF}$$

---

#### C. Diagrama de Secuencia del Handshake, Streaming a 10 Hz y Heartbeat

```mermaid
sequenceDiagram
    participant App as App / Máquina Espresso
    participant S3 as ESP32-S3 (LUNAR-5710)
    participant Dot as Timemore Dot (a4:6d:33:91:a3:41)

    Note over S3,Dot: Conexión Cliente BLE (0xFFE0/0xFFF0) + Keep-Alive cada 25s
    Dot-->>S3: Notificaciones de Peso en tiempo real

    App->>S3: 1. Conexión BLE + Suscripción CCCD en 49535343-1e4d-...
    Note over S3: Pausa escáner 10s para priorizar Handshake
    App->>S3: 2. Write IDENTIFY (20B) en 49535343-8841-...\n[EF DD 0B 30 31 32 33 34 35 36 37 38 39 30 31 32 33 34 9A 6D]
    S3-->>App: Notify STATUS (0x08, len=0x07) + Notify WEIGHT (0x0C, len=0x08)
    App->>S3: 3. Write NOTIFICATION_REQUEST (14B) (+300 ms)\n[EF DD 0C 09 00 01 01 02 02 05 03 04 15 06]
    S3-->>App: Notify STATUS (0x08, len=0x07) + Streaming continuo WEIGHT a 10 Hz (13B)

    loop Cada ~3.1 segundos (Mantenimiento de Sesión Acaia)
        App->>S3: Write HEARTBEAT (7B): [EF DD 00 02 00 02 00]
        S3-->>App: Notify HEARTBEAT ACK (0x00): [EF DD 00 02 00 02 00]
        S3-->>App: Notify STATUS (0x08, len=0x07, Batería=90%, Unidad=g)
        S3-->>App: Notify WEIGHT (0x0C, len=0x08, Evento=0x05)
    end
```

---

#### D. Detalle Byte por Byte de las Tramas Principales

1. **Handshake `IDENTIFY` recibido de la App (20 bytes):**
   ```text
   EF DD 0B 30 31 32 33 34 35 36 37 38 39 30 31 32 33 34 9A 6D
   ```
   - `0x0B`: Comando Identify.
   - `30..34`: 15 bytes ASCII (`"012345678901234"`).
   - `9A 6D`: Checksum par (`0x9A`) e impar (`0x6D`).

2. **Configuración `NOTIFICATION_REQUEST` recibida de la App (14 bytes):**
   ```text
   EF DD 0C 09 00 01 01 02 02 05 03 04 15 06
   ```
   - `0x0C`: Comando Event Notification Request.
   - `0x09`: Longitud (`8 bytes de pares [evento, intervalo] + 1 = 9`).
   - `00 01`: Peso (`0x00`) cada `1` unidad de tiempo (100 ms).
   - `01 02`: Batería (`0x01`).
   - `02 05`: Timer (`0x02`).
   - `03 04`: Eventos de botón/tara (`0x03`).
   - `15 06`: Checksum par (`0x09+0x01+0x02+0x05+0x04 = 0x15`) e impar (`0x00+0x01+0x02+0x03 = 0x06`).

3. **Trama de Peso `EVENT_WEIGHT` transmitida por el ESP32-S3 a 10 Hz (13 bytes):**
   ```text
   [0]  [1]  [2]  [3]  [4]  [5]  [6]  [7]  [8]  [9]   [10]    [11]     [12]
   0xEF 0xDD 0x0C 0x08 0x05  W0   W1   W2   W3  Unit  Flags  cksum1   cksum2
   ```
   - `Byte 2 (0x0C)`: `ACAIA_MSG_EVENT`.
   - `Byte 3 (0x08)`: Longitud del bloque (`7 bytes de datos + 1 = 8`).
   - `Byte 4 (0x05)`: `ACAIA_EVENT_WEIGHT`.
   - `Bytes 5..8 (W0..W3)`: Entero de 32 bits sin signo en **Little-Endian** con el peso escalado (`round(|peso| * 10)`).
     - La librería del cliente decodifica: `peso = (((Byte[6] << 8) + Byte[5]) / 10^Byte[9]) * ((Byte[10] & 0x02) ? -1 : 1)`.
   - `Byte 9 (Unit)`: `0x01` ($10^1 \rightarrow$ resolución `0.1 g`) o `0x02` ($10^2 \rightarrow$ resolución `0.01 g`).
   - `Byte 10 (Flags)`: `Bit 0 (0x01)` = inestable; `Bit 1 (0x02)` = signo negativo (`-`).
   - *Ejemplo para `0.0 g` estable:* `EF DD 0C 08 05 00 00 00 00 01 00 09 05`.

4. **Latido `HEARTBEAT` cada ~3.1 segundos (7 bytes):**
   - **App → ESP32-S3:** `EF DD 00 02 00 02 00`
   - **ESP32-S3 → App:** Responde inmediatamente con el ACK `EF DD 00 02 00 02 00` seguido del paquete `STATUS (0x08, len=0x07)` (`EF DD 08 07 5A 02 00 00 00 01 62 02` para Batería=90% `0x5A`, unidad=gramos `0x02`).

---

#### E. Ejemplo Real de Logs de Operación Exitosa (Monitor Serie)

A continuación se muestra una captura real del Monitor Serie (`115200 baudios`) con el puente operando en estado **Verde Fijo** (tanto la **Timemore Dot `a4:6d:33:91:a3:41`** como la **App en `LUNAR-5710`** conectadas simultáneamente e intercambiando latidos y paquetes de peso):

```text
[  138410 ms] [ACAIA-RX CMD] (7 bytes): EF DD 00 02 00 02 00 
[  138410 ms] [ACAIA-RX] Heartbeat (0x00) recibido de la App.
[  138418 ms] [ACAIA-TX] Heartbeat ACK (0x00) enviado.
[  138418 ms] [ACAIA-TX] Paquete STATUS (0x08, len=0x07) enviado (Bateria=90%).
[  141589 ms] [ACAIA-SRV] >>> APP ESCRIBIO (onWrite) EN 49535343-8841-43f4-a8d4-ecbe34729bb3 (7 bytes) <<<
[  141589 ms] [ACAIA-RX CMD] (7 bytes): EF DD 00 02 00 02 00 
[  141589 ms] [ACAIA-RX] Heartbeat (0x00) recibido de la App.
[  141599 ms] [ACAIA-TX] Heartbeat ACK (0x00) enviado.
[  141599 ms] [ACAIA-TX] Paquete STATUS (0x08, len=0x07) enviado (Bateria=90%).
[  141810 ms] [STATUS] Dot=CONECTADA (a4:6d:33:91:a3:41) | AppAcaia=CONECTADA (LUNAR-5710) | Peso=  +0.0g | Bat=90% | Rx=1321 Tx=1723 | Heap=242536 B
[  144709 ms] [ACAIA-SRV] >>> APP ESCRIBIO (onWrite) EN 49535343-8841-43f4-a8d4-ecbe34729bb3 (7 bytes) <<<
[  144709 ms] [ACAIA-RX CMD] (7 bytes): EF DD 00 02 00 02 00 
[  144709 ms] [ACAIA-RX] Heartbeat (0x00) recibido de la App.
[  144711 ms] [ACAIA-TX] Heartbeat ACK (0x00) enviado.
[  144711 ms] [ACAIA-TX] Paquete STATUS (0x08, len=0x07) enviado (Bateria=90%).
[  146812 ms] [STATUS] Dot=CONECTADA (a4:6d:33:91:a3:41) | AppAcaia=CONECTADA (LUNAR-5710) | Peso=  +0.0g | Bat=90% | Rx=1370 Tx=1792 | Heap=242536 B
```

5. **Despacho Asíncrono Libre de Bloqueos (Anti-Crash / Anti-Bootloop):**
   - Ningún callback de NimBLE (`onWrite`, `onSubscribe`, `onResult`, `onTimemoreNotifyCallback`) invoca funciones bloqueantes del stack (`notify()`, `writeValue()` ni `scan->stop()`).
   - Los callbacks únicamente activan banderas `volatile` (`pendingAcaiaWeightImmediate`, `pendingAcaiaStatusPacket`, `pendingStopScan`, etc.) y el `loop()` principal ejecuta las operaciones GATT de forma ordenada en el contexto de la tarea de aplicación.

---

### 4.9 Estructura del Paquete de Publicidad (`ADV_IND` <= 31 Bytes) y Modo Alternativo (`#define EMULATE_BOOKOO`) para Wendougee E-Bar / GeeFlow

Algunas aplicaciones de máquinas de espresso en tablets Android (como **Wendougee E-Bar / GeeFlow**) aplican filtros de hardware estrictos (`ScanFilter`) **únicamente sobre el paquete primario de publicidad (`Legacy ADV_IND`, máximo 31 bytes)** e inspeccionan el servicio estándar **Device Information (`0x180A`)** tras conectar.

Para garantizar compatibilidad simultánea con **Wendougee E-Bar / GeeFlow** y **de1app (Decent)**:

1. **Desglose Byte a Byte de la PDU Primaria (`ADV_IND` = 29 bytes `<= 31B`) en Modo Acaia (`ACAIA_L_XXXX`):**

| Offset | Longitud | AD Type | Bytes en Hexadecimal | Descripción |
| :---: | :---: | :---: | :--- | :--- |
| `0..2` | **3 B** | `0x01` | `02 01 06` | **Flags BLE:** `LE General Discoverable` + `BR/EDR Not Supported` |
| `3..8` | **6 B** | `0x03` | `05 03 20 18 08 FF` | **Complete 16-bit Service UUIDs:** `0x1820` (IPSP) + `0xFF08` (Acaia) |
| `9..14` | **6 B** | `0x16` | `05 16 20 18 43 33` | **Service Data 16-bit:** UUID `0x1820` + Firma Acaia `0x4333` |
| `15..28` | **14 B** | `0x09` | `0D 09 41 43 41 49 41 5F 4C 5F XX XX XX XX` | **Complete Local Name:** `"ACAIA_L_XXXX"` (12 caracteres ASCII) |
| **Total** | **29 B** | — | **29 / 31 bytes usados** | **Cabe íntegramente en la PDU Primaria Legacy sin truncamiento** |

2. **Scan Response PDU (`SCAN_RSP` = 24 bytes `<= 31B`):**
   - Contiene el **UUID de 128 bits de Microchip ISSC** (`49535343-fe7d-4ae5-8fa9-9fafd205e455`, **18 bytes**) + **Service Data `0x4333`** (**6 bytes**), manteniendo 100% compatibilidad con **de1app** y **Beanconqueror**.
3. **Intervalo de Advertising Rápido (`20 ms` a `40 ms`):**
   - Configurado con `setMinInterval(32)` (`20 ms`) y `setMaxInterval(64)` (`40 ms`) para asegurar detección inmediata en ventanas de escaneo cortas de Android.
4. **Servicio de Información del Dispositivo (`0x180A`):**
   - Expone las características de solo lectura **Model Number (`0x2A24` = `"LUNAR"`)**, **Manufacturer Name (`0x2A29` = `"Acaia"`)** y **Firmware Revision (`0x2A26` = `"2.0.08"`)**.
5. **Modo BooKoo Themis Ultra (`#define EMULATE_BOOKOO`) e Ingeniería Inversa de Wendougee / GeeFlow:**
   - **Arquitectura Real de Wendougee (Data-S / Lita + E-Bar / GeeFlow):**
     A partir de la ingeniería inversa del protocolo Wendougee documentada en el proyecto [`drobekk/GeeFlow`](https://github.com/drobekk/GeeFlow) (`WendougeeScaleParser.kt`, `WendougeeAccessoryControl.kt`, `WendougeeBleDeviceDiscoverer.kt`, `WendougeeSmartScaleTest.kt`) y de las especificaciones oficiales [`BooKooCode/OpenSource`](https://github.com/BooKooCode/OpenSource), se determinó cómo funciona el descubrimiento de básculas en estas máquinas:
     1. **La tablet Android NO se conecta directamente a la báscula por Bluetooth:** La tablet (con **E-Bar** o **GeeFlow**) solo mantiene una conexión BLE directa con la placa interna de la cafetera (`Wendougee_AABBCCDDEEFF`, características `0x2B10` para Modbus RTU y `0x2C10` para tramas propietarias `FF 55 FF FF ...`).
     2. **El escáner principal de "Agregar Dispositivo" de la app ignora todas las básculas:** En `WendougeeBleDeviceDiscoverer.kt`, el filtro principal de dispositivos BLE solo acepta cafeteras cuyo nombre contenga `"wendougee"`, `"wdg"` o `"lita"` seguido de 12 dígitos hexadecimales (`([a-fA-F0-9]{12})$`).
     3. **Dónde y quién escanea la báscula:** El descubrimiento ocurre **dentro de la configuración de la máquina ya conectada** (*Configuración -> Conectividad -> Smart Scale*):
        - Al activar *Smart Scale*, la tablet envía a la cafetera el comando `0x9A` (`INTEGRATION_FLAGS_COMMAND`: `FF 55 FF FF 9A 00 01 [flags | 0x04] [cksum]`).
        - Al pulsar *Scan / Rescan*, la tablet envía a la cafetera el comando `0x82` (`CMD_SCALE_SCAN`: `FF 55 FF FF 82 00 01 01 D7`).
        - **El módulo BLE interno de la cafetera Wendougee** actúa como Central BLE, escanea el entorno y devuelve a la tablet tramas `0x81` (`SCALE_FOUND`: `FF 55 FF FF 81 [len_hi] [len_lo] [ASCII_NAME] [cksum]`).
        - La app y el firmware de la máquina filtran los nombres usando la lista estricta extraída de `GlobalBleState`:
          ```kotlin
          SCALE_NAME_PREFIXES = listOf("BOOKOO", "TFY_", "AiLink_", "ECLAIR", "wiseda")
          ```
          *(Nota: **Acaia no está soportada nativamente** por el módulo BLE interno de la cafetera Wendougee).*
        - Cuando el usuario selecciona la báscula en la pantalla, la tablet envía `0x80` (`SCALE_CMD_CONNECT`) con el nombre ASCII a la cafetera. **La cafetera se conecta directamente al puente**, se suscribe a `0x0FFE` -> `0xFF11`, calcula el peso y caudal, y los inyecta en sus **Registros Holding Modbus 1415 (`WEIGHT`, offset 22) y 1423 (`WEIGHT_RATE`, offset 38)** hacia la tablet.
   - **Formato Exacto de Nombre y Publicidad en `#define EMULATE_BOOKOO`:**
     - Según las capturas reales de la máquina en `WendougeeSmartScaleTest.kt` (`ff55ffff860012424f4f4b4f4f5f53435f5520383936303538c1`), la máquina espera el formato de 18 caracteres **`"BOOKOO_SC_U XXXXXX"`** (`"BOOKOO_SC_U "` + 6 dígitos hexadecimales de la MAC BLE, ej. `"BOOKOO_SC_U 895710"`). Este nombre cumple simultáneamente con **Wendougee E-Bar**, **GeeFlow** (`startsWith("BOOKOO")`), **de1app** (`startsWith("BOOKOO_SC")`) y **Beanconqueror**.
     - La PDU Primaria (`ADV_IND`, **27 bytes `<= 31B`**) contiene únicamente:
       - `Flags (0x01)`: `02 01 06` (3 B)
       - `Complete 16-bit Service UUID (0x03)`: `03 03 FE 0F` (`0x0FFE`, 4 B)
       - `Complete Local Name (0x09)`: `"BOOKOO_SC_U XXXXXX"` completo sin truncar (20 B)
     - La respuesta de escaneo (`SCAN_RSP`, **24 bytes `<= 31B`**) repite el nombre completo (20 B) y el UUID `0x0FFE` (4 B).
     - Además, cuando la Timemore Dot está apagada o desconectada, el puente realiza escaneos pasivos/ligeros (25% de ciclo útil de radio) **sin detener jamás la publicidad BLE** (`stopAdvertising()` solo se invoca cuando el escáner confirma que la Timemore Dot acaba de transmitir un paquete en vivo), garantizando que la cafetera Wendougee siempre encuentre al puente al enviar `CMD_SCALE_SCAN (0x82)`.
   - **Trama de 20 Bytes del Protocolo BooKoo Themis Ultra (`0x0FFE` -> `0xFF11`):**
     - `Byte 0..1`: `0x03 0x0B` (Product Number + Type)
     - `Byte 2..4`: Tiempo en milisegundos (`24-bit Big-Endian`)
     - `Byte 5`: Unidad de peso (`0x01` = Gramos, según `BooKooCode/OpenSource/bookoo_ultra_scale/protocols.md`)
     - `Byte 6`: Signo del peso (`0x2B` = `'+'`, `0x2D` = `'-'`)
     - `Byte 7..9`: Magnitud absoluta del peso en centésimas de gramo (`|peso_g| * 100`, `24-bit Big-Endian`)
     - `Byte 10`: Signo del caudal (`0x2B` = `'+'`, `0x2D` = `'-'`)
     - `Byte 11..12`: Magnitud del caudal suavizado EMA en centésimas de g/s (`|flow_gps| * 100`, `16-bit Big-Endian`, alimentando el registro Modbus `1423 WEIGHT_RATE` de la Wendougee)
     - `Byte 13`: Batería (`0..100 %`)
     - `Byte 14..15`: Tiempo de standby (`0x0064` = 100 min, `16-bit Big-Endian`)
     - `Byte 16`: Buzzer gear (`0x01`)
     - `Byte 17`: Flow rate smoothing switch (`0x01` = habilitado)
     - `Byte 18`: Reservado (`0x00`)
     - `Byte 19`: Checksum XOR de los bytes `0..18`

---

## 5. Mensajes de Debug y Comandos por Monitor Serie (`115200 baudios`)

Conectando la placa por USB-C y abriendo el **Monitor Serie** a `115200 baudios`:

1. **Secuencia de Arranque:**
   - Al encender, el firmware imprime una breve cuenta regresiva (~1.2 s) con la causa exacta del último reinicio (`POWERON`, `SW`, `PANIC`, `WDT`, `BROWNOUT`, `USB`) antes de iniciar el radio BLE, y al final la línea `[BOOT] Tareas: bridge_tx (core 0, prio 20) | scale_link (core 1, prio 3) | loop (core 1)`.
2. **Reporte Automático (cada 5 segundos):**
   ```text
   [  130028 ms] [STATUS] Dot=CONECTADA (a4:6d:33:91:a3:41) | AppAcaia=CONECTADA (BOOKOO_SC_U CE5710) | Peso=  -1.1g | Bat=90% | Rx=1039 Tx=675 | Heap=226964 B
   [  130029 ms] [LINK] Dot: itvl=20.00ms lat=0 phy=2/2M | App: itvl=60.00ms lat=0 phy=2/2M mtu=256
   [  130029 ms] [LAT] n=49 (9.8 Hz) pisadas=0 | dt balanza ms min/avg/max=100/102/200 | despertar us=34/42/100 | e2e us=291/337/626
   [  130029 ms] [LAT] e2e hist <0.25ms:0 <1ms:49 <5ms:0 <15ms:0 >=15ms:0 | txPeso=50 txFallos=0
   ```
   - **`[LINK]`**: intervalo de conexión, latencia de esclavo, PHY y MTU que **realmente** aceptó cada extremo.
   - **`[LAT]`**: frecuencia real de la balanza (`dt balanza`), muestras pisadas antes de procesarse (`pisadas`), tiempo en despertar `bridge_tx` y **e2e** = llegada de la muestra al ESP32 → notificación entregada al stack BLE (no incluye el tiempo en el aire), con histograma, paquetes de peso enviados y rechazados.
   - **`[TIMEMORE-RX]`**: en cada conexión con la balanza, las 3 primeras tramas de peso crudas y el peso interpretado.
   - **`[ACAIA-SRV] Intervalo App ...`**: resultado de la verificación del intervalo con la App (`OK` o `Re-solicitando ...`).
3. **Comandos Interactivos desde la PC:**
   Escribe cualquiera de estas letras en el Monitor Serie y presiona `Enter`:
   - `?` o `i` : Muestra inmediatamente el resumen de estado.
   - `l` : Muestra en el acto los parámetros de enlace y las estadísticas de latencia.
   - `t` : Ejecuta una **TARA** (por software al instante + física en la balanza).
   - `r` : Borra la dirección MAC guardada en **NVS** y reinicia la búsqueda de balanzas cercanas.
   - `s` : Fuerza un nuevo ciclo de escaneo BLE.
   - `p` : Pausa / reanuda el escaneo de la balanza (deja el radio libre para la App).

---

## 6. Configuración Exacta en Arduino IDE 2.x (`Herramientas` / `Tools`)

Para compilar y cargar el firmware en la **Waveshare ESP32-S3-Zero** sin errores de `sdkconfig.h` ni bucles de reinicio por memoria Octal:

| Parámetro en `Herramientas` | Valor Obligatorio |
| :--- | :--- |
| **Board (Placa)** | **`Waveshare ESP32-S3-Zero`** *(o **`ESP32S3 Dev Module`** normal — **NUNCA** seleccionar la variante `Octal`)* |
| **USB CDC On Boot** | **`Enabled`** *(obligatorio: el sketch usa `Serial.setTxTimeoutMs()`, que solo existe en ese modo)* |
| **USB Mode** | **`Hardware CDC and JTAG`** |
| **Flash Mode** | **`QIO 80MHz`** *(o `DIO 80MHz`)* |
| **Flash Size** | **`4MB (32Mb)`** |
| **Partition Scheme** | **`Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)`** |
| **PSRAM** | **`Disabled`** *(o `QSPI PSRAM`)* |
| **CPU Frequency** | **`240MHz (WiFi)`** |
| **Upload Speed** | **`921600`** |

Probado con **esp32 de Espressif v3.3.12** y **NimBLE-Arduino v2.5.2**. Las funciones de baja latencia (notify dirigido, PHY 2M, verificación de enlace y estadísticas) requieren NimBLE-Arduino **v2.x**.

---

## 7. Arquitectura Multinúcleo y Baja Latencia

### 7.1. Reparto de tareas entre los dos núcleos

Antes, toda la lógica corría en `loop()` (core 1, prioridad 1) con un `delay(3)` fijo, y los dos núcleos solo se usaban por la configuración por defecto de Arduino y NimBLE. Ahora el reparto es explícito:

| Núcleo | Tarea | Prioridad | Responsabilidad |
| :---: | :--- | :---: | :--- |
| 0 | Controlador BT + `nimble_host` | 23 / 21 | Pila BLE (todos los callbacks de NimBLE corren aquí). |
| 0 | **`bridge_tx`** | 20 | **Único emisor hacia la App/máquina.** Se despierta por bits de notificación FreeRTOS en cuanto termina el callback de la balanza, en el mismo núcleo que `nimble_host` (sin salto entre núcleos). Envía peso, respuestas del handshake y timer. |
| 1 | **`scale_link`** | 3 | Todo lo que puede bloquear: escaneo, `connect()` síncrono (hasta 4 s + reintento), descubrimiento GATT, secuencia de inicio de la Dot, sondeo, tara y timer físicos. Mientras espera, `bridge_tx` sigue atendiendo a la App: el handshake ya no se congela durante una reconexión de la balanza. |
| 1 | `loop()` | 1 | LED, botón `BOOT`, comandos Serial y estadísticas. |

### 7.2. Pipeline dirigido por eventos

- Los callbacks no envían nada: publican eventos con **bits de notificación FreeRTOS** (`postBridgeEvent()` / `postScaleEvent()`), que son atómicos y no pierden eventos (las antiguas banderas `pending*` sí podían perderlos entre núcleos).
- **Envío inmediato:** cada muestra nueva se reenvía en cuanto llega. El reenvío cada 100 ms queda solo como **relleno** si la balanza no manda nada. Entre dos pesos hay al menos `WEIGHT_MIN_GAP_MS` (15 ms) para no saturar la cola ATT, y entre dos paquetes cualesquiera al menos `HANDSHAKE_PACING_MS` (2 ms).
- **Prioridad de envío:** peso nuevo > Heartbeat ACK > INFO > STATUS > ACK de evento > timer inmediato > relleno > timer a 10 Hz.
- **Caudal (g/s):** se calcula una vez por muestra real, con su marca de tiempo de llegada (dt exacto, sin jitter del planificador), con EMA `FLOW_EMA_ALPHA = 0.35`.
- **Timer Acaia:** tiene su propio reloj a 10 Hz (antes iba atado al relleno de peso y se congelaba con el envío inmediato).
- **Notify dirigido:** las notificaciones van al *handle* de la conexión de la App. Sin handle, NimBLE v2 construye un `std::vector` (reserva en heap) en cada paquete.

### 7.3. Ajustes de radio

| Enlace | Rol del ESP32 | Ajuste | Momento |
| :--- | :--- | :--- | :--- |
| Balanza | Central (lo impone) | Intervalo **15 ms** fijo (antes 30–50 ms) + **PHY 2M** | 1.5 s después de conectar (tras el cifrado SMP) |
| App / máquina | Periférico (solo lo pide) | Intervalo **15–30 ms** + **PHY 2M** | Al conectar (intervalo) y a los 3 s (PHY) |

- La petición a la App cumple las reglas de iOS/Android (mínimo ≥ 15 ms y máximo ≥ mínimo + 15 ms). Una petición de 10–20 ms se rechazaba entera y el central se quedaba en 60 ms.
- `scale_link` verifica el intervalo que realmente aplicó la App y, si sigue por encima de 30 ms, lo vuelve a pedir cada 5 s, hasta 3 veces.
- PHY 2M reduce a la mitad el tiempo en el aire por paquete. Si un extremo no lo soporta, el enlace sigue en 1M.

### 7.4. Presupuesto de latencia medido

Medido con una tablet Android con de1app como central (antes del ajuste de intervalo compatible con iOS/Android):

| Tramo | Media | Peor caso |
| :--- | :---: | :---: |
| Esperar el evento de conexión con la balanza (15–20 ms) | ~8–10 ms | 15–20 ms |
| Procesamiento dentro del ESP32 | **0.33 ms** | **< 0.7 ms** |
| Esperar el evento de conexión con la App (60 ms observados) | ~30 ms | 60 ms |

La balanza muestrea a **10 Hz**: el peso llega, de media, con 50 ms de antigüedad por la propia frecuencia de muestreo. El intervalo de la App es el tramo controlable de mayor peso.

### 7.5. Interruptores de ajuste (al inicio del `.ino`)

| Constante | Valor | Para qué sirve |
| :--- | :---: | :--- |
| `WEIGHT_MIN_GAP_MS` | 15 | Separación mínima entre pesos. Subir a 25–30 si la App se atasca. |
| `SCALE_CONN_ITVL_MIN/MAX` | 12 / 12 | Intervalo con la balanza (×1.25 ms). Volver a 24/40 si se desconecta. |
| `APP_CONN_ITVL_MIN/MAX` | 12 / 24 | Intervalo pedido a la App (×1.25 ms). |
| `ENABLE_2M_PHY_SCALE` / `_APP` | 1 / 1 | Poner a 0 si algún enlace se vuelve inestable. |
| `ENABLE_LATENCY_STATS` | 1 | Instrumentación `[LAT]`; 0 la desactiva por completo. |

---

## 8. Compatibilidad con las Apps de Decent Espresso

- **de1app:** tiene **soporte nativo para la Timemore Dot** desde julio de 2026 ([de1app PR #345](https://github.com/decentespresso/de1app/pull/345)). Con de1app actualizado se puede conectar la Dot directamente, sin este puente. Los comandos de la Dot de este firmware son copias exactas de esa implementación.
- **Decaid (la app nueva de Decent):** no tiene driver de Timemore. Decent rechazó el soporte nativo y pidió a la comunidad un **plugin externo** de báscula ([decaid #737](https://github.com/decentespresso/decaid/issues/737)). Decaid reconoce cualquier dispositivo cuyo nombre contenga `bookoo`, así que el puente en modo BooKoo debería funcionar con ella (pendiente de probar).

