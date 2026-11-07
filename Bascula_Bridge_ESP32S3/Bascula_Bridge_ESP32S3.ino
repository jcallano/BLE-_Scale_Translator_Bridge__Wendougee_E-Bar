/**
 * ============================================================================
 * PROYECTO: Puente BLE Transparente (MitM) Timemore Dot -> Emulador Acaia Lunar
 * HARDWARE: Waveshare ESP32-S3-Zero (ESP32-S3)
 * ENTORNO:  Arduino IDE 2.x | esp32 by Espressif Systems v3.x+ (ESP-IDF v5.x)
 * LIBRERÍA: NimBLE-Arduino (Compatible automáticamente con v1.4.x y v2.x)
 * ============================================================================
 *
 * CORRECCIONES DE ESTABILIDAD (ANTI-BOOTLOOP ESP32-S3):
 * 1. Sin constructores globales de NimBLEUUID / NimBLEAddress / String antes de
 *    main()/setup() para evitar fallos de inicialización temprana en ESP-IDF v5.x.
 * 2. Uso estricto de UUIDs de 16 bits (uint16_t 0xFF08, 0xFFE0, etc.) para que
 *    el paquete de Advertising ocupe 21 bytes (<= 31 bytes límite BLE) y coloque
 *    0xFF08 en la lista pública de 16 bits (AD Type 0x03) sin desbordar buffer.
 * 3. Callbacks BLE 100% libres de bloqueo y sin re-entrada (ningún notify(),
 *    writeValue() ni scan->stop() se invoca dentro del contexto de nimble_host);
 *    todo se despacha con bits de notificación FreeRTOS (sin pérdida de eventos)
 *    a dos tareas: bridge_tx (core 0, emisor único hacia la App) y scale_link
 *    (core 1, enlace bloqueante con la Timemore). Ver "ARQUITECTURA MULTINÚCLEO".
 * 4. Controlador WS2812 (GPIO 21) con caché de estado RGB: solo invoca al
 *    periférico RMT (rgbLedWrite) cuando el color realmente cambia, evitando
 *    colisiones de RMT/DMA con el stack BLE en Core 3.x.
 * 5. Telemetría y mensajes de Debug detallados por Puerto Serial (USB CDC) con
 *    diagnóstico de causa de reinicio, trazas de paquetes BLE y comandos
 *    interactivos por monitor serie ('?', 'l', 't', 'r', 's', 'p').
 * ============================================================================
 */

#include <Arduino.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <esp_timer.h>
#if __has_include(<esp_mac.h>)
  #include <esp_mac.h>
#endif
#if __has_include(<esp_system.h>)
  #include <esp_system.h>
#endif

// Detección automática de versión de NimBLE-Arduino (v2.x incluye NimBLEConnInfo.h)
#if __has_include(<NimBLEConnInfo.h>)
  #define NIMBLE_V2 1
#endif

// ============================================================================
// CONFIGURACIÓN DE HARDWARE Y DEBUG SERIAL
// ============================================================================
#define ENABLE_SERIAL_DEBUG           1     // 1 = Mensajes de debug detallados por USB Serial
#define DEBUG_VERBOSE_BLE_PACKETS     1     // 1 = Mostrar tramas HEX recibidas/enviadas
#define ENABLE_RGB_LED                1     // 1 = Habilitar LED WS2812 integrado

#define PIN_BOOT_BUTTON               0     // Botón BOOT integrado en ESP32-S3-Zero
#define PIN_RGB_LED_S3_ZERO           21    // WS2812 RGB en Waveshare ESP32-S3-Zero
#define LED_BRIGHTNESS_MAX            28    // Brillo moderado (0-255)

// Tiempos del sistema
#define NVS_RESET_HOLD_MS             3000  // 3 segundos en GPIO 0 para borrar MAC
#define SCAN_WINDOW_SECONDS           3     // Ventana de escaneo BLE (3s)
#define RECONNECT_INTERVAL_FAST_MS    800   // Pausa corta (0.8s) entre escaneos cuando la App aún no está conectada
#define RECONNECT_INTERVAL_APP_MS     2000  // Pausa moderada (2s) entre escaneos cuando la App ya está conectada
#define APP_HANDSHAKE_GRACE_MS        10000 // Pausa de escaneo (10s) cuando la App conecta para no interferir el Handshake
#define ACAIA_NOTIFY_INTERVAL_MS      100   // Reenvío de "relleno" si la balanza no manda nada nuevo en 100 ms
#define TIMEMORE_KEEPALIVE_INTERVAL_MS 10000 // Sondeo cada 10 s, como de1app (mantiene la Dot activa)

// ----------------------------------------------------------------------------
// AJUSTES DE BAJA LATENCIA (PIPELINE DIRIGIDO POR EVENTOS)
// ----------------------------------------------------------------------------
// Cada muestra nueva de la Timemore se reenvía en cuanto llega (la tarea loop se
// despierta por notificación FreeRTOS, sin esperar al siguiente tick de 100 ms).
// WEIGHT_MIN_GAP_MS limita la frecuencia máxima de salida para no saturar la cola
// ATT del host (15 ms ≈ 1 intervalo de conexión con la App -> máx ~66 Hz).
#define WEIGHT_MIN_GAP_MS             15
// Intervalos de conexión BLE (unidades de 1.25 ms). Se piden DESPUÉS de conectar.
// Balanza (somos Central -> lo imponemos): 15 ms fijos (antes 30-50 ms).
#define SCALE_CONN_ITVL_MIN           12
#define SCALE_CONN_ITVL_MAX           12
// App/Máquina (somos Periférico -> solo lo solicitamos): 15-30 ms.
// Cumple las reglas de Apple/Android (mín >= 15 ms, máx >= mín + 15 ms); una
// petición fuera de esas reglas (p.ej. 10-20 ms) se rechaza entera y el central
// se queda con su valor por defecto (se observó 60 ms).
#define APP_CONN_ITVL_MIN             12
#define APP_CONN_ITVL_MAX             24
// Si el central no aplicó el intervalo, se vuelve a pedir tras el handshake
// (algunos centrales rechazan cambios durante el descubrimiento GATT).
#define APP_ITVL_RETRY_MAX            3
#define APP_ITVL_RETRY_PERIOD_MS      5000
// Suavizado EMA del caudal (0..1). Más alto = menos retardo, más ruido.
// El EMA se calcula ahora por cada muestra real de la balanza, con su timestamp.
#define FLOW_EMA_ALPHA                0.35f
#define FLOW_MIN_DT_MS                40    // Ignora muestras más juntas que esto para el cálculo de caudal
#define SERIAL_STATUS_INTERVAL_MS     5000  // Resumen de estado por Serial cada 5s

// ----------------------------------------------------------------------------
// ARQUITECTURA MULTINÚCLEO (FreeRTOS SMP del ESP32-S3)
// ----------------------------------------------------------------------------
// Core 0: Controlador BT + nimble_host (prio 21) + bridge_tx (prio 20)
//         -> bridge_tx es la ÚNICA tarea que transmite a la App. Corre en el mismo
//            núcleo que nimble_host: la muestra de la balanza se reenvía en cuanto
//            el callback termina, sin salto entre núcleos.
// Core 1: scale_link (prio 3): escaneo, connect() bloqueante, descubrimiento GATT,
//            keep-alive y escrituras hacia la Timemore. Puede bloquear segundos
//            sin afectar al handshake ni al streaming hacia la App.
//         loop() (prio 1): LED, botón, comandos Serial y estadísticas.
#define BRIDGE_TASK_CORE              0
#define BRIDGE_TASK_PRIO              (configMAX_PRIORITIES - 5)
#define BRIDGE_TASK_STACK             6144
#define SCALE_TASK_CORE               1
#define SCALE_TASK_PRIO               3
#define SCALE_TASK_STACK              8192
#define HANDSHAKE_PACING_MS           2     // Separación entre paquetes de handshake consecutivos

// Ajuste de enlace diferido (evita colisiones de procedimientos LL con el cifrado
// SMP o con el descubrimiento GATT de la App justo después de conectar).
#define SCALE_LINK_TUNE_DELAY_MS      1500  // Tras conectar la Timemore: intervalo corto + PHY
#define APP_LINK_TUNE_DELAY_MS        3000  // Tras conectar la App: PHY
// PHY 2M: mitad de tiempo en el aire por paquete. Si un periférico no lo soporta,
// el enlace sigue en 1M sin problema. Poner a 0 si se observa inestabilidad.
#define ENABLE_2M_PHY_SCALE           1
#define ENABLE_2M_PHY_APP             1

// Instrumentación de latencia: solo marcas de tiempo en µs y contadores en RAM
// (decenas de ciclos por muestra). Se imprime cada 5 s desde loop() en el core 1.
#define ENABLE_LATENCY_STATS          1

// Resolución reportada en protocolo Acaia: 1 = 0.1g (x10), 2 = 0.01g (x100)
#define ACAIA_WEIGHT_UNIT_MODE        1

// ============================================================================
// MODO DE EMULACIÓN PARA APPS ESTRICTAS (WENDOUGEE E-BAR / GEEFLOW / DE1APP)
// ============================================================================
// Descomentar la siguiente línea para cambiar la identidad del puente a una
// báscula "BooKoo Themis" (Nombre: "Themis_XXXX", Servicio: 0x0FFE / 0xFF08)
// en lugar de "ACAIA_L_XXXX" si la app filtra específicamente por BooKoo:
#define EMULATE_BOOKOO


// Macros de Debug por Puerto Serial con marca de tiempo (100% NO BLOQUEANTES en USB CDC)
#if ENABLE_SERIAL_DEBUG
  #define DBG_PRINTF(fmt, ...)  do { if (Serial && Serial.availableForWrite() > 64) Serial.printf("[%8lu ms] " fmt "\r\n", millis(), ##__VA_ARGS__); } while (0)
  #define DBG_PRINTLN(msg)      do { if (Serial && Serial.availableForWrite() > 64) Serial.printf("[%8lu ms] %s\r\n", millis(), msg); } while (0)
#else
  #define DBG_PRINTF(fmt, ...)
  #define DBG_PRINTLN(msg)
#endif

// ============================================================================
// UUIDs DE 16 BITS (SIN CONSTRUCTORES GLOBALES PARA EVITAR CRASH PRE-MAIN)
// ============================================================================
// Cliente: Timemore Dot (0xFFE0 / 0xFFE1) y variantes Open Scale (0xFFF0) / Basic (0x181D)
static constexpr uint16_t UUID16_TIMEMORE_SVC_FFE0  = 0xFFE0;
static constexpr uint16_t UUID16_TIMEMORE_CHR_FFE1  = 0xFFE1;
static constexpr uint16_t UUID16_TIMEMORE_CHR_FFE2  = 0xFFE2;

static constexpr uint16_t UUID16_TIMEMORE_SVC_FFF0  = 0xFFF0;
static constexpr uint16_t UUID16_TIMEMORE_CHR_FFF1  = 0xFFF1;
static constexpr uint16_t UUID16_TIMEMORE_CHR_FFF2  = 0xFFF2;

static constexpr uint16_t UUID16_TIMEMORE_SVC_181D  = 0x181D;
static constexpr uint16_t UUID16_TIMEMORE_CHR_2A9D  = 0x2A9D;

// Servidor: Emulador Acaia Lunar (0xFF08 / 0xFF10 / 0xFF11) + Legacy IPSP (0x1820 / 0x2A80)
static constexpr uint16_t UUID16_ACAIA_SVC_FF08     = 0xFF08;
static constexpr uint16_t UUID16_ACAIA_CHR_FF10     = 0xFF10;
static constexpr uint16_t UUID16_ACAIA_CHR_FF11     = 0xFF11;

static constexpr uint16_t UUID16_ACAIA_SVC_1820     = 0x1820;
static constexpr uint16_t UUID16_ACAIA_CHR_2A80     = 0x2A80;
static constexpr uint16_t UUID16_ACAIA_SVC_DATA_4333 = 0x4333;

// Servidor: Device Information Service (0x180A) requerido por apps estrictas (Wendougee E-Bar / GeeFlow)
static constexpr uint16_t UUID16_DEVINFO_SVC_180A   = 0x180A;
static constexpr uint16_t UUID16_DEVINFO_MODEL_2A24 = 0x2A24;
static constexpr uint16_t UUID16_DEVINFO_FW_2A26    = 0x2A26;
static constexpr uint16_t UUID16_DEVINFO_MANUF_2A29 = 0x2A29;

// Servidor: Modo alternativo BooKoo Themis (0x0FFE -> Notify 0xFF11, Write 0xFF12)
static constexpr uint16_t UUID16_BOOKOO_SVC_0FFE    = 0x0FFE;
static constexpr uint16_t UUID16_BOOKOO_CHR_FF11    = 0xFF11;
static constexpr uint16_t UUID16_BOOKOO_CHR_FF12    = 0xFF12;

// Servidor: Acaia Lunar 2021 / Pyxis (Microchip ISSC Transparent UART 128-bit UUIDs)
// Requerido cuando el dispositivo se anuncia como "ACAIA_L_XXXX" o "LUNAR-XXXX"
static const char* UUID128_ACAIA_ISSC_SVC           = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
static const char* UUID128_ACAIA_ISSC_CHR_NOTIFY    = "49535343-1e4d-4bd9-ba61-23c647249616";
static const char* UUID128_ACAIA_ISSC_CHR_WRITE     = "49535343-8841-43f4-a8d4-ecbe34729bb3";

// ============================================================================
// CONSTANTES DE PROTOCOLO ACAIA (0xEF, 0xDD)
// ============================================================================
static constexpr uint8_t ACAIA_HEADER_1             = 0xEF;
static constexpr uint8_t ACAIA_HEADER_2             = 0xDD;

static constexpr uint8_t ACAIA_MSG_SYSTEM           = 0x00; // Heartbeat / System
static constexpr uint8_t ACAIA_MSG_TARE             = 0x04; // Comando de Tara
static constexpr uint8_t ACAIA_MSG_INFO             = 0x07; // Respuesta de versión/info
static constexpr uint8_t ACAIA_MSG_STATUS           = 0x08; // Estado de batería y unidades
static constexpr uint8_t ACAIA_MSG_IDENTIFY         = 0x0B; // Handshake Identify de la App
static constexpr uint8_t ACAIA_MSG_EVENT            = 0x0C; // Configuración y envío de Eventos
static constexpr uint8_t ACAIA_MSG_TIMER            = 0x0D; // Control de Timer (Start/Stop/Reset)

static constexpr uint8_t ACAIA_EVENT_WEIGHT         = 0x05; // Evento de peso
static constexpr uint8_t ACAIA_EVENT_BATTERY        = 0x06; // Evento de batería
static constexpr uint8_t ACAIA_EVENT_TIMER          = 0x07; // Evento de cronómetro
static constexpr uint8_t ACAIA_EVENT_ACK            = 0x0B; // Evento de ACK

// ============================================================================
// VARIABLES GLOBALES POD (PLAIN OLD DATA - SEGURAS EN ARRANQUE)
// ============================================================================
Preferences preferences;
static const char* NVS_NAMESPACE = "bascula_cfg";
static const char* NVS_KEY_MAC   = "dot_mac";
static const char* NVS_KEY_TYPE  = "dot_type";

char acaiaDeviceName[24]         = "ACAIA_L_0000";
char savedTimemoreMac[20]        = {0};
uint8_t savedTimemoreAddrType    = 0;
bool hasSavedMac                 = false;

// Candidato encontrado en escaneo
volatile bool candidateFound     = false;
char candidateMac[20]            = {0};
uint8_t candidateAddrType        = 0;
int candidateRSSI                = -999;
char candidateName[36]           = {0};

// Estado de conexión Cliente (Timemore Dot)
volatile bool scaleConnected     = false;
volatile bool doConnectToScale   = false;
volatile bool pendingStopScan    = false;
volatile bool isScanning         = false;
volatile bool scanPausedByUser   = false;
unsigned long lastScanOrConnAttemptMs = 0;

NimBLEClient* pScaleClient                         = nullptr;
NimBLERemoteCharacteristic* pScaleNotifyChar       = nullptr;
NimBLERemoteCharacteristic* pScaleWriteChar        = nullptr;

// Estado de conexión Servidor (Emulador Acaia Lunar / BooKoo Themis)
NimBLEServer* pAcaiaServer                         = nullptr;
NimBLECharacteristic* pAcaiaWriteCharFF10          = nullptr;
NimBLECharacteristic* pAcaiaNotifyCharFF11         = nullptr;
NimBLECharacteristic* pAcaiaIsscNotifyChar1E4D     = nullptr;
NimBLECharacteristic* pAcaiaIsscWriteChar8841      = nullptr;
NimBLECharacteristic* pBookooNotifyCharFF11        = nullptr;
NimBLECharacteristic* pBookooWriteCharFF12         = nullptr;
// Puntero al canal de notificación activo suscrito por la App (evita duplicar paquetes en NimBLE v2)
NimBLECharacteristic* volatile pActiveAcaiaNotifyChar = nullptr;

volatile bool appConnected               = false;
volatile bool appIdentified              = false;
volatile bool appNotificationsEnabled    = false;
volatile bool pendingRestartAdvertising  = false;
volatile unsigned long appHandshakeGraceUntilMs = 0;

volatile uint16_t appConnHandle          = 0xFFFF; // BLE_HS_CONN_HANDLE_NONE
volatile unsigned long appConnectedAtMs  = 0;
volatile bool appLinkTuned               = true;

