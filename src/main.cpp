#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <bootloader_common.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <esp_task_wdt.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Falta include/secrets.h. Copia include/secrets.example.h a include/secrets.h y configura los valores locales."
#endif

// =============================================================================
// 1. MOTOR USB
// =============================================================================
#define private public
#define protected public
#include <EspUsbHost.h>
#undef private
#undef protected
#include <usb/usb_host.h>

// =============================================================================
// 2. BUILD / CONFIG
// =============================================================================
static constexpr char PROJECT_NAME[] = "3D Print Guardian";
static constexpr char FW_VERSION[]   = "1.3.0";
static constexpr char FW_BUILD[]     = "20260706.4";

static constexpr char WIFI_SSID[]     = GUARDIAN_WIFI_SSID;
static constexpr char WIFI_PASSWORD[] = GUARDIAN_WIFI_PASSWORD;

static constexpr char AP_SSID[]       = GUARDIAN_AP_SSID;
static constexpr char AP_PASSWORD[]   = GUARDIAN_AP_PASSWORD;
static constexpr char MDNS_HOSTNAME[] = GUARDIAN_MDNS_HOSTNAME;

static constexpr char DEFAULT_PRINTER_IP_1[] = GUARDIAN_DEFAULT_PRINTER_IP_1;
static constexpr char DEFAULT_PRINTER_IP_2[] = GUARDIAN_DEFAULT_PRINTER_IP_2;

static constexpr char STOP_COMMAND[] = "{\"method\":\"set\",\"params\":{\"stop\":1}}";

// OTA
static constexpr char OTA_METADATA_URL[]      = GUARDIAN_OTA_METADATA_URL;
static constexpr char OTA_AUTH_HEADER_NAME[]  = "Authorization";
static constexpr char OTA_AUTH_PREFIX[]       = "Bearer";
static constexpr char OTA_AUTH_TOKEN[]        = GUARDIAN_OTA_AUTH_TOKEN;
static constexpr char OTA_CA_CERT_PEM[]       = GUARDIAN_OTA_CA_CERT_PEM;
static constexpr bool OTA_ALLOW_INSECURE_HTTPS = GUARDIAN_OTA_ALLOW_INSECURE_HTTPS;

// Basic Auth para rutas criticas y, por defecto, tambien para status/config de lectura.
// Si usuario y password quedan vacios, la auth se desactiva explicitamente (modo laboratorio).
static constexpr char WEB_AUTH_USERNAME[] = GUARDIAN_WEB_AUTH_USERNAME;
static constexpr char WEB_AUTH_PASSWORD[] = GUARDIAN_WEB_AUTH_PASSWORD;
static constexpr bool WEB_AUTH_PROTECT_STATUS = GUARDIAN_WEB_AUTH_PROTECT_STATUS;
static constexpr bool WEB_AUTH_PROTECT_CONFIG_GET = GUARDIAN_WEB_AUTH_PROTECT_CONFIG_GET;
static constexpr char ARDUINO_OTA_PASSWORD[] = GUARDIAN_ARDUINO_OTA_PASSWORD;

// NTP
static constexpr char NTP_TZ_INFO[]   = "UTC0";
static constexpr char NTP_SERVER_1[]  = "time.google.com";
static constexpr char NTP_SERVER_2[]  = "pool.ntp.org";
static constexpr char NTP_SERVER_3[]  = "time.nist.gov";

static constexpr uint16_t DEFAULT_WS_PORT = 9999;
static constexpr char WS_PATH[]           = "/";

static constexpr uint32_t DEFAULT_UPS_STOP_DELAY_MS    = 120000UL;
// Clasificacion del byte de estado del UPS (el que sigue al marcador '#' en el informe HID):
// en bateria reporta un valor bajo (UPS_CODE_ON_BATTERY); en linea reporta valores altos que
// fluctuan dentro de una banda (105-113 observados), por eso se usa un umbral en vez de un rango fijo.
static constexpr uint8_t  UPS_CODE_ON_BATTERY          = 2;    // corte de red confirmado
static constexpr uint8_t  UPS_CODE_ON_LINE_MIN         = 100;  // >= este valor = en linea / carga normal
static constexpr uint32_t UPS_QUERY_INTERVAL_MS        = 10000UL;
static constexpr uint32_t UPS_LISTEN_DELAY_MS          = 1800UL;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS       = 10000UL;
static constexpr uint32_t WS_RECONNECT_INTERVAL_MS     = 5000UL;
static constexpr uint32_t WS_MANUAL_REINIT_MS          = 30000UL;
static constexpr uint32_t WS_HEARTBEAT_INTERVAL_MS     = 15000UL;
static constexpr uint32_t WS_HEARTBEAT_TIMEOUT_MS      = 3000UL;
static constexpr uint8_t  WS_HEARTBEAT_RETRIES         = 2;
static constexpr uint32_t STOP_RETRY_INTERVAL_MS       = 8000UL;
static constexpr uint8_t  STOP_MAX_ATTEMPTS            = 3;
static constexpr uint32_t STOP_ACK_GRACE_MS            = 3000UL;
static constexpr uint32_t STOP_SUPERVISOR_LOG_MS       = 30000UL;
static constexpr uint32_t SERIAL_STATUS_INTERVAL_MS    = 30000UL;
static constexpr uint32_t STATE_PERSIST_DEBOUNCE_MS    = 2500UL;
static constexpr uint32_t UPS_WAIT_LOG_INTERVAL_MS     = 45000UL;
static constexpr uint32_t UPS_STALE_READ_MS            = 45000UL;
static constexpr uint32_t BATTERY_PROGRESS_LOG_MS      = 30000UL;
static constexpr uint32_t TIME_SYNC_RETRY_MS           = 15000UL;
static constexpr uint32_t OTA_HTTP_TIMEOUT_MS          = 15000UL;
static constexpr uint32_t OTA_TLS_TIME_WAIT_MS         = 8000UL;
static constexpr uint32_t OTA_REBOOT_DELAY_MS          = 1500UL;
static constexpr uint32_t OTA_METADATA_CACHE_MS        = 300000UL;
static constexpr uint32_t RESTART_DELAY_MS             = 900UL;
static constexpr uint8_t  WIFI_MAX_FAIL_RESET          = 6;   // watchdog WiFi: reiniciar stack tras 6 intentos fallidos (~60 s)

static constexpr size_t EVENT_LOG_CAPACITY   = 16;
static constexpr size_t EVENT_TEXT_SIZE      = 144;
static constexpr size_t EVENT_TIME_SIZE      = 24;
static constexpr size_t SMALL_TEXT_SIZE      = 32;
static constexpr size_t MEDIUM_TEXT_SIZE     = 96;
static constexpr size_t LAST_TEXT_SIZE       = 160;
static constexpr size_t URL_TEXT_SIZE        = 256;
static constexpr size_t CHANGELOG_TEXT_SIZE  = 192;
static constexpr size_t SHA256_HEX_SIZE      = 65;
static constexpr size_t WS_DISCOVERY_LOG_CAPACITY = 24;
static constexpr size_t WS_DISCOVERY_TEXT_SIZE = 192;
static constexpr size_t JSON_STATUS_CAPACITY = 18432;
static constexpr size_t JSON_DIAG_CAPACITY   = 24576;
static constexpr size_t JSON_OTA_CAPACITY    = 2048;
static constexpr size_t JSON_MANIFEST_CAP    = 2048;
static constexpr size_t JSON_SEND_BUFFER_CAPACITY = JSON_DIAG_CAPACITY + 64;

// =============================================================================
// 3. TYPES
// =============================================================================
enum class EventSeverity : uint8_t {
  Info = 0,
  Success,
  Warning,
  Error
};

enum class OtaState : uint8_t {
  Idle = 0,
  Checking,
  Available,
  UpToDate,
  Downloading,
  Flashing,
  Success,
  Error
};

enum class StopAckState : uint8_t {
  None = 0,
  Pending,
  Hint,
  Exhausted
};

struct LogEntry {
  uint32_t ms;
  EventSeverity severity;
  char text[EVENT_TEXT_SIZE];
  char isoUtc[EVENT_TIME_SIZE];
};

struct PrinterTelemetry {
  uint32_t rxCount;
  uint32_t txCount;
  uint32_t jsonOkCount;
  uint32_t jsonErrorCount;
  uint32_t lastParsedMs;
  bool hasState;
  bool hasStatus;
  bool hasProgress;
  bool hasHotendTemp;
  bool hasBedTemp;
  char state[SMALL_TEXT_SIZE];
  char status[SMALL_TEXT_SIZE];
  float progressPct;
  float hotendTempC;
  float bedTempC;
};

struct PrinterState {
  const char* name;
  char host[SMALL_TEXT_SIZE];
  WebSocketsClient* client;
  bool connected;
  bool stopSent;
  uint8_t stopAttempts;
  StopAckState ackState;
  uint32_t lastConnectAttemptMs;
  uint32_t lastConnectedMs;
  uint32_t lastDisconnectedMs;
  uint32_t lastStopSentMs;
  uint32_t lastStopAttemptMs;
  uint32_t lastAckMs;
  uint32_t lastProbeSentMs;
  char lastText[LAST_TEXT_SIZE];
  char lastError[LAST_TEXT_SIZE];
  char ackHint[LAST_TEXT_SIZE];
  char lastProbeName[SMALL_TEXT_SIZE];
  PrinterTelemetry telemetry;
};

struct WsDiscoveryEntry {
  uint32_t ms;
  uint8_t printerIndex;
  char direction[4];
  char text[WS_DISCOVERY_TEXT_SIZE];
};

struct DeviceConfig {
  char printerHost1[SMALL_TEXT_SIZE];
  char printerHost2[SMALL_TEXT_SIZE];
  char otaMetadataUrl[URL_TEXT_SIZE];
  uint16_t wsPort;
  uint32_t upsStopDelayMs;
};

struct OtaRuntime {
  volatile OtaState state;
  volatile uint8_t progress;
  bool updateAvailable;
  bool secureTransport;
  bool manifestRequiresTls;
  uint32_t lastCheckMs;
  uint32_t lastSuccessMs;
  uint32_t expectedSize;
  uint32_t downloadedSize;
  char remoteVersion[SMALL_TEXT_SIZE];
  char remoteBuild[SMALL_TEXT_SIZE];
  char binUrl[URL_TEXT_SIZE];
  char expectedSha256[SHA256_HEX_SIZE];
  char installedSha256[SHA256_HEX_SIZE];
  char changelog[CHANGELOG_TEXT_SIZE];
  char lastError[LAST_TEXT_SIZE];
  char lastCheckedUtc[EVENT_TIME_SIZE];
  char lastSuccessVersion[SMALL_TEXT_SIZE];
};

struct OtaDownloadIntegrity {
  mbedtls_sha256_context sha;
  bool active;
  uint32_t bytes;
};

struct TimeRuntime {
  bool configured;
  bool synced;
  uint32_t lastAttemptMs;
  time_t lastEpoch;
  char lastIsoUtc[EVENT_TIME_SIZE];
};

// =============================================================================
// 4. GLOBAL STATE
// =============================================================================
WebServer server(80);
Preferences prefs;

WebSocketsClient printerClient1;
WebSocketsClient printerClient2;

DeviceConfig deviceConfig = {};
OtaRuntime otaRuntime = {};
TimeRuntime timeRuntime = {};

LogEntry eventLog[EVENT_LOG_CAPACITY];
size_t eventCount = 0;
size_t eventHead = 0;

PrinterState printers[] = {
  {"K1-MAX 1", "", &printerClient1, false, false, 0, StopAckState::None, 0, 0, 0, 0, 0, 0, 0, "", "", "", "", {}},
  {"K1-MAX 2", "", &printerClient2, false, false, 0, StopAckState::None, 0, 0, 0, 0, 0, 0, 0, "", "", "", "", {}}
};
static constexpr size_t PRINTER_COUNT = sizeof(printers) / sizeof(printers[0]);
WsDiscoveryEntry wsDiscoveryLog[WS_DISCOVERY_LOG_CAPACITY];
size_t wsDiscoveryCount = 0;
size_t wsDiscoveryHead = 0;

bool stopActive = false;
bool stopCompletionLogged = false;
bool onBattery = false;
bool previousBatteryState = false;
bool upsStateKnown = false;
bool waitingUpsResponse = false;
bool mdnsStarted = false;
bool stateDirty = false;
bool restartRequested = false;
bool lastUpsUsbReady = false;

uint8_t lastUpsStatusCode = 0;
int16_t lastUnmappedUpsCode = -1; // ultimo codigo UPS desconocido ya logueado (dedupe); -1 = ninguno
uint8_t lastWiFiDisconnectReason = 0;
uint32_t bootCount = 0;
uint32_t outageStartedMs = 0;
uint32_t lastUpsSeenMs = 0;
uint32_t lastStopRequestMs = 0;
uint32_t lastUpsQueryMs = 0;
uint32_t upsResponseScheduledAtMs = 0;
uint32_t lastWiFiAttemptMs = 0;
uint32_t lastPersistMs = 0;
uint32_t restartAtMs = 0;
uint32_t lastStatusLogMs = 0;
uint32_t lastUpsWaitLogMs = 0;
uint32_t lastBatteryProgressLogMs = 0;
uint32_t lastStopSupervisorLogMs = 0;

char lastEvent[EVENT_TEXT_SIZE] = "Sin eventos.";
char previousEvent[EVENT_TEXT_SIZE] = "Sin historial persistido.";
char stopReason[SMALL_TEXT_SIZE] = "ninguno";
char currentResetReason[SMALL_TEXT_SIZE] = "desconocido";
char previousResetReason[SMALL_TEXT_SIZE] = "sin dato";

TaskHandle_t otaTaskHandle = nullptr;

static SemaphoreHandle_t  g_otaMutex            = nullptr; // mutex para acceso concurrente a otaRuntime
static OtaDownloadIntegrity g_otaDownloadIntegrity = {};
static volatile uint8_t   g_pendingUpsCode       = 0;      // codigo UPS recibido en el callback USB (ISR-like)
static volatile bool      g_pendingUpsValid      = false;  // indica que g_pendingUpsCode tiene un dato nuevo
static SemaphoreHandle_t  g_logMutex             = nullptr; // mutex para logEvent(): llamado desde tareas OTA y loop
static volatile bool      g_pendingWsReconnect   = false;  // flag: reconectar WS desde loop() tras obtener IP
static volatile bool      g_pendingWsDisconnect  = false;  // flag: desconectar WS desde loop() tras perder WiFi
static uint8_t            wifiFailCount          = 0;      // contador de intentos WiFi fallidos consecutivos