// ----------------------------------------------------------------------------
// EVENTOS ENTRE TAREAS (bits de notificación FreeRTOS: atómicos, sin pérdidas)
// ----------------------------------------------------------------------------
// Hacia bridge_tx (core 0):
static constexpr uint32_t EVT_SAMPLE      = 1u << 0; // Muestra nueva de la Timemore
static constexpr uint32_t EVT_HB_ACK      = 1u << 1; // Responder Heartbeat Acaia
static constexpr uint32_t EVT_INFO        = 1u << 2; // Enviar paquete INFO
static constexpr uint32_t EVT_STATUS      = 1u << 3; // Enviar paquete STATUS
static constexpr uint32_t EVT_ACK_EVENT   = 1u << 4; // Enviar evento ACK
static constexpr uint32_t EVT_TIMER_IMM   = 1u << 5; // Enviar timer ya
static constexpr uint32_t EVT_WEIGHT_IMM  = 1u << 6; // Enviar peso ya
static constexpr uint32_t EVT_TARE        = 1u << 7; // Tara (software inmediata + física diferida)
static constexpr uint32_t EVT_APP_TX_MASK = EVT_HB_ACK | EVT_INFO | EVT_STATUS | EVT_ACK_EVENT |
                                            EVT_TIMER_IMM | EVT_WEIGHT_IMM;
// Hacia scale_link (core 1):
static constexpr uint32_t SCL_EVT_WAKE      = 1u << 0; // Cambió el estado (escaneo/desconexión/App)
static constexpr uint32_t SCL_EVT_TARE      = 1u << 1; // Enviar tara física a la Timemore
static constexpr uint32_t SCL_EVT_TIMER     = 1u << 2; // Enviar comando de timer (pendingTimerCmd)
static constexpr uint32_t SCL_EVT_NVS_RESET = 1u << 3; // Borrar MAC/bonds y reescanear
static constexpr uint32_t SCL_EVT_SCAN      = 1u << 4; // Forzar escaneo (comando 's')

TaskHandle_t bridgeTaskHandle            = nullptr;
TaskHandle_t scaleTaskHandle             = nullptr;
volatile uint8_t pendingTimerCmd         = 0xFF; // 0=Start, 1=Stop, 2=Reset, 0xFF=None

// Telemetría de peso y batería
volatile float rawWeightGrams            = 0.0f;
volatile float softwareTareOffset        = 0.0f;
volatile float currentWeightGrams        = 0.0f;
volatile float currentFlowRateGps        = 0.0f;
float prevFlowWeightGrams                = 0.0f;
unsigned long prevFlowCalcMs             = 0;

// Pipeline de baja latencia: el callback BLE publica la muestra con su timestamp
// y despierta a bridge_tx; bridge_tx calcula caudal y envía de inmediato.
static portMUX_TYPE sampleMux            = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t scaleSampleSeq         = 0;   // Se incrementa con cada muestra nueva
volatile unsigned long scaleSampleMs     = 0;   // millis() en que llegó la muestra
volatile uint32_t scaleSampleUs          = 0;   // esp_timer (µs) en que llegó la muestra
uint32_t processedSampleSeq              = 0;   // Solo bridge_tx
volatile bool flowResetRequested         = false;
volatile bool weightIsStable             = true;
volatile uint8_t scaleBatteryPct         = 90;
volatile uint32_t timemorePacketsRxCount = 0;
volatile uint32_t acaiaPacketsTxCount    = 0;

// Cronómetro interno emulado
bool timerRunning                        = false;
unsigned long timerStartMillis           = 0;
unsigned long timerAccumulatedMs         = 0;

// Temporizadores
unsigned long lastKeepAliveMs            = 0;   // Solo scale_link
unsigned long lastSerialStatusMs         = 0;   // Solo loop
volatile unsigned long scaleConnectedAtMs = 0;
volatile bool scaleLinkTuned             = true;
volatile unsigned long resetBlinkUntilMs = 0;   // Parpadeo magenta no bloqueante tras borrar NVS

// ----------------------------------------------------------------------------
// ESTADÍSTICAS DE LATENCIA (escritas solo por bridge_tx, leídas y reiniciadas por loop)
// ----------------------------------------------------------------------------
struct LatencyStats {
  uint32_t samples;        // Muestras procesadas
  uint32_t coalesced;      // Muestras pisadas antes de procesarse (llegaron 2 seguidas)
  uint32_t wakeMinUs, wakeMaxUs; uint64_t wakeSumUs;   // Llegada -> bridge_tx procesando
  uint32_t e2eMinUs,  e2eMaxUs;  uint64_t e2eSumUs;    // Llegada -> notify() entregado al stack
  uint32_t e2eCount;
  uint32_t hist[5];        // e2e: <250us, <1ms, <5ms, <15ms, >=15ms
  uint32_t dtMinMs, dtMaxMs; uint64_t dtSumMs; uint32_t dtCount; // Intervalo entre muestras
  uint32_t txWeight;       // Notificaciones de peso enviadas
  uint32_t txFail;         // notify() rechazados por el stack (sin buffers)
};
static portMUX_TYPE statsMux             = portMUX_INITIALIZER_UNLOCKED;
static LatencyStats latStats;
volatile uint32_t notifyFailTotal        = 0;

static inline void resetLatencyStats(LatencyStats& s) {
  memset(&s, 0, sizeof(s));
  s.wakeMinUs = UINT32_MAX;
  s.e2eMinUs  = UINT32_MAX;
  s.dtMinMs   = UINT32_MAX;
}

// ----------------------------------------------------------------------------
// DESPACHO DE EVENTOS ENTRE TAREAS (llamables desde callbacks de nimble_host)
// ----------------------------------------------------------------------------
static inline void postBridgeEvent(uint32_t bits) {
  TaskHandle_t h = bridgeTaskHandle;
  if (h != nullptr) xTaskNotify(h, bits, eSetBits);
}

static inline void postScaleEvent(uint32_t bits) {
  TaskHandle_t h = scaleTaskHandle;
  if (h != nullptr) xTaskNotify(h, bits, eSetBits);
}

// Botón BOOT y LED RGB con caché de estado
unsigned long bootBtnPressStartMs        = 0;
bool bootBtnPressed                      = false;
bool bootBtnLongPressHandled             = false;
unsigned long ledFlashUntilMs            = 0;

uint8_t cachedLedR                       = 255;
uint8_t cachedLedG                       = 255;
uint8_t cachedLedB                       = 255;
bool ledInitialized                      = false;

// ============================================================================
// DIAGNÓSTICO DE REINICIO Y UTILIDADES DE DEBUG SERIAL
// ============================================================================
const char* getResetReasonString() {
#if __has_include(<esp_system.h>)
  esp_reset_reason_t r = esp_reset_reason();
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (Encendido normal)";
    case ESP_RST_SW:        return "SW (Reinicio por software)";
    case ESP_RST_PANIC:     return "PANIC (Excepcion / Guru Meditation)";
    case ESP_RST_INT_WDT:   return "INT_WDT (Interrupt Watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (Task Watchdog)";
    case ESP_RST_WDT:       return "WDT (Otro Watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (Caida de tension USB/3.3V)";
    case ESP_RST_USB:       return "USB (Reinicio por puerto USB CDC)";
    default:                return "OTRO / DESCONOCIDO";
  }
#else
  return "N/A";
#endif
}

void debugPrintHex(const char* prefix, const uint8_t* data, size_t len) {
#if ENABLE_SERIAL_DEBUG && DEBUG_VERBOSE_BLE_PACKETS
  if (!Serial || Serial.availableForWrite() < 128) return;
  Serial.printf("[%8lu ms] %s (%u bytes): ", millis(), prefix, (unsigned)len);
  for (size_t i = 0; i < len; i++) {
    Serial.printf("%02X ", data[i]);
  }
  Serial.println();
#endif
}

// ============================================================================
// CONTROL SEGURO DEL LED RGB (WS2812) CON CACHÉ ANTI-RMT SPAM
// ============================================================================
void initRgbLed() {
#if ENABLE_RGB_LED
  // Nota Core 3.x: NO llamar a pinMode(OUTPUT) antes de rgbLedWrite(), ya que
  // rgbLedWrite() reclama e inicializa internamente el canal RMT del GPIO.
  ledInitialized = true;
#endif
}

void writeRgbPixelSafe(uint8_t r, uint8_t g, uint8_t b) {
#if ENABLE_RGB_LED
  if (!ledInitialized) return;

  uint8_t sr = (uint16_t(r) * LED_BRIGHTNESS_MAX) / 255;
  uint8_t sg = (uint16_t(g) * LED_BRIGHTNESS_MAX) / 255;
  uint8_t sb = (uint16_t(b) * LED_BRIGHTNESS_MAX) / 255;

  // Solo escribir al periférico RMT si el color realmente cambió
  if (sr == cachedLedR && sg == cachedLedG && sb == cachedLedB) {
    return;
  }
  cachedLedR = sr;
  cachedLedG = sg;
  cachedLedB = sb;

  #if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  rgbLedWrite(PIN_RGB_LED_S3_ZERO, sr, sg, sb);
  #else
  neopixelWrite(PIN_RGB_LED_S3_ZERO, sr, sg, sb);
  #endif
#endif
}

void triggerTareLedFlash() {
  ledFlashUntilMs = millis() + 180;
}

void updateStatusLed() {
  unsigned long now = millis();

  if (now < ledFlashUntilMs) {
    writeRgbPixelSafe(255, 255, 255); // Blanco: Tara
    return;
  }

  if (now < resetBlinkUntilMs) {
    // Magenta parpadeante (120 ms): confirmación de borrado NVS, sin bloquear tareas
    if (((now / 120) % 2) == 0) writeRgbPixelSafe(255, 0, 180);
    else                        writeRgbPixelSafe(0, 0, 0);
    return;
  }

  if (bootBtnPressed && !bootBtnLongPressHandled && (now - bootBtnPressStartMs > 400)) {
    writeRgbPixelSafe(255, 0, 128); // Rosa/Magenta: presionando botón BOOT
    return;
  }

  if (scaleConnected && appConnected) {
    writeRgbPixelSafe(0, 255, 40);    // VERDE FIJO: Timemore + App conectados
  } else if (scaleConnected && !appConnected) {
    writeRgbPixelSafe(0, 180, 255);   // CIAN FIJO: Timemore conectada, esperando App
  } else if (!scaleConnected && appConnected) {
    writeRgbPixelSafe(255, 140, 0);   // ÁMBAR FIJO: App conectada, buscando Timemore
  } else {
    // AZUL PARPADEANTE (cambia solo cada 450 ms)
    bool blinkOn = ((now / 450) % 2) == 0;
    if (blinkOn) {
      writeRgbPixelSafe(0, 70, 255);
    } else {
      writeRgbPixelSafe(0, 0, 0);
    }
  }
}

void blinkResetConfirmation() {
  // No bloqueante: updateStatusLed() (loop) dibuja 6 parpadeos durante 1.44 s
  resetBlinkUntilMs = millis() + 6 * 240;
}

// ============================================================================
// FUNCIONES MATEMÁTICAS: CRC-16 (TIMEMORE) Y CHECKSUM DUAL (ACAIA)
// ============================================================================
uint16_t calculateTimemoreCRC16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= static_cast<uint16_t>(data[i]);
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void appendAcaiaChecksum(uint8_t* packet, size_t payloadOffset, size_t payloadLen) {
  uint8_t cksum1 = 0;
  uint8_t cksum2 = 0;
  for (size_t i = 0; i < payloadLen; i++) {
    if ((i & 1) == 0) {
      cksum1 = static_cast<uint8_t>((cksum1 + packet[payloadOffset + i]) & 0xFF);
    } else {
      cksum2 = static_cast<uint8_t>((cksum2 + packet[payloadOffset + i]) & 0xFF);
    }
  }
  packet[payloadOffset + payloadLen]     = cksum1;
  packet[payloadOffset + payloadLen + 1] = cksum2;
}

// ============================================================================
// CONSTRUCCIÓN Y ENVÍO DE TRAMAS ACAIA LUNAR (EJECUTADAS SOLO DESDE bridge_tx)
// ============================================================================
/**
 * Envía la notificación únicamente a la conexión de la App (appConnHandle).
 * En NimBLE v2 notify() sin handle recorre getPeerDevices(), que construye un
 * std::vector (reserva en heap) en CADA paquete; con el handle directo se evita.
 * Devuelve false si el stack rechazó el paquete (sin buffers).
 */
static bool notifyToApp(NimBLECharacteristic* chr, const uint8_t* data, size_t len) {
  chr->setValue(data, len);
#if defined(NIMBLE_V2)
  uint16_t h = appConnHandle;
  bool ok = (h != 0xFFFF) ? chr->notify(h) : chr->notify();
#else
  chr->notify();
  bool ok = true;
#endif
  if (ok) {
    acaiaPacketsTxCount = acaiaPacketsTxCount + 1;
  } else {
    notifyFailTotal = notifyFailTotal + 1;
  }
  return ok;
}

bool sendAcaiaRawPacket(const uint8_t* data, size_t len) {
  if (!appConnected) return false;

  // NOTA CRÍTICA NIMBLE v2.x:
  // En NimBLE v2.x, characteristic->notify() envía un paquete ATT HANDLE_VALUE_NTF
  // a todos los clientes conectados incluso si no se suscribieron a ese CCCD.
  // Por eso enviamos ÚNICAMENTE por el canal activo suscrito por la App (pActiveAcaiaNotifyChar)
  // evitando duplicar paquetes en el aire y eliminando el lag/atasco en de1app.
  NimBLECharacteristic* targetChar = pActiveAcaiaNotifyChar;
  if (targetChar == nullptr) {
    targetChar = (pAcaiaIsscNotifyChar1E4D != nullptr) ? pAcaiaIsscNotifyChar1E4D : pAcaiaNotifyCharFF11;
  }

  if (targetChar == nullptr) return false;
  return notifyToApp(targetChar, data, len);
}

bool sendAcaiaMessage(uint8_t msgType, const uint8_t* payload, size_t payloadLen) {
  if (payloadLen > 48) return false;
  uint8_t packet[64];
  packet[0] = ACAIA_HEADER_1;
  packet[1] = ACAIA_HEADER_2;
  packet[2] = msgType;
  if (payloadLen > 0 && payload != nullptr) {
    memcpy(&packet[3], payload, payloadLen);
  }
  appendAcaiaChecksum(packet, 3, payloadLen);
  return sendAcaiaRawPacket(packet, 3 + payloadLen + 2);
}

void sendAcaiaHeartbeatAck() {
  const uint8_t payload[] = { 0x02, 0x00 };
  sendAcaiaMessage(ACAIA_MSG_SYSTEM, payload, sizeof(payload));
  DBG_PRINTLN("[ACAIA-TX] Heartbeat ACK (0x00) enviado.");
}

void sendAcaiaStatusPacket() {
  uint8_t batt = scaleBatteryPct;
  if (batt > 100) batt = 100;

  // NOTA PROTOCOLO ACAIA: El byte 0 del payload (packet[3]) incluye al propio
  // byte de longitud (N_datos + 1 = sizeof(payload) = 7 = 0x07).
  const uint8_t payload[7] = {
    0x07, // Longitud total del bloque (1 byte len + 6 bytes datos = 7)
    batt, // Batería (0-100%)
    0x02, // Unidades: 2 = gramos
    0x00, // Modo normal
    0x00, // Auto-off desactivado
    0x00, // Botones habilitados
    0x01  // Sonido activado
  };
  sendAcaiaMessage(ACAIA_MSG_STATUS, payload, sizeof(payload));
  DBG_PRINTF("[ACAIA-TX] Paquete STATUS (0x08, len=0x07) enviado (Bateria=%u%%).", batt);
}

void sendAcaiaInfoPacket() {
  const uint8_t payload[8] = {
    0x08, // Longitud total del bloque (1 byte len + 7 bytes datos = 8)
    0x02, // Major version 2
    0x00, // Minor version 0
    0x08, // Patch version 8
    'L', 'U', 'N', 'A'
  };
  sendAcaiaMessage(ACAIA_MSG_INFO, payload, sizeof(payload));
  DBG_PRINTLN("[ACAIA-TX] Paquete INFO (0x07, len=0x08) enviado.");
}

void sendAcaiaAckEvent() {
  const uint8_t payload[4] = { 0x04, ACAIA_EVENT_ACK, 0x00, 0x00 };
  sendAcaiaMessage(ACAIA_MSG_EVENT, payload, sizeof(payload));
}

/**
 * Cálculo de caudal (g/s) con EMA para Wendougee (Registro 1423) y de1app.
 * Se ejecuta UNA vez por muestra nueva de la balanza usando el timestamp de
 * llegada de esa muestra (no el instante de envío), así el dt es exacto y el
 * caudal no se retrasa ni oscila por el jitter del planificador.
 */
void updateFlowFromSample(float weightGrams, unsigned long sampleMs) {
  if (flowResetRequested) {
    flowResetRequested  = false;
    currentFlowRateGps  = 0.0f;
    prevFlowWeightGrams = weightGrams;
    prevFlowCalcMs      = sampleMs;
    return;
  }

  unsigned long dtMs = sampleMs - prevFlowCalcMs;
  if (prevFlowCalcMs == 0 || dtMs > 1000) {
    currentFlowRateGps  = 0.0f;
    prevFlowWeightGrams = weightGrams;
    prevFlowCalcMs      = sampleMs;
  } else if (dtMs >= FLOW_MIN_DT_MS) {
    float rawFlow = ((weightGrams - prevFlowWeightGrams) * 1000.0f) / static_cast<float>(dtMs);
    if (fabsf(rawFlow) > 50.0f) rawFlow = 0.0f; // Descartar salto por tara
    currentFlowRateGps = (FLOW_EMA_ALPHA * rawFlow) + ((1.0f - FLOW_EMA_ALPHA) * currentFlowRateGps);
    if (fabsf(currentFlowRateGps) < 0.05f) currentFlowRateGps = 0.0f;
    prevFlowWeightGrams = weightGrams;
    prevFlowCalcMs      = sampleMs;
  }
}

bool sendBookooWeightNotification(float weightGrams) {
  if (!appConnected || pBookooNotifyCharFF11 == nullptr) return false;

  unsigned long nowMs = millis();
  unsigned long totalMs = timerAccumulatedMs;
  if (timerRunning) {
    totalMs += (nowMs - timerStartMillis);
  }

  // El caudal (currentFlowRateGps) se calcula en updateFlowFromSample() por cada
  // muestra real de la balanza; aquí solo se empaqueta.
  bool isNegative = (weightGrams < -0.005f);
  uint32_t centiGrams = static_cast<uint32_t>(roundf(fabsf(weightGrams) * 100.0f));
  bool isFlowNegative = (currentFlowRateGps < -0.005f);
  uint16_t centiFlow = static_cast<uint16_t>(roundf(fabsf(currentFlowRateGps) * 100.0f));
  uint8_t batt = (scaleBatteryPct > 100) ? 100 : scaleBatteryPct;

  // Trama oficial de 20 bytes BooKoo Themis Mini/Ultra (Servicio 0x0FFE -> Característica 0xFF11)
  // Ref: https://github.com/BooKooCode/OpenSource/blob/main/bookoo_ultra_scale/protocols.md
  uint8_t pkt[20];
  pkt[0]  = 0x03; // Product Number (0x03)
  pkt[1]  = 0x0B; // Type (0x0B = Weight & Telemetry)
  pkt[2]  = static_cast<uint8_t>((totalMs >> 16) & 0xFF);
  pkt[3]  = static_cast<uint8_t>((totalMs >> 8) & 0xFF);
  pkt[4]  = static_cast<uint8_t>(totalMs & 0xFF);
  pkt[5]  = 0x01; // Unidad de peso oficial BooKoo: 0x01 = Gram
  pkt[6]  = isNegative ? 0x2D : 0x2B; // Signo peso: '-' (0x2D) o '+' (0x2B)
  pkt[7]  = static_cast<uint8_t>((centiGrams >> 16) & 0xFF);
  pkt[8]  = static_cast<uint8_t>((centiGrams >> 8) & 0xFF);
  pkt[9]  = static_cast<uint8_t>(centiGrams & 0xFF);
  pkt[10] = isFlowNegative ? 0x2D : 0x2B; // Signo flujo: '-' (0x2D) o '+' (0x2B)
  pkt[11] = static_cast<uint8_t>((centiFlow >> 8) & 0xFF); // Flow rate * 100 MSB
  pkt[12] = static_cast<uint8_t>(centiFlow & 0xFF);        // Flow rate * 100 LSB
  pkt[13] = batt; // Batería (0-100%)
  pkt[14] = 0x00; // Standby time MSB (10 min = 0x0064)
  pkt[15] = 0x64; // Standby time LSB
  pkt[16] = 0x01; // Buzzer gear
  pkt[17] = 0x01; // Flow smoothing switch
  pkt[18] = 0x00; // Reservado / Auto-mode

  uint8_t xorSum = 0;
  for (size_t i = 0; i < 19; i++) {
    xorSum ^= pkt[i];
  }
  pkt[19] = xorSum; // Checksum XOR de bytes 0..18

  return notifyToApp(pBookooNotifyCharFF11, pkt, sizeof(pkt));
}

bool sendAcaiaWeightNotification(float weightGrams, bool stable) {
  if (!appConnected) return false;

#if defined(EMULATE_BOOKOO)
  return sendBookooWeightNotification(weightGrams);
#else
  bool isNegative = (weightGrams < -0.04f);
  float absWeight = fabsf(weightGrams);

  #if ACAIA_WEIGHT_UNIT_MODE == 2
  uint32_t scaledWeight = static_cast<uint32_t>(roundf(absWeight * 100.0f));
  uint8_t unitCode = 0x02; // 0.01 g
  #else
  uint32_t scaledWeight = static_cast<uint32_t>(roundf(absWeight * 10.0f));
  uint8_t unitCode = 0x01; // 0.1 g
  #endif

  uint8_t signAndStability = 0x00;
  if (!stable) {
    signAndStability |= 0x01; // Bit 0: inestable
  }
  if (isNegative) {
    signAndStability |= 0x02; // Bit 1: negativo
  }

  // En protocolo Acaia MSG_EVENT (0x0C):
  // payload[0] (packet[3]) = sizeof(payload) = 0x08 (1 byte len + 7 bytes de evento).
  // Con esto la trama completa mide 13 bytes:
  // [0xEF, 0xDD, 0x0C, 0x08, 0x05, w0, w1, w2, w3, unit, sign, cksum1, cksum2]
  uint8_t payload[8];
  payload[0] = 0x08;
  payload[1] = ACAIA_EVENT_WEIGHT;
  payload[2] = static_cast<uint8_t>(scaledWeight & 0xFF);
  payload[3] = static_cast<uint8_t>((scaledWeight >> 8) & 0xFF);
  payload[4] = static_cast<uint8_t>((scaledWeight >> 16) & 0xFF);
  payload[5] = static_cast<uint8_t>((scaledWeight >> 24) & 0xFF);
  payload[6] = unitCode;
  payload[7] = signAndStability;

  return sendAcaiaMessage(ACAIA_MSG_EVENT, payload, sizeof(payload));
#endif
}

void sendAcaiaTimerNotification() {
  if (!appConnected) return;

  unsigned long totalMs = timerAccumulatedMs;
  if (timerRunning) {
    totalMs += (millis() - timerStartMillis);
  }

  uint8_t minutes = static_cast<uint8_t>((totalMs / 60000UL) % 60);
  uint8_t seconds = static_cast<uint8_t>((totalMs / 1000UL) % 60);
  uint8_t tenths  = static_cast<uint8_t>((totalMs % 1000UL) / 100UL);

  uint8_t payload[5];
  payload[0] = 0x05; // 1 byte len + 4 bytes evento = 5
  payload[1] = ACAIA_EVENT_TIMER;
  payload[2] = minutes;
  payload[3] = seconds;
  payload[4] = tenths;

  sendAcaiaMessage(ACAIA_MSG_EVENT, payload, sizeof(payload));
}

// ============================================================================
// COMANDOS Y DECODIFICACIÓN TIMEMORE DOT (CLIENTE BLE)
// ============================================================================
bool sendTimemoreOpenScaleFrame(uint8_t opcode, uint8_t cmd, const uint8_t* payload, uint16_t payloadLen) {
  if (!scaleConnected || pScaleWriteChar == nullptr) return false;
  if (payloadLen > 32) return false;

  uint8_t frame[48];
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = opcode;
  frame[3] = cmd;
  frame[4] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  frame[5] = static_cast<uint8_t>(payloadLen & 0xFF);

  if (payloadLen > 0 && payload != nullptr) {
    memcpy(&frame[6], payload, payloadLen);
  }

  size_t headerAndPayloadLen = 6 + payloadLen;
  uint16_t crc = calculateTimemoreCRC16(frame, headerAndPayloadLen);
  frame[headerAndPayloadLen]     = static_cast<uint8_t>((crc >> 8) & 0xFF);
  frame[headerAndPayloadLen + 1] = static_cast<uint8_t>(crc & 0xFF);

  debugPrintHex("[TIMEMORE-TX]", frame, headerAndPayloadLen + 2);
  return pScaleWriteChar->writeValue(frame, headerAndPayloadLen + 2, false);
}

/**
 * Tara por software (bridge_tx, core 0): instantánea, la App ve 0.0 g en el
 * siguiente paquete. La tara física se delega a scale_link (core 1), que
 * serializa todas las escrituras GATT hacia la Timemore con connect/keep-alive.
 */
void applySoftwareTare() {
  portENTER_CRITICAL(&sampleMux);
  softwareTareOffset = rawWeightGrams;
  currentWeightGrams = 0.0f;
  portEXIT_CRITICAL(&sampleMux);
  flowResetRequested = true;   // Evita un pico negativo de caudal tras la tara
  currentFlowRateGps = 0.0f;
  triggerTareLedFlash();
  postScaleEvent(SCL_EVT_TARE);
}

// ----------------------------------------------------------------------------
// TRAMAS DE LA TIMEMORE DOT (servicio 0xFFF0: notify 0xFFF1, write 0xFFF2)
// ----------------------------------------------------------------------------
// Bytes EXACTOS usados por de1app (PR #345, decentespresso/de1app), extraídos
// de capturas HCI de la app oficial de Timemore y probados con una Dot real.
// Se envían tal cual: su longitud/cola no encaja con el CRC que calculaba
// sendTimemoreOpenScaleFrame(), así que no se recalculan.
static const uint8_t DOT_INIT_CMDS[][8] = {
  { 0xA5, 0x5A, 0x02, 0x13, 0x00, 0x00, 0x00, 0x00 },
  { 0xA5, 0x5A, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00 },
  { 0xA5, 0x5A, 0x02, 0x05, 0x00, 0x00, 0x00, 0x00 },
  { 0xA5, 0x5A, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00 },
  { 0xA5, 0x5A, 0x02, 0x06, 0x00, 0x00, 0x00, 0x00 },
  { 0xA5, 0x5A, 0x02, 0x0C, 0x00, 0x00, 0x00, 0x00 },
};
static const uint8_t DOT_POLL_1[]       = { 0xA5, 0x5A, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t DOT_POLL_2[]       = { 0xA5, 0x5A, 0x03, 0x08, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x25 };
static const uint8_t DOT_TARE[]         = { 0xA5, 0x5A, 0x03, 0x0D, 0x00, 0x02, 0x00, 0x00, 0x00, 0x71 };
static const uint8_t DOT_TIMER_START[]  = { 0xA5, 0x5A, 0x03, 0x02, 0x00, 0x01, 0x01, 0x00, 0x20 };
static const uint8_t DOT_TIMER_STOP[]   = { 0xA5, 0x5A, 0x03, 0x02, 0x00, 0x01, 0x02, 0x00, 0xFF, 0xD0 };
static const uint8_t DOT_TIMER_RESET[]  = { 0xA5, 0x5A, 0x03, 0x02, 0x00, 0x01, 0x03, 0x00, 0xFF, 0x81 };

volatile bool scaleUsesDotService = false; // true si la balanza expone 0xFFF0 (Timemore Dot)

static bool writeScaleRaw(const uint8_t* data, size_t len) {
  NimBLERemoteCharacteristic* w = pScaleWriteChar;
  if (!scaleConnected || w == nullptr) return false;
  debugPrintHex("[TIMEMORE-TX]", data, len);
  return w->writeValue(data, len, false);
}

// Secuencia de inicio de la app oficial (dos pasadas, 150 ms entre comandos) +
// sondeo inicial. Solo desde scale_link: bloquea ~2.5 s sin afectar a la App.
void sendTimemoreDotInitSequence() {
  DBG_PRINTLN("[TIMEMORE-TX] Enviando secuencia de inicio de la Dot (app oficial)...");
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < sizeof(DOT_INIT_CMDS) / sizeof(DOT_INIT_CMDS[0]); i++) {
      if (!scaleConnected) return;
      writeScaleRaw(DOT_INIT_CMDS[i], sizeof(DOT_INIT_CMDS[i]));
      vTaskDelay(pdMS_TO_TICKS(150));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

// Sondeo periódico (cada TIMEMORE_KEEPALIVE_INTERVAL_MS) que mantiene la Dot activa
void sendTimemoreKeepAlive() {
  if (!scaleConnected) return;
  if (scaleUsesDotService) {
    writeScaleRaw(DOT_POLL_1, sizeof(DOT_POLL_1));
    vTaskDelay(pdMS_TO_TICKS(100));
    writeScaleRaw(DOT_POLL_2, sizeof(DOT_POLL_2));
  } else if (pScaleWriteChar != nullptr) {
    // Otras variantes (0xFFE0 / 0x181D): sondeo genérico anterior
    sendTimemoreOpenScaleFrame(0x02, 0x05, nullptr, 0);
    sendTimemoreOpenScaleFrame(0x02, 0x02, nullptr, 0);
  }
  DBG_PRINTLN("[KEEP-ALIVE] Sondeo enviado a la Timemore.");
}

// Tara física en la Timemore (solo desde scale_link)
void executeTimemoreTare() {
  if (!scaleConnected || pScaleWriteChar == nullptr) {
    DBG_PRINTLN("[TARA] Tara por software aplicada (0.0g). Balanza fisica no tiene canal Write o no esta conectada.");
    return;
  }

  if (scaleUsesDotService) {
    // Igual que de1app: sondeo y, 200 ms después, el comando de tara
    writeScaleRaw(DOT_POLL_1, sizeof(DOT_POLL_1));
    vTaskDelay(pdMS_TO_TICKS(100));
    writeScaleRaw(DOT_POLL_2, sizeof(DOT_POLL_2));
    vTaskDelay(pdMS_TO_TICKS(100));
    writeScaleRaw(DOT_TARE, sizeof(DOT_TARE));
  } else {
    // Variante UART 0xFFE1/0xFFE2
    const uint8_t uartTareCmd[] = { 0xAA, 0x02, 0x43, 0x00, 0x45 };
    writeScaleRaw(uartTareCmd, sizeof(uartTareCmd));
  }
  DBG_PRINTLN("[TARA] Comando de TARA enviado a la Timemore + respaldo 0.0g activo.");
}

void executeTimemoreTimerCommand(uint8_t action) {
  if (!scaleConnected || pScaleWriteChar == nullptr || !scaleUsesDotService) return;

  const uint8_t* cmd = DOT_TIMER_START;
  size_t len = sizeof(DOT_TIMER_START);
  if (action == 1)      { cmd = DOT_TIMER_STOP;  len = sizeof(DOT_TIMER_STOP); }
  else if (action == 2) { cmd = DOT_TIMER_RESET; len = sizeof(DOT_TIMER_RESET); }

  writeScaleRaw(cmd, len);
  DBG_PRINTF("[TIMEMORE-TX] Comando de Timer enviado (accion=%u: %s).", action,
             action == 0 ? "start" : (action == 1 ? "stop" : "reset"));
}

// Captura de las primeras tramas de peso de cada conexión, para comparar con la
// pantalla de la Dot. El callback solo hace memcpy; loop() las imprime.
static constexpr uint8_t DBG_FRAME_SLOTS = 3;
static constexpr uint8_t DBG_FRAME_MAX   = 24;
uint8_t dbgFrames[DBG_FRAME_SLOTS][DBG_FRAME_MAX];
uint8_t dbgFrameLens[DBG_FRAME_SLOTS];
float dbgFrameWeights[DBG_FRAME_SLOTS];
volatile uint8_t dbgFrameCount   = 0; // Escrito por el callback
uint8_t dbgFramesPrinted         = 0; // Solo loop

bool parseTimemorePayload(const uint8_t* data, size_t len, float& outWeightGrams, bool& outStable, uint8_t& inoutBattery) {
  if (data == nullptr || len < 4) return false;

  // CASO 0: Timemore Dot (formato verificado por de1app con hardware real):
  //   A5 5A [tipo] [estado] .. .. .. .. [WH] [WL] ...
  //   tipo 0x01 = peso; peso = int16 big-endian en bytes 8-9, en 0.1 g.
  //   tipo 0x02 (respuesta) y 0x03 (ACK) no llevan peso.
  if (scaleUsesDotService && len >= 10 && data[0] == 0xA5 && data[1] == 0x5A) {
    if (data[2] != 0x01) return false;
    int16_t raw16 = static_cast<int16_t>((static_cast<uint16_t>(data[8]) << 8) | data[9]);
    outWeightGrams = static_cast<float>(raw16) / 10.0f;
    outStable = true;
    return true;
  }

  // CASO A: Timemore Open Scale Protocol (0xA5, 0x5A)
  if (len >= 8 && data[0] == 0xA5 && data[1] == 0x5A) {
    uint8_t cmd = data[3];
    uint16_t payloadLen = (static_cast<uint16_t>(data[4]) << 8) | data[5];

    if (cmd == 0x01 && len >= 10) {
      int32_t rawVal = 0;
      if (payloadLen >= 4 && len >= 12) {
        rawVal = (static_cast<int32_t>(static_cast<int8_t>(data[6])) << 24) |
                 (static_cast<int32_t>(data[7]) << 16) |
                 (static_cast<int32_t>(data[8]) << 8) |
                 static_cast<int32_t>(data[9]);
      } else {
        int16_t raw16 = static_cast<int16_t>((static_cast<uint16_t>(data[6]) << 8) | data[7]);
        rawVal = raw16;
      }
      outWeightGrams = static_cast<float>(rawVal) / 10.0f;
      if (6 + payloadLen <= len && payloadLen >= 7) {
        uint8_t statusFlags = data[6 + payloadLen - 1];
        outStable = ((statusFlags & 0x01) != 0);
      } else {
        outStable = true;
      }
      return true;
    }

    if (cmd == 0x05 && payloadLen >= 1 && len >= 7) {
      uint8_t batt = data[6];
      if (batt <= 100) inoutBattery = batt;
      return false;
    }
  }

  // CASO B: Protocolo UART 0xFFE0 / 0xFFE1 con cabecera 0xAA o 0x55
  if (data[0] == 0xAA || data[0] == 0x55) {
    if (len >= 7 && ((data[0] == 0xAA && data[1] == 0x55) || (data[0] == 0x55 && data[1] == 0xAA))) {
      uint8_t statusByte = data[3];
      bool isNeg = ((statusByte & 0x80) != 0) || (statusByte == 0x01) || (statusByte == '-');
      outStable  = ((statusByte & 0x40) == 0);
      int16_t rawMag = static_cast<int16_t>((static_cast<uint16_t>(data[4]) << 8) | data[5]);
      if (rawMag < 0) {
        outWeightGrams = static_cast<float>(rawMag) / 10.0f;
      } else {
        outWeightGrams = (isNeg ? -1.0f : 1.0f) * (static_cast<float>(rawMag) / 10.0f);
      }
      if (len >= 8 && data[6] > 0 && data[6] <= 100) inoutBattery = data[6];
      return true;
    }

    if (len >= 5) {
      uint8_t signByte = data[1];
      bool isNeg = ((signByte & 0x80) != 0) || (signByte == 0x01) || (signByte == '-');
      int16_t raw16 = static_cast<int16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]);
      if (raw16 < 0) {
        outWeightGrams = static_cast<float>(raw16) / 10.0f;
      } else {
        outWeightGrams = (isNeg ? -1.0f : 1.0f) * (static_cast<float>(raw16) / 10.0f);
      }
      outStable = true;
      if (len >= 6 && data[4] > 0 && data[4] <= 100) inoutBattery = data[4];
      return true;
    }
  }

  // CASO C: Protocolo Timemore GATT 0x181D / 0x2A9D (Little-Endian)
  if (len >= 4 && data[0] <= 0x1F) {
    int16_t rawLe16 = static_cast<int16_t>(static_cast<uint16_t>(data[1]) |
                                          (static_cast<uint16_t>(data[2]) << 8));
    if (rawLe16 >= -20000 && rawLe16 <= 50000) {
      outWeightGrams = static_cast<float>(rawLe16) / 10.0f;
      outStable = true;
      return true;
    }
  }

  // CASO D: Trama ASCII en UART transparente
  bool looksAscii = true;
  bool hasDigit = false;
  for (size_t i = 0; i < len; i++) {
    char c = static_cast<char>(data[i]);
    if (c >= '0' && c <= '9') hasDigit = true;
    if (c != ' ' && c != '+' && c != '-' && c != '.' &&
        c != 'g' && c != 'G' && c != '\r' && c != '\n' && c != '\0' &&
        !(c >= '0' && c <= '9')) {
      looksAscii = false;
      break;
    }
  }
  if (looksAscii && hasDigit) {
    char buf[32];
    size_t copyLen = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
    bool neg = false;
    size_t outIdx = 0;
    for (size_t i = 0; i < copyLen; i++) {
      char c = static_cast<char>(data[i]);
      if (c == '-') neg = true;
      if ((c >= '0' && c <= '9') || c == '.') buf[outIdx++] = c;
    }
    buf[outIdx] = '\0';
    if (outIdx > 0) {
      float val = atof(buf);
      outWeightGrams = neg ? -val : val;
      outStable = true;
      return true;
    }
  }

  return false;
}