// =============================================================================
// 5. WEB PANEL
// =============================================================================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="description" content="Panel local del ESP32 3D Print Guardian: UPS, WiFi, impresoras y firmware OTA">
<meta name="theme-color" content="#0a0b10">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Cdefs%3E%3ClinearGradient id='g' x1='0' y1='0' x2='64' y2='64'%3E%3Cstop offset='0' stop-color='%233b82f6'/%3E%3Cstop offset='1' stop-color='%238b5cf6'/%3E%3C/linearGradient%3E%3C/defs%3E%3Crect width='64' height='64' rx='14' fill='url(%23g)'/%3E%3Ctext x='32' y='43' font-family='Arial,sans-serif' font-size='27' font-weight='700' fill='%23fff' text-anchor='middle'%3ELK%3C/text%3E%3C/svg%3E">
<title>3D Print Guardian</title>
<style>
:root{--bg:#0a0b10;--bg2:#0d0f16;--paper:rgba(19,21,31,.86);--paper2:rgba(24,27,40,.7);--line:rgba(132,146,190,.14);--line2:rgba(132,146,190,.08);--tx:#e7eaf3;--tx2:#9aa3bd;--tx3:#5d6783;--blue:#3b82f6;--violet:#8b5cf6;--cyan:#22d3ee;--green:#34d399;--amber:#fbbf24;--red:#f87171;--grad:linear-gradient(135deg,#3b82f6,#8b5cf6);--glow:0 0 0 1px rgba(99,102,241,.18),0 10px 40px rgba(59,130,246,.10);--shadow:0 14px 38px rgba(0,0,0,.5),0 2px 8px rgba(0,0,0,.4);--p1:0px;--p2:0px;
--mono:"SF Mono",ui-monospace,Menlo,Consolas,"Cascadia Code",monospace;--sans:-apple-system,BlinkMacSystemFont,"Segoe UI",Inter,Roboto,sans-serif}
@media(prefers-reduced-motion:reduce){*,::before,::after{animation:none!important;transition:none!important}.reveal{opacity:1!important;transform:none!important}}
.paused *{animation-play-state:paused!important}
*{box-sizing:border-box}html{background:var(--bg);-webkit-text-size-adjust:100%;scroll-behavior:smooth}
body{margin:0;min-height:100vh;color:var(--tx);overflow-x:hidden;font:14px/1.55 var(--sans);-webkit-font-smoothing:antialiased;background:radial-gradient(ellipse 80% 55% at 50% -12%,rgba(76,90,255,.16),transparent 60%),radial-gradient(ellipse 50% 40% at 92% 8%,rgba(139,92,246,.12),transparent 65%),radial-gradient(ellipse 45% 35% at 4% 30%,rgba(34,211,238,.07),transparent 65%),var(--bg)}
body::before{content:"";position:fixed;inset:0;z-index:-6;background-image:linear-gradient(rgba(132,146,190,.05) 1px,transparent 1px),linear-gradient(90deg,rgba(132,146,190,.05) 1px,transparent 1px);background-size:42px 42px;mask-image:radial-gradient(ellipse 90% 70% at 50% 0%,#000 30%,transparent 80%);-webkit-mask-image:radial-gradient(ellipse 90% 70% at 50% 0%,#000 30%,transparent 80%);pointer-events:none}
body::after{content:"";position:fixed;inset:0;z-index:-5;background:radial-gradient(ellipse 42% 30% at 70% 100%,rgba(59,130,246,.08),transparent 70%);pointer-events:none}
.b{position:fixed;border-radius:50%;pointer-events:none;z-index:-4;filter:blur(2px);will-change:transform}
.b1{width:620px;height:520px;background:radial-gradient(circle,rgba(59,130,246,.16) 0%,transparent 66%);top:-220px;left:-160px;animation:bm 28s ease-in-out infinite alternate}
.b2{width:520px;height:460px;background:radial-gradient(circle,rgba(139,92,246,.15) 0%,transparent 66%);top:16%;right:-170px;animation:bm 34s ease-in-out infinite alternate-reverse}
.b3{width:420px;height:380px;background:radial-gradient(circle,rgba(34,211,238,.10) 0%,transparent 66%);bottom:2%;left:10%;animation:bm 23s ease-in-out infinite alternate}
@keyframes bm{to{transform:translate(56px,64px) scale(1.06)}}
.ambient{position:fixed;inset:0;width:100%;height:100%;z-index:-3;pointer-events:none;opacity:.5;transform:translate3d(0,var(--p2),0)}
.ambient .wave{fill:none;stroke:rgba(59,130,246,.20);stroke-width:1.6;stroke-linecap:round;stroke-dasharray:10 16;animation:flow 22s linear infinite}
.ambient .wave2{fill:none;stroke:rgba(139,92,246,.16);stroke-width:1.3;stroke-linecap:round;stroke-dasharray:7 20;animation:flow 30s linear infinite reverse}
.ambient .link{fill:none;stroke:rgba(99,102,241,.18)}
.ambient .node{fill:#11131c;stroke:rgba(99,160,255,.6);stroke-width:1.6;animation:nd 4.6s ease-in-out infinite}
.ambient .bit{fill:rgba(34,211,238,.42);animation:bt 11s ease-in-out infinite}
@keyframes flow{to{stroke-dashoffset:-240}}@keyframes nd{50%{transform:scale(1.22);opacity:.55}}@keyframes bt{50%{transform:translateY(-24px);opacity:.5}}
a{color:#7eb1ff;text-decoration:none}a:hover{text-decoration:underline}
code{font:12px/1.45 var(--mono);color:var(--cyan)}
.shell{position:relative;max-width:1160px;margin:0 auto;padding:22px 16px 60px}
.hero{position:relative;z-index:20;display:grid;grid-template-columns:minmax(0,1.45fr) minmax(270px,.8fr);gap:18px;align-items:stretch;margin-bottom:16px;padding:20px;border:1px solid var(--line);border-radius:18px;background:linear-gradient(180deg,rgba(24,27,42,.92),rgba(15,17,26,.92));box-shadow:var(--shadow),var(--glow);backdrop-filter:blur(22px) saturate(140%);-webkit-backdrop-filter:blur(22px) saturate(140%);overflow:hidden}
.hero::before{content:"";position:absolute;inset:0 0 auto;height:1px;background:linear-gradient(90deg,transparent,rgba(139,160,255,.5),transparent)}
.hero::after{content:"";position:absolute;top:0;left:0;width:3px;height:100%;background:var(--grad);box-shadow:0 0 18px rgba(99,102,241,.55)}
.hero-main{position:relative;z-index:1;display:flex;flex-direction:column;gap:14px}
.brand{display:flex;align-items:flex-start;gap:13px}
.brand-mark{width:56px;height:46px;display:grid;place-items:center;flex:0 0 auto}
.brand-logo{display:block;filter:drop-shadow(0 6px 18px rgba(99,102,241,.45))}
.logo-bg{fill:url(#logoG)}.logo-shield{fill:none;stroke:#fff;stroke-width:3.4;stroke-linejoin:round}.logo-bolt{fill:rgba(255,255,255,.92)}
h1{margin:0;font-size:1.95rem;line-height:1.06;font-weight:760;letter-spacing:-.022em;background:linear-gradient(110deg,#f4f6ff 25%,#9db4ff 70%,#c4a8ff 100%);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}
.brand-copy p{margin:7px 0 0;max-width:620px;color:var(--tx2);font-size:.9rem}
.caps{display:flex;flex-wrap:wrap;gap:8px}
.pill{display:inline-flex;align-items:center;gap:7px;min-height:30px;padding:5px 12px;border-radius:8px;border:1px solid var(--line);background:rgba(30,34,52,.6);color:var(--tx2);font:600 .73rem/1.3 var(--mono)}
.pill strong{color:var(--tx);font-weight:700}
.dot{width:8px;height:8px;border-radius:50%;background:var(--tx3);flex:0 0 auto}
.dot.live{background:var(--green);box-shadow:0 0 0 0 rgba(52,211,153,.5),0 0 10px rgba(52,211,153,.7);animation:pulse 2.2s infinite}
@keyframes pulse{70%{box-shadow:0 0 0 8px rgba(52,211,153,0),0 0 10px rgba(52,211,153,.7)}100%{box-shadow:0 0 0 0 rgba(52,211,153,0),0 0 10px rgba(52,211,153,.7)}}
.banner{display:flex;align-items:flex-start;gap:10px;padding:11px 13px;border-radius:10px;border:1px solid var(--line);background:rgba(28,32,48,.55);color:var(--tx2);font-size:.86rem}
.banner strong{color:var(--tx)}
.banner .dot{margin-top:5px}
.banner.ok{border-color:rgba(52,211,153,.28);background:rgba(52,211,153,.07)}.banner.ok .dot{background:var(--green)}
.banner.warn{border-color:rgba(251,191,36,.3);background:rgba(251,191,36,.08)}.banner.warn .dot{background:var(--amber)}
.banner.error{border-color:rgba(248,113,113,.34);background:rgba(248,113,113,.09)}.banner.error .dot{background:var(--red);animation:pulse 1.2s infinite}
.hero-visual{position:relative;min-height:190px;transform:translate3d(0,var(--p1),0);transition:transform .18s linear}
.stage{position:absolute;inset:10px;border-radius:14px;background:linear-gradient(150deg,rgba(28,32,52,.9),rgba(13,15,24,.9));border:1px solid rgba(120,135,200,.2);box-shadow:inset 0 1px 0 rgba(160,175,255,.14),0 18px 36px rgba(0,0,0,.5);overflow:hidden}
.stage::before{content:"";position:absolute;z-index:0;inset:-45% -30%;background:conic-gradient(from 130deg,transparent,rgba(59,130,246,.22),rgba(139,92,246,.18),rgba(34,211,238,.12),transparent 72%);animation:turn 18s linear infinite}
.stage::after{content:"";position:absolute;inset:0;z-index:1;background-image:linear-gradient(rgba(132,146,190,.07) 1px,transparent 1px),linear-gradient(90deg,rgba(132,146,190,.07) 1px,transparent 1px);background-size:26px 26px}
.diagram{position:absolute;inset:0;z-index:2;width:100%;height:100%}
.diagram .wire{fill:none;stroke:rgba(99,130,255,.38);stroke-width:1.8}
.diagram .energy{fill:none;stroke:rgba(34,211,238,.55);stroke-width:2;stroke-linecap:round;stroke-dasharray:8 10;animation:flow 7s linear infinite}
.diagram .nd{fill:#0d0f18;stroke:var(--cyan);stroke-width:1.7}
.diagram .pline{fill:none;stroke:#8b5cf6;stroke-width:2.6;stroke-linecap:round;stroke-dasharray:1 210;animation:dash 4s ease-in-out infinite;filter:drop-shadow(0 0 6px rgba(139,92,246,.8))}
.scan{position:absolute;z-index:3;left:14%;right:14%;top:34%;height:2px;border-radius:99px;background:linear-gradient(90deg,transparent,var(--cyan),var(--violet),transparent);box-shadow:0 0 22px rgba(34,211,238,.5);animation:scan 3.8s ease-in-out infinite}
@keyframes turn{to{transform:rotate(360deg)}}@keyframes scan{50%{transform:translateY(40px)}}@keyframes dash{50%{stroke-dasharray:130 210;stroke-dashoffset:-92}100%{stroke-dashoffset:-210}}
.grid{display:grid;gap:14px}.g3{grid-template-columns:repeat(3,minmax(0,1fr))}.g2{grid-template-columns:repeat(2,minmax(0,1fr))}.mb{margin-bottom:14px}
.tabs{position:sticky;top:10px;z-index:30;display:flex;gap:4px;margin-bottom:14px;padding:5px;border:1px solid var(--line);border-radius:13px;background:linear-gradient(180deg,rgba(24,27,42,.94),rgba(15,17,26,.94));box-shadow:var(--shadow),var(--glow);backdrop-filter:blur(20px) saturate(140%);-webkit-backdrop-filter:blur(20px) saturate(140%);overflow-x:auto;scrollbar-width:none}
.tabs::-webkit-scrollbar{display:none}
.tab{display:inline-flex;align-items:center;justify-content:center;gap:7px;flex:1;border:0;border-radius:9px;padding:10px 13px;background:transparent;color:var(--tx2);font:650 .82rem/1 var(--sans);cursor:pointer;white-space:nowrap;transition:background .15s ease,color .15s ease;outline:none}
.tab .ic{width:15px;height:15px}
.tab:hover:not(.on){color:var(--tx);background:rgba(99,110,160,.1)}
.tab.on{background:var(--grad);color:#fff;box-shadow:0 8px 22px rgba(99,102,241,.35)}
.tab:focus-visible{box-shadow:0 0 0 3px rgba(99,140,255,.4)}
.tbdg{width:7px;height:7px;border-radius:50%;background:var(--red);display:none;flex:0 0 auto;box-shadow:0 0 8px rgba(248,113,113,.8)}
#tbdg-ota{background:var(--cyan);box-shadow:0 0 8px rgba(34,211,238,.8)}
.tab.on .tbdg{background:#fff;box-shadow:none}
.tab-page{display:none}.tab-page.on{display:block}
.card{position:relative;overflow:hidden;border-radius:14px;border:1px solid var(--line);background:var(--paper);box-shadow:var(--shadow);backdrop-filter:blur(18px) saturate(135%);-webkit-backdrop-filter:blur(18px) saturate(135%);transition:border-color .25s ease,box-shadow .25s ease}
.card:hover{border-color:rgba(120,140,255,.3);box-shadow:var(--shadow),0 0 24px rgba(79,99,255,.1)}
.card::before{content:"";position:absolute;inset:0 0 auto;height:1px;background:linear-gradient(90deg,transparent,rgba(150,165,255,.35),transparent)}
.reveal{opacity:0;transform:translateY(18px);transition:opacity .65s ease,transform .65s ease}.reveal.on{opacity:1;transform:none}
.hd{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:13px 16px;border-bottom:1px solid var(--line2);min-height:50px;background:rgba(13,15,24,.4)}
.eyebrow{display:inline-flex;align-items:center;gap:8px;color:var(--tx3);font:750 .68rem/1.3 var(--mono);text-transform:uppercase;letter-spacing:.1em}
.ic{fill:currentColor;flex:0 0 auto}
.eyebrow .ic{width:14px;height:14px;color:#7c9aff;filter:drop-shadow(0 0 6px rgba(99,102,241,.45))}
.body{padding:14px 16px}.rows{display:grid;gap:7px}
.row{display:flex;justify-content:space-between;gap:14px;padding:7px 0;border-bottom:1px solid var(--line2);align-items:flex-start}
.row:last-child{border-bottom:0;padding-bottom:0}
.k{color:var(--tx2);font-size:.8rem}
.v{text-align:right;font-size:.82rem;word-break:break-word;font-family:var(--mono);color:var(--tx)}
.tag{display:inline-flex;align-items:center;gap:5px;padding:3px 9px;border-radius:6px;border:1px solid;font:700 .68rem/1.4 var(--mono);white-space:nowrap;letter-spacing:.02em}
.t-ok{color:var(--green);background:rgba(52,211,153,.1);border-color:rgba(52,211,153,.3)}
.t-warn{color:var(--amber);background:rgba(251,191,36,.1);border-color:rgba(251,191,36,.3)}
.t-err{color:var(--red);background:rgba(248,113,113,.1);border-color:rgba(248,113,113,.32)}
.t-info{color:#8ab6ff;background:rgba(59,130,246,.1);border-color:rgba(59,130,246,.32)}
.t-muted{color:var(--tx3);background:rgba(120,130,165,.07);border-color:var(--line)}
.control{display:flex;justify-content:space-between;align-items:center;gap:16px;flex-wrap:wrap}
.control h3{margin:0;font-size:1rem;font-weight:700}
.control p{margin:5px 0 0;max-width:540px;color:var(--tx2);font-size:.84rem}
.actions{display:flex;flex-wrap:wrap;gap:9px}
.btn{position:relative;border:0;border-radius:10px;padding:10px 16px;min-height:40px;font:700 .84rem/1.15 var(--sans);cursor:pointer;transition:transform .14s ease,opacity .16s ease,box-shadow .2s ease,border-color .2s ease;outline:none;white-space:normal}
.btn:hover:not(:disabled){transform:translateY(-1px)}.btn:active:not(:disabled){transform:scale(.985)}
.btn:focus-visible{box-shadow:0 0 0 3px rgba(99,140,255,.4)}
.btn:disabled{opacity:.4;cursor:not-allowed;transform:none}
.btn-stop{background:linear-gradient(135deg,#ef4444,#b91c1c);color:#fff;box-shadow:0 10px 28px rgba(239,68,68,.3),0 0 0 1px rgba(255,120,120,.25) inset}
.btn-stop:hover:not(:disabled){box-shadow:0 12px 32px rgba(239,68,68,.45)}
.btn-main{background:var(--grad);color:#fff;box-shadow:0 10px 28px rgba(99,102,241,.32),0 0 0 1px rgba(160,170,255,.3) inset}
.btn-main:hover:not(:disabled){box-shadow:0 12px 34px rgba(99,102,241,.48)}
.btn-ghost{background:rgba(34,38,58,.7);color:var(--tx);border:1px solid var(--line)}
.btn-ghost:hover:not(:disabled){border-color:rgba(130,150,255,.4);background:rgba(42,47,72,.8)}
.btn-warn{background:rgba(251,191,36,.1);color:var(--amber);border:1px solid rgba(251,191,36,.32)}
.btn-warn:hover:not(:disabled){background:rgba(251,191,36,.16)}
.timer{display:flex;align-items:center;gap:22px;padding:18px 16px}
.ring{position:relative;width:108px;height:108px;flex:0 0 auto;filter:drop-shadow(0 0 14px rgba(251,191,36,.25))}
.ring svg{display:block;transform:rotate(-90deg)}
.ring-center{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
.ring-val{font:750 1.6rem/1 var(--mono);font-variant-numeric:tabular-nums}
.ring-lbl{margin-top:4px;color:var(--tx3);font:700 .6rem/1 var(--mono);text-transform:uppercase;letter-spacing:.1em}
.timer-copy h3{margin:0;font-size:1rem;font-weight:700}
.timer-copy p{margin:6px 0 0;color:var(--tx2);max-width:520px;font-size:.84rem}
.printer-title{display:flex;align-items:flex-start;gap:10px}
.wsdot{width:8px;height:8px;border-radius:50%;background:var(--tx3);margin-top:5px;flex:0 0 auto}
.wsdot.up{background:var(--green);box-shadow:0 0 10px rgba(52,211,153,.8)}
.mono{font:11.5px/1.45 var(--mono);color:var(--tx3)}
.ota-track{margin-top:12px;height:5px;border-radius:999px;background:rgba(132,146,190,.12);overflow:hidden}
.ota-bar{height:100%;width:0;border-radius:999px;background:var(--grad);transition:width .4s ease;box-shadow:0 0 16px rgba(99,102,241,.6)}
.spin{width:14px;height:14px;border-radius:50%;display:inline-block;vertical-align:-3px;border:2px solid rgba(99,140,255,.25);border-top-color:#7ea2ff;animation:spin .75s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.log{display:grid}
.log-item{display:grid;grid-template-columns:auto auto minmax(0,1fr);gap:11px;padding:10px 16px;border-bottom:1px solid var(--line2)}
.log-item:last-child{border-bottom:0}
.log-item:hover{background:rgba(99,110,160,.05)}
.sev{width:7px;height:7px;border-radius:50%;margin-top:6px}
.sev-info{background:var(--blue);box-shadow:0 0 8px rgba(59,130,246,.6)}
.sev-success{background:var(--green);box-shadow:0 0 8px rgba(52,211,153,.6)}
.sev-warning{background:var(--amber);box-shadow:0 0 8px rgba(251,191,36,.6)}
.sev-error{background:var(--red);box-shadow:0 0 8px rgba(248,113,113,.6)}
.log-time{font:11px/1.4 var(--mono);color:var(--tx3);min-width:52px}
.log-msg{color:var(--tx2);font-size:.81rem;line-height:1.5;word-break:break-word}
.foot{margin-top:28px;padding-top:16px;border-top:1px solid var(--line);text-align:center;color:var(--tx3);font-size:.75rem;line-height:1.9}
.cfg-row{display:flex;align-items:center;gap:14px;padding:9px 0;border-bottom:1px solid var(--line2)}
.cfg-row:last-child{border-bottom:0}
.cfg-lbl{flex:0 0 176px;color:var(--tx2);font-size:.8rem;font-weight:600}
.cfg-in{flex:1;border:1px solid var(--line);border-radius:8px;padding:8px 11px;font:.83rem/1.4 var(--mono);background:rgba(11,13,20,.8);color:var(--tx);outline:none;transition:border-color .15s ease,box-shadow .15s ease}
.cfg-in::placeholder{color:var(--tx3)}
.cfg-in:focus{border-color:#5b7bff;box-shadow:0 0 0 3px rgba(91,123,255,.18)}
.cfg-wide{flex-wrap:wrap}.cfg-wide .cfg-lbl{flex:1 1 100%;margin-bottom:4px}.cfg-wide .cfg-in{flex:1 1 100%}
.modal-bg{position:fixed;inset:0;z-index:60;display:none;align-items:center;justify-content:center;padding:18px;background:rgba(5,6,11,.72);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px)}
.modal-bg.show{display:flex}
.modal{width:100%;max-width:390px;border:1px solid var(--line);border-radius:16px;background:linear-gradient(180deg,rgba(26,29,44,.97),rgba(14,16,25,.97));box-shadow:var(--shadow),var(--glow);padding:20px}
.modal h3{margin:0;font-size:1.02rem;font-weight:700}
.modal p{margin:8px 0 0;color:var(--tx2);font-size:.84rem;line-height:1.5}
.modal .cfg-in{width:100%;margin-top:10px}
.modal .actions{margin-top:16px;justify-content:flex-end}
.modal-err{margin:10px 0 0;color:var(--red);font-size:.78rem;min-height:1em}
@media(max-width:980px){.hero{position:relative;top:auto;grid-template-columns:1fr}.hero-visual{min-height:160px}.g3{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:640px){.shell{padding:12px 10px 42px}.hero{padding:14px;border-radius:14px}.brand{gap:10px}h1{font-size:1.42rem}.g3,.g2{grid-template-columns:1fr}.hd,.body,.log-item{padding-left:13px;padding-right:13px}.row{gap:10px}.timer{flex-direction:column;align-items:flex-start}.actions,.btn{width:100%}.btn{text-align:center}.stage{inset:4px}.hero-visual{min-height:132px}.cfg-lbl{flex:0 0 140px}.tabs{top:6px;border-radius:11px}.tab{flex:0 0 auto;padding:9px 12px;font-size:.78rem}}
</style>
</head>
<body>
<div class="b b1" aria-hidden="true"></div>
<div class="b b2" aria-hidden="true"></div>
<div class="b b3" aria-hidden="true"></div>
<svg class="ambient" viewBox="0 0 1200 900" preserveAspectRatio="none" aria-hidden="true">
  <path class="wave" d="M-40 280C200 215 340 340 560 265S870 210 1250 285"></path>
  <path class="wave" style="animation-duration:27s;opacity:.6" d="M-60 660C200 570 370 720 600 628S940 565 1270 668"></path>
  <path class="wave2" d="M-30 470C220 405 370 510 600 445S910 395 1260 470"></path>
  <path class="link" d="M140 175L275 112L415 200L545 130L682 222L800 156L952 228L1090 170"></path>
  <path class="link" d="M225 528L368 472L520 552L665 482L820 554L996 486L1148 545"></path>
  <path class="link" style="opacity:.6" d="M80 355L215 304L365 385L506 315L646 394L785 322L928 404"></path>
  <circle class="node" cx="140" cy="175" r="5"></circle><circle class="node" cx="275" cy="112" r="4"></circle><circle class="node" cx="415" cy="200" r="5"></circle><circle class="node" cx="545" cy="130" r="4"></circle><circle class="node" cx="682" cy="222" r="5"></circle><circle class="node" cx="800" cy="156" r="4"></circle><circle class="node" cx="952" cy="228" r="5"></circle><circle class="node" cx="1090" cy="170" r="4"></circle>
  <circle class="node" cx="225" cy="528" r="4"></circle><circle class="node" cx="520" cy="552" r="5"></circle><circle class="node" cx="820" cy="554" r="4"></circle><circle class="node" cx="1148" cy="545" r="5"></circle>
  <circle class="bit" cx="1028" cy="148" r="3"></circle><circle class="bit" cx="112" cy="534" r="3"></circle><circle class="bit" cx="724" cy="748" r="4"></circle><circle class="bit" style="animation-duration:13s;animation-delay:-5s" cx="458" cy="84" r="3"></circle><circle class="bit" style="animation-duration:16s;animation-delay:-8s" cx="958" cy="648" r="3"></circle>
</svg>
<svg style="display:none" aria-hidden="true">
  <symbol id="i-dash" viewBox="0 0 24 24"><path d="M3 13h8V3H3v10zm0 8h8v-6H3v6zm10 0h8V11h-8v10zm0-18v6h8V3h-8z"/></symbol>
  <symbol id="i-bolt" viewBox="0 0 24 24"><path d="M7 2v11h3v9l7-12h-4l4-8H7z"/></symbol>
  <symbol id="i-wifi" viewBox="0 0 24 24"><path d="M1 9l2 2c4.97-4.97 13.03-4.97 18 0l2-2C16.93 2.93 7.08 2.93 1 9zm8 8l3 3 3-3a4.24 4.24 0 0 0-6 0zm-4-4l2 2a7.07 7.07 0 0 1 10 0l2-2C15.14 9.14 8.87 9.14 5 13z"/></symbol>
  <symbol id="i-chip" viewBox="0 0 24 24"><path d="M15 9H9v6h6V9zm-2 4h-2v-2h2v2zm8-2V9h-2V7a2 2 0 0 0-2-2h-2V3h-2v2h-2V3H9v2H7a2 2 0 0 0-2 2v2H3v2h2v2H3v2h2v2a2 2 0 0 0 2 2h2v2h2v-2h2v2h2v-2h2a2 2 0 0 0 2-2v-2h2v-2h-2v-2h2zm-4 6H7V7h10v10z"/></symbol>
  <symbol id="i-clock" viewBox="0 0 24 24"><path d="M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm0 18a8 8 0 1 1 0-16 8 8 0 0 1 0 16zm.5-13H11v6l5.25 3.15.75-1.23-4.5-2.67V7z"/></symbol>
  <symbol id="i-stop" viewBox="0 0 24 24"><path d="M15.73 3H8.27L3 8.27v7.46L8.27 21h7.46L21 15.73V8.27L15.73 3zM12 17.3a1.3 1.3 0 1 1 0-2.6 1.3 1.3 0 0 1 0 2.6zm1-4.3h-2V7h2v6z"/></symbol>
  <symbol id="i-printer" viewBox="0 0 24 24"><path d="M19 8H5a3 3 0 0 0-3 3v6h4v4h12v-4h4v-6a3 3 0 0 0-3-3zm-3 11H8v-5h8v5zm3-7a1 1 0 1 1 0-2 1 1 0 0 1 0 2zm-1-9H6v4h12V3z"/></symbol>
  <symbol id="i-cloud" viewBox="0 0 24 24"><path d="M19.35 10.04A7.49 7.49 0 0 0 12 4C9.11 4 6.6 5.64 5.35 8.04A5.99 5.99 0 0 0 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM17 13l-5 5-5-5h3V9h4v4h3z"/></symbol>
  <symbol id="i-wrench" viewBox="0 0 24 24"><path d="M22.7 19l-9.1-9.1a6.1 6.1 0 0 0-1.5-6.9c-2-2-5-2.4-7.4-1.3L9 6 6 9 1.6 4.7C.4 7.1.9 10.1 2.9 12.1a6.1 6.1 0 0 0 6.9 1.5l9.1 9.1c.4.4 1 .4 1.4 0l2.3-2.3c.5-.4.5-1.1.1-1.4z"/></symbol>
  <symbol id="i-gear" viewBox="0 0 24 24"><path d="M19.14 12.94c.04-.3.06-.61.06-.94s-.02-.64-.07-.94l2.03-1.58a.49.49 0 0 0 .12-.61l-1.92-3.32a.49.49 0 0 0-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54A.48.48 0 0 0 13.92 2h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96a.49.49 0 0 0-.59.22L2.73 8.47c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58a.49.49 0 0 0-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32a.49.49 0 0 0-.12-.61l-2.01-1.58zM12 15.6a3.6 3.6 0 1 1 0-7.2 3.6 3.6 0 0 1 0 7.2z"/></symbol>
  <symbol id="i-list" viewBox="0 0 24 24"><path d="M4 6h16v2H4V6zm0 5h16v2H4v-2zm0 5h10v2H4v-2z"/></symbol>
</svg>
<div class="shell">
  <section class="hero reveal">
    <div class="hero-main">
      <div class="brand">
        <div class="brand-mark">
          <svg class="brand-logo" viewBox="0 0 92 64" width="58" height="40" role="img" aria-label="3D Print Guardian">
            <defs>
              <linearGradient id="logoG" x1="0" y1="0" x2="92" y2="64" gradientUnits="userSpaceOnUse">
                <stop offset="0" stop-color="#3b82f6"/>
                <stop offset="1" stop-color="#8b5cf6"/>
              </linearGradient>
            </defs>
            <rect x="0" y="0" width="92" height="64" rx="14" class="logo-bg"/>
            <path class="logo-shield" d="M46 11l16 6v12.5c0 9.8-6.6 18.4-16 23.5-9.4-5.1-16-13.7-16-23.5V17z"></path>
            <path class="logo-bolt" d="M50 19.5 39 35.5h6l-3.5 10.5L53 30h-6l3-10.5z"></path>
          </svg>
        </div>
        <div class="brand-copy">
          <h1>3D Print Guardian</h1>
          <p>Panel local del ESP32 para vigilar energia, WiFi, impresoras y firmware sin depender del monitor serial.</p>
        </div>
      </div>
      <div class="caps">
        <div class="pill"><span class="dot" id="wifi-dot"></span><span id="wifi-pill">Conectando</span></div>
        <div class="pill"><span>FW</span>&nbsp;<strong id="fw-pill">--</strong></div>
        <div class="pill"><span>Boot</span>&nbsp;<strong id="boot-pill">--</strong></div>
        <div class="pill" id="auth-pill" style="display:none"><span>Auth</span>&nbsp;<strong>Critica</strong></div>
      </div>
      <div id="banner" class="banner ok"><span class="dot"></span><div>Cargando estado del ESP32...</div></div>
    </div>
    <div class="hero-visual" aria-hidden="true">
      <div class="stage">
        <svg class="diagram" viewBox="0 0 360 220" preserveAspectRatio="none">
          <path class="energy" d="M18 154C75 122 112 178 170 145S263 116 342 152"></path>
          <path class="wire" d="M58 72L124 48L184 86L248 52L306 86"></path>
          <path class="wire" d="M86 124L144 94L213 126L282 96"></path>
          <path class="pline" d="M58 72L124 48L184 86L248 52L306 86"></path>
          <circle class="nd" cx="58" cy="72" r="7"></circle><circle class="nd" cx="124" cy="48" r="5"></circle><circle class="nd" cx="184" cy="86" r="6"></circle><circle class="nd" cx="248" cy="52" r="5"></circle><circle class="nd" cx="306" cy="86" r="7"></circle>
          <circle class="nd" cx="86" cy="124" r="5"></circle><circle class="nd" cx="144" cy="94" r="6"></circle><circle class="nd" cx="213" cy="126" r="5"></circle><circle class="nd" cx="282" cy="96" r="6"></circle>
        </svg>
        <span class="scan"></span>
      </div>
    </div>
  </section>

  <section id="timer-card" class="card reveal mb" style="display:none">
    <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-clock"/></svg>Temporizador STOP automatico</span><span class="tag t-warn">Corte activo</span></div>
    <div class="timer">
      <div class="ring">
        <svg width="108" height="108" viewBox="0 0 108 108">
          <circle fill="none" stroke="rgba(132,146,190,.14)" stroke-width="8" cx="54" cy="54" r="46"></circle>
          <circle id="timer-arc" fill="none" stroke="var(--amber)" stroke-width="8" stroke-linecap="round" cx="54" cy="54" r="46" stroke-dasharray="289.03" stroke-dashoffset="0"></circle>
        </svg>
        <div class="ring-center">
          <div id="timer-value" class="ring-val">2:00</div>
          <div class="ring-lbl">restante</div>
        </div>
      </div>
      <div class="timer-copy">
        <h3>Corte de energia detectado</h3>
        <p>Si la linea no vuelve antes de que termine el contador, el ESP32 activara el STOP de seguridad en ambas impresoras.</p>
      </div>
    </div>
  </section>

  <nav class="tabs reveal" role="tablist" aria-label="Secciones del panel">
    <button class="tab on" data-tab="estado" role="tab" aria-selected="true"><svg class="ic" viewBox="0 0 24 24"><use href="#i-dash"/></svg>Estado</button>
    <button class="tab" data-tab="imp" role="tab" aria-selected="false"><svg class="ic" viewBox="0 0 24 24"><use href="#i-printer"/></svg>Impresoras<span class="tbdg" id="tbdg-imp"></span></button>
    <button class="tab" data-tab="ota" role="tab" aria-selected="false"><svg class="ic" viewBox="0 0 24 24"><use href="#i-cloud"/></svg>Firmware<span class="tbdg" id="tbdg-ota"></span></button>
    <button class="tab" data-tab="cfg" role="tab" aria-selected="false"><svg class="ic" viewBox="0 0 24 24"><use href="#i-gear"/></svg>Config</button>
    <button class="tab" data-tab="support" role="tab" aria-selected="false"><svg class="ic" viewBox="0 0 24 24"><use href="#i-wrench"/></svg>Soporte<span class="tbdg" id="tbdg-support"></span></button>
    <button class="tab" data-tab="log" role="tab" aria-selected="false"><svg class="ic" viewBox="0 0 24 24"><use href="#i-list"/></svg>Eventos</button>
  </nav>

  <div class="tab-page on" id="tab-estado">
  <section class="grid g3 mb">
    <article class="card reveal">
      <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-bolt"/></svg>Energia / UPS</span><span id="ups-chip"></span></div>
      <div class="body rows" id="ups-body"></div>
    </article>
    <article class="card reveal">
      <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-wifi"/></svg>Red</span><span id="net-chip"></span></div>
      <div class="body rows" id="net-body"></div>
    </article>
    <article class="card reveal">
      <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-chip"/></svg>Sistema</span><span id="sys-chip"></span></div>
      <div class="body rows" id="sys-body"></div>
    </article>
  </section>

  <section class="card reveal mb">
    <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-stop"/></svg>Control de impresion</span><span id="stop-chip"></span></div>
    <div class="body">
      <div class="control">
        <div>
          <h3>Detener impresion ahora</h3>
          <p>Envia el comando STOP por WebSocket a las dos impresoras y deja el latch activo para reintentar si alguna se reconecta despues.</p>
        </div>
        <button class="btn btn-stop" data-action="/api/stop" data-confirm="Se enviara el comando STOP a las dos impresoras de inmediato y el latch quedara activo.">Enviar STOP</button>
      </div>
    </div>
  </section>
  </div>

  <div class="tab-page" id="tab-imp">
  <section class="grid g2 mb">
    <article id="printer-0" class="card reveal"></article>
    <article id="printer-1" class="card reveal"></article>
  </section>
  <section class="card reveal mb">
    <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-list"/></svg>Descubrimiento WS</span><span id="wsd-chip" class="tag t-muted">0</span></div>
    <div class="body">
      <div class="actions" style="margin-bottom:12px">
        <button class="btn btn-ghost" data-probe="status" data-printer="0">P1 status</button>
        <button class="btn btn-ghost" data-probe="state" data-printer="0">P1 state</button>
        <button class="btn btn-ghost" data-probe="temp" data-printer="0">P1 temp</button>
        <button class="btn btn-ghost" data-probe="info" data-printer="0">P1 info</button>
        <button class="btn btn-ghost" data-probe="status" data-printer="1">P2 status</button>
        <button class="btn btn-ghost" data-probe="state" data-printer="1">P2 state</button>
        <button class="btn btn-ghost" data-probe="temp" data-printer="1">P2 temp</button>
        <button class="btn btn-ghost" data-probe="info" data-printer="1">P2 info</button>
        <button class="btn btn-warn" id="btn-wsd-clear">Limpiar registro</button>
      </div>
      <div id="wsd-log" class="log">
        <div class="log-item"><span class="sev sev-info"></span><span class="log-time">--</span><span class="log-msg">Sin trafico WebSocket registrado.</span></div>
      </div>
    </div>
  </section>
  </div>

  <div class="tab-page" id="tab-ota">
  <section class="card reveal mb">
    <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-cloud"/></svg>Firmware OTA</span><span id="ota-chip"></span></div>
    <div class="body">
      <div class="rows" id="ota-body"></div>
      <div id="ota-progress-wrap" style="display:none">
        <div class="ota-track"><div class="ota-bar" id="ota-bar"></div></div>
      </div>
      <div class="actions" style="margin-top:16px">
        <button class="btn btn-ghost" id="btn-check">Buscar actualizacion</button>
        <button class="btn btn-main" id="btn-install" style="display:none">Instalar update</button>
        <input type="file" id="up-file" accept=".bin" style="display:none">
        <button class="btn btn-ghost" id="btn-upload">Subir firmware (.bin)</button>
      </div>
    </div>
  </section>
  </div>

  <div class="tab-page" id="tab-cfg">
  <section class="card reveal mb">
    <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-gear"/></svg>Configuracion</span><span id="cfg-chip" class="tag t-muted">Sin cambios</span></div>
    <div class="body">
      <div class="rows">
        <div class="cfg-row"><span class="cfg-lbl">Impresora 1 IP / host</span><input class="cfg-in" id="c-p1" type="text" maxlength="31" autocomplete="off" spellcheck="false" placeholder="192.168.x.x"></div>
        <div class="cfg-row"><span class="cfg-lbl">Impresora 2 IP / host</span><input class="cfg-in" id="c-p2" type="text" maxlength="31" autocomplete="off" spellcheck="false" placeholder="192.168.x.x"></div>
        <div class="cfg-row"><span class="cfg-lbl">Puerto WebSocket</span><input class="cfg-in" id="c-wsp" type="number" min="1" max="65535" style="max-width:110px"></div>
        <div class="cfg-row"><span class="cfg-lbl">Temporizador STOP (s)</span><input class="cfg-in" id="c-del" type="number" min="30" max="3600" style="max-width:110px"><span style="color:var(--tx3);font-size:.75rem">30 - 3600 s</span></div>
        <div class="cfg-row cfg-wide"><span class="cfg-lbl">URL Manifest OTA</span><input class="cfg-in" id="c-ota" type="url" maxlength="255" autocomplete="off" spellcheck="false" placeholder="https://tu-servidor.com/manifest.json"></div>
      </div>
      <div class="actions" style="margin-top:14px">
        <button class="btn btn-main" id="btn-cfg-save">Guardar config</button>
        <button class="btn btn-ghost" id="btn-cfg-discard">Descartar</button>
      </div>
    </div>
  </section>

  </div>

  <div class="tab-page" id="tab-support">
  <section class="grid g3 mb">
    <article class="card reveal">
      <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-wrench"/></svg>Salud operativa</span><span id="support-chip"></span></div>
      <div class="body rows" id="support-health"></div>
    </article>
    <article class="card reveal">
      <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-cloud"/></svg>Firmware / Particiones</span><span id="support-fw-chip"></span></div>
      <div class="body rows" id="support-fw"></div>
    </article>
    <article class="card reveal">
      <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-chip"/></svg>Memoria</span><span id="support-mem-chip"></span></div>
      <div class="body rows" id="support-memory"></div>
    </article>
  </section>

  <section class="card reveal mb">
    <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-wrench"/></svg>Mantenimiento</span><span class="tag t-muted">Protegido</span></div>
    <div class="body">
      <div class="actions">
        <button class="btn btn-ghost" id="btn-diag-open">Abrir diagnostico JSON</button>
        <button class="btn btn-ghost" id="btn-diag-download">Descargar diagnostico</button>
        <button class="btn btn-ghost" data-action="/api/clear-stop" data-confirm="El latch STOP quedara libre y el ESP32 dejara de reintentar el envio del comando.">Limpiar latch STOP</button>
        <button class="btn btn-ghost" data-action="/api/reconnect">Reconectar WiFi / WS</button>
        <button class="btn btn-warn" data-action="/api/reboot" data-confirm="El ESP32 se reiniciara y el panel dejara de responder durante unos segundos.">Reiniciar ESP32</button>
      </div>
    </div>
  </section>
  </div>

  <div class="tab-page" id="tab-log">
  <section class="card reveal">
    <div class="hd"><span class="eyebrow"><svg class="ic" viewBox="0 0 24 24"><use href="#i-list"/></svg>Registro de eventos</span><span id="log-chip" class="tag t-muted">0</span></div>
    <div id="log" class="log">
      <div class="log-item"><span class="sev sev-info"></span><span class="log-time">--</span><span class="log-msg">Esperando eventos...</span></div>
    </div>
  </section>
  </div>

  <div class="foot">
    Sin WiFi principal puedes entrar por el AP <code>ESP32-IMPRESORAS</code> en <a href="http://192.168.4.1">192.168.4.1</a>.<br>
    "ACK STOP" es una pista basada en mensajes recibidos por WebSocket, no una confirmacion definitiva del firmware de la impresora.
  </div>
</div>

<div class="modal-bg" id="auth-modal" role="dialog" aria-modal="true" aria-labelledby="auth-title">
  <div class="modal">
    <h3 id="auth-title">Autenticacion requerida</h3>
    <p>Esta accion es critica. Ingresa las credenciales del panel.</p>
    <input class="cfg-in" id="auth-user" type="text" placeholder="Usuario" autocomplete="username" spellcheck="false">
    <input class="cfg-in" id="auth-pass" type="password" placeholder="Contrasena" autocomplete="current-password">
    <div class="modal-err" id="auth-err"></div>
    <div class="actions">
      <button class="btn btn-ghost" id="auth-cancel">Cancelar</button>
      <button class="btn btn-main" id="auth-ok">Ingresar</button>
    </div>
  </div>
</div>

<div class="modal-bg" id="cf-modal" role="dialog" aria-modal="true" aria-labelledby="cf-title">
  <div class="modal">
    <h3 id="cf-title">Confirmar accion</h3>
    <p id="cf-text"></p>
    <div class="actions">
      <button class="btn btn-ghost" id="cf-cancel">Cancelar</button>
      <button class="btn btn-stop" id="cf-ok">Confirmar</button>
    </div>
  </div>
</div>

<script>
(function(){
var D=document,root=D.documentElement,STATE=null,AUTH=null,refreshTimer=null,ARC=289.03;
var reduce=window.matchMedia&&window.matchMedia('(prefers-reduced-motion: reduce)');
function q(id){return D.getElementById(id)}
try{AUTH=JSON.parse(sessionStorage.getItem('guardianAuth')||'null')}catch(_){AUTH=null}
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]})}
function fmt(ms){var s=Math.floor((ms||0)/1000),d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),r=s%60;if(d)return d+'d '+h+'h '+m+'m';if(h)return h+'h '+m+'m '+r+'s';if(m)return m+'m '+String(r).padStart(2,'0')+'s';return r+'s'}
function row(k,v){return '<div class="row"><div class="k">'+esc(k)+'</div><div class="v">'+v+'</div></div>'}
function tag(text,cls){return '<span class="tag '+cls+'">'+esc(text)+'</span>'}
function bytes(v){v=Number(v||0);if(v>=1048576)return (v/1048576).toFixed(1)+' MB';if(v>=1024)return Math.round(v/1024)+' KB';return v+' B'}
function kb(v){return bytes(Number(v||0)*1024)}
function authHeader(){return AUTH&&AUTH.user?{'Authorization':'Basic '+btoa(AUTH.user+':'+AUTH.pass)}:{}}
function askAuth(errMsg){return new Promise(function(res){var m=q('auth-modal'),ub=q('auth-ok'),cb=q('auth-cancel');q('auth-err').textContent=errMsg||'';q('auth-user').value=AUTH&&AUTH.user?AUTH.user:'';q('auth-pass').value='';m.classList.add('show');setTimeout(function(){q(AUTH&&AUTH.user?'auth-pass':'auth-user').focus()},60);function done(v){m.classList.remove('show');ub.removeEventListener('click',ok);cb.removeEventListener('click',no);m.removeEventListener('keydown',key);res(v)}function ok(){var u=q('auth-user').value.trim(),p=q('auth-pass').value;if(!u){q('auth-err').textContent='Ingresa el usuario.';return}AUTH={user:u,pass:p};sessionStorage.setItem('guardianAuth',JSON.stringify(AUTH));done(true)}function no(){done(false)}function key(e){if(e.key==='Enter'){e.preventDefault();ok()}else if(e.key==='Escape')no()}ub.addEventListener('click',ok);cb.addEventListener('click',no);m.addEventListener('keydown',key)})}
function askConfirm(text,danger){return new Promise(function(res){var m=q('cf-modal'),ob=q('cf-ok'),cb=q('cf-cancel');q('cf-text').textContent=text;ob.className='btn '+(danger?'btn-stop':'btn-main');m.classList.add('show');setTimeout(function(){cb.focus()},60);function done(v){m.classList.remove('show');ob.removeEventListener('click',y);cb.removeEventListener('click',n);m.removeEventListener('keydown',key);res(v)}function y(){done(true)}function n(){done(false)}function key(e){if(e.key==='Escape')n()}ob.addEventListener('click',y);cb.addEventListener('click',n);m.addEventListener('keydown',key)})}
async function ensureAuth(){if(AUTH&&AUTH.user)return true;return askAuth('')}
function clearAuth(){AUTH=null;sessionStorage.removeItem('guardianAuth')}
async function api(url,method,critical,body){var options={method:method||'GET',cache:'no-store',credentials:'same-origin',headers:{}};if(body){options.body=typeof body==='string'?body:JSON.stringify(body);options.headers['Content-Type']='application/json'}if(critical&&AUTH&&AUTH.user)Object.assign(options.headers,authHeader());var response=await fetch(url,options);if(response.status===401&&critical){clearAuth();if(!(await askAuth('Usuario o contrasena incorrectos. Intenta nuevamente.')))throw new Error('Autenticacion cancelada');Object.assign(options.headers,authHeader());response=await fetch(url,options)}if(!response.ok){var msg='HTTP '+response.status;try{var txt=await response.text();if(txt)msg=txt}catch(_){}throw new Error(msg)}var ct=response.headers.get('content-type')||'';return ct.indexOf('application/json')>=0?response.json():response.text()}
async function rawApi(url){var options={method:'GET',cache:'no-store',credentials:'same-origin',headers:{}};if(AUTH&&AUTH.user)Object.assign(options.headers,authHeader());var response=await fetch(url,options);if(response.status===401){clearAuth();if(!(await askAuth('Usuario o contrasena incorrectos. Intenta nuevamente.')))throw new Error('Autenticacion cancelada');Object.assign(options.headers,authHeader());response=await fetch(url,options)}if(!response.ok)throw new Error('HTTP '+response.status);return response}
function severityClass(s){if(s==='success')return 'sev-success';if(s==='warning')return 'sev-warning';if(s==='error')return 'sev-error';return 'sev-info'}
function setBanner(kind,html){var el=q('banner');el.className='banner '+kind;el.innerHTML='<span class="dot"></span><div>'+html+'</div>'}
function renderCountdown(data){var card=q('timer-card'),show=data.on_battery&&!data.stop_active;card.style.display=show?'':'none';if(show)card.classList.add('on');if(!show)return;var total=Math.max(1,data.stop_delay_s||120),elapsed=Math.floor((data.outage_ms||0)/1000),remain=Math.max(0,total-elapsed),ratio=remain/total,color=ratio>0.5?'var(--amber)':(ratio>0.2?'#fb923c':'var(--red)');q('timer-value').textContent=Math.floor(remain/60)+':'+String(remain%60).padStart(2,'0');var arc=q('timer-arc');arc.style.stroke=color;arc.style.strokeDashoffset=String(ARC*(1-ratio))}
function renderWsDiscovery(items){var root=q('wsd-log'),list=items||[];q('wsd-chip').textContent=String(list.length);if(!list.length){root.innerHTML='<div class="log-item"><span class="sev sev-info"></span><span class="log-time">--</span><span class="log-msg">Sin trafico WebSocket registrado.</span></div>';return}root.innerHTML=list.slice().reverse().map(function(e){var cls=e.direction==='rx'?'sev-success':(e.direction==='tx'?'sev-warning':'sev-info'),dir=String(e.direction||'--').toUpperCase(),name=e.printer||('P'+String((e.printer_index||0)+1));return '<div class="log-item"><span class="sev '+cls+'"></span><span class="log-time">'+esc(e.uptime||'--')+'</span><span class="log-msg"><strong>'+esc(dir)+' '+esc(name)+':</strong> <code>'+esc(e.text||'')+'</code></span></div>'}).join('')}
function renderPrinters(printers){printers.forEach(function(p,i){var t=p.telemetry||{},ackLabel=p.ack_state==='hint'?tag('ACK probable','t-info'):(p.ack_state==='exhausted'?tag('Sin ACK','t-err'):(p.ack_state==='pending'?tag('ACK pendiente','t-warn'):tag('Sin ACK','t-muted'))),wsLabel=p.connected?tag('WS activo','t-ok'):tag('WS caido','t-err'),max=p.stop_max_attempts||STATE.stop_max_attempts||3,temp=(t.hotend_temp_c!=null||t.bed_temp_c!=null)?esc((t.hotend_temp_c!=null?('Hotend '+Number(t.hotend_temp_c).toFixed(1)+' C'):'Hotend --')+' / '+(t.bed_temp_c!=null?('Bed '+Number(t.bed_temp_c).toFixed(1)+' C'):'Bed --')):'--',prog=t.progress_pct!=null?esc(Number(t.progress_pct).toFixed(1)+'%'):'--',state=t.state||t.status||'';q('printer-'+i).innerHTML='<div class="hd"><div class="printer-title"><span class="wsdot '+(p.connected?'up':'')+'"></span><div><div style="font-size:.94rem;font-weight:700">'+esc(p.name)+'</div><div class="mono">'+esc(p.host)+':'+esc(p.port)+'</div></div></div>'+wsLabel+'</div><div class="body rows">'+row('Estado detectado',state?esc(state):'--')+row('Progreso',prog)+row('Temperaturas',temp)+row('Trafico WS',esc(String(t.rx_count||0)+' rx / '+String(t.tx_count||0)+' tx'))+row('JSON parseado',t.parsed?tag('Si','t-ok'):(t.json_error_count?tag('Error','t-warn'):tag('Sin JSON','t-muted')))+row('STOP enviado',p.stop_sent?tag('Si','t-ok'):tag('No',STATE.stop_active?'t-err':'t-muted'))+row('Intentos STOP',esc(String(p.stop_attempts||0)+'/'+String(max)))+row('ACK STOP',ackLabel)+row('Ultimo probe',p.last_probe_name?esc(p.last_probe_name+' / '+(p.last_probe_sent_text||'--')):'--')+row('Ultimo intento',esc(p.last_stop_attempt_text||'--'))+row('Ultimo envio',esc(p.last_stop_sent_text||'--'))+row('Ultimo ACK',esc(p.last_ack_text||'--'))+row('Ultimo WS OK',esc(p.last_connected_text||'--'))+row('Ultimo texto','<code>'+esc(p.last_text||'--')+'</code>')+(p.ack_hint?row('ACK hint','<code>'+esc(p.ack_hint)+'</code>'):'')+(p.last_error?row('Error','<span style="color:var(--red)">'+esc(p.last_error)+'</span>'):'')+'</div>'})}
function renderLogs(logs){q('log-chip').textContent=String(logs.length||0);var root=q('log');if(!logs.length){root.innerHTML='<div class="log-item"><span class="sev sev-info"></span><span class="log-time">--</span><span class="log-msg">Sin eventos recientes.</span></div>';return}root.innerHTML=logs.slice().reverse().map(function(e){return '<div class="log-item"><span class="sev '+severityClass(e.severity)+'"></span><span class="log-time">'+esc(e.uptime||'--')+'</span><span class="log-msg">'+esc(e.text)+'</span></div>'}).join('')}
function renderOta(ota){var chip='';if(ota.state==='available')chip=tag('Disponible','t-info');else if(ota.state==='success')chip=tag('Actualizado','t-ok');else if(ota.state==='error')chip=tag('Error','t-err');else if(ota.busy)chip=tag('En progreso','t-warn');q('ota-chip').innerHTML=chip;q('ota-body').innerHTML=[row('Version local',esc(ota.fw_version+' ('+ota.fw_build+')')),row('Version remota',esc((ota.remote_version||'--')+(ota.remote_build?' ('+ota.remote_build+')':''))),row('Estado',esc(ota.state_label||ota.state)),row('Transporte',ota.secure_transport?tag('HTTPS','t-ok'):tag('HTTP / inseguro','t-warn')),row('Integridad',ota.integrity_match?tag('SHA OK','t-ok'):(ota.expected_sha256?tag('Pendiente','t-muted'):tag('Manifest incompleto','t-warn'))),row('Tamaño esperado',ota.expected_size?bytes(ota.expected_size):'--'),row('Descargado',ota.downloaded_size?bytes(ota.downloaded_size):'--'),row('SHA manifest',ota.expected_sha256?'<code>'+esc(ota.expected_sha256)+'</code>':'--'),row('SHA descarga',ota.installed_sha256?'<code>'+esc(ota.installed_sha256)+'</code>':'--'),row('Manifest',ota.metadata_url?'<code>'+esc(ota.metadata_url)+'</code>':'--'),row('Ultima consulta',esc(ota.last_checked_utc||'--')),row('Ultima instalada',esc(ota.last_success_version||'--')),(ota.changelog?row('Cambios',esc(ota.changelog)):''),(ota.last_error?row('Error','<span style="color:var(--red)">'+esc(ota.last_error)+'</span>'):'')].join('');var pw=q('ota-progress-wrap');if(ota.busy||ota.state==='success'){pw.style.display='';q('ota-bar').style.width=String(ota.progress||0)+'%'}else pw.style.display='none';var ib=q('btn-install');ib.style.display=ota.update_available?'':'none';ib.disabled=!!ota.busy;ib.textContent='Instalar '+(ota.remote_version?('v'+ota.remote_version):'update');var cb=q('btn-check');cb.disabled=!!ota.busy;cb.innerHTML=ota.busy?'<span class="spin"></span> Trabajando...':'Buscar actualizacion'}
function supportIssues(data){var issues=[],hw=data.hardware||{},ups=data.ups||{},wifi=data.wifi||{},printers=data.printers||[],ota=data.ota||{};if(!ups.usb_ready)issues.push('UPS USB desconectada');else if(!ups.state_known)issues.push('UPS sin lectura');else if(ups.read_stale)issues.push('UPS sin lectura reciente');if(!wifi.connected)issues.push('WiFi STA abajo');var down=printers.filter(function(p){return !p.connected}).length;if(down)issues.push(String(down)+' WS impresora caido(s)');if(data.stop_active)issues.push('Latch STOP activo');if(data.stop_exhausted_count)issues.push(String(data.stop_exhausted_count)+' STOP sin ACK');if(ota.state==='error')issues.push('OTA con error');if(ota.busy)issues.push('OTA en curso');if(hw.heap_free_kb&&hw.heap_free_kb<64)issues.push('Heap libre bajo');if(hw.psram_found===false)issues.push('PSRAM no detectada');return issues}
function renderSupport(data){var hw=data.hardware||{},ota=data.ota||{},issues=supportIssues(data),ok=!issues.length;q('support-chip').innerHTML=ok?tag('OK','t-ok'):tag(String(issues.length)+' alerta(s)','t-warn');q('tbdg-support').style.display=ok?'':'inline-flex';q('support-health').innerHTML=[row('Estado general',ok?tag('Operativo','t-ok'):tag('Revisar','t-warn')),row('Observaciones',issues.length?esc(issues.join(' | ')):'Sin alertas activas'),row('STOP entrega',esc(String(data.stop_delivered_count||0)+'/'+String((data.printers||[]).length||2)+' transmitidas, '+String(data.stop_pending_count||0)+' pendiente(s)')),row('STOP sin ACK',data.stop_exhausted_count?tag(String(data.stop_exhausted_count),'t-err'):tag('0','t-ok')),row('Ultimo evento',esc(data.last_event||'--')),row('Reset actual',esc(data.current_reset_reason||'--'))].join('');q('support-fw-chip').innerHTML=tag(hw.running_partition||'--','t-muted');q('support-fw').innerHTML=[row('Board',esc(hw.board||'--')),row('Firmware',esc((data.fw_version||'--')+' / '+(data.fw_build||'--'))),row('Particion activa',esc(hw.running_partition||'--')),row('Particion boot',esc(hw.boot_partition||'--')),row('Siguiente OTA',esc(hw.next_update_partition||'--')),row('Slots OTA',esc(String(hw.ota_partition_count||'--'))),row('Sketch usado',hw.sketch_size?bytes(hw.sketch_size):'--'),row('Espacio OTA libre',hw.free_sketch_space?bytes(hw.free_sketch_space):'--'),row('Ultimo OTA OK',esc(ota.last_success_version||'--'))].join('');q('support-mem-chip').innerHTML=hw.heap_free_kb&&hw.heap_free_kb<64?tag('Bajo','t-warn'):tag('Normal','t-ok');q('support-memory').innerHTML=[row('Heap libre',hw.heap_free_kb!=null?kb(hw.heap_free_kb):'--'),row('Heap minimo',hw.heap_min_kb!=null?kb(hw.heap_min_kb):'--'),row('Bloque maximo',hw.heap_max_alloc?bytes(hw.heap_max_alloc):'--'),row('PSRAM',hw.psram_found?tag('Detectada','t-ok'):tag('No detectada','t-warn')),row('PSRAM libre',hw.psram_free!=null?bytes(hw.psram_free):'--'),row('Flash total',hw.flash_size?bytes(hw.flash_size):(hw.flash_mb?esc(String(hw.flash_mb)+' MB'):'--'))].join('')}
async function diagnosticsText(url){var r=await rawApi(url);return r.text()}
async function openDiagnostics(){try{var txt=await diagnosticsText('/api/diagnostics');var u=URL.createObjectURL(new Blob([txt],{type:'application/json'}));window.open(u,'_blank','noopener');setTimeout(function(){URL.revokeObjectURL(u)},30000)}catch(err){setBanner('error','<strong>Diagnostico:</strong> '+esc(err.message))}}
async function downloadDiagnostics(){try{var txt=await diagnosticsText('/api/diagnostics/export'),u=URL.createObjectURL(new Blob([txt],{type:'application/json'})),a=D.createElement('a');a.href=u;a.download='guardian-diagnostics-'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';D.body.appendChild(a);a.click();a.remove();setTimeout(function(){URL.revokeObjectURL(u)},30000)}catch(err){setBanner('error','<strong>Descarga:</strong> '+esc(err.message))}}
function render(data){STATE=data;var printers=data.printers||[],wc=data.wifi&&data.wifi.connected,pending=data.stop_pending_count!=null?data.stop_pending_count:printers.filter(function(p){return !p.stop_sent}).length,exhausted=data.stop_exhausted_count||0,delivered=data.stop_delivered_count||0,total=printers.length||2;q('fw-pill').textContent=data.fw_version;q('boot-pill').textContent=String(data.boot_count);q('auth-pill').style.display=data.auth&&data.auth.critical_required?'':'none';q('wifi-dot').className='dot'+(wc?' live':'');q('wifi-pill').textContent=wc?data.wifi.ssid:'Sin WiFi';if(data.stop_active){var smsg='<strong>STOP activo.</strong> Motivo: '+esc(data.stop_reason)+'. '+esc(String(delivered)+'/'+String(total))+' transmitidas.'+(pending?' Quedan '+esc(String(pending))+' pendiente(s).':'')+(exhausted?' '+esc(String(exhausted))+' sin ACK tras reintentos.':'');setBanner('error',smsg)}else if(data.ota&&data.ota.busy){setBanner('warn','<strong>OTA en curso.</strong> '+esc(data.ota.state_label||data.ota.state)+' '+esc(String(data.ota.progress||0))+'%. Evita cortar energia o reiniciar el ESP32.')}else if(data.on_battery){setBanner('warn','<strong>Corte de energia detectado.</strong> UPS en bateria hace '+esc(data.outage_text)+'.')}else if(wc){setBanner('ok','<strong>Sistema estable.</strong> WiFi activa, AP de respaldo disponible y panel respondiendo.')}else{setBanner('warn','<strong>Modo contingencia.</strong> La WiFi principal no esta arriba, pero el AP local del ESP32 sigue disponible.')}q('ups-chip').innerHTML=!data.ups.usb_ready?tag('USB desconectado','t-muted'):(data.ups.read_stale?tag('Lectura vencida','t-warn'):(data.on_battery?tag('Bateria','t-warn'):tag(data.ups.state_known?'Linea':'Esperando lectura',data.ups.state_known?'t-ok':'t-muted')));q('ups-body').innerHTML=[row('UPS USB',data.ups.usb_ready?tag('Detectada','t-ok'):tag('No conectada','t-muted')),row('Estado',data.ups.state_label?esc(data.ups.state_label):'--'),row('Lectura',data.ups.read_stale?tag('Vencida','t-warn'):(data.ups.state_known?tag('Reciente','t-ok'):tag('Sin dato','t-muted'))),row('Codigo raw',esc(String(data.ups.code))),row('Ultima lectura',esc(data.ups.last_seen_text||'--')),row('Temporizador STOP',esc(data.stop_delay_text||'--')),row('Corte actual',data.on_battery?esc(data.outage_text):'--')].join('');q('net-chip').innerHTML=wc?tag('WiFi arriba','t-ok'):tag('STA abajo','t-warn');q('net-body').innerHTML=[row('Estado STA',esc(data.wifi.status)),row('IP STA','<code>'+esc(data.wifi.sta_ip||'--')+'</code>'),row('RSSI',wc?esc(String(data.wifi.rssi)+' dBm'):'--'),row('AP fallback','<code>'+esc(data.wifi.ap_ip||'--')+'</code>'),row('mDNS',data.wifi.mdns_url?'<a href="'+esc(data.wifi.mdns_url)+'">'+esc(data.wifi.mdns_url)+'</a>':'--')].join('');q('sys-chip').innerHTML=tag(data.current_reset_reason,'t-muted');q('sys-body').innerHTML=[row('Uptime',esc(data.uptime_text)),row('Hora UTC',data.time&&data.time.synced?esc(data.time.iso_utc):tag('Sin NTP','t-muted')),row('Flash / PSRAM',esc(String(data.hardware.flash_mb)+' MB / '+String(data.hardware.psram_mb)+' MB')),row('Heap libre',esc(String(data.hardware.heap_free_kb)+' KB')),row('STOP entrega',esc(String(delivered)+'/'+String(total)+' transmitidas')),row('Ultimo STOP',data.last_stop_request_ms?esc(fmt(data.last_stop_request_ms)+' desde boot'):'--'),row('Auth critica',data.auth&&data.auth.critical_required?tag('Activa','t-warn'):tag('Desactivada','t-muted'))].join('');q('stop-chip').innerHTML=data.stop_active?(exhausted?tag('Sin ACK','t-err'):tag('Latch activo','t-err')):tag('Libre','t-ok');var wsDown=false;printers.forEach(function(p){if(!p.connected)wsDown=true});q('tbdg-imp').style.display=wsDown?'':'none';q('tbdg-ota').style.display=data.ota&&data.ota.update_available?'':'none';renderCountdown(data);renderPrinters(printers);renderWsDiscovery(data.ws_discovery||[]);renderOta(data.ota||{});renderSupport(data);renderLogs(data.logs||[])}
function nextDelay(){if(D.hidden)return 9000;if(!STATE)return 2500;if(STATE.ota&&STATE.ota.busy)return 1500;if(STATE.stop_active||STATE.on_battery)return 1500;return 4000}
function scheduleRefresh(){if(refreshTimer)clearTimeout(refreshTimer);refreshTimer=setTimeout(refresh,nextDelay())}
async function refresh(){try{render(await api('/api/status','GET',true))}catch(err){setBanner('error','<strong>Sin respuesta del ESP32.</strong> '+esc(err.message))}finally{scheduleRefresh()}}
function tick(){if(!D.hidden&&STATE)renderCountdown(STATE)}
async function runAction(url,critical,btn,confirmText,danger){try{if(confirmText&&!(await askConfirm(confirmText,!!danger)))return;if(critical&&STATE&&STATE.auth&&STATE.auth.critical_required&&!(AUTH&&AUTH.user)){if(!(await ensureAuth()))return}if(btn)btn.disabled=true;await api(url,'POST',!!critical);await refresh()}catch(err){setBanner('error','<strong>Error.</strong> '+esc(err.message))}finally{if(btn)btn.disabled=false}}
async function runProbe(btn){try{if(STATE&&STATE.auth&&STATE.auth.critical_required&&!(AUTH&&AUTH.user)){if(!(await ensureAuth()))return}btn.disabled=true;await api('/api/ws/probe','POST',true,{printer:parseInt(btn.dataset.printer,10),probe:btn.dataset.probe});await refresh()}catch(err){setBanner('error','<strong>Probe WS.</strong> '+esc(err.message))}finally{btn.disabled=false}}
async function clearWsDiscovery(){try{await api('/api/ws/discovery/clear','POST',true);await refresh()}catch(err){setBanner('error','<strong>Descubrimiento WS.</strong> '+esc(err.message))}}
function cfgChip(cls,txt){var el=q('cfg-chip');el.className='tag '+cls;el.textContent=txt}
async function loadCfg(){try{var c=await api('/api/config','GET',true);q('c-p1').value=c.printer1_host||'';q('c-p2').value=c.printer2_host||'';q('c-wsp').value=c.ws_port||'';q('c-del').value=c.ups_stop_delay_s||'';q('c-ota').value=c.ota_metadata_url||'';cfgChip('t-muted','Cargado')}catch(err){cfgChip('t-err','Error al cargar')}}
async function saveCfg(){q('btn-cfg-save').disabled=true;try{var body={printer1_host:q('c-p1').value.trim(),printer2_host:q('c-p2').value.trim(),ws_port:parseInt(q('c-wsp').value,10)||0,ups_stop_delay_s:parseInt(q('c-del').value,10)||0,ota_metadata_url:q('c-ota').value.trim()};await api('/api/config','POST',true,body);cfgChip('t-ok','Guardado');await refresh()}catch(err){cfgChip('t-err',err.message||'Error');setBanner('error','<strong>Config:</strong> '+esc(err.message))}finally{q('btn-cfg-save').disabled=false}}
function setTab(name){if(!q('tab-'+name))name='estado';[].forEach.call(D.querySelectorAll('.tab'),function(b){var on=b.dataset.tab===name;b.classList.toggle('on',on);b.setAttribute('aria-selected',on?'true':'false')});[].forEach.call(D.querySelectorAll('.tab-page'),function(p){p.classList.toggle('on',p.id==='tab-'+name)});try{sessionStorage.setItem('guardianTab',name)}catch(_){}}
function setupReveal(){var els=[].slice.call(D.querySelectorAll('.reveal'));if(reduce&&reduce.matches){els.forEach(function(el){el.classList.add('on')});return}if('IntersectionObserver' in window){var io=new IntersectionObserver(function(entries){entries.forEach(function(e){if(e.isIntersecting){e.target.classList.add('on');io.unobserve(e.target)}})},{threshold:.12});els.forEach(function(el){io.observe(el)})}else els.forEach(function(el){el.classList.add('on')})}
var ticking=false;function updateParallax(){if(!(reduce&&reduce.matches)&&!D.hidden){var y=window.scrollY||0;root.style.setProperty('--p1',(-Math.min(42,y*.05)).toFixed(1)+'px');root.style.setProperty('--p2',(Math.min(30,y*.025)).toFixed(1)+'px')}ticking=false}
function onScroll(){if(!ticking){ticking=true;requestAnimationFrame(updateParallax)}}
function motionState(){root.classList.toggle('paused',D.hidden||!!(reduce&&reduce.matches));scheduleRefresh();updateParallax()}
D.querySelectorAll('[data-action]').forEach(function(btn){btn.addEventListener('click',function(){runAction(btn.dataset.action,true,btn,btn.dataset.confirm||'',btn.classList.contains('btn-stop')||btn.classList.contains('btn-warn'))})});
D.querySelectorAll('[data-probe]').forEach(function(btn){btn.addEventListener('click',function(){runProbe(btn)})});
q('btn-check').addEventListener('click',function(){runAction('/api/update/check',true)});q('btn-install').addEventListener('click',function(){runAction('/api/update/start',true,null,'Se descargara e instalara la nueva version del firmware y el ESP32 se reiniciara al terminar. No cortes la energia durante el proceso.',false)});
async function uploadFirmware(file,btn){try{if(STATE&&STATE.auth&&STATE.auth.critical_required&&!(AUTH&&AUTH.user)){if(!(await ensureAuth()))return}
btn.disabled=true;btn.innerHTML='<span class="spin"></span> Subiendo...';
var fd=new FormData();fd.append('firmware',file,file.name);
var options={method:'POST',cache:'no-store',credentials:'same-origin',headers:{},body:fd};
if(AUTH&&AUTH.user)Object.assign(options.headers,authHeader());
var r=await fetch('/api/update/upload',options);
if(r.status===401){clearAuth();if(!(await askAuth('Usuario o contrasena incorrectos. Intenta nuevamente.')))throw new Error('Autenticacion cancelada');Object.assign(options.headers,authHeader());r=await fetch('/api/update/upload',options)}
if(!r.ok){var t='HTTP '+r.status;try{var x=await r.json();if(x&&x.error)t=x.error}catch(_){}throw new Error(t)}
setBanner('ok','<strong>Firmware subido y verificado.</strong> El ESP32 se esta reiniciando; el panel volvera en unos segundos.')}
catch(err){setBanner('error','<strong>Error.</strong> '+esc(err.message))}
finally{btn.disabled=false;btn.textContent='Subir firmware (.bin)'}}
q('btn-upload').addEventListener('click',function(){q('up-file').click()});
q('up-file').addEventListener('change',async function(){var file=this.files[0];this.value='';if(!file)return;
if(!/\.bin$/i.test(file.name)){setBanner('error','<strong>Archivo invalido.</strong> Selecciona un firmware .bin generado para este equipo.');return}
if(await askConfirm('Se flasheara "'+file.name+'" ('+bytes(file.size)+') directamente en el ESP32 y se reiniciara al terminar. Usa solo binarios compilados para este equipo (guardian-n16r8). No cortes la energia durante el proceso.',true))uploadFirmware(file,q('btn-upload'))});
q('btn-cfg-save').addEventListener('click',saveCfg);q('btn-cfg-discard').addEventListener('click',loadCfg);
q('btn-diag-open').addEventListener('click',openDiagnostics);q('btn-diag-download').addEventListener('click',downloadDiagnostics);
q('btn-wsd-clear').addEventListener('click',clearWsDiscovery);
D.querySelectorAll('.tab').forEach(function(b){b.addEventListener('click',function(){setTab(b.dataset.tab)})});
var tab0='estado';try{tab0=sessionStorage.getItem('guardianTab')||'estado'}catch(_){}setTab(tab0);
setupReveal();window.addEventListener('scroll',onScroll,{passive:true});D.addEventListener('visibilitychange',motionState);if(reduce&&reduce.addEventListener)reduce.addEventListener('change',motionState);setInterval(tick,1000);motionState();refresh();loadCfg();
})();
</script>
</body>
</html>

)HTML";

// =============================================================================
// 6. HELPERS
// =============================================================================
static void copyText(char* dest, size_t destSize, const char* src) {
  if (destSize == 0) {
    return;
  }
  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }
  snprintf(dest, destSize, "%s", src);
}

static bool textEmpty(const char* text) {
  return text == nullptr || text[0] == '\0';
}

static bool startsWithText(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) {
    return false;
  }
  return strncmp(value, prefix, strlen(prefix)) == 0;
}

static bool isHttpsUrl(const char* url) {
  return startsWithText(url, "https://");
}

static void sanitizePayload(const uint8_t* payload, size_t length, char* dest, size_t destSize) {
  if (destSize == 0) {
    return;
  }
  if (payload == nullptr || length == 0) {
    dest[0] = '\0';
    return;
  }
  const size_t copyLen = (length < (destSize - 1)) ? length : (destSize - 1);
  memcpy(dest, payload, copyLen);
  dest[copyLen] = '\0';
  for (size_t i = 0; i < copyLen; ++i) {
    const unsigned char ch = static_cast<unsigned char>(dest[i]);
    if (ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') {
      dest[i] = ' ';
    }
  }
}

static void formatDurationToBuf(uint32_t ms, char* buffer, size_t bufferSize) {
  const uint32_t totalSeconds = ms / 1000;
  const uint32_t days = totalSeconds / 86400;
  const uint32_t hours = (totalSeconds % 86400) / 3600;
  const uint32_t minutes = (totalSeconds % 3600) / 60;
  const uint32_t seconds = totalSeconds % 60;

  if (days > 0) {
    snprintf(buffer, bufferSize, "%lud %02luh %02lum %02lus",
             static_cast<unsigned long>(days),
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  } else if (hours > 0) {
    snprintf(buffer, bufferSize, "%luh %02lum %02lus",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  } else if (minutes > 0) {
    snprintf(buffer, bufferSize, "%lum %02lus",
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  } else {
    snprintf(buffer, bufferSize, "%lus", static_cast<unsigned long>(seconds));
  }
}

static void formatSinceBootToBuf(uint32_t timestampMs, char* buffer, size_t bufferSize) {
  if (timestampMs == 0) {
    copyText(buffer, bufferSize, "-");
    return;
  }
  formatDurationToBuf(timestampMs, buffer, bufferSize);
}

static bool currentTimeIsoUtc(char* buffer, size_t bufferSize) {
  const time_t now = time(nullptr);
  if (now < 1700000000) {
    buffer[0] = '\0';
    return false;
  }

  struct tm tmUtc;
  gmtime_r(&now, &tmUtc);
  strftime(buffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
  return true;
}

static const char* wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "ssid no disponible";
    case WL_CONNECTED:       return "conectada";
    case WL_CONNECT_FAILED:  return "fallo de conexion";
    case WL_CONNECTION_LOST: return "conexion perdida";
    case WL_DISCONNECTED:    return "desconectada";
    default:                 return "desconocida";
  }
}

static const char* resetReasonToText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external_reset";
    case ESP_RST_SW:        return "software_reset";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio_reset";
    default:                return "otro_reset";
  }
}

static const char* eventSeverityText(EventSeverity severity) {
  switch (severity) {
    case EventSeverity::Info:    return "info";
    case EventSeverity::Success: return "success";
    case EventSeverity::Warning: return "warning";
    case EventSeverity::Error:   return "error";
    default:                     return "info";
  }
}

static const char* otaStateText(OtaState state) {
  switch (state) {
    case OtaState::Idle:        return "idle";
    case OtaState::Checking:    return "checking";
    case OtaState::Available:   return "available";
    case OtaState::UpToDate:    return "up_to_date";
    case OtaState::Downloading: return "downloading";
    case OtaState::Flashing:    return "flashing";
    case OtaState::Success:     return "success";
    case OtaState::Error:       return "error";
    default:                    return "idle";
  }
}

static const char* otaStateLabel(OtaState state) {
  switch (state) {
    case OtaState::Idle:        return "Inactivo";
    case OtaState::Checking:    return "Consultando";
    case OtaState::Available:   return "Disponible";
    case OtaState::UpToDate:    return "Al dia";
    case OtaState::Downloading: return "Descargando";
    case OtaState::Flashing:    return "Flasheando";
    case OtaState::Success:     return "Instalado";
    case OtaState::Error:       return "Error";
    default:                    return "Inactivo";
  }
}

static const char* stopAckText(StopAckState state) {
  switch (state) {
    case StopAckState::None:      return "none";
    case StopAckState::Pending:   return "pending";
    case StopAckState::Hint:      return "hint";
    case StopAckState::Exhausted: return "exhausted";
    default:                      return "none";
  }
}

static bool otaBusy() {
  const OtaState state = otaRuntime.state;
  return state == OtaState::Checking || state == OtaState::Downloading || state == OtaState::Flashing;
}

static void markStateDirty() {
  stateDirty = true;
}

static void logEvent(EventSeverity severity, const char* fmt, ...) {
  // Formatear el mensaje antes de tomar el mutex (vsnprintf puede tardar)
  char message[EVENT_TEXT_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  // Proteger el ring buffer ante acceso concurrente desde tareas OTA (core 1) y el handler WiFi (core 0)
  if (g_logMutex) xSemaphoreTake(g_logMutex, portMAX_DELAY);

  copyText(lastEvent, sizeof(lastEvent), message);          // copia al campo "ultimo evento" del estado global
  eventLog[eventHead].ms = millis();
  eventLog[eventHead].severity = severity;
  copyText(eventLog[eventHead].text, sizeof(eventLog[eventHead].text), message);
  if (!currentTimeIsoUtc(eventLog[eventHead].isoUtc, sizeof(eventLog[eventHead].isoUtc))) {
    eventLog[eventHead].isoUtc[0] = '\0';
  }

  eventHead = (eventHead + 1) % EVENT_LOG_CAPACITY;        // avanzar cabeza del ring buffer (circular)
  if (eventCount < EVENT_LOG_CAPACITY) {
    ++eventCount;
  }

  if (g_logMutex) xSemaphoreGive(g_logMutex);

  // Serial fuera del mutex: la API Arduino Serial ya tiene su propio lock interno
  const char* prefix = "";
  switch (severity) {
    case EventSeverity::Success: prefix = "[OK] "; break;
    case EventSeverity::Warning: prefix = "[WARN] "; break;
    case EventSeverity::Error:   prefix = "[ERR] "; break;
    default: break;
  }
  Serial.printf("[%10lu ms] %s%s\n", static_cast<unsigned long>(millis()), prefix, message);
}

static void recordWsDiscovery(size_t printerIndex, const char* direction, const char* text) {
  if (printerIndex >= PRINTER_COUNT || text == nullptr) {
    return;
  }

  WsDiscoveryEntry& entry = wsDiscoveryLog[wsDiscoveryHead];
  entry.ms = millis();
  entry.printerIndex = static_cast<uint8_t>(printerIndex);
  copyText(entry.direction, sizeof(entry.direction), direction);
  copyText(entry.text, sizeof(entry.text), text);
  wsDiscoveryHead = (wsDiscoveryHead + 1) % WS_DISCOVERY_LOG_CAPACITY;
  if (wsDiscoveryCount < WS_DISCOVERY_LOG_CAPACITY) {
    ++wsDiscoveryCount;
  }
}

static void clearWsDiscoveryLog() {
  wsDiscoveryCount = 0;
  wsDiscoveryHead = 0;
  for (size_t i = 0; i < WS_DISCOVERY_LOG_CAPACITY; ++i) {
    wsDiscoveryLog[i].ms = 0;
    wsDiscoveryLog[i].printerIndex = 0;
    wsDiscoveryLog[i].direction[0] = '\0';
    wsDiscoveryLog[i].text[0] = '\0';
  }
}

static bool allPrintersStopSent() {
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    if (!printers[i].stopSent) {
      return false;
    }
  }
  return true;
}

static uint8_t stopDeliveredPrinterCount() {
  if (!stopActive) {
    return 0;
  }
  uint8_t count = 0;
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    if (printers[i].stopSent) {
      ++count;
    }
  }
  return count;
}

static uint8_t stopPendingPrinterCount() {
  if (!stopActive) {
    return 0;
  }
  uint8_t count = 0;
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    if (!printers[i].stopSent) {
      ++count;
    }
  }
  return count;
}

static uint8_t stopExhaustedPrinterCount() {
  if (!stopActive) {
    return 0;
  }
  uint8_t count = 0;
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    if (printers[i].ackState == StopAckState::Exhausted) {
      ++count;
    }
  }
  return count;
}

static uint8_t disconnectedPrinterCount() {
  uint8_t count = 0;
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    if (!printers[i].connected) {
      ++count;
    }
  }
  return count;
}

static bool upsReadingStale(uint32_t now) {
  return upsStateKnown && lastUpsSeenMs != 0 && (now - lastUpsSeenMs) > UPS_STALE_READ_MS;
}

static void clearOtaError() {
  if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
  otaRuntime.lastError[0] = '\0';
  if (g_otaMutex) xSemaphoreGive(g_otaMutex);
}

static void setOtaError(const char* errorText) {
  if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
  otaRuntime.state = OtaState::Error;
  otaRuntime.progress = 0;
  copyText(otaRuntime.lastError, sizeof(otaRuntime.lastError), errorText);
  if (g_otaMutex) xSemaphoreGive(g_otaMutex);
  logEvent(EventSeverity::Error, "[OTA] %s", errorText);
}

static void setOtaState(OtaState state, uint8_t progress = 0) {
  if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
  otaRuntime.state = state;
  otaRuntime.progress = progress;
  if (g_otaMutex) xSemaphoreGive(g_otaMutex);
}

static bool criticalAuthEnabled() {
  return WEB_AUTH_USERNAME[0] != '\0' && WEB_AUTH_PASSWORD[0] != '\0';
}

static bool authorizeCriticalRequest() {
  // Si las credenciales estan vacias, la auth esta desactivada (modo desarrollo)
  if (!criticalAuthEnabled()) {
    return true;
  }
  // WebServer verifica la cabecera Authorization: Basic base64(user:pass)
  if (server.authenticate(WEB_AUTH_USERNAME, WEB_AUTH_PASSWORD)) {
    return true;
  }
  // Solo loguear cuando el cliente envio credenciales incorrectas. El desafio inicial
  // (peticion sin cabecera Authorization) es parte normal del flujo Basic Auth y
  // registrarlo inundaria el ring buffer de eventos en cada carga del panel.
  if (server.header("Authorization").length() > 0) {
    logEvent(EventSeverity::Warning, "[AUTH] Credenciales invalidas en %s desde %s.",
             server.uri().c_str(), server.client().remoteIP().toString().c_str());
  }
  // Responde 401 con WWW-Authenticate para que el browser y el JS del panel pidan credenciales
  server.requestAuthentication(BASIC_AUTH, PROJECT_NAME, "Credenciales requeridas");
  return false;
}

static void otaAuthHeaderValue(char* buffer, size_t bufferSize) {
  if (textEmpty(OTA_AUTH_TOKEN)) {
    buffer[0] = '\0';
    return;
  }
  if (textEmpty(OTA_AUTH_PREFIX)) {
    snprintf(buffer, bufferSize, "%s", OTA_AUTH_TOKEN);
  } else {
    snprintf(buffer, bufferSize, "%s %s", OTA_AUTH_PREFIX, OTA_AUTH_TOKEN);
  }
}

static bool normalizeSha256Hex(const char* text, char* dest, size_t destSize) {
  if (text == nullptr || destSize < SHA256_HEX_SIZE) {
    return false;
  }

  if (startsWithText(text, "sha256:")) {
    text += 7;
  }

  size_t out = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      continue;
    }
    if (!isxdigit(ch) || out >= 64) {
      return false;
    }
    dest[out++] = static_cast<char>(tolower(ch));
  }

  if (out != 64) {
    return false;
  }
  dest[64] = '\0';
  return true;
}

static void sha256BytesToHex(const uint8_t* bytes, char* dest, size_t destSize) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  if (destSize < SHA256_HEX_SIZE) {
    return;
  }
  for (size_t i = 0; i < 32; ++i) {
    dest[i * 2] = HEX_DIGITS[(bytes[i] >> 4) & 0x0F];
    dest[(i * 2) + 1] = HEX_DIGITS[bytes[i] & 0x0F];
  }
  dest[64] = '\0';
}

static bool containsIgnoreCase(const char* text, const char* needle) {
  if (text == nullptr || needle == nullptr || needle[0] == '\0') {
    return false;
  }
  const size_t textLen = strlen(text);
  const size_t needleLen = strlen(needle);
  if (needleLen > textLen) {
    return false;
  }

  for (size_t i = 0; i + needleLen <= textLen; ++i) {
    bool match = true;
    for (size_t j = 0; j < needleLen; ++j) {
      if (tolower(static_cast<unsigned char>(text[i + j])) != tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

static bool equalsIgnoreCase(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  while (*left != '\0' && *right != '\0') {
    if (tolower(static_cast<unsigned char>(*left)) != tolower(static_cast<unsigned char>(*right))) {
      return false;
    }
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static bool keyMatchesAny(const char* key, const char* const* keys, size_t keyCount) {
  if (key == nullptr || keys == nullptr) {
    return false;
  }
  for (size_t i = 0; i < keyCount; ++i) {
    if (equalsIgnoreCase(key, keys[i])) {
      return true;
    }
  }
  return false;
}

static bool jsonStringToBuffer(JsonVariantConst value, char* dest, size_t destSize) {
  if (value.is<const char*>()) {
    copyText(dest, destSize, value.as<const char*>());
    return !textEmpty(dest);
  }
  if (value.is<String>()) {
    copyText(dest, destSize, value.as<String>().c_str());
    return !textEmpty(dest);
  }
  return false;
}

static bool extractStringByKey(JsonVariantConst node, const char* const* keys, size_t keyCount, char* dest, size_t destSize, uint8_t depth = 0) {
  if (depth > 6 || destSize == 0) {
    return false;
  }

  if (node.is<JsonObjectConst>()) {
    JsonObjectConst object = node.as<JsonObjectConst>();
    for (JsonPairConst pair : object) {
      if (keyMatchesAny(pair.key().c_str(), keys, keyCount) && jsonStringToBuffer(pair.value(), dest, destSize)) {
        return true;
      }
    }
    for (JsonPairConst pair : object) {
      if (extractStringByKey(pair.value(), keys, keyCount, dest, destSize, depth + 1)) {
        return true;
      }
    }
  } else if (node.is<JsonArrayConst>()) {
    JsonArrayConst array = node.as<JsonArrayConst>();
    for (JsonVariantConst item : array) {
      if (extractStringByKey(item, keys, keyCount, dest, destSize, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

static bool extractNumberByKey(JsonVariantConst node, const char* const* keys, size_t keyCount, float& value, uint8_t depth = 0) {
  if (depth > 6) {
    return false;
  }

  if (node.is<JsonObjectConst>()) {
    JsonObjectConst object = node.as<JsonObjectConst>();
    for (JsonPairConst pair : object) {
      if (keyMatchesAny(pair.key().c_str(), keys, keyCount) && pair.value().is<float>()) {
        value = pair.value().as<float>();
        return true;
      }
    }
    for (JsonPairConst pair : object) {
      if (extractNumberByKey(pair.value(), keys, keyCount, value, depth + 1)) {
        return true;
      }
    }
  } else if (node.is<JsonArrayConst>()) {
    JsonArrayConst array = node.as<JsonArrayConst>();
    for (JsonVariantConst item : array) {
      if (extractNumberByKey(item, keys, keyCount, value, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

static bool extractNumberFromNamedObject(JsonVariantConst node,
                                         const char* const* objectKeys,
                                         size_t objectKeyCount,
                                         const char* const* valueKeys,
                                         size_t valueKeyCount,
                                         float& value,
                                         uint8_t depth = 0) {
  if (depth > 6) {
    return false;
  }

  if (node.is<JsonObjectConst>()) {
    JsonObjectConst object = node.as<JsonObjectConst>();
    for (JsonPairConst pair : object) {
      if (keyMatchesAny(pair.key().c_str(), objectKeys, objectKeyCount)
          && extractNumberByKey(pair.value(), valueKeys, valueKeyCount, value, depth + 1)) {
        return true;
      }
    }
    for (JsonPairConst pair : object) {
      if (extractNumberFromNamedObject(pair.value(), objectKeys, objectKeyCount, valueKeys, valueKeyCount, value, depth + 1)) {
        return true;
      }
    }
  } else if (node.is<JsonArrayConst>()) {
    JsonArrayConst array = node.as<JsonArrayConst>();
    for (JsonVariantConst item : array) {
      if (extractNumberFromNamedObject(item, objectKeys, objectKeyCount, valueKeys, valueKeyCount, value, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

static int parseVersionComponent(const char*& text) {
  int value = 0;
  while (*text != '\0' && !isdigit(static_cast<unsigned char>(*text)) && *text != '.') {
    ++text;
  }
  while (isdigit(static_cast<unsigned char>(*text))) {
    value = (value * 10) + (*text - '0');
    ++text;
  }
  while (*text != '\0' && *text != '.') {
    ++text;
  }
  if (*text == '.') {
    ++text;
  }
  return value;
}

static int compareVersions(const char* left, const char* right) {
  const char* a = left;
  const char* b = right;
  for (int i = 0; i < 4; ++i) {
    const int av = parseVersionComponent(a);
    const int bv = parseVersionComponent(b);
    if (av != bv) {
      return av - bv;
    }
  }
  return 0;
}

static void configureDefaults() {
  // Valores de fabrica: se sobrescriben si existe config guardada en NVS
  copyText(deviceConfig.printerHost1, sizeof(deviceConfig.printerHost1), DEFAULT_PRINTER_IP_1);
  copyText(deviceConfig.printerHost2, sizeof(deviceConfig.printerHost2), DEFAULT_PRINTER_IP_2);
  copyText(deviceConfig.otaMetadataUrl, sizeof(deviceConfig.otaMetadataUrl), OTA_METADATA_URL);
  deviceConfig.wsPort = DEFAULT_WS_PORT;
  deviceConfig.upsStopDelayMs = DEFAULT_UPS_STOP_DELAY_MS;
}

static void loadPersistentState() {
  // Namespace "upsmon" en NVS; false = modo lectura/escritura
  prefs.begin("upsmon", false);
  configureDefaults();

  copyText(deviceConfig.printerHost1, sizeof(deviceConfig.printerHost1),
           prefs.getString("cfg_p1", deviceConfig.printerHost1).c_str());
  copyText(deviceConfig.printerHost2, sizeof(deviceConfig.printerHost2),
           prefs.getString("cfg_p2", deviceConfig.printerHost2).c_str());
  copyText(deviceConfig.otaMetadataUrl, sizeof(deviceConfig.otaMetadataUrl),
           prefs.getString("cfg_meta", deviceConfig.otaMetadataUrl).c_str());
  deviceConfig.wsPort = prefs.getUShort("cfg_wsp", deviceConfig.wsPort);
  deviceConfig.upsStopDelayMs = prefs.getULong("cfg_stopd", deviceConfig.upsStopDelayMs);

  bootCount = prefs.getULong("boot_count", 0) + 1;
  stopActive = prefs.getBool("stop_active", false);
  previousBatteryState = prefs.getBool("on_batt", false);
  lastUpsStatusCode = prefs.getUChar("ups_code", 0);
  lastStopRequestMs = prefs.getULong("last_stop_ms", 0);

  copyText(previousEvent, sizeof(previousEvent), prefs.getString("last_evt", "Sin historial persistido.").c_str());
  copyText(previousResetReason, sizeof(previousResetReason), prefs.getString("rst_reason", "sin dato").c_str());
  copyText(stopReason, sizeof(stopReason), prefs.getString("stop_reason", "ninguno").c_str());
  copyText(otaRuntime.lastSuccessVersion, sizeof(otaRuntime.lastSuccessVersion),
           prefs.getString("ota_last_ok", "").c_str());

  printers[0].stopSent = prefs.getBool("stop_p1", false);
  printers[0].stopAttempts = prefs.getUChar("stop_a1", printers[0].stopSent ? 1 : 0);
  printers[1].stopSent = prefs.getBool("stop_p2", false);
  printers[1].stopAttempts = prefs.getUChar("stop_a2", printers[1].stopSent ? 1 : 0);

  copyText(printers[0].host, sizeof(printers[0].host), deviceConfig.printerHost1);
  copyText(printers[1].host, sizeof(printers[1].host), deviceConfig.printerHost2);

  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    printers[i].lastStopAttemptMs = 0;
    if (!stopActive) {
      printers[i].ackState = StopAckState::None;
    } else if (printers[i].stopSent && printers[i].stopAttempts >= STOP_MAX_ATTEMPTS) {
      printers[i].ackState = StopAckState::Exhausted;
    } else {
      printers[i].ackState = StopAckState::Pending;
    }
  }

  stopCompletionLogged = allPrintersStopSent();

  otaRuntime.state = OtaState::Idle;
  otaRuntime.progress = 0;
  otaRuntime.updateAvailable = false;
  otaRuntime.secureTransport = false;
  otaRuntime.manifestRequiresTls = false;
  otaRuntime.lastCheckMs = 0;
  otaRuntime.lastSuccessMs = 0;
  otaRuntime.expectedSize = 0;
  otaRuntime.downloadedSize = 0;
  otaRuntime.remoteVersion[0] = '\0';
  otaRuntime.remoteBuild[0] = '\0';
  otaRuntime.binUrl[0] = '\0';
  otaRuntime.expectedSha256[0] = '\0';
  otaRuntime.installedSha256[0] = '\0';
  otaRuntime.changelog[0] = '\0';
  otaRuntime.lastError[0] = '\0';
  otaRuntime.lastCheckedUtc[0] = '\0';
}

static void persistStateIfNeeded(bool force = false) {
  // Debounce de escritura NVS: la flash tiene ciclos de escritura limitados (~100 k)
  if (!force) {
    if (!stateDirty) {
      return;
    }
    if ((millis() - lastPersistMs) < STATE_PERSIST_DEBOUNCE_MS) {
      return; // esperar el debounce para agrupar multiples cambios en una sola escritura
    }
  }

  prefs.putULong("boot_count", bootCount);
  prefs.putBool("stop_active", stopActive);
  prefs.putBool("stop_p1", printers[0].stopSent);
  prefs.putBool("stop_p2", printers[1].stopSent);
  prefs.putUChar("stop_a1", printers[0].stopAttempts);
  prefs.putUChar("stop_a2", printers[1].stopAttempts);
  prefs.putBool("on_batt", onBattery);
  prefs.putUChar("ups_code", lastUpsStatusCode);
  prefs.putULong("last_stop_ms", lastStopRequestMs);
  prefs.putString("last_evt", lastEvent);
  prefs.putString("stop_reason", stopReason);
  prefs.putString("rst_reason", currentResetReason);
  prefs.putString("ota_last_ok", otaRuntime.lastSuccessVersion);

  stateDirty = false;
  lastPersistMs = millis();
}

static void printNetworkInfo() {
  Serial.printf("[WiFi] status=%s STA=%s AP=%s RSSI=%d\n",
                wifiStatusText(WiFi.status()),
                WiFi.localIP().toString().c_str(),
                WiFi.softAPIP().toString().c_str(),
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
}

static void beginTimeSync() {
  if (timeRuntime.configured) {
    return;
  }
  // configTzTime configura el SNTP del IDF con hasta 3 servidores y la zona horaria
  configTzTime(NTP_TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  timeRuntime.configured = true;
  timeRuntime.lastAttemptMs = millis();
  logEvent(EventSeverity::Info, "[NTP] Sincronizacion NTP inicializada.");
}

static bool refreshTimeSync(uint32_t timeoutMs) {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, timeoutMs)) {
    return false;
  }

  timeRuntime.synced = true;
  timeRuntime.lastEpoch = time(nullptr);
  currentTimeIsoUtc(timeRuntime.lastIsoUtc, sizeof(timeRuntime.lastIsoUtc));
  return true;
}

static bool ensureTimeForTls() {
  // Sin TLS valido no se puede verificar el certificado HTTPS del servidor OTA
  if (OTA_ALLOW_INSECURE_HTTPS) {
    return true;
  }
  if (timeRuntime.synced && time(nullptr) > 1700000000) {
    return true; // hora ya sincronizada y valida
  }
  beginTimeSync();

  // Espera bloqueante hasta OTA_TLS_TIME_WAIT_MS; alimentar WDT en cada iteracion
  // (esta funcion corre dentro de otaCheckTask/otaInstallTask que registran la tarea con el WDT)
  const uint32_t deadline = millis() + OTA_TLS_TIME_WAIT_MS;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    esp_task_wdt_reset(); // evitar disparo del WDT durante la espera de NTP (hasta 8 s)
    if (refreshTimeSync(250)) {
      if (timeRuntime.lastIsoUtc[0] != '\0') {
        logEvent(EventSeverity::Success, "[NTP] Hora sincronizada: %s.", timeRuntime.lastIsoUtc);
      }
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // ceder CPU al scheduler en lugar de delay() bloqueante
  }
  return refreshTimeSync(10);
}

static void serviceTimeSync() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!timeRuntime.configured) {
    beginTimeSync();
  }
  if (!timeRuntime.synced && (millis() - timeRuntime.lastAttemptMs) >= TIME_SYNC_RETRY_MS) {
    timeRuntime.lastAttemptMs = millis();
    if (refreshTimeSync(10) && timeRuntime.lastIsoUtc[0] != '\0') {
      logEvent(EventSeverity::Success, "[NTP] Hora sincronizada: %s.", timeRuntime.lastIsoUtc);
    }
  }
}

static void ensureMdnsStarted() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    logEvent(EventSeverity::Success, "[mDNS] Panel disponible en http://%s.local/.", MDNS_HOSTNAME);
    markStateDirty();
  } else {
    logEvent(EventSeverity::Warning, "[mDNS] No se pudo iniciar el servicio mDNS.");
  }
}

static void connectStation(bool force = false) {
  if (WiFi.status() == WL_CONNECTED) {
    wifiFailCount = 0; // conexion activa: resetear el contador del watchdog WiFi
    return;
  }
  const uint32_t now = millis();
  if (!force && (now - lastWiFiAttemptMs) < WIFI_RETRY_INTERVAL_MS) {
    return;
  }
  lastWiFiAttemptMs = now;

  // Watchdog WiFi: si llevamos demasiados intentos sin exito, reiniciar el stack completo
  // Esto recupera el ESP32 de estados internos corruptos del driver WiFi
  if (wifiFailCount >= WIFI_MAX_FAIL_RESET) {
    wifiFailCount = 0;
    logEvent(EventSeverity::Warning, "[WiFi] Watchdog: reiniciando stack tras %u intentos fallidos.", WIFI_MAX_FAIL_RESET);
    WiFi.disconnect(true);            // desconectar y borrar credenciales guardadas en RAM
    vTaskDelay(pdMS_TO_TICKS(300));   // dar tiempo al driver para limpiar conexiones TCP activas
    WiFi.mode(WIFI_AP_STA);           // restaurar modo dual AP+STA
    WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 4); // relanzar AP en canal 6, max 4 clientes
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    logEvent(EventSeverity::Info, "[WiFi] Stack reiniciado, reconectando a %s...", WIFI_SSID);
    return;
  }

  WiFi.disconnect(false); // limpiar estado intermedio (WL_IDLE, WL_NO_SSID_AVAIL, etc.) antes de begin()
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  ++wifiFailCount;
  logEvent(EventSeverity::Info, "[WiFi] Intentando conectar a %s... (intento %u/%u)",
           WIFI_SSID, wifiFailCount, WIFI_MAX_FAIL_RESET);
}

static bool looksLikeStopAck(const char* payload) {
  return containsIgnoreCase(payload, "stop")
      || containsIgnoreCase(payload, "paused")
      || containsIgnoreCase(payload, "pause")
      || containsIgnoreCase(payload, "cancel")
      || containsIgnoreCase(payload, "abort");
}

static void parsePrinterTelemetry(size_t index, const char* payload) {
  if (index >= PRINTER_COUNT || textEmpty(payload)) {
    return;
  }

  const char* p = payload;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    ++p;
  }
  if (*p != '{' && *p != '[') {
    return;
  }

  PrinterTelemetry& telemetry = printers[index].telemetry;
  StaticJsonDocument<1536> doc;
  const DeserializationError err = deserializeJson(doc, p);
  if (err) {
    ++telemetry.jsonErrorCount;
    return;
  }

  ++telemetry.jsonOkCount;
  telemetry.lastParsedMs = millis();
  JsonVariantConst root = doc.as<JsonVariantConst>();

  static const char* const STATE_KEYS[] = {"state", "print_state", "printing_state", "gcode_state"};
  static const char* const STATUS_KEYS[] = {"status", "message", "msg"};
  static const char* const PROGRESS_KEYS[] = {"progress", "print_progress", "percent", "percentage"};
  static const char* const TEMP_VALUE_KEYS[] = {"temperature", "temp", "actual", "value"};
  static const char* const HOTEND_OBJECT_KEYS[] = {"extruder", "tool0", "nozzle", "hotend", "heater_hotend"};
  static const char* const BED_OBJECT_KEYS[] = {"heater_bed", "bed", "bed_temp", "build_plate", "platform"};

  char text[SMALL_TEXT_SIZE];
  if (extractStringByKey(root, STATE_KEYS, sizeof(STATE_KEYS) / sizeof(STATE_KEYS[0]), text, sizeof(text))) {
    copyText(telemetry.state, sizeof(telemetry.state), text);
    telemetry.hasState = true;
  }
  if (extractStringByKey(root, STATUS_KEYS, sizeof(STATUS_KEYS) / sizeof(STATUS_KEYS[0]), text, sizeof(text))) {
    copyText(telemetry.status, sizeof(telemetry.status), text);
    telemetry.hasStatus = true;
  }

  float number = 0.0f;
  if (extractNumberByKey(root, PROGRESS_KEYS, sizeof(PROGRESS_KEYS) / sizeof(PROGRESS_KEYS[0]), number)) {
    if (number >= 0.0f && number <= 1.0f) {
      number *= 100.0f;
    }
    if (number >= 0.0f && number <= 100.0f) {
      telemetry.progressPct = number;
      telemetry.hasProgress = true;
    }
  }
  if (extractNumberFromNamedObject(root,
                                   HOTEND_OBJECT_KEYS,
                                   sizeof(HOTEND_OBJECT_KEYS) / sizeof(HOTEND_OBJECT_KEYS[0]),
                                   TEMP_VALUE_KEYS,
                                   sizeof(TEMP_VALUE_KEYS) / sizeof(TEMP_VALUE_KEYS[0]),
                                   number)
      && number > -20.0f && number < 350.0f) {
    telemetry.hotendTempC = number;
    telemetry.hasHotendTemp = true;
  }
  if (extractNumberFromNamedObject(root,
                                   BED_OBJECT_KEYS,
                                   sizeof(BED_OBJECT_KEYS) / sizeof(BED_OBJECT_KEYS[0]),
                                   TEMP_VALUE_KEYS,
                                   sizeof(TEMP_VALUE_KEYS) / sizeof(TEMP_VALUE_KEYS[0]),
                                   number)
      && number > -20.0f && number < 180.0f) {
    telemetry.bedTempC = number;
    telemetry.hasBedTemp = true;
  }
}

static void markStopAckExhausted(size_t index, uint32_t now) {
  if (index >= PRINTER_COUNT) {
    return;
  }
  PrinterState& printer = printers[index];
  if (!stopActive || !printer.stopSent || printer.ackState != StopAckState::Pending) {
    return;
  }
  if (printer.stopAttempts < STOP_MAX_ATTEMPTS || printer.lastStopSentMs == 0) {
    return;
  }
  if ((now - printer.lastStopSentMs) < STOP_ACK_GRACE_MS) {
    return;
  }

  printer.ackState = StopAckState::Exhausted;
  copyText(printer.lastError, sizeof(printer.lastError), "sin ACK probable tras reintentos STOP");
  logEvent(EventSeverity::Error, "[%s] STOP transmitido %u veces sin ACK probable.",
           printer.name,
           static_cast<unsigned>(printer.stopAttempts));
  markStateDirty();
}

static bool sendStopToPrinter(size_t index, bool forceAttempt) {
  if (index >= PRINTER_COUNT || !stopActive) {
    return false;
  }

  PrinterState& printer = printers[index];
  const uint32_t now = millis();

  if (printer.ackState == StopAckState::Hint) {
    return false;
  }

  if (!printer.connected) {
    return false;
  }

  if (printer.stopAttempts >= STOP_MAX_ATTEMPTS) {
    markStopAckExhausted(index, now);
    return false;
  }

  const uint32_t lastAttemptMs = printer.lastStopAttemptMs != 0 ? printer.lastStopAttemptMs : printer.lastStopSentMs;
  if (!forceAttempt && lastAttemptMs != 0 && (now - lastAttemptMs) < STOP_RETRY_INTERVAL_MS) {
    return false;
  }

  printer.lastStopAttemptMs = now;
  if (!printer.client->sendTXT(STOP_COMMAND)) {
    copyText(printer.lastError, sizeof(printer.lastError), "sendTXT devolvio false");
    printer.connected = false;
    printer.lastDisconnectedMs = now;
    logEvent(EventSeverity::Error, "[%s] Fallo el envio del STOP.", printer.name);
    markStateDirty();
    return false;
  }

  printer.stopSent = true;
  printer.stopAttempts++;
  printer.ackState = StopAckState::Pending;
  printer.lastStopSentMs = now;
  printer.lastAckMs = 0;
  printer.ackHint[0] = '\0';
  printer.lastError[0] = '\0';
  ++printer.telemetry.txCount;
  recordWsDiscovery(index, "tx", STOP_COMMAND);

  if (printer.stopAttempts == 1) {
    logEvent(EventSeverity::Warning, "[%s] STOP enviado por WebSocket.", printer.name);
  } else {
    logEvent(EventSeverity::Warning, "[%s] STOP reenviado (%u/%u).",
             printer.name,
             static_cast<unsigned>(printer.stopAttempts),
             static_cast<unsigned>(STOP_MAX_ATTEMPTS));
  }
  markStateDirty();

  if (!stopCompletionLogged && allPrintersStopSent()) {
    stopCompletionLogged = true;
    logEvent(EventSeverity::Warning, "[STOP] El ESP32 ya transmitio el comando a las dos impresoras.");
    markStateDirty();
  }
  return true;
}

static const char* wsProbePayload(const char* probe) {
  if (probe == nullptr) {
    return nullptr;
  }
  if (strcmp(probe, "status") == 0) {
    return "{\"method\":\"get\",\"params\":{\"status\":1}}";
  }
  if (strcmp(probe, "state") == 0) {
    return "{\"method\":\"get\",\"params\":{\"state\":1}}";
  }
  if (strcmp(probe, "temp") == 0) {
    return "{\"method\":\"get\",\"params\":{\"temperature\":1}}";
  }
  if (strcmp(probe, "info") == 0) {
    return "{\"method\":\"get\",\"params\":{\"info\":1}}";
  }
  return nullptr;
}

static bool sendWsProbeToPrinter(size_t index, const char* probe, char* errorText, size_t errorSize) {
  if (index >= PRINTER_COUNT) {
    snprintf(errorText, errorSize, "printer_index_invalid");
    return false;
  }

  const char* payload = wsProbePayload(probe);
  if (payload == nullptr) {
    snprintf(errorText, errorSize, "probe_invalid");
    return false;
  }

  PrinterState& printer = printers[index];
  if (!printer.connected) {
    snprintf(errorText, errorSize, "printer_not_connected");
    return false;
  }

  if (!printer.client->sendTXT(payload)) {
    snprintf(errorText, errorSize, "send_failed");
    copyText(printer.lastError, sizeof(printer.lastError), "probe sendTXT devolvio false");
    printer.connected = false;
    printer.lastDisconnectedMs = millis();
    markStateDirty();
    return false;
  }

  printer.lastProbeSentMs = millis();
  copyText(printer.lastProbeName, sizeof(printer.lastProbeName), probe);
  ++printer.telemetry.txCount;
  recordWsDiscovery(index, "tx", payload);
  logEvent(EventSeverity::Info, "[%s] Probe WS enviado: %s.", printer.name, probe);
  markStateDirty();
  return true;
}

static void reconnectPrinter(size_t index, bool force = false) {
  if (index >= PRINTER_COUNT || WiFi.status() != WL_CONNECTED) {
    return;
  }
  PrinterState& printer = printers[index];
  const uint32_t now = millis();
  // Throttle de reinicializacion: evitar spam de begin() si el AP de la impresora esta caido
  if (!force && (now - printer.lastConnectAttemptMs) < WS_MANUAL_REINIT_MS) {
    return;
  }
  printer.lastConnectAttemptMs = now;
  printer.client->disconnect();  // cerrar TCP existente antes de renegociar para evitar estado doble
  printer.client->begin(printer.host, deviceConfig.wsPort, WS_PATH);
  printer.client->setReconnectInterval(WS_RECONNECT_INTERVAL_MS); // la lib reintentara cada 5 s si no conecta
  printer.client->enableHeartbeat(WS_HEARTBEAT_INTERVAL_MS, WS_HEARTBEAT_TIMEOUT_MS, WS_HEARTBEAT_RETRIES); // detectar sockets muertos
  logEvent(EventSeverity::Info, "[WS] Reconectando %s hacia %s:%u.", printer.name, printer.host, deviceConfig.wsPort);
}

static void reconnectAllPrinters(bool force = false) {
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    reconnectPrinter(i, force);
  }
}

static void requestStop(const char* reason, bool resetPrinterState = true) {
  // Activar el latch STOP: persiste en NVS para sobrevivir reboots y reintentarse al reconectar
  stopActive = true;
  stopCompletionLogged = false;
  lastStopRequestMs = millis();
  copyText(stopReason, sizeof(stopReason), reason);

  if (resetPrinterState) {
    for (size_t i = 0; i < PRINTER_COUNT; ++i) {
      printers[i].stopSent = false;
      printers[i].stopAttempts = 0;
      printers[i].ackState = StopAckState::Pending;
      printers[i].lastStopSentMs = 0;
      printers[i].lastStopAttemptMs = 0;
      printers[i].lastAckMs = 0;
      printers[i].lastProbeSentMs = 0;
      printers[i].lastError[0] = '\0';
      printers[i].ackHint[0] = '\0';
      printers[i].lastProbeName[0] = '\0';
    }
  }

  lastStopSupervisorLogMs = 0;
  logEvent(EventSeverity::Warning, "[STOP] Latch activado: %s.", stopReason);
  markStateDirty();
}

static void clearStopState(const char* origin) {
  stopActive = false;
  stopCompletionLogged = false;
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    printers[i].stopSent = false;
    printers[i].stopAttempts = 0;
    printers[i].ackState = StopAckState::None;
    printers[i].lastStopSentMs = 0;
    printers[i].lastStopAttemptMs = 0;
    printers[i].lastAckMs = 0;
    printers[i].lastProbeSentMs = 0;
    printers[i].lastError[0] = '\0';
    printers[i].ackHint[0] = '\0';
    printers[i].lastProbeName[0] = '\0';
  }
  logEvent(EventSeverity::Info, "[STOP] Latch limpiado desde %s.", origin);
  markStateDirty();
}

static void handlePrinterEvent(size_t index, WStype_t type, uint8_t* payload, size_t length) {
  if (index >= PRINTER_COUNT) {
    return;
  }

  PrinterState& printer = printers[index];
  char payloadText[WS_DISCOVERY_TEXT_SIZE];
  sanitizePayload(payload, length, payloadText, sizeof(payloadText));

  switch (type) {
    case WStype_CONNECTED:
      printer.connected = true;
      printer.lastConnectedMs = millis();
      printer.lastError[0] = '\0';
      recordWsDiscovery(index, "ev", "connected");
      logEvent(EventSeverity::Success, "[%s] WebSocket conectado.", printer.name);
      markStateDirty();
      sendStopToPrinter(index, true); // si el latch STOP esta activo, enviarlo inmediatamente al reconectar
      break;

    case WStype_DISCONNECTED:
      if (printer.connected) { // solo loggear si estabamos conectados (evitar spam en reconexiones)
        logEvent(EventSeverity::Warning, "[%s] WebSocket desconectado.", printer.name);
      }
      printer.connected = false;
      printer.lastDisconnectedMs = millis();
      recordWsDiscovery(index, "ev", "disconnected");
      markStateDirty();
      break;

    case WStype_ERROR:
      printer.connected = false;
      printer.lastDisconnectedMs = millis();
      copyText(printer.lastError, sizeof(printer.lastError), payloadText[0] ? payloadText : "error websocket");
      recordWsDiscovery(index, "ev", printer.lastError);
      logEvent(EventSeverity::Error, "[%s] Error WS: %s", printer.name, printer.lastError);
      markStateDirty();
      break;

    case WStype_TEXT:
      ++printer.telemetry.rxCount;
      copyText(printer.lastText, sizeof(printer.lastText), payloadText);
      recordWsDiscovery(index, "rx", payloadText);
      parsePrinterTelemetry(index, payloadText);
      // Deteccion heuristica de ACK: buscar palabras clave en la respuesta de la impresora
      if (printer.stopSent && looksLikeStopAck(payloadText)) {
        const bool wasAcked = printer.ackState == StopAckState::Hint;
        printer.ackState = StopAckState::Hint; // Hint = pista, no confirmacion definitiva
        printer.lastAckMs = millis();
        printer.lastError[0] = '\0';
        copyText(printer.ackHint, sizeof(printer.ackHint), payloadText);
        if (!wasAcked) {
          logEvent(EventSeverity::Info, "[%s] ACK STOP probable por WS.", printer.name);
        }
        markStateDirty();
      }
      break;

    default:
      break;
  }
}

static void setupPrinterCallbacks() {
  // Registrar callbacks de eventos WebSocket para cada impresora antes de llamar begin()
  // Los lambdas capturan el indice por valor para enrutar al handler correcto
  printerClient1.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
    handlePrinterEvent(0, type, payload, length);
  });
  printerClient2.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
    handlePrinterEvent(1, type, payload, length);
  });
}

static void scheduleRestart(const char* reason) {
  logEvent(EventSeverity::Warning, "[SISTEMA] Reinicio solicitado: %s.", reason);
  restartRequested = true;
  restartAtMs = millis() + RESTART_DELAY_MS; // diferir para que la respuesta HTTP llegue al cliente antes
  markStateDirty();
}

static void maybeRestart() {
  if (!restartRequested) {
    return;
  }
  if (static_cast<int32_t>(millis() - restartAtMs) < 0) {
    return; // esperar a que expire el delay antes de reiniciar
  }
  persistStateIfNeeded(true); // forzar escritura NVS antes del reboot
  delay(100);
  ESP.restart();
}

// =============================================================================
// 7. UPS
// =============================================================================
class MonitorUPS : public EspUsbHost {
public:
  bool isReady() const {
    return this->deviceHandle != nullptr && this->clientHandle != nullptr;
  }

  void sendQueryStatus() {
    if (!isReady()) {
      return;
    }

    // Reservar transfer de control USB: 8 bytes setup + 8 bytes datos = 16 bytes totales
    usb_transfer_t* transfer = nullptr;
    const esp_err_t allocErr = usb_host_transfer_alloc(16, 0, &transfer);
    if (allocErr != ESP_OK || transfer == nullptr) {
      logEvent(EventSeverity::Error, "[UPS] No pude reservar transfer control: %s.", esp_err_to_name(allocErr));
      return;
    }

    memset(transfer->data_buffer, 0, 16);
    transfer->device_handle = this->deviceHandle;
    transfer->bEndpointAddress = 0;           // endpoint 0 = control pipe
    transfer->callback = [](usb_transfer_t* t) {
      usb_host_transfer_free(t); // liberar inmediatamente; la respuesta llega por el endpoint IN
    };

    // Setup packet HID SET_REPORT (clase 0x21, request 0x09) para enviar el query QS al UPS
    auto* setup = reinterpret_cast<usb_setup_packet_t*>(transfer->data_buffer);
    setup->bmRequestType = 0x21; // host->device, clase, interfaz
    setup->bRequest = 0x09;      // SET_REPORT
    setup->wValue = 0x0200;      // report type=output (0x02), report ID=0
    setup->wIndex = 0x00;        // interfaz 0
    setup->wLength = 8;

    // Comando propietario Cyber Power: 0x51='Q', 0x53='S' -> "QS" (Query Status), 0x0D=CR
    const uint8_t queryCmd[] = {0x51, 0x53, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(&transfer->data_buffer[8], queryCmd, sizeof(queryCmd));
    transfer->num_bytes = 16;

    const esp_err_t submitErr = usb_host_transfer_submit_control(this->clientHandle, transfer);
    if (submitErr != ESP_OK) {
      usb_host_transfer_free(transfer);
      logEvent(EventSeverity::Error, "[UPS] Fallo al enviar consulta QS: %s.", esp_err_to_name(submitErr));
    }
  }

  void listenInterrupt() {
    if (!isReady()) {
      return;
    }

    usb_transfer_t* transferIn = nullptr;
    const esp_err_t allocErr = usb_host_transfer_alloc(64, 0, &transferIn);
    if (allocErr != ESP_OK || transferIn == nullptr) {
      logEvent(EventSeverity::Error, "[UPS] No pude reservar transfer IN: %s.", esp_err_to_name(allocErr));
      return;
    }

    transferIn->device_handle = this->deviceHandle;
    transferIn->bEndpointAddress = 0x81;
    transferIn->num_bytes = 64;

    // Callback del transfer IN: corre en el contexto del USB host task (no en loop())
    // Solo escribir en variables volatiles; no llamar logEvent() ni funciones del loop()
    transferIn->callback = [](usb_transfer_t* t) {
      if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i < t->actual_num_bytes; ++i) {
          // Buscar el byte marcador 0x23 ('#') que precede al codigo de estado en el informe HID
          if (t->data_buffer[i] == 35 && (i + 1) < t->actual_num_bytes) {
            g_pendingUpsCode  = t->data_buffer[i + 1]; // byte siguiente al '#' = codigo de estado
            g_pendingUpsValid = true;                  // senal para el loop() de que hay dato nuevo
            break;
          }
        }
      }
      usb_host_transfer_free(t); // siempre liberar, incluso si el transfer fallo
    };

    const esp_err_t submitErr = usb_host_transfer_submit(transferIn);
    if (submitErr != ESP_OK) {
      usb_host_transfer_free(transferIn);
      logEvent(EventSeverity::Error, "[UPS] Fallo al escuchar endpoint IN: %s.", esp_err_to_name(submitErr));
    }
  }
};

MonitorUPS ups;

static const char* upsStateKey() {
  if (!ups.isReady()) {
    return "usb_disconnected";
  }
  if (!upsStateKnown) {
    return "waiting";
  }
  return onBattery ? "battery" : "line";
}

static const char* upsStateLabel() {
  if (!ups.isReady()) {
    return "UPS USB desconectada";
  }
  if (!upsStateKnown) {
    return "Esperando lectura";
  }
  return onBattery ? "En bateria" : "En linea";
}

// =============================================================================
// 8. OTA
// =============================================================================
static void otaIntegrityBegin() {
  if (g_otaDownloadIntegrity.active) {
    mbedtls_sha256_free(&g_otaDownloadIntegrity.sha);
  }
  mbedtls_sha256_init(&g_otaDownloadIntegrity.sha);
  mbedtls_sha256_starts_ret(&g_otaDownloadIntegrity.sha, 0);
  g_otaDownloadIntegrity.bytes = 0;
  g_otaDownloadIntegrity.active = true;
}

static void otaIntegrityAbort() {
  if (g_otaDownloadIntegrity.active) {
    mbedtls_sha256_free(&g_otaDownloadIntegrity.sha);
  }
  g_otaDownloadIntegrity.active = false;
  g_otaDownloadIntegrity.bytes = 0;
}

static bool otaIntegrityFinish(char* shaHex, size_t shaHexSize) {
  if (!g_otaDownloadIntegrity.active || shaHexSize < SHA256_HEX_SIZE) {
    return false;
  }

  uint8_t digest[32];
  const int finishResult = mbedtls_sha256_finish_ret(&g_otaDownloadIntegrity.sha, digest);
  mbedtls_sha256_free(&g_otaDownloadIntegrity.sha);
  g_otaDownloadIntegrity.active = false;
  if (finishResult != 0) {
    shaHex[0] = '\0';
    return false;
  }

  sha256BytesToHex(digest, shaHex, shaHexSize);
  return true;
}

static esp_err_t otaDownloadHttpEvent(esp_http_client_event_t* event) {
  if (event == nullptr) {
    return ESP_OK;
  }
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr && event->data_len > 0 && g_otaDownloadIntegrity.active) {
    mbedtls_sha256_update_ret(
      &g_otaDownloadIntegrity.sha,
      static_cast<const unsigned char*>(event->data),
      static_cast<size_t>(event->data_len));
    g_otaDownloadIntegrity.bytes += static_cast<uint32_t>(event->data_len);
  }
  return ESP_OK;
}

static esp_err_t otaHttpClientInitCb(esp_http_client_handle_t client) {
  char authValue[MEDIUM_TEXT_SIZE];
  otaAuthHeaderValue(authValue, sizeof(authValue));
  if (!textEmpty(authValue)) {
    const esp_err_t authErr = esp_http_client_set_header(client, OTA_AUTH_HEADER_NAME, authValue);
    if (authErr != ESP_OK) {
      return authErr;
    }
  }
  return esp_http_client_set_header(client, "User-Agent", PROJECT_NAME);
}

static bool beginManifestRequest(HTTPClient& http, WiFiClientSecure& secureClient, WiFiClient& plainClient, char* errorText, size_t errorSize) {
  const bool https = isHttpsUrl(deviceConfig.otaMetadataUrl);
  otaRuntime.manifestRequiresTls = https;

  if (https) {
    if (!OTA_ALLOW_INSECURE_HTTPS && !ensureTimeForTls()) {
      snprintf(errorText, errorSize, "NTP no sincronizado; HTTPS no puede validar certificados");
      return false;
    }
    if (!textEmpty(OTA_CA_CERT_PEM)) {
      secureClient.setCACert(OTA_CA_CERT_PEM);
      otaRuntime.secureTransport = true;
    } else if (OTA_ALLOW_INSECURE_HTTPS) {
      secureClient.setInsecure();
      otaRuntime.secureTransport = false;
      logEvent(EventSeverity::Warning, "[OTA] HTTPS sin verificacion de certificado. Solo para pruebas.");
    } else {
      snprintf(errorText, errorSize, "Define OTA_CA_CERT_PEM o activa OTA_ALLOW_INSECURE_HTTPS");
      return false;
    }
    secureClient.setTimeout((OTA_HTTP_TIMEOUT_MS / 1000) + 2);
    if (!http.begin(secureClient, String(deviceConfig.otaMetadataUrl))) {
      snprintf(errorText, errorSize, "No se pudo abrir metadata HTTPS");
      return false;
    }
  } else {
    otaRuntime.secureTransport = false;
    if (!http.begin(plainClient, String(deviceConfig.otaMetadataUrl))) {
      snprintf(errorText, errorSize, "No se pudo abrir metadata HTTP");
      return false;
    }
  }

  http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);

  char authValue[MEDIUM_TEXT_SIZE];
  otaAuthHeaderValue(authValue, sizeof(authValue));
  if (!textEmpty(authValue)) {
    http.addHeader(OTA_AUTH_HEADER_NAME, authValue);
  }

  return true;
}

static bool fetchOtaManifest(char* errorText, size_t errorSize) {
  if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
  otaRuntime.lastError[0] = '\0';
  otaRuntime.remoteVersion[0] = '\0';
  otaRuntime.remoteBuild[0] = '\0';
  otaRuntime.binUrl[0] = '\0';
  otaRuntime.expectedSha256[0] = '\0';
  otaRuntime.installedSha256[0] = '\0';
  otaRuntime.expectedSize = 0;
  otaRuntime.downloadedSize = 0;
  otaRuntime.changelog[0] = '\0';
  otaRuntime.updateAvailable = false;
  if (g_otaMutex) xSemaphoreGive(g_otaMutex);

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(errorText, errorSize, "Sin WiFi");
    return false;
  }
  if (textEmpty(deviceConfig.otaMetadataUrl)) {
    snprintf(errorText, errorSize, "OTA metadata URL vacia");
    return false;
  }

  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  if (!beginManifestRequest(http, secureClient, plainClient, errorText, errorSize)) {
    return false;
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    snprintf(errorText, errorSize, "HTTP %d al consultar metadata", httpCode);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  static StaticJsonDocument<JSON_MANIFEST_CAP> doc;
  doc.clear();
  const DeserializationError parseError = deserializeJson(doc, body);
  if (parseError) {
    snprintf(errorText, errorSize, "JSON invalido: %s", parseError.c_str());
    return false;
  }

  const char* remoteVersion = doc["version"] | "";
  const char* remoteBuild = doc["build"] | "";
  const char* binUrl = doc["bin_url"] | "";
  const char* manifestProject = doc["project"] | "";
  const uint32_t expectedSize = doc["size"].as<uint32_t>();
  const char* expectedShaRaw = doc["sha256"] | "";
  const char* changelog = doc["changelog"] | "";

  if (textEmpty(remoteVersion) || textEmpty(binUrl)) {
    snprintf(errorText, errorSize, "Manifest incompleto: falta version o bin_url");
    return false;
  }
  if (!textEmpty(manifestProject) && strcmp(manifestProject, PROJECT_NAME) != 0) {
    snprintf(errorText, errorSize, "Manifest no corresponde a %s", PROJECT_NAME);
    return false;
  }
  if (!isHttpsUrl(binUrl)) {
    snprintf(errorText, errorSize, "Manifest invalido: bin_url debe ser HTTPS");
    return false;
  }
  if (expectedSize == 0) {
    snprintf(errorText, errorSize, "Manifest incompleto: falta size");
    return false;
  }
  if (expectedSize > ESP.getFreeSketchSpace()) {
    snprintf(errorText, errorSize, "Firmware demasiado grande: %lu > %lu bytes",
             static_cast<unsigned long>(expectedSize),
             static_cast<unsigned long>(ESP.getFreeSketchSpace()));
    return false;
  }

  char expectedSha[SHA256_HEX_SIZE];
  if (!normalizeSha256Hex(expectedShaRaw, expectedSha, sizeof(expectedSha))) {
    snprintf(errorText, errorSize, "Manifest invalido: sha256 debe tener 64 hex");
    return false;
  }

  if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
  copyText(otaRuntime.remoteVersion, sizeof(otaRuntime.remoteVersion), remoteVersion);
  copyText(otaRuntime.remoteBuild, sizeof(otaRuntime.remoteBuild), remoteBuild);
  copyText(otaRuntime.binUrl, sizeof(otaRuntime.binUrl), binUrl);
  copyText(otaRuntime.expectedSha256, sizeof(otaRuntime.expectedSha256), expectedSha);
  otaRuntime.expectedSize = expectedSize;
  otaRuntime.downloadedSize = 0;
  copyText(otaRuntime.changelog, sizeof(otaRuntime.changelog), changelog);
  otaRuntime.lastCheckMs = millis();
  currentTimeIsoUtc(otaRuntime.lastCheckedUtc, sizeof(otaRuntime.lastCheckedUtc));
  // Update si la version remota es mayor, o si es la misma version con un build mas nuevo
  // (rebuild/hotfix del equipo sin bump de version, ej. 20260706.1 -> 20260706.2).
  const int versionDelta = compareVersions(remoteVersion, FW_VERSION);
  otaRuntime.updateAvailable = versionDelta > 0
      || (versionDelta == 0 && !textEmpty(remoteBuild) && compareVersions(remoteBuild, FW_BUILD) > 0);
  if (g_otaMutex) xSemaphoreGive(g_otaMutex);
  return true;
}

static void otaCheckTask(void*) {
  esp_task_wdt_add(nullptr); // registrar esta tarea en el WDT para detectar bloqueos durante la consulta
  setOtaState(OtaState::Checking, 0);
  clearOtaError();
  logEvent(EventSeverity::Info, "[OTA] Consultando metadata en %s", deviceConfig.otaMetadataUrl);

  char errorText[LAST_TEXT_SIZE];
  if (!fetchOtaManifest(errorText, sizeof(errorText))) {
    setOtaError(errorText);
    esp_task_wdt_delete(nullptr); // desregistrar antes de terminar para no dejar handles huerfanos
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (otaRuntime.updateAvailable) {
    setOtaState(OtaState::Available, 0);
    logEvent(EventSeverity::Success, "[OTA] Update disponible: %s (%s). Local: %s (%s).",
             otaRuntime.remoteVersion, otaRuntime.remoteBuild, FW_VERSION, FW_BUILD);
  } else {
    setOtaState(OtaState::UpToDate, 0);
    logEvent(EventSeverity::Info, "[OTA] Firmware al dia. Local %s (%s) / Remoto %s (%s).",
             FW_VERSION, FW_BUILD, otaRuntime.remoteVersion, otaRuntime.remoteBuild);
  }

  esp_task_wdt_delete(nullptr);
  otaTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void otaInstallTask(void*) {
  esp_task_wdt_add(nullptr); // registrar con WDT: la descarga puede durar varios minutos
  clearOtaError();

  if (WiFi.status() != WL_CONNECTED) {
    setOtaError("Sin WiFi");
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  if (!otaRuntime.updateAvailable || textEmpty(otaRuntime.binUrl)) {
    setOtaError("No hay update disponible");
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  char binUrl[URL_TEXT_SIZE];
  char remoteVersion[SMALL_TEXT_SIZE];
  char expectedSha256[SHA256_HEX_SIZE];
  uint32_t expectedSize = 0;
  if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
  copyText(binUrl, sizeof(binUrl), otaRuntime.binUrl);
  copyText(remoteVersion, sizeof(remoteVersion), otaRuntime.remoteVersion);
  copyText(expectedSha256, sizeof(expectedSha256), otaRuntime.expectedSha256);
  expectedSize = otaRuntime.expectedSize;
  if (g_otaMutex) xSemaphoreGive(g_otaMutex);

  if (!isHttpsUrl(binUrl)) {
    setOtaError("bin_url debe ser HTTPS para esp_https_ota");
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  if (expectedSize == 0 || textEmpty(expectedSha256)) {
    setOtaError("Manifest sin size/sha256; vuelve a consultar metadata");
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  // ensureTimeForTls() alimenta el WDT internamente durante su loop bloqueante
  if (!OTA_ALLOW_INSECURE_HTTPS && !ensureTimeForTls()) {
    setOtaError("NTP no sincronizado para validar TLS");
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  setOtaState(OtaState::Downloading, 1);
  logEvent(EventSeverity::Warning, "[OTA] Iniciando descarga de %s (%lu bytes).",
           remoteVersion,
           static_cast<unsigned long>(expectedSize));

  otaIntegrityBegin();
  esp_http_client_config_t httpConfig = {};
  httpConfig.url = binUrl;                      // URL HTTPS del binario a flashear
  httpConfig.timeout_ms = OTA_HTTP_TIMEOUT_MS;
  httpConfig.keep_alive_enable = true;
  httpConfig.skip_cert_common_name_check = OTA_ALLOW_INSECURE_HTTPS; // false en produccion
  httpConfig.event_handler = otaDownloadHttpEvent; // calcula SHA-256 del .bin completo recibido
  httpConfig.cert_pem = textEmpty(OTA_CA_CERT_PEM) ? nullptr : OTA_CA_CERT_PEM; // CA para validar el servidor

  esp_https_ota_config_t otaConfig = {};
  otaConfig.http_config = &httpConfig;
  otaConfig.http_client_init_cb = otaHttpClientInitCb; // agrega cabeceras de auth al cliente HTTP
  otaConfig.partial_http_download = true;              // descarga en chunks para no agotar la RAM
  otaConfig.max_http_request_size = 4096;
  otaConfig.bulk_flash_erase = false;                  // borrar flash por sectores es mas seguro ante cortes

  esp_https_ota_handle_t otaHandle = nullptr;
  esp_err_t err = esp_https_ota_begin(&otaConfig, &otaHandle); // abre conexion y valida el certificado TLS
  if (err != ESP_OK) {
    char buffer[LAST_TEXT_SIZE];
    snprintf(buffer, sizeof(buffer), "esp_https_ota_begin fallo: %s", esp_err_to_name(err));
    otaIntegrityAbort();
    setOtaError(buffer);
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  const int announcedSize = esp_https_ota_get_image_size(otaHandle);
  if (announcedSize > 0 && static_cast<uint32_t>(announcedSize) != expectedSize) {
    esp_https_ota_abort(otaHandle);
    otaIntegrityAbort();
    char buffer[LAST_TEXT_SIZE];
    snprintf(buffer, sizeof(buffer), "Tamaño OTA no coincide: HTTP %d / manifest %lu",
             announcedSize,
             static_cast<unsigned long>(expectedSize));
    setOtaError(buffer);
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // Bucle de descarga: cada llamada escribe un chunk en la particion OTA inactiva
  while (true) {
    esp_task_wdt_reset(); // alimentar WDT durante la descarga (puede durar decenas de segundos)
    err = esp_https_ota_perform(otaHandle);
    if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      const int totalSize = esp_https_ota_get_image_size(otaHandle);
      const int readSize  = esp_https_ota_get_image_len_read(otaHandle);
      if (readSize >= 0) {
        if (static_cast<uint32_t>(readSize) > expectedSize) {
          esp_https_ota_abort(otaHandle);
          otaIntegrityAbort();
          char buffer[LAST_TEXT_SIZE];
          snprintf(buffer, sizeof(buffer), "Descarga excedio tamaño esperado: %d > %lu",
                   readSize,
                   static_cast<unsigned long>(expectedSize));
          setOtaError(buffer);
          esp_task_wdt_delete(nullptr);
          otaTaskHandle = nullptr;
          vTaskDelete(nullptr);
          return;
        }
        otaRuntime.downloadedSize = static_cast<uint32_t>(readSize);
      }
      if (readSize >= 0) {
        const uint32_t progressBase = expectedSize > 0 ? expectedSize : static_cast<uint32_t>(totalSize);
        const uint8_t pct = progressBase > 0 ? static_cast<uint8_t>((static_cast<uint32_t>(readSize) * 100) / progressBase) : 1;
        otaRuntime.progress = pct > 98 ? 98 : pct; // reservar el 100% para cuando termine el flash
      }
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }
    break;
  }

  if (err != ESP_OK) {
    esp_https_ota_abort(otaHandle); // abortar limpiamente: invalida la particion para evitar boot corrupto
    otaIntegrityAbort();
    char buffer[LAST_TEXT_SIZE];
    snprintf(buffer, sizeof(buffer), "esp_https_ota_perform fallo: %s", esp_err_to_name(err));
    setOtaError(buffer);
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // Verificar que se recibio la imagen completa antes de marcarla como bootable
  if (!esp_https_ota_is_complete_data_received(otaHandle)) {
    esp_https_ota_abort(otaHandle);
    otaIntegrityAbort();
    setOtaError("Imagen OTA incompleta");
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  const int finalReadSize = esp_https_ota_get_image_len_read(otaHandle);
  if (finalReadSize < 0 || static_cast<uint32_t>(finalReadSize) != expectedSize) {
    esp_https_ota_abort(otaHandle);
    otaIntegrityAbort();
    char buffer[LAST_TEXT_SIZE];
    snprintf(buffer, sizeof(buffer), "Tamaño final no coincide: %d / %lu",
             finalReadSize,
             static_cast<unsigned long>(expectedSize));
    setOtaError(buffer);
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  const uint32_t hashedBytes = g_otaDownloadIntegrity.bytes;
  char actualSha256[SHA256_HEX_SIZE];
  if (!otaIntegrityFinish(actualSha256, sizeof(actualSha256))) {
    esp_https_ota_abort(otaHandle);
    setOtaError("No se pudo calcular SHA-256 de descarga");
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  if (hashedBytes != expectedSize) {
    esp_https_ota_abort(otaHandle);
    char buffer[LAST_TEXT_SIZE];
    snprintf(buffer, sizeof(buffer), "SHA-256 cubrio %lu bytes, esperado %lu",
             static_cast<unsigned long>(hashedBytes),
             static_cast<unsigned long>(expectedSize));
    setOtaError(buffer);
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  if (strcmp(actualSha256, expectedSha256) != 0) {
    esp_https_ota_abort(otaHandle);
    if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
    copyText(otaRuntime.installedSha256, sizeof(otaRuntime.installedSha256), actualSha256);
    otaRuntime.downloadedSize = expectedSize;
    if (g_otaMutex) xSemaphoreGive(g_otaMutex);
    setOtaError("SHA-256 de firmware no coincide con manifest");
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  setOtaState(OtaState::Flashing, 99);
  esp_task_wdt_reset();
  err = esp_https_ota_finish(otaHandle); // valida la imagen ESP-IDF y marca la particion como siguiente boot
  if (err != ESP_OK) {
    char buffer[LAST_TEXT_SIZE];
    snprintf(buffer, sizeof(buffer), "esp_https_ota_finish fallo: %s", esp_err_to_name(err));
    setOtaError(buffer);
    esp_task_wdt_delete(nullptr);
    otaTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
  otaRuntime.progress = 100;
  otaRuntime.state = OtaState::Success;
  otaRuntime.updateAvailable = false;
  otaRuntime.downloadedSize = expectedSize;
  otaRuntime.lastSuccessMs = millis();
  copyText(otaRuntime.installedSha256, sizeof(otaRuntime.installedSha256), actualSha256);
  copyText(otaRuntime.lastSuccessVersion, sizeof(otaRuntime.lastSuccessVersion), remoteVersion);
  otaRuntime.lastError[0] = '\0';
  if (g_otaMutex) xSemaphoreGive(g_otaMutex);
  markStateDirty();
  logEvent(EventSeverity::Success, "[OTA] Firmware v%s verificado e instalado. Reiniciando...", remoteVersion);

  vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
  persistStateIfNeeded(true);    // guardar estado antes del reboot para conservar boot_count, etc.
  esp_task_wdt_delete(nullptr);
  ESP.restart();

  otaTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// =============================================================================
// 9. WEB API
// =============================================================================
static void jsonSend(const JsonDocument& doc, int statusCode = 200) {
  // Buffer estatico (en .bss, no en heap) para evitar fragmentacion de memoria
  static char jsonBuf[JSON_SEND_BUFFER_CAPACITY];
  serializeJson(doc, jsonBuf, sizeof(jsonBuf));
  server.sendHeader("Cache-Control", "no-store, max-age=0"); // evitar que el browser cachee estado dinamico
  server.send(statusCode, "application/json", jsonBuf);
}

static void sendActionResponse(const char* action, bool ok = true) {
  StaticJsonDocument<256> doc;
  doc["ok"] = ok;
  doc["action"] = action;
  doc["last_event"] = lastEvent;
  jsonSend(doc, ok ? 200 : 400);
}

static void sendJsonError(int statusCode, const char* error) {
  StaticJsonDocument<256> doc;
  doc["ok"] = false;
  doc["error"] = error;
  jsonSend(doc, statusCode);
}

// Estado del upload directo de firmware por el panel (POST /api/update/upload).
// El body multipart llega en chunks dentro de UNA sola llamada a server.handleClient(),
// por lo que el loop() no corre entre chunks: hay que alimentar el WDT manualmente.
static bool g_uploadAuthorized = false; // credenciales validadas en UPLOAD_FILE_START
static bool g_uploadSucceeded  = false; // Update.end() valido y reinicio agendado
static bool g_uploadRejected   = false; // rechazado (busy/begin fallido): ignorar chunks restantes

static void handleFirmwareUploadChunk() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    g_uploadSucceeded = false;
    g_uploadRejected = false;
    // Autenticar ANTES de tocar la flash; si falla se ignora el body y el handler final responde 401
    g_uploadAuthorized = !criticalAuthEnabled() || server.authenticate(WEB_AUTH_USERNAME, WEB_AUTH_PASSWORD);
    if (!g_uploadAuthorized) {
      logEvent(EventSeverity::Warning, "[OTA] Upload de firmware rechazado: credenciales invalidas (%s).",
               server.client().remoteIP().toString().c_str());
      return;
    }
    if (otaBusy() || otaTaskHandle != nullptr) {
      g_uploadRejected = true;
      setOtaError("Upload rechazado: otra operacion OTA en curso");
      return;
    }
    // UPDATE_SIZE_UNKNOWN: multipart no anuncia el tamano del archivo; Update valida
    // cabecera magica y espacio disponible, y borra sectores a medida que escribe
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      g_uploadRejected = true;
      setOtaError("Upload: no se pudo iniciar la particion OTA");
      return;
    }
    setOtaState(OtaState::Flashing, 1);
    logEvent(EventSeverity::Info, "[OTA] Upload de firmware iniciado: %s.", upload.filename.c_str());
    return;
  }

  if (!g_uploadAuthorized || g_uploadRejected) {
    return; // drenar el body sin escribir nada
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset(); // el loop() no corre durante el upload; alimentar el WDT por chunk
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      g_uploadRejected = true;
      Update.abort();
      setOtaError("Upload: fallo escribiendo en flash");
      return;
    }
    // Progreso estimado contra el tamano del sketch actual (el nuevo suele ser similar)
    const uint32_t estimate = ESP.getSketchSize();
    uint8_t pct = estimate > 0 ? static_cast<uint8_t>((upload.totalSize * 100ULL) / estimate) : 50;
    if (pct > 99) pct = 99;
    if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
    otaRuntime.progress = pct;
    otaRuntime.downloadedSize = upload.totalSize;
    if (g_otaMutex) xSemaphoreGive(g_otaMutex);
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) { // true = aceptar tamano desconocido; valida imagen y cierra particion
      char errorText[LAST_TEXT_SIZE];
      snprintf(errorText, sizeof(errorText), "Upload: imagen invalida (%s)", Update.errorString());
      setOtaError(errorText);
      return;
    }
    g_uploadSucceeded = true;
    setOtaState(OtaState::Success, 100);
    if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
    otaRuntime.lastSuccessMs = millis();
    copyText(otaRuntime.lastSuccessVersion, sizeof(otaRuntime.lastSuccessVersion), "upload manual");
    if (g_otaMutex) xSemaphoreGive(g_otaMutex);
    logEvent(EventSeverity::Success, "[OTA] Firmware subido por panel (%lu bytes). Reiniciando...",
             static_cast<unsigned long>(upload.totalSize));
    markStateDirty();
    scheduleRestart("upload de firmware por panel");
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    setOtaError("Upload abortado por el cliente");
  }
}

static void fillOtaJson(JsonObject ota) {
  // Timeout de 30 ms: si la tarea OTA tiene el mutex, no bloquear el loop del WebServer mas tiempo
  if (g_otaMutex && xSemaphoreTake(g_otaMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  ota["fw_version"] = FW_VERSION;
  ota["fw_build"] = FW_BUILD;
  ota["remote_version"] = otaRuntime.remoteVersion;
  ota["remote_build"] = otaRuntime.remoteBuild;
  ota["update_available"] = otaRuntime.updateAvailable;
  ota["expected_size"] = otaRuntime.expectedSize;
  ota["downloaded_size"] = otaRuntime.downloadedSize;
  ota["expected_sha256"] = otaRuntime.expectedSha256;
  ota["installed_sha256"] = otaRuntime.installedSha256;
  ota["integrity_required"] = true;
  ota["integrity_match"] = !textEmpty(otaRuntime.expectedSha256)
                         && !textEmpty(otaRuntime.installedSha256)
                         && strcmp(otaRuntime.expectedSha256, otaRuntime.installedSha256) == 0;
  ota["state"] = otaStateText(otaRuntime.state);
  ota["state_label"] = otaStateLabel(otaRuntime.state);
  ota["progress"] = otaRuntime.progress;
  ota["last_error"] = otaRuntime.lastError;
  ota["changelog"] = otaRuntime.changelog;
  ota["last_checked_utc"] = otaRuntime.lastCheckedUtc;
  ota["last_success_version"] = otaRuntime.lastSuccessVersion;
  ota["metadata_url"] = deviceConfig.otaMetadataUrl;
  ota["bin_url"] = otaRuntime.binUrl;
  ota["secure_transport"] = otaRuntime.secureTransport;
  ota["busy"] = otaBusy();
  if (g_otaMutex) xSemaphoreGive(g_otaMutex);
}

static void fillHardwareJson(JsonObject hardware) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

  hardware["board"] = ARDUINO_BOARD;
  hardware["chip_model"] = ESP.getChipModel();
  hardware["chip_revision"] = ESP.getChipRevision();
  hardware["flash_size"] = ESP.getFlashChipSize();
  hardware["flash_mb"] = ESP.getFlashChipSize() / (1024 * 1024);
  hardware["psram_size"] = ESP.getPsramSize();
  hardware["psram_free"] = ESP.getFreePsram();
  hardware["psram_mb"] = ESP.getPsramSize() / (1024 * 1024);
  hardware["psram_found"] = psramFound();
  hardware["heap_free"] = ESP.getFreeHeap();
  hardware["heap_min"] = ESP.getMinFreeHeap();
  hardware["heap_max_alloc"] = ESP.getMaxAllocHeap();
  hardware["heap_free_kb"] = ESP.getFreeHeap() / 1024;
  hardware["heap_min_kb"] = ESP.getMinFreeHeap() / 1024;
  hardware["sketch_size"] = ESP.getSketchSize();
  hardware["free_sketch_space"] = ESP.getFreeSketchSpace();
  hardware["ota_partition_count"] = esp_ota_get_app_partition_count();
  hardware["running_partition"] = running ? running->label : "";
  hardware["boot_partition"] = boot ? boot->label : "";
  hardware["next_update_partition"] = next ? next->label : "";
}

static void fillPrinterTelemetryJson(JsonObject object, const PrinterTelemetry& telemetry, bool includeMs) {
  object["rx_count"] = telemetry.rxCount;
  object["tx_count"] = telemetry.txCount;
  object["json_ok_count"] = telemetry.jsonOkCount;
  object["json_error_count"] = telemetry.jsonErrorCount;
  object["parsed"] = telemetry.lastParsedMs != 0;
  object["state"] = telemetry.hasState ? telemetry.state : "";
  object["status"] = telemetry.hasStatus ? telemetry.status : "";
  if (telemetry.hasProgress) {
    object["progress_pct"] = telemetry.progressPct;
  }
  if (telemetry.hasHotendTemp) {
    object["hotend_temp_c"] = telemetry.hotendTempC;
  }
  if (telemetry.hasBedTemp) {
    object["bed_temp_c"] = telemetry.bedTempC;
  }

  char parsedText[32];
  formatSinceBootToBuf(telemetry.lastParsedMs, parsedText, sizeof(parsedText));
  object["last_parsed_text"] = parsedText;
  if (includeMs) {
    object["last_parsed_ms"] = telemetry.lastParsedMs;
  }
}

static void fillWsDiscoveryJson(JsonArray array, bool includeMs) {
  const size_t start = (wsDiscoveryCount < WS_DISCOVERY_LOG_CAPACITY) ? 0 : wsDiscoveryHead;
  for (size_t i = 0; i < wsDiscoveryCount; ++i) {
    const size_t idx = (start + i) % WS_DISCOVERY_LOG_CAPACITY;
    const WsDiscoveryEntry& entry = wsDiscoveryLog[idx];
    JsonObject item = array.createNestedObject();
    item["printer_index"] = entry.printerIndex;
    item["printer"] = entry.printerIndex < PRINTER_COUNT ? printers[entry.printerIndex].name : "";
    item["direction"] = entry.direction;
    item["text"] = entry.text;
    char uptimeText[32];
    formatDurationToBuf(entry.ms, uptimeText, sizeof(uptimeText));
    item["uptime"] = uptimeText;
    if (includeMs) {
      item["ms"] = entry.ms;
    }
  }
}

static void handleStatusApi() {
  if (WEB_AUTH_PROTECT_STATUS && !authorizeCriticalRequest()) return;

  // Documento estatico: no fragmenta el heap; es el endpoint mas llamado (~cada 4 s desde el browser)
  static StaticJsonDocument<JSON_STATUS_CAPACITY> doc;
  doc.clear();

  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const uint32_t uptimeMs = millis();
  const uint32_t outageMs = (onBattery && outageStartedMs != 0) ? (uptimeMs - outageStartedMs) : 0;

  char uptimeText[32];
  char outageText[32];
  char stopDelayText[32];
  formatDurationToBuf(uptimeMs, uptimeText, sizeof(uptimeText));
  formatDurationToBuf(outageMs, outageText, sizeof(outageText));
  formatDurationToBuf(deviceConfig.upsStopDelayMs, stopDelayText, sizeof(stopDelayText));

  doc["fw_version"] = FW_VERSION;
  doc["fw_build"] = FW_BUILD;
  doc["boot_count"] = bootCount;
  doc["uptime_text"] = uptimeText;
  doc["last_event"] = lastEvent;
  doc["stop_active"] = stopActive;
  doc["stop_reason"] = stopReason;
  doc["last_stop_request_ms"] = lastStopRequestMs;
  doc["stop_delivered_count"] = stopDeliveredPrinterCount();
  doc["stop_pending_count"] = stopPendingPrinterCount();
  doc["stop_exhausted_count"] = stopExhaustedPrinterCount();
  doc["stop_max_attempts"] = STOP_MAX_ATTEMPTS;
  doc["stop_retry_interval_s"] = STOP_RETRY_INTERVAL_MS / 1000;
  doc["on_battery"] = onBattery;
  doc["outage_ms"] = outageMs;
  doc["outage_text"] = outageText;
  doc["stop_delay_s"] = deviceConfig.upsStopDelayMs / 1000;
  doc["stop_delay_text"] = stopDelayText;
  doc["current_reset_reason"] = currentResetReason;

  JsonObject auth = doc.createNestedObject("auth");
  auth["critical_required"] = criticalAuthEnabled();

  JsonObject time = doc.createNestedObject("time");
  time["synced"] = timeRuntime.synced;
  time["iso_utc"] = timeRuntime.lastIsoUtc;

  JsonObject upsJson = doc.createNestedObject("ups");
  upsJson["usb_ready"] = ups.isReady();
  upsJson["state_known"] = upsStateKnown;
  upsJson["state"] = upsStateKey();
  upsJson["state_label"] = upsStateLabel();
  upsJson["code"] = lastUpsStatusCode;
  upsJson["last_seen_ms"] = lastUpsSeenMs;
  upsJson["read_stale"] = upsReadingStale(uptimeMs);
  upsJson["stale_after_s"] = UPS_STALE_READ_MS / 1000;
  char upsSeenText[32];
  formatSinceBootToBuf(lastUpsSeenMs, upsSeenText, sizeof(upsSeenText));
  upsJson["last_seen_text"] = upsSeenText;

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["connected"] = wifiConnected;
  wifi["status"] = wifiStatusText(WiFi.status());
  wifi["ssid"] = WIFI_SSID;
  wifi["sta_ip"] = WiFi.localIP().toString();
  wifi["rssi"] = wifiConnected ? WiFi.RSSI() : 0;
  wifi["ap_ssid"] = AP_SSID;
  wifi["ap_ip"] = WiFi.softAPIP().toString();
  wifi["mdns_url"] = mdnsStarted ? (String("http://") + MDNS_HOSTNAME + ".local/") : "";
  wifi["last_disconnect_reason"] = lastWiFiDisconnectReason;

  JsonObject hardware = doc.createNestedObject("hardware");
  fillHardwareJson(hardware);

  JsonObject ota = doc.createNestedObject("ota");
  fillOtaJson(ota);

  JsonArray printerArray = doc.createNestedArray("printers");
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    const PrinterState& printer = printers[i];
    JsonObject item = printerArray.createNestedObject();
    item["name"] = printer.name;
    item["host"] = printer.host;
    item["port"] = deviceConfig.wsPort;
    item["connected"] = printer.connected;
    item["stop_sent"] = printer.stopSent;
    item["stop_attempts"] = printer.stopAttempts;
    item["stop_max_attempts"] = STOP_MAX_ATTEMPTS;
    item["retry_exhausted"] = printer.ackState == StopAckState::Exhausted;
    item["ack_state"] = stopAckText(printer.ackState);
    item["last_text"] = printer.lastText;
    item["last_error"] = printer.lastError;
    item["ack_hint"] = printer.ackHint;
    item["last_probe_name"] = printer.lastProbeName;
    JsonObject telemetry = item.createNestedObject("telemetry");
    fillPrinterTelemetryJson(telemetry, printer.telemetry, false);

    char text[32];
    formatSinceBootToBuf(printer.lastConnectedMs, text, sizeof(text));
    item["last_connected_text"] = text;
    formatSinceBootToBuf(printer.lastDisconnectedMs, text, sizeof(text));
    item["last_disconnected_text"] = text;
    formatSinceBootToBuf(printer.lastStopSentMs, text, sizeof(text));
    item["last_stop_sent_text"] = text;
    formatSinceBootToBuf(printer.lastStopAttemptMs, text, sizeof(text));
    item["last_stop_attempt_text"] = text;
    formatSinceBootToBuf(printer.lastAckMs, text, sizeof(text));
    item["last_ack_text"] = text;
    formatSinceBootToBuf(printer.lastProbeSentMs, text, sizeof(text));
    item["last_probe_sent_text"] = text;
  }

  JsonArray wsDiscovery = doc.createNestedArray("ws_discovery");
  fillWsDiscoveryJson(wsDiscovery, false);

  JsonArray logs = doc.createNestedArray("logs");
  const size_t start = (eventCount < EVENT_LOG_CAPACITY) ? 0 : eventHead;
  for (size_t i = 0; i < eventCount; ++i) {
    const size_t idx = (start + i) % EVENT_LOG_CAPACITY;
    JsonObject entry = logs.createNestedObject();
    entry["severity"] = eventSeverityText(eventLog[idx].severity);
    entry["text"] = eventLog[idx].text;
    entry["iso_utc"] = eventLog[idx].isoUtc;
    char eventUptime[32];
    formatDurationToBuf(eventLog[idx].ms, eventUptime, sizeof(eventUptime));
    entry["uptime"] = eventUptime;
  }

  jsonSend(doc);
}

static void handleDiagnosticsApi() {
  // Expone particiones, IPs, URLs OTA y estado interno: solo para soporte autenticado
  if (!authorizeCriticalRequest()) return;
  static StaticJsonDocument<JSON_DIAG_CAPACITY> doc;
  doc.clear();

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

  doc["project"] = PROJECT_NAME;
  doc["support_schema"] = 1;
  doc["fw_version"] = FW_VERSION;
  doc["fw_build"] = FW_BUILD;
  doc["boot_count"] = bootCount;
  doc["uptime_ms"] = millis();
  char uptimeText[32];
  formatDurationToBuf(millis(), uptimeText, sizeof(uptimeText));
  doc["uptime_text"] = uptimeText;
  doc["time_synced"] = timeRuntime.synced;
  doc["time_utc"] = timeRuntime.lastIsoUtc;
  doc["last_event"] = lastEvent;
  doc["previous_event"] = previousEvent;
  doc["current_reset_reason"] = currentResetReason;
  doc["previous_reset_reason"] = previousResetReason;
  doc["wifi_sta_ip"] = WiFi.localIP().toString();
  doc["wifi_ap_ip"] = WiFi.softAPIP().toString();
  doc["wifi_status"] = wifiStatusText(WiFi.status());
  doc["wifi_rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["wifi_last_disconnect_reason"] = lastWiFiDisconnectReason;
  doc["auth_critical_required"] = criticalAuthEnabled();
  doc["stop_active"] = stopActive;
  doc["stop_reason"] = stopReason;
  doc["stop_delivered_count"] = stopDeliveredPrinterCount();
  doc["stop_pending_count"] = stopPendingPrinterCount();
  doc["stop_exhausted_count"] = stopExhaustedPrinterCount();
  doc["stop_max_attempts"] = STOP_MAX_ATTEMPTS;
  doc["stop_retry_interval_ms"] = STOP_RETRY_INTERVAL_MS;
  doc["stop_ack_grace_ms"] = STOP_ACK_GRACE_MS;
  doc["ups_usb_ready"] = ups.isReady();
  doc["ups_state"] = upsStateKey();
  doc["ups_code"] = lastUpsStatusCode;
  doc["ups_last_seen_ms"] = lastUpsSeenMs;
  doc["ups_read_stale"] = upsReadingStale(millis());
  doc["ups_stale_read_ms"] = UPS_STALE_READ_MS;
  doc["ota_partition_count"] = esp_ota_get_app_partition_count();
  doc["running_partition"] = running ? running->label : "";
  doc["boot_partition"] = boot ? boot->label : "";
  doc["next_update_partition"] = next ? next->label : "";
  doc["sketch_size"] = ESP.getSketchSize();
  doc["free_sketch_space"] = ESP.getFreeSketchSpace();
  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();
  doc["heap_max_alloc"] = ESP.getMaxAllocHeap();
  doc["psram_size"] = ESP.getPsramSize();
  doc["psram_free"] = ESP.getFreePsram();
  doc["chip_model"] = ESP.getChipModel();
  doc["chip_revision"] = ESP.getChipRevision();
  doc["flash_size"] = ESP.getFlashChipSize();
  doc["mdns_started"] = mdnsStarted;
  doc["metadata_url"] = deviceConfig.otaMetadataUrl;
  doc["printer_host_1"] = deviceConfig.printerHost1;
  doc["printer_host_2"] = deviceConfig.printerHost2;
  doc["ups_stop_delay_ms"] = deviceConfig.upsStopDelayMs;
  doc["ws_port"] = deviceConfig.wsPort;

  JsonObject hardware = doc.createNestedObject("hardware");
  fillHardwareJson(hardware);

  JsonArray printerArray = doc.createNestedArray("printers");
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    const PrinterState& printer = printers[i];
    JsonObject item = printerArray.createNestedObject();
    item["name"] = printer.name;
    item["host"] = printer.host;
    item["port"] = deviceConfig.wsPort;
    item["connected"] = printer.connected;
    item["stop_sent"] = printer.stopSent;
    item["stop_attempts"] = printer.stopAttempts;
    item["stop_max_attempts"] = STOP_MAX_ATTEMPTS;
    item["retry_exhausted"] = printer.ackState == StopAckState::Exhausted;
    item["ack_state"] = stopAckText(printer.ackState);
    item["last_text"] = printer.lastText;
    item["last_error"] = printer.lastError;
    item["ack_hint"] = printer.ackHint;
    item["last_probe_name"] = printer.lastProbeName;
    JsonObject telemetry = item.createNestedObject("telemetry");
    fillPrinterTelemetryJson(telemetry, printer.telemetry, true);
    item["last_connected_ms"] = printer.lastConnectedMs;
    item["last_disconnected_ms"] = printer.lastDisconnectedMs;
    item["last_stop_sent_ms"] = printer.lastStopSentMs;
    item["last_stop_attempt_ms"] = printer.lastStopAttemptMs;
    item["last_ack_ms"] = printer.lastAckMs;
    item["last_probe_sent_ms"] = printer.lastProbeSentMs;
  }

  JsonArray wsDiscovery = doc.createNestedArray("ws_discovery");
  fillWsDiscoveryJson(wsDiscovery, true);

  JsonArray logs = doc.createNestedArray("logs");
  const size_t start = (eventCount < EVENT_LOG_CAPACITY) ? 0 : eventHead;
  for (size_t i = 0; i < eventCount; ++i) {
    const size_t idx = (start + i) % EVENT_LOG_CAPACITY;
    JsonObject entry = logs.createNestedObject();
    entry["ms"] = eventLog[idx].ms;
    entry["severity"] = eventSeverityText(eventLog[idx].severity);
    entry["text"] = eventLog[idx].text;
    entry["iso_utc"] = eventLog[idx].isoUtc;
  }

  JsonObject ota = doc.createNestedObject("ota");
  fillOtaJson(ota);

  jsonSend(doc);
}

static void handleDiagnosticsExportApi() {
  if (!authorizeCriticalRequest()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=\"guardian-diagnostics.json\"");
  handleDiagnosticsApi();
}

static void handleOtaStatusApi() {
  // Expone URL del manifest y estado OTA: protegido para evitar enumeracion de infraestructura
  if (!authorizeCriticalRequest()) return;
  StaticJsonDocument<JSON_OTA_CAPACITY> doc;
  JsonObject ota = doc.to<JsonObject>();
  fillOtaJson(ota);
  jsonSend(doc);
}

// =============================================================================
// CONFIG API
// =============================================================================
static void handleConfigGetApi() {
  if (WEB_AUTH_PROTECT_CONFIG_GET && !authorizeCriticalRequest()) return;

  StaticJsonDocument<512> doc;
  doc["printer1_host"] = deviceConfig.printerHost1;
  doc["printer2_host"] = deviceConfig.printerHost2;
  doc["ws_port"] = deviceConfig.wsPort;
  doc["ups_stop_delay_s"] = deviceConfig.upsStopDelayMs / 1000;
  doc["ota_metadata_url"] = deviceConfig.otaMetadataUrl;
  jsonSend(doc, 200);
}

static void handleConfigPostApi() {
  if (!authorizeCriticalRequest()) return;
  const String body = server.arg("plain");
  if (body.isEmpty()) { sendJsonError(400, "empty_body"); return; }
  StaticJsonDocument<640> req;
  const DeserializationError err = deserializeJson(req, body);
  if (err != DeserializationError::Ok) { sendJsonError(400, "invalid_json"); return; }

  bool changed = false;
  bool printerChanged = false;

  if (req.containsKey("printer1_host")) {
    const char* v = req["printer1_host"];
    if (v && strlen(v) > 0 && strlen(v) < SMALL_TEXT_SIZE) {
      copyText(deviceConfig.printerHost1, sizeof(deviceConfig.printerHost1), v);
      copyText(printers[0].host, sizeof(printers[0].host), v);
      prefs.putString("cfg_p1", v);
      changed = printerChanged = true;
    }
  }
  if (req.containsKey("printer2_host")) {
    const char* v = req["printer2_host"];
    if (v && strlen(v) > 0 && strlen(v) < SMALL_TEXT_SIZE) {
      copyText(deviceConfig.printerHost2, sizeof(deviceConfig.printerHost2), v);
      copyText(printers[1].host, sizeof(printers[1].host), v);
      prefs.putString("cfg_p2", v);
      changed = printerChanged = true;
    }
  }
  if (req.containsKey("ws_port")) {
    const uint16_t port = req["ws_port"].as<uint16_t>();
    if (port > 0) {
      deviceConfig.wsPort = port;
      prefs.putUShort("cfg_wsp", port);
      changed = printerChanged = true;
    }
  }
  if (req.containsKey("ups_stop_delay_s")) {
    const uint32_t ds = req["ups_stop_delay_s"].as<uint32_t>();
    if (ds >= 30 && ds <= 3600) {
      deviceConfig.upsStopDelayMs = ds * 1000;
      prefs.putULong("cfg_stopd", deviceConfig.upsStopDelayMs);
      changed = true;
    }
  }
  if (req.containsKey("ota_metadata_url")) {
    const char* v = req["ota_metadata_url"];
    if (v && strlen(v) < URL_TEXT_SIZE) {
      copyText(deviceConfig.otaMetadataUrl, sizeof(deviceConfig.otaMetadataUrl), v);
      prefs.putString("cfg_meta", v);
      if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
      otaRuntime.updateAvailable = false; // el manifest anterior ya no aplica a la nueva URL
      if (g_otaMutex) xSemaphoreGive(g_otaMutex);
      changed = true;
    }
  }

  if (printerChanged) {
    reconnectAllPrinters(true);
    logEvent(EventSeverity::Info, "[CFG] Config actualizada — reconectando impresoras.");
  } else if (changed) {
    logEvent(EventSeverity::Info, "[CFG] Config actualizada.");
  }
  if (changed) markStateDirty();
  handleConfigGetApi();
}

static void handleWsProbeApi() {
  if (!authorizeCriticalRequest()) return;
  const String body = server.arg("plain");
  if (body.isEmpty()) { sendJsonError(400, "empty_body"); return; }

  StaticJsonDocument<384> req;
  const DeserializationError err = deserializeJson(req, body);
  if (err != DeserializationError::Ok) { sendJsonError(400, "invalid_json"); return; }

  const int printerIndex = req["printer"] | -1;
  const char* probe = req["probe"] | "";
  char errorText[LAST_TEXT_SIZE];
  if (!sendWsProbeToPrinter(static_cast<size_t>(printerIndex), probe, errorText, sizeof(errorText))) {
    sendJsonError(409, errorText);
    return;
  }

  StaticJsonDocument<256> doc;
  doc["ok"] = true;
  doc["action"] = "ws-probe";
  doc["printer"] = printerIndex;
  doc["probe"] = probe;
  jsonSend(doc);
}

static void handleWsDiscoveryClearApi() {
  if (!authorizeCriticalRequest()) return;
  clearWsDiscoveryLog();
  logEvent(EventSeverity::Info, "[WS] Registro de descubrimiento limpiado desde web.");
  sendActionResponse("ws-discovery-clear");
}

static void setupWebServer() {
  // Panel HTML: requiere auth para proteger acceso desde la red STA
  server.on("/", HTTP_GET, []() {
    if (!authorizeCriticalRequest()) return;
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });

  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204, "text/plain", ""); // evitar 404 en logs del browser
  });

  // Rutas publicas (lectura de estado)
  server.on("/api/status", HTTP_GET, handleStatusApi);
  server.on("/api/config", HTTP_GET, handleConfigGetApi);

  // Rutas protegidas (diagnostico y OTA status exponen info sensible)
  server.on("/api/diagnostics", HTTP_GET, handleDiagnosticsApi);   // auth interna
  server.on("/api/diagnostics/export", HTTP_GET, handleDiagnosticsExportApi); // auth interna
  server.on("/api/update/status", HTTP_GET, handleOtaStatusApi);   // auth interna
  server.on("/api/config", HTTP_POST, handleConfigPostApi);        // auth interna
  server.on("/api/ws/probe", HTTP_POST, handleWsProbeApi);         // auth interna
  server.on("/api/ws/discovery/clear", HTTP_POST, handleWsDiscoveryClearApi); // auth interna

  server.on("/api/stop", HTTP_POST, []() {
    if (!authorizeCriticalRequest()) {
      return;
    }
    requestStop("manual desde web", true);
    for (size_t i = 0; i < PRINTER_COUNT; ++i) {
      sendStopToPrinter(i, true);
    }
    sendActionResponse("stop");
  });

  server.on("/api/clear-stop", HTTP_POST, []() {
    if (!authorizeCriticalRequest()) {
      return;
    }
    clearStopState("web");
    sendActionResponse("clear-stop");
  });

  server.on("/api/reconnect", HTTP_POST, []() {
    if (!authorizeCriticalRequest()) {
      return;
    }
    logEvent(EventSeverity::Info, "[WEB] Reconexion manual solicitada.");
    connectStation(true);
    reconnectAllPrinters(true);
    sendActionResponse("reconnect");
  });

  server.on("/api/reboot", HTTP_POST, []() {
    if (!authorizeCriticalRequest()) {
      return;
    }
    scheduleRestart("web");
    sendActionResponse("reboot");
  });

  server.on("/api/update/check", HTTP_POST, []() {
    if (!authorizeCriticalRequest()) {
      return;
    }
    if (otaBusy() || otaTaskHandle != nullptr) {
      sendJsonError(409, "ota_busy");
      return;
    }
    // Cache de manifest: si ya hay un update detectado hace menos de OTA_METADATA_CACHE_MS,
    // el resultado sigue vigente; evitar re-consultar el servidor en cada click.
    // Si el estado es "al dia" siempre se re-consulta, para ver de inmediato un manifest recien publicado.
    bool manifestCacheValid = false;
    if (g_otaMutex) xSemaphoreTake(g_otaMutex, portMAX_DELAY);
    manifestCacheValid = otaRuntime.updateAvailable
        && otaRuntime.lastCheckMs != 0
        && (millis() - otaRuntime.lastCheckMs) < OTA_METADATA_CACHE_MS;
    if (g_otaMutex) xSemaphoreGive(g_otaMutex);
    if (manifestCacheValid) {
      logEvent(EventSeverity::Info, "[OTA] Update %s ya detectado; usando manifest en cache.", otaRuntime.remoteVersion);
      sendActionResponse("update-check");
      return;
    }
    // Stack de 12 KB para la tarea de consulta; core 1 para no interferir con el WiFi task (core 0)
    BaseType_t taskResult = xTaskCreatePinnedToCore(otaCheckTask, "ota_check", 12288, nullptr, 1, &otaTaskHandle, 1);
    if (taskResult != pdPASS) {
      otaTaskHandle = nullptr;
      sendJsonError(500, "ota_task_create_failed");
      return;
    }
    sendActionResponse("update-check");
  });

  server.on("/api/update/start", HTTP_POST, []() {
    if (!authorizeCriticalRequest()) {
      return;
    }
    if (otaBusy() || otaTaskHandle != nullptr) {
      sendJsonError(409, "ota_busy");
      return;
    }
    if (!otaRuntime.updateAvailable) {
      sendJsonError(409, "no_update_available");
      return;
    }
    // Stack de 32 KB: esp_https_ota necesita espacio para TLS (mbedtls) + buffers de descarga
    BaseType_t taskResult = xTaskCreatePinnedToCore(otaInstallTask, "ota_install", 32768, nullptr, 1, &otaTaskHandle, 1);
    if (taskResult != pdPASS) {
      otaTaskHandle = nullptr;
      sendJsonError(500, "ota_task_create_failed");
      return;
    }
    sendActionResponse("update-start");
  });

  // Upload directo de firmware.bin desde el panel: alternativa local al OTA por manifest.
  // No requiere servidor externo; el binario (que embebe secretos) nunca sale de la LAN.
  server.on("/api/update/upload", HTTP_POST,
    []() {
      // Handler final: corre cuando el body ya fue consumido por el handler de upload
      if (!g_uploadAuthorized) {
        authorizeCriticalRequest(); // responde 401 con el desafio Basic Auth
        return;
      }
      if (!g_uploadSucceeded) {
        sendJsonError(500, textEmpty(otaRuntime.lastError) ? "upload_failed" : otaRuntime.lastError);
        return;
      }
      sendActionResponse("update-upload"); // el reinicio ya quedo agendado; la respuesta sale antes
    },
    []() {
      handleFirmwareUploadChunk();
    });

  server.onNotFound([]() {
    sendJsonError(404, "not_found");
  });

  server.begin();
  logEvent(EventSeverity::Success, "[WEB] Panel iniciado. AP: http://192.168.4.1/");
}

// =============================================================================
// 10. NETWORK / SERVICES
// =============================================================================
static void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  // IMPORTANTE: este callback corre en el WiFi task (core 0), NO en el loop() (core 1).
  // NO llamar metodos de WebSocketsClient aqui: no es thread-safe.
  // Usar flags volatiles para diferir esas operaciones al loop() principal.
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      logEvent(EventSeverity::Success, "[WiFi] Asociado a %s.", WIFI_SSID);
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiFailCount = 0; // reset watchdog: la conexion fue exitosa
      logEvent(EventSeverity::Success, "[WiFi] IP obtenida: %s.", WiFi.localIP().toString().c_str());
      printNetworkInfo();
      beginTimeSync();        // configTzTime() es thread-safe segun IDF
      ensureMdnsStarted();    // MDNS.begin() es rapido y seguro desde aqui
      g_pendingWsReconnect = true;  // diferir reconnectAllPrinters() al loop() para evitar race con WebSocketsClient
      markStateDirty();
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      lastWiFiDisconnectReason = info.wifi_sta_disconnected.reason;
      if (mdnsStarted) {
        MDNS.end();           // MDNS.end() es seguro desde el event handler
        mdnsStarted = false;
      }
      // Solo actualizar flags de estado; el disconnect() real del WS se hace en el loop()
      for (size_t i = 0; i < PRINTER_COUNT; ++i) {
        printers[i].connected = false;          // escritura atomica de bool, seguro
        printers[i].lastDisconnectedMs = millis();
      }
      g_pendingWsDisconnect = true; // diferir client->disconnect() al loop() para evitar race
      logEvent(EventSeverity::Warning, "[WiFi] Desconectada. Razon %u.", static_cast<unsigned>(lastWiFiDisconnectReason));
      markStateDirty();
      break;

    default:
      break;
  }
}

static void setupNetwork() {
  WiFi.persistent(false);          // no guardar SSID/pass en flash: evita corrupcion NVS si se cambian
  WiFi.setSleep(false);            // desactivar power-save del WiFi: reduce latencia y desconexiones
  WiFi.setAutoReconnect(true);     // el driver intenta reconectar automaticamente tras perder la STA
  WiFi.mode(WIFI_AP_STA);          // modo dual: STA para la red principal + AP de respaldo siempre activo
  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.setTxPower(WIFI_POWER_17dBm); // 17 dBm: balance entre alcance y consumo (default ~20 dBm puede causar picos)
  WiFi.onEvent(handleWiFiEvent);

  // Canal 6 (menos congestionado que el 1 en la mayoria de entornos),
  // hidden=false, max 4 clientes simultaneos en el AP.
  // softAP() debe llamarse ANTES de softAPConfig() para que la interfaz AP exista.
  if (WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 4)) {
    // Fijar IP del AP explicitamente DESPUES de que la interfaz exista
    WiFi.softAPConfig(
      IPAddress(192, 168, 4, 1),  // IP del gateway del AP
      IPAddress(192, 168, 4, 1),  // gateway = misma IP
      IPAddress(255, 255, 255, 0) // mascara /24
    );
    logEvent(EventSeverity::Success, "[AP] Red respaldo activa: %s (%s).", AP_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    logEvent(EventSeverity::Error, "[AP] No pude iniciar el punto de acceso.");
  }
  connectStation(true); // intento inicial de conexion STA sin esperar el intervalo

  ArduinoOTA.setHostname(MDNS_HOSTNAME);
  ArduinoOTA.setPort(3232);
  if (!textEmpty(ARDUINO_OTA_PASSWORD)) {
    ArduinoOTA.setPassword(ARDUINO_OTA_PASSWORD);
    logEvent(EventSeverity::Info, "[OTA] ArduinoOTA protegido con password.");
  } else {
    logEvent(EventSeverity::Warning, "[OTA] ArduinoOTA sin password; usar solo en laboratorio.");
  }
  ArduinoOTA.onStart([]() {
    esp_task_wdt_delete(nullptr);
    logEvent(EventSeverity::Info, "[OTA] Actualizacion inalambrica iniciada.");
  });
  ArduinoOTA.onEnd([]() {
    logEvent(EventSeverity::Success, "[OTA] Actualizacion inalambrica completa. Reiniciando.");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPct = 0;
    unsigned int pct = progress * 100 / total;
    if (pct != lastPct && pct % 10 == 0) {
      lastPct = pct;
      logEvent(EventSeverity::Info, "[OTA] Progreso: %u%%.", pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    esp_task_wdt_add(nullptr);
    logEvent(EventSeverity::Error, "[OTA] Error inalambrico: %u.", static_cast<unsigned>(error));
  });
  ArduinoOTA.begin();
  logEvent(EventSeverity::Info, "[OTA] ArduinoOTA listo (port 3232, mDNS: %s.local).", MDNS_HOSTNAME);
}

static void serviceWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    connectStation(false); // llama al watchdog interno si wifiFailCount >= WIFI_MAX_FAIL_RESET
    return;
  }
  // Intentar iniciar mDNS si se perdio y se volvio a conectar (ej. tras reboot del AP)
  ensureMdnsStarted();
}

static void serviceWebSockets() {
  // Procesar flags diferidos del handler WiFi (que corre en otro task/core)
  // Estos deben ejecutarse aqui, en el loop(), donde WebSocketsClient es accedido normalmente
  if (g_pendingWsDisconnect) {
    g_pendingWsDisconnect = false;
    for (size_t i = 0; i < PRINTER_COUNT; ++i) {
      printers[i].client->disconnect(); // limpiar TCP pendiente para evitar sockets zombi
    }
  }
  if (g_pendingWsReconnect) {
    g_pendingWsReconnect = false;
    reconnectAllPrinters(true); // reconectar tras obtener IP WiFi
  }

  // Bombear el loop interno de WebSocketsClient: procesa frames, reconexiones y heartbeat
  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    printers[i].client->loop();
  }

  // Si WiFi esta disponible y un cliente WS no esta conectado, intentar reinicializarlo
  if (WiFi.status() == WL_CONNECTED) {
    for (size_t i = 0; i < PRINTER_COUNT; ++i) {
      if (!printers[i].connected) {
        reconnectPrinter(i, false); // respeta el intervalo WS_MANUAL_REINIT_MS
      }
    }
  }

}

static void serviceStopSupervisor() {
  const uint32_t now = millis();

  if (onBattery && outageStartedMs != 0 && !stopActive && (now - outageStartedMs) >= deviceConfig.upsStopDelayMs) {
    char reason[SMALL_TEXT_SIZE];
    snprintf(reason, sizeof(reason), "corte de luz > %lu s",
             static_cast<unsigned long>(deviceConfig.upsStopDelayMs / 1000));
    requestStop(reason, true);
  }

  if (!stopActive) {
    return;
  }

  for (size_t i = 0; i < PRINTER_COUNT; ++i) {
    if (printers[i].connected) {
      sendStopToPrinter(i, false); // respeta STOP_RETRY_INTERVAL_MS, STOP_MAX_ATTEMPTS y ACK probable
    }
    markStopAckExhausted(i, now);
  }

  if (lastStopSupervisorLogMs == 0 || (now - lastStopSupervisorLogMs) >= STOP_SUPERVISOR_LOG_MS) {
    lastStopSupervisorLogMs = now;
    logEvent(EventSeverity::Warning,
             "[STOP] Supervisor: %u/%u transmitidas, %u pendiente(s), %u sin ACK, %u WS caido(s).",
             static_cast<unsigned>(stopDeliveredPrinterCount()),
             static_cast<unsigned>(PRINTER_COUNT),
             static_cast<unsigned>(stopPendingPrinterCount()),
             static_cast<unsigned>(stopExhaustedPrinterCount()),
             static_cast<unsigned>(disconnectedPrinterCount()));
  }
}

static void processUpsCode(uint8_t code) {
  // Interpretacion del byte de estado del UPS Cyber Power (informe HID 0x51 0x53):
  // el valor bajo UPS_CODE_ON_BATTERY (2) = corte de red; los valores altos (>= UPS_CODE_ON_LINE_MIN)
  // = en linea, fluctuando dentro de una banda. Se clasifica por umbral, no por rango fijo, para
  // tolerar la deriva del UPS sin perder la deteccion de corte.
  lastUpsStatusCode = code;
  upsStateKnown = true;
  lastUpsSeenMs = millis();

  if (code >= UPS_CODE_ON_LINE_MIN) {
    if (onBattery) {
      char duration[32];
      formatDurationToBuf(millis() - outageStartedMs, duration, sizeof(duration));
      logEvent(EventSeverity::Success, "[UPS] Luz restaurada tras %s. Codigo %u. %s",
               duration,
               static_cast<unsigned>(code),
               stopActive ? "Latch STOP permanece activo." : "STOP no fue necesario.");
    }
    onBattery = false;
    outageStartedMs = 0;
    lastBatteryProgressLogMs = 0;
    lastUnmappedUpsCode = -1; // estado conocido: olvidar el ultimo desconocido para re-loguear si reaparece
  } else if (code == UPS_CODE_ON_BATTERY) {
    if (!onBattery) {
      onBattery = true;
      outageStartedMs = millis();
      lastBatteryProgressLogMs = 0;
      logEvent(EventSeverity::Warning, "[UPS] Corte detectado. Temporizador iniciado.");
    } else if ((millis() - lastBatteryProgressLogMs) >= BATTERY_PROGRESS_LOG_MS) {
      lastBatteryProgressLogMs = millis();
      char duration[32];
      formatDurationToBuf(millis() - outageStartedMs, duration, sizeof(duration));
      logEvent(EventSeverity::Warning, "[UPS] Seguimos en bateria: %s.", duration);
    }
    lastUnmappedUpsCode = -1;
  } else {
    // Codigo desconocido: no cambia el estado (conservador) y se loguea una sola vez por valor
    // distinto, para descubrir codigos nuevos del UPS sin inundar el registro cada 10 s.
    if (code != lastUnmappedUpsCode) {
      lastUnmappedUpsCode = code;
      logEvent(EventSeverity::Warning, "[UPS] Codigo no mapeado: %u (estado sin cambios).",
               static_cast<unsigned>(code));
    }
  }
  markStateDirty();
}

static void serviceUps() {
  ups.task();

  const bool ready = ups.isReady();
  if (ready != lastUpsUsbReady) {
    lastUpsUsbReady = ready;
    if (!ready) {
      upsStateKnown = false;
    }
    logEvent(ready ? EventSeverity::Success : EventSeverity::Warning,
             ready ? "[UPS] Dispositivo USB detectado." : "[UPS] Dispositivo USB desconectado.");
    markStateDirty();
  }

  if (!ready) {
    if ((millis() - lastUpsWaitLogMs) >= UPS_WAIT_LOG_INTERVAL_MS) {
      lastUpsWaitLogMs = millis();
      Serial.println("[UPS] Esperando UPS por USB...");
    }
    return;
  }

  // Leer el codigo UPS recibido en el callback USB (similar a ISR) de forma segura
  // g_pendingUpsValid es volatile; se lee una vez para evitar TOCTOU
  if (g_pendingUpsValid) {
    const uint8_t code = g_pendingUpsCode; // copiar antes de limpiar la bandera
    g_pendingUpsValid = false;
    processUpsCode(code);
  }

  // Ciclo de consulta: enviar QS cada UPS_QUERY_INTERVAL_MS y leer respuesta HID despues de UPS_LISTEN_DELAY_MS
  if (lastUpsQueryMs == 0 || (millis() - lastUpsQueryMs) >= UPS_QUERY_INTERVAL_MS) {
    ups.sendQueryStatus();          // enviar informe HID 0x51 0x53 al UPS
    waitingUpsResponse = true;
    lastUpsQueryMs = millis();
    upsResponseScheduledAtMs = millis();
  }

  if (waitingUpsResponse && (millis() - upsResponseScheduledAtMs) >= UPS_LISTEN_DELAY_MS) {
    waitingUpsResponse = false;
    ups.listenInterrupt(); // registrar transfer IN en endpoint 0x81 para leer la respuesta
  }
}

static void serviceStatusLog() {
  if ((millis() - lastStatusLogMs) < SERIAL_STATUS_INTERVAL_MS) {
    return;
  }
  lastStatusLogMs = millis();
  Serial.printf("[STATUS] WiFi=%s IP=%s AP=%s UPS=%s STOP=%s WS1=%s WS2=%s OTA=%s\n",
                wifiStatusText(WiFi.status()),
                WiFi.localIP().toString().c_str(),
                WiFi.softAPIP().toString().c_str(),
                upsStateKey(),
                stopActive ? "ACTIVO" : "LIBRE",
                printers[0].connected ? "UP" : "DOWN",
                printers[1].connected ? "UP" : "DOWN",
                otaStateText(otaRuntime.state));
}

// =============================================================================
// 11. SETUP / LOOP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(1200); // dar tiempo al USB-Serial para que el PC lo detecte antes del primer log

  g_logMutex = xSemaphoreCreateMutex();  // crear ANTES del primer logEvent() para que ya este disponible
  g_otaMutex = xSemaphoreCreateMutex();  // protege otaRuntime contra acceso concurrente desde tareas OTA

  copyText(currentResetReason, sizeof(currentResetReason), resetReasonToText(esp_reset_reason())); // leer causa del ultimo reset
  loadPersistentState(); // cargar config y estado desde NVS (previo al primer logEvent)

  logEvent(EventSeverity::Info, "=== BOOT #%lu === FW %s build %s", static_cast<unsigned long>(bootCount), FW_VERSION, FW_BUILD);
  logEvent(EventSeverity::Info, "[BOOT] Reset actual: %s | previo: %s.", currentResetReason, previousResetReason);
  logEvent(EventSeverity::Info, "[BOOT] Flash: %lu MB | PSRAM: %s (%lu MB).",
           static_cast<unsigned long>(ESP.getFlashChipSize() / (1024 * 1024)),
           psramFound() ? "SI" : "NO",
           static_cast<unsigned long>(ESP.getPsramSize() / (1024 * 1024)));

  if (stopActive) {
    logEvent(EventSeverity::Warning, "[BOOT] Restaure latch STOP pendiente desde NVS.");
  }
  if (previousBatteryState) {
    // El ESP32 se reinicio en medio de un corte: rearmar el temporizador de forma conservadora
    // para que el supervisor STOP actue aunque el UPS no entregue una lectura fresca tras el boot.
    // Si la luz ya volvio, la primera lectura del UPS (~12 s) limpia este estado sin efectos.
    onBattery = true;
    outageStartedMs = millis();
    logEvent(EventSeverity::Warning, "[BOOT] NVS indica corte en curso antes del reinicio; temporizador STOP rearmado.");
  }
  if (!textEmpty(otaRuntime.lastSuccessVersion)) {
    logEvent(EventSeverity::Info, "[BOOT] Ultimo OTA exitoso registrado: %s.", otaRuntime.lastSuccessVersion);
  }

  esp_task_wdt_init(10, true);           // WDT de 10 s; panic=true -> reinicia si se dispara
  esp_task_wdt_add(nullptr);             // registrar el task del loop() con el WDT

  setupPrinterCallbacks();
  setupNetwork();
  setupWebServer();

  logEvent(EventSeverity::Info, "[UPS] Iniciando host USB...");
  ups.begin();

  printNetworkInfo();
  markStateDirty();
  persistStateIfNeeded(true);
}

void loop() {
  esp_task_wdt_reset();       // alimentar el WDT en cada iteracion; si loop() se bloquea >10 s -> reboot
  ArduinoOTA.handle();        // atender actualizaciones OTA por ArduinoOTA (PlatformIO OTA)
  serviceWiFi();              // reconectar STA si cayo + watchdog WiFi
  serviceTimeSync();          // mantener NTP sincronizado
  serviceWebSockets();        // loop WS, flags diferidos del handler y reconexiones
  serviceUps();               // consultar UPS HID y procesar codigo de estado
  serviceStopSupervisor();    // decidir/reintentar STOP sin depender de nuevas lecturas UPS
  server.handleClient();      // atender una peticion HTTP pendiente del WebServer
  serviceStatusLog();         // imprimir resumen periodico en Serial
  persistStateIfNeeded();     // escribir en NVS si hay cambios pendientes (debounced)
  maybeRestart();             // ejecutar reinicio si fue solicitado via API
  yield();                    // ceder al scheduler FreeRTOS para que otros tasks progresen
}