/**
 * Callback de notificación de la Timemore Dot (tarea nimble_host, core 0).
 * IMPORTANTE: No llama a notify() desde aquí. Publica la muestra con su marca de
 * tiempo y despierta a bridge_tx (mismo núcleo, prioridad inmediatamente inferior),
 * que la reenvía en cuanto este callback retorna.
 */
void onTimemoreNotifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic,
                              uint8_t* pData,
                              size_t length,
                              bool isNotify) {
  uint32_t arrivalUs = static_cast<uint32_t>(esp_timer_get_time());
  timemorePacketsRxCount = timemorePacketsRxCount + 1;

  float parsedWeight = 0.0f;
  bool parsedStable  = true;
  uint8_t batt       = scaleBatteryPct;

  bool isWeight = parseTimemorePayload(pData, length, parsedWeight, parsedStable, batt);
  if (isWeight) {
    // Batería solo desde tramas de peso (como antes). El byte de la respuesta 0x05
    // de la Dot NO es un porcentaje (con batería llena reporta 3), así que se ignora.
    scaleBatteryPct = batt;

    // Captura de las primeras tramas (solo memcpy; las imprime loop())
    uint8_t slot = dbgFrameCount;
    if (slot < DBG_FRAME_SLOTS) {
      size_t n = (length < DBG_FRAME_MAX) ? length : DBG_FRAME_MAX;
      memcpy(dbgFrames[slot], pData, n);
      dbgFrameLens[slot]    = static_cast<uint8_t>(n);
      dbgFrameWeights[slot] = parsedWeight;
      dbgFrameCount         = slot + 1;
    }
    unsigned long arrivalMs = millis();

    portENTER_CRITICAL(&sampleMux);
    if (fabsf(parsedWeight) < 0.15f && fabsf(softwareTareOffset) > 0.15f) {
      softwareTareOffset = 0.0f;
    }
    rawWeightGrams     = parsedWeight;
    currentWeightGrams = parsedWeight - softwareTareOffset;
    weightIsStable     = parsedStable;
    scaleSampleMs      = arrivalMs;
    scaleSampleUs      = arrivalUs;
    scaleSampleSeq     = scaleSampleSeq + 1;
    portEXIT_CRITICAL(&sampleMux);

    postBridgeEvent(EVT_SAMPLE);
    // NOTA CRÍTICA DE RENDIMIENTO: No llamar a Serial.printf dentro de este callback
    // de nimble_host (10-20 Hz) para mantener latencia < 5 us y cero bloqueos USB CDC.
  }
}

// ============================================================================
// PROCESADOR DE COMANDOS ENTRANTES DE LA APP MÓVIL (SIN RE-ENTRADA GATT)
// ============================================================================
static void parseAcaiaCommand(const uint8_t* data, size_t len, uint32_t& bridgeBits, uint32_t& scaleBits) {
  if (data == nullptr || len == 0) return;

  debugPrintHex("[ACAIA-RX CMD]", data, len);

  // Soporte nativo para comandos BooKoo Themis (0x03, 0x0A, cmd, d1, d2, xor)
  if (len >= 3 && data[0] == 0x03 && data[1] == 0x0A) {
    uint8_t bkCmd = data[2];
    if (bkCmd == 0x01) { // Tara (03 0A 01 00 00 08)
      DBG_PRINTLN("[BOOKOO-RX] Comando de TARA (0x01) recibido desde la App/Maquina.");
      bridgeBits |= EVT_TARE;
      bridgeBits |= EVT_WEIGHT_IMM;
    } else if (bkCmd == 0x04) { // Start Timer
      DBG_PRINTLN("[BOOKOO-RX] Comando START TIMER (0x04) recibido.");
      if (!timerRunning) {
        timerStartMillis = millis();
        timerRunning = true;
      }
      pendingTimerCmd = 0; scaleBits |= SCL_EVT_TIMER;
    } else if (bkCmd == 0x05) { // Stop Timer
      DBG_PRINTLN("[BOOKOO-RX] Comando STOP TIMER (0x05) recibido.");
      if (timerRunning) {
        timerAccumulatedMs += (millis() - timerStartMillis);
        timerRunning = false;
      }
      pendingTimerCmd = 1; scaleBits |= SCL_EVT_TIMER;
    } else if (bkCmd == 0x06) { // Reset Timer
      DBG_PRINTLN("[BOOKOO-RX] Comando RESET TIMER (0x06) recibido.");
      timerRunning       = false;
      timerAccumulatedMs = 0;
      timerStartMillis   = millis();
      pendingTimerCmd    = 2; scaleBits |= SCL_EVT_TIMER;
    } else if (bkCmd == 0x07) { // Tare + Start Timer
      DBG_PRINTLN("[BOOKOO-RX] Comando TARE + START TIMER (0x07) recibido.");
      bridgeBits |= EVT_TARE;
      timerAccumulatedMs = 0;
      timerStartMillis   = millis();
      timerRunning       = true;
      pendingTimerCmd    = 0; scaleBits |= SCL_EVT_TIMER;
      bridgeBits |= EVT_WEIGHT_IMM;
    } else if (bkCmd == 0x08) { // Flow Smoothing Switch (03 0A 08 00/01 00 xor)
      DBG_PRINTLN("[BOOKOO-RX] Comando FLOW SMOOTHING (0x08) recibido.");
      bridgeBits |= EVT_WEIGHT_IMM;
    }
    return;
  }

  // Si es un pulso corto de Handshake/Heartbeat BooKoo (1-2 bytes, ej. {0x02, 0x00} o {0x00})
  if (len < 3) {
    bridgeBits |= EVT_WEIGHT_IMM;
    return;
  }

  size_t idx = 0;
  while (idx + 2 < len) {
    if (data[idx] != ACAIA_HEADER_1 || data[idx + 1] != ACAIA_HEADER_2) {
      idx++;
      continue;
    }

    uint8_t msgType = data[idx + 2];

    switch (msgType) {
      case ACAIA_MSG_SYSTEM: { // 0x00 - HEARTBEAT
        DBG_PRINTLN("[ACAIA-RX] Heartbeat (0x00) recibido de la App.");
        bridgeBits |= EVT_HB_ACK;
        bridgeBits |= EVT_STATUS;
        bridgeBits |= EVT_WEIGHT_IMM;
        idx += 6;
        break;
      }

      case ACAIA_MSG_IDENTIFY: { // 0x0B - IDENTIFY HANDSHAKE
        DBG_PRINTLN("[ACAIA-RX] Handshake IDENTIFY (0x0B) recibido de la App.");
        appIdentified               = true;
        appNotificationsEnabled     = true;
        bridgeBits |= EVT_STATUS;
        bridgeBits |= EVT_WEIGHT_IMM;
        idx += 20;
        break;
      }

      case ACAIA_MSG_EVENT: { // 0x0C - NOTIFICATION REQUEST
        DBG_PRINTLN("[ACAIA-RX] Solicitud NOTIFICATION_REQUEST (0x0C) recibida.");
        appNotificationsEnabled     = true;
        bridgeBits |= EVT_STATUS;
        bridgeBits |= EVT_WEIGHT_IMM;
        uint8_t evtLen = (idx + 3 < len) ? data[idx + 3] : 0;
        idx += (evtLen > 0) ? (3 + evtLen + 2) : len;
        break;
      }

      case ACAIA_MSG_TARE: { // 0x04 - COMANDO DE TARA
        DBG_PRINTLN("[ACAIA-RX] Comando de TARA (0x04) recibido desde la App.");
        bridgeBits |= EVT_TARE;
        bridgeBits |= EVT_WEIGHT_IMM;
        idx += 6;
        break;
      }

      case ACAIA_MSG_TIMER: { // 0x0D - CONTROL DE CRONÓMETRO
        uint8_t timerAction = 0;
        if (idx + 4 < len) {
          timerAction = data[idx + 4];
        } else if (idx + 3 < len) {
          timerAction = data[idx + 3];
        }

        DBG_PRINTF("[ACAIA-RX] Comando TIMER (0x0D) recibido -> accion=%u", timerAction);
        if (timerAction == 0) {
          if (!timerRunning) {
            timerStartMillis = millis();
            timerRunning = true;
          }
        } else if (timerAction == 1) {
          if (timerRunning) {
            timerAccumulatedMs += (millis() - timerStartMillis);
            timerRunning = false;
          }
        } else if (timerAction == 2) {
          timerRunning = false;
          timerAccumulatedMs = 0;
          timerStartMillis = millis();
        }
        pendingTimerCmd            = timerAction; scaleBits |= SCL_EVT_TIMER;
        bridgeBits |= EVT_ACK_EVENT;
        bridgeBits |= EVT_TIMER_IMM;
        idx += 7;
        break;
      }

      default: {
        DBG_PRINTF("[ACAIA-RX] Comando msgType=0x%02X recibido -> respondiendo Status.", msgType);
        bridgeBits |= EVT_STATUS;
        idx = len;
        break;
      }
    }
  }
}

// Se ejecuta en nimble_host (core 0): solo decodifica y publica eventos.
void handleIncomingAcaiaCommand(const uint8_t* data, size_t len) {
  uint32_t bridgeBits = 0;
  uint32_t scaleBits  = 0;
  parseAcaiaCommand(data, len, bridgeBits, scaleBits);
  if (bridgeBits) postBridgeEvent(bridgeBits);
  if (scaleBits)  postScaleEvent(scaleBits);
}

// ============================================================================
// CALLBACKS DEL SERVIDOR BLE (EMULADOR ACAIA LUNAR)
// ============================================================================
class AcaiaServerCallbacks : public NimBLEServerCallbacks {
#if defined(NIMBLE_V2)
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    appConnHandle            = connInfo.getConnHandle();
    appConnectedAtMs         = millis();
    appLinkTuned             = false; // scale_link pedirá PHY 2M tras APP_LINK_TUNE_DELAY_MS
    appConnected             = true;
    appNotificationsEnabled  = true;
    pendingStopScan          = true; // Liberar radio BLE para que el discovery/handshake de la App no falle
    appHandshakeGraceUntilMs = millis() + APP_HANDSHAKE_GRACE_MS;
    // Intervalo de baja latencia (APP_CONN_ITVL_MIN..MAX, 15-30 ms) solicitado a la App.
    // scale_link verifica el intervalo aplicado y lo vuelve a pedir si el central lo ignoró.
    pServer->updateConnParams(connInfo.getConnHandle(), APP_CONN_ITVL_MIN, APP_CONN_ITVL_MAX, 0, 400);
    postScaleEvent(SCL_EVT_WAKE);
    postBridgeEvent(EVT_WEIGHT_IMM);
    DBG_PRINTF("[ACAIA-SRV] App movil conectada! MAC Cliente: %s",
               connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    appConnected              = false;
    appIdentified             = false;
    appNotificationsEnabled   = false;
    appConnHandle             = 0xFFFF;
    appLinkTuned              = true;
    pActiveAcaiaNotifyChar    = nullptr;
    pendingRestartAdvertising = true;
    postScaleEvent(SCL_EVT_WAKE);
    DBG_PRINTF("[ACAIA-SRV] App movil desconectada (razon=%d). Reanudando Advertising...", reason);
  }

  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    DBG_PRINTF("[ACAIA-SRV] Parametros de enlace App: intervalo=%.2f ms, latencia=%u, timeout=%u ms",
               connInfo.getConnInterval() * 1.25f, connInfo.getConnLatency(),
               connInfo.getConnTimeout() * 10);
  }

  void onPhyUpdate(NimBLEConnInfo& connInfo, uint8_t txPhy, uint8_t rxPhy) override {
    DBG_PRINTF("[ACAIA-SRV] PHY enlace App: TX=%uM RX=%uM", txPhy, rxPhy);
  }
#else
  void onConnect(NimBLEServer* pServer) override {
    appConnected             = true;
    appNotificationsEnabled  = true;
    pendingStopScan          = true;
    appHandshakeGraceUntilMs = millis() + APP_HANDSHAKE_GRACE_MS;
    DBG_PRINTLN("[ACAIA-SRV] App movil conectada!");
  }

  void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
    appConnected             = true;
    appNotificationsEnabled  = true;
    pendingStopScan          = true;
    appHandshakeGraceUntilMs = millis() + APP_HANDSHAKE_GRACE_MS;
    pServer->updateConnParams(desc->conn_handle, APP_CONN_ITVL_MIN, APP_CONN_ITVL_MAX, 0, 400);
    DBG_PRINTF("[ACAIA-SRV] App movil conectada! MAC Cliente: %s",
               NimBLEAddress(desc->peer_ota_addr).toString().c_str());
  }

  void onDisconnect(NimBLEServer* pServer) override {
    appConnected              = false;
    appIdentified             = false;
    appNotificationsEnabled   = false;
    pActiveAcaiaNotifyChar    = nullptr;
    pendingRestartAdvertising = true;
    DBG_PRINTLN("[ACAIA-SRV] App movil desconectada. Reanudando Advertising...");
  }

  void onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
    appConnected              = false;
    appIdentified             = false;
    appNotificationsEnabled   = false;
    pActiveAcaiaNotifyChar    = nullptr;
    pendingRestartAdvertising = true;
    DBG_PRINTLN("[ACAIA-SRV] App movil desconectada. Reanudando Advertising...");
  }
#endif
};

class AcaiaCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
#if defined(NIMBLE_V2)
  void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    DBG_PRINTF("[ACAIA-SRV] >>> APP LEYO (onRead) CARACTERISTICA: %s <<<",
               pCharacteristic->getUUID().toString().c_str());
  }

  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    if (pCharacteristic == pAcaiaIsscWriteChar8841 && pAcaiaIsscNotifyChar1E4D != nullptr) {
      pActiveAcaiaNotifyChar = pAcaiaIsscNotifyChar1E4D;
    } else if (pCharacteristic == pAcaiaWriteCharFF10 && pAcaiaNotifyCharFF11 != nullptr) {
      pActiveAcaiaNotifyChar = pAcaiaNotifyCharFF11;
    }
    NimBLEAttValue val = pCharacteristic->getValue();
    handleIncomingAcaiaCommand(val.data(), val.size());
  }

  void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    DBG_PRINTF("[ACAIA-SRV] >>> APP SUSCRITA (onSubscribe) EN %s (subValue=%u) <<<",
               pCharacteristic->getUUID().toString().c_str(), subValue);
    if (subValue > 0) {
      if (pCharacteristic == pAcaiaIsscNotifyChar1E4D || pCharacteristic == pAcaiaNotifyCharFF11) {
        pActiveAcaiaNotifyChar = pCharacteristic;
      }
      appNotificationsEnabled     = true;
      postBridgeEvent(EVT_WEIGHT_IMM);
    }
  }
#else
  void onRead(NimBLECharacteristic* pCharacteristic) override {
    DBG_PRINTF("[ACAIA-SRV] >>> APP LEYO (onRead) CARACTERISTICA: %s <<<",
               pCharacteristic->getUUID().toString().c_str());
  }

  void onWrite(NimBLECharacteristic* pCharacteristic) override {
    if (pCharacteristic == pAcaiaIsscWriteChar8841 && pAcaiaIsscNotifyChar1E4D != nullptr) {
      pActiveAcaiaNotifyChar = pAcaiaIsscNotifyChar1E4D;
    } else if (pCharacteristic == pAcaiaWriteCharFF10 && pAcaiaNotifyCharFF11 != nullptr) {
      pActiveAcaiaNotifyChar = pAcaiaNotifyCharFF11;
    }
    std::string val = pCharacteristic->getValue();
    handleIncomingAcaiaCommand(reinterpret_cast<const uint8_t*>(val.data()), val.size());
  }

  void onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) override {
    DBG_PRINTF("[ACAIA-SRV] >>> APP SUSCRITA (onSubscribe) EN %s (subValue=%u) <<<",
               pCharacteristic->getUUID().toString().c_str(), subValue);
    if (subValue > 0) {
      if (pCharacteristic == pAcaiaIsscNotifyChar1E4D || pCharacteristic == pAcaiaNotifyCharFF11) {
        pActiveAcaiaNotifyChar = pCharacteristic;
      }
      appNotificationsEnabled     = true;
      postBridgeEvent(EVT_WEIGHT_IMM);
    }
  }
#endif
};

static AcaiaServerCallbacks acaiaServerCallbacks;
static AcaiaCharacteristicCallbacks acaiaCharCallbacks;

// ============================================================================
// CONFIGURACIÓN DEL PAQUETE DE ADVERTISING LEGACY (PDU PRIMARIA <= 31 BYTES)
// ============================================================================
/**
 * ESTRUCTURA EXACTA BYTE A BYTE DEL PAQUETE PRIMARIO (ADV_IND - LEGACY PDU):
 * Límite físico estricto de Bluetooth 4.x Legacy Advertising: 31 bytes.
 *
 * MODO A: Acaia Lunar (por defecto, sin EMULATE_BOOKOO) -> Total: 29 bytes (<= 31B)
 * ---------------------------------------------------------------------------------
 * Offset | Bytes | AD Type | Descripción y Contenido Hexadecimal
 * -------|-------|---------|-------------------------------------------------------
 *  0..2  |  3 B  |  0x01   | Flags: [0x02, 0x01, 0x06] (LE General Disc. + BR/EDR Unsup)
 *  3..8  |  6 B  |  0x03   | Complete 16-bit Service UUIDs:
 *        |       |         | [0x05, 0x03, 0x20, 0x18, 0x08, 0xFF] (0x1820 + 0xFF08)
 *  9..14 |  6 B  |  0x16   | Service Data - 16-bit UUID (0x1820 + firma Acaia 0x4333):
 *        |       |         | [0x05, 0x16, 0x20, 0x18, 0x43, 0x33]
 * 15..28 | 14 B  |  0x09   | Complete Local Name ("ACAIA_L_XXXX", 12 chars):
 *        |       |         | [0x0D, 0x09, 'A','C','A','I','A','_','L','_','X','X','X','X']
 * -------|-------|---------|-------------------------------------------------------
 * TOTAL PDU PRIMARIA (ADV_IND) = 3 + 6 + 6 + 14 = 29 bytes (100% dentro de 31 bytes)
 *
 * MODO B: BooKoo Themis Ultra (#define EMULATE_BOOKOO) -> Total: 27 bytes (<= 31B)
 * ---------------------------------------------------------------------------------
 *  0..2  |  3 B  |  0x01   | Flags: [0x02, 0x01, 0x06]
 *  3..6  |  4 B  |  0x03   | Complete 16-bit UUID: [0x03, 0x03, 0xFE, 0x0F] (0x0FFE)
 *  7..26 | 20 B  |  0x09   | Complete Local Name ("BOOKOO_SC_U XXXXXX", 18 chars exactos
 *        |       |         | capturados de Wendougee Data-S / GeeFlow):
 *        |       |         | [0x13, 0x09, 'B','O','O','K','O','O','_','S','C','_','U',' ','X','X','X','X','X','X']
 * -------|-------|---------|-------------------------------------------------------
 * TOTAL PDU PRIMARIA (ADV_IND) = 3 + 4 + 20 = 27 bytes (100% dentro de 31 bytes)
 */
void setupAdvertising() {
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->stop();

  NimBLEAdvertisementData advData;
  NimBLEAdvertisementData scanRespData;

  size_t nameLen = strlen(acaiaDeviceName);
  if (nameLen > 18) nameLen = 18;

  // Construcción determinista byte a byte del paquete ADV_IND primario (<= 31 bytes)
  uint8_t rawAdv[31];
  size_t idx = 0;

  // 1. Flags estándar BLE (3 bytes: 02 01 06)
  rawAdv[idx++] = 0x02;
  rawAdv[idx++] = 0x01;
  rawAdv[idx++] = (BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);

#if defined(EMULATE_BOOKOO)
  // 2. Complete List of 16-bit Service UUIDs (4 bytes: 0x0FFE BooKoo)
  //    NOTA: La báscula BooKoo real anuncia únicamente 0x0FFE (4B) sin 0xFF08 ni
  //    Service Data 0x16, dejando 24 bytes libres para el nombre completo de 18 chars
  //    "BOOKOO_SC_U XXXXXX" (20B) dentro de la PDU primaria de 31 bytes (total 27B).
  rawAdv[idx++] = 0x03;
  rawAdv[idx++] = 0x03;
  rawAdv[idx++] = 0xFE;
  rawAdv[idx++] = 0x0F;
#else
  // 2. Complete List of 16-bit Service UUIDs (6 bytes: 0x1820 IPSP + 0xFF08 Acaia)
  rawAdv[idx++] = 0x05;
  rawAdv[idx++] = 0x03;
  rawAdv[idx++] = 0x20;
  rawAdv[idx++] = 0x18;
  rawAdv[idx++] = 0x08;
  rawAdv[idx++] = 0xFF;

  // 3. Service Data corto de Acaia (6 bytes: 05 16 20 18 43 33)
  rawAdv[idx++] = 0x05;
  rawAdv[idx++] = 0x16;
  rawAdv[idx++] = 0x20;
  rawAdv[idx++] = 0x18;
  rawAdv[idx++] = 0x43;
  rawAdv[idx++] = 0x33;
#endif

  // 4. Complete Local Name (AD Type 0x09) en el paquete primario:
  //    - "ACAIA_L_XXXX"       (14B) -> Total ADV_IND = 29 bytes <= 31B
  //    - "BOOKOO_SC_U XXXXXX" (20B) -> Total ADV_IND = 27 bytes <= 31B
  rawAdv[idx++] = static_cast<uint8_t>(1 + nameLen);
  rawAdv[idx++] = 0x09;
  memcpy(&rawAdv[idx], acaiaDeviceName, nameLen);
  idx += nameLen;

#if defined(NIMBLE_V2)
  advData.addData(rawAdv, idx);
#else
  advData.addData(reinterpret_cast<char*>(rawAdv), idx);
#endif

  // 5. Scan Response PDU (<= 31B):
#if defined(EMULATE_BOOKOO)
  // Construcción determinista byte a byte del SCAN_RSP (24 bytes <= 31B) para
  // el escáner BLE interno de la máquina Wendougee Data-S / Lita y de1app:
  uint8_t rawScan[31];
  size_t scanIdx = 0;
  // Complete Local Name (0x09, 20B)
  rawScan[scanIdx++] = static_cast<uint8_t>(1 + nameLen);
  rawScan[scanIdx++] = 0x09;
  memcpy(&rawScan[scanIdx], acaiaDeviceName, nameLen);
  scanIdx += nameLen;
  // Complete 16-bit Service UUID 0x0FFE (4B: 03 03 FE 0F)
  rawScan[scanIdx++] = 0x03;
  rawScan[scanIdx++] = 0x03;
  rawScan[scanIdx++] = 0xFE;
  rawScan[scanIdx++] = 0x0F;
  #if defined(NIMBLE_V2)
  scanRespData.addData(rawScan, scanIdx);
  #else
  scanRespData.addData(reinterpret_cast<char*>(rawScan), scanIdx);
  #endif
#else
  // Expone el UUID de 128 bits ISSC (18B) + Service Data 0x4333 (6B) = 24 bytes <= 31B
  scanRespData.addServiceUUID(NimBLEUUID(UUID128_ACAIA_ISSC_SVC));
  uint64_t efuseMac = ESP.getEfuseMac();
  const uint8_t scanSvcData4333[2] = {
    static_cast<uint8_t>((efuseMac >> 32) & 0xFF),
    static_cast<uint8_t>((efuseMac >> 40) & 0xFF)
  };
  scanRespData.setServiceData(
      NimBLEUUID(UUID16_ACAIA_SVC_DATA_4333),
      std::string(reinterpret_cast<const char*>(scanSvcData4333), sizeof(scanSvcData4333))
  );
#endif

  // 6. Intervalos de publicidad rápidos: 20 ms a 40 ms (unidades de 0.625 ms: 32 a 64)
  pAdvertising->setMinInterval(32); // 32 * 0.625 ms = 20.0 ms
  pAdvertising->setMaxInterval(64); // 64 * 0.625 ms = 40.0 ms

  pAdvertising->setAdvertisementData(advData);
  pAdvertising->setScanResponseData(scanRespData);

#if defined(NIMBLE_V2)
  pAdvertising->enableScanResponse(true);
#else
  pAdvertising->setScanResponse(true);
#endif

  pAdvertising->start();
  DBG_PRINTF("[ACAIA-SRV] Legacy Advertising activo (20-40ms) como '%s' [PDU Primaria = %u/31 bytes].",
             acaiaDeviceName, (unsigned)idx);
}

// ============================================================================
// INICIALIZACIÓN DEL SERVIDOR BLE ACAIA LUNAR / BOOKOO THEMIS
// ============================================================================
void setupAcaiaServer() {
  pAcaiaServer = NimBLEDevice::createServer();
  pAcaiaServer->setCallbacks(&acaiaServerCallbacks);
  pAcaiaServer->advertiseOnDisconnect(true);

  // Paquete inicial válido de 13 bytes (0.0g) con longitud 0x08 y checksum 0x09, 0x05:
  const uint8_t initialWeightFrame[13] = {
    0xEF, 0xDD, 0x0C, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x09, 0x05
  };

  // Paquete inicial válido de 20 bytes BooKoo Themis (0.00g, unidad=0x01 Gram):
  const uint8_t initialBookooFrame[20] = {
    0x03, 0x0B, 0x00, 0x00, 0x00, 0x01, 0x2B, 0x00, 0x00, 0x00,
    0x2B, 0x00, 0x00, 0x5A, 0x00, 0x64, 0x01, 0x01, 0x00, 0x37
  };

  // 1. Servicio de Información del Dispositivo (0x180A)
  //    Requerido por apps de máquinas de espresso (Wendougee E-Bar / GeeFlow) que validan
  //    la autenticidad del hardware leyendo Model Number (0x2A24) y Manufacturer (0x2A29)
  NimBLEService* pDevInfoService = pAcaiaServer->createService(NimBLEUUID(UUID16_DEVINFO_SVC_180A));

  NimBLECharacteristic* pModelChar = pDevInfoService->createCharacteristic(
      NimBLEUUID(UUID16_DEVINFO_MODEL_2A24),
      NIMBLE_PROPERTY::READ
  );
  NimBLECharacteristic* pManufChar = pDevInfoService->createCharacteristic(
      NimBLEUUID(UUID16_DEVINFO_MANUF_2A29),
      NIMBLE_PROPERTY::READ
  );
  NimBLECharacteristic* pFwRevChar = pDevInfoService->createCharacteristic(
      NimBLEUUID(UUID16_DEVINFO_FW_2A26),
      NIMBLE_PROPERTY::READ
  );

#if defined(EMULATE_BOOKOO)
  pModelChar->setValue("Themis");
  pManufChar->setValue("BOOKOO");
#else
  pModelChar->setValue("LUNAR");
  pManufChar->setValue("Acaia");
#endif
  pFwRevChar->setValue("2.0.08");

#if defined(EMULATE_BOOKOO)
  // 2. Servicio Nativo BooKoo Themis (0x0FFE -> Notify 0xFF11, Write 0xFF12)
  //    En modo EMULATE_BOOKOO se instancia exclusivamente 0x0FFE para que el UUID 0xFF11
  //    sea único en toda la tabla GATT y de1app / Wendougee se suscriban sin ambigüedad.
  NimBLEService* pBookooService = pAcaiaServer->createService(NimBLEUUID(UUID16_BOOKOO_SVC_0FFE));
  pBookooNotifyCharFF11 = pBookooService->createCharacteristic(
      NimBLEUUID(UUID16_BOOKOO_CHR_FF11),
      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
  );
  pBookooNotifyCharFF11->setValue(initialBookooFrame, sizeof(initialBookooFrame));
  pBookooNotifyCharFF11->setCallbacks(&acaiaCharCallbacks);

  pBookooWriteCharFF12 = pBookooService->createCharacteristic(
      NimBLEUUID(UUID16_BOOKOO_CHR_FF12),
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
  );
  pBookooWriteCharFF12->setCallbacks(&acaiaCharCallbacks);
#else
  // 2. Servicio Primario Acaia 0xFF08 (16-bit UUID estricto de la especificación)
  NimBLEService* pPrimaryService = pAcaiaServer->createService(NimBLEUUID(UUID16_ACAIA_SVC_FF08));

  pAcaiaWriteCharFF10 = pPrimaryService->createCharacteristic(
      NimBLEUUID(UUID16_ACAIA_CHR_FF10),
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pAcaiaWriteCharFF10->setCallbacks(&acaiaCharCallbacks);

  pAcaiaNotifyCharFF11 = pPrimaryService->createCharacteristic(
      NimBLEUUID(UUID16_ACAIA_CHR_FF11),
      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
  );
  pAcaiaNotifyCharFF11->setValue(initialWeightFrame, sizeof(initialWeightFrame));
  pAcaiaNotifyCharFF11->setCallbacks(&acaiaCharCallbacks);

  // 3. Servicio Acaia Lunar 2021 / Pyxis (ISSC Transparent UART: 49535343-fe7d-...)
  //    NOTA CRÍTICA: No exponer el característica GATT viejo 0x2A80 dentro de 0x1820,
  //    porque librerías como AcaiaArduinoBLE verifican 0x2A80 primero y entran en
  //    modo "OLD (10 bytes)" ignorando las tramas de 13 bytes de la Lunar 2021.
  NimBLEService* pIsscService = pAcaiaServer->createService(NimBLEUUID(UUID128_ACAIA_ISSC_SVC));
  pAcaiaIsscNotifyChar1E4D = pIsscService->createCharacteristic(
      NimBLEUUID(UUID128_ACAIA_ISSC_CHR_NOTIFY),
      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
  );
  pAcaiaIsscNotifyChar1E4D->setValue(initialWeightFrame, sizeof(initialWeightFrame));
  pAcaiaIsscNotifyChar1E4D->setCallbacks(&acaiaCharCallbacks);

  pAcaiaIsscWriteChar8841 = pIsscService->createCharacteristic(
      NimBLEUUID(UUID128_ACAIA_ISSC_CHR_WRITE),
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pAcaiaIsscWriteChar8841->setCallbacks(&acaiaCharCallbacks);
#endif

#if defined(NIMBLE_V2)
  pAcaiaServer->start();
#else
  pDevInfoService->start();
  #if defined(EMULATE_BOOKOO)
  pBookooService->start();
  #else
  pPrimaryService->start();
  pIsscService->start();
  #endif
#endif

  // 5. Configurar e iniciar el paquete de publicidad Legacy (<= 31 bytes)
  setupAdvertising();
}

// ============================================================================
// CALLBACKS Y LÓGICA DEL CLIENTE BLE (TIMEMORE DOT)
// ============================================================================
class TimemoreClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    DBG_PRINTLN("[TIMEMORE-CLI] Enlace BLE a nivel GAP establecido con la balanza.");
  }

#if defined(NIMBLE_V2)
  void onPhyUpdate(NimBLEClient* pClient, uint8_t txPhy, uint8_t rxPhy) override {
    DBG_PRINTF("[TIMEMORE-CLI] PHY enlace Timemore: TX=%uM RX=%uM", txPhy, rxPhy);
  }

  void onConnectFail(NimBLEClient* pClient, int reason) override {
    DBG_PRINTF("[TIMEMORE-CLI] Callback onConnectFail (razon=%d / 0x%03X).", reason, reason);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    DBG_PRINTF("[TIMEMORE-CLI] SMP Security/Bonding completado (encrypted=%d, bonded=%d).",
               connInfo.isEncrypted() ? 1 : 0, connInfo.isBonded() ? 1 : 0);
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    scaleConnected          = false;
    pScaleNotifyChar        = nullptr;
    pScaleWriteChar         = nullptr;
    candidateFound          = false;
    doConnectToScale        = false;
    lastScanOrConnAttemptMs = 0; // Forzar reconexión/escaneo inmediato en scale_link
    scaleLinkTuned          = true;
    postScaleEvent(SCL_EVT_WAKE);
    DBG_PRINTF("[TIMEMORE-CLI] Balanza Timemore desconectada (razon=%d). Buscando reconexion inmediata...",
               reason);
  }
#else
  void onDisconnect(NimBLEClient* pClient) override {
    scaleConnected          = false;
    pScaleNotifyChar        = nullptr;
    pScaleWriteChar         = nullptr;
    candidateFound          = false;
    doConnectToScale        = false;
    lastScanOrConnAttemptMs = 0;
    scaleLinkTuned          = true;
    postScaleEvent(SCL_EVT_WAKE);
    DBG_PRINTLN("[TIMEMORE-CLI] Balanza Timemore desconectada. Buscando reconexion inmediata...");
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    scaleConnected          = false;
    pScaleNotifyChar        = nullptr;
    pScaleWriteChar         = nullptr;
    candidateFound          = false;
    doConnectToScale        = false;
    lastScanOrConnAttemptMs = 0;
    scaleLinkTuned          = true;
    postScaleEvent(SCL_EVT_WAKE);
    DBG_PRINTF("[TIMEMORE-CLI] Balanza Timemore desconectada (razon=%d). Buscando reconexion inmediata...",
               reason);
  }
#endif
};

static TimemoreClientCallbacks timemoreClientCallbacks;

bool isTimemoreAdvertisedDevice(const NimBLEAdvertisedDevice* dev) {
  if (dev == nullptr) return false;

  // 1. Si hay MAC guardada en NVS, verificar coincidencia directa de MAC
  //    NOTA CRÍTICA: Si la MAC no coincide, NO retornar false; continuar evaluando
  //    UUIDs y Nombre por si la balanza rotó su dirección Random al reencender.
  if (hasSavedMac && savedTimemoreMac[0] != '\0') {
    String devMac = String(dev->getAddress().toString().c_str());
    devMac.toLowerCase();
    if (devMac.equals(savedTimemoreMac)) {
      return true;
    }
  }

  // 2. Filtro por Service UUID de Timemore en lista de servicios (0xFFE0, 0xFFF0, 0x181D)
  if (dev->isAdvertisingService(NimBLEUUID(UUID16_TIMEMORE_SVC_FFE0)) ||
      dev->isAdvertisingService(NimBLEUUID(UUID16_TIMEMORE_SVC_FFF0)) ||
      dev->isAdvertisingService(NimBLEUUID(UUID16_TIMEMORE_SVC_181D))) {
    return true;
  }

  // 3. Filtro por Service Data UUID (AD Type 0x16) usado en encendidos normales fuera de modo pairing
  if (dev->haveServiceData()) {
    uint8_t svcDataCount = dev->getServiceDataCount();
    for (uint8_t i = 0; i < svcDataCount; i++) {
      NimBLEUUID sdUuid = dev->getServiceDataUUID(i);
      if (sdUuid == NimBLEUUID(UUID16_TIMEMORE_SVC_FFE0) ||
          sdUuid == NimBLEUUID(UUID16_TIMEMORE_SVC_FFF0) ||
          sdUuid == NimBLEUUID(UUID16_TIMEMORE_SVC_181D)) {
        return true;
      }
    }
  }

  // 4. Filtro por subcadena de nombre: "TIMEMORE", "DOT", "MACARON", "BLACK MIRROR"
  if (dev->haveName()) {
    String upperName = String(dev->getName().c_str());
    upperName.toUpperCase();
    if (upperName.indexOf("TIMEMORE") >= 0 ||
        upperName.indexOf("DOT") >= 0 ||
        upperName.indexOf("MACARON") >= 0 ||
        upperName.indexOf("BLACK MIRROR") >= 0) {
      return true;
    }
  }

  return false;
}

void evaluateScannedDevice(const NimBLEAdvertisedDevice* advertisedDevice) {
  if (scaleConnected || doConnectToScale) return;
  if (!isTimemoreAdvertisedDevice(advertisedDevice)) return;

  int rssi            = advertisedDevice->getRSSI();
  std::string addrStr = advertisedDevice->getAddress().toString();
  uint8_t addrType    = advertisedDevice->getAddress().getType();
  uint8_t advType     = advertisedDevice->getAdvType();
  bool connectable    = advertisedDevice->isConnectable();
  std::string nameStr = advertisedDevice->haveName() ? advertisedDevice->getName() : "(Sin Nombre)";

  DBG_PRINTF("[SCAN] Balanza Timemore detectada: Nombre='%s' | MAC=%s (tipo=%u) | advType=%u (conn=%d) | RSSI=%d dBm",
             nameStr.c_str(), addrStr.c_str(), addrType, advType, connectable ? 1 : 0, rssi);

#if defined(NIMBLE_V2) && ENABLE_SERIAL_DEBUG
  const std::vector<uint8_t>& rawAdv = advertisedDevice->getPayload();
  if (!rawAdv.empty()) {
    Serial.printf("[%8lu ms] [SCAN-RAW] (%u bytes): ", millis(), (unsigned)rawAdv.size());
    for (size_t i = 0; i < rawAdv.size(); i++) {
      Serial.printf("%02X ", rawAdv[i]);
    }
    Serial.println();
  }
#endif

  // Si el paquete de anuncio es un beacon NO conectable (ADV_NONCONN_IND=3 o SCAN_IND=2),
  // esperar a un paquete conectable (ADV_IND=0 o DIR_IND=1) para no bloquear 4s en connect()
  if (!connectable) {
    DBG_PRINTLN("[SCAN] Paquete actual es no-conectable (beacon/standby). Esperando trama conectable ADV_IND...");
    return;
  }

  strncpy(candidateMac, addrStr.c_str(), sizeof(candidateMac) - 1);
  candidateMac[sizeof(candidateMac) - 1] = '\0';
  candidateAddrType = addrType;
  candidateRSSI     = rssi;
  strncpy(candidateName, nameStr.c_str(), sizeof(candidateName) - 1);
  candidateName[sizeof(candidateName) - 1] = '\0';
  candidateFound    = true;
  doConnectToScale  = true;

  // Detener escaneo inmediatamente desde scale_link para conectar en el acto
  if (NimBLEDevice::getScan()->isScanning()) {
    pendingStopScan = true;
  }
  postScaleEvent(SCL_EVT_WAKE);
}

#if defined(NIMBLE_V2)
class TimemoreScanCallbacks : public NimBLEScanCallbacks {
  // Usamos onResult() junto con setScanResponseTimeout(150) para NO interrumpir
  // el intercambio SCAN_REQ -> SCAN_RSP en el aire:
  // - Si la balanza responde SCAN_RSP (con su nombre "TIMEMORE..."), onResult()
  //   se dispara en ~1-2 ms con el paquete completo.
  // - Si la balanza no responde SCAN_RSP, onResult() se dispara a los 150 ms
  //   (dejando ~850 ms libres antes del siguiente ADV_IND de 1000 ms).
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    evaluateScannedDevice(advertisedDevice);
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    isScanning = false;
    lastScanOrConnAttemptMs = millis();
    if (candidateFound && !scaleConnected) {
      doConnectToScale = true;
    }
    postScaleEvent(SCL_EVT_WAKE);
  }
};
#else
class TimemoreScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
    evaluateScannedDevice(advertisedDevice);
  }
};

void onScanEndedCallbackV1(NimBLEScanResults results) {
  isScanning = false;
  lastScanOrConnAttemptMs = millis();
  if (candidateFound && !scaleConnected) {
    doConnectToScale = true;
  }
  postScaleEvent(SCL_EVT_WAKE);
}
#endif

static TimemoreScanCallbacks timemoreScanCallbacks;

// ============================================================================
// GESTIÓN DE MEMORIA NVS (Preferences.h)
// ============================================================================
void loadSavedScaleFromNVS() {
  preferences.begin(NVS_NAMESPACE, true);
  String mac = preferences.getString(NVS_KEY_MAC, "");
  savedTimemoreAddrType = preferences.getUChar(NVS_KEY_TYPE, 0);
  preferences.end();

  mac.toLowerCase();
  if (mac.length() == 17) {
    strncpy(savedTimemoreMac, mac.c_str(), sizeof(savedTimemoreMac) - 1);
    savedTimemoreMac[sizeof(savedTimemoreMac) - 1] = '\0';
    hasSavedMac = true;
    DBG_PRINTF("[NVS] MAC Timemore cargada desde Flash NVS: %s (tipo=%u)",
               savedTimemoreMac, savedTimemoreAddrType);
  } else {
    savedTimemoreMac[0] = '\0';
    hasSavedMac = false;
    DBG_PRINTLN("[NVS] Sin MAC guardada en Flash. Modo Auto-Discovery por mejor RSSI activado.");
  }
}

void saveScaleToNVS(const char* macStr, uint8_t addrType) {
  if (macStr == nullptr || strlen(macStr) != 17) return;

  String lowerMac = String(macStr);
  lowerMac.toLowerCase();

  if (hasSavedMac && lowerMac.equals(savedTimemoreMac) && savedTimemoreAddrType == addrType) {
    return;
  }

  preferences.begin(NVS_NAMESPACE, false);
  preferences.putString(NVS_KEY_MAC, lowerMac);
  preferences.putUChar(NVS_KEY_TYPE, addrType);
  preferences.end();

  strncpy(savedTimemoreMac, lowerMac.c_str(), sizeof(savedTimemoreMac) - 1);
  savedTimemoreMac[sizeof(savedTimemoreMac) - 1] = '\0';
  savedTimemoreAddrType = addrType;
  hasSavedMac = true;

  DBG_PRINTF("[NVS] MAC guardada en Flash NVS: %s (tipo=%u)", savedTimemoreMac, savedTimemoreAddrType);
}

void clearSavedScaleFromNVS() {
  DBG_PRINTLN("==============================================================");
  DBG_PRINTLN("[RESET] Borrando MAC guardada en NVS y reiniciando busqueda...");
  DBG_PRINTLN("==============================================================");

  preferences.begin(NVS_NAMESPACE, false);
  preferences.clear();
  preferences.end();

  savedTimemoreMac[0]     = '\0';
  hasSavedMac             = false;
  candidateFound          = false;
  candidateRSSI           = -999;
  doConnectToScale        = false;
  lastScanOrConnAttemptMs = 0;

#if defined(NIMBLE_V2)
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::deleteAllBonds();
    if (isScanning) {
      NimBLEDevice::getScan()->stop();
      isScanning = false;
    }
  }
#else
  if (NimBLEDevice::getInitialized()) {
    NimBLEDevice::deleteAllBonds();
    if (isScanning) {
      NimBLEDevice::getScan()->stop();
      isScanning = false;
    }
  }
#endif

  if (pScaleClient != nullptr && pScaleClient->isConnected()) {
    pScaleClient->disconnect();
  }
  scaleConnected = false;

  blinkResetConfirmation();
  DBG_PRINTLN("[RESET] NVS y Bonds BLE limpios. Escaneando cualquier Timemore Dot cercana por mejor RSSI...");
}

// ============================================================================
// ESCANEO Y CONEXIÓN A LA BALANZA TIMEMORE DOT
// ============================================================================
static bool tryDirectConnectNext = false;

void startTimemoreScan() {
  if (scaleConnected || isScanning || doConnectToScale) return;

  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan->isScanning()) {
    isScanning = true;
    return;
  }

  // Ajustar duty-cycle dinámicamente:
  // - Si la App NO está conectada: escaneo rápido (ventana 55ms cada 85ms = 65% duty cycle)
  //   para atrapar a la Timemore Dot al instante apenas encienda o tras un reset.
  // - Si la App SÍ está conectada: escaneo moderado (ventana 45ms cada 160ms = 28% duty cycle)
  //   para no afectar el streaming de 10 Hz ni los Heartbeats de Acaia.
  if (!appConnected) {
    pScan->setInterval(85);
    pScan->setWindow(55);
  } else {
    pScan->setInterval(160);
    pScan->setWindow(45);
  }

  pScan->clearResults();
  candidateFound  = false;
  candidateRSSI   = -999;
  pendingStopScan = false;
  isScanning      = true;

  if (hasSavedMac) {
    DBG_PRINTF("[SCAN] Buscando Timemore Dot (%s) + auto-deteccion activa (%ds)...",
               savedTimemoreMac, SCAN_WINDOW_SECONDS);
  } else {
    DBG_PRINTF("[SCAN] Escaneando balanzas Timemore cercanas (ventana %ds)...", SCAN_WINDOW_SECONDS);
  }

#if defined(NIMBLE_V2)
  if (!pScan->start(SCAN_WINDOW_SECONDS * 1000, false, true)) {
    isScanning = false;
  }
#else
  if (!pScan->start(SCAN_WINDOW_SECONDS, onScanEndedCallbackV1, false)) {
    isScanning = false;
  }
#endif
}

bool discoverAndSubscribeTimemore(NimBLEClient* pClient) {
  pScaleNotifyChar    = nullptr;
  pScaleWriteChar     = nullptr;
  scaleUsesDotService = false;

  // 1. Servicio de la Timemore Dot 0xFFF0 -> 0xFFF1 (Notify) / 0xFFF2 (Write)
  //    Es el que usa de1app (verificado con hardware real), por eso va primero.
  NimBLERemoteService* pSvc = pClient->getService(NimBLEUUID(UUID16_TIMEMORE_SVC_FFF0));
  if (pSvc != nullptr) {
    DBG_PRINTLN("[TIMEMORE-CLI] Servicio Timemore Dot 0xFFF0 descubierto.");
    pScaleNotifyChar = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_FFF1));
    pScaleWriteChar  = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_FFF2));
    scaleUsesDotService = (pScaleNotifyChar != nullptr);
  }

  // 2. Fallback: variante UART 0xFFE0 -> 0xFFE1 / 0xFFE2
  if (pScaleNotifyChar == nullptr) {
    pSvc = pClient->getService(NimBLEUUID(UUID16_TIMEMORE_SVC_FFE0));
    if (pSvc != nullptr) {
      DBG_PRINTLN("[TIMEMORE-CLI] Servicio UART 0xFFE0 descubierto.");
      pScaleNotifyChar = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_FFE1));
      pScaleWriteChar  = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_FFE2));
      if (pScaleWriteChar == nullptr && pScaleNotifyChar != nullptr &&
          (pScaleNotifyChar->canWrite() || pScaleNotifyChar->canWriteNoResponse())) {
        pScaleWriteChar = pScaleNotifyChar;
      }
    }
  }

  // 3. Fallback Servicio Weight Scale 0x181D -> 0x2A9D
  if (pScaleNotifyChar == nullptr) {
    pSvc = pClient->getService(NimBLEUUID(UUID16_TIMEMORE_SVC_181D));
    if (pSvc != nullptr) {
      DBG_PRINTLN("[TIMEMORE-CLI] Servicio Weight Scale 0x181D descubierto.");
      pScaleNotifyChar = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_2A9D));
      pScaleWriteChar  = pScaleNotifyChar;
    }
  }

  if (pScaleNotifyChar == nullptr) {
    DBG_PRINTLN("[TIMEMORE-CLI] ERROR: No se encontraron las caracteristicas 0xFFE1 / 0xFFF1 / 0x2A9D.");
    return false;
  }

  if (pScaleNotifyChar->canNotify()) {
    if (!pScaleNotifyChar->subscribe(true, onTimemoreNotifyCallback)) {
      DBG_PRINTLN("[TIMEMORE-CLI] ERROR: Fallo al suscribir notificaciones CCCD.");
      return false;
    }
    DBG_PRINTF("[TIMEMORE-CLI] Suscrito exitosamente a notificaciones en %s.",
               pScaleNotifyChar->getUUID().toString().c_str());
  } else if (pScaleNotifyChar->canIndicate()) {
    if (!pScaleNotifyChar->subscribe(false, onTimemoreNotifyCallback)) {
      DBG_PRINTLN("[TIMEMORE-CLI] ERROR: Fallo al suscribir indicaciones.");
      return false;
    }
  }

  return true;
}

bool connectToTimemoreScale(const char* macStr, uint8_t addrType, uint32_t timeoutMs = 4500, bool tryAltAddrType = false) {
  if (macStr == nullptr || strlen(macStr) != 17) return false;

  DBG_PRINTF("[TIMEMORE-CLI] Conectando a Timemore Dot [%s] (tipo=%u, timeout=%lu ms)...",
             macStr, addrType, (unsigned long)timeoutMs);

  pendingStopScan = false;
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan->isScanning()) {
    pScan->stop();
  }
  isScanning = false;

  // Si la App aún no está conectada, pausar temporalmente el Advertising durante
  // el intento de conexión saliente para evitar colisiones de radio con CONNECT_IND.
  if (!appConnected) {
    NimBLEDevice::stopAdvertising();
  }

  // Pausa de estabilización de 60 ms tras detener el escáner antes de llamar a connect()
  // (evita el error 0x3E / 574 por condición de carrera en el cambio de estado del radio)
  delay(60);

  if (pScaleClient == nullptr) {
    pScaleClient = NimBLEDevice::createClient();
    pScaleClient->setClientCallbacks(&timemoreClientCallbacks, false);
    // Usar los parámetros iniciales probados con el SoC Phyplus PHY6222 de la Timemore Dot:
    // intervalo 30-50ms (24..40 * 1.25ms), latencia 0, supervisión 4000ms (400 * 10ms),
    // ventana de iniciador 16/16 (10ms/10ms)
    pScaleClient->setConnectionParams(24, 40, 0, 400, 16, 16);
#if defined(NIMBLE_V2) && defined(CONFIG_BT_NIMBLE_EXT_ADV)
    // Forzar exclusivamente 1M PHY para no dividir el tiempo del iniciador con Coded PHY
    pScaleClient->setConnectPhy(BLE_GAP_LE_PHY_1M_MASK);
#endif
  }

  if (pScaleClient->isConnected()) {
    pScaleClient->disconnect();
    delay(30);
  }

#if defined(NIMBLE_V2)
  pScaleClient->setConnectTimeout(timeoutMs);
#else
  uint32_t timeoutSec = (timeoutMs + 999) / 1000;
  if (timeoutSec < 1) timeoutSec = 1;
  pScaleClient->setConnectTimeout(timeoutSec);
#endif

  uint8_t connectedAddrType = addrType;
  NimBLEAddress targetAddr(std::string(macStr), addrType);
#if defined(NIMBLE_V2)
  // exchangeMTU = false durante el establecimiento inicial del enlace para no inyectar
  // paquetes ATT_EXCHANGE_MTU_REQ en los primeros 6 eventos de conexión (evita 0x3E / 574)
  bool connected = pScaleClient->connect(targetAddr, true, false, false);
#else
  bool connected = pScaleClient->connect(targetAddr, true);
#endif

  // Si falla con el addrType inicial y se pidió probar el alternativo (en arranque directo)
  if (!connected && tryAltAddrType) {
    uint8_t altType = (addrType == 0) ? 1 : 0;
    DBG_PRINTF("[TIMEMORE-CLI] Fallo con addrType=%u (err=%d). Reintentando [%s] con addrType=%u...",
               addrType, pScaleClient->getLastError(), macStr, altType);
#if defined(NIMBLE_V2)
    pScaleClient->setConnectTimeout(2000);
#else
    pScaleClient->setConnectTimeout(2);
#endif
    NimBLEAddress altAddr(std::string(macStr), altType);
#if defined(NIMBLE_V2)
    if (pScaleClient->connect(altAddr, true, false, false)) {
#else
    if (pScaleClient->connect(altAddr, true)) {
#endif
      connected = true;
      connectedAddrType = altType;
    }
  }

  if (!connected) {
    int lastErr = pScaleClient->getLastError();
    DBG_PRINTF("[TIMEMORE-CLI] Conexion no establecida (err=%d / 0x%03X).", lastErr, lastErr);
    if (lastErr == 574) {
      DBG_PRINTLN("[TIMEMORE-CLI] NOTA (err=574 / 0x3E): La balanza emitio ADV_IND pero ignoro CONNECT_IND (White List activa).");
      DBG_PRINTLN("[TIMEMORE-CLI] -> Pon la Timemore Dot en MODO EMPAREJAMIENTO una vez para registrar el Bond BLE en su White List.");
    }
    if (!appConnected) {
      NimBLEDevice::startAdvertising();
    }
    return false;
  }

  if (!discoverAndSubscribeTimemore(pScaleClient)) {
    pScaleClient->disconnect();
    if (!appConnected) {
      NimBLEDevice::startAdvertising();
    }
    return false;
  }

#if defined(NIMBLE_V2)
  // IMPORTANTE: Iniciar negociación SMP Bonding DESPUÉS de completar el descubrimiento GATT
  // (para no colisionar con m_pTaskData) de modo que el SoC Phyplus (GAPBondMgr) de la
  // Timemore Dot guarde la MAC del ESP32-S3 en su White List (GAPBOND_AUTO_SYNC_WL)
  pScaleClient->secureConnection(true);
#endif

  // El intervalo corto (15-20 ms) y PHY 2M se piden en scale_link tras
  // SCALE_LINK_TUNE_DELAY_MS, cuando el cifrado SMP ya terminó (evita colisión de
  // procedimientos LL con el SoC PHY6222).
  scaleConnectedAtMs = millis();
  scaleLinkTuned     = false;

  softwareTareOffset = 0.0f;
  flowResetRequested = true;
  lastKeepAliveMs    = millis();
  scaleConnected     = true;

  saveScaleToNVS(macStr, connectedAddrType);

  // Reanudar Advertising ANTES de la secuencia de inicio (~2.5 s), para que la
  // máquina/App pueda descubrir el puente mientras tanto.
  if (!appConnected) {
    NimBLEDevice::startAdvertising();
  }

  // Volver a capturar las primeras tramas de peso de esta conexión
  dbgFrameCount    = 0;
  dbgFramesPrinted = 0;

  if (scaleUsesDotService) {
    // Secuencia de la app oficial + sondeo inicial (igual que de1app)
    sendTimemoreDotInitSequence();
    sendTimemoreKeepAlive();
  } else {
    // Otras variantes: solicitud de peso inicial + sondeo genérico
    sendTimemoreOpenScaleFrame(0x02, 0x01, nullptr, 0);
    sendTimemoreKeepAlive();
  }
  lastKeepAliveMs = millis();

  DBG_PRINTF("[TIMEMORE-CLI] >>> CONEXION EXITOSA con Timemore Dot (%s, tipo=%u) <<<",
             macStr, connectedAddrType);
  return true;
}

// ============================================================================
// BOTÓN BOOT (GPIO 0) Y COMANDOS INTERACTIVOS POR MONITOR SERIAL
// ============================================================================
void checkBootButtonAtStartup() {
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    DBG_PRINTLN("[BOOT] Boton GPIO 0 presionado al encender. Manten 3 segundos para borrar MAC NVS...");
    unsigned long startHold = millis();
    while (digitalRead(PIN_BOOT_BUTTON) == LOW) {
      unsigned long elapsed = millis() - startHold;
      if (((elapsed / 150) % 2) == 0) {
        writeRgbPixelSafe(255, 0, 120);
      } else {
        writeRgbPixelSafe(0, 0, 0);
      }
      if (elapsed >= NVS_RESET_HOLD_MS) {
        clearSavedScaleFromNVS();
        while (digitalRead(PIN_BOOT_BUTTON) == LOW) {
          delay(20);
        }
        return;
      }
      delay(20);
    }
  }
}

void pollBootButtonRuntime() {
  bool isLow = (digitalRead(PIN_BOOT_BUTTON) == LOW);
  unsigned long now = millis();

  if (isLow) {
    if (!bootBtnPressed) {
      bootBtnPressed          = true;
      bootBtnLongPressHandled = false;
      bootBtnPressStartMs     = now;
    } else if (!bootBtnLongPressHandled && (now - bootBtnPressStartMs >= NVS_RESET_HOLD_MS)) {
      bootBtnLongPressHandled = true;
      postScaleEvent(SCL_EVT_NVS_RESET); // Lo ejecuta scale_link (toca escáner/cliente BLE)
    }
  } else {
    if (bootBtnPressed) {
      unsigned long pressDuration = now - bootBtnPressStartMs;
      if (!bootBtnLongPressHandled && pressDuration >= 50 && pressDuration < 1200) {
        DBG_PRINTLN("[BOOT] Pulsacion corta en GPIO 0 -> Ejecutando TARA manual.");
        postBridgeEvent(EVT_TARE);
      }
      bootBtnPressed          = false;
      bootBtnLongPressHandled = false;
    }
  }
}

void printSerialStatusSummary() {
  DBG_PRINTF("[STATUS] Dot=%s (%s) | AppAcaia=%s (%s) | Peso=%+6.1fg | Bat=%u%% | Rx=%lu Tx=%lu | Heap=%u B",
             scaleConnected ? "CONECTADA" : (isScanning ? "ESCANEANDO" : "DESCONECTADA"),
             hasSavedMac ? savedTimemoreMac : "Auto-RSSI",
             appConnected ? "CONECTADA" : "ESPERANDO",
             acaiaDeviceName,
             currentWeightGrams,
             scaleBatteryPct,
             (unsigned long)timemorePacketsRxCount,
             (unsigned long)acaiaPacketsTxCount,
             (unsigned)ESP.getFreeHeap());
}

/**
 * Parámetros reales negociados de ambos enlaces (lo que el otro extremo aceptó).
 * Se ejecuta en loop (core 1, prioridad 1): no toca el camino crítico.
 */
void printLinkInfo() {
#if defined(NIMBLE_V2)
  char dotBuf[64] = "--";
  char appBuf[72] = "--";
  if (scaleConnected && pScaleClient != nullptr) {
    NimBLEConnInfo ci = pScaleClient->getConnInfo();
    uint8_t tx = 0, rx = 0;
    pScaleClient->getPhy(&tx, &rx);
    snprintf(dotBuf, sizeof(dotBuf), "itvl=%.2fms lat=%u phy=%u/%uM",
             ci.getConnInterval() * 1.25f, ci.getConnLatency(), tx, rx);
  }
  uint16_t h = appConnHandle;
  if (appConnected && h != 0xFFFF && pAcaiaServer != nullptr) {
    NimBLEConnInfo ci = pAcaiaServer->getPeerInfoByHandle(h);
    uint8_t tx = 0, rx = 0;
    pAcaiaServer->getPhy(h, &tx, &rx);
    snprintf(appBuf, sizeof(appBuf), "itvl=%.2fms lat=%u phy=%u/%uM mtu=%u",
             ci.getConnInterval() * 1.25f, ci.getConnLatency(), tx, rx, ci.getMTU());
  }
  DBG_PRINTF("[LINK] Dot: %s | App: %s", dotBuf, appBuf);
#endif
}

/**
 * Copia y reinicia las estadísticas de latencia (sección crítica de ~100 ns) e
 * imprime el resumen de la ventana. e2e = llegada de la muestra al ESP32 ->
 * notificación entregada al stack BLE (no incluye el tiempo en el aire).
 */
void printLatencyStats() {
#if ENABLE_LATENCY_STATS
  LatencyStats s;
  portENTER_CRITICAL(&statsMux);
  s = latStats;
  resetLatencyStats(latStats);
  portEXIT_CRITICAL(&statsMux);

  if (s.samples == 0) {
    DBG_PRINTF("[LAT] Sin muestras de la balanza en esta ventana (txFallos total=%lu).",
               (unsigned long)notifyFailTotal);
    return;
  }
  uint32_t wakeAvg = static_cast<uint32_t>(s.wakeSumUs / s.samples);
  uint32_t e2eAvg  = s.e2eCount ? static_cast<uint32_t>(s.e2eSumUs / s.e2eCount) : 0;
  uint32_t dtAvg   = s.dtCount ? static_cast<uint32_t>(s.dtSumMs / s.dtCount) : 0;
  DBG_PRINTF("[LAT] n=%lu (%.1f Hz) pisadas=%lu | dt balanza ms min/avg/max=%lu/%lu/%lu | "
             "despertar us=%lu/%lu/%lu | e2e us=%lu/%lu/%lu",
             (unsigned long)s.samples, s.samples * 1000.0f / SERIAL_STATUS_INTERVAL_MS,
             (unsigned long)s.coalesced,
             (unsigned long)(s.dtCount ? s.dtMinMs : 0), (unsigned long)dtAvg, (unsigned long)s.dtMaxMs,
             (unsigned long)s.wakeMinUs, (unsigned long)wakeAvg, (unsigned long)s.wakeMaxUs,
             (unsigned long)(s.e2eCount ? s.e2eMinUs : 0), (unsigned long)e2eAvg, (unsigned long)s.e2eMaxUs);
  DBG_PRINTF("[LAT] e2e hist <0.25ms:%lu <1ms:%lu <5ms:%lu <15ms:%lu >=15ms:%lu | txPeso=%lu txFallos=%lu",
             (unsigned long)s.hist[0], (unsigned long)s.hist[1], (unsigned long)s.hist[2],
             (unsigned long)s.hist[3], (unsigned long)s.hist[4],
             (unsigned long)s.txWeight, (unsigned long)s.txFail);
#endif
}

void pollSerialDebugCommands() {
#if ENABLE_SERIAL_DEBUG
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '?' || c == 'i' || c == 'I') {
      printSerialStatusSummary();
    } else if (c == 't' || c == 'T') {
      DBG_PRINTLN("[SERIAL-CMD] Comando 't' recibido -> Ejecutando TARA.");
      postBridgeEvent(EVT_TARE);
    } else if (c == 'r' || c == 'R') {
      DBG_PRINTLN("[SERIAL-CMD] Comando 'r' recibido -> Borrando MAC guardada en NVS.");
      postScaleEvent(SCL_EVT_NVS_RESET);
    } else if (c == 's' || c == 'S') {
      DBG_PRINTLN("[SERIAL-CMD] Comando 's' recibido -> Forzando escaneo BLE.");
      postScaleEvent(SCL_EVT_SCAN);
    } else if (c == 'l' || c == 'L') {
      printLinkInfo();
      printLatencyStats();
    } else if (c == 'p' || c == 'P') {
      scanPausedByUser = !scanPausedByUser;
      if (scanPausedByUser) {
        pendingStopScan = true;
        postScaleEvent(SCL_EVT_WAKE);
        DBG_PRINTLN("[SERIAL-CMD] Escaneo de Timemore PAUSADO (100% radio dedicado a la App Acaia).");
      } else {
        DBG_PRINTLN("[SERIAL-CMD] Escaneo de Timemore REANUDADO.");
      }
    }
  }
#endif
}

// ============================================================================
// SETUP PRINCIPAL CON VENTANA USB CDC Y TRAZAS PASO A PASO
// ============================================================================
void bridgeTaskFn(void* arg);
void scaleLinkTaskFn(void* arg);

void setup() {
  Serial.begin(115200);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  // CRÍTICO ESP32-S3 USB CDC: Timeout de TX en 0 ms para que Serial.printf JAMÁS
  // bloquee la CPU ni el stack BLE cuando el Monitor Serie está cerrado o se usa con cargador.
  Serial.setTxTimeoutMs(0);
#endif

  // Espera breve de 2 segundos para estabilizar USB CDC sin retrasar la reconexión BLE
  for (int i = 2; i > 0; i--) {
    Serial.printf("[ARRANQUE] USB Serial activo. Iniciando sistema en %d s... (Ultimo Reset: %s)\r\n",
                  i, getResetReasonString());
    Serial.flush();
    delay(600);
  }

  Serial.println();
  Serial.println("==============================================================");
  Serial.println("  TIMEMORE DOT -> ACAIA LUNAR BLE BRIDGE (ESP32-S3-Zero)");
  Serial.println("==============================================================");
  DBG_PRINTF("[BOOT 1/6] Causa del ultimo reinicio: %s", getResetReasonString());
  DBG_PRINTF("[BOOT 1/6] Memoria RAM libre inicial: %u bytes | NimBLE API: %s",
             (unsigned)ESP.getFreeHeap(),
#if defined(NIMBLE_V2)
             "v2.x"
#else
             "v1.4.x"
#endif
  );
  Serial.flush();

  // Paso 2: Inicializar GPIO 0 (BOOT) y LED WS2812
  DBG_PRINTLN("[BOOT 2/6] Inicializando boton BOOT (GPIO 0) y LED RGB (GPIO 21)...");
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  initRgbLed();
  writeRgbPixelSafe(0, 0, 50);
  checkBootButtonAtStartup();

  // Paso 3: Cargar MAC desde Preferences (NVS)
  DBG_PRINTLN("[BOOT 3/6] Leyendo configuracion NVS (Preferences.h)...");
  loadSavedScaleFromNVS();

  // Paso 4: Construir nombre "ACAIA_L_XXXX" o "BOOKOO_SC_U XXXXXX" (18 chars exactos
  //         requeridos por el controlador BLE interno de la máquina Wendougee Data-S / Lita
  //         y compatibles simultáneamente con GeeFlow, E-Bar y de1app)
  uint64_t efuseMac = ESP.getEfuseMac();
  uint8_t mac3 = static_cast<uint8_t>((efuseMac >> 24) & 0xFF);
  uint8_t mac4 = static_cast<uint8_t>((efuseMac >> 32) & 0xFF);
  uint8_t mac5 = static_cast<uint8_t>((efuseMac >> 40) & 0xFF);
#if defined(EMULATE_BOOKOO)
  snprintf(acaiaDeviceName, sizeof(acaiaDeviceName), "BOOKOO_SC_U %02X%02X%02X", mac3, mac4, mac5);
#else
  snprintf(acaiaDeviceName, sizeof(acaiaDeviceName), "ACAIA_L_%02X%02X", mac4, mac5);
#endif

  DBG_PRINTF("[BOOT 4/6] Inicializando stack NimBLE como '%s'...", acaiaDeviceName);
  NimBLEDevice::init(acaiaDeviceName);

  // bridge_tx debe existir ANTES de publicar el servidor: la App puede conectar y
  // escribir el handshake en cuanto arranca el Advertising.
  resetLatencyStats(latStats);
  xTaskCreatePinnedToCore(bridgeTaskFn, "bridge_tx", BRIDGE_TASK_STACK, nullptr,
                          BRIDGE_TASK_PRIO, &bridgeTaskHandle, BRIDGE_TASK_CORE);

  // Habilitar soporte de Bonding BLE Legacy Just-Works (compatible con SoC Phyplus PHY6222
  // y su GAPBondMgr / GAPBOND_AUTO_SYNC_WL) para que la Timemore Dot guarde al ESP32-S3
  // en su White List cuando se empareje y reconecte automáticamente al encender
  NimBLEDevice::setSecurityAuth(true, false, false);
#if defined(BLE_HS_IO_NO_INPUT_OUTPUT)
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
#endif
#if defined(BLE_SM_PAIR_KEY_DIST_ENC) && defined(BLE_SM_PAIR_KEY_DIST_ID)
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
#endif

#if defined(ESP_PWR_LVL_P6)
  NimBLEDevice::setPower(ESP_PWR_LVL_P6);
#elif defined(ESP_PWR_LVL_P9)
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
#endif

  // Paso 5: Iniciar Servidor BLE Acaia Lunar
  DBG_PRINTF("[BOOT 5/6] Configurando Servidor Acaia Lunar (%s)...", acaiaDeviceName);
  setupAcaiaServer();

  // Paso 6: Configurar Escáner BLE Cliente Timemore Dot
  DBG_PRINTLN("[BOOT 6/6] Configurando Escaner Cliente BLE para Timemore Dot...");
  NimBLEScan* pScan = NimBLEDevice::getScan();
#if defined(NIMBLE_V2)
  // wantDuplicates = true para no descartar paquetes sucesivos, y timeout corto de 150ms
  // para que onResult() reciba el SCAN_RSP completo en ~2ms (si la balanza lo envía)
  // o entregue el ADV_IND en 150ms sin interrumpir el intercambio en el aire.
  pScan->setScanCallbacks(&timemoreScanCallbacks, true);
  pScan->setScanResponseTimeout(150);
#else
  pScan->setAdvertisedDeviceCallbacks(&timemoreScanCallbacks, true);
#endif
  pScan->setActiveScan(true);
#if defined(BLE_HCI_SCAN_FILT_NO_WL_INITA)
  pScan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL_INITA);
#endif
  // Ventana de escaneo ligera (25% duty cycle: 15ms cada 60ms) para que el Advertising
  // de BooKoo/Acaia tenga >= 75% del tiempo de radio libre y la máquina Wendougee
  // reciba ADV_IND + SCAN_RSP de inmediato sin interrupciones:
  pScan->setInterval(96); // 96 * 0.625ms = 60.0 ms
  pScan->setWindow(24);   // 24 * 0.625ms = 15.0 ms

  DBG_PRINTLN("--------------------------------------------------------------");
  DBG_PRINTLN("Comandos en Monitor Serie: [?] Estado [l] Enlaces+Latencia [t] Tara [r] Reset NVS [s] Escanear [p] Pausar Scan");
  DBG_PRINTLN("--------------------------------------------------------------");
  Serial.flush();

  // scale_link (core 1) hace la reconexión directa a la MAC guardada / el escaneo.
  // Antes se hacía aquí y bloqueaba el arranque hasta 3.8 s.
  xTaskCreatePinnedToCore(scaleLinkTaskFn, "scale_link", SCALE_TASK_STACK, nullptr,
                          SCALE_TASK_PRIO, &scaleTaskHandle, SCALE_TASK_CORE);
  DBG_PRINTF("[BOOT] Tareas: bridge_tx (core %d, prio %d) | scale_link (core %d, prio %d) | loop (core %d)",
             BRIDGE_TASK_CORE, (int)BRIDGE_TASK_PRIO, SCALE_TASK_CORE, SCALE_TASK_PRIO, xPortGetCoreID());
}

// ============================================================================
// TAREA bridge_tx (CORE 0): ÚNICO EMISOR HACIA LA APP / MÁQUINA
// ============================================================================
/**
 * Dirigida por eventos: duerme en xTaskNotifyWait() hasta que llega una muestra,
 * un comando de la App o vence un plazo (relleno de 100 ms, timer, ritmo mínimo).
 *
 * Reglas de transmisión (máximo 1 paquete por ranura de HANDSHAKE_PACING_MS):
 *   1. Peso nuevo (si pasaron >= WEIGHT_MIN_GAP_MS desde el anterior) -> prioridad máxima
 *   2. Handshake: Heartbeat ACK > INFO > STATUS > ACK evento > Timer inmediato
 *   3. Relleno de peso si no se envió nada en ACAIA_NOTIFY_INTERVAL_MS
 *   4. Timer Acaia a 10 Hz mientras corre
 * Todos los plazos se actualizan al INTENTAR enviar (aunque el stack rechace el
 * paquete), así la tarea nunca entra en un bucle activo.
 */
void bridgeTaskFn(void* arg) {
  static constexpr uint32_t HS_MASK   = EVT_APP_TX_MASK & ~EVT_WEIGHT_IMM;
  static constexpr uint32_t PACING_US = HANDSHAKE_PACING_MS * 1000UL;

  uint32_t pending             = 0;
  unsigned long lastWeightTxMs = 0;
  unsigned long lastTimerTxMs  = 0;
  uint32_t lastAnyTxUs         = 0;
  unsigned long prevSampleMs   = 0;
  uint32_t lastArrivalUs       = 0;
  bool latencyPending          = false; // Hay una muestra cuyo e2e aún no se midió

  for (;;) {
    // ---- 1. Calcular el próximo plazo y dormir hasta entonces (o hasta un evento) ----
    TickType_t waitTicks = pdMS_TO_TICKS(ACAIA_NOTIFY_INTERVAL_MS);
    if (appConnected) {
      unsigned long now = millis();
      uint32_t sinceW = now - lastWeightTxMs;
      uint32_t waitMs = (sinceW >= ACAIA_NOTIFY_INTERVAL_MS) ? 0 : (ACAIA_NOTIFY_INTERVAL_MS - sinceW);
      if (timerRunning) {
        uint32_t sinceT = now - lastTimerTxMs;
        uint32_t tIn = (sinceT >= ACAIA_NOTIFY_INTERVAL_MS) ? 0 : (ACAIA_NOTIFY_INTERVAL_MS - sinceT);
        if (tIn < waitMs) waitMs = tIn;
      }
      if (pending & HS_MASK) waitMs = 0;
      if (pending & EVT_WEIGHT_IMM) {
        uint32_t gIn = (sinceW >= WEIGHT_MIN_GAP_MS) ? 0 : (WEIGHT_MIN_GAP_MS - sinceW);
        if (gIn < waitMs) waitMs = gIn;
      }
      uint32_t sinceAnyUs = static_cast<uint32_t>(esp_timer_get_time()) - lastAnyTxUs;
      if (sinceAnyUs < PACING_US) {
        uint32_t pIn = (PACING_US - sinceAnyUs + 999) / 1000;
        if (waitMs < pIn) waitMs = pIn;
      }
      waitTicks = pdMS_TO_TICKS(waitMs);
    }

    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, waitTicks);
    pending |= bits;

    // ---- 2. Muestra nueva: caudal con el timestamp real de llegada ----
    if (pending & EVT_SAMPLE) {
      pending &= ~EVT_SAMPLE;
      uint32_t wakeUs = static_cast<uint32_t>(esp_timer_get_time());
      portENTER_CRITICAL(&sampleMux);
      uint32_t seq      = scaleSampleSeq;
      float w           = currentWeightGrams;
      unsigned long tMs = scaleSampleMs;
      uint32_t tUs      = scaleSampleUs;
      portEXIT_CRITICAL(&sampleMux);

      if (seq != processedSampleSeq) {
        uint32_t lost = seq - processedSampleSeq - 1;
        processedSampleSeq = seq;
        updateFlowFromSample(w, tMs);
        pending |= EVT_WEIGHT_IMM;

#if ENABLE_LATENCY_STATS
        uint32_t wake = wakeUs - tUs;
        uint32_t dt   = (prevSampleMs != 0) ? static_cast<uint32_t>(tMs - prevSampleMs) : 0;
        portENTER_CRITICAL(&statsMux);
        latStats.samples++;
        latStats.coalesced += lost;
        if (wake < latStats.wakeMinUs) latStats.wakeMinUs = wake;
        if (wake > latStats.wakeMaxUs) latStats.wakeMaxUs = wake;
        latStats.wakeSumUs += wake;
        if (dt > 0 && dt < 2000) {
          if (dt < latStats.dtMinMs) latStats.dtMinMs = dt;
          if (dt > latStats.dtMaxMs) latStats.dtMaxMs = dt;
          latStats.dtSumMs += dt;
          latStats.dtCount++;
        }
        portEXIT_CRITICAL(&statsMux);
        prevSampleMs   = tMs;
        lastArrivalUs  = tUs;
        latencyPending = true;
#else
        (void)lost; (void)wakeUs; (void)tUs; (void)prevSampleMs; (void)lastArrivalUs;
#endif
      }
    }

    // ---- 3. Tara: software inmediata aquí, física en scale_link ----
    if (pending & EVT_TARE) {
      pending &= ~EVT_TARE;
      applySoftwareTare();
      pending |= EVT_WEIGHT_IMM;
    }

    if (!appConnected) {
      pending &= ~EVT_APP_TX_MASK;
      latencyPending = false;
      continue;
    }

    // ---- 4. Transmitir como máximo un paquete por ranura de ritmo ----
    uint32_t nowUs = static_cast<uint32_t>(esp_timer_get_time());
    if (nowUs - lastAnyTxUs < PACING_US) continue;
    unsigned long now = millis();

    bool sendWeight = false;
    if ((pending & EVT_WEIGHT_IMM) && (now - lastWeightTxMs >= WEIGHT_MIN_GAP_MS)) {
      sendWeight = true;
    } else if (pending & HS_MASK) {
      if (pending & EVT_HB_ACK)         { pending &= ~EVT_HB_ACK;    sendAcaiaHeartbeatAck(); }
      else if (pending & EVT_INFO)      { pending &= ~EVT_INFO;      sendAcaiaInfoPacket(); }
      else if (pending & EVT_STATUS)    { pending &= ~EVT_STATUS;    sendAcaiaStatusPacket(); }
      else if (pending & EVT_ACK_EVENT) { pending &= ~EVT_ACK_EVENT; sendAcaiaAckEvent(); }
      else                              { pending &= ~EVT_TIMER_IMM; sendAcaiaTimerNotification(); lastTimerTxMs = now; }
      lastAnyTxUs = nowUs;
      continue;
    } else if (now - lastWeightTxMs >= ACAIA_NOTIFY_INTERVAL_MS) {
      sendWeight = true; // Relleno: la balanza no mandó nada nuevo en 100 ms
    } else if (timerRunning && (now - lastTimerTxMs >= ACAIA_NOTIFY_INTERVAL_MS)) {
      sendAcaiaTimerNotification();
      lastTimerTxMs = now;
      lastAnyTxUs   = nowUs;
      continue;
    }

    if (sendWeight) {
      pending &= ~EVT_WEIGHT_IMM;
      bool ok = sendAcaiaWeightNotification(currentWeightGrams, weightIsStable);
      lastWeightTxMs = now;
      lastAnyTxUs    = nowUs;
#if ENABLE_LATENCY_STATS
      uint32_t doneUs = static_cast<uint32_t>(esp_timer_get_time());
      portENTER_CRITICAL(&statsMux);
      if (ok) latStats.txWeight++; else latStats.txFail++;
      if (latencyPending && ok) {
        uint32_t e2e = doneUs - lastArrivalUs;
        if (e2e < latStats.e2eMinUs) latStats.e2eMinUs = e2e;
        if (e2e > latStats.e2eMaxUs) latStats.e2eMaxUs = e2e;
        latStats.e2eSumUs += e2e;
        latStats.e2eCount++;
        uint8_t b = (e2e < 250) ? 0 : (e2e < 1000) ? 1 : (e2e < 5000) ? 2 : (e2e < 15000) ? 3 : 4;
        latStats.hist[b]++;
      }
      portEXIT_CRITICAL(&statsMux);
      if (ok) latencyPending = false;
#else
      (void)ok;
#endif
    }
  }
}

// ============================================================================
// TAREA scale_link (CORE 1): ENLACE CON LA TIMEMORE (PUEDE BLOQUEAR)
// ============================================================================
/**
 * Todo lo que puede bloquear vive aquí: connect() síncrono (hasta 4 s + reintento),
 * descubrimiento GATT, readValue() del keep-alive y las escrituras a la balanza.
 * Mientras esta tarea espera, bridge_tx sigue atendiendo a la App en el core 0.
 */
void scaleLinkTaskFn(void* arg) {
  // Reconexión directa rápida a la MAC guardada (atrapa a la balanza si ya está encendida)
  if (hasSavedMac) {
    DBG_PRINTF("[BOOT] Intentando reconexion directa rapida a MAC guardada (%s)...", savedTimemoreMac);
    if (!connectToTimemoreScale(savedTimemoreMac, savedTimemoreAddrType, 1800, true)) {
      startTimemoreScan();
    }
  } else {
    startTimemoreScan();
  }

  for (;;) {
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(20));
    unsigned long now = millis();

    // 1. Peticiones del usuario (botón / Serial)
    if (bits & SCL_EVT_NVS_RESET) {
      clearSavedScaleFromNVS();
    }
    if (bits & SCL_EVT_SCAN) {
      scanPausedByUser = false;
      startTimemoreScan();
    }

    // 2. Detener escaneo si un callback encontró la balanza o se pausó
    if (pendingStopScan) {
      pendingStopScan = false;
      NimBLEScan* pScan = NimBLEDevice::getScan();
      if (pScan->isScanning()) {
        pScan->stop();
      }
      isScanning = false;
    }

    // 3. Reiniciar Advertising si la App se desconectó
    if (pendingRestartAdvertising) {
      pendingRestartAdvertising = false;
      NimBLEDevice::startAdvertising();
    }

    // 4. Escrituras hacia la Timemore (serializadas con connect/keep-alive)
    if (bits & SCL_EVT_TARE) {
      executeTimemoreTare();
    }
    if (bits & SCL_EVT_TIMER) {
      uint8_t cmd = pendingTimerCmd;
      pendingTimerCmd = 0xFF;
      if (cmd != 0xFF) executeTimemoreTimerCommand(cmd);
    }

    // 5. Ajuste diferido del enlace con la Timemore: intervalo corto + PHY 2M
    if (scaleConnected && !scaleLinkTuned && (now - scaleConnectedAtMs >= SCALE_LINK_TUNE_DELAY_MS)) {
      scaleLinkTuned = true;
      bool okItvl = pScaleClient->updateConnParams(SCALE_CONN_ITVL_MIN, SCALE_CONN_ITVL_MAX, 0, 400);
      bool okPhy  = false;
#if ENABLE_2M_PHY_SCALE && defined(NIMBLE_V2)
      okPhy = pScaleClient->updatePhy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
#endif
      DBG_PRINTF("[TIMEMORE-CLI] Ajuste de enlace: intervalo %.2f-%.2f ms (%s) | PHY 2M (%s)",
                 SCALE_CONN_ITVL_MIN * 1.25f, SCALE_CONN_ITVL_MAX * 1.25f,
                 okItvl ? "pedido" : "error", okPhy ? "pedido" : "no");
    }

    // 6. Ajuste diferido del enlace con la App: PHY 2M (tras el descubrimiento GATT)
    if (appConnected && !appLinkTuned && (now - appConnectedAtMs >= APP_LINK_TUNE_DELAY_MS)) {
      appLinkTuned = true;
#if ENABLE_2M_PHY_APP && defined(NIMBLE_V2)
      uint16_t h = appConnHandle;
      if (h != 0xFFFF && pAcaiaServer != nullptr) {
        bool okPhy = pAcaiaServer->updatePhy(h, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
        DBG_PRINTF("[ACAIA-SRV] Ajuste de enlace App: PHY 2M (%s)", okPhy ? "pedido" : "error");
      }
#endif
    }

    // 6b. Verificar el intervalo que el central realmente aplicó a la App y, si sigue
    //     por encima de APP_CONN_ITVL_MAX, volver a pedirlo (hasta APP_ITVL_RETRY_MAX veces).
#if defined(NIMBLE_V2)
    {
      static uint8_t appItvlRetries        = 0;
      static unsigned long appItvlCheckAt  = 0;
      static unsigned long appSessionStart = 0;
      if (appConnected && appConnectedAtMs != appSessionStart) {
        appSessionStart = appConnectedAtMs; // Nueva conexión de App: reiniciar reintentos
        appItvlRetries  = 0;
        appItvlCheckAt  = appConnectedAtMs + APP_LINK_TUNE_DELAY_MS;
      }
      uint16_t h = appConnHandle;
      if (appConnected && h != 0xFFFF && pAcaiaServer != nullptr &&
          appItvlRetries < APP_ITVL_RETRY_MAX && (long)(now - appItvlCheckAt) >= 0) {
        appItvlCheckAt = now + APP_ITVL_RETRY_PERIOD_MS;
        uint16_t itvl = pAcaiaServer->getPeerInfoByHandle(h).getConnInterval();
        if (itvl > APP_CONN_ITVL_MAX) {
          appItvlRetries++;
          pAcaiaServer->updateConnParams(h, APP_CONN_ITVL_MIN, APP_CONN_ITVL_MAX, 0, 400);
          DBG_PRINTF("[ACAIA-SRV] Intervalo App = %.2f ms (> %.2f). Re-solicitando %.2f-%.2f ms (intento %u/%u)",
                     itvl * 1.25f, APP_CONN_ITVL_MAX * 1.25f, APP_CONN_ITVL_MIN * 1.25f,
                     APP_CONN_ITVL_MAX * 1.25f, appItvlRetries, APP_ITVL_RETRY_MAX);
        } else {
          appItvlRetries = APP_ITVL_RETRY_MAX; // Ya está dentro del objetivo
          DBG_PRINTF("[ACAIA-SRV] Intervalo App OK = %.2f ms", itvl * 1.25f);
        }
      }
    }
#endif

    // 7. Conexión y auto-reconexión con la Timemore Dot
    //    (Respeta la ventana de gracia appHandshakeGraceUntilMs y la pausa manual scanPausedByUser)
    //    NOTA CRÍTICA PARA WENDOUGEE: Nunca invocar connectToTimemoreScale() a ciegas si
    //    candidateFound == false, porque apaga el Advertising durante el timeout de conexión
    //    e impediría que la máquina Wendougee descubra el puente.
    if (!scaleConnected && !scanPausedByUser && (now >= appHandshakeGraceUntilMs)) {
      unsigned long waitInterval = appConnected ? RECONNECT_INTERVAL_APP_MS : RECONNECT_INTERVAL_FAST_MS;

      if (doConnectToScale && candidateFound) {
        doConnectToScale = false;
        lastScanOrConnAttemptMs = now;
        connectToTimemoreScale(candidateMac, candidateAddrType, 4000, false);
      } else if (!isScanning && (now - lastScanOrConnAttemptMs >= waitInterval)) {
        lastScanOrConnAttemptMs = now;
        startTimemoreScan();
      }
    } else if (scaleConnected) {
      // 8. Keep-Alive cada 25 s para evitar que la Timemore Dot se apague sola
      if (now - lastKeepAliveMs >= TIMEMORE_KEEPALIVE_INTERVAL_MS) {
        lastKeepAliveMs = now;
        sendTimemoreKeepAlive();
      }
    }
  }
}

// ============================================================================
// LOOP (CORE 1, PRIORIDAD 1): INTERFAZ DE USUARIO Y DIAGNÓSTICO
// ============================================================================
// Nada sensible a la latencia vive aquí: el reenvío de peso es de bridge_tx y la
// gestión del enlace con la balanza es de scale_link.
void loop() {
  pollBootButtonRuntime();
  pollSerialDebugCommands();
  updateStatusLed();

  // Primeras tramas de peso de la conexión: bytes crudos + peso interpretado,
  // para comparar con la pantalla de la Dot.
  while (dbgFramesPrinted < dbgFrameCount) {
    uint8_t i = dbgFramesPrinted++;
    char hex[DBG_FRAME_MAX * 3 + 1];
    size_t p = 0;
    for (uint8_t b = 0; b < dbgFrameLens[i]; b++) {
      p += snprintf(&hex[p], sizeof(hex) - p, "%02X ", dbgFrames[i][b]);
    }
    hex[p] = '\0';
    DBG_PRINTF("[TIMEMORE-RX] Trama de peso %u/%u: %s-> %.1f g (servicio %s)", i + 1, DBG_FRAME_SLOTS,
               hex, dbgFrameWeights[i], scaleUsesDotService ? "0xFFF0" : "otro");
  }

  unsigned long now = millis();
  if (now - lastSerialStatusMs >= SERIAL_STATUS_INTERVAL_MS) {
    lastSerialStatusMs = now;
    printSerialStatusSummary();
    printLinkInfo();
    printLatencyStats();
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
