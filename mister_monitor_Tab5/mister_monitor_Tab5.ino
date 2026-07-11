#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <JPEGDEC.h>
#include <WebServer.h>
#include "mister_types.h"
#include "AppConfig.h"
#include <WiFiUdp.h>
#include "ss_credentials.h"

// ===== ANTI-CRASH: Reset diagnostics, memory safety =====
#include "esp_system.h"       // esp_reset_reason()
#include "esp_heap_caps.h"    // heap_caps_malloc(), MALLOC_CAP_INTERNAL

// Brownout register: available on ESP32/S2/S3, NOT on ESP32-P4.
// __has_include lets the code compile on any chip variant without errors.
#if __has_include("soc/rtc_cntl_reg.h")
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
  #define HAS_BROWNOUT_REG 1
#else
  #define HAS_BROWNOUT_REG 0
#endif

// -------------------------------------------------------
// psramMalloc(): safe allocator for large buffers.
//
// On ESP32-P4 (M5Tab), heap_caps_malloc(MALLOC_CAP_SPIRAM)
// returns addresses in the 0x500xxxxx region which causes
// Store Access Faults (MCAUSE=7). The PSRAM cache is not
// mapped for normal CPU load/store without specific sdkconfig
// settings that the M5Tab Arduino board package does not set.
//
// Plain malloc() is NOT a safe workaround: Espressif's own
// ESP-IDF docs for the P4 state that "make RAM allocatable
// using malloc() as well" is the DEFAULT for CONFIG_SPIRAM_USE
// whenever PSRAM is present, so malloc() can silently hand out
// the same 0x500xxxxx addresses for large-enough requests.
//
// We therefore request MALLOC_CAP_INTERNAL explicitly, which
// forces the allocation onto internal SRAM regardless of the
// SPIRAM_USE default. The M5Tab internal heap has ~510 KB free
// at boot, which is more than enough for all JPEG buffers
// (50-300 KB each).
// -------------------------------------------------------
inline uint8_t* psramMalloc(size_t size) {
  uint8_t* ptr = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ptr) {
    Serial.printf("[MEM] malloc FAILED for %u bytes! Free heap: %u\n",
                  size, ESP.getFreeHeap());
  }
  return ptr;
}

// ========== CONFIGURATION ==========
// All user settings are loaded at boot from /config.ini on the ESP32 SD card.
// Edit config.ini — do NOT hardcode credentials here.
// Defaults below apply when the file is missing or a key is absent.

AppConfig appConfig;          // Populated from /config.ini in setup()

// Network — pointers are reassigned from appConfig after SD init
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* misterIP = "";   // set in setup() from appConfig.misterIP (single source of truth: AppConfig.h)

// ScreenScraper runtime strings.
String _ss_dev_user_str;
String _ss_dev_pass_str;
String _ss_user_str;
String _ss_pass_str;
String _boxart_region_str       = "wor";
String _info_lang_str           = "en";   // [gameinfo] info_lang
int    _info_synopsis_max       = 900;    // [gameinfo] info_synopsis_max
String _core_images_path_str    = "/cores";
String _default_core_image_str  = "/cores/menu.jpg";
// Last HTTP status code returned by any ScreenScraper endpoint. Written by
// every ScreenScraper HTTP call and read afterwards to surface the failure
// reason on screen once a download attempt has given up.
int g_lastSSHttpCode = 0;

// Media type search order — updated from appConfig in setup()
// Defaults match AppConfig struct defaults (overridden by config.ini).
String GAME_MEDIA_ORDER_STR              = "box3d,box2d,wheel-carbon,wheel-steel,wheel,fanart,marquee,screenshot";
String ARCADE_MEDIA_ORDER_STR           = "fanart,marquee,wheel-carbon,wheel-steel,wheel,box3d,box2d,screenshot";
String ARCADE_SUBSYSTEM_MEDIA_ORDER_STR = "wheel-steel,wheel-carbon,wheel";
String CORE_MEDIA_ORDER_STR             = "wheel-steel,wheel-carbon,wheel,photo,illustration,box3d,box2d,marquee,fanart,screenshot";

// Macros preserve all existing call sites without any further changes.
#define SCREENSCRAPER_DEV_USER   (_ss_dev_user_str.c_str())
#define SCREENSCRAPER_DEV_PASS   (_ss_dev_pass_str.c_str())
#define SCREENSCRAPER_USER       (_ss_user_str.c_str())
#define SCREENSCRAPER_PASS       (_ss_pass_str.c_str())
#define BOXART_REGION            (_boxart_region_str.c_str())
#define CORE_IMAGES_PATH         (_core_images_path_str.c_str())
#define DEFAULT_CORE_IMAGE       (_default_core_image_str.c_str())

// Internal ScreenScraper constants — not exposed in config.ini
#define SCREENSCRAPER_DEBUG_PASS "uoAfIjh2AMd"
#define SCREENSCRAPER_SOFTWARE   "M5Stack-MiSTer-Monitor"
#define SAFE_JSON_BUFFER_SIZE    8192
#define MAX_RESPONSE_SIZE        50000

// Hardware constants — fixed for M5Tab
#define TFCARD_CS_PIN     42
#define TARGET_WIDTH      1280
#define TARGET_HEIGHT     720
#define IMAGE_AREA_HEIGHT 620
#define ORIGINAL_WIDTH    320
#define ORIGINAL_HEIGHT   240
#define M5TAB_WIDTH       1280
#define M5TAB_HEIGHT      720
#define SCALE_X           ((float)M5TAB_WIDTH  / ORIGINAL_WIDTH)
#define SCALE_Y           ((float)M5TAB_HEIGHT / ORIGINAL_HEIGHT)

// Theme Colors — compile-time constants
#define THEME_BLACK     0x0000
#define THEME_YELLOW    0xFFE0
#define THEME_GREEN     0x07E0
#define THEME_CYAN      0x07FF
#define THEME_ORANGE    0xFD20
#define THEME_RED       0xF800
#define THEME_BLUE      0x001F
#define THEME_GRAY      0x4208
#define THEME_WHITE     0xFFFF

// Runtime-configurable parameters — formerly #defines, now global variables.
int  CORE_IMAGE_TIMEOUT           = 30000;
bool ENABLE_ALPHABETICAL_FOLDERS  = true;
int  SCREENSCRAPER_TIMEOUT        = 30000;
int  SCREENSCRAPER_RETRIES        = 2;
bool USE_HTTPS_SCREENSCRAPER      = false;
bool ENABLE_DEBUG_MODE            = false;
bool DEBUG_FORCE_UPDATE           = false;
int  DEBUG_FORCE_LEVEL            = 0;
bool ENABLE_AUTO_DOWNLOAD         = true;
int  MAX_IMAGE_SIZE               = 500000;
int  DOWNLOAD_TIMEOUT             = 30000;
int  SCROLL_SPEED_MS              = 300;
int  SCROLL_PAUSE_START_MS        = 2000;
int  SCROLL_PAUSE_END_MS          = 3000;
bool FORCE_CORE_REDOWNLOAD        = false;
bool FORCE_GAME_REDOWNLOAD        = false;
// ===================================

// Data variables
String currentCore = "MENU";
String previousCore = "";
String currentGame = "";
String previousGame = "";
bool coreChanged = false;
bool gameChanged = false;
bool showingCoreImage = false;
unsigned long coreImageStartTime = 0;  // Time when image was shown
unsigned long lastCoreCheck = 0;  // To check core changes while showing image
String coreDownloadFailedFor = "";  // Core for which ScreenScraper download failed — prevents screensaver retry loop

float cpuUsage = 0.0;
float memoryUsage = 0.0;
String uptimeFormatted = "00:00:00";
float sdUsedGB = 0.0;
float sdTotalGB = 0.0;
float sdUsagePercent = 0.0;
int usbDeviceCount = 0;
int serialPortCount = 0;
String networkIP = "0.0.0.0";
bool networkConnected = false;
String sessionDuration = "00:00:00";
int requestsCount = 0;

// Interface variables
bool connected = false;
bool wasConnected = false;   // tracks connected-state edges for the reconnect banner
bool sdCardAvailable = false;
int currentPage = 0;
const int totalPages = 6;  // 5 system pages + GAME INFO
unsigned long lastUpdate = 0;
unsigned long lastPageChange = 0;
unsigned long animTimer = 0;
int blinkState = 0;
bool needsRedraw = true;

// Screensaver variables
unsigned long lastButtonPress = 0;
const unsigned long SCREENSAVER_TIMEOUT = 30000;  // 30 seconds of inactivity

// GAME INFO panel timing:
//  - subpage 1/2 (fields) is shown for 30 s, then flips to the synopsis
//  - subpage 2/2 (synopsis) lasts exactly as long as its scroll needs, plus a
//    short tail, and then the panel exits back to the game/core image
//  - a panel with no synopsis falls back to a plain 1-min timeout
int  gameInfoSubPage = 0;                 // 0 = fields (1/2), 1 = synopsis (2/2)
unsigned long gameInfoSubPageChange = 0;  // last subpage change (auto-flip timer)
bool gameInfoForceExit = false;           // synopsis finished -> leave the panel
const unsigned long GAMEINFO_SCREEN_TIMEOUT  = 60000;   // 1 min fallback
const unsigned long GAMEINFO_SUBPAGE_TIMEOUT = 30000;   // 30 s on the fields page

// Synopsis vertical scroll (subpage 2/2)
int  gameInfoSynScroll   = 0;             // index of the top visible line
unsigned long gameInfoSynScrollTime = 0;  // last scroll step / pause start
unsigned long gameInfoSynCycledTime = 0;  // when the text finished scrolling
bool gameInfoSynPaused   = true;          // pausing at top or bottom
bool gameInfoSynCycled   = false;         // a full scroll cycle has completed
const unsigned long GAMEINFO_SYN_STEP_MS  = 1200;  // one line per 1.2 s
const unsigned long GAMEINFO_SYN_PAUSE_MS = 4000;  // hold at top and bottom
// Tail spent frozen on the LAST lines before leaving the panel. Added to the
// 2.5 s bottom hold, that is ~7.5 s of reading time on the final screenful.
const unsigned long GAMEINFO_SYN_EXIT_MS  = 5000;  // tail after the last line
const unsigned long GAMEINFO_SYN_FIT_MS   = 15000; // dwell when it needs no scroll

// Variables for automatic download
bool downloadInProgress = false;

// When false, the download progress HUD (bar + status line) is suppressed.
// The interactive path (user just loaded a game) turns it on; the background
// CRC-recurrent retry leaves it off, so its bar never stamps over whatever
// image the user is currently looking at. showDownloadProgress checks it.
bool downloadHudEnabled = false;

// Scope guard: guarantees downloadInProgress is cleared on EVERY exit path
// (present and future) of a download function. Eliminates the "flag leak"
// class of bug where one failed download permanently blocked all later
// downloads until a power cycle.
struct DownloadFlagGuard {
  DownloadFlagGuard()  { downloadInProgress = true; }
  ~DownloadFlagGuard() { downloadInProgress = false; }
};
String lastSearchedGame = "";      // Cache to avoid repeated searches

// JPEG decoder object
JPEGDEC jpeg;

// --- MiSTer Monitor UDP discovery ---
static const uint16_t MMON_DISCOVERY_PORT = 51234;
static const char*    MMON_DISCOVER_REQ   = "MMON_DISCOVER_V1";
static const char*    MMON_REPLY_PREFIX   = "MMON_SERVER_V1";

// Locate the server by UDP broadcast. On success sets the global misterIP
// (the same one used to build the http://misterIP:8081 URLs) and returns true.
bool discoverMister(uint8_t attempts = 5, uint16_t replyWaitMs = 600) {
  WiFiUDP udp;
  udp.begin(0);                          // ephemeral local port
  IPAddress broadcast(255, 255, 255, 255);

  for (uint8_t i = 0; i < attempts; i++) {
    udp.beginPacket(broadcast, MMON_DISCOVERY_PORT);
    udp.write((const uint8_t*)MMON_DISCOVER_REQ, strlen(MMON_DISCOVER_REQ));
    udp.endPacket();
    Serial.printf("Discovery: broadcast %d/%d\n", i + 1, attempts);

    uint32_t deadline = millis() + replyWaitMs;
    while (millis() < deadline) {
      if (udp.parsePacket() > 0) {
        char buf[64] = {0};
        int len = udp.read(buf, sizeof(buf) - 1);
        if (len > 0 && strncmp(buf, MMON_REPLY_PREFIX, strlen(MMON_REPLY_PREFIX)) == 0) {
          appConfig.misterIP = udp.remoteIP().toString();  // persistent String backing store
          misterIP = appConfig.misterIP.c_str();           // repoint const char* (same idiom as setup)
          Serial.printf("Discovery: server at %s\n", misterIP);
          udp.stop();
          return true;
        }
      }
      delay(10);
    }
  }
  udp.stop();
  Serial.println("Discovery: no server found");
  return false;
}

// ========== SCREENSHOT SERVER ==========
WebServer screenshotServer(8080);

void handleScreenshotRoot() {
  String ip = WiFi.localIP().toString();
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='5'>"; // Auto-refresh every 5s
  html += "<title>MiSTer Monitor - Screenshot</title>";
  html += "<style>";
  html += "body { background:#000; color:#0FF; font-family:monospace; text-align:center; margin:0; padding:20px; }";
  html += "h1 { color:#FF0; letter-spacing:4px; font-size:1.2em; }";
  html += "img { max-width:100%; border:2px solid #0FF; display:block; margin:20px auto; }";
  html += ".btn { display:inline-block; margin:10px; padding:10px 24px; background:#0FF; color:#000; ";
  html += "       font-weight:bold; text-decoration:none; border-radius:4px; font-family:monospace; }";
  html += ".info { color:#888; font-size:0.85em; margin-top:10px; }";
  html += "</style></head><body>";
  html += "<h1>[ MiSTer Monitor - Live Screen ]</h1>";
  html += "<img src='/screenshot.bmp' alt='Screenshot'>";
  html += "<br>";
  html += "<a class='btn' href='/screenshot.bmp'>📥 Download BMP</a>";
  html += "<a class='btn' href='/'>🔄 Refresh</a>";
  html += "<p class='info'>Auto-refresh every 5 seconds &nbsp;|&nbsp; ";
  html += "Resolution: " + String(M5.Display.width()) + "x" + String(M5.Display.height()) + " &nbsp;|&nbsp; ";
  html += "IP: " + ip + ":8080</p>";
  html += "</body></html>";
  screenshotServer.send(200, "text/html", html);
}

void handleScreenshot() {
  int w = M5.Display.width();   // 1280
  int h = M5.Display.height();  // 720

  // Standard 24-bit RGB888 BMP — universally supported by all browsers and viewers
  // Row stride must be padded to 4-byte boundary
  // For 1280px: 1280*3 = 3840 bytes → already multiple of 4, no padding needed
  const uint32_t HEADER_SIZE = 54;           // 14 file header + 40 DIB header
  const uint32_t ROW_STRIDE  = ((uint32_t)w * 3 + 3) & ~3;  // padded to 4 bytes
  const uint32_t IMAGE_SIZE  = ROW_STRIDE * h;
  const uint32_t FILE_SIZE   = HEADER_SIZE + IMAGE_SIZE;

  uint8_t header[54];
  memset(header, 0, sizeof(header));

  // -- BMP File Header (14 bytes) --
  header[0] = 'B'; header[1] = 'M';
  header[2] = FILE_SIZE & 0xFF;
  header[3] = (FILE_SIZE >> 8)  & 0xFF;
  header[4] = (FILE_SIZE >> 16) & 0xFF;
  header[5] = (FILE_SIZE >> 24) & 0xFF;
  header[10] = HEADER_SIZE;  // pixel data offset

  // -- DIB Header BITMAPINFOHEADER (40 bytes, offset 14) --
  header[14] = 40;   // DIB header size
  header[18] = w & 0xFF;       header[19] = (w >> 8) & 0xFF;
  // Negative height = top-down row order (avoids flipping the image)
  int32_t negH = -(int32_t)h;
  header[22] = negH & 0xFF;        header[23] = (negH >> 8)  & 0xFF;
  header[24] = (negH >> 16) & 0xFF; header[25] = (negH >> 24) & 0xFF;
  header[26] = 1;    // color planes
  header[28] = 24;   // bits per pixel (RGB888)
  header[30] = 0;    // BI_RGB — no compression
  header[34] = IMAGE_SIZE & 0xFF;
  header[35] = (IMAGE_SIZE >> 8)  & 0xFF;
  header[36] = (IMAGE_SIZE >> 16) & 0xFF;
  header[37] = (IMAGE_SIZE >> 24) & 0xFF;

  screenshotServer.setContentLength(FILE_SIZE);
  screenshotServer.send(200, "image/bmp", "");
  screenshotServer.sendContent((const char*)header, HEADER_SIZE);

  // Allocate one row of RGB565 source + one row of RGB888 destination
  // Using PSRAM if available to preserve internal heap
  uint16_t* rowSrc = (uint16_t*)psramMalloc(w * 2);
  uint8_t*  rowDst = (uint8_t*) psramMalloc(ROW_STRIDE);

  if (!rowSrc || !rowDst) {
    Serial.println("[SCREENSHOT] ERROR: Not enough memory for row buffers");
    if (rowSrc) free(rowSrc);
    if (rowDst) free(rowDst);
    return;
  }

  memset(rowDst, 0, ROW_STRIDE);  // Zero padding bytes at end of row

  Serial.printf("[SCREENSHOT] Capturing %dx%d RGB888...\n", w, h);
  unsigned long t0 = millis();

  for (int y = 0; y < h; y++) {
    // Read one row of RGB565 pixels from display
    M5.Display.readRect(0, y, w, 1, rowSrc);

    // Convert RGB565 → RGB888 in-place into rowDst
    // BMP stores pixels as B, G, R (reversed byte order)
    for (int x = 0; x < w; x++) {
      // readRect on M5Tab returns big-endian RGB565 — swap bytes before extracting
      uint16_t px_raw = rowSrc[x];
      uint16_t px = (px_raw << 8) | (px_raw >> 8);  // fix endianness

      // Standard RGB565 extraction after byte-swap
      uint8_t r = ((px >> 11) & 0x1F) << 3;   // bits [15:11] → Red
      uint8_t g = ((px >> 5)  & 0x3F) << 2;   // bits [10:5]  → Green
      uint8_t b =  (px        & 0x1F) << 3;   // bits [4:0]   → Blue

      // BMP standard byte order is BGR
      rowDst[x * 3 + 0] = b;
      rowDst[x * 3 + 1] = g;
      rowDst[x * 3 + 2] = r;
    }

    screenshotServer.sendContent((const char*)rowDst, ROW_STRIDE);
  }

  free(rowSrc);
  free(rowDst);
  Serial.printf("[SCREENSHOT] Done in %lums\n", millis() - t0);
}

void setupScreenshotServer() {
  screenshotServer.on("/", handleScreenshotRoot);
  screenshotServer.on("/screenshot.bmp", handleScreenshot);
  screenshotServer.begin();
  Serial.printf("[SCREENSHOT] Server running at http://%s:8080\n", 
                WiFi.localIP().toString().c_str());
  Serial.println("[SCREENSHOT] Open in browser: http://<IP>:8080");
}
// ========================================

// Global offset for callback-based image centering
// JPEGDEC doesn't accept large offsets in decode(), so we apply them in the callback
static int g_jpegOffsetX = 0;
static int g_jpegOffsetY = 0;

// ========== TOUCH BUTTON STRUCTURE ==========
ScrollTextState gameFooterScroll  = {"", 8, 0, 0, 0, false, false, false};
ScrollTextState imageFooterScroll = {"", 25, 0, 0, 0, false, false, false};

// GAME INFO panel title: size 2 = 12 px/char. The "1/2 >>" indicator now
// occupies the right end of that row, leaving 216 px -> 18 visible chars.
ScrollTextState gameInfoTitleScroll = {"", 18, 0, 0, 0, false, false, false};

// GAME INFO grid (subpage 1/2): every value gets its own horizontal scroll, so
// a long genre / developer / publisher is read in full instead of clipped.
// The row's y is decided at draw time by the vertical centring, and recorded
// here so the loop's scroll refresher can repaint that exact row.
const int GI_VAL_X     = 80;   // value column origin
const int GI_VAL_CHARS = 25;   // 225 px at 9 px/char (size 1.5)
ScrollTextState gameInfoRowScroll[6] = {};
int  gameInfoRowY[6]     = {0};
bool gameInfoRowShown[6] = {false};
ScrollTextState mainHUDCoreScroll = {"", 14, 0, 0, 0, false, false, false};

// Function declarations
void initSDCard();
bool findCoreImage(String coreName, String &imagePath);
String getAlphabeticalPath(String coreName);
bool displayCoreImage(String imagePath);
void showCoreImageScreen(String coreName);
void showGameImageScreen(String coreName, String gameName);
void showGameImageScreenCorrected(String coreName, String gameName);
int jpegDrawCallback(JPEGDRAW *pDraw);
void showSDCardError();
void showImageNotFound(String coreName);

bool loadFullScreenFrame(const char* framePath);
bool loadMisterLogo(int x, int y);
void drawCyberpunkFrame();
void drawProgressSquares(int completedCount);
void showBootSequence();
void drawWiFiProgressCircles(int currentAttempt, bool connected, int maxAttempts);
void connectWithAnimation();
void testMiSTerConnectivity(bool discovered);
void showReconnectBanner();
void updateMiSTerData();
void getCurrentCore();
void getCurrentGame();
bool getStateSnapshot();
void getSystemData();
void getStorageData();
void getUSBData();
void getNetworkAndSession();
void updateDisplay();
void displayMainHUD();
void displaySystemMonitor();
void displayStorageArray();
void displayNetworkTerminal();
void displayDeviceScanner();
void drawHeader(String title, String subtitle);
void drawPageIndicators();
void drawHudConnectionDot(bool isConnected, bool active);
void drawFooter();
void drawMiSTerLogo(int x, int y);
void drawPanel(int x, int y, int w, int h, uint16_t color);
void drawMiniPanel(int x, int y, int w, int h, String label, String value, uint16_t color);
void drawProgressBar(int x, int y, int w, int h, float percent);
void drawStorageBar(int x, int y, int w, int h, float percent);
void drawDigitalClock(int x, int y, String time, String label);
void drawStatusIndicator(int x, int y, uint16_t color, bool active);
void drawRadarScan(int centerX, int centerY, int radius, int angle);
void drawPortArray(int x, int y, int usbCount, int serialCount);
void drawDataTransmission();
void drawMisterLogoRightPanel();
String getPageTitle();
String getPageSubtitle();
float extractFloatValue(String json, String key);
int extractIntValue(String json, String key);
String extractStringValue(String json, String key);

// Function declarations for ScreenScraper
bool findGameImageExact(String coreName, String gameName, String &imagePath);
bool displayCoreImageCentered(String imagePath);
void showDownloadingScreen(String coreName, String gameName);
void showDownloadProgress(int progress, String text);
void showDownloadProgressColored(int progress, String text, uint16_t barColor);

void addGameImageFooter(String gameName);
void drawCoreImageFooter();

GameInfo extractGameInfoFromJeuInfos(String& response, String originalFilename);

// Utility functions
bool isValidHash(String hash, String type);
String urlEncode(String str);
void handleScreenScraperError(int httpCode, String response);

// ROM details function
RomDetails getCurrentRomDetails();
RomDetails getCurrentRomDetailsForced();

// Enhanced extraction functions  
bool extractBoolValue(String json, String key);

// ScreenScraper helper functions
String getScreenScraperSystemId(String coreName);
String getExactFileName(String gameName);
String sanitizeCoreFilename(String name);
String getSavePath(String exactFileName, String searchCore);

// Media and image functions
bool downloadImageFromScreenScraper(String imageUrl, String savePath);
bool downloadImageFromMediaJeu(String mediaUrl, String savePath);
bool downloadCoreImageFromScreenScraper(String coreName, bool forceDownload = FORCE_CORE_REDOWNLOAD);
String getCoreSavePath(String searchCore);
void ssNotifyOnce(const String& key, const char* message, const String& detail);
void ssNotifyUnsupportedCore(const String& coreName);
void showCoreImageScreenWithAutoDownload(String coreName);
void showCoreDownloadingScreen(String coreName);
bool downloadCoreImageStreamingSafe(String baseUrl, String savePath);
String extractMediaUrl(String response, String mediaKey);
void showMenuImageWithCoreOverlay(String coreName);
void forceMemoryCleanup();
String buildCorrectMediaJeuUrl(String gameId, String systemId, String mediaType, String specificSystemeId = "");
void initScrollText(ScrollTextState* state, String text, int maxDisplayChars);
String getScrolledText(ScrollTextState* state);

// --- GAME INFO panel (metadata) ---
String getMetaPathFromImagePath(const String &imagePath);
bool saveGameMeta(const String &metaPath, const GameMeta &m);
bool loadGameMeta(const String &metaPath, GameMeta &m);
String jsonUnescapeAndFold(const String &in);
bool fetchGameMetadataJSON(String gameId, String coreName, RomDetails romDetails, GameMeta &out);
int  wrapTextToLines(const String &text, int maxChars, String *out, int maxOut);
void drawWrappedText(int x, int y, int w, int lineH, int maxLines, const String &text);
void displayGameInfo();
void drawGameInfoIcon(bool pressed = false);
bool gameInfoAvailable();
void drawGameInfoSynopsis();
bool gameInfoSynNeedsScroll();
void resetGameInfoSynScroll();
void tickGameInfoSynScroll();

// ========== SCREEN RENDERING OPTIMIZATION ==========
bool backgroundLoaded = false;  // For frame02.jpg (interface screens)
bool bootFrameLoaded = false;   // For frame01.jpg (boot/connection screens)

GameInfo searchWithJeuInfosPreciseJSON(String coreName, RomDetails romDetails);
bool isNameSearchSystem(const String& systemId);
GameInfo searchWithJeuRechercheJSON(String coreName, String cleanName);
bool tryNameSearchFallback(String coreName, String gameName, RomDetails romDetails);
bool downloadGameBoxartStreamingSafeJSON(String coreName, String gameName);

String currentGameForCrc = "";         // Current game that needs a CRC
String currentCoreForCrc = "";         // Core of the current game  
unsigned long lastCrcRecurrentTime = 0; // Last recurrent CRC attempt
bool crcRecurrentActive = false;       // Whether the recurrent search is active
int crcRecurrentAttempts = 0;          // Recurring attempt counter

// ========== GAME METADATA (GAME INFO panel) ==========
GameMeta currentMeta;                  // metadata for the game on screen
String   metaFetchAttemptedFor = "";   // one lazy fetch attempt per game
bool     metaFetchInProgress   = false; // guard: cleared on EVERY return path
                                        // (same discipline as downloadInProgress)

bool lastRomHasCrc           = false;  // true when current game's ROM has a valid CRC
bool lastRomCrcChecked       = false;  // true once something actually asked the server
                                       // for ROM details; until then lastRomHasCrc is
                                       // merely "not known yet", not "no CRC"
bool lastGameFoundNoMedia    = false;  // game IS catalogued in SS, but has zero artwork
// Evidence from the last downloadImageFromMediaJeu run, to tell a definitive
// "no artwork exists" (clean NOMEDIA answers) from transient transport hiccups:
bool g_mediaSawNoMedia       = false;
bool g_mediaSawValidJpeg     = false;
int  g_mediaAttemptCount     = 0;      // attempts in the current media run (drives the HUD)
bool lastGameImageOK         = false;  // true when a game-specific image is displayed
                                        // (either cached or freshly downloaded)
bool lastGameSearchExhausted = false;  // true when ScreenScraper search with valid
                                        // CRC completed without finding an image
                                        // (system in DB, but game not).
bool scanInProgress          = false;  // true while SCAN button operation is running
                                        // (used to lock further SCAN presses and to
                                        //  show "SCANNING" label on the button)
String lastValidCore = "";
bool serverHasError = false; 
String serverErrorType = "";

void showCoreNotFoundScreen(String coreName);
bool isErrorCore(String core);
bool isArcadeCore(String coreName);
bool tryDownloadMediaTypeWorking(String baseUrl, String savePath, const char* mediaType, const char* mediaName);
bool tryMediaTypeWithRegions(String baseUrl, String savePath, const char* mediaBase, const char* mediaLabel, bool includeGeneric = true);
bool tryMediaTypesForToken(String baseUrl, String savePath, String token);
bool applyMediaOrderAndDownload(String baseUrl, String savePath, String orderStr);
static bool showingGameImage = true;  // true = game image, false = system image
static unsigned long lastRotationTime = 0;
String lastArcadeSystemeId = "";  // Store last arcade subsystem ID

void checkMisterDebugState();
void checkServerErrorState();

void processCrcRecurrent();
void startCrcRecurrentForGame(String gameName, String coreName);
void stopCrcRecurrent();
void updateArcadeSubsystemForCurrentGame(String coreName, String gameName);
void updateArcadeSubsystemForCurrentGameEnhanced(String coreName, String gameName, bool forceUpdate);

void playPrevButtonSound();
void playScanButtonSound();
void playNextButtonSound();

String lastProcessedGame = "";           // Last game we processed for subsystem
bool forceSubsystemUpdate = false;       // Flag to force subsystem update
unsigned long gameChangeTime = 0;        // When the game changed

// ========== SCALED DISPLAY WRAPPER CLASS ==========
// This class wraps M5.Display to automatically scale all drawing operations
// from logical coordinates (320x240) to a 2x scaled area (640x480)
// positioned at offset (90, 120) on the M5Tab display (1280x720)

class ScaledDisplay {
private:
  // Scaling and offset constants for 2x scaling
  static constexpr float SCALE_FACTOR = 2.0;
  static constexpr int OFFSET_X = 90;
  static constexpr int OFFSET_Y = 200;  // Normal offset for the monitor screens
  
  // Internal scaling helper functions
  // These convert logical coordinates to physical coordinates
  inline int scaleX(int x) { return (int)(x * SCALE_FACTOR) + OFFSET_X; }
  inline int scaleY(int y) { return (int)(y * SCALE_FACTOR) + OFFSET_Y; }
  inline int scaleW(int w) { return (int)(w * SCALE_FACTOR); }
  inline int scaleH(int h) { return (int)(h * SCALE_FACTOR); }
  
public:
  // ========== DRAWING PRIMITIVES WITH AUTO-SCALING ==========
  
  void fillRect(int x, int y, int w, int h, uint16_t color) {
    M5.Display.fillRect(scaleX(x), scaleY(y), scaleW(w), scaleH(h), color);
  }
  
  void drawRect(int x, int y, int w, int h, uint16_t color) {
    M5.Display.drawRect(scaleX(x), scaleY(y), scaleW(w), scaleH(h), color);
  }
  
  void fillScreen(uint16_t color) {
    M5.Display.fillScreen(color);
  }
  
  void drawFastHLine(int x, int y, int w, uint16_t color) {
    M5.Display.drawFastHLine(scaleX(x), scaleY(y), scaleW(w), color);
  }
  
  void drawFastVLine(int x, int y, int h, uint16_t color) {
    M5.Display.drawFastVLine(scaleX(x), scaleY(y), scaleH(h), color);
  }
  
  void drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    M5.Display.drawLine(scaleX(x0), scaleY(y0), scaleX(x1), scaleY(y1), color);
  }
  
  void fillCircle(int x, int y, int r, uint16_t color) {
    M5.Display.fillCircle(scaleX(x), scaleY(y), (int)(r * SCALE_FACTOR), color);
  }
  
  void drawCircle(int x, int y, int r, uint16_t color) {
    M5.Display.drawCircle(scaleX(x), scaleY(y), (int)(r * SCALE_FACTOR), color);
  }
  
  // ========== TEXT OPERATIONS WITH AUTO-SCALING ==========
  
  void setCursor(int x, int y) {
    M5.Display.setCursor(scaleX(x), scaleY(y));
  }
  
  void setTextColor(uint16_t color) {
    M5.Display.setTextColor(color);
  }
  
  void setTextColor(uint16_t fg, uint16_t bg) {
    M5.Display.setTextColor(fg, bg);
  }
  
  void setTextWrap(bool wrap) {
    M5.Display.setTextWrap(wrap);
  }

  void setTextSize(float size) {
    // M5GFX accepts a fractional scale, so intermediate sizes such as 1.5
    // are available. float rather than an added overload: with both, every
    // integer setTextSize() call in this sketch would become ambiguous.
    // Text size also needs to be scaled to look proportional
    M5.Display.setTextSize(size * SCALE_FACTOR);
  }
  
  void print(const char* text) {
    M5.Display.print(text);
  }
  
  void print(String text) {
    M5.Display.print(text);
  }
  
  void print(char c) {
    M5.Display.print(c);
  }
  
  void print(int num) {
    M5.Display.print(num);
  }
  
  void print(float num, int decimalPlaces = 2) {
    M5.Display.print(num, decimalPlaces);
  }
  
  void println(const char* text) {
    M5.Display.println(text);
  }
  
  void println(String text) {
    M5.Display.println(text);
  }
  
  void println(char c) {
    M5.Display.println(c);
  }
  
  void println(int num) {
    M5.Display.println(num);
  }
  
  void println(float num, int decimalPlaces = 2) {
    M5.Display.println(num, decimalPlaces);
  }
  
  void printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    M5.Display.print(buffer);
  }
  
  // ========== IMAGE OPERATIONS (PASS THROUGH WITHOUT SCALING) ==========
  // JPEG images are already at correct resolution, so we use M5.Display directly
  
  void pushImage(int x, int y, int w, int h, uint16_t* data) {
    // Images are NOT scaled - they're already at target resolution
    M5.Display.pushImage(x, y, w, h, data);
  }
};

// Create global instance of scaled display
// This replaces all M5.Lcd calls in the original code
ScaledDisplay Lcd;

// ========== TOUCH BUTTON INSTANCES ==========
// Buttons positioned in PHYSICAL coordinates to match frame02.jpg graphics
// Located in the right panel of the screen

TouchButton btnPrev = {
  950,             // x: right panel physical position
  135,             // y: top button
  200,             // w: button width
  80,              // h: button height
  "PREV",          
  THEME_GREEN,     
  false            
};

TouchButton btnScan = {
  950,             // x: same column
  300,             // y: middle button
  200,             // w: same width
  80,              // h: same height
  "SCAN",          
  THEME_GREEN,     
  false            
};

TouchButton btnNext = {
  950,             // x: same column
  470,             // y: bottom button
  200,             // w: same width
  80,              // h: same height
  "NEXT",          
  THEME_GREEN,     
  false            
};

// Wrapper to use default settings (FORCE_CORE_REDOWNLOAD)
bool downloadCoreImageFromScreenScraperDefault(String coreName) {
  return downloadCoreImageFromScreenScraper(coreName, FORCE_CORE_REDOWNLOAD);
}

// Wrapper to explicitly force download 
bool forceDownloadCoreImage(String coreName) {
  return downloadCoreImageFromScreenScraper(coreName, true);
}

// Wrapper to download only if it doesn't exist
bool downloadCoreImageIfNotExists(String coreName) {
  return downloadCoreImageFromScreenScraper(coreName, false);
}

void showDownloadingScreen(String coreName, String gameName) {
  // ========== LOAD FRAME01.JPG AS BASE ==========
  M5.Display.fillScreen(THEME_BLACK);
  
  if (!loadFullScreenFrame("/cores/frame01.jpg")) {
    Serial.println("Failed to load frame01.jpg for download screen");
  }

  // Small delay to ensure frame is rendered
  delay(100);
  
  // ========== LOAD AND OVERLAY MISTER LOGO ==========
  // Logo dimensions: 350x140 pixels
  // Position: centered in right panel (785, 150)
  bool logoLoaded = loadMisterLogo(785, 150);
  
  if (!logoLoaded) {
    // Fallback: draw text-based logo
    M5.Display.setTextColor(THEME_WHITE);
    M5.Display.setTextSize(4);
    M5.Display.setCursor(850, 180);
    M5.Display.print("MiSTer");
    M5.Display.setTextColor(THEME_YELLOW);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(870, 230);
    M5.Display.print("FPGA");
    
    Serial.println("Logo not loaded - using text fallback");
  }
  
  // ========== LEFT PANEL CONTENT (PHYSICAL COORDINATES with OFFSET_Y=130) ==========
  // Constants for this specific layout
  const int PANEL_OFFSET_X = 90;
  const int PANEL_OFFSET_Y = 130;  // Offset specific to the download screen
  const int SCALE = 2;  // Escala 2x
  
  // Macro to convert logical coordinates to physical
  #define PHYS_X(x) (PANEL_OFFSET_X + ((x) * SCALE))
  #define PHYS_Y(y) (PANEL_OFFSET_Y + ((y) * SCALE))
  
  // Clear the left panel content area (physical: 90,130 size 640x480)
  M5.Display.fillRect(PANEL_OFFSET_X, PANEL_OFFSET_Y, 640, 480, THEME_BLACK);
  
  // Header with ScreenScraper logo area
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(4);  // Logical size 2 = physical size 4
  M5.Display.setCursor(PHYS_X(40), PHYS_Y(5));
  M5.Display.print("SCREENSCRAPER");
  
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(2);  // Logical size 1 = physical size 2
  M5.Display.setCursor(PHYS_X(65), PHYS_Y(25));
  M5.Display.print("AUTO-DOWNLOAD");
  
  // Decorative line
  M5.Display.drawFastHLine(PHYS_X(20), PHYS_Y(40), 560, THEME_CYAN);  // 280*2=560
  
  // Main download panel (using drawPanel would require modifying it; draw rect directly)
  M5.Display.drawRect(PHYS_X(10), PHYS_Y(48), 600, 180, THEME_BLUE);  // 300*2=600, 90*2=180
  M5.Display.fillRect(PHYS_X(10)+2, PHYS_Y(48)+2, 596, 176, THEME_BLACK);
  
  // Download status title
  M5.Display.setTextColor(THEME_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(58));
  M5.Display.print("DOWNLOADING GAME ARTWORK");
  
  // Separator
  M5.Display.drawFastHLine(PHYS_X(20), PHYS_Y(70), 560, THEME_GRAY);
  
  // Core info
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(78));
  M5.Display.print("CORE:");
  
  M5.Display.setTextColor(THEME_YELLOW);
  M5.Display.setCursor(PHYS_X(65), PHYS_Y(78));
  String displayCore = coreName.length() > 20 ? coreName.substring(0, 20) + "..." : coreName;
  M5.Display.print(displayCore);
  
  // Game info
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(93));
  M5.Display.print("GAME:");
  
  M5.Display.setTextColor(THEME_YELLOW);
  M5.Display.setCursor(PHYS_X(65), PHYS_Y(93));
  String displayGame = gameName.length() > 20 ? gameName.substring(0, 20) + "..." : gameName;
  M5.Display.print(displayGame);
  
  // Status
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(113));
  M5.Display.print("STATUS: Searching database...");
  
  // Status indicator in top-right (physical coordinates)
  drawStatusIndicator(290, 15, THEME_GREEN, true);
  
  #undef PHYS_X
  #undef PHYS_Y
}

// Stoplight gradient: reserved for progress that genuinely converges on
// success (bytes being fetched). GREEN must mean "about to succeed".
void showDownloadProgress(int progress, String text) {
  if (!downloadHudEnabled) return;   // background retry: stay silent
  uint16_t barColor = (progress < 30) ? THEME_YELLOW :
                      (progress < 70) ? THEME_CYAN   : THEME_GREEN;
  showDownloadProgressColored(progress, text, barColor);
}

// Explicit-colour variant. The media-type sweep uses it with a fixed colour:
// its bar measures how much of the SEARCH SPACE has been swept, not proximity
// to success — a green bar at 90% while every type answers NOMEDIA is a lie.
void showDownloadProgressColored(int progress, String text, uint16_t barColor) {
  if (!downloadHudEnabled) return;   // background retry: stay silent
  // ========== UPDATE PROGRESS BAR (PHYSICAL COORDINATES with OFFSET_Y=130) ==========
  const int PANEL_OFFSET_X = 90;
  const int PANEL_OFFSET_Y = 130;
  const int SCALE = 2;
  
  #define PHYS_X(x) (PANEL_OFFSET_X + ((x) * SCALE))
  #define PHYS_Y(y) (PANEL_OFFSET_Y + ((y) * SCALE))
  
  // Clear progress area (below the STATUS which is at logical Y=113)
  M5.Display.fillRect(PHYS_X(10), PHYS_Y(130), 600, 180, THEME_BLACK);
  
  // Progress bar
  M5.Display.drawRect(PHYS_X(20), PHYS_Y(140), 560, 40, THEME_WHITE);
  M5.Display.fillRect(PHYS_X(20)+2, PHYS_Y(140)+2, 556, 36, THEME_BLACK);
  
  // Fill progress bar
  if (progress > 0) {
    int fillWidth = (progress * 552) / 100;  // 276*2 = 552
    M5.Display.fillRect(PHYS_X(20)+4, PHYS_Y(140)+4, fillWidth, 32, barColor);
  }
  
  // Progress percentage in center
  M5.Display.setTextColor(THEME_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(148), PHYS_Y(145));
  M5.Display.printf("%d%%", progress);
  
  // Status text below progress bar
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(170));
  
  // Truncate text if too long
  String displayText = text.length() > 38 ? text.substring(0, 38) + "..." : text;
  M5.Display.print(displayText);
  
  // Footer
  M5.Display.setTextColor(THEME_GRAY);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(200));
  M5.Display.print("Source: ScreenScraper.fr");
  
  #undef PHYS_X
  #undef PHYS_Y
}

// Visible window (in characters) for the game name in the image footer.
#define GAME_FOOTER_VISIBLE_CHARS_FULL    58

void addGameImageFooter(String gameName) {
  // Footer in PHYSICAL coordinates (full screen width, below image area at Y=620)
  
  // Draw separator line at top of footer
  M5.Display.drawFastHLine(0, 620, 1280, THEME_GREEN);
  M5.Display.fillRect(0, 621, 1280, 99, THEME_BLACK);
  
  // Both footer lines are centred in the free span between the "GAME:" label
  // (ends at x=130) and the button (starts at x=1040), whose centre is x=585:
  //   name: 43 chars x 18 px = 774 px from x=198  -> 68 px either side
  //   hint: 39 chars x 18 px = 702 px from x=234  -> 104 px either side
  // Without the button (game not in ScreenScraper) the name reclaims the width.
  const bool showInfoButton = gameInfoAvailable();
  int  visibleChars = showInfoButton ? 43 : GAME_FOOTER_VISIBLE_CHARS_FULL;
  
  M5.Display.setTextWrap(false);
  
  // Game name label (upper line of footer)
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(40, 638);
  M5.Display.print("GAME:");
  
  // Initialize the scroll state for this game name.
  if (imageFooterScroll.fullText != gameName ||
      imageFooterScroll.maxChars != visibleChars) {
    initScrollText(&imageFooterScroll, gameName, visibleChars);
  }
  
  // Print whatever the scroll state currently exposes (full name on first
  // draw, scrolled window thereafter). The refresh block in loop() will
  // animate it.
  String displayGame = getScrolledText(&imageFooterScroll);
  // The window is a fixed maxChars wide (padded, so the painted pixel width is
  // constant and setTextColor(fg, bg) stays flicker-free). A name that fits is
  // centred inside that window; one that scrolls must stay left-aligned, or the
  // text would wobble as the window slides over it.
  if (!imageFooterScroll.needsScroll) {
    int lead = (imageFooterScroll.maxChars - (int)displayGame.length()) / 2;
    for (int i = 0; i < lead; i++) displayGame = " " + displayGame;
  }
  while ((int)displayGame.length() < imageFooterScroll.maxChars) {
    displayGame += ' ';
  }
  M5.Display.setTextColor(THEME_YELLOW, THEME_BLACK);  // bg color → flicker-free
  M5.Display.setTextSize(3);
  M5.Display.setCursor(198, 638);
  M5.Display.print(displayGame);
  
  // Instructions (lower line of footer) — shifted left when button is visible
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(234, 685);
  M5.Display.print("Touch the screen to show MiSTer monitor");

  // GAME INFO button in the right third of the footer — only when the game
  // can actually produce a panel.
  if (showInfoButton) drawGameInfoIcon();
}

// Footer for fullscreen core image screens.
// Layout (footer band: y=621..720, height 99):
//   - If game is active: line 1 (y=638) "GAME: <name>" with scroll, line 2 (y=685) "Touch..."
//   - If no game:        single centered "Touch..." (y=660), as before
// -----------------------------------------------------------------------------
// gameInfoAvailable() — can this game show a GAME INFO panel at all?
//
// The button is hidden whenever the answer is a definite no, so it never leads
// to an empty panel. Four states, most certain first:
//   1. metadata already in RAM for this game            -> yes
//   2. a lazy fetch was already attempted and failed    -> no  (not in the DB)
//   3. a .meta sidecar sits next to the artwork on SD   -> yes (cached earlier)
//   4. never tried: offer it only if the game is identifiable — a CRC is what
//      the jeuInfos query is built from, and lastGameSearchExhausted already
//      means "system is in ScreenScraper, this game is not".
//
// The SD probe is memoised per game: the footer redraws on every screen change
// and this must not turn into a directory scan each time.
// -----------------------------------------------------------------------------
bool gameInfoAvailable() {
  if (currentGame.length() == 0) return false;

  if (currentMeta.loaded && currentMeta.forGame == currentGame) return true;
  if (metaFetchAttemptedFor == currentGame) return false;

  static String metaProbeFor     = "\x01";   // sentinel: never a real game name
  static bool   metaProbeSidecar = false;
  if (metaProbeFor != currentGame) {
    metaProbeFor = currentGame;
    metaProbeSidecar = false;
    String imgPath;
    if (sdCardAvailable && findGameImageExact(currentCore, currentGame, imgPath)) {
      metaProbeSidecar = SD.exists(getMetaPathFromImagePath(imgPath));
    }
  }
  if (metaProbeSidecar) return true;

  if (lastGameSearchExhausted) return false;   // known absent from ScreenScraper

  // Reaching here means the core IS mapped to a ScreenScraper system and the
  // search is not exhausted. The only open question is whether the ROM can be
  // identified by CRC — and lastRomHasCrc only answers it once something has
  // actually fetched the ROM details. It never does when the artwork was
  // already cached: the download returns early and the CRC recurrent stops as
  // soon as an image exists. Offer the panel in that unknown state; its lazy
  // fetch settles the question and hides the button on the next redraw if the
  // ROM turns out to have no CRC.
  if (lastRomCrcChecked) return lastRomHasCrc;
  return true;
}

// -----------------------------------------------------------------------------
// drawGameInfoIcon() — "GAME INFO" button in the right third of the fullscreen
// image footer. Tapping that third opens the GAME INFO panel (see the
// image-mode touch handler).
//
// PHYSICAL coordinates: the Tab5 image footer is drawn directly on M5.Display
// (band y 620..720 of the 1280x720 panel), so the button lives there too.
// Filled in THEME_CYAN like the "GAME:" label beside it; black text for
// contrast. Body 1040..1240 x 630..710; hit target x>=853, y>=620.
// The right margin stays at 40 px, matching the footer text's left margin, so
// the button was narrowed rather than pushed against the bezel. The game name
// window is kept clear of x=1040 to leave real air between the two.
// -----------------------------------------------------------------------------
void drawGameInfoIcon(bool pressed) {
  const int BX = 1040, BY = 630, BW = 200, BH = 80;

  // Pressed state flashes the fill white, mirroring buttonPressFeedback()'s
  // white-label flash on the PRV / SCAN / NEXT column.
  uint16_t bg = pressed ? THEME_WHITE : THEME_CYAN;
  M5.Display.fillRect(BX, BY, BW, BH, bg);

  // "GAME INFO": 9 chars at size 3 = 162 px wide, 24 px tall — centred (19 px pad)
  M5.Display.setTextWrap(false);
  M5.Display.setTextColor(THEME_BLACK, bg);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(BX + (BW - 162) / 2, BY + (BH - 24) / 2);
  M5.Display.print("GAME INFO");
}

void drawCoreImageFooter() {
  M5.Display.drawFastHLine(0, 620, 1280, THEME_GREEN);
  M5.Display.fillRect(0, 621, 1280, 99, THEME_BLACK);
  
  bool hasGame    = (currentGame.length() > 0);
  
  M5.Display.setTextWrap(false);
  
  if (hasGame) {
    // === Line 1: GAME: <name> ===
    const bool showInfoButton = gameInfoAvailable();
    int visibleChars = showInfoButton ? 43 : GAME_FOOTER_VISIBLE_CHARS_FULL;
    
    // Initialize the scroll state.
    if (imageFooterScroll.fullText != currentGame ||
        imageFooterScroll.maxChars != visibleChars) {
      initScrollText(&imageFooterScroll, currentGame, visibleChars);
    }
    String displayGame = getScrolledText(&imageFooterScroll);
    // Centre a name that fits; a scrolling window stays left-aligned
    // (see addGameImageFooter()).
    if (!imageFooterScroll.needsScroll) {
      int lead = (imageFooterScroll.maxChars - (int)displayGame.length()) / 2;
      for (int i = 0; i < lead; i++) displayGame = " " + displayGame;
    }
    while ((int)displayGame.length() < imageFooterScroll.maxChars) {
    displayGame += ' ';
    }
    
    // GAME: label
    M5.Display.setTextColor(THEME_CYAN);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(40, 638);
    M5.Display.print("GAME:");
    
    // Game name
    M5.Display.setTextColor(THEME_YELLOW, THEME_BLACK);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(198, 638);
    M5.Display.print(displayGame);
    
    // === Line 2: hint ===
    M5.Display.setTextColor(THEME_GREEN);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(234, 685);
    M5.Display.print("Touch the screen to show MiSTer monitor");

    // A game is loaded, so the GAME INFO panel may be reachable from here too:
    // the 30 s rotation alternates game image <-> core image, and the touch
    // handler accepts the button hitbox on both. Draw it under exactly the
    // same condition the handler tests, so it is never an invisible button.
    if (showInfoButton) drawGameInfoIcon();

  } else {
    // No game: just the centered hint, like before
    M5.Display.setTextColor(THEME_GREEN);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(250, 660);
    M5.Display.print("Touch the screen to show MiSTer monitor");
  }
  
}

void drawFooter() {
  // Footer area at bottom of screen - now adapted for frame02.jpg design
  // Draw separator line at top of footer
  // M5.Display.drawFastHLine(0, 215 * SCALE_Y, M5TAB_WIDTH, THEME_GREEN);
  
  // Fill footer background
  M5.Display.fillRect(0, 216 * SCALE_Y, M5TAB_WIDTH, 24 * SCALE_Y, THEME_BLACK);
  
  // System uptime on the left
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10 * SCALE_X, 220 * SCALE_Y);
  M5.Display.printf("SYS: %02d:%02d", (millis() / 60000) % 60, (millis() / 1000) % 60);
  
  // Current game or core in center with scrolling support.
  //
  // Coordinate math at setTextSize(2):
  //   - Cursor at logical x=120, y=220 → physical x=480, y=660 (SCALE_X=4, SCALE_Y=3).
  //   - Each char at size 2 is 12 px wide physically (6 logical × 2 font scale).
  //   - The "GAME: "/"CORE: " prefix (6 chars) occupies 72 px → text starts at x=552 physical.
  //   - The IMG:OK indicator starts at logical x=250 → physical x=1000.
  //   - Use a stable visible window of 20 chars: 20 × 12 = 240 px wide, fits
  //     between x=552 and x=792 with margin to spare before x=1000.
  //
  // Use setTextColor(fg, bg) so the background is painted behind each glyph in
  // a single pass — no flicker, no stale pixels.
  
  const int FOOTER_PHY_X         = 120 * SCALE_X;        // 480
  const int FOOTER_PHY_Y         = 220 * SCALE_Y;        // 660
  const int FOOTER_PREFIX_PX     = 6 * 12;               // 72  (6 chars × 12 px each, size 2)
  const int FOOTER_TEXT_PHY_X    = FOOTER_PHY_X + FOOTER_PREFIX_PX;  // 552
  const int FOOTER_VISIBLE_CHARS = 20;
  const int FOOTER_TEXT_PHY_W    = FOOTER_VISIBLE_CHARS * 12;  // 240 px
  
  // Always pad displayed text to FOOTER_VISIBLE_CHARS for stable pixel width
  auto padToWindow = [](String s, int n) {
    while ((int)s.length() < n) s += ' ';
    return s;
  };
  
  M5.Display.setTextWrap(false);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(THEME_CYAN, THEME_BLACK);
  
// Suppressed on the GAME INFO page (5): the game name is already the
  // panel's scrolling title, and repeating it in the footer is noise.
  if (currentPage != 5) {
    if (currentGame.length() > 0) {
      if (gameFooterScroll.fullText != currentGame) {
        initScrollText(&gameFooterScroll, currentGame, FOOTER_VISIBLE_CHARS);
      }
      String displayGame = padToWindow(getScrolledText(&gameFooterScroll), FOOTER_VISIBLE_CHARS);
    
      // Print the prefix and the value as a single string for atomic redraw
      M5.Display.setCursor(FOOTER_PHY_X, FOOTER_PHY_Y);
      M5.Display.printf("GAME: %s", displayGame.c_str());
    } else {
      if (gameFooterScroll.fullText != currentCore) {
        initScrollText(&gameFooterScroll, currentCore, FOOTER_VISIBLE_CHARS);
      }
      String displayCore = padToWindow(getScrolledText(&gameFooterScroll), FOOTER_VISIBLE_CHARS);
    
      M5.Display.setCursor(FOOTER_PHY_X, FOOTER_PHY_Y);
      M5.Display.printf("CORE: %s", displayCore.c_str());
    }
  }
  
  // SD card status indicator on the right
  if (sdCardAvailable) {
    M5.Display.setCursor(250 * SCALE_X, 220 * SCALE_Y);
    M5.Display.print("IMG:OK");
  }
}

// Parses a ROM details JSON response into `details`.
// Handles: response preview, size truncation, memory check, all field extraction, and summary log.
// `prefix` is "" for a first attempt or "Retry " for a retry — used in log messages.
// Returns false if memory was insufficient; in that case `response` is freed and
// `details.error` is set. The caller must call http.end() and return `details`.
static bool _parseRomDetailsJson(String& response, RomDetails& details, const char* prefix) {
  Serial.printf("%sROM details response: %d bytes\n", prefix, response.length());
  Serial.printf("%sResponse preview:\n%s\n", prefix,
                response.substring(0, min(200, (int)response.length())).c_str());

  if (response.length() > 5000) {
    Serial.printf("Large ROM response (%d bytes), truncating\n", response.length());
    response = response.substring(0, 5000);
  }

  if (ESP.getFreeHeap() < 40000) {
    Serial.printf("Low memory after ROM response (%d bytes)\n", ESP.getFreeHeap());
    response = "";
    details.error = prefix[0] ? "Memory insufficient on retry" : "Memory insufficient";
    return false;
  }

  Serial.printf("Extracting ROM details from %sJSON...\n", prefix);

  details.filename       = extractStringValue(response, "filename");
  Serial.printf("  %sFilename: '%s'\n", prefix, details.filename.c_str());

  details.crc32          = extractStringValue(response, "crc32");
  Serial.printf("  %sCRC32: '%s' (length: %d)\n", prefix, details.crc32.c_str(), details.crc32.length());

  details.md5            = extractStringValue(response, "md5");
  Serial.printf("  %sMD5: '%s' (length: %d)\n", prefix, details.md5.c_str(), details.md5.length());

  details.sha1           = extractStringValue(response, "sha1");
  Serial.printf("  %sSHA1: '%s' (length: %d)\n", prefix, details.sha1.c_str(), details.sha1.length());

  details.filesize       = extractIntValue(response, "size");
  Serial.printf("  %sFile size: %ld bytes\n", prefix, details.filesize);

  details.available      = extractBoolValue(response, "available");
  Serial.printf("  %sAvailable: %s\n", prefix, details.available ? "YES" : "NO");

  details.hashCalculated = extractBoolValue(response, "hash_calculated");
  Serial.printf("  %sHash calculated: %s\n", prefix, details.hashCalculated ? "YES" : "NO");

  details.fileTooLarge   = extractBoolValue(response, "file_too_large");

  details.searchName     = extractStringValue(response, "search_name");
  details.nameSearchHint = extractBoolValue(response, "name_search_hint");
  Serial.printf("  %sFile too large: %s\n", prefix, details.fileTooLarge ? "YES" : "NO");

  details.error          = extractStringValue(response, "error");
  if (details.error.length() > 0) {
    Serial.printf("  %sError: '%s'\n", prefix, details.error.c_str());
  }

  details.path           = extractStringValue(response, "path");
  Serial.printf("  %sPath: '%s'\n", prefix, details.path.c_str());

  details.timestamp      = extractIntValue(response, "timestamp");

  response = ""; // Free memory explicitly

  Serial.printf("ROM Details %sSummary:\n", prefix);
  if (details.available) {
    Serial.printf("  ROM available: %s (%ld bytes)\n", details.filename.c_str(), details.filesize);
    if (details.hashCalculated && details.crc32.length() > 0) {
      Serial.printf("  CRC32 available for precise search: %s\n", details.crc32.c_str());
    } else {
      Serial.printf("  No CRC32 - will use name-based search\n");
    }
  } else {
    Serial.printf("  ROM not available or accessible\n");
  }

  return true;
}

RomDetails getCurrentRomDetails() {
  RomDetails details = {"", "", "", "", 0, false, false, false, "", "", 0, "", false};

  Serial.printf("=== GETTING ROM DETAILS ===\n");
  Serial.printf("Free heap before request: %d bytes\n", ESP.getFreeHeap());

  if (ESP.getFreeHeap() < 50000) {
    Serial.printf("Low memory for ROM details (%d bytes)\n", ESP.getFreeHeap());
    details.error = "Low memory";
    return details;
  }

  String url = String("http://") + misterIP + ":8081/status/rom/details";
  Serial.printf("Requesting ROM details from: %s\n", url.c_str());

  // The first request kicks off the server-side hash; large CD images won't be
  // ready within a single HTTP timeout (a 435 MB CHD ~45 s, a 700 MB one ~75 s).
  // Later attempts harvest the cached CRC once the server has computed it, so we
  // poll on timeout/empty until the CRC arrives or we hit the attempt ceiling.
  //
  // Budget ≈ MAX_ATTEMPTS * (TIMEOUT + RETRY_WAIT). 5 * (12 s + 20 s) ≈ 150 s.
  const int           ROM_DETAILS_MAX_ATTEMPTS  = 5;
  const unsigned long ROM_DETAILS_TIMEOUT_MS    = 12000;
  const unsigned long ROM_DETAILS_RETRY_WAIT_MS = 20000;

  for (int attempt = 1; attempt <= ROM_DETAILS_MAX_ATTEMPTS; attempt++) {
    if (ESP.getFreeHeap() < 45000) {
      Serial.printf("Low memory for ROM details (%d bytes), aborting\n", ESP.getFreeHeap());
      details.error += " + Low memory";
      break;
    }

    Serial.printf("ROM details attempt %d/%d...\n", attempt, ROM_DETAILS_MAX_ATTEMPTS);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(ROM_DETAILS_TIMEOUT_MS);
    http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");

    unsigned long requestStart = millis();
    int code = http.GET();
    Serial.printf("HTTP Response: %d (took %lu ms)\n", code, millis() - requestStart);

    if (code == 200) {
      String response = http.getString();
      Serial.printf("ROM details response: %d bytes\n", response.length());
      Serial.printf("Response preview (first 200 chars):\n%s\n",
                    response.substring(0, min(200, (int)response.length())).c_str());

      if (response.length() > 5000) {
        Serial.printf("Large ROM response (%d bytes), truncating\n", response.length());
        response = response.substring(0, 5000);
      }

      if (!_parseRomDetailsJson(response, details, "")) {
        http.end();
        Serial.printf("Free heap after ROM details: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("=== ROM DETAILS COMPLETE (MEMORY FAIL) ===\n\n");
        return details;
      }
      http.end();

      if (details.available && details.crc32.length() > 0) {
        Serial.printf("ROM details ready on attempt %d (CRC %s)\n",
                      attempt, details.crc32.c_str());
        break;
      }

      if (!details.available || details.fileTooLarge) {
        Serial.printf("ROM details: definitive negative (available=%s, tooLarge=%s) - stopping\n",
                      details.available ? "YES" : "NO",
                      details.fileTooLarge ? "YES" : "NO");
        break;
      }

      Serial.printf("ROM details: 200 without CRC yet, will retry\n");
    } else {
      http.end();
      Serial.printf("Attempt %d/%d failed: HTTP %d\n", attempt, ROM_DETAILS_MAX_ATTEMPTS, code);
      details.error = "HTTP " + String(code);
    }

    if (attempt < ROM_DETAILS_MAX_ATTEMPTS) {
      Serial.printf("Waiting %lus before retry (WDT-safe, yielding every 100ms)...\n",
                    ROM_DETAILS_RETRY_WAIT_MS / 1000);
      unsigned long _waitStart = millis();
      while (millis() - _waitStart < ROM_DETAILS_RETRY_WAIT_MS) {
        M5.update();                      // Keep touch state fresh
        screenshotServer.handleClient();  // Keep HTTP server alive
        delay(100);                       // Yield to FreeRTOS / feed WDT
      }
    }
  }

  Serial.printf("Free heap after ROM details: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("=== ROM DETAILS COMPLETE ===\n\n");
  return details;
}

RomDetails getCurrentRomDetailsForced() {
  // Calls /status/rom/details?force=1 — the server bypasses timestamp checks
  // and reads CURRENTPATH/ACTIVEGAME directly to compute CRC.
  RomDetails details = {"", "", "", "", 0, false, false, false, "", "", 0, "", false};

  Serial.printf("=== FORCED ROM DETAILS (bypass timestamp) ===\n");
  Serial.printf("Free heap before request: %d bytes\n", ESP.getFreeHeap());

  if (ESP.getFreeHeap() < 50000) {
    details.error = "Low memory";
    return details;
  }

  String url = String("http://") + misterIP + ":8081/status/rom/details?force=1";
  Serial.printf("Requesting forced ROM details from: %s\n", url.c_str());

  // Same bounded-retry rationale as getCurrentRomDetails(): ?force=1 triggers a
  // fresh server-side hash, so large CD images won't be ready within a single
  // HTTP timeout. Poll until the CRC is cached or we hit the attempt ceiling.
  // Budget ≈ MAX_ATTEMPTS * (TIMEOUT + RETRY_WAIT). 5 * (12 s + 20 s) ≈ 150 s.
  const int           ROM_DETAILS_MAX_ATTEMPTS  = 5;
  const unsigned long ROM_DETAILS_TIMEOUT_MS    = 12000;
  const unsigned long ROM_DETAILS_RETRY_WAIT_MS = 20000;

  for (int attempt = 1; attempt <= ROM_DETAILS_MAX_ATTEMPTS; attempt++) {
    if (ESP.getFreeHeap() < 45000) {
      Serial.printf("Low memory for forced ROM details (%d bytes), aborting\n", ESP.getFreeHeap());
      details.error += " + Low memory";
      break;
    }

    Serial.printf("Forced ROM details attempt %d/%d...\n", attempt, ROM_DETAILS_MAX_ATTEMPTS);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(ROM_DETAILS_TIMEOUT_MS);
    http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");

    unsigned long requestStart = millis();
    int code = http.GET();
    Serial.printf("Forced HTTP Response: %d (took %lu ms)\n", code, millis() - requestStart);

    if (code == 200) {
      String response = http.getString();
      Serial.printf("Forced response (%d bytes): %s\n", response.length(),
                    response.substring(0, min(200, (int)response.length())).c_str());

      if (response.length() > 5000) {
        Serial.printf("Large forced ROM response (%d bytes), truncating\n", response.length());
        response = response.substring(0, 5000);
      }

      details.filename      = extractStringValue(response, "filename");
      details.crc32         = extractStringValue(response, "crc32");
      details.md5           = extractStringValue(response, "md5");
      details.sha1          = extractStringValue(response, "sha1");
      details.filesize      = extractIntValue(response,  "size");
      details.available     = extractBoolValue(response, "available");
      details.hashCalculated= extractBoolValue(response, "hash_calculated");
      details.fileTooLarge  = extractBoolValue(response, "file_too_large");
      details.error         = extractStringValue(response, "error");
      details.path          = extractStringValue(response, "path");
      details.timestamp     = extractIntValue(response,  "timestamp");

      Serial.printf("Forced CRC32: '%s' | available=%s | hashOK=%s\n",
                    details.crc32.c_str(),
                    details.available ? "YES" : "NO",
                    details.hashCalculated ? "YES" : "NO");

      http.end();

      if (details.available && details.crc32.length() > 0) {
        Serial.printf("Forced ROM details ready on attempt %d (CRC %s)\n",
                      attempt, details.crc32.c_str());
        break;
      }

      if (!details.available || details.fileTooLarge) {
        Serial.printf("Forced ROM details: definitive negative (available=%s, tooLarge=%s) - stopping\n",
                      details.available ? "YES" : "NO",
                      details.fileTooLarge ? "YES" : "NO");
        break;
      }

      Serial.printf("Forced ROM details: 200 without CRC yet, will retry\n");
    } else {
      http.end();
      Serial.printf("Forced attempt %d/%d failed: HTTP %d\n", attempt, ROM_DETAILS_MAX_ATTEMPTS, code);
      details.error = "Forced HTTP " + String(code);
    }

    if (attempt < ROM_DETAILS_MAX_ATTEMPTS) {
      Serial.printf("Waiting %lus before retry (WDT-safe, yielding every 100ms)...\n",
                    ROM_DETAILS_RETRY_WAIT_MS / 1000);
      unsigned long _waitStart = millis();
      while (millis() - _waitStart < ROM_DETAILS_RETRY_WAIT_MS) {
        M5.update();                      // Keep touch state fresh
        screenshotServer.handleClient();  // Keep HTTP server alive
        delay(100);                       // Yield to FreeRTOS / feed WDT
      }
    }
  }

  Serial.printf("Free heap after forced ROM details: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("=== FORCED ROM DETAILS COMPLETE ===\n\n");
  return details;
}


String extractMediaUrl(String response, String mediaKey) {
  String searchKey = "\"" + mediaKey + "\":\"";
  int startPos = response.indexOf(searchKey);
  
  if (startPos == -1) {
    return ""; // Not found
  }
  
  startPos += searchKey.length();
  int endPos = response.indexOf("\"", startPos);
  
  if (endPos == -1) {
    return ""; // Invalid format
  }
  
  String url = response.substring(startPos, endPos);
  
  // Clean up escape sequences if any
  url.replace("\\", "");
  
  return url;
}


String urlEncode(String str) {
  String encoded = "";
  
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    
    if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      // Percent-encode as an unsigned byte. Casting avoids sign extension on
      // bytes above 0x7F, which would otherwise emit a broken escape sequence.
      char hexBuf[4];
      snprintf(hexBuf, sizeof(hexBuf), "%%%02X", (uint8_t)c);
      encoded += hexBuf;
    }
  }
  
  return encoded;
}

void handleScreenScraperError(int httpCode, String response) {
  Serial.printf("ScreenScraper Error Analysis (HTTP %d):\n", httpCode);
  
  switch (httpCode) {
    case 400:
      Serial.println("   Bad Request - Check URL parameters");
      if (response.indexOf("champs obligatoires") != -1) {
        Serial.println("   Missing required fields in URL");
      }
      break;
    case 401:
      Serial.println("   API closed for non-members (server overloaded)");
      Serial.println("   Try again later when server load decreases");
      break;
    case 403:
      Serial.println("   Login error - Check developer credentials");
      Serial.println("   Verify devid and devpassword are correct");
      break;
    case 404:
      Serial.println("   Game/ROM not found in database");
      Serial.println("   CRC might not be catalogued or system incorrect");
      break;
    case 423:
      Serial.println("   API completely closed (server problems)");
      Serial.println("   Wait for ScreenScraper maintenance to complete");
      break;
    case 426:
      Serial.println("   Software blacklisted (version obsolete)");
      Serial.println("   Update software or change softname parameter");
      break;
    case 429:
      if (response.indexOf("threads") != -1) {
        Serial.println("   Thread limit reached - reduce request speed");
      } else if (response.indexOf("minute") != -1) {
        Serial.println("   Rate limit per minute reached");
      } else {
        Serial.println("   Rate limit reached");
      }
      Serial.println("   Wait before making more requests");
      break;
    case 430:
      Serial.println("   Daily quota exceeded - try tomorrow");
      Serial.println("   ScreenScraper limits requests per day per user");
      break;
    case 431:
      Serial.println("   Too many unrecognized ROMs today");
      Serial.println("   Try with ROMs that are more likely to be catalogued");
      break;
    case -1:
      Serial.println("   Connection failed - server unreachable");
      Serial.println("   Check internet connection and DNS resolution");
      break;
    case -11:
      Serial.println("   Timeout - ScreenScraper is overloaded");
      Serial.println("   This is normal during peak hours");
      break;
    case -3:
    // Use a unique name that includes the subsystem ID
      break;
    default:
      Serial.printf("   Unknown error: %d\n", httpCode);
  }
  
  // Show error response preview if one exists
  if (response.length() > 0 && response.length() < 500) {
    Serial.println("Error response preview:");
    Serial.println(response.substring(0, min(200, (int)response.length())));
  }
}

// ---------------------------------------------------------------------------
// Credential-safe logging helpers.
//
// ScreenScraper URLs carry credentials as plain-text query parameters, so any
// log line that prints a full URL must be routed through
// redactScreenScraperUrl() first. The shared developer password is ALWAYS
// masked, even in debug mode, because it ships embedded in every public
// binary. The user's own password is masked by default and only revealed when
// the user enables debug=true in config.ini.
// ---------------------------------------------------------------------------
void maskUrlParameter(String &url, const char *param) {
  int keyPos = url.indexOf(param);
  if (keyPos < 0) return;
  int valueStart = keyPos + strlen(param);
  int valueEnd = url.indexOf('&', valueStart);
  if (valueEnd < 0) valueEnd = url.length();
  url = url.substring(0, valueStart) + "***" + url.substring(valueEnd);
}

String redactScreenScraperUrl(const String &url) {
  String out = url;
  maskUrlParameter(out, "devpassword=");
  if (!ENABLE_DEBUG_MODE) {
    maskUrlParameter(out, "sspassword=");
  }
  return out;
}

// Logs structural properties of a credential (length, boundary characters,
// whitespace, URL-breaking characters) without revealing its value. This is
// enough to diagnose the common config.ini mistakes: an empty value, trailing
// spaces, an inline comment swallowed into the value, or characters that
// break the query string.
void logCredentialShape(const char *label, const String &value) {
  if (value.length() == 0) {
    Serial.printf("[SS] %s: EMPTY\n", label);
    return;
  }
  bool hasWhitespace = false;
  bool hasUrlUnsafe = false;
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') hasWhitespace = true;
    if (c == '&' || c == '+' || c == '#' || c == '%' || c == '=' ||
        c == '?' || c == '/' || c == ';') hasUrlUnsafe = true;
  }
  Serial.printf("[SS] %s: len=%u first='%c' last='%c' whitespace=%s url-unsafe=%s\n",
                label, value.length(),
                value.charAt(0), value.charAt(value.length() - 1),
                hasWhitespace ? "YES" : "NO", hasUrlUnsafe ? "YES" : "NO");
}

// Maps a ScreenScraper HTTP status code to a short on-screen message.
// Semantics match handleScreenScraperError(). The numeric code is included
// so a user can report it verbatim in a support request.
String ssHudMessage(int httpCode) {
  const char *label;
  switch (httpCode) {
    case 400: label = "SS BAD REQUEST";     break;
    case 401: label = "SS SERVER BUSY";     break;
    case 403: label = "SS LOGIN FAILED";    break;
    case 404: label = "NOT IN SS DATABASE"; break;
    case 423: label = "SS API CLOSED";      break;
    case 426: label = "SS UPDATE REQUIRED"; break;
    case 429: label = "SS RATE LIMIT";      break;
    case 430: label = "SS DAILY QUOTA";     break;
    case 431: label = "SS ROM QUOTA";       break;
    case -1:  label = "SS UNREACHABLE";     break;
    case -11: label = "SS TIMEOUT";         break;
    default:  label = (httpCode <= 0) ? "SS CONNECTION ERROR" : "SS ERROR"; break;
  }
  return String(label) + " (" + String(httpCode) + ")";
}

// One-shot full-screen notice. Two constraints shape it:
//  - Screens redraw on every rotation tick, so a blocking notice must fire
//    only once per key — otherwise an unsupported core would nag forever.
//  - It must paint its OWN background: it can fire from states where no
//    downloading screen was ever drawn (an MGL launch delivers core+game in
//    one step), and a bare HUD strip would stamp over the previous screen.
// Layout mirrors this board's showDownloadingScreen: frame01 background,
// logo on the right panel, content on the left panel (PHYS_X/PHYS_Y idiom).
void ssNotifyOnce(const String& key, const char* message, const String& detail) {
  static String lastNotifiedKey = "\x01";   // sentinel: never a real key
  if (lastNotifiedKey == key) return;
  lastNotifiedKey = key;

  M5.Display.fillScreen(THEME_BLACK);
  if (!loadFullScreenFrame("/cores/frame01.jpg")) {
    Serial.println("Failed to load frame01.jpg for artwork-status screen");
  }
  delay(100);
  if (!loadMisterLogo(785, 150)) {
    M5.Display.setTextColor(THEME_WHITE);
    M5.Display.setTextSize(4);
    M5.Display.setCursor(850, 180);
    M5.Display.print("MiSTer");
  }

  const int PANEL_OFFSET_X = 90;
  const int PANEL_OFFSET_Y = 130;
  const int SCALE = 2;
  #define PHYS_X(x) (PANEL_OFFSET_X + ((x) * SCALE))
  #define PHYS_Y(y) (PANEL_OFFSET_Y + ((y) * SCALE))

  M5.Display.fillRect(PANEL_OFFSET_X, PANEL_OFFSET_Y, 640, 480, THEME_BLACK);

  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(4);
  M5.Display.setCursor(PHYS_X(40), PHYS_Y(5));
  M5.Display.print("SCREENSCRAPER");

  M5.Display.setTextColor(THEME_YELLOW);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(65), PHYS_Y(25));
  M5.Display.print("ARTWORK STATUS");

  M5.Display.drawFastHLine(PHYS_X(0), PHYS_Y(38), 640, THEME_YELLOW);

  M5.Display.setTextColor(THEME_WHITE);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(PHYS_X(10), PHYS_Y(55));
  M5.Display.print(message);

  if (detail.length() > 0) {
    String d = detail.length() > 38 ? detail.substring(0, 38) + "..." : detail;
    M5.Display.setTextColor(THEME_YELLOW);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(PHYS_X(10), PHYS_Y(80));
    M5.Display.print(d);
  }

  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(10), PHYS_Y(105));
  M5.Display.print("Showing default image instead...");

  #undef PHYS_X
  #undef PHYS_Y

  delay(3000);
}

// The core itself has no ScreenScraper system id: no artwork is possible for
// it OR for any of its games. Distinct from "game not in the database".
void ssNotifyUnsupportedCore(const String& coreName) {
  ssNotifyOnce("core:" + coreName, "CORE NOT IN SS DATABASE", coreName);
}

String getCoreSavePath(String searchCore) {
  String savePath;
  
  Serial.printf("Building CORE save path for: '%s'\n", searchCore.c_str());
  Serial.printf("lastArcadeSystemeId: '%s' (length: %d)\n", lastArcadeSystemeId.c_str(), lastArcadeSystemeId.length());
  
  String finalCoreName = searchCore;
  String searchCoreLower = searchCore;
  searchCoreLower.toLowerCase();
  if (searchCoreLower == "arcade" && lastArcadeSystemeId.length() > 0) {
    // Use a unique name that includes the subsystem ID
    finalCoreName = "Arcade_" + lastArcadeSystemeId;
    Serial.printf("Using subsystem-specific core name: %s\n", finalCoreName.c_str());
  }
  
  // Sanitize before any path concatenation — friendly names may contain '/'
  String safeFinalName = sanitizeCoreFilename(finalCoreName);

  if (ENABLE_ALPHABETICAL_FOLDERS) {
    String alphabetPath = getAlphabeticalPath(safeFinalName);
    
    Serial.printf("Alphabetical core path: %s\n", alphabetPath.c_str());
    
    // Create alphabetical directory if it does not exist
    if (!SD.exists(alphabetPath)) {
      if (SD.mkdir(alphabetPath)) {
        Serial.printf("Created alphabet dir for core: %s\n", alphabetPath.c_str());
      } else {
        Serial.printf("Failed to create alphabet dir: %s\n", alphabetPath.c_str());
      }
    }
    
    // The core image goes directly in the alphabetical folder
    savePath = alphabetPath + "/" + safeFinalName + ".jpg";
  } else {
    savePath = String(CORE_IMAGES_PATH) + "/" + safeFinalName + ".jpg";
  }
  
  Serial.printf("Final CORE save path: %s\n", savePath.c_str());
  return savePath;
}

bool downloadCoreImageFromScreenScraper(String coreName, bool forceDownload) {
  if (downloadInProgress) {
    Serial.println("Download already in progress");
    return false;
  }
  
  DownloadFlagGuard dlGuard;
  g_lastSSHttpCode = 0;
  bool success = false;
  
  Serial.printf("\n=== ENHANCED CORE IMAGE DOWNLOAD ===\n");
  Serial.printf("Core: '%s'\n", coreName.c_str());
  Serial.printf("Force Download: %s\n", forceDownload ? "YES" : "NO");
  Serial.printf("lastArcadeSystemeId: '%s' (length: %d)\n", lastArcadeSystemeId.c_str(), lastArcadeSystemeId.length());
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  
  // Skip download for menu/main
  String coreNameLower = coreName;
  coreNameLower.toLowerCase();
  if (coreNameLower == "menu" || coreNameLower == "main") {
    Serial.println("Skipping download for MENU core");
    return false;
  }
  
  String systemId = getScreenScraperSystemId(coreName);
  
  // Arcade subsystem management
  if (systemId == "75" && lastArcadeSystemeId.length() > 0) {
    String oldSystemId = systemId;
    systemId = lastArcadeSystemeId;
    Serial.printf("Using arcade subsystem ID %s instead of generic Arcade ID %s\n", 
                 systemId.c_str(), oldSystemId.c_str());
    
    // Check if the specific image for the subsystem already exists
    String specificImagePath = getCoreSavePath(coreName); // Now it returns a route with a subsystem.
    if (!SD.exists(specificImagePath)) {
      forceDownload = true;
      Serial.printf("Subsystem-specific image not found at %s, forcing download\n", 
                   specificImagePath.c_str());
    } else {
      Serial.printf("Subsystem-specific image already exists: %s\n", 
                   specificImagePath.c_str());
      
      // If a forced download is not performed and the specific image already exists, use the existing one.
      if (!forceDownload) {
        return true;
      }
    }
  }
  
  // Force download for arcade subsystems to ensure we get the specific image
  if (systemId != "75" && lastArcadeSystemeId.length() > 0) {
    Serial.printf("Forcing download for arcade subsystem %s\n", systemId.c_str());
    forceDownload = true;
  }
  
  if (systemId.length() == 0) {
    Serial.printf("System '%s' not supported by ScreenScraper\n", coreName.c_str());
    ssNotifyUnsupportedCore(coreName);
    return false;
  }
  
  Serial.printf("Mapped to ScreenScraper system ID: %s\n", systemId.c_str());
  
  // Build CLEAN mediaSysteme.php URL (NO duplicate resize parameters)
  String baseUrl = "https://api.screenscraper.fr/api2/mediaSysteme.php";
  baseUrl += "?devid=" + String(SCREENSCRAPER_DEV_USER);
  baseUrl += "&devpassword=" + String(SCREENSCRAPER_DEV_PASS);
  baseUrl += "&softname=" + String(SCREENSCRAPER_SOFTWARE);
  baseUrl += "&ssid=" + urlEncode(String(SCREENSCRAPER_USER));
  baseUrl += "&sspassword=" + urlEncode(String(SCREENSCRAPER_PASS));
  baseUrl += "&systemeid=" + systemId;
  
  // Empty hash parameters (required by API)
  baseUrl += "&crc=";
  baseUrl += "&md5=";
  baseUrl += "&sha1=";
  
  Serial.printf("Base URL: %s\n", redactScreenScraperUrl(baseUrl).c_str());
  
  // Determine save path for core image
  String searchCore = coreName;
  searchCore.toLowerCase();
  String savePath = getCoreSavePath(searchCore);
  
  Serial.printf("Save path: %s\n", savePath.c_str());
  
  // CHECK IF AN IMAGE ALREADY EXISTS AND IF A DOWNLOAD SHOULD BE FORCED
  bool imageExists = SD.exists(savePath);
  if (imageExists) {
    Serial.printf("Existing image found: %s\n", savePath.c_str());
    if (!forceDownload) {
      Serial.println("Image exists and force download disabled - skipping download");
      return true; // Consider it a success if it already exists
    } else {
      Serial.println("FORCE DOWNLOAD enabled - will redownload with new priority");
      // Optionally, rename the existing file as a backup
      String backupPath = savePath + ".backup";
      if (SD.exists(backupPath)) {
        SD.remove(backupPath);
      }
      SD.rename(savePath, backupPath);
      Serial.printf("Existing image backed up to: %s\n", backupPath.c_str());
    }
  }
  
  // USE SECURE STREAMING FUNCTION for cores
  success = downloadCoreImageStreamingSafe(baseUrl, savePath);
  
  if (success) {
    Serial.printf("CORE IMAGE DOWNLOAD SUCCESS!\n");
    Serial.printf("   Core: %s\n", coreName.c_str());
    Serial.printf("   System ID: %s\n", systemId.c_str());
    Serial.printf("   File: %s\n", savePath.c_str());
    Serial.printf("   Force Download: %s\n", forceDownload ? "Enabled" : "Disabled");
    
    // ADDITIONAL DEBUG FOR ARCADE SUBSYSTEMS
    if (systemId != "75" && lastArcadeSystemeId.length() > 0) {
      Serial.printf("ARCADE SUBSYSTEM SUCCESS!\n");
      Serial.printf("   Subsystem ID: %s\n", systemId.c_str());
      Serial.printf("   Specific image saved as: %s\n", savePath.c_str());
    }
  } else {
    Serial.println("Core image download failed for all media types");
    Serial.printf("Last ScreenScraper HTTP status: %d (%s)\n",
                  g_lastSSHttpCode, ssHudMessage(g_lastSSHttpCode).c_str());
    Serial.printf("   Core: %s\n", coreName.c_str());
    Serial.printf("   System ID: %s\n", systemId.c_str());
    Serial.println("   Possible reasons:");
    Serial.println("   - System not in ScreenScraper database");
    Serial.println("   - No media available for this system");
    Serial.println("   - Network connectivity issues");
    Serial.println("   - ScreenScraper server overloaded");
    
    // ADDITIONAL DEBUG FOR ARCADE SUBSYSTEM PROBLEMS
    if (systemId != "75" && lastArcadeSystemeId.length() > 0) {
      Serial.printf("ARCADE SUBSYSTEM FAILED!\n");
      Serial.printf("   Attempted subsystem ID: %s\n", systemId.c_str());
      Serial.printf("   Fallback: Try generic Arcade image if available\n");
    }
  }
  
  Serial.printf("Free heap after download: %d bytes\n", ESP.getFreeHeap());
  Serial.println("=== CORE IMAGE DOWNLOAD COMPLETE ===\n");
  
  return success;
}

// Core image download screen
void showCoreDownloadingScreen(String coreName) {
  // ========== LOAD FRAME01.JPG AS BASE ==========
  M5.Display.fillScreen(THEME_BLACK);
  
  if (!loadFullScreenFrame("/cores/frame01.jpg")) {
    Serial.println("Failed to load frame01.jpg for core download screen");
  }
  
  // Small delay to ensure frame is rendered
  delay(100);
  
  // ========== LOAD AND OVERLAY MISTER LOGO ==========
  // Logo dimensions: 350x140 pixels
  // Position: centered in right panel (785, 150)
  bool logoLoaded = loadMisterLogo(785, 150);
  
  if (!logoLoaded) {
    // Fallback: draw text-based logo
    M5.Display.setTextColor(THEME_WHITE);
    M5.Display.setTextSize(4);
    M5.Display.setCursor(850, 180);
    M5.Display.print("MiSTer");
    M5.Display.setTextColor(THEME_YELLOW);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(870, 230);
    M5.Display.print("FPGA");
    
    Serial.println("Logo not loaded - using text fallback");
  }

  // ========== LEFT PANEL CONTENT (PHYSICAL COORDINATES with OFFSET_Y=130) ==========
  const int PANEL_OFFSET_X = 90;
  const int PANEL_OFFSET_Y = 130;
  const int SCALE = 2;
  
  #define PHYS_X(x) (PANEL_OFFSET_X + ((x) * SCALE))
  #define PHYS_Y(y) (PANEL_OFFSET_Y + ((y) * SCALE))
  
  // Clear the left panel content area
  M5.Display.fillRect(PANEL_OFFSET_X, PANEL_OFFSET_Y, 640, 480, THEME_BLACK);
  
  // Header with ScreenScraper logo area
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(4);  // Logical size 2 = physical size 4
  M5.Display.setCursor(PHYS_X(40), PHYS_Y(5));
  M5.Display.print("SCREENSCRAPER");
  
  M5.Display.setTextColor(THEME_ORANGE);
  M5.Display.setTextSize(2);  // Logical size 1 = physical size 2
  M5.Display.setCursor(PHYS_X(60), PHYS_Y(25));
  M5.Display.print("SYSTEM DOWNLOAD");
  
  // Decorative line
  M5.Display.drawFastHLine(PHYS_X(20), PHYS_Y(40), 560, THEME_CYAN);
  
  // Main download panel (orange for system vs blue for game)
  M5.Display.drawRect(PHYS_X(10), PHYS_Y(48), 600, 180, THEME_ORANGE);
  M5.Display.fillRect(PHYS_X(10)+2, PHYS_Y(48)+2, 596, 176, THEME_BLACK);
  
  // Download status title
  M5.Display.setTextColor(THEME_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(58));
  M5.Display.print("DOWNLOADING SYSTEM ARTWORK");
  
  // Separator
  M5.Display.drawFastHLine(PHYS_X(20), PHYS_Y(70), 560, THEME_GRAY);
  
  // System info
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(78));
  M5.Display.print("SYSTEM:");
  
  M5.Display.setTextColor(THEME_YELLOW);
  M5.Display.setCursor(PHYS_X(75), PHYS_Y(78));
  String displayCore = coreName.length() > 18 ? coreName.substring(0, 18) + "..." : coreName;
  M5.Display.print(displayCore);
  
  // Media types
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(93));
  M5.Display.print("MEDIA TYPES:");
  
  M5.Display.setTextColor(THEME_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(106));
  M5.Display.print("screen marquee > photo > wheel");
    Serial.println("SD not available, showing SD error");
  // Status
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(PHYS_X(20), PHYS_Y(123));
  M5.Display.print("STATUS: Searching database...");
  
  // Status indicator in top-right (physical coordinates, orange for system)
  drawStatusIndicator(290, 15, THEME_ORANGE, true);
  
  #undef PHYS_X
  #undef PHYS_Y
}

void showCoreImageScreenWithAutoDownload(String coreName) {
  String imagePath = "";
  
  Serial.printf(" === ENHANCED CORE IMAGE DISPLAY ===\n");
  Serial.printf("Core: '%s'\n", coreName.c_str());
  Serial.printf("lastArcadeSystemeId: '%s' (length: %d)\n", 
                lastArcadeSystemeId.c_str(), lastArcadeSystemeId.length());
  
  if (!sdCardAvailable) {
    Serial.println("SD not available, showing SD error");
    showSDCardError();
    return;
  }
  
  if (coreName.length() == 0 || coreName == "NO SERVER" || coreName == "TIMEOUT" || coreName.startsWith("ERROR") || coreName.equalsIgnoreCase("MENU")) {
  Serial.printf("Special state detected: '%s' - showing menu with overlay\n", coreName.c_str());
  showMenuImageWithCoreOverlay(coreName);
  return;
}
  
  // For Arcade cores, ALWAYS ensure we have subsystem info
  String coreNameLower = coreName;
  coreNameLower.toLowerCase();
  bool isMameCore = (coreNameLower == "arcade");
  bool needsSubsystemSearch = false;
  
  if (isMameCore && currentGame.length() > 0) {
    Serial.printf("Arcade core detected with active game: '%s'\n", currentGame.c_str());
    
    // Check if we need to search for subsystem info
    if (lastArcadeSystemeId.length() == 0) {
      Serial.println("No subsystem ID available - need to search");
      needsSubsystemSearch = true;
    } else {
      Serial.printf("Subsystem ID available: %s\n", lastArcadeSystemeId.c_str());
      // Check if subsystem-specific image exists
      String subsystemCoreName = "Arcade_" + lastArcadeSystemeId;
      // Defensive sanitization — keep filename rules consistent everywhere
      String safeSubsystemName = sanitizeCoreFilename(subsystemCoreName);
      String subsystemPath;
      
      if (ENABLE_ALPHABETICAL_FOLDERS) {
        String alphabetPath = getAlphabeticalPath(safeSubsystemName);
        subsystemPath = alphabetPath + "/" + safeSubsystemName + ".jpg";
      } else {
        subsystemPath = String(CORE_IMAGES_PATH) + "/" + safeSubsystemName + ".jpg";
      }
      
      if (!SD.exists(subsystemPath)) {
        Serial.printf("Subsystem image doesn't exist: %s\n", subsystemPath.c_str());
        Serial.println("Need to download subsystem image");
        needsSubsystemSearch = true; // Force search to ensure we have correct subsystem
      }
    }
  }
  
  // STEP 1: If Arcade needs subsystem search, do it FIRST
  if (needsSubsystemSearch) {
    Serial.println("=== PERFORMING MANDATORY SUBSYSTEM SEARCH ===");
    
    // Call the existing function to update subsystem info
    updateArcadeSubsystemForCurrentGame(coreName, currentGame);
    
    // Small delay to ensure processing completes
    delay(100);
    
    Serial.printf("After subsystem search - lastArcadeSystemeId: '%s'\n", 
                  lastArcadeSystemeId.c_str());
  }
  
  // STEP 2: Now try to find the appropriate image
  Serial.printf("Trying to find image for: '%s'\n", coreName.c_str());
  
  bool imageExists = findCoreImage(coreName, imagePath);
  bool shouldDownload = false;
  
  // ENHANCED LOGIC: Decide if download is needed
  if (FORCE_CORE_REDOWNLOAD) {
    Serial.println("FORCE_CORE_REDOWNLOAD enabled - will download regardless of existing image");
    shouldDownload = true;
  } else if (!imageExists) {
    Serial.println("No core image found - will attempt download");
    shouldDownload = true;
  } else {
    Serial.println("Image exists - checking if it's the correct one...");
    
    // ENHANCED CHECK: For Arcade, verify we have the right image
    if (isMameCore && lastArcadeSystemeId.length() > 0) {
      String expectedSubsystemName = "Arcade_" + lastArcadeSystemeId;
      
      // Check if current found image is actually the subsystem-specific image
      if (imagePath.indexOf(expectedSubsystemName) >= 0) {
        Serial.printf("Found correct subsystem image: %s\n", imagePath.c_str());
        shouldDownload = false;
      } else {
        Serial.printf("Found generic Arcade image, but need subsystem image: %s\n", expectedSubsystemName.c_str());
        Serial.println("Will download subsystem-specific image");
        shouldDownload = true;
      }
    } else {
      shouldDownload = false;
    }
  }
  
  // STEP 3: Download if necessary
  if (shouldDownload && ENABLE_AUTO_DOWNLOAD && WiFi.status() == WL_CONNECTED && !downloadInProgress) {
    Serial.println("Attempting core image download from ScreenScraper...");
    
    // Show downloading screen for core
    showCoreDownloadingScreen(coreName);
    
    if (downloadCoreImageFromScreenScraperDefault(coreName)) {
      Serial.println("Core image download successful!");
      
      // Re-check for image after successful download  
      if (findCoreImage(coreName, imagePath)) {
        Serial.printf("Image verified after download: %s\n", imagePath.c_str());
      } else {
        Serial.println("Download succeeded but image not found - using fallback");
      }
    } else {
      Serial.println("Core image download failed");
      
      // Try to find any existing image as fallback
      if (!findCoreImage(coreName, imagePath)) {
        Serial.println("No fallback image available - showing Menu Image With Core Overlay");
        coreDownloadFailedFor = coreName;  // Prevent screensaver from retrying this core
        showMenuImageWithCoreOverlay(coreName);
        return;
      }
    }
  }
  
  // STEP 4: Display the final image
  if (imagePath.length() > 0) {
    Serial.printf("Displaying core image: %s\n", imagePath.c_str());
    
    // Enhanced logging for subsystem images
    if (isMameCore && lastArcadeSystemeId.length() > 0) {
      if (imagePath.indexOf("Arcade_" + lastArcadeSystemeId) >= 0) {
        Serial.printf("SUCCESS: Displaying subsystem-specific image for subsystem %s\n", 
                     lastArcadeSystemeId.c_str());
      } else {
        Serial.printf("FALLBACK: Displaying generic Arcade image (subsystem %s image not available)\n", 
                     lastArcadeSystemeId.c_str());
      }
    }
    
    // Use the original display logic from the existing function
    if (displayCoreImage(imagePath)) {
      Serial.println("Image displayed correctly, adding footer");
      
      // Footer in PHYSICAL coordinates (full screen width, below image area)
      drawCoreImageFooter();
      
      Serial.println("Footer added successfully");
      return;
    } else {
      Serial.println("Error displaying image, fallback to menu image");
    }
  } else {
    Serial.printf("No image path available for core: %s\n", coreName.c_str());
  }
  
  // FALLBACK: Show menu image with core overlay
  showMenuImageWithCoreOverlay(coreName);
  
  Serial.println("=== ENHANCED CORE IMAGE DISPLAY COMPLETE ===\n");
}

// ========== TOUCH HANDLING FUNCTION ==========
// This function replaces the physical button checks (M5.BtnA, M5.BtnB, M5.BtnC)
// It converts touch coordinates and checks which button (if any) was pressed

void handleTouch() {
  // Step 1: Get current touch state from M5Unified
  // This returns a structure with all touch information
  auto touch = M5.Touch.getDetail();
  
  // Step 2: Check if this is a NEW touch event
  // wasPressed() is true only at the moment of initial contact
  // This prevents one touch from triggering multiple actions
  if (touch.wasPressed()) {
    
    // Step 3: Get physical touch coordinates (1280x720 space)
    int physicalX = touch.x;
    int physicalY = touch.y;
    
    // Step 4: Convert to logical coordinates (320x240 space)
    // This is where the magic happens - we transform M5Tab coordinates
    // back to the original M5Stack coordinate system
    // int logicalX = (int)(physicalX / SCALE_X);
    // int logicalY = (int)(physicalY / SCALE_Y);
    
    // Step 5: Debug logging (helpful during development and testing)
    Serial.println("Touch detected!");
    Serial.printf("  Physical coordinates: (%d, %d)\n", physicalX, physicalY);
    // Serial.printf("  Logical coordinates: (%d, %d)\n", logicalX, logicalY);
    
    // Step 6: Check each button in sequence
    // Using if-else ensures only one button can be activated per touch
    
    // GAME INFO subpage toggle (page 5 only).
    // The "1/2>>" indicator (logical 238,40 — physical ~566,280) is the visual
    // affordance, but the hit target is the WHOLE monitor area: everything
    // left of the PRV/SCAN/NEXT column (x>=950). Page 5 has no other
    // interactive element there, and using a single generous bound keeps it
    // immune to sampling hiccups and axis quirks.
    // Gated on a synopsis existing, because that is exactly when the indicator
    // is drawn; without it there is no second subpage to toggle to.
    if (currentPage == 5 && currentMeta.loaded &&
        currentMeta.synopsis.length() > 0 &&
        physicalX < 940) {
      Serial.println("  -> GAME INFO subpage toggle");

      // Feedback: repaint the indicator in white, beep, hold — same contract
      // as buttonPressFeedback(). The full redraw below restores the cyan.
      Lcd.setTextWrap(false);
      Lcd.setTextColor(THEME_WHITE, THEME_BLACK);
      Lcd.setTextSize(2);
      Lcd.setCursor(238, 40);
      Lcd.print(gameInfoSubPage == 0 ? "1/2>>" : "<<2/2");
      playNextButtonSound();
      delay(200);

      gameInfoSubPage ^= 1;
      gameInfoSubPageChange = millis();
      resetGameInfoSynScroll();
      needsRedraw = true;
      lastButtonPress = millis();
    }

    // Check PREV button
    else if (btnPrev.contains(physicalX, physicalY)) {
      Serial.println("  -> PREV button pressed");
      
      // Visual and audio feedback
      buttonPressFeedback(&btnPrev, playPrevButtonSound);
      
      // Execute PREV action: navigate to previous page
      currentPage = (currentPage - 1 + totalPages) % totalPages;
      needsRedraw = true;
      lastPageChange = millis();
      lastButtonPress = millis();
    }
    
    // Check SCAN button — global refresh, including forced ROM details rescan
    else if (btnScan.contains(physicalX, physicalY)) {
      // Lock: ignore additional SCAN presses while a scan is already running.
      if (scanInProgress) {
        Serial.println("  -> SCAN button pressed (IGNORED — already scanning)");
        lastButtonPress = millis();
      } else {
        Serial.println("  -> SCAN button pressed (global refresh + force ROM rescan)");
        
        // Quick visual+audio feedback (200 ms, label flashes white)
        buttonPressFeedback(&btnScan, playScanButtonSound);
        
        // === Enter SCANNING state ===
        scanInProgress = true;
        const char* originalLabel = btnScan.label;  // remember literal "SCAN" pointer
        btnScan.label = "SCANNING";
        
        // Erase the old "SCAN" label area before redrawing with longer text.
        // btnScan is at (950,300) with size 200x80; the label is centered.
        // Wipe a generous strip across the middle of the button so neither
        // "SCAN" nor "SCANNING" leave residual pixels at any phase.
        M5.Display.fillRect(btnScan.x + 10, btnScan.y + 24,
                            btnScan.w - 20, 32, THEME_BLACK);
        btnScan.draw(THEME_YELLOW);   // SCANNING in yellow to stand out
        
        // === Run the actual operation ===
        // Step 1: refresh all MiSTer state (core, game, system, network, etc.)
        updateMiSTerData();
        
        // Step 2: if a game is active, force a complete fresh search.
        if (currentGame.length() > 0) {
          Serial.println("Game active — forcing ROM details + image rescan");
          RomDetails fresh = getCurrentRomDetailsForced();
          lastRomHasCrc = fresh.available && fresh.hashCalculated && fresh.crc32.length() > 0;
          lastRomCrcChecked = true;
          Serial.printf("After SCAN rescan: CRC available = %s\n",
                        lastRomHasCrc ? "YES" : "NO");

          // SCAN ALWAYS forces a complete fresh image search, even when the
          // CRC was already known.
          lastSearchedGame        = "";
          lastGameImageOK         = false;
          lastGameSearchExhausted = false;

          btnScan.label   = originalLabel;
          scanInProgress  = false;
          lastButtonPress = millis();
          showGameImageScreen(currentCore, currentGame);
          showingCoreImage   = true;
          coreImageStartTime = millis();
          return;   // switched to image screen; skip the HUD-redraw tail
        }
        
        // === Exit SCANNING state ===
        // Restore original label BEFORE the page redraw is triggered, so any
        // btnScan.draw() called from updateDisplay() picks up "SCAN" again.
        btnScan.label = originalLabel;
        scanInProgress = false;
        
        // Wipe the (now larger) "SCANNING" footprint and draw "SCAN"
        M5.Display.fillRect(btnScan.x + 10, btnScan.y + 24,
                            btnScan.w - 20, 32, THEME_BLACK);
        btnScan.draw(THEME_CYAN);
        
        needsRedraw     = true;
        lastPageChange  = millis();
        lastButtonPress = millis();
      }
    }
    
    // Check NEXT button
    else if (btnNext.contains(physicalX, physicalY)) {
      Serial.println("  -> NEXT button pressed");
      
      // Visual and audio feedback
      buttonPressFeedback(&btnNext, playNextButtonSound);
      
      // Execute NEXT action: navigate to next page
      currentPage = (currentPage + 1) % totalPages;
      needsRedraw = true;
      lastPageChange = millis();
      lastButtonPress = millis();
    }
    
    // Touch was outside all buttons - ignore it
    else {
      Serial.println("  -> Touch outside button areas (ignored)");
    }
  }
}

void setup() {
  // =====================================================================
  // ANTI-CRASH BLOCK — must run BEFORE M5.begin() and Serial
  // =====================================================================

  // 1) Disable brownout detector to prevent resets during WiFi/SD power peaks.
  //    Only compiled on chips that have the register (ESP32/S2/S3).
  //    On ESP32-P4 this block is skipped at compile time — no error.
  #if HAS_BROWNOUT_REG
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  #endif

  // =====================================================================
  // Normal init
  // =====================================================================
  auto cfg = M5.config();

  cfg.clear_display = true;
  cfg.output_power = true;
  cfg.internal_imu = false;
  cfg.external_imu = false;

  M5.begin(cfg);

  // ========== START SERIAL FIRST ==========
  Serial.begin(115200);
  delay(500);  // Give the Serial port time to flush

  // =====================================================================
  // 2) Log the reset reason — tells us exactly WHY the device restarted.
  //    Check Serial Monitor after a crash to read this line.
  //    Reasons: POWERON=1, EXT=2, SW=3, PANIC=4, INT_WDT=5,
  //             TASK_WDT=6, WDT=7, DEEPSLEEP=8, BROWNOUT=9, SDIO=10
  // =====================================================================
  {
    esp_reset_reason_t reason = esp_reset_reason();
    const char* resetNames[] = {
      "UNKNOWN", "POWERON", "EXT_PIN", "SW_RESET",
      "PANIC/EXCEPTION", "INT_WATCHDOG", "TASK_WATCHDOG", "OTHER_WDT",
      "DEEPSLEEP", "BROWNOUT", "SDIO"
    };
    int idx = (int)reason;
    Serial.println("\n\n========================================");
    Serial.printf("  RESET REASON: %s (code %d)\n",
                  (idx >= 0 && idx <= 10) ? resetNames[idx] : "UNKNOWN", idx);
    if (reason == ESP_RST_PANIC)
      Serial.println("  >>> CRASH DETECTED: check for null pointer or stack overflow <<<");
    if (reason == ESP_RST_TASK_WDT)
      Serial.println("  >>> TASK WATCHDOG: main loop was blocked too long <<<");
    if (reason == ESP_RST_BROWNOUT)
      Serial.println("  >>> BROWNOUT: power supply insufficient during peak load <<<");
    if (reason == ESP_RST_SW)
      Serial.println("  >>> SW RESTART: ESP.restart() was called (likely low heap) <<<");
    Serial.println("========================================\n");
  }

  // =====================================================================
  // 3) Detect and log PSRAM — confirms large buffers can use external RAM
  // =====================================================================
  {
    bool hasPsram = psramFound();
    size_t psramSize = hasPsram ? ESP.getPsramSize() : 0;
    size_t freePsram = hasPsram ? ESP.getFreePsram() : 0;
    Serial.printf("[MEMORY] Internal heap: %u bytes free\n", ESP.getFreeHeap());
    Serial.printf("[MEMORY] PSRAM %s | Size: %u KB | Free: %u KB\n",
                  hasPsram ? "FOUND" : "NOT FOUND",
                  psramSize / 1024, freePsram / 1024);
    if (!hasPsram) {
      Serial.println("[MEMORY] WARNING: No PSRAM detected. Large allocs use internal heap.");
    }
  }

  Serial.println("\n\n=== BOOT START ===");
  
  Serial.println("=== TESTING SPEAKER ===");

  M5.Speaker.begin();
  M5.Speaker.setVolume(128);

  Serial.println("Playing Pac-Man boot sound...");
  M5.Speaker.tone(494, 100);  // B
  delay(110);
  M5.Speaker.tone(988, 100);  // B (high octave)
  delay(110);
  M5.Speaker.tone(740, 100);  // F#
  delay(110);
  M5.Speaker.tone(622, 100);  // D#
  delay(110);
  M5.Speaker.tone(494, 100);  // B
  delay(110);
  M5.Speaker.tone(740, 100);  // F#
  delay(110);
  M5.Speaker.tone(622, 150);  // D# (longer)
  delay(200);

  Serial.println("Speaker test complete");
  

  // Initialize speaker for M5Tab (M5Unified)
  /*auto spk_cfg = M5.Speaker.config();
  spk_cfg.sample_rate = 48000;  // Sample rate
  spk_cfg.task_priority = 2;    // Task priority
  spk_cfg.task_pinned_core = PRO_CPU_NUM;
  spk_cfg.dma_buf_count = 8;
  spk_cfg.dma_buf_len = 256;
  
  M5.Speaker.config(spk_cfg);
  M5.Speaker.begin();
  M5.Speaker.setVolume(100);  // High volume (0-255)*/
  
  Serial.println("Speaker initialized");
  
  M5.Display.setRotation(1);
  M5.Display.setBrightness(128);
  M5.Display.setColorDepth(16);

  Serial.println("=== MiSTer Monitor with Core and Games Images on M5Tab Starting ===");
  Serial.printf("Display: %dx%d\n", M5.Display.width(), M5.Display.height());
  Serial.printf("Target MiSTer IP: %s\n", misterIP);
  
  Serial.println("=== INITIALIZING SD CARD ===");
  Serial.printf("Using CS pin: GPIO %d\n", TFCARD_CS_PIN);
  Serial.println("Mode: SPI at 25 MHz");
  
  int sdRetries = 0;
  while (!SD.begin(TFCARD_CS_PIN, SPI, 25000000) && sdRetries < 5) {
    sdRetries++;
    Serial.printf("SD Card initialization attempt %d/5 failed!\n", sdRetries);
    delay(500);
  }
  
  if (sdRetries < 5) {
    Serial.println("SD Card initialized successfully");
    sdCardAvailable = true;
    
    // Display SD card information for diagnostics
    uint8_t cardType = SD.cardType();
    Serial.printf("Card Type: %s\n", 
                  cardType == CARD_MMC ? "MMC" :
                  cardType == CARD_SD ? "SDSC" :
                  cardType == CARD_SDHC ? "SDHC" : "UNKNOWN");
    
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("Card Size: %llu MB\n", cardSize);
  } else {
    Serial.println("SD Card initialization failed after 5 attempts");
    Serial.println("Application will continue but image features will be disabled");
    sdCardAvailable = false;
  }
  
  // ── Load /config.ini from SD card ─────────────────────────────────────────
  loadConfig(appConfig);

  ssid     = appConfig.ssid.c_str();
  password = appConfig.wifiPass.c_str();
  // MiSTer IP from config.ini. UDP discovery overwrites this at boot on
  // success; on failure this value remains as the fallback (which is the
  // whole point of the config.ini ip= key). appConfig is global, so the
  // c_str() pointer stays valid for the program's lifetime.
  misterIP = appConfig.misterIP.c_str();
    if (appConfig.ssDevUser.length() > 0) {
    _ss_dev_user_str = appConfig.ssDevUser;
    _ss_dev_pass_str = appConfig.ssDevPass;
  } else {
    _ss_dev_user_str = SS_DEV_ID_EMBEDDED;
    _ss_dev_pass_str = SS_DEV_PASS_EMBEDDED;
  }

  _ss_user_str            = appConfig.ssUser;
  _ss_pass_str            = appConfig.ssPass;
  _boxart_region_str      = appConfig.boxartRegion;
  _info_lang_str          = appConfig.infoLang;
  _info_synopsis_max      = appConfig.infoSynopsisMax;
  if (_info_synopsis_max < 200)  _info_synopsis_max = 200;
  if (_info_synopsis_max > 2000) _info_synopsis_max = 2000;
  _core_images_path_str   = appConfig.coreImagesPath;
  _default_core_image_str = appConfig.defaultCoreImage;

  GAME_MEDIA_ORDER_STR              = appConfig.gameMediaOrder;
  ARCADE_MEDIA_ORDER_STR           = appConfig.arcadeMediaOrder;
  ARCADE_SUBSYSTEM_MEDIA_ORDER_STR = appConfig.arcadeSubsystemMediaOrder;
  CORE_MEDIA_ORDER_STR             = appConfig.coreMediaOrder;

  CORE_IMAGE_TIMEOUT          = appConfig.coreImageTimeout;
  ENABLE_ALPHABETICAL_FOLDERS = appConfig.alphabeticalFolders;
  SCREENSCRAPER_TIMEOUT       = appConfig.ssTimeout;
  SCREENSCRAPER_RETRIES       = appConfig.ssRetries;
  USE_HTTPS_SCREENSCRAPER     = appConfig.ssUseHttps;
  ENABLE_DEBUG_MODE           = appConfig.debugMode;
  ENABLE_AUTO_DOWNLOAD        = appConfig.autoDownload;
  MAX_IMAGE_SIZE              = appConfig.maxImageSize;
  DOWNLOAD_TIMEOUT            = appConfig.downloadTimeout;
  FORCE_CORE_REDOWNLOAD       = appConfig.forceCoreRedownload;
  FORCE_GAME_REDOWNLOAD       = appConfig.forceGameRedownload;
  SCROLL_SPEED_MS             = appConfig.scrollSpeedMs;
  SCROLL_PAUSE_START_MS       = appConfig.scrollPauseStartMs;
  SCROLL_PAUSE_END_MS         = appConfig.scrollPauseEndMs;

  Serial.printf("[CONFIG] MiSTer IP   : %s\n", misterIP);
  Serial.printf("[CONFIG] WiFi SSID   : %s\n", ssid);
  Serial.printf("[CONFIG] SS User     : %s\n", SCREENSCRAPER_USER);
  Serial.printf("[CONFIG] Region      : %s\n", BOXART_REGION);
  Serial.printf("[CONFIG] Game order       : %s\n", GAME_MEDIA_ORDER_STR.c_str());
  Serial.printf("[CONFIG] Arcade order     : %s\n", ARCADE_MEDIA_ORDER_STR.c_str());
  Serial.printf("[CONFIG] Arcade subsys ord: %s\n", ARCADE_SUBSYSTEM_MEDIA_ORDER_STR.c_str());
  Serial.printf("[CONFIG] Core order       : %s\n", CORE_MEDIA_ORDER_STR.c_str());
  // ──────────────────────────────────────────────────────────────────────────

  bootFrameLoaded = false;  // Reset boot frame flag
  backgroundLoaded = false; // Ensure interface frame loads

  // Boot screen
  showBootSequence();
  
  // FIRST: Connect WiFi (highest priority)
  connectWithAnimation();

  // Get current core and game BEFORE showing interface
  if (!getStateSnapshot()) {
    getCurrentCore();
    getCurrentGame();
  }
  
  // If SD available, show appropriate image
  if (sdCardAvailable) {
    // Prioritize game image over core image
    if (currentGame.length() > 0 && currentCore != "MENU") {
      showGameImageScreen(currentCore, currentGame);
    } else {
      showCoreImageScreenWithAutoDownload(currentCore);
    }
    showingCoreImage = true;
    coreImageStartTime = millis(); // Record start time
    lastCoreCheck = millis(); // Initialize check timer
  }
  

  Serial.println("==========================================\n");
  
  // First complete update
  updateMiSTerData();
  needsRedraw = true;
  
  // Initialize last button press time
  lastButtonPress = millis();

  if (ENABLE_AUTO_DOWNLOAD) {
    Serial.println("ScreenScraper auto-download enabled");
    Serial.printf("User: %s\n", SCREENSCRAPER_USER);
  }
  // Start screenshot HTTP server (only if WiFi connected)
  if (WiFi.status() == WL_CONNECTED) {
    setupScreenshotServer();
  }
}

void loop() {
  M5.update();
  screenshotServer.handleClient();  // Non-blocking screenshot server poll

  // --- Periodic MiSTer re-discovery while offline ---------------------------
  // Discovery runs once at boot. If the MiSTer wasn't on the network yet
  // (e.g. its IP delayed by a CIFS mount), boot-time discovery fails and the
  // display would stay OFFLINE forever. While disconnected, re-broadcast
  // periodically to pick the MiSTer up once it appears, then refresh state.
  {
    static unsigned long lastRediscovery = 0;
    const unsigned long  REDISCOVERY_INTERVAL_MS = 10000;
    if (!connected && WiFi.status() == WL_CONNECTED &&
        millis() - lastRediscovery > REDISCOVERY_INTERVAL_MS) {
      lastRediscovery = millis();
      Serial.println("[REDISCOVERY] Offline — re-broadcasting for MiSTer...");
      if (discoverMister(2, 400)) {        // quick probe (<=0.8s); sets misterIP
        Serial.printf("[REDISCOVERY] Found at %s — refreshing state\n", misterIP);
        updateMiSTerData();                // getCurrentCore() sets connected on HTTP 200
        needsRedraw = true;
      }
    }
  }
  // --------------------------------------------------------------------------
  // Reconnect banner: fire on any OFFLINE -> ONLINE transition, no matter which
  // path restored the link (re-discovery, screensaver refresh, normal polling).
  if (connected && !wasConnected) {
    Serial.println("[RECONNECT] Link restored — showing banner");
    showReconnectBanner();
    delay(1200);
    needsRedraw = true;          // repaint normal UI over the banner next loop
  }
  wasConnected = connected;

  // SAFETY: Check for critical memory levels every 30 seconds
  static unsigned long lastMemoryCheck = 0;
  if (millis() - lastMemoryCheck > 30000) {
    size_t freeHeap    = ESP.getFreeHeap();
    size_t freePsram   = psramFound() ? ESP.getFreePsram() : 0;
    size_t largestFree = ESP.getMaxAllocHeap();
    Serial.printf("[HEAP] Free: %u B | Largest block: %u B | PSRAM free: %u KB\n",
                  freeHeap, largestFree, freePsram / 1024);

    if (freeHeap < 50000) {
      Serial.printf("[HEAP] CRITICAL: %u bytes free — running forceMemoryCleanup()\n", freeHeap);
      forceMemoryCleanup();

      if (ESP.getFreeHeap() < 30000) {
        // Log all memory state before intentional restart
        Serial.printf("[HEAP] EMERGENCY RESTART: heap %u < 30000 bytes\n", ESP.getFreeHeap());
        Serial.printf("[HEAP] PSRAM free: %u bytes\n", freePsram);
        Serial.printf("[HEAP] This restart is intentional (SW_RESET reason on next boot)\n");
        delay(200);  // Flush serial
        ESP.restart();
      }
    }
    lastMemoryCheck = millis();
  }

  if (showingCoreImage) {
  // Check timeout for core image display (30 seconds)
  if (millis() - coreImageStartTime > CORE_IMAGE_TIMEOUT) {
    if (currentCore == coreDownloadFailedFor && currentGame.length() == 0) {
      // No image available for this core AND no game loaded — re-show menu overlay and stay
      showMenuImageWithCoreOverlay(currentCore);
      coreImageStartTime = millis();
      return;
    }
    showingCoreImage = false;
    backgroundLoaded = false;
    needsRedraw = true;
    return;
  }
    
  // In image mode: ANY touch exits to interface
  // We don't need to check specific buttons, any touch will do
  auto touch = M5.Touch.getDetail();
  if (touch.wasPressed()) {
    int tx = touch.x;
    int ty = touch.y;

    // === GAME INFO button: right third of the image footer (physical) ===
    // Footer band starts at y=620; the right third begins at x=853. Tested
    // with the SAME predicate that decides whether the button is drawn: when
    // it is hidden this falls through to the default "exit to monitor".
    if (currentGame.length() > 0 && tx >= 853 && ty >= 620 && gameInfoAvailable()) {
      Serial.println("GAME INFO button pressed - opening panel");

      // Same feedback contract as buttonPressFeedback(): flash white, beep,
      // hold 200 ms. No restore needed — the panel replaces this screen.
      drawGameInfoIcon(true);
      playNextButtonSound();
      delay(200);

      currentPage           = 5;
      gameInfoSubPage       = 0;
      gameInfoSubPageChange = millis();
      resetGameInfoSynScroll();
      showingCoreImage      = false;
      backgroundLoaded      = false;
      needsRedraw           = true;
      lastButtonPress       = millis();
      return;
    }

    // === Default: exit to monitor ===
    Serial.println("Touch detected - exiting core image to interface");
    M5.Display.fillRect(0, 0, 1280, 80, THEME_CYAN);
    M5.Display.setTextSize(4);
    M5.Display.setTextColor(THEME_BLACK);
    M5.Display.setCursor(400, 25);
    M5.Display.print("LOADING INTERFACE...");
    showingCoreImage         = false;
    backgroundLoaded         = false;
    needsRedraw              = true;
    lastButtonPress          = millis();
    return;
  }
    
    // Check core and game changes every 10 seconds
    if (millis() - lastCoreCheck > 10000) {
      String oldCore = currentCore;
      String oldGame = currentGame;
      Serial.println("Checking core/game change while showing image...");
      if (!getStateSnapshot()) {
        getCurrentCore();
        getCurrentGame();
      }
      
      // Check if game changed first (higher priority)
      if (oldGame != currentGame && sdCardAvailable) {
        lastRotationTime = 0; // Reset rotation timer for new game
        String coreNameLower = currentCore;
  coreNameLower.toLowerCase();
  bool isMameCore = (coreNameLower == "arcade");
  
  if (isMameCore && oldGame != currentGame) {
    Serial.printf("Arcade game change during display - forcing subsystem reset\n");
    Serial.printf("Previous game: '%s'\n", oldGame.c_str());
    Serial.printf("Current game: '%s'\n", currentGame.c_str());
    Serial.printf("Previous lastArcadeSystemeId: '%s'\n", lastArcadeSystemeId.c_str());
    
    // Force subsystem reset for game change during display
    lastArcadeSystemeId = "";
    lastProcessedGame = "";
    forceSubsystemUpdate = true;
    
    Serial.printf("Cleared subsystem state for display game change\n");
  }
        if (currentGame.length() > 0) {
  Serial.printf("Game changed during display: '%s' -> '%s'\n", oldGame.c_str(), currentGame.c_str());
  if (isMameCore) {
      Serial.printf("Calling subsystem update during display for Arcade...\n");
      updateArcadeSubsystemForCurrentGameEnhanced(currentCore, currentGame, forceSubsystemUpdate);
      forceSubsystemUpdate = false; // Reset flag after use
    } else {
      updateArcadeSubsystemForCurrentGame(currentCore, currentGame);
    }
  startCrcRecurrentForGame(currentGame, currentCore);
  
  showGameImageScreen(currentCore, currentGame);
} else {
  Serial.println("Game unloaded, showing core image");
  lastArcadeSystemeId = "";
    lastProcessedGame = "";
    forceSubsystemUpdate = false;
    Serial.printf("Cleared subsystem state on display game unload\n");
  stopCrcRecurrent();
  
  showCoreImageScreenWithAutoDownload(currentCore);
}
        coreImageStartTime = millis(); // Reset timer
      }
      // Check if core changed (ALWAYS update core image when core changes, regardless of game state)
      else if (oldCore != currentCore && sdCardAvailable) {
        Serial.printf("Core changed during display: '%s' -> '%s'\n", oldCore.c_str(), currentCore.c_str());
        coreDownloadFailedFor = "";  // New core — allow download attempt
        // If there's an active game, show game image, otherwise show core image
        if (currentGame.length() > 0) {
          Serial.printf("Core changed with active game, showing game image for new core\n");
          showGameImageScreen(currentCore, currentGame);
        } else {
          Serial.printf("Core changed without game, showing core image\n");
          showCoreImageScreenWithAutoDownload(currentCore);
        }
        coreImageStartTime = millis(); // Reset timer
      }
      
      lastCoreCheck = millis();
    }
    
  // Rotation logic for games (only when game is active and core is not MENU)
  // Rotation debug logging
  static unsigned long lastRotationLog = 0;
  if (millis() - lastRotationLog > 5000) { // Log every 5 seconds
    Serial.printf("ROTATION STATUS: game='%s', core='%s', showingGameImage=%s, lastRotationTime=%lu\n", 
                  currentGame.c_str(), currentCore.c_str(), 
                  showingGameImage ? "true" : "false", lastRotationTime);
    if (lastRotationTime > 0) {
      Serial.printf("   Time since last rotation: %lu ms (target: 30000)\n", millis() - lastRotationTime);
    }
    lastRotationLog = millis();
  }
  
  // Rotation logic for games (only when game is active and core is not MENU)
    if (currentGame.length() > 0 && currentCore != "MENU") {
      // Initialize rotation timer on first display
      if (lastRotationTime == 0) {
        lastRotationTime = millis();
        showingGameImage = true;
      }
      
      // Check for 30-second rotation
      if (millis() - lastRotationTime > 30000) { // 30 seconds
        showingGameImage = !showingGameImage;
        lastRotationTime = millis();
        
        if (showingGameImage) {
          showGameImageScreen(currentCore, currentGame);
        } else {
          showCoreImageScreenWithAutoDownload(currentCore);
        }
        coreImageStartTime = millis(); // Reset main timer
      }
    }

    if (showingCoreImage) {
      // Animate the GAME: scroll on any fullscreen image screen that has a
      // game name in the footer (game image, core image with GAME line,
      // menu image with overlay).
      if (currentGame.length() > 0) {
        static unsigned long lastFooterUpdate = 0;
        if (millis() - lastFooterUpdate > 100) {
          if (imageFooterScroll.needsScroll && imageFooterScroll.fullText.length() > 0) {
            String scrolledText = getScrolledText(&imageFooterScroll);
            
            // Pad the visible window to a constant character count so the
            // pixel width is stable across frames. This is what lets us use
            // setTextColor(fg, bg) without leaving stale glyphs at the right.
            int targetLen = imageFooterScroll.maxChars;
            while ((int)scrolledText.length() < targetLen) {
              scrolledText += ' ';
            }
            
            // Y/X coordinates depend on which screen we're on.
            int gameLineY = showingGameImage ? 638
                          : (currentCore == coreDownloadFailedFor ? 668 : 638);
            // Must match the draw sites exactly, or the scroll would jump:
            // 198 in addGameImageFooter()/drawCoreImageFooter(), 120 in
            // showMenuImageWithCoreOverlay().
            int textCursorX = showingGameImage ? 198
                          : (currentCore == coreDownloadFailedFor ? 120 : 198);
            
            M5.Display.setTextWrap(false);
            M5.Display.setTextColor(THEME_YELLOW, THEME_BLACK);
            M5.Display.setTextSize(3);
            M5.Display.setCursor(textCursorX, gameLineY);
            M5.Display.print(scrolledText);
          }
          lastFooterUpdate = millis();
        }
      }
    }

    // Don't continue with rest of loop while showing image
    return;
  }
  
  // === NORMAL INTERFACE CODE ===
  
  // Check timeout to activate screensaver
  // The GAME INFO panel governs its own exit when it has a synopsis to show:
  // it leaves as soon as the text has finished scrolling, however long that
  // takes. The 1-min timeout is only a fallback for panels that never reach
  // that state (no metadata, or metadata without a synopsis). Both funnel into
  // the same return-to-image path below rather than duplicating it.
  bool gameInfoSelfExits = (currentPage == 5 && currentMeta.loaded &&
                            currentMeta.synopsis.length() > 0);
  unsigned long screensaverTimeout =
      (currentPage == 5) ? GAMEINFO_SCREEN_TIMEOUT : SCREENSAVER_TIMEOUT;
  bool timeToLeave =
      (!gameInfoSelfExits && millis() - lastButtonPress > screensaverTimeout) ||
      (currentPage == 5 && gameInfoForceExit);
  gameInfoForceExit = false;                 // one-shot: never latches
  if (!showingCoreImage && sdCardAvailable && timeToLeave
      && (currentCore != coreDownloadFailedFor || currentGame.length() > 0 || FORCE_CORE_REDOWNLOAD)) {
    Serial.println("=== SCREENSAVER ACTIVATION ANALYSIS ===");
    Serial.printf("Current core: '%s'\n", currentCore.c_str());
    Serial.printf("Current game: '%s'\n", currentGame.c_str());
    Serial.printf("Connected: %s\n", connected ? "YES" : "NO");
    Serial.printf("Time since last button: %lu ms\n", millis() - lastButtonPress);
    
    // ENHANCED LOGIC: Force fresh data check before screensaver
    Serial.println("Forcing fresh data check before screensaver...");
    String oldCore = currentCore;
    String oldGame = currentGame;
    
    // Get fresh data from MiSTer
    if (!getStateSnapshot()) {
      getCurrentCore();
      getCurrentGame();
    }
    
    if (oldCore != currentCore) {
      Serial.printf("Core changed during screensaver check: '%s' -> '%s'\n", oldCore.c_str(), currentCore.c_str());
    }
    if (oldGame != currentGame) {
      Serial.printf("Game changed during screensaver check: '%s' -> '%s'\n", oldGame.c_str(), currentGame.c_str());
    }
    
    Serial.println("Activating screensaver - showing image due to inactivity");
    
    // ENHANCED DECISION LOGIC: Better arcade detection
    bool isArcadeCore = (currentCore.equalsIgnoreCase("mame") || 
                         currentCore.equalsIgnoreCase("arcade"));
    bool hasActiveGame = (currentGame.length() > 0);
    bool shouldShowGame = false;
    
    if (hasActiveGame && (currentCore != "MENU" && currentCore != "Menu")) {
      shouldShowGame = true;
      Serial.printf("Active game detected: '%s' on core '%s'\n", currentGame.c_str(), currentCore.c_str());
    } else if (isArcadeCore && !hasActiveGame) {
      Serial.printf("Arcade core without game - checking if game detection failed\n");
      // For arcade cores, try to show core image even without detected game
      shouldShowGame = false;
    } else if (currentCore == "MENU" || currentCore == "Menu") {
      Serial.printf("Menu core detected - showing menu image\n");
      shouldShowGame = false;
    } else {
      Serial.printf("No active game, showing core image for: '%s'\n", currentCore.c_str());
      shouldShowGame = false;
    }
    
    if (shouldShowGame) {
      Serial.printf("Showing game image: '%s' on '%s'\n", currentGame.c_str(), currentCore.c_str());
      showGameImageScreen(currentCore, currentGame);
    } else {
      Serial.printf("Showing core image for: '%s'\n", currentCore.c_str());
      showCoreImageScreenWithAutoDownload(currentCore);
    }
    
    showingCoreImage = true;
    coreImageStartTime = millis();
    lastCoreCheck = millis();
    Serial.printf("Set showingCoreImage = true, timestamp = %lu\n", coreImageStartTime);
    Serial.println("=== SCREENSAVER ACTIVATION COMPLETE ===");
    return;
  }
  
  // Navigation with debounce
  handleTouch();

  if (showingCoreImage) return;
  
  static unsigned long lastStateLog = 0;
  if (millis() - lastStateLog > 30000) {
    Serial.printf("STATE: currentGame='%s', crcRecurrentActive=%s, downloadInProgress=%s\n", 
                  currentGame.c_str(), 
                  crcRecurrentActive ? "YES" : "NO",
                  downloadInProgress ? "YES" : "NO");
    lastStateLog = millis();
  }

  processCrcRecurrent();

  // Update data every 60 seconds (1 minute)
  if (millis() - lastUpdate > 60000) {
    String oldCore = currentCore;
    String oldGame = currentGame;
    updateMiSTerData();
    
    
// Detect game change with arcade subsystem reset
if (oldGame != currentGame && sdCardAvailable) {
  lastRotationTime = 0; // Reset rotation timer for new game
  Serial.printf("=== ENHANCED GAME CHANGE DETECTED ===\n");
  Serial.printf("Previous game: '%s' (length: %d)\n", oldGame.c_str(), oldGame.length());
  Serial.printf("Current game: '%s' (length: %d)\n", currentGame.c_str(), currentGame.length());
  Serial.printf("Current core: '%s'\n", currentCore.c_str());
  Serial.printf("SD card available: %s\n", sdCardAvailable ? "YES" : "NO");
  
  // CRITICAL: For Arcade core game changes, force subsystem reset
  String coreNameLower = currentCore;
  coreNameLower.toLowerCase();
  bool isMameCore = (coreNameLower == "arcade");
  
  if (isMameCore) {
    Serial.printf("Arcade core detected - preparing for subsystem update\n");
    Serial.printf("Previous processed game: '%s'\n", lastProcessedGame.c_str());
    Serial.printf("Current lastArcadeSystemeId: '%s'\n", lastArcadeSystemeId.c_str());
    
    // FORCE subsystem reset for any game change in Arcade
    if (oldGame != currentGame) {
      Serial.printf("FORCING subsystem reset due to game change\n");
      lastArcadeSystemeId = "";           // Clear old subsystem
      lastProcessedGame = "";             // Clear processed game tracker
      forceSubsystemUpdate = true;        // Force update flag
      gameChangeTime = millis();          // Record change time
      
      Serial.printf("Cleared subsystem state for new arcade game\n");
    }
  }
  
  if (currentGame.length() > 0) {
    Serial.printf("Game loaded: '%s' on core '%s'\n", currentGame.c_str(), currentCore.c_str());
    
    // Update subsystem with special handling for Arcade
    if (isMameCore) {
      Serial.printf("Calling ENHANCED subsystem update for Arcade with force=%s...\n", 
                   forceSubsystemUpdate ? "YES" : "NO");
      updateArcadeSubsystemForCurrentGameEnhanced(currentCore, currentGame, forceSubsystemUpdate);
      forceSubsystemUpdate = false; // Reset flag after use
    } else {
      // For non-Arcade cores, use original function
      updateArcadeSubsystemForCurrentGame(currentCore, currentGame);
    }
    
    startCrcRecurrentForGame(currentGame, currentCore);
    
    Serial.printf("Calling showGameImageScreen()...\n");
    showGameImageScreen(currentCore, currentGame);
    Serial.printf("showGameImageScreen() returned\n");
  } else if (oldGame.length() > 0) {
    Serial.println("Game unloaded, returning to core image");
    
    // Clear ALL subsystem-related state when unloading game
    lastArcadeSystemeId = "";
    lastProcessedGame = "";
    forceSubsystemUpdate = false;
    Serial.printf("Cleared ALL subsystem state on game unload\n");
    
    stopCrcRecurrent();
    
    Serial.printf("Calling showCoreImageScreenWithAutoDownload()...\n");
    showCoreImageScreenWithAutoDownload(currentCore);
    Serial.printf("showCoreImageScreenWithAutoDownload() returned\n");
  }
  
  showingCoreImage = true;
  coreImageStartTime = millis();
  lastButtonPress = millis();
  Serial.printf("Set showingCoreImage = true\n");
  Serial.printf("=== ENHANCED GAME CHANGE HANDLING COMPLETE ===\n");
}
    
    // Detect core change with detailed logging (only if no game active)
    if (oldCore != currentCore && currentGame.length() == 0 && sdCardAvailable) {
      Serial.printf("\n=== CORE CHANGE DETECTED ===\n");
      Serial.printf("Previous core: '%s'\n", oldCore.c_str());
      Serial.printf("Current core: '%s'\n", currentCore.c_str());
      Serial.printf("No active game (length: %d)\n", currentGame.length());
      Serial.printf("Calling showCoreImageScreenWithAutoDownload()...\n");
      showCoreImageScreenWithAutoDownload(currentCore);
      Serial.printf("showCoreImageScreenWithAutoDownload() returned\n");
      
      showingCoreImage = true;
      coreImageStartTime = millis();
      lastButtonPress = millis(); // Reset screensaver timer for automatic image display
      Serial.printf("Set showingCoreImage = true, timestamp = %lu\n", coreImageStartTime);
      Serial.printf("=== CORE CHANGE HANDLING COMPLETE ===\n");
      return;
    }
    
    needsRedraw = true;
    lastUpdate = millis();
  }
  
  // === GAME INFO subpage handling ===========================================
  // Lifecycle of the panel when left alone:
  //   subpage 1/2 (fields)   -> 30 s, then flip to the synopsis
  //   subpage 2/2 (synopsis) -> exactly as long as its scroll takes, plus a
  //                             2 s tail, then exit back to the game image
  // A synopsis short enough to need no scrolling dwells for GAMEINFO_SYN_FIT_MS.
  // Nothing here touches lastButtonPress, so the 1-min fallback timeout still
  // rescues the panel when there is no metadata (and hence no synopsis) at all.
  {
    static int giLastPage = -1;
    if (currentPage == 5 && !showingCoreImage) {
      if (giLastPage != 5) {
        gameInfoSubPage = 0;
        gameInfoSubPageChange = millis();
        resetGameInfoSynScroll();
      } else if (currentMeta.loaded && currentMeta.synopsis.length() > 0) {
        if (gameInfoSubPage == 0) {
          if (millis() - gameInfoSubPageChange > GAMEINFO_SUBPAGE_TIMEOUT) {
            gameInfoSubPage = 1;
            gameInfoSubPageChange = millis();
            resetGameInfoSynScroll();
            needsRedraw = true;
          }
        } else {
          tickGameInfoSynScroll();
          if (gameInfoSynCycled) {
            unsigned long dwell = gameInfoSynNeedsScroll() ? GAMEINFO_SYN_EXIT_MS
                                                           : GAMEINFO_SYN_FIT_MS;
            if (millis() - gameInfoSynCycledTime >= dwell) {
              Serial.println("GAME INFO: synopsis finished — returning to image");
              gameInfoForceExit = true;
            }
          }
        }
      }
    }
    // Leaving the panel (or any page) for image mode forces a clean re-entry.
    giLastPage = showingCoreImage ? -1 : currentPage;
  }

  // Only redraw when necessary
  if (needsRedraw) {
    updateDisplay();
    needsRedraw = false;
  }
  
  static unsigned long lastFooterScrollUpdate = 0;
  if (millis() - lastFooterScrollUpdate > 100) { // Update every 100ms
    // Page 5 (GAME INFO) draws no game name in the footer, so nothing to scroll.
    if (!showingCoreImage && currentPage != 5 && gameFooterScroll.needsScroll) {
      String textToShow = (currentGame.length() > 0) ? currentGame : currentCore;
      
      if (gameFooterScroll.fullText == textToShow) {
        // Same physical coordinates as drawFooter() (size 2 → 12 px/char physical):
        //   prefix "GAME: " or "CORE: " ends at x = 120*SCALE_X + 6*12 = 552 phys.
        // We only redraw the value zone (from x=552 onwards), padded to maxChars
        // for stable width. flicker-free thanks to setTextColor(fg, bg).
        const int textStartX_phys = 120 * SCALE_X + 6 * 12;  // 552
        
        String displayText = getScrolledText(&gameFooterScroll);
        while ((int)displayText.length() < gameFooterScroll.maxChars) {
          displayText += ' ';
        }
        
        M5.Display.setTextWrap(false);
        M5.Display.setTextColor(THEME_CYAN, THEME_BLACK);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(textStartX_phys, 220 * SCALE_Y);
        M5.Display.print(displayText);
      }
    }
    lastFooterScrollUpdate = millis();
  }
  // Refresh the core name scroll on the main HUD page (page 0).
  static unsigned long lastMainHUDCoreUpdate = 0;
  if (currentPage == 0 && !showingCoreImage &&
      millis() - lastMainHUDCoreUpdate > 100) {
    if (mainHUDCoreScroll.needsScroll) {
      String displayText = getScrolledText(&mainHUDCoreScroll);
      while ((int)displayText.length() < mainHUDCoreScroll.maxChars) {
        displayText += ' ';
      }
      
      // Same physical position as Lcd.setCursor(20, 80) at size 3 inside
      // the panel of color = (connected ? THEME_GREEN : THEME_YELLOW).
      // Lcd maps (20,80) → physical (130, 360). Width: 14 chars × 12 px logical
      // × 2 scale × 1 (size 3 doubles per glyph at 6px logical → 12 logical
      // × 2 scale = 24 px per char, 14 × 24 = 336 px wide).
      uint16_t panelColor = connected ? THEME_GREEN : THEME_YELLOW;
      Lcd.setTextColor(THEME_BLACK, panelColor);
      Lcd.setTextSize(3);
      Lcd.setCursor(20, 80);
      Lcd.print(displayText);
    }
    lastMainHUDCoreUpdate = millis();
  }

  // Scroll the game title on the GAME INFO page (5). Repaints only the title
  // row, at exactly the same cursor/size/colours displayGameInfo() uses, so
  // it stays in sync across subpage flips without a full redraw.
  static unsigned long lastGameInfoTitleUpdate = 0;
  if (currentPage == 5 && !showingCoreImage &&
      millis() - lastGameInfoTitleUpdate > 100) {
    if (gameInfoTitleScroll.needsScroll) {
      String displayTitle = getScrolledText(&gameInfoTitleScroll);
      while ((int)displayTitle.length() < gameInfoTitleScroll.maxChars) {
        displayTitle += ' ';
      }
      Lcd.setTextWrap(false);
      Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
      Lcd.setTextSize(2);
      Lcd.setCursor(10, 40);
      Lcd.print(displayTitle);
    }
    lastGameInfoTitleUpdate = millis();
  }

  // Scroll any over-long grid value (genre, developer, publisher...) on the
  // fields subpage. Each row repaints only its value column, at the y that
  // displayGameInfo() recorded when it centred the block.
  static unsigned long lastGameInfoRowUpdate = 0;
  if (currentPage == 5 && !showingCoreImage && gameInfoSubPage == 0 &&
      millis() - lastGameInfoRowUpdate > 100) {
    Lcd.setTextWrap(false);
    Lcd.setTextColor(THEME_WHITE, THEME_BLACK);
    Lcd.setTextSize(1.5);
    for (int r = 0; r < 6; r++) {
      if (!gameInfoRowShown[r] || !gameInfoRowScroll[r].needsScroll) continue;
      String v = getScrolledText(&gameInfoRowScroll[r]);
      while ((int)v.length() < gameInfoRowScroll[r].maxChars) v += ' ';
      Lcd.setCursor(GI_VAL_X, gameInfoRowY[r]);
      Lcd.print(v);
    }
    lastGameInfoRowUpdate = millis();
  }
  
  // Subtle animations only for specific elements
  if (millis() - animTimer > 1000) {
    blinkState = (blinkState + 1) % 4;
    
    // Only update status indicators without clearing screen
    if (currentPage == 0) {
      drawHudConnectionDot(connected, connected && blinkState < 2);
    }
    animTimer = millis();
  }
  
  // Poll fast on the GAME INFO page too: its title/grid scroll refreshers make
  // each iteration longer, and at a 100 ms cadence a quick tap on the subpage
  // indicator can fall entirely between two touch samples. The refreshers keep
  // their own 100 ms timers, so this only raises the touch sampling rate.
  delay(needsRedraw || showingCoreImage || currentPage == 5 ? 10 : 100);
}

void initSDCard() {
  Serial.println("\n=== Initializing SD card ===");
  
  // Give WiFi time to stabilize
  delay(500);
  
  // Try to initialize SD with specific configuration for M5Stack
  if (SD.begin(TFCARD_CS_PIN)) {
    sdCardAvailable = true;
    Serial.println("SD card initialized successfully");
    
    // Basic verification without consuming much memory
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      Serial.println("No SD card detected");
      sdCardAvailable = false;
      return;
    }
    
    Serial.printf("Card type: %s\n", 
                  cardType == CARD_MMC ? "MMC" :
                  cardType == CARD_SD ? "SDSC" :
                  cardType == CARD_SDHC ? "SDHC" : "UNKNOWN");
    
    // Verify only that main directory exists
    if (SD.exists(CORE_IMAGES_PATH)) {
      Serial.printf("Directory found: %s\n", CORE_IMAGES_PATH);
    } else {
      Serial.printf("Creating directory: %s\n", CORE_IMAGES_PATH);
      if (!SD.mkdir(CORE_IMAGES_PATH)) {
        Serial.println("Error creating directory");
        sdCardAvailable = false;
        return;
      }
    }
    
    // Create subdirectories only if enabled
    if (ENABLE_ALPHABETICAL_FOLDERS) {
      // Create # folder for numbers
      String numDir = String(CORE_IMAGES_PATH) + "/#";
      if (!SD.exists(numDir)) SD.mkdir(numDir);
      
      // Create some basic folders (not all)
      char basicFolders[] = {'A', 'G', 'M', 'N', 'S'};
      for (char c : basicFolders) {
        String subdir = String(CORE_IMAGES_PATH) + "/" + String(c);
        if (!SD.exists(subdir)) SD.mkdir(subdir);
      }
      Serial.println("Basic subdirectories created");
    }
    
  } else {
    sdCardAvailable = false;
    Serial.println("Error initializing SD card");
  }
}

// Implementation of the debug function (add this with other functions)
void checkMisterDebugState() {
  /**
   * Quick debug check when Menu state is detected
   * Helps diagnose if MiSTer is really in menu or there's a detection issue
   */
  
  // Only check occasionally to avoid spam
  static unsigned long lastDebugCheck = 0;
  if (millis() - lastDebugCheck < 15000) { // Every 15 seconds max
    return;
  }
  lastDebugCheck = millis();
  
  Serial.println("=== QUICK MISTER DEBUG CHECK ===");
  
  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/debug/game";
  
  http.begin(url);
  http.setTimeout(3000); // Quick timeout
  http.addHeader("User-Agent", "M5Stack-Monitor");
  
  int code = http.GET();
  
  if (code == 200) {
    String response = http.getString();
    Serial.printf("Game debug: %s\n", response.substring(0, 200).c_str());
    
    // Quick check for arcade indicators in debug response
    if (response.indexOf("arcade") > 0) {
      Serial.println("DEBUG: Arcade activity detected despite Menu core!");
    }
    
    if (response.indexOf("\"game\":\"\"") < 0 && response.indexOf("game") > 0) {
      Serial.println("DEBUG: Game activity detected despite Menu core!");
    }
  } else {
    Serial.printf("Debug endpoint failed: HTTP %d\n", code);
  }
  
  http.end();
  Serial.println("=== DEBUG CHECK COMPLETE ===");
}

void checkServerErrorState() {
  /**
   * Check server error state using the new endpoint
   * ONLY executes if there's successful connection (doesn't add load on errors)
   */
  
  // Only check occasionally to avoid overload
  static unsigned long lastErrorCheck = 0;
  unsigned long now = millis();
  if (now - lastErrorCheck < 30000) { // Every 30 seconds
    return;
  }
  lastErrorCheck = now;
  
  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/error_state";
  
  http.begin(url);
  http.setTimeout(3000); // Short timeout
  http.addHeader("User-Agent", "M5Stack-Monitor");
  
  int code = http.GET();
  
  if (code == 200) {
    String response = http.getString();
    
    // ULTRA SIMPLE JSON PARSING - Only look for specific fields
    if (response.indexOf("\"has_error\":true") > 0) {
      serverHasError = true;
      
      // Extract error type (simple parsing without libraries)
      int errorStart = response.indexOf("\"error_state\":\"") + 15;
      if (errorStart > 14) {
        int errorEnd = response.indexOf("\"", errorStart);
        if (errorEnd > errorStart && errorEnd - errorStart < 50) {
          serverErrorType = response.substring(errorStart, errorEnd);
          Serial.printf("Server reports error: %s\n", serverErrorType.c_str());
        }
      }
    } else {
      serverHasError = false;
      serverErrorType = "";
    }
  }
  
  http.end();
}

void showGameImageScreen(String coreName, String gameName) {
  Serial.printf("\n=== GAME IMAGE SCREEN (REDIRECT TO STREAMING-SAFE) ===\n");
  Serial.printf("Core: '%s' | Game: '%s'\n", coreName.c_str(), gameName.c_str());



  Serial.printf("Redirecting to streaming-safe version...\n");
  
  // Redirect to the improved streaming-safe version
  showGameImageScreenCorrected(coreName, gameName);
  
  Serial.printf("=== REDIRECT COMPLETE ===\n");
}


// Fixed findCoreImage function - this was missing!
bool findCoreImage(String coreName, String &imagePath) {
  Serial.printf("=== ENHANCED CORE IMAGE FINDER ===\n");
  Serial.printf("Core: '%s'\n", coreName.c_str());
  Serial.printf("lastArcadeSystemeId: '%s' (length: %d)\n", lastArcadeSystemeId.c_str(), lastArcadeSystemeId.length());
  
  // Enhanced logic for Arcade: prioritize subsystem image if available and confirmed
  String coreNameLower = coreName;
  coreNameLower.toLowerCase();
  if (coreNameLower == "arcade" && lastArcadeSystemeId.length() > 0) {
    // Try subsystem-specific image first
    String subsystemCoreName = "Arcade_" + lastArcadeSystemeId;
    String safeSubsystemName = sanitizeCoreFilename(subsystemCoreName);
    String subsystemPath;
    
    if (ENABLE_ALPHABETICAL_FOLDERS) {
      String alphabetPath = getAlphabeticalPath(subsystemCoreName);
      subsystemPath = alphabetPath + "/" + safeSubsystemName + ".jpg";
    } else {
      subsystemPath = String(CORE_IMAGES_PATH) + "/" + safeSubsystemName + ".jpg";
    }
    
    Serial.printf("Checking subsystem-specific image: %s\n", subsystemPath.c_str());
    
    if (SD.exists(subsystemPath)) {
      imagePath = subsystemPath;
      Serial.printf("Found subsystem-specific image: %s\n", imagePath.c_str());
      return true;
    } else {
      Serial.printf("Subsystem-specific image not found: %s\n", subsystemPath.c_str());
      
      // Emergency download: lastArcadeSystemeId present means ScreenScraper
      // already validated this subsystem — attempt download immediately.
      if (ENABLE_AUTO_DOWNLOAD && WiFi.status() == WL_CONNECTED && !downloadInProgress) {
        Serial.printf("Attempting emergency download of subsystem image...\n");
        
        // Force download of subsystem image
        if (downloadCoreImageFromScreenScraper("arcade", true)) {
          Serial.printf("Emergency download successful!\n");
          
          // Check if it exists now
          if (SD.exists(subsystemPath)) {
            imagePath = subsystemPath;
            Serial.printf("Now using downloaded subsystem image: %s\n", imagePath.c_str());
            return true;
          }
        } else {
          Serial.printf("Emergency download failed\n");
        }
      }   // if (ENABLE_AUTO_DOWNLOAD)
    }     // else (SD.exists)
  }       // if (coreNameLower == "arcade")
  
  // Search for generic image (original logic)
  // Sanitize coreName so '/' in friendly names like "Nintendo NES/Famicom"
  // does not turn into a phantom subdirectory.
  String safeCoreName = sanitizeCoreFilename(coreName);
  if (ENABLE_ALPHABETICAL_FOLDERS) {
    String alphabetPath = getAlphabeticalPath(safeCoreName);
    imagePath = alphabetPath + "/" + safeCoreName + ".jpg";
  } else {
    imagePath = String(CORE_IMAGES_PATH) + "/" + safeCoreName + ".jpg";
  }
  
  Serial.printf("Checking generic image: %s\n", imagePath.c_str());
  
  if (SD.exists(imagePath)) {
    Serial.printf("Found generic image: %s\n", imagePath.c_str());
    return true;
  }
  
  Serial.printf("No image found for core: %s\n", coreName.c_str());
  return false;
}

String getAlphabeticalPath(String coreName) {
  if (coreName.length() == 0) return String(CORE_IMAGES_PATH) + "/A";
  
  char firstChar = coreName.charAt(0);
  
  // If starts with number (0-9), use "#" folder
  if (firstChar >= '0' && firstChar <= '9') {
    return String(CORE_IMAGES_PATH) + "/#";
  }
  // If starts with lowercase letter, convert to uppercase
  else if (firstChar >= 'a' && firstChar <= 'z') {
    return String(CORE_IMAGES_PATH) + "/" + String((char)(firstChar - 32));
  }
  // If starts with uppercase letter, use as is
  else if (firstChar >= 'A' && firstChar <= 'Z') {
    return String(CORE_IMAGES_PATH) + "/" + String(firstChar);
  }
  // For other characters, use "#" folder
  else {
    return String(CORE_IMAGES_PATH) + "/#";
  }
}

// Callback for JPEG decoder
int jpegDrawCallback(JPEGDRAW *pDraw) {
  // Debug: log first call to verify callback is being invoked
  static bool firstCall = true;
  if (firstCall) {
    Serial.printf("jpegDrawCallback: First block at (%d,%d) size %dx%d\n", 
                  pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
    Serial.printf("  Applying global offset: (%d,%d)\n", g_jpegOffsetX, g_jpegOffsetY);
    firstCall = false;
  }
  
  // Apply global offset for centering (JPEGDEC doesn't accept large offsets in decode())
  int finalX = pDraw->x + g_jpegOffsetX;
  int finalY = pDraw->y + g_jpegOffsetY;
  
  // Verify data is within screen bounds and doesn't overlap footer
  if (finalX >= 0 && finalY >= 0 && 
      finalX + pDraw->iWidth <= TARGET_WIDTH && 
      finalY + pDraw->iHeight <= IMAGE_AREA_HEIGHT) {
    // Use M5.Display directly (not Lcd) for image rendering
    M5.Display.pushImage(finalX, finalY, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  } else {
    // Log if trying to draw outside allowed area
    Serial.printf("jpegDrawCallback: Block outside bounds at (%d,%d) size %dx%d\n",
                  finalX, finalY, pDraw->iWidth, pDraw->iHeight);
    if (finalY + pDraw->iHeight > IMAGE_AREA_HEIGHT) {
      Serial.printf("  Would overlap footer (footer at Y=%d)\n", IMAGE_AREA_HEIGHT);
    }
  }
  
  return 1; // Continue decoding
}

// Helper function to reset callback state for new image
void resetJpegCallback() {
  // Can't directly access firstCall, so we'll set a flag
  // Actually, firstCall is static inside the function, we can't reset it
  // This is fine - first block logging happens once per session
}

bool displayCoreImage(String imagePath) {
  // Use displayCoreImageCentered which now implements callback-based centering
  // (JPEGDEC doesn't accept large offsets in decode(), so we apply them in the callback)
  return displayCoreImageCentered(imagePath);
}

void showCoreImageScreen(String coreName) {
  // backgroundLoaded = false;
  String imagePath;
  
  Serial.printf("\n=== CORE SCREEN: %s ===\n", coreName.c_str());
  
  if (!sdCardAvailable) {
    Serial.println("SD not available, showing SD error");
    showSDCardError();
    return;
  }
  
  if (coreName.length() == 0 || coreName == "NO SERVER" || coreName == "TIMEOUT" || coreName.startsWith("ERROR")) {
    Serial.printf("Special state detected: '%s' - showing menu with overlay\n", coreName.c_str());
    showMenuImageWithCoreOverlay(coreName);
    return;
  }
  
  Serial.printf("Trying to find image for: '%s'\n", coreName.c_str());
  
  if (findCoreImage(coreName, imagePath)) {
    Serial.printf("Image found: %s\n", imagePath.c_str());
    
    // Show core image
    if (displayCoreImage(imagePath)) {
      Serial.println("Image displayed correctly, adding footer");
      
      // Only add footer with instructions (no header or top overlay)
      drawCoreImageFooter();
      
      Serial.println("Footer added successfully");
      return;
    } else {
      Serial.println("Error displaying image, fallback to menu image");
    }
  } else {
    Serial.println("No image found for core, showing menu image with overlay");
  }
  
  showMenuImageWithCoreOverlay(coreName);
}

void showMenuImageWithCoreOverlay(String coreName) {
  // backgroundLoaded = false;
  String menuImagePath;
  bool menuImageFound = false;
  
  Serial.printf("Showing menu image with core overlay for: '%s'\n", coreName.c_str());
  
  // ========== SPECIAL CASE: MENU STATE - EARLY DETECTION ==========
  if (coreName.equalsIgnoreCase("MENU")) {
    Serial.println("MENU state detected - showing simple menu interface");
    
    // Try to find and display menu image
    String menuPaths[] = {
      "/cores/menu.jpg",
      "/cores/MENU.jpg", 
      "/cores/Menu.jpg",
      "/cores/main.jpg",
      "/cores/MAIN.jpg"
    };
    
    // Find menu image
    for (String path : menuPaths) {
      if (SD.exists(path)) {
        File testFile = SD.open(path);
        if (testFile && testFile.size() > 0) {
          menuImagePath = path;
          testFile.close();
          menuImageFound = true;
          Serial.printf("Menu image found: %s\n", path.c_str());
          break;
        }
        if (testFile) testFile.close();
      }
    }
    
    // Display menu image or fallback
    if (menuImageFound && displayCoreImage(menuImagePath)) {
      Serial.println("Menu image displayed successfully");
    } else {
      // Fallback: show basic menu screen
      Lcd.fillScreen(THEME_BLACK);
      Lcd.setTextColor(THEME_CYAN);
      Lcd.setTextSize(2);
      Lcd.setCursor(10, 10);
      Lcd.print("MiSTer Monitor");
      
      Lcd.setTextColor(THEME_WHITE);
      Lcd.setTextSize(1);
      Lcd.setCursor(10, 50);
      Lcd.print("Menu Mode");
    }
    
    // Footer for the pure MENU state (no game, no core overlay).
    // Single line: just the hint, centered, with comfortable margins.
    M5.Display.drawFastHLine(0, 620, 1280, THEME_GREEN);
    M5.Display.fillRect(0, 621, 1280, 99, THEME_BLACK);
    
    M5.Display.setTextWrap(false);
    M5.Display.setTextColor(THEME_GREEN);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(250, 660);
    M5.Display.print("Touch the screen to show MiSTer monitor");
    
    Serial.println("Menu interface displayed without active core overlay");
    return; // IMPORTANT: Exit here to avoid executing core overlay logic
  }
  
  // ========== MENU IMAGE SEARCH ==========
  // Try to find menu image for core overlay cases
  String menuPaths[] = {
    "/cores/menu.jpg",
    "/cores/MENU.jpg", 
    "/cores/Menu.jpg",
    "/cores/main.jpg",
    "/cores/MAIN.jpg"
  };
  
  for (String path : menuPaths) {
    if (SD.exists(path)) {
      File testFile = SD.open(path);
      if (testFile && testFile.size() > 0) {
        menuImagePath = path;
        testFile.close();
        menuImageFound = true;
        Serial.printf("Menu image found: %s\n", path.c_str());
        break;
      }
      if (testFile) testFile.close();
    }
  }
  
  // ========== IMPROVED IMAGE DISPLAY AND OVERLAY ==========
  if (menuImageFound && displayCoreImage(menuImagePath)) {
    Serial.println("Menu image displayed, adding enhanced overlay");
    
    // NEW INTEGRATED LOGIC: Determine overlay type based on multiple sources
    bool isLocalErrorState = (coreName == "NO SERVER" || coreName == "TIMEOUT" || coreName.startsWith("ERROR"));
    bool shouldShowError = serverHasError || isLocalErrorState;
    
    if (shouldShowError) {
      // ========== INTEGRATED ERROR OVERLAY ==========
      Serial.println("Showing integrated error overlay");
      
      String displayError = "";
      if (serverHasError && serverErrorType.length() > 0) {
        displayError = serverErrorType;
      } else if (isLocalErrorState) {
        displayError = coreName;
      } else {
        displayError = "ERROR";
      }
      
      M5.Display.fillRect(1100, 20, 160, 40, THEME_BLACK);
      M5.Display.drawRect(1100, 20, 160, 40, THEME_RED);
      M5.Display.setTextColor(THEME_RED);
      M5.Display.setTextSize(2);
      M5.Display.setCursor(1115, 32);
      
      if (displayError == "NO SERVER") {
        M5.Display.print("NO SERVER");
      } else if (displayError == "TIMEOUT") {
        M5.Display.print("TIMEOUT");
      } else if (displayError == "OFFLINE") {
        M5.Display.print("OFFLINE");
      } else if (displayError == "DISCONNECTED") {
        M5.Display.print("DISCONN.");
      } else if (displayError.startsWith("ERROR")) {
        M5.Display.print("ERROR");
      } else {
        M5.Display.print(displayError.substring(0, 10));
      }
      
      Serial.printf("Error overlay displayed: %s\n", displayError.c_str());
    }
    
  } else {
    // ========== FALLBACK IF THERE IS NO MENU IMAGE ==========
    Serial.println("No menu image found, showing main HUD");
    displayMainHUD();
  }

  // ========== FOOTER FOR NON-MENU CORES (with optional GAME line) ==========
  // Layout (footer band y=621..720, height 99):
  //   Line 1 (y=628): "ACTIVE CORE: <core>"  size 2 label + size 3 value
  //   Line 2 (y=668): "GAME: <name>"          size 2 label + size 3 value (only if game)
  //   Hint:    bottom-right, size 2, y=695
  M5.Display.drawFastHLine(0, 620, 1280, THEME_GREEN);
  M5.Display.fillRect(0, 621, 1280, 99, THEME_BLACK);
  
  M5.Display.setTextWrap(false);
  bool hasGame    = (currentGame.length() > 0);
  const bool showInfoButton = hasGame && gameInfoAvailable();
  
  // === Line 1: ACTIVE CORE ===
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(20, 632);   // size 2 label, slightly above the size-3 value baseline
  M5.Display.print("ACTIVE CORE:");
  
  M5.Display.setTextColor(THEME_YELLOW);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(245, 628);
  String footerCore = coreName.length() > 30 ? coreName.substring(0, 30) : coreName;
  if (footerCore.equalsIgnoreCase("arcade")) footerCore = "Arcade";
  M5.Display.print(footerCore);
  
  // === Line 2: GAME (only when a game is loaded) ===
  if (hasGame) {
    // Game value sits at x=120, size 3 (18 px/char).
    //   With the button:    47 chars end at x=966, 74 px of air before it
    //                       (x>=1040); the hint keeps its own row below.
    //   Without the button: the hint moves up onto this row, right-aligned,
    //                       so the name yields to 40 chars (120..840).
    int visibleChars = showInfoButton ? 47 : 40;
    if (imageFooterScroll.fullText != currentGame ||
        imageFooterScroll.maxChars != visibleChars) {
      initScrollText(&imageFooterScroll, currentGame, visibleChars);
    }
    String displayGame = getScrolledText(&imageFooterScroll);
    while ((int)displayGame.length() < imageFooterScroll.maxChars) {
    displayGame += ' ';
    }
    
    M5.Display.setTextColor(THEME_CYAN);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(20, 672);
    M5.Display.print("GAME:");
    
     M5.Display.setTextColor(THEME_YELLOW, THEME_BLACK);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(120, 668);
    M5.Display.print(displayGame);
  }
  
  // === Hint =================================================================
  // With the GAME INFO button: unchanged — size 2 on its own row at y=695,
  // shifted left to x=600 so it clears the button.
  // Without it: the row at y=668 has room, so the hint sits there at size 3,
  // right-aligned (21 chars x 18 px = 378 px, 20 px margin -> x=882). With no
  // game at all that row is empty, so it is centred instead (x=451).
    M5.Display.setTextColor(THEME_CYAN);
    if (showInfoButton) {
      M5.Display.setTextSize(2);
      M5.Display.setCursor(600, 695);
    } else {
      M5.Display.setTextSize(3);
      M5.Display.setCursor(hasGame ? 882 : 451, 668);
    }
    M5.Display.print("Touch to show monitor");

  if (showInfoButton) drawGameInfoIcon();
}

void showCoreNotFoundScreen(String coreName) {
  /**
   * Fallback screen when no menu image is available
   * Shows basic information and error status
   */
  Lcd.fillScreen(THEME_BLACK);
  
  // Header
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(2);
  Lcd.setCursor(10, 10);
  Lcd.print("MiSTer Monitor");
  
  // Core info
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(10, 50);
  Lcd.print("Current Core:");
  
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(2);
  Lcd.setCursor(10, 70);
  String displayCore = coreName.length() > 20 ? 
                      coreName.substring(0, 20) + "..." : coreName;
  String coreDisplay = displayCore;
    if (coreDisplay.equalsIgnoreCase("arcade")) {
      coreDisplay = "Arcade";
    }
    Lcd.print(coreDisplay);
  
  // Error indicator if needed
  bool shouldShowError = serverHasError || isErrorCore(coreName);
  if (shouldShowError) {
    Lcd.setTextColor(THEME_RED);
    Lcd.setTextSize(1);
    Lcd.setCursor(10, 110);
    Lcd.print("System Status: ERROR");
    
    // Show error in absolute top-right corner
    M5.Display.fillRect(1100, 20, 160, 40, THEME_BLACK);
    M5.Display.drawRect(1100, 20, 160, 40, THEME_RED);
    M5.Display.setTextColor(THEME_RED);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(1115, 32);
    M5.Display.print("ERROR");
  } else {
    Lcd.setTextColor(THEME_GREEN);
    Lcd.setTextSize(1);
    Lcd.setCursor(10, 110);
    Lcd.print("System Status: ACTIVE");
  }
  
  // Instructions
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1);
  Lcd.setCursor(10, 150);
  Lcd.print("No menu image found");
  Lcd.setCursor(10, 165);
  Lcd.print("Check SD card: /cores/menu.jpg");
  
  // Footer in PHYSICAL coordinates
  M5.Display.fillRect(0, 621, 1280, 99, THEME_BLACK);
  M5.Display.drawFastHLine(0, 620, 1280, THEME_GREEN);
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(350, 660);
  M5.Display.print("Press any button for interface");
  }

/**
 * Load and display full screen frame image (frame01.jpg)
 * This serves as the base frame for the entire interface
 * Returns true if successful
 */
bool loadFullScreenFrame(const char* framePath) {
  if (!sdCardAvailable || !SD.exists(framePath)) {
    Serial.printf("Frame image not found: %s\n", framePath);
    return false;
  }
  
  File frameFile = SD.open(framePath);
  if (!frameFile) {
    Serial.println("Failed to open frame file");
    return false;
  }
  
  size_t fileSize = frameFile.size();
  Serial.printf("Frame image size: %d bytes\n", fileSize);
  
  if (fileSize == 0 || fileSize > 500000) {
    Serial.println("Invalid frame file size");
    frameFile.close();
    return false;
  }
  
  // Allocate buffer for frame image (PSRAM-aware to preserve internal heap)
  uint8_t* buffer = (uint8_t*)psramMalloc(fileSize);
  if (!buffer) {
    Serial.println("No memory for frame image");
    frameFile.close();
    return false;
  }
  
  size_t bytesRead = frameFile.read(buffer, fileSize);
  frameFile.close();
  
  if (bytesRead != fileSize) {
    Serial.println("Error reading frame file");
    free(buffer);
    return false;
  }
  
  // Verify JPEG signature
  if (buffer[0] != 0xFF || buffer[1] != 0xD8) {
    Serial.println("Frame file is not a valid JPEG");
    free(buffer);
    return false;
  }
  
  // Callback for full screen frame - no offset, just raw display
  auto frameCallback = [](JPEGDRAW *pDraw) -> int {
    M5.Display.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
    return 1;
  };
  
  // Decode frame image at position (0,0) - full screen
  if (jpeg.openRAM(buffer, fileSize, frameCallback)) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    bool success = jpeg.decode(0, 0, 0);  // Display at origin (0,0)
    jpeg.close();
    free(buffer);
    
    if (success) {
      Serial.println("Full screen frame loaded successfully");
      return true;
    } else {
      Serial.println("Error decoding frame image");
      return false;
    }
  }
  
  free(buffer);
  Serial.println("Failed to open frame JPEG decoder");
  return false;
}

/**
 * Load and display MiSTer logo from SD card
 * Returns true if successful
 */
bool loadMisterLogo(int x, int y) {
  String logoPath = "/cores/logo_mister.jpg";
  
  if (!sdCardAvailable || !SD.exists(logoPath)) {
    Serial.printf("Logo not found: %s\n", logoPath.c_str());
    return false;
  }
  
  File logoFile = SD.open(logoPath);
  if (!logoFile) {
    Serial.println("Failed to open logo file");
    return false;
  }
  
  size_t fileSize = logoFile.size();
  if (fileSize == 0 || fileSize > 100000) {
    Serial.println("Invalid logo file size");
    logoFile.close();
    return false;
  }
  
  uint8_t* buffer = (uint8_t*)psramMalloc(fileSize);
  if (!buffer) {
    Serial.println("No memory for logo");
    logoFile.close();
    return false;
  }
  
  logoFile.read(buffer, fileSize);
  logoFile.close();
  
  // Verify JPEG signature
  if (buffer[0] != 0xFF || buffer[1] != 0xD8) {
    Serial.println("Logo must be JPG format");
    free(buffer);
    return false;
  }
  
  // Simple callback for logo
  auto logoCallback = [](JPEGDRAW *pDraw) -> int {
    M5.Display.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
    return 1;
  };
  
  if (jpeg.openRAM(buffer, fileSize, logoCallback)) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    bool success = jpeg.decode(x, y, 0);
    jpeg.close();
    free(buffer);
    return success;
  }
  
  free(buffer);
  return false;
}

/**
 * Draw MiSTer logo in the right panel, above the buttons
 * Optimized position for frame02.jpg layout
 * Returns true if logo was loaded from SD, false if text fallback was used
 */
void drawMisterLogoRightPanel() {
  // Draw MiSTer logo in the correct position when frame02.jpg is shown
  // Logo positioned at (75, 100) - top left corner
  String logoPath = "/cores/logo_mister.jpg";
  
  if (SD.exists(logoPath)) {
    File logoFile = SD.open(logoPath);
    if (logoFile) {
      size_t fileSize = logoFile.size();
      uint8_t *buffer = (uint8_t*)psramMalloc(fileSize);
      
      if (buffer) {
        size_t bytesRead = logoFile.read(buffer, fileSize);
        logoFile.close();
        
        if (bytesRead == fileSize) {
          if (jpeg.openRAM(buffer, fileSize, jpegDrawCallback)) {
            int imgW = jpeg.getWidth();
            int imgH = jpeg.getHeight();
            
            // Position logo at (75, 100)
            int logoX = 75;
            int logoY = 100;
            
            jpeg.setPixelType(RGB565_BIG_ENDIAN);
            jpeg.decode(logoX, logoY, 0);
            jpeg.close();
            
            Serial.printf("MiSTer logo drawn at (%d, %d) size %dx%d\n", logoX, logoY, imgW, imgH);
          }
        }
        free(buffer);
      } else {
        logoFile.close();
      }
    }
  } else {
    Serial.println("Warning: logo_mister.jpg not found at /cores/logo_mister.jpg");
  }
}

/**
 * Play button feedback sounds
 * Each button has a distinct tone for better UX
 */

// PREV button sound - lower pitch (800 Hz)
void playPrevButtonSound() {
  M5.Speaker.tone(800, 80);
  Serial.println("PREV sound");
}

void playScanButtonSound() {
  M5.Speaker.tone(1200, 80);
  Serial.println("SCAN sound");
}

void playNextButtonSound() {
  M5.Speaker.tone(1600, 80);
  Serial.println("NEXT sound");
}

/**
 * Generic button press feedback
 * Provides visual and audio feedback when a button is pressed
 * Changes text to white, plays sound, then restores original color
 */
void buttonPressFeedback(TouchButton* btn, void (*soundFunction)()) {
  // Visual feedback: change the label color to white briefly.
  btn->draw(THEME_WHITE);
  
  // Audio feedback
  soundFunction();
  
  // Hold long enough for the user to register the press
  delay(200);
  
  // Restore original label color
  btn->draw(THEME_CYAN);
}

/**
 * Draw frame - now just loads frame01.jpg
 * If frame image fails, falls back to drawing frame programmatically
 * Uses bootFrameLoaded flag to avoid reloading
 */
void drawCyberpunkFrame() {
  // If frame already loaded, skip loading
  if (bootFrameLoaded) {
    Serial.println("Boot frame already loaded, skipping reload");
    return;
  }
  // Try to load full screen frame image first
  bool frameLoaded = loadFullScreenFrame("/cores/frame01.jpg");
  
  if (frameLoaded) {
    Serial.println("Using frame01.jpg as frame");
    bootFrameLoaded = true;  // Mark as loaded
    return;  // Frame loaded successfully, nothing else needed
  }
  
  // FALLBACK: If frame image not found, draw frame programmatically
  Serial.println("Frame image not found, drawing programmatic fallback");
  
  // Clear screen with black background
  M5.Display.fillScreen(THEME_BLACK);
  
  // Draw simple frame as fallback
  int cornerCut = 30;
  int frameThick = 3;
  int margin = 5;
  
  int left = margin;
  int right = 1280 - margin;
  int top = margin;
  int bottom = 720 - margin;
  
  // Main frame outline
  for (int t = 0; t < frameThick; t++) {
    // Top edge
    M5.Display.drawLine(left + cornerCut, top + t, right - cornerCut, top + t, THEME_CYAN);
    // Bottom edge
    M5.Display.drawLine(left + cornerCut, bottom - t, right - cornerCut, bottom - t, THEME_CYAN);
    // Left edge
    M5.Display.drawLine(left + t, top + cornerCut, left + t, bottom - cornerCut, THEME_CYAN);
    // Right edge
    M5.Display.drawLine(right - t, top + cornerCut, right - t, bottom - cornerCut, THEME_CYAN);
  }
  
  // Corner diagonal cuts
  for (int t = 0; t < frameThick; t++) {
    M5.Display.drawLine(left + t, top + cornerCut, left + cornerCut, top + t, THEME_CYAN);
    M5.Display.drawLine(right - cornerCut, top + t, right - t, top + cornerCut, THEME_CYAN);
    M5.Display.drawLine(left + t, bottom - cornerCut, left + cornerCut, bottom - t, THEME_CYAN);
    M5.Display.drawLine(right - cornerCut, bottom - t, right - t, bottom - cornerCut, THEME_CYAN);
  }
  
  // Center divider line
  int dividerX = 640;
  for (int t = 0; t < 2; t++) {
    M5.Display.drawFastVLine(dividerX + t, top + 50, bottom - top - 100, THEME_CYAN);
  }
  bootFrameLoaded = true;  // Mark as loaded even if using fallback
}

/**
 * Draw progress squares (5 squares that turn from blue to green)
 * Position: (470, 170) - repositioned for new layout
 */
void drawProgressSquares(int completedCount) {
  int startX = 790;
  int startY = 320;
  int squareSize = 60;
  int spacing = 20;
  
  for (int i = 0; i < 5; i++) {
    int x = startX + i * (squareSize + spacing);
    uint16_t color = (i < completedCount) ? THEME_GREEN : THEME_BLUE;
    
    // Filled square
    M5.Display.fillRect(x, startY, squareSize, squareSize, color);
    
    // Border
    M5.Display.drawRect(x - 2, startY - 2, squareSize + 4, squareSize + 4, THEME_CYAN);
    
    // Inner detail
    if (i < completedCount) {
      // Checkmark pattern for completed
      M5.Display.drawLine(x + 15, startY + 30, x + 25, startY + 40, THEME_WHITE);
      M5.Display.drawLine(x + 25, startY + 40, x + 45, startY + 15, THEME_WHITE);
    }
  }
}

void showBootSequence() {
  // Clear screen completely
  M5.Display.fillScreen(THEME_BLACK);
  
  // ========== LOAD FULL SCREEN FRAME IMAGE ==========
  // This replaces all the manual frame drawing
  drawCyberpunkFrame();  // Now loads frame01.jpg
  
  // Small delay to ensure frame is rendered
  delay(100);
  
  // ========== LOAD AND OVERLAY MISTER LOGO ==========
  // Logo dimensions: 350x140 pixels
  // Position: centered in right panel (785, 150)
  bool logoLoaded = loadMisterLogo(785, 150);
  
  if (!logoLoaded) {
    // Fallback: draw text-based logo
    M5.Display.setTextColor(THEME_WHITE);
    M5.Display.setTextSize(4);
    M5.Display.setCursor(850, 180);
    M5.Display.print("MiSTer");
    M5.Display.setTextColor(THEME_YELLOW);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(870, 230);
    M5.Display.print("FPGA");
    
    Serial.println("Logo not loaded - using text fallback");
  }
  
  // ========== DRAW PROGRESS SQUARES (initially blue) ==========
  drawProgressSquares(0);
  
  // ========== BOOT SEQUENCE CONTENT (left panel 640x480) ==========
  // Content area starts at physical coordinates (20, 140) with 2x scaling
  
  int bootOffsetX = 110;
  int bootOffsetY = 140;
  int scale = 2;
  
  // Terminal-style boot header (scaled 2x)
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(4);
  M5.Display.setCursor(bootOffsetX + 80*scale, bootOffsetY + 20*scale);
  M5.Display.print("SYSTEM");
  
  M5.Display.setTextColor(THEME_YELLOW);
  M5.Display.setCursor(bootOffsetX + 40*scale, bootOffsetY + 45*scale);
  M5.Display.print("INITIALIZE");
  
  // Boot lines with typing effect
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(2);
  
  String bootLines[] = {
    "> Loading MiSTer interface...",
    "> Initializing SD card system...",
    "> Loading core image decoder...",
    "> Establishing connection protocols...",
    "> System ready - ONLINE"
  };
  
  for (int i = 0; i < 5; i++) {
    int lineY = bootOffsetY + 90*scale + i * 15*scale;
    M5.Display.setCursor(bootOffsetX, lineY);
    
    // Typing effect
    for (int j = 0; j < bootLines[i].length(); j++) {
      M5.Display.print(bootLines[i].charAt(j));
      delay(20);
    }
    
    // Update progress squares as each line completes
    drawProgressSquares(i + 1);
    delay(100);
  }
  
  // Progress bar at bottom of boot area
  int barX = bootOffsetX + 20*scale;
  int barY = bootOffsetY + 170*scale;
  int barW = 240*scale;
  int barH = 12*scale;
  
  M5.Display.drawRect(barX, barY, barW, barH, THEME_WHITE);
  M5.Display.fillRect(barX + scale, barY + scale, barW - 2*scale, barH - 2*scale, THEME_BLACK);
  
  // Animated progress fill
  for (int progress = 0; progress <= 100; progress += 2) {
    int fillWidth = ((barW - 4*scale) * progress) / 100;
    uint16_t barColor = (progress < 30) ? THEME_YELLOW :
                       (progress < 70) ? THEME_CYAN : THEME_GREEN;
    
    M5.Display.fillRect(barX + 2*scale, barY + 2*scale, fillWidth, barH - 4*scale, barColor);
    M5.Display.drawFastHLine(barX + 2*scale, barY + 2*scale, fillWidth, THEME_WHITE);
    
    delay(15);
  }
  
  delay(1000);
}

/**
 * Draw WiFi connection progress circles
 * Shows blue circles for attempts, turns all green on success
 * Position: right panel, where progress squares were in boot sequence
 * @param currentAttempt - Current connection attempt (1-30)
 * @param connected - true when connection successful
 * @param maxAttempts - Maximum attempts to show (default 30)
 */
void drawWiFiProgressCircles(int currentAttempt, bool connected, int maxAttempts = 30) {
  int startX = 670;      // Start position X (right panel)
  int startY = 320;      // Start position Y (same as progress squares)
  int circleRadius = 12; // Radius of each circle
  int spacing = 15;      // Space between circles
  int circlesPerRow = 10; // 10 circles per row
  
  // Calculate how many circles to draw
  int circlesToDraw = min(currentAttempt, maxAttempts);
  // Clear progress squares area in right panel
  M5.Display.fillRect(780, 310, 400, 80, THEME_BLACK);
  // Draw circles in rows of 10
  for (int i = 0; i < circlesToDraw; i++) {
    int row = i / circlesPerRow;
    int col = i % circlesPerRow;
    
    int x = startX + col * (circleRadius * 2 + spacing);
    int y = startY + row * (circleRadius * 2 + spacing);
    
    // Choose color based on connection status
    uint16_t fillColor = connected ? THEME_GREEN : THEME_BLUE;
    uint16_t borderColor = THEME_CYAN;
    
    // Draw filled circle
    M5.Display.fillCircle(x, y, circleRadius, fillColor);
    
    // Draw border
    M5.Display.drawCircle(x, y, circleRadius + 1, borderColor);
    
    // Add inner highlight for 3D effect
    if (connected) {
      M5.Display.fillCircle(x - 3, y - 3, 3, THEME_WHITE);
    }
  }
  
  // Draw attempt counter text below circles
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(startX, startY + 100);
  
  if (connected) {
    M5.Display.setTextColor(THEME_GREEN);
    M5.Display.setTextSize(3);
    M5.Display.print("CONNECTED");
  } else {
    M5.Display.printf("ATTEMPT %02d/%02d", currentAttempt, maxAttempts);
  }
}

void connectWithAnimation() {
  // ========== LEFT PANEL: CONNECTION ANIMATION AREA ==========
  // Position: (110, 140) with 2x scaling (same as boot sequence)
  int animOffsetX = 110;
  int animOffsetY = 140;
  int scale = 2;
  
  M5.Display.fillRect(90, 180, 640, 410, THEME_BLACK);  // Clear left panel only
  
  drawCyberpunkFrame();  // This will skip if already loaded
  
  delay(100);
  
  // ========== LOAD MISTER LOGO ==========
  bool logoLoaded = loadMisterLogo(785, 150);
  if (!logoLoaded) {
    // Fallback text logo
    M5.Display.setTextColor(THEME_WHITE);
    M5.Display.setTextSize(4);
    M5.Display.setCursor(850, 180);
    M5.Display.print("MiSTer");
    M5.Display.setTextColor(THEME_YELLOW);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(870, 230);
    M5.Display.print("FPGA");
  }
  
  // Draw header in left panel
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(4); // 2x original size
  M5.Display.setCursor(animOffsetX + 60*scale, animOffsetY + 15*scale);
  M5.Display.print("CONNECTING");
  
  // ========== RIGHT PANEL: INITIAL STATE ==========
  // Draw initial empty state (no circles yet)
  drawWiFiProgressCircles(0, false, 30);
  
  // WiFi scan diagnostic
  Serial.println("=== SCANNING NETWORKS ===");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    Serial.printf("  [%d] SSID: %s  RSSI: %d  Auth: %d\n",
      i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i));
  }
  Serial.println("=== END SCAN ===");

  Serial.println("=== STARTING WiFi CONNECTION ===");
  Serial.printf("SSID: %s\n", ssid);
  Serial.printf("MiSTer IP configured: %s\n", misterIP);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  int maxAttempts = 30;
  
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    attempts++;
    
    // ========== LEFT PANEL: RADAR ANIMATION ==========
    // Clear animation area (but keep header)
    M5.Display.fillRect(animOffsetX, animOffsetY + 60*scale, 
                        520, 120*scale, THEME_BLACK);
    
    // Draw radar scan animation (centered in left panel)
    int radarCenterX = animOffsetX + 80*scale;  // Center of radar
    int radarCenterY = animOffsetY + 120*scale; // Vertical center
    int radarRadius = 60*scale;                 // Scaled radius
    int angle = attempts * 12;                  // Rotating angle
    
    // Draw radar circles
    for (int r = 20*scale; r <= radarRadius; r += 20*scale) {
      M5.Display.drawCircle(radarCenterX, radarCenterY, r, THEME_GREEN);
    }
    
    // Draw scanning line
    float radAngle = angle * PI / 180.0;
    int endX = radarCenterX + radarRadius * cos(radAngle);
    int endY = radarCenterY + radarRadius * sin(radAngle);
    M5.Display.drawLine(radarCenterX, radarCenterY, endX, endY, THEME_YELLOW);
    
    // Draw radar crosshair
    M5.Display.drawFastHLine(radarCenterX - 10*scale, radarCenterY, 20*scale, THEME_CYAN);
    M5.Display.drawFastVLine(radarCenterX, radarCenterY - 10*scale, 20*scale, THEME_CYAN);
    
    // ========== RIGHT PANEL: UPDATE PROGRESS CIRCLES ==========
    // Clear previous circles area
    M5.Display.fillRect(670, 320, 570, 120, THEME_BLACK);
    
    // Draw updated circles with current attempt count
    drawWiFiProgressCircles(attempts, false, maxAttempts);
    
    Serial.printf("WiFi attempt %d/%d...\n", attempts, maxAttempts);
    delay(500);
  }
  
  // ========== CONNECTION RESULT ==========
  // Clear left panel animation area
  //M5.Display.fillRect(animOffsetX, animOffsetY + 60*scale, 
  //                    520, 140*scale, THEME_BLACK);
  
  if (WiFi.status() == WL_CONNECTED) {
    // ========== SUCCESS STATE ==========
    
    // Left panel: Success message
    // M5.Display.setTextColor(THEME_GREEN);
    // M5.Display.setTextSize(4);
    // M5.Display.setCursor(animOffsetX + 40*scale, animOffsetY + 95*scale);
    // M5.Display.print("CONNECTED");
    
    M5.Display.setTextColor(THEME_CYAN);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 230*scale);
    M5.Display.printf("IP: %s", WiFi.localIP().toString().c_str());
    
    Serial.printf("WiFi connected successfully!\n");
    Serial.printf("Assigned IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    
    // Right panel: Turn all circles GREEN
    M5.Display.fillRect(670, 320, 570, 120, THEME_BLACK);
    drawWiFiProgressCircles(attempts, true, maxAttempts);
    
    // Test MiSTer connectivity
    delay(1000);
    
    // Clear left panel for MiSTer test
    M5.Display.fillRect(animOffsetX, animOffsetY + 60*scale, 
                        520, 140*scale, THEME_BLACK);
    
    M5.Display.setTextColor(THEME_YELLOW);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(animOffsetX + 45*scale, animOffsetY + 95*scale);
    M5.Display.print("Testing MiSTer...");
    
    // Auto-discover the MiSTer server IP.
    // On success overwrites misterIP; on failure leaves the config.ini value unchanged.
    Serial.println("=== DISCOVERING MiSTer SERVER ===");
    bool discovered = discoverMister();

    Serial.println("=== TESTING MiSTer CONNECTIVITY ===");
    testMiSTerConnectivity(discovered);
    
  } else {
    // ========== FAILURE STATE ==========
    
    // Left panel: Error message
    M5.Display.setTextColor(THEME_RED);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(animOffsetX + 35*scale, animOffsetY + 95*scale);
    M5.Display.print("WIFI FAILED");
    
    Serial.printf("Error connecting WiFi!\n");
    
    // Right panel: Show failure with red circles
    M5.Display.fillRect(670, 320, 580, 120, THEME_BLACK);
    
    // Draw all attempted circles in RED
    for (int i = 0; i < attempts; i++) {
      int row = i / 10;
      int col = i % 10;
      int x = 670 + col * 39;
      int y = 320 + row * 39;
      M5.Display.fillCircle(x, y, 12, THEME_RED);
      M5.Display.drawCircle(x, y, 13, THEME_YELLOW);
    }
    
    M5.Display.setTextColor(THEME_RED);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(670, 420);
    M5.Display.print("CONNECTION FAILED");
  }
  
  delay(2000);
}

void testMiSTerConnectivity(bool discovered) {
  // No IP to probe (empty default + discovery failed + no ip= in config):
  // skip http://:8081 and tell the user what to do.
  if (strlen(misterIP) == 0) {
    Serial.println("MiSTer IP unknown: discovery failed and no ip= in config.ini");
    int animOffsetX = 110, animOffsetY = 140, scale = 2;
    M5.Display.fillRect(animOffsetX, animOffsetY + 60*scale, 520, 140*scale, THEME_BLACK);
    M5.Display.setTextColor(THEME_RED);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 95*scale);
    M5.Display.print("MiSTer NOT FOUND");
    M5.Display.setTextColor(THEME_YELLOW);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 120*scale);
    M5.Display.print("Set ip= in config.ini");
    connected = false;
    delay(2000);
    return;
  }

  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/core";
  Serial.printf("Testing connectivity to: %s\n", url.c_str());
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();

  int animOffsetX = 110;
  int animOffsetY = 140;
  int scale = 2;
  int safeWidth = 520;

  M5.Display.fillRect(animOffsetX, animOffsetY + 60*scale, safeWidth, 140*scale, THEME_BLACK);

  if (code == 200) {
    Serial.printf("MiSTer responds correctly!\n");
    M5.Display.setTextColor(THEME_GREEN);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 95*scale);
    M5.Display.print("MiSTer: ONLINE");
    M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 115*scale);
    M5.Display.printf("Server: %s:8081", misterIP);
    connected = true;
  } else {
    connected = false;
    Serial.printf("MiSTer not responding (code: %d)\n", code);
    M5.Display.setTextColor(THEME_RED);
    M5.Display.setTextSize(3);
    if (discovered) {
      // Found via UDP but :8081 is silent → the Python server probably isn't running.
      M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 95*scale);
      M5.Display.print("MiSTer FOUND, no reply");
      M5.Display.setTextColor(THEME_YELLOW);
      M5.Display.setTextSize(2);
      M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 120*scale);
      M5.Display.print("Is the script running?");
    } else {
      // Using the config.ini fallback IP and it doesn't answer → likely wrong IP.
      M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 95*scale);
      M5.Display.print("MiSTer: OFFLINE");
      M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 115*scale);
      M5.Display.printf("Check IP: %s", misterIP);
    }
  }

  http.end();
  delay(2000);
}

void showReconnectBanner() {
  // Footer-style success banner across the bottom band (Y=620..720), which sits
  // BELOW the image area — so it can be restored independently without
  // re-decoding the full-screen image. Holds briefly, then repaints the footer.
  const char* msg = "CONNECTED TO MiSTer";
  int tw = (int)strlen(msg) * 18;                 // 18 px/char at size 3
  M5.Display.fillRect(0, 620, 1280, 100, THEME_GREEN);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(THEME_BLACK, THEME_GREEN);
  M5.Display.setCursor((1280 - tw) / 2, 658);     // centered in the band
  M5.Display.print(msg);

  delay(1200);                                    // hold long enough to read

  // Restore the normal footer over the banner (we're over a full-screen image).
  if (currentGame.length() > 0) addGameImageFooter(currentGame);
  else                          drawCoreImageFooter();
}

void updateMiSTerData() {
  Serial.println("=== Updating MiSTer data ===");
  if (!getStateSnapshot()) {   // atomic path (server >= 2.6)
    getCurrentCore();          // legacy fallback (server <= 2.5.x)
    getCurrentGame();
  }
  getSystemData();
  getStorageData();
  getUSBData();
  getNetworkAndSession();
  Serial.println("=== Update complete ===");
}

// Atomic state fetch: ONE request returns core+game from a single server-side
// lock acquisition (/status/snapshot, server >= 2.6). Eliminates the split-brain
// where core and game were read in two separate requests that a core switch
// could land between (the "searching Doom on the C64 core" bug).
// Returns false on any HTTP/parse problem so the caller can fall back to the
// legacy two-request path, whose error handling remains authoritative.
bool getStateSnapshot() {
  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/snapshot";

  http.begin(url);
  http.setTimeout(8000);
  http.addHeader("User-Agent", "MiSTer-Monitor");

  int code = http.GET();
  if (code != 200) {
    http.end();
    Serial.printf("Snapshot unavailable (HTTP %d) - using legacy path\n", code);
    return false;
  }

  String response = http.getString();
  http.end();

  String newCore = extractStringValue(response, "core");
  String newGame = extractStringValue(response, "game");
  newCore.trim();
  newGame.trim();

  if (newCore.length() == 0) {
    // /status/snapshot always carries "core"; an empty value means a
    // malformed body, not a Menu state. Let the legacy path decide.
    Serial.println("Snapshot missing 'core' - using legacy path");
    return false;
  }

  bool coreDidChange = false;   // set below; used by the cross-core rekey
  bool gameDidChange = false;

  // ---- CORE side effects (ported from getCurrentCore's 200-handler) ----
  previousCore = currentCore;
  currentCore  = newCore;

  if (currentCore == "Menu" || currentCore == "MENU") {
    Serial.println("Server reports Menu state - checking debug endpoint");
    checkMisterDebugState();
  }

  if (!isErrorCore(currentCore)) {
    lastValidCore = currentCore;
  }

  if (previousCore != currentCore && previousCore != "") {
    coreChanged = true;
    coreDidChange = true;
    Serial.printf("Core changed: '%s' -> '%s'\n", previousCore.c_str(), currentCore.c_str());
  }

  connected = true;
  Serial.printf("Core: '%s' (snapshot)\n", currentCore.c_str());
  checkServerErrorState();

  // ---- GAME side effects (ported from getCurrentGame's 200-handler) ----
  previousGame = currentGame;
  currentGame  = newGame;

  if (previousGame != currentGame && currentGame.length() > 0) {
    gameChanged = true;
    gameDidChange = true;
    Serial.printf("Game changed: '%s' -> '%s'\n", previousGame.c_str(), currentGame.c_str());
    // core+game come from ONE atomic server read: coherent by construction
    startCrcRecurrentForGame(currentGame, currentCore);
  } else if (currentGame.length() == 0 && previousGame.length() > 0) {
    gameChanged = true;
    gameDidChange = true;
    Serial.println("Game unloaded, returning to core image");
    stopCrcRecurrent();
  }

  // Cross-core launch transient: main-menu launches (MGL/MRA) announce the
  // game 1-2 s BEFORE the new CORENAME lands, so one poll can pair the new
  // game with the OLD core (field evidence: Aero Fighters' SNES CRC searched
  // under the Amiga systemeid, 404 twice, exhausted flag poisoned). When the
  // corrective core commit arrives the game string is unchanged, gameChanged
  // never fires, and every per-game search flag built under the wrong system
  // would survive. Re-key the search to the corrected pairing —
  // startCrcRecurrentForGame resets exhausted/no-media/attempt state.
  if (coreDidChange && !gameDidChange && currentGame.length() > 0) {
    Serial.printf("Core corrected under same game - rekeying search: '%s' on '%s'\n",
                  currentGame.c_str(), currentCore.c_str());
    lastSearchedGame = "";           // a wrong-core success must not block the redo
    startCrcRecurrentForGame(currentGame, currentCore);
  }

  return true;
}

void getCurrentCore() {
  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/core";
  
  Serial.printf("Connecting to: %s\n", url.c_str());
  
  http.begin(url);
  http.setTimeout(8000);
  http.addHeader("User-Agent", "M5Stack-Monitor");
  
  int code = http.GET();
  Serial.printf("HTTP code: %d\n", code);
  
  if (code == 200) {
    String response = http.getString();
    Serial.printf("Raw response: '%s' (length: %d)\n", response.c_str(), response.length());

    previousCore = currentCore;
    currentCore = response;
    currentCore.trim();
    currentCore.replace("\n", "");
    currentCore.replace("\r", "");
    currentCore.replace("\"", "");

    Serial.printf("After cleaning: '%s' (length: %d)\n", currentCore.c_str(), currentCore.length());

    if (currentCore.length() == 0) {
      currentCore = "MENU";
      Serial.println("Empty response - setting to MENU");
    }

    if (currentCore == "Menu" || currentCore == "MENU") {
      Serial.println("Server reports Menu state - checking debug endpoint");
      checkMisterDebugState();
    }

    if (!isErrorCore(currentCore)) {
      lastValidCore = currentCore;
    }

    if (previousCore != currentCore && previousCore != "") {
      coreChanged = true;
      Serial.printf("Core changed: '%s' -> '%s'\n", previousCore.c_str(), currentCore.c_str());
    }

    connected = true;
    Serial.printf("Core: '%s'\n", currentCore.c_str());
    
    // Check server error state
    checkServerErrorState();
    
  } else {
    // ERROR HANDLING: Use last valid local core
    String errorType = "";
    if (code == -1) {
      Serial.printf("Cannot connect to server\n");
      errorType = "NO SERVER";
    } else if (code == -11) {
      Serial.printf("Timeout\n");
      errorType = "TIMEOUT";
    } else {
      Serial.printf("HTTP error: %d\n", code);
      errorType = "CONNECTION ERROR";
    }
    
    // Use last valid local core as fallback
    if (lastValidCore.length() > 0) {
      currentCore = lastValidCore;
      serverHasError = true;
      serverErrorType = errorType;
      Serial.printf("Connection error, using local last valid core: '%s'\n", lastValidCore.c_str());
    } else {
      currentCore = "Menu";
      serverHasError = true;
      serverErrorType = errorType;
    }
    
    connected = false;
  }
  
  http.end();
}

bool isErrorCore(String core) {
  /**
   * Helper function to check if a core name represents an error state
   * Used for backward compatibility with local error detection
   */
  return (core == "NO SERVER" || core == "TIMEOUT" || core == "OFFLINE" || 
          core == "DISCONNECTED" || core.startsWith("ERROR") || 
          core == "CONNECTION ERROR" || core == "NETWORK ERROR");
}

void getCurrentGame() {
  Serial.printf("DEBUG: getCurrentGame() called\n");
  Serial.printf("DEBUG: previousGame='%s', currentGame='%s'\n", previousGame.c_str(), currentGame.c_str());
  
  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/game";
  
  Serial.printf("Checking for active game: %s\n", url.c_str());
  
  http.begin(url);
  http.setTimeout(5000);
  
  int code = http.GET();
  
  Serial.printf("DEBUG: HTTP response code: %d\n", code);
  
  if (code == 200) {
    String response = http.getString();
    response.trim();
    
    Serial.printf("DEBUG: Raw response: '%s'\n", response.c_str());
    
    previousGame = currentGame;
    currentGame = response;
    
    Serial.printf("DEBUG: After update - previousGame='%s', currentGame='%s'\n", 
                  previousGame.c_str(), currentGame.c_str());
    
    // Detect game change
    if (previousGame != currentGame && currentGame.length() > 0) {
      gameChanged = true;
      Serial.printf("Game changed: '%s' -> '%s'\n", previousGame.c_str(), currentGame.c_str());
      
      startCrcRecurrentForGame(currentGame, currentCore);
      
    } else if (currentGame.length() == 0 && previousGame.length() > 0) {
      gameChanged = true;
      Serial.println("Game unloaded, returning to core image");
      
      stopCrcRecurrent();
    }
    
  } else {
    // Network error - DON'T assume game unloaded
    Serial.printf("Network error (HTTP %d) - keeping current game state\n", code);
    Serial.printf("Current game preserved: '%s'\n", currentGame.c_str());
    
    // DON'T change currentGame or stop CRC recurrent on network errors
    // The CRC recurrent will keep trying every 10 seconds
    
    // Only log the error, don't change game state
  }
  
  http.end();
}

void startCrcRecurrentForGame(String gameName, String coreName) {
  Serial.printf("DEBUG: startCrcRecurrentForGame() called with game='%s', core='%s'\n", 
                gameName.c_str(), coreName.c_str());
  if (gameName.length() == 0 || coreName.length() == 0) {
    Serial.println("Invalid game/core for CRC recurrent");
    return;
  }
  
  // Reset per-game state regardless of whether the recurrent will run
  currentGameForCrc = gameName;
  currentCoreForCrc = coreName;
  lastRomHasCrc            = false;
  lastRomCrcChecked        = false;
  lastGameImageOK          = false;
  lastGameFoundNoMedia     = false;
  
  // Short-circuit: if the core is not in ScreenScraper's DB, do not activate
  // the recurrent. ScreenScraper requires a system ID, and asking MiSTer for
  // ROM details (which calculates CRC32 over the file) is wasted work.
  // Mark the search as exhausted from the start.
  if (getScreenScraperSystemId(coreName).length() == 0) {
    Serial.printf("Core '%s' not mapped to ScreenScraper — recurrent NOT started\n",
                  coreName.c_str());
    lastGameSearchExhausted = true;
    crcRecurrentActive      = false;
    crcRecurrentAttempts    = 0;
    return;
  }
  
  // Normal start
  crcRecurrentActive       = true;
  lastCrcRecurrentTime     = 0;     // Force immediate check
  crcRecurrentAttempts     = 0;
  lastGameSearchExhausted  = false;
  
  Serial.printf("CRC recurrent started: '%s' core '%s'\n", gameName.c_str(), coreName.c_str());
  Serial.printf("DEBUG: crcRecurrentActive=%s\n", crcRecurrentActive ? "true" : "false");
}

void stopCrcRecurrent() {
  /**
   * Stops recurring CRC search
   */
  if (crcRecurrentActive) {
    Serial.printf("CRC recurrent stopped for: '%s'\n", currentGameForCrc.c_str());
  }
  
  crcRecurrentActive = false;
  currentGameForCrc = "";
  currentCoreForCrc = "";
  lastCrcRecurrentTime = 0;
  crcRecurrentAttempts = 0;
}

void processCrcRecurrent() {
  /**
   * Processes recurrent CRC every 10 seconds
   * CALL FROM YOUR MAIN LOOP
   */
  
  static unsigned long lastDebugLog = 0;
  if (millis() - lastDebugLog > 5000) {  // Log every 5 seconds
    Serial.printf("DEBUG processCrcRecurrent(): active=%s, game='%s', downloadInProgress=%s, exhausted=%s\n", 
                  crcRecurrentActive ? "YES" : "NO", 
                  currentGameForCrc.c_str(),
                  downloadInProgress ? "YES" : "NO",
                  lastGameSearchExhausted ? "YES" : "NO");
    lastDebugLog = millis();
  }
  
  // Do nothing if it is not active
  if (!crcRecurrentActive || currentGameForCrc.length() == 0) {
    return;
  }
  
  // CRITICAL: Do not interfere with your downloadInProgress system
  if (downloadInProgress) {
    Serial.printf("DEBUG: Skipping CRC recurrent - download in progress\n");
    return;
  }
  
  // Verify that the game is still the same
  if (currentGameForCrc != currentGame) {
    Serial.printf("Game changed during recurrent, stopping\n");
    stopCrcRecurrent();
    return;
  }
  
  // If the core isn't mapped to any ScreenScraper system, no number of retries
  // will produce a hit. Mark search exhausted and stop.
  if (getScreenScraperSystemId(currentCoreForCrc).length() == 0) {
    Serial.printf("Core '%s' not in ScreenScraper DB — stopping CRC recurrent\n",
                  currentCoreForCrc.c_str());
    lastGameSearchExhausted = true;
    stopCrcRecurrent();
    return;
  }
  
  // if a previous attempt already confirmed the game isn't in
  // ScreenScraper's DB, also stop.
  if (lastGameSearchExhausted) {
    Serial.printf("Search already exhausted for '%s' — stopping CRC recurrent\n",
                  currentGameForCrc.c_str());
    stopCrcRecurrent();
    return;
  }
  
  
  unsigned long now = millis();
  
  // Verify exactly 10 seconds
  if (now - lastCrcRecurrentTime < 10000) {
    return;
  }
  
  // Check if it already has an image (avoids unnecessary searches)
  String imagePath;
  if (findGameImageExact(currentCoreForCrc, currentGameForCrc, imagePath)) {
    Serial.printf("Image found for '%s', stopping CRC recurrent\n", currentGameForCrc.c_str());
    stopCrcRecurrent();
    return;
  }
  
  // Attempt limit (optional, prevents infinite loops)
  if (crcRecurrentAttempts >= 30) {  // 30 attempts = 5 minutes maximum
    Serial.printf("Max recurrent attempts reached for '%s'\n", currentGameForCrc.c_str());
    stopCrcRecurrent();
    return;
  }
  
  // RUN RECURRENT SEARCH
  Serial.printf("CRC recurrent attempt #%d for: '%s'\n", 
                crcRecurrentAttempts + 1, currentGameForCrc.c_str());
  
  lastCrcRecurrentTime = now;
  crcRecurrentAttempts++;

  // USE YOUR EXISTING FUNCTION WITHOUT CHANGES
  bool success = downloadGameBoxartStreamingSafeJSON(currentCoreForCrc, currentGameForCrc);
  
  if (success) {
    Serial.printf("CRC recurrent SUCCESS for: '%s'\n", currentGameForCrc.c_str());
    stopCrcRecurrent();
    
    // Trigger screen update if necessary
    if (currentGameForCrc == currentGame) {
      gameChanged = true;  // Force image refresh
    }
  } else {
    Serial.printf("CRC recurrent attempt #%d failed, retry in 10s\n", crcRecurrentAttempts);
  }
}

void getSystemData() {
  HTTPClient http;
  http.begin(String("http://") + misterIP + ":8081/status/system");
  
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    
    cpuUsage = extractFloatValue(payload, "cpu_usage");
    memoryUsage = extractFloatValue(payload, "memory_usage");
    
    int uptimeSeconds = extractIntValue(payload, "uptime_seconds");
    int hours = uptimeSeconds / 3600;
    int minutes = (uptimeSeconds % 3600) / 60;
    int secs = uptimeSeconds % 60;
    
    uptimeFormatted = String(hours < 10 ? "0" : "") + String(hours) + ":" +
                     String(minutes < 10 ? "0" : "") + String(minutes) + ":" +
                     String(secs < 10 ? "0" : "") + String(secs);
  }
  http.end();
}

void getStorageData() {
  HTTPClient http;
  http.begin(String("http://") + misterIP + ":8081/status/storage");
  
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    
    int sdStart = payload.indexOf("\"sd_card\":");
    if (sdStart != -1) {
      String sdSection = payload.substring(sdStart, payload.indexOf("}", sdStart) + 1);
      
      sdTotalGB = extractFloatValue(sdSection, "total_gb");
      sdUsedGB = extractFloatValue(sdSection, "used_gb");
      sdUsagePercent = extractFloatValue(sdSection, "usage_percent");
    }
  }
  http.end();
}

void getUSBData() {
  HTTPClient http;
  http.begin(String("http://") + misterIP + ":8081/status/usb");
  
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    
    usbDeviceCount = 0;
    serialPortCount = 0;
    
    Serial.printf("=== USB DETECTION DEBUG ===\n");
    Serial.printf("Raw USB Response: %s\n", payload.c_str());
    
    // Count real USB devices (hybrid: blacklist + whitelist of types)
    int pos = 0;
    int totalFound = 0;
    
    while ((pos = payload.indexOf("\"name\":", pos)) != -1) {
      int nameStart = payload.indexOf("\"", pos + 7) + 1;
      int nameEnd = payload.indexOf("\"", nameStart);
      
      if (nameStart > 0 && nameEnd > nameStart) {
        String deviceName = payload.substring(nameStart, nameEnd);
        String deviceNameLower = deviceName;
        deviceNameLower.toLowerCase();
        
        totalFound++;
        Serial.printf("[%d] Device: '%s'\n", totalFound, deviceName.c_str());
        
        // STEP 1: Strict blacklist for system devices
        bool isSystemDevice = false;
        
        // USB hubs and controllers
        if (deviceNameLower.indexOf("hub") != -1 ||
            deviceNameLower.indexOf("root hub") != -1 ||
            deviceNameLower.indexOf("linux foundation") != -1 ||
            deviceNameLower.indexOf("dwc") != -1 ||
            deviceNameLower.indexOf("ehci") != -1 ||
            deviceNameLower.indexOf("ohci") != -1 ||
            deviceNameLower.indexOf("xhci") != -1 ||
            deviceNameLower.indexOf("vhci") != -1 ||
            deviceNameLower.indexOf("otg") != -1) {
          isSystemDevice = true;
        }
        
        // Common specific hubs
        if (deviceNameLower.indexOf("terminus") != -1 ||
            deviceNameLower.indexOf("fe 2.1") != -1 ||
            deviceNameLower.indexOf("7-port") != -1 ||
            deviceNameLower.indexOf("usb 2.0 hub") != -1 ||
            deviceNameLower.indexOf("usb 3.0 hub") != -1 ||
            deviceNameLower.indexOf("usb hub") != -1) {
          isSystemDevice = true;
        }
        
        // System controllers and virtual devices
        if (deviceNameLower.indexOf("virtual") != -1 ||
            deviceNameLower.indexOf("host controller") != -1 ||
            (deviceNameLower.indexOf("controller") != -1 && 
             (deviceNameLower.indexOf("host") != -1 || deviceNameLower.indexOf("otg") != -1))) {
          isSystemDevice = true;
        }
        
        // MiSTer internal devices
        if (deviceNameLower.indexOf("terasic") != -1 ||
            deviceNameLower.indexOf("de10") != -1 ||
            deviceNameLower.indexOf("altera") != -1 ||
            deviceNameLower.indexOf("mister") != -1) {
          isSystemDevice = true;
        }
        
        // Specific vendor IDs for hubs/system
        if (deviceNameLower.indexOf("1d6b:") != -1 ||  // Linux Foundation
            deviceNameLower.indexOf("1a40:0201") != -1) {  // Specific Terminus Hub
          isSystemDevice = true;
        }
        
        // Additional filters for generic/suspicious devices
        if (deviceName.length() < 4 ||                    
            deviceNameLower.indexOf("unknown") != -1 ||   
            deviceNameLower.indexOf("generic") != -1) {   
          isSystemDevice = true;
        }
        
        // If system device, filter immediately
        if (isSystemDevice) {
          Serial.printf("FILTERED OUT (system/internal device)\n");
          pos = nameEnd;
          continue;
        }
        
        // STEP 2: Whitelist of device TYPES (not specific brands)
        bool isRealDevice = false;
        
        // Input peripherals (by type, not brand)
        if (deviceNameLower.indexOf("keyboard") != -1 ||
            deviceNameLower.indexOf("mouse") != -1 ||
            deviceNameLower.indexOf("gamepad") != -1 ||
            deviceNameLower.indexOf("joystick") != -1) {
          isRealDevice = true;
        }
        
        // Game controllers (by type/function)
        if (deviceNameLower.indexOf("controller") != -1 ||
            deviceNameLower.indexOf("pad") != -1 ||
            deviceNameLower.indexOf("stick") != -1) {
          isRealDevice = true;
        }
        
        // USB-Serial adapters (by function)
        if (deviceNameLower.indexOf("usb serial") != -1 ||
            deviceNameLower.indexOf("serial adapter") != -1 ||
            deviceNameLower.indexOf("usb-serial") != -1 ||
            deviceNameLower.indexOf("hl-340") != -1) {
          isRealDevice = true;
        }
        
        // USB storage (by type)
        if (deviceNameLower.indexOf("mass storage") != -1 ||
            deviceNameLower.indexOf("usb drive") != -1 ||
            deviceNameLower.indexOf("flash") != -1 ||
            deviceNameLower.indexOf("storage") != -1) {
          isRealDevice = true;
        }
        
        // USB audio/video devices
        if (deviceNameLower.indexOf("audio") != -1 ||
            deviceNameLower.indexOf("webcam") != -1 ||
            deviceNameLower.indexOf("camera") != -1) {
          isRealDevice = true;
        }
        
        // Known peripheral brands (extensive but not exhaustive list)
        if (deviceNameLower.indexOf("trust") != -1 ||
            deviceNameLower.indexOf("8bitdo") != -1 ||
            deviceNameLower.indexOf("microsoft") != -1 ||
            deviceNameLower.indexOf("logitech") != -1 ||
            deviceNameLower.indexOf("razer") != -1 ||
            deviceNameLower.indexOf("corsair") != -1 ||
            deviceNameLower.indexOf("steelseries") != -1 ||
            deviceNameLower.indexOf("roccat") != -1 ||
            deviceNameLower.indexOf("cooler master") != -1 ||
            deviceNameLower.indexOf("hyperx") != -1 ||
            deviceNameLower.indexOf("sony") != -1 ||
            deviceNameLower.indexOf("nintendo") != -1 ||
            deviceNameLower.indexOf("qinheng") != -1 ||
            deviceNameLower.indexOf("ftdi") != -1 ||
            deviceNameLower.indexOf("prolific") != -1) {
          isRealDevice = true;
        }
        
        // Final logic: count only if real device
        if (isRealDevice) {
          usbDeviceCount++;
          Serial.printf("COUNTED as external device (%d total) - Real device\n", usbDeviceCount);
        } else {
          Serial.printf("  ? UNKNOWN TYPE - not counted (might be internal)\n");
        }
      }
      pos = nameEnd;
    }
    
    // Count serial ports (ttyUSB, ttyACM)
    pos = 0;
    while ((pos = payload.indexOf("\"tty", pos)) != -1) {
      int nameStart = pos + 1;
      int nameEnd = payload.indexOf("\"", nameStart);
      
      if (nameStart > 0 && nameEnd > nameStart) {
        String portName = payload.substring(nameStart, nameEnd);
        
        if (portName.startsWith("ttyUSB") || portName.startsWith("ttyACM")) {
          serialPortCount++;
          Serial.printf("Serial port: %s (%d total)\n", portName.c_str(), serialPortCount);
        }
      }
      pos = nameEnd;
    }
    
    Serial.printf("=== USB SUMMARY ===\n");
    Serial.printf("Total devices found: %d\n", totalFound);
    Serial.printf("External devices: %d\n", usbDeviceCount);
    Serial.printf("Serial ports: %d\n", serialPortCount);
    Serial.printf("==================\n");
  } else {
    Serial.printf("Error getting USB data: %d\n", code);
    usbDeviceCount = 0;
    serialPortCount = 0;
  }
  http.end();
}

void getNetworkAndSession() {
  HTTPClient http;
  http.begin(String("http://") + misterIP + ":8081/status/all");
  
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    
    networkIP = extractStringValue(payload, "ip_address");
    networkConnected = (payload.indexOf("\"connected\":true") != -1);
    sessionDuration = extractStringValue(payload, "session_duration_formatted");
    requestsCount = extractIntValue(payload, "requests_count");
  }
  http.end();
}

void updateDisplay() {
  // Don't clear screen or draw header for pages that use frame02.jpg
  // Those pages handle their own background and layout
  
  // Pages 0-4 all use frame02.jpg, so they handle everything themselves
  // No need for Lcd.fillScreen() or drawHeader() here
  
  // Content by page
  switch(currentPage) {
    case 0: displayMainHUD(); break;
    case 1: displaySystemMonitor(); break;
    case 2: displayStorageArray(); break;
    case 3: displayNetworkTerminal(); break;
    case 4: displayDeviceScanner(); break;
    case 5: displayGameInfo(); break;
  }
  
  // Note: Each display function now handles:
  // - Loading frame02.jpg background (if not already loaded)
  // - Drawing logo and buttons (if not already drawn)
  // - Clearing and updating only its content area
  // - Drawing footer
}

void displayMainHUD() {
  // ========== LOAD FULL SCREEN FRAME IMAGE (only if needed) ==========
  if (!backgroundLoaded) {
    M5.Display.fillScreen(THEME_BLACK);
    loadFullScreenFrame("/cores/frame02.jpg");
    // delay(50);
    
    // ========== DRAW MISTER LOGO IN RIGHT PANEL ==========
    drawMisterLogoRightPanel();
    
    // Draw touch buttons (only once since they don't change)
    btnPrev.draw();
    btnScan.draw();
    btnNext.draw();
    
    backgroundLoaded = true;
  }
  
  // ========== CLEAR CONTENT AREA ONLY ==========
  // Clear area where original 320x240 content was drawn (Y=35 to Y=205)
  Lcd.fillRect(0, 35, 320, 180, THEME_BLACK);
  
  // ========== ORIGINAL 320x240 CONTENT (auto-scaled 2x by Lcd) ==========
  uint16_t panelColor = connected ? THEME_GREEN : THEME_YELLOW;
  drawPanel(10, 50, 300, 70, panelColor);
  
  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 60);
  Lcd.print("ACTIVE CORE");
  
  // Core name with scroll for long names. Window of 14 chars at size 3 fits
  // comfortably between x=20 and x=290 logical (plenty of margin).
  String coreNormalized = currentCore;
  if (coreNormalized.equalsIgnoreCase("arcade")) {
    coreNormalized = "Arcade";
  }
  
  if (mainHUDCoreScroll.fullText != coreNormalized) {
    initScrollText(&mainHUDCoreScroll, coreNormalized, 14);
  }
  String coreDisplay = getScrolledText(&mainHUDCoreScroll);
  // Pad to 14 chars for stable pixel width (avoids stale glyphs to the right)
  while ((int)coreDisplay.length() < 14) coreDisplay += ' ';
  
  // Use setTextColor(fg, bg) for flicker-free repaint when this function
  // is called repeatedly (which it is, from updateDisplay()).
  Lcd.setTextColor(THEME_BLACK, panelColor);
  Lcd.setTextSize(3);
  Lcd.setCursor(20, 80);
  Lcd.print(coreDisplay);
  
  if (!connected) {
    Lcd.setTextColor(THEME_BLACK);
    Lcd.setTextSize(1);
    Lcd.setCursor(20, 105);
    Lcd.print("CHECK CONNECTION");
  } else if (sdCardAvailable) {
    Lcd.setTextColor(THEME_BLACK);
    Lcd.setTextSize(1);
    Lcd.setCursor(20, 105);
    Lcd.print("CORE IMAGES: READY");
  }
  
  // Digital uptime clock
  drawDigitalClock(10, 130, uptimeFormatted, "UPTIME");
  
  // Metrics in small panels
  drawMiniPanel(10, 170, 90, 30, "CPU", String(cpuUsage, 1) + "%", cpuUsage > 80 ? THEME_RED : THEME_GREEN);
  drawMiniPanel(115, 170, 90, 30, "RAM", String(memoryUsage, 1) + "%", memoryUsage > 80 ? THEME_RED : THEME_GREEN);
  drawMiniPanel(220, 170, 90, 30, "USB", String(usbDeviceCount), THEME_CYAN);
  
  if (!connected) {
    Lcd.setTextColor(THEME_CYAN);
    Lcd.setTextSize(1);
    Lcd.setCursor(10, 155);
    Lcd.print("Reconnecting to MiSTer...");
  } else if (sdCardAvailable) {
    Lcd.setTextColor(THEME_CYAN);
    Lcd.setTextSize(1);
    Lcd.setCursor(10, 155);
    Lcd.print("Core changes show images automatically");
  }
  
  drawPageIndicators();
  // Footer
  drawFooter();
}

void displaySystemMonitor() {
  // ========== LOAD FULL SCREEN FRAME IMAGE (only if needed) ==========
  if (!backgroundLoaded) {
    M5.Display.fillScreen(THEME_BLACK);
    loadFullScreenFrame("/cores/frame02.jpg");
    delay(50);
    
    // ========== DRAW MISTER LOGO IN RIGHT PANEL ==========
    drawMisterLogoRightPanel();
    
    // Draw touch buttons (only once since they don't change)
    btnPrev.draw();
    btnScan.draw();
    btnNext.draw();
    
    backgroundLoaded = true;
  }
  
  // ========== CLEAR CONTENT AREA ONLY ==========
  Lcd.fillRect(0, 35, 320, 180, THEME_BLACK);
  
  // ========== ORIGINAL 320x240 CONTENT (auto-scaled 2x by Lcd) ==========
  drawPanel(10, 50, 300, 40, THEME_GREEN);
  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 57);
  Lcd.print("CPU LOAD");
  Lcd.setTextSize(2);
  Lcd.setCursor(20, 72);
  Lcd.printf("%.1f%%", cpuUsage);
  
  drawProgressBar(120, 65, 160, 15, cpuUsage);
  
  drawPanel(10, 100, 300, 40, THEME_CYAN);
  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 107);
  Lcd.print("MEMORY");
  Lcd.setTextSize(2);
  Lcd.setCursor(20, 122);
  Lcd.printf("%.1f%%", memoryUsage);
  
  drawProgressBar(120, 115, 160, 15, memoryUsage);
  
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(1);
  Lcd.setCursor(10, 155);
  Lcd.print("SYSTEM STATUS:");
  
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setCursor(10, 170);
  Lcd.printf("RUNTIME: %s", uptimeFormatted.c_str());
  
  Lcd.setCursor(10, 185);
  Lcd.printf("CONNECTION: %s", connected ? "ACTIVE" : "LOST");
  
  drawPageIndicators();
  // Footer
  drawFooter();
}

void displayStorageArray() {
  // ========== LOAD FULL SCREEN FRAME IMAGE (only if needed) ==========
  if (!backgroundLoaded) {
    M5.Display.fillScreen(THEME_BLACK);
    loadFullScreenFrame("/cores/frame02.jpg");
    delay(50);
    
    // ========== DRAW MISTER LOGO IN RIGHT PANEL ==========
    drawMisterLogoRightPanel();
    
    // Draw touch buttons (only once since they don't change)
    btnPrev.draw();
    btnScan.draw();
    btnNext.draw();
    
    backgroundLoaded = true;
  }
  
  // ========== CLEAR CONTENT AREA ONLY ==========
  Lcd.fillRect(0, 35, 320, 180, THEME_BLACK);
  
  // ========== ORIGINAL 320x240 CONTENT (auto-scaled 2x by Lcd) ==========
  drawPanel(10, 50, 300, 80, THEME_ORANGE);
  
  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 60);
  Lcd.print("STORAGE ARRAY - SD CARD");
  
  Lcd.setTextSize(2);
  Lcd.setCursor(20, 80);
  Lcd.printf("%.1fGB", sdUsedGB);
  
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 100);
  Lcd.printf("of %.1fGB total", sdTotalGB);
  
  Lcd.setCursor(20, 115);
  Lcd.printf("Free: %.1fGB", sdTotalGB - sdUsedGB);
  
  drawStorageBar(10, 140, 300, 25, sdUsagePercent);
  
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1);
  Lcd.setCursor(10, 180);
  Lcd.printf("USAGE: %.0f%% | LOCAL SD: %s", sdUsagePercent, sdCardAvailable ? "OK" : "ERROR");
  
  drawPageIndicators();
  // Footer
  drawFooter();
}

void displayNetworkTerminal() {
  // ========== LOAD FULL SCREEN FRAME IMAGE (only if needed) ==========
  if (!backgroundLoaded) {
    M5.Display.fillScreen(THEME_BLACK);
    loadFullScreenFrame("/cores/frame02.jpg");
    delay(50);
    
    // ========== DRAW MISTER LOGO IN RIGHT PANEL ==========
    drawMisterLogoRightPanel();
    
    // Draw touch buttons (only once since they don't change)
    btnPrev.draw();
    btnScan.draw();
    btnNext.draw();
    
    backgroundLoaded = true;
  }
  
  // ========== CLEAR CONTENT AREA ONLY ==========
  Lcd.fillRect(0, 35, 320, 180, THEME_BLACK);
  
  // ========== ORIGINAL 320x240 CONTENT (auto-scaled 2x by Lcd) ==========
  // Main panel - based on M5Stack ↔ MiSTer connection
  drawPanel(10, 50, 300, 60, connected ? THEME_GREEN : THEME_RED);
  
  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 60);
  Lcd.print("M5STACK <-> MISTER");
  
  Lcd.setTextSize(2);
  Lcd.setCursor(20, 75);
  Lcd.print(connected ? "CONNECTED" : "DISCONNECTED");
  
  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 95);
  Lcd.printf("Target: %s:8081", misterIP);
  
  // Detailed network info
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(1);
  Lcd.setCursor(10, 125);
  Lcd.print("NETWORK INFO:");
  
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setCursor(10, 140);
  Lcd.printf("MiSTer IP: %s", connected && networkIP != "N/A" ? networkIP.c_str() : misterIP);
  
  Lcd.setCursor(10, 155);
  Lcd.printf("Monitor IP: %s", WiFi.localIP().toString().c_str());
  
  // Only show MiSTer network status if we have data
  if (connected && networkConnected) {
    Lcd.setCursor(10, 170);
    Lcd.setTextColor(THEME_GREEN);
    Lcd.printf("MiSTer Network: ONLINE");
  } else if (connected) {
    Lcd.setCursor(10, 170);
    Lcd.setTextColor(THEME_CYAN);
    Lcd.printf("MiSTer Network: Unknown");
  }
  
  // Server statistics
  if (connected) {
    Lcd.setCursor(10, 185);
    Lcd.setTextColor(THEME_CYAN);
    Lcd.printf("Session: %s | Requests: %d", sessionDuration.c_str(), requestsCount);
  } else {
    Lcd.setCursor(10, 185);
    Lcd.setTextColor(THEME_RED);
    Lcd.print("Check server & network settings");
  }
  
  drawPageIndicators();
  // Footer
  drawFooter();
}

void displayDeviceScanner() {
  // ========== LOAD FULL SCREEN FRAME IMAGE (only if needed) ==========
  if (!backgroundLoaded) {
    M5.Display.fillScreen(THEME_BLACK);
    loadFullScreenFrame("/cores/frame02.jpg");
    delay(50);
    
    // ========== DRAW MISTER LOGO IN RIGHT PANEL ==========
    drawMisterLogoRightPanel();
    
    // Draw touch buttons (only once since they don't change)
    btnPrev.draw();
    btnScan.draw();
    btnNext.draw();
    
    backgroundLoaded = true;
  }
  
  // ========== CLEAR CONTENT AREA ONLY ==========
  Lcd.fillRect(0, 35, 320, 180, THEME_BLACK);
  
  // ========== ORIGINAL 320x240 CONTENT (auto-scaled 2x by Lcd) ==========
  drawPanel(10, 50, 300, 50, THEME_BLUE);
  
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 60);
  Lcd.print("DEVICE SCANNER");
  
  Lcd.setTextSize(2);
  Lcd.setCursor(20, 75);
  Lcd.printf("USB: %d | SERIAL: %d", usbDeviceCount, serialPortCount);
  
  drawPortArray(10, 105, usbDeviceCount, serialPortCount);
  
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1);
  Lcd.setCursor(10, 202);
  Lcd.printf("ACTIVE DEVICES DETECTED | USB PORTS SCANNED");
  
  drawPageIndicators();
  // Footer
  drawFooter();
}

void drawHeader(String title, String subtitle) {
  Lcd.fillRect(0, 0, 320, 35, THEME_BLACK);
  
  drawMiSTerLogo(10, 5);
  
  Lcd.drawFastHLine(0, 32, 320, THEME_YELLOW);
  Lcd.drawFastHLine(0, 33, 320, THEME_YELLOW);
  
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1);
  Lcd.setCursor(120, 8);
  Lcd.print(title);
  
  Lcd.setTextColor(THEME_GREEN);
  Lcd.setCursor(120, 20);
  Lcd.print(subtitle);
  
  for (int i = 0; i < totalPages; i++) {
    uint16_t color = (i == currentPage) ? THEME_YELLOW : THEME_GRAY;
    Lcd.fillRect(250 + i * 8, 8, 6, 6, color);
    if (i == currentPage) {
      Lcd.drawRect(249 + i * 8, 7, 8, 8, THEME_YELLOW);
    }
  }
  
  drawHudConnectionDot(connected, connected);
}

// Connection dot for the HUD screens, drawn in PHYSICAL coordinates. The page
// squares (drawPageIndicators) live at physical X 410..690 / Y 210..240, past
// the 320-wide logical header, so a logical drawStatusIndicator(290,15) scales
// onto them. This draws the dot in the black gap to the RIGHT of the squares.
void drawHudConnectionDot(bool isConnected, bool active) {
  int cx = 720, cy = 225, r = 12;
  if (active) {
    M5.Display.fillCircle(cx, cy, r, isConnected ? THEME_GREEN : THEME_RED);
    M5.Display.fillCircle(cx, cy, r / 2, THEME_WHITE);
  } else {
    M5.Display.drawCircle(cx, cy, r, isConnected ? THEME_GREEN : THEME_RED);
  }
}

void drawPageIndicators() {
  // Draw page indicators showing which screen is active
  // Position: using physical coordinates (not scaled)
  for (int i = 0; i < totalPages; i++) {
    uint16_t color = (i == currentPage) ? THEME_YELLOW : THEME_GRAY;
    
    int indicatorX = 410 + i * 50;  // Positioned in right panel
    int indicatorY = 210;
    int indicatorSize = 30;
    
    // Filled square
    M5.Display.fillRect(indicatorX, indicatorY, indicatorSize, indicatorSize, color);
    
    // Border for active page
    if (i == currentPage) {
      M5.Display.drawRect(indicatorX - 2, indicatorY - 2, 
                         indicatorSize + 4, indicatorSize + 4, THEME_YELLOW);
    } else {
      M5.Display.drawRect(indicatorX, indicatorY, 
                         indicatorSize, indicatorSize, THEME_CYAN);
    }
  }
}

void drawMiSTerLogo(int x, int y) {
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(2);
  Lcd.setCursor(x, y);
  Lcd.print("MiSTer");
  
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(1);
  Lcd.setCursor(x, y + 18);
  Lcd.print("FPGA");
  
  for (int i = 0; i < 8; i++) {
    Lcd.fillCircle(x + 45 + i * 4, y + 22, 1, THEME_CYAN);
  }
}

void drawPanel(int x, int y, int w, int h, uint16_t color) {
  Lcd.fillRect(x, y, w, h, color);
  Lcd.drawRect(x, y, w, h, THEME_WHITE);
  Lcd.drawRect(x + 1, y + 1, w - 2, h - 2, THEME_WHITE);
  
  Lcd.drawLine(x, y, x + 8, y, THEME_BLACK);
  Lcd.drawLine(x, y, x, y + 8, THEME_BLACK);
  Lcd.drawLine(x + w - 8, y, x + w, y, THEME_BLACK);
  Lcd.drawLine(x + w, y, x + w, y + 8, THEME_BLACK);
}

void drawMiniPanel(int x, int y, int w, int h, String label, String value, uint16_t color) {
  Lcd.drawRect(x, y, w, h, color);
  Lcd.fillRect(x + 1, y + 1, w - 2, h - 2, THEME_BLACK);
  
  Lcd.setTextColor(color);
  Lcd.setTextSize(1);
  Lcd.setCursor(x + 5, y + 5);
  Lcd.print(label);
  
  Lcd.setTextSize(1);
  Lcd.setCursor(x + 5, y + 18);
  Lcd.print(value);
}

void drawProgressBar(int x, int y, int w, int h, float percent) {
  Lcd.drawRect(x, y, w, h, THEME_WHITE);
  Lcd.fillRect(x + 1, y + 1, w - 2, h - 2, THEME_BLACK);
  
  int fillW = (percent / 100.0) * (w - 4);
  if (fillW > 0) {
    uint16_t barColor = (percent > 80) ? THEME_RED : 
                       (percent > 60) ? THEME_YELLOW : THEME_GREEN;
    
    Lcd.fillRect(x + 2, y + 2, fillW, h - 4, barColor);
    Lcd.drawFastHLine(x + 2, y + 2, fillW, THEME_WHITE);
  }
}

void drawStorageBar(int x, int y, int w, int h, float percent) {
  drawProgressBar(x, y, w, h, percent);
  
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(2);
  int textX = x + (w / 2) - 20;
  Lcd.setCursor(textX, y + 5);
  Lcd.printf("%.0f%%", percent);
}

void drawDigitalClock(int x, int y, String time, String label) {
  Lcd.setTextColor(THEME_GREEN);
  Lcd.setTextSize(1);
  Lcd.setCursor(x, y);
  Lcd.print(label + ":");
  
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(2);
  Lcd.setCursor(x + 60, y - 3);
  Lcd.print(time);
}

void drawStatusIndicator(int x, int y, uint16_t color, bool active) {
  if (active) {
    Lcd.fillCircle(x, y, 8, color);
    Lcd.fillCircle(x, y, 4, THEME_WHITE);
  } else {
    Lcd.drawCircle(x, y, 8, color);
    Lcd.drawCircle(x, y, 4, color);
  }
}

void drawRadarScan(int centerX, int centerY, int radius, int angle) {
  for (int r = 20; r <= radius; r += 20) {
    Lcd.drawCircle(centerX, centerY, r, THEME_GREEN);
  }
  
  float radAngle = angle * PI / 180.0;
  int endX = centerX + radius * cos(radAngle);
  int endY = centerY + radius * sin(radAngle);
  
  Lcd.drawLine(centerX, centerY, endX, endY, THEME_YELLOW);
}

void drawPortArray(int x, int y, int usbCount, int serialCount) {
  // Show USB port information more informatively
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(x, y);
  Lcd.printf("USB DEVICES CONNECTED: %d", usbCount);
  
  Lcd.setCursor(x, y + 15);
  Lcd.printf("SERIAL PORTS ACTIVE: %d", serialCount);
  
  // Simplified graphical visualization of connected devices
  int maxDisplay = min(usbCount, 8); // Display maximum 8 devices
  for (int i = 0; i < 8; i++) {
    uint16_t color = (i < maxDisplay) ? THEME_GREEN : THEME_GRAY;
    int posX = x + (i % 4) * 35;
    int posY = y + 35 + (i / 4) * 25;
    
    Lcd.drawRect(posX, posY, 30, 15, color);
    if (i < maxDisplay) {
      Lcd.fillRect(posX + 2, posY + 2, 26, 11, color);
    }
    
    Lcd.setTextColor(THEME_WHITE);
    Lcd.setTextSize(1);
    Lcd.setCursor(posX + 8, posY + 18);
    Lcd.printf("U%d", i + 1);
  }
  
  // Serial port indicators
  for (int i = 0; i < 3; i++) {
    uint16_t color = (i < serialCount) ? THEME_CYAN : THEME_GRAY;
    int posX = x + 200 + i * 25;
    int posY = y + 35;
    
    Lcd.drawRect(posX, posY, 20, 15, color);
    if (i < serialCount) {
      Lcd.fillRect(posX + 2, posY + 2, 16, 11, color);
    }
    
    Lcd.setTextColor(THEME_WHITE);
    Lcd.setTextSize(1);
    Lcd.setCursor(posX + 4, posY + 18);
    Lcd.printf("S%d", i + 1);
  }
  
  // Additional info if there are more devices
  if (usbCount > 8) {
    Lcd.setTextColor(THEME_YELLOW);
    Lcd.setTextSize(1);
    Lcd.setCursor(x, y + 85);
    Lcd.printf("+ %d more USB devices", usbCount - 8);
  }
}

// =============================================================================
// ============================ GAME INFO PANEL ================================
// =============================================================================
// Metadata (synopsis, year, publisher, developer, genre, players, rating) for
// the game in play, extracted from ScreenScraper's jeuInfos.php and cached on
// the microSD as a .meta sidecar next to the artwork.
//
// Design notes (see docs/propuesta-game-info-panel.md):
//  - ONE extra jeuInfos call per NEW game, then cached forever on SD.
//  - The response is read through a bounded SLIDING WINDOW so the huge
//    multi-language synopsis[] never accumulates in heap (window <= ~6 KB).
//  - Matching is whitespace-tolerant: the live API returns `"key": value`
//    with a space after the colon (verified against real captures).
//  - Extraction is ORDER-INDEPENDENT: each field is captured whenever its
//    marker shows up, so field reordering on the API side cannot break it.
// =============================================================================

// -----------------------------------------------------------------------------
// Sidecar metadata path: same folder + base name as the artwork, ext .meta
// "/cores/S/Game (USA).jpg" -> "/cores/S/Game (USA).meta"
// -----------------------------------------------------------------------------
String getMetaPathFromImagePath(const String &imagePath) {
  int dot = imagePath.lastIndexOf('.');
  if (dot <= 0) return imagePath + ".meta";
  return imagePath.substring(0, dot) + ".meta";
}

bool saveGameMeta(const String &metaPath, const GameMeta &m) {
  File f = SD.open(metaPath, FILE_WRITE);
  if (!f) {
    Serial.printf("META: cannot open %s for write\n", metaPath.c_str());
    return false;
  }
  // synopsis goes LAST; values are single-line (whitespace already collapsed)
  f.printf("v=1\nlang=%s\n", _info_lang_str.c_str());
  f.printf("year=%s\n",      m.year.c_str());
  f.printf("developer=%s\n", m.developer.c_str());
  f.printf("publisher=%s\n", m.publisher.c_str());
  f.printf("players=%s\n",   m.players.c_str());
  f.printf("rating=%s\n",    m.rating.c_str());
  f.printf("genre=%s\n",     m.genre.c_str());
  f.printf("synopsis=%s\n",  m.synopsis.c_str());
  f.close();
  Serial.printf("META: saved %s\n", metaPath.c_str());
  return true;
}

bool loadGameMeta(const String &metaPath, GameMeta &m) {
  if (!SD.exists(metaPath)) return false;
  File f = SD.open(metaPath, FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');           // split on FIRST '=' only
    if (eq < 1) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    if      (key == "year")      m.year      = val;
    else if (key == "developer") m.developer = jsonUnescapeAndFold(val);
    else if (key == "publisher") m.publisher = jsonUnescapeAndFold(val);
    else if (key == "players")   m.players   = val;
    else if (key == "rating")    m.rating    = val;
    else if (key == "genre")     m.genre     = jsonUnescapeAndFold(val);
    else if (key == "synopsis")  m.synopsis  = jsonUnescapeAndFold(val);
    // Free-text fields are run through the decoder on load as well: it is
    // idempotent on already-clean text, and it repairs sidecars written by
    // earlier firmware that stored raw HTML entities such as &quot;.
  }
  f.close();
  m.loaded = true;
  Serial.printf("META: loaded %s\n", metaPath.c_str());
  return true;
}

// -----------------------------------------------------------------------------
// JSON scanning helpers — whitespace-tolerant (the live ScreenScraper API
// returns `"key": "value"` with spaces after colons; verified on captures).
// All of them operate on a partial window and return -1 / "" when the
// pattern is not COMPLETELY inside the window yet, so callers simply retry
// on the next chunk.
// -----------------------------------------------------------------------------
static inline bool metaIsWs(char c) {
  return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

// Index of `"key"` whose string value equals `value` (tolerating whitespace
// around ':'), or -1.
static int findKeyStringValue(const String &win, const char *key,
                              const char *value, int from = 0) {
  String kq = String("\"") + key + "\"";
  String vq = String("\"") + value + "\"";
  int p = win.indexOf(kq, from);
  while (p >= 0) {
    int i = p + kq.length();
    while (i < (int)win.length() && metaIsWs(win[i])) i++;
    if (i < (int)win.length() && win[i] == ':') {
      i++;
      while (i < (int)win.length() && metaIsWs(win[i])) i++;
      if ((int)win.length() - i >= (int)vq.length() &&
          win.substring(i, i + vq.length()) == vq) return p;
    }
    p = win.indexOf(kq, p + 1);
  }
  return -1;
}

// Index of the '[' that opens the array value of `"key"`, or -1.
static int findKeyArrayStart(const String &win, const char *key, int from = 0) {
  String kq = String("\"") + key + "\"";
  int p = win.indexOf(kq, from);
  while (p >= 0) {
    int i = p + kq.length();
    while (i < (int)win.length() && metaIsWs(win[i])) i++;
    if (i < (int)win.length() && win[i] == ':') {
      i++;
      while (i < (int)win.length() && metaIsWs(win[i])) i++;
      if (i < (int)win.length() && win[i] == '[') return i;
    }
    p = win.indexOf(kq, p + 1);
  }
  return -1;
}

// Index just past the opening quote of the FIRST `"text": "` at/after `from`,
// or -1 when not fully in the window yet.
static int findTextValueStart(const String &win, int from) {
  int t = win.indexOf("\"text\"", from);
  if (t < 0) return -1;
  int i = t + 6;
  while (i < (int)win.length() && metaIsWs(win[i])) i++;
  if (i >= (int)win.length() || win[i] != ':') return -1;
  i++;
  while (i < (int)win.length() && metaIsWs(win[i])) i++;
  if (i >= (int)win.length() || win[i] != '"') return -1;
  return i + 1;
}

// Raw (still-escaped) value of the first `"text"` following `"marker"`.
// "" until the closing unescaped quote is inside the window.
static String extractTextAfterMarker(const String &win, const char *marker) {
  int m = win.indexOf(String("\"") + marker + "\"");
  if (m < 0) return "";
  int q1 = findTextValueStart(win, m);
  if (q1 < 0) return "";
  int i = q1;
  while (i < (int)win.length()) {
    if (win[i] == '\\') { i += 2; continue; }
    if (win[i] == '"') return win.substring(q1, i);
    i++;
  }
  return "";
}

// -----------------------------------------------------------------------------
// foldLatin1() / jsonUnescapeAndFold()
// - Resolves JSON escapes: \" \\ \/ \n \r \t \uXXXX
// - Folds common UTF-8 Latin characters to plain ASCII so the stock GLCD
//   font can render synopsis text (a-acute -> a, n-tilde -> n, ...).
// - Collapses whitespace runs to a single space.
// -----------------------------------------------------------------------------
static char foldLatin1(uint16_t cp) {
  switch (cp) {
    case 0xE1: case 0xE0: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return 'a';
    case 0xC1: case 0xC0: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
    case 0xE9: case 0xE8: case 0xEA: case 0xEB: return 'e';
    case 0xC9: case 0xC8: case 0xCA: case 0xCB: return 'E';
    case 0xED: case 0xEC: case 0xEE: case 0xEF: return 'i';
    case 0xCD: case 0xCC: case 0xCE: case 0xCF: return 'I';
    case 0xF3: case 0xF2: case 0xF4: case 0xF5: case 0xF6: return 'o';
    case 0xD3: case 0xD2: case 0xD4: case 0xD5: case 0xD6: return 'O';
    case 0xFA: case 0xF9: case 0xFB: case 0xFC: return 'u';
    case 0xDA: case 0xD9: case 0xDB: case 0xDC: return 'U';
    case 0xF1: return 'n';  case 0xD1: return 'N';   // n-tilde
    case 0xE7: return 'c';  case 0xC7: return 'C';   // c-cedilla
    case 0xDF: return 's';                            // sharp s
    case 0x2019: case 0x2018: return '\'';            // curly quotes
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: return '-';             // en/em dash
    case 0x2026: return '.';                          // ellipsis (approx)
    case 0x00A0: return ' ';                          // nbsp
    case 0x00A1: case 0x00BF: return 0;               // inverted !/? -> drop
    default: return (cp < 0x80) ? (char)cp : '?';
  }
}

// -----------------------------------------------------------------------------
// decodeHtmlEntity() — resolve one HTML entity starting at in[i] (which is '&').
// On success writes the Unicode code point to `cp` and returns the index just
// past the ';'. On failure returns -1 and the caller emits a literal '&'.
// ScreenScraper synopses are HTML fragments, so they carry &quot; &amp; &eacute;
// and friends on top of the JSON escaping.
// -----------------------------------------------------------------------------
static int decodeHtmlEntity(const String &in, int i, uint16_t &cp) {
  int semi = in.indexOf(';', i + 1);
  if (semi < 0 || semi - i > 10) return -1;          // not an entity
  String name = in.substring(i + 1, semi);
  if (name.length() == 0) return -1;

  if (name[0] == '#') {                              // numeric: &#233; or &#xE9;
    long v = (name.length() > 1 && (name[1] == 'x' || name[1] == 'X'))
               ? strtol(name.c_str() + 2, nullptr, 16)
               : strtol(name.c_str() + 1, nullptr, 10);
    if (v <= 0 || v > 0xFFFF) return -1;
    cp = (uint16_t)v;
    return semi + 1;
  }

  struct Ent { const char *n; uint16_t cp; };
  static const Ent TABLE[] = {
    {"quot", 0x22}, {"apos", 0x27}, {"amp", 0x26}, {"lt", 0x3C}, {"gt", 0x3E},
    {"nbsp", 0xA0}, {"iexcl", 0xA1}, {"iquest", 0xBF}, {"laquo", 0xAB},
    {"raquo", 0xBB}, {"deg", 0xB0}, {"middot", 0xB7}, {"times", 0xD7},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026},
    {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2}, {"auml", 0xE4},
    {"ccedil", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecirc", 0xEA},
    {"euml", 0xEB}, {"igrave", 0xEC}, {"iacute", 0xED}, {"icirc", 0xEE},
    {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4},
    {"ouml", 0xF6}, {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucirc", 0xFB},
    {"uuml", 0xFC}, {"szlig", 0xDF},
  };
  for (unsigned k = 0; k < sizeof(TABLE) / sizeof(TABLE[0]); k++) {
    if (name == TABLE[k].n) { cp = TABLE[k].cp; return semi + 1; }
  }
  return -1;
}

String jsonUnescapeAndFold(const String &in) {
  String out;
  out.reserve(in.length());
  for (int i = 0; i < (int)in.length(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '\\' && i + 1 < (int)in.length()) {
      char n = in[i + 1];
      if      (n == 'n' || n == 'r' || n == 't') { out += ' '; i++; }
      else if (n == '"' || n == '\\' || n == '/') { out += n;  i++; }
      else if (n == 'u' && i + 5 < (int)in.length()) {
        char hex[5];
        in.substring(i + 2, i + 6).toCharArray(hex, 5);
        uint16_t cp = (uint16_t)strtol(hex, nullptr, 16);
        char fc = foldLatin1(cp);
        if (fc) out += fc;
        i += 5;
      }
      else { i++; }                                  // unknown escape: drop
    }
    else if (c == '&') {                             // HTML entity, e.g. &quot;
      uint16_t cp = 0;
      int next = decodeHtmlEntity(in, i, cp);
      if (next > 0) {
        char fc = foldLatin1(cp);
        if (fc) out += fc;
        i = next - 1;                                // loop's i++ lands on next
      } else {
        out += '&';                                  // a literal ampersand
      }
    }
    else if (c < 0x80) out += (char)c;
    else if ((c & 0xE0) == 0xC0 && i + 1 < (int)in.length()) {   // 2-byte UTF-8
      uint16_t cp = ((c & 0x1F) << 6) | ((unsigned char)in[i + 1] & 0x3F);
      char fc = foldLatin1(cp);
      if (fc) out += fc;
      i++;
    }
    else if ((c & 0xF0) == 0xE0 && i + 2 < (int)in.length()) {   // 3-byte UTF-8
      uint16_t cp = ((c & 0x0F) << 12) |
                    (((unsigned char)in[i + 1] & 0x3F) << 6) |
                    ((unsigned char)in[i + 2] & 0x3F);
      char fc = foldLatin1(cp);
      if (fc) out += fc;
      i += 2;
    }
    else out += '?';                                 // 4-byte or malformed
  }
  // Collapse whitespace runs
  String clean;
  clean.reserve(out.length());
  bool prevSpace = false;
  for (int i = 0; i < (int)out.length(); i++) {
    char c = out[i];
    if (c == ' ' || c == '\t') { if (!prevSpace) clean += ' '; prevSpace = true; }
    else { clean += c; prevSpace = false; }
  }
  clean.trim();
  return clean;
}

// -----------------------------------------------------------------------------
// fetchGameMetadataJSON()
//
// One extra jeuInfos.php call per NEW game (then cached on SD forever).
// gameId: ScreenScraper numeric game id when known ("" -> identify by ROM
// hashes, exactly like searchWithJeuInfosPreciseJSON does).
// Returns true if at least one field was captured (partial data is fine).
//
// Algorithm validated end-to-end against real API captures (meta_by_crc.json
// and meta_by_gameid.json): typical read 4-8 KB of a 30-40 KB body, peak
// window under 5 KB, early stop at "medias".
// Flag discipline: metaFetchInProgress is cleared on EVERY return path.
// -----------------------------------------------------------------------------
bool fetchGameMetadataJSON(String gameId, String coreName,
                           RomDetails romDetails, GameMeta &out) {
  if (metaFetchInProgress) return false;
  metaFetchInProgress = true;

  if (ESP.getFreeHeap() < 100000) {
    Serial.printf("META: insufficient heap (%d)\n", ESP.getFreeHeap());
    metaFetchInProgress = false;
    return false;
  }

  String url = "https://api.screenscraper.fr/api2/jeuInfos.php";
  url += "?devid=" + String(SCREENSCRAPER_DEV_USER);
  url += "&devpassword=" + String(SCREENSCRAPER_DEV_PASS);
  url += "&softname=" + String(SCREENSCRAPER_SOFTWARE);
  url += "&output=json";
  url += "&ssid=" + urlEncode(String(SCREENSCRAPER_USER));
  url += "&sspassword=" + urlEncode(String(SCREENSCRAPER_PASS));
  if (gameId.length() > 0) {
    url += "&gameid=" + gameId;
  } else {
    String systemId = getScreenScraperSystemId(coreName);
    if (systemId.length() == 0) {
      Serial.printf("META: core '%s' not mapped, skipping\n", coreName.c_str());
      metaFetchInProgress = false;
      return false;
    }
    url += "&systemeid=" + systemId;
    url += "&romtype=rom";
    url += "&romnom=" + urlEncode(romDetails.filename);
    url += "&crc=" + romDetails.crc32;
    url += "&romtaille=" + String(romDetails.filesize);
    url += "&md5=" + romDetails.md5;
    url += "&sha1=";
  }
  Serial.printf("META fetch URL: %s\n", redactScreenScraperUrl(url).c_str());

  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);
  http.addHeader("User-Agent", "MiSTer-Monitor");
  http.addHeader("Accept", "application/json");

  int httpCode = http.GET();
  g_lastSSHttpCode = httpCode;
  if (httpCode != 200) {
    Serial.printf("META: HTTP %d\n", httpCode);
    http.end();
    metaFetchInProgress = false;
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();

  // ---- sliding window state --------------------------------------------------
  const size_t WIN_MAX   = 6144;   // window cap
  const size_t TAIL_KEEP = 512;    // overlap kept when trimming
  const size_t HARD_CAP  = 80000;  // absolute bytes read from the wire
  String win;
  win.reserve(WIN_MAX + 600);
  size_t totalRead = 0;
  unsigned long lastDataMs = millis();

  bool gotPub = false, gotDev = false, gotPlayers = false, gotRating = false;
  bool gotDates = false, gotGenres = false;

  // synopsis: 0 = searching language, 1 = copying text, 2 = done
  int  synState = 0;
  bool synEsc = false, synTakingEN = false, inSynopsisBlock = false;
  String synRaw, synRawEN;
  synRaw.reserve(_info_synopsis_max + 8);

  String datesBlock, genresBlock;
  bool inDates = false, inGenres = false;
  int  depth = 0;

  // Capture one bracketed array block (bounded, bracket-depth tracked).
  auto captureBlock = [&](const char *key, bool &inFlag, String &blk, bool &done) {
    if (done) return;
    if (!inFlag) {
      int b = findKeyArrayStart(win, key);
      if (b < 0) return;
      inFlag = true;
      depth = 0;
      win.remove(0, b);                 // window now starts at '['
    }
    int i = 0;
    while (i < (int)win.length()) {
      char c = win[i];
      i++;
      if (c == '[') depth++;
      if (c == ']') {
        depth--;
        if (depth == 0) { blk += c; done = true; inFlag = false; break; }
      }
      if ((int)blk.length() < 1500) blk += c;
    }
    win.remove(0, i);
  };

  char tmp[513];

  while (totalRead < HARD_CAP) {
    if (!http.connected() && stream->available() == 0) break;
    if (ESP.getFreeHeap() < 60000) { Serial.println("META: low heap, stop"); break; }
    if (millis() - lastDataMs > 8000) { Serial.println("META: read stall, stop"); break; }

    int avail = stream->available();
    if (avail <= 0) {
      M5.update();
      screenshotServer.handleClient();
      delay(20);
      continue;
    }
    int n = stream->readBytes(tmp, (avail > 512) ? 512 : avail);
    if (n <= 0) continue;
    tmp[n] = 0;
    lastDataMs = millis();
    totalRead += n;
    win += tmp;

    // ---- simple fields (early prefix of response.jeu) ------------------------
    if (!gotPub) {
      String v = extractTextAfterMarker(win, "editeur");
      if (v.length()) { out.publisher = jsonUnescapeAndFold(v); gotPub = true; }
    }
    if (!gotDev) {
      String v = extractTextAfterMarker(win, "developpeur");
      if (v.length()) { out.developer = jsonUnescapeAndFold(v); gotDev = true; }
    }
    if (!gotPlayers) {
      String v = extractTextAfterMarker(win, "joueurs");
      if (v.length()) { out.players = jsonUnescapeAndFold(v); gotPlayers = true; }
    }
    if (!gotRating) {
      String v = extractTextAfterMarker(win, "note");
      if (v.length()) { out.rating = jsonUnescapeAndFold(v) + "/20"; gotRating = true; }
    }

    // ---- synopsis: preferred language, fallback English ----------------------
    if (synState == 0) {
      if (!inSynopsisBlock && findKeyArrayStart(win, "synopsis") >= 0)
        inSynopsisBlock = true;
      if (inSynopsisBlock) {
        int lp = findKeyStringValue(win, "langue", _info_lang_str.c_str());
        int le = findKeyStringValue(win, "langue", "en");
        int use = (lp >= 0) ? lp
                            : ((le >= 0 && synRawEN.length() == 0) ? le : -1);
        if (use >= 0) {
          synTakingEN = (lp < 0);
          int q1 = findTextValueStart(win, use);
          if (q1 >= 0) {
            win.remove(0, q1);          // consume up to the opening quote
            synState = 1;
          }
        }
        // Synopsis region ended without a match -> give up on synopsis
        if (synState == 0 && (findKeyArrayStart(win, "classifications") >= 0 ||
                              findKeyArrayStart(win, "dates")  >= 0 ||
                              findKeyArrayStart(win, "genres") >= 0 ||
                              findKeyArrayStart(win, "medias") >= 0)) {
          synState = 2;
        }
      }
    }
    if (synState == 1) {
      // consume the whole window into the destination (window never grows)
      String &dst = synTakingEN ? synRawEN : synRaw;
      int i = 0;
      while (i < (int)win.length()) {
        char c = win[i];
        i++;
        if (synEsc) {
          if ((int)dst.length() < _info_synopsis_max) { dst += '\\'; dst += c; }
          synEsc = false;
          continue;
        }
        if (c == '\\') { synEsc = true; continue; }
        if (c == '"') {                 // synopsis text finished
          synState = (synTakingEN && _info_lang_str != "en") ? 0 : 2;
          break;
        }
        if ((int)dst.length() < _info_synopsis_max) dst += c;
      }
      win.remove(0, i);
      if (synState == 1) continue;      // still copying: read next chunk
    }

    // ---- dates[] and genres[] mini-blocks (never while copying synopsis) -----
    if (synState == 2 || !inSynopsisBlock) {
      captureBlock("dates",  inDates,  datesBlock,  gotDates);
      captureBlock("genres", inGenres, genresBlock, gotGenres);
    }

    // ---- stop / trim ----------------------------------------------------------
    if (findKeyArrayStart(win, "medias") >= 0) break;   // all fields are behind us
    if (gotPub && gotDev && gotPlayers && gotRating &&
        synState == 2 && gotDates && gotGenres) break;
    if (!inDates && !inGenres && synState != 1 && (size_t)win.length() > WIN_MAX)
      win.remove(0, win.length() - TAIL_KEEP);
  }
  http.end();

  // ---- post-process -----------------------------------------------------------
  if (synRaw.length() == 0 && synRawEN.length() > 0) synRaw = synRawEN;
  if (synRaw.length() > 0) {
    out.synopsis = jsonUnescapeAndFold(synRaw);
    if ((int)out.synopsis.length() >= _info_synopsis_max - 4) out.synopsis += "...";
  }

  // Year: region priority pref -> wor -> us -> eu -> jp (mirrors the artwork
  // region ordering); take the first 4 chars ("1990-07-02" -> "1990").
  if (datesBlock.length() > 0) {
    const char *ORDER[5];
    ORDER[0] = _boxart_region_str.c_str();
    ORDER[1] = "wor"; ORDER[2] = "us"; ORDER[3] = "eu"; ORDER[4] = "jp";
    for (int r = 0; r < 5 && out.year.length() == 0; r++) {
      int p = findKeyStringValue(datesBlock, "region", ORDER[r]);
      if (p < 0) continue;
      String v = extractTextAfterMarker(datesBlock.substring(p), "region");
      if (v.length() >= 4) out.year = v.substring(0, 4);
    }
    if (out.year.length() == 0) {       // any region at all
      String v = extractTextAfterMarker(datesBlock, "region");
      if (v.length() >= 4) out.year = v.substring(0, 4);
    }
  }

  // Genre: up to two genre names in the preferred language, fallback English.
  if (genresBlock.length() > 0) {
    int from = 0, taken = 0;
    while (taken < 2) {
      int p = findKeyStringValue(genresBlock, "langue", _info_lang_str.c_str(), from);
      if (p < 0 && taken == 0)
        p = findKeyStringValue(genresBlock, "langue", "en", from);
      if (p < 0) break;
      String v = extractTextAfterMarker(genresBlock.substring(p), "langue");
      if (v.length()) {
        String g = jsonUnescapeAndFold(v);
        if (out.genre.indexOf(g) < 0) {  // avoid duplicates across genres
          if (out.genre.length()) out.genre += " / ";
          out.genre += g;
          taken++;
        }
      }
      from = p + 10;
    }
  }

  bool any = out.year.length() || out.developer.length() || out.publisher.length() ||
             out.genre.length() || out.synopsis.length() || out.players.length();
  out.loaded = any;
  Serial.printf("META fetch done: read=%u heap=%d | y=%s dev='%s' pub='%s' pl=%s rt=%s gen='%s' syn=%d ch\n",
                (unsigned)totalRead, ESP.getFreeHeap(),
                out.year.c_str(), out.developer.c_str(), out.publisher.c_str(),
                out.players.c_str(), out.rating.c_str(), out.genre.c_str(),
                out.synopsis.length());
  metaFetchInProgress = false;
  return any;
}

// -----------------------------------------------------------------------------
// wrapTextToLines() — greedy word wrap by character count.
// ScaledDisplay does not expose textWidth(); the default GLCD font is a fixed
// 6 px per character per size unit, so wrapping by character count is exact.
// Words longer than a full line are hard-split (URLs, long romanised titles).
// Returns the number of lines written to `out` (never more than maxOut).
// -----------------------------------------------------------------------------
int wrapTextToLines(const String &text, int maxChars, String *out, int maxOut) {
  if (maxChars < 4 || maxOut < 1) return 0;
  int n = 0;
  String cur = "";
  int from = 0;
  while (from <= (int)text.length() && n < maxOut) {
    int sp = text.indexOf(' ', from);
    String word = (sp < 0) ? text.substring(from) : text.substring(from, sp);

    // Hard-split any word that cannot fit on a line by itself
    while ((int)word.length() > maxChars && n < maxOut) {
      if (cur.length()) { out[n++] = cur; cur = ""; if (n >= maxOut) break; }
      out[n++] = word.substring(0, maxChars);
      word = word.substring(maxChars);
    }
    if (n >= maxOut) return n;

    String cand = cur.length() ? cur + " " + word : word;
    if ((int)cand.length() <= maxChars) {
      cur = cand;
    } else {
      out[n++] = cur;
      cur = word;
    }
    if (sp < 0) break;
    from = sp + 1;
  }
  if (n < maxOut && cur.length()) out[n++] = cur;
  return n;
}

// -----------------------------------------------------------------------------
// drawWrappedText() — draw up to maxLines wrapped lines inside width w,
// appending "..." to the last line when the text does not fit.
// -----------------------------------------------------------------------------
void drawWrappedText(int x, int y, int w, int lineH, int maxLines, const String &text) {
  int maxChars = w / 6;                  // 6 px/char, default font, size 1
  if (maxChars < 8 || maxLines < 1) return;

  // One extra slot tells us whether the text overflowed the visible lines.
  const int CAP = 64;
  if (maxLines > CAP - 1) maxLines = CAP - 1;
  static String lines[CAP];
  int n = wrapTextToLines(text, maxChars, lines, maxLines + 1);

  int shown = (n > maxLines) ? maxLines : n;
  for (int i = 0; i < shown; i++) {
    String l = lines[i];
    if (i == shown - 1 && n > maxLines) {
      if ((int)l.length() > maxChars - 4) l = l.substring(0, maxChars - 4);
      l += " ...";
    }
    Lcd.setCursor(x, y + i * lineH);
    Lcd.print(l);
  }
  for (int i = 0; i < CAP; i++) lines[i] = "";   // release heap
}

// =============================================================================
// Synopsis subpage (2/2) — vertical line scroll
// =============================================================================
// At textSize(1.5) — the same size as the metadata grid — the GLCD font gives a
// 9 px advance and a 12 px cap height, so 300 px is 33 chars/line and the
// content band (y 82..190) fits 7 lines: ~231 visible characters against a
// stored synopsis of up to 2000. The text is therefore wrapped ONCE into a
// cached line array and a 7-line window scrolls down it, pausing at both ends.
// -----------------------------------------------------------------------------
// Line cache ceiling. info_synopsis_max clamps at 2000 chars; at 33 chars per
// line that is ~61 lines before word breaks, so 100 leaves real headroom.
#define GAMEINFO_SYN_MAX_LINES 100
static String gameInfoSynLines[GAMEINFO_SYN_MAX_LINES];
static int    gameInfoSynLineCount = 0;
static String gameInfoSynCachedFor = "";

static const int SYN_X      = 10;
static const int SYN_TOP    = 82;
static const int SYN_LINEH  = 16;   // 12 px glyph at size 1.5 + 4 px leading
static const int SYN_VIS    = 7;    // visible lines: (194 - 82) / 16
static const int SYN_CHARS  = 33;   // 300 px / (6 px * 1.5)

// Wrap the current synopsis into the line cache (idempotent).
static void buildGameInfoSynLines() {
  if (gameInfoSynCachedFor == currentMeta.synopsis) return;
  for (int i = 0; i < GAMEINFO_SYN_MAX_LINES; i++) gameInfoSynLines[i] = "";
  gameInfoSynLineCount = wrapTextToLines(currentMeta.synopsis, SYN_CHARS,
                                         gameInfoSynLines, GAMEINFO_SYN_MAX_LINES);
  gameInfoSynCachedFor = currentMeta.synopsis;
  Serial.printf("META: synopsis wrapped into %d lines\n", gameInfoSynLineCount);
}

// True when the synopsis is taller than the visible window.
bool gameInfoSynNeedsScroll() {
  buildGameInfoSynLines();
  return gameInfoSynLineCount > SYN_VIS;
}

// Rewind to the top and pause there (called on every subpage/page entry).
void resetGameInfoSynScroll() {
  gameInfoSynScroll     = 0;
  gameInfoSynScrollTime = millis();
  gameInfoSynCycledTime = 0;
  gameInfoSynPaused     = true;
  gameInfoSynCycled     = false;
  gameInfoForceExit     = false;
}

// -----------------------------------------------------------------------------
// drawGameInfoSynopsis() — paint the visible window.
// Every one of the SYN_VIS rows is printed padded to SYN_CHARS with an explicit
// background colour, so a scroll step overwrites the previous frame exactly and
// no fillRect (hence no flicker) is needed.
// -----------------------------------------------------------------------------
void drawGameInfoSynopsis() {
  buildGameInfoSynLines();

  int maxTop = gameInfoSynLineCount - SYN_VIS;
  if (maxTop < 0) maxTop = 0;
  if (gameInfoSynScroll > maxTop) gameInfoSynScroll = maxTop;

  Lcd.setTextWrap(false);
  Lcd.setTextColor(THEME_WHITE, THEME_BLACK);
  Lcd.setTextSize(1.5);
  for (int i = 0; i < SYN_VIS; i++) {
    int li = gameInfoSynScroll + i;
    String l = (li < gameInfoSynLineCount) ? gameInfoSynLines[li] : String("");
    while ((int)l.length() < SYN_CHARS) l += ' ';
    Lcd.setCursor(SYN_X, SYN_TOP + i * SYN_LINEH);
    Lcd.print(l);
  }
}

// -----------------------------------------------------------------------------
// tickGameInfoSynScroll() — advance one line per GAMEINFO_SYN_STEP_MS, holding
// GAMEINFO_SYN_PAUSE_MS at the top and at the bottom.
//
// When the bottom hold expires the window FREEZES on the last lines and
// gameInfoSynCycled is set (stamping gameInfoSynCycledTime). It deliberately
// does not rewind: the loop then waits GAMEINFO_SYN_EXIT_MS more before leaving
// the panel, and that extra time must be spent looking at the END of the text,
// which is what the reader is still finishing.
// -----------------------------------------------------------------------------
void tickGameInfoSynScroll() {
  if (gameInfoSynCycled) return;            // finished: frozen at the bottom

  // The line cache MUST be current before maxTop is computed. handleTouch()
  // runs earlier in the loop than this block, so a tap on the subpage toggle
  // lands here in the same iteration — before any redraw has had the chance to
  // build the lines. With a stale count (zero on the first game after boot)
  // maxTop would come out negative, the scroll would be declared finished on
  // the spot, and the panel would bounce back to the game image without ever
  // scrolling. buildGameInfoSynLines() is idempotent, so this costs a string
  // comparison on every other call.
  buildGameInfoSynLines();

  int maxTop = gameInfoSynLineCount - SYN_VIS;
  if (maxTop < 1) {                         // fits entirely: nothing to scroll
    gameInfoSynCycled     = true;
    gameInfoSynCycledTime = millis();
    return;
  }

  unsigned long now = millis();

  if (gameInfoSynPaused) {
    if (now - gameInfoSynScrollTime < GAMEINFO_SYN_PAUSE_MS) return;
    if (gameInfoSynScroll >= maxTop) {      // the bottom hold just finished
      gameInfoSynCycled     = true;         // stay on the last lines
      gameInfoSynCycledTime = now;
      return;
    }
    gameInfoSynPaused     = false;          // the top hold just finished
    gameInfoSynScrollTime = now;
    return;
  }

  if (now - gameInfoSynScrollTime >= GAMEINFO_SYN_STEP_MS) {
    gameInfoSynScroll++;
    gameInfoSynScrollTime = now;
    if (gameInfoSynScroll >= maxTop) {      // reached the end: hold there
      gameInfoSynScroll = maxTop;
      gameInfoSynPaused = true;
    }
    drawGameInfoSynopsis();
  }
}

// -----------------------------------------------------------------------------
// displayGameInfo() — page 5 (GAME INFO / NOW PLAYING)
//
// Self-healing metadata resolution on every draw:
//  1. game changed -> reset + try the .meta sidecar next to the artwork
//  2. no sidecar -> ONE lazy fetch attempt per game (needs a valid CRC and
//     no download in progress), then the sidecar is written for next time
// On Tab5 each page draws its own chrome: the frame02.jpg background (once,
// via backgroundLoaded), then its content in the logical 320x240 space, then
// drawPageIndicators() and drawFooter() — displayGameInfo() does the same.
// -----------------------------------------------------------------------------
void displayGameInfo() {
  // ========== LOAD FULL SCREEN FRAME IMAGE (only if needed) ==========
  if (!backgroundLoaded) {
    M5.Display.fillScreen(THEME_BLACK);
    loadFullScreenFrame("/cores/frame02.jpg");
    delay(50);
    drawMisterLogoRightPanel();
    btnPrev.draw();
    btnScan.draw();
    btnNext.draw();
    backgroundLoaded = true;
  }

  // ========== CLEAR CONTENT AREA ONLY ==========
  Lcd.fillRect(0, 35, 320, 180, THEME_BLACK);

  bool haveGame = (currentGame.length() > 0);

  // ---- 1. game changed: reset and try the sidecar -----------------------------
  if (haveGame && currentMeta.forGame != currentGame) {
    currentMeta = GameMeta();
    currentMeta.forGame = currentGame;
    String imgPath;
    if (findGameImageExact(currentCore, currentGame, imgPath)) {
      loadGameMeta(getMetaPathFromImagePath(imgPath), currentMeta);
    }
  }

  // ---- 2. no sidecar: one lazy fetch attempt per game -------------------------
  if (haveGame && !currentMeta.loaded &&
      metaFetchAttemptedFor != currentGame &&
      !downloadInProgress && !metaFetchInProgress) {
    RomDetails rd = getCurrentRomDetails();

    // This is the only place that queries ROM details for a game whose artwork
    // came from the SD cache, so record the verdict for gameInfoAvailable().
    lastRomHasCrc     = rd.available && rd.hashCalculated && rd.crc32.length() > 0;
    lastRomCrcChecked = true;

    if (rd.available && rd.hashCalculated && rd.crc32.length() > 0) {
      metaFetchAttemptedFor = currentGame;
      Lcd.setTextColor(THEME_CYAN);
      Lcd.setTextSize(1);
      Lcd.setCursor(10, 70);
      Lcd.print("FETCHING GAME INFO...");
      GameMeta m;
      if (fetchGameMetadataJSON("", currentCore, rd, m)) {   // "" = identify by CRC
        m.forGame = currentGame;
        currentMeta = m;
        String imgPath2;
        if (findGameImageExact(currentCore, currentGame, imgPath2)) {
          saveGameMeta(getMetaPathFromImagePath(imgPath2), m);
        }
      }
      Lcd.fillRect(0, 35, 320, 180, THEME_BLACK);            // redraw clean
    }
  }

  // ---- header: game title (scrolls horizontally when too long) ----------------
  // Scroll state is (re)initialised only when the underlying text changes, so
  // the scroll position survives redraws (subpage flips, animation ticks).
  // The visible window is padded to maxChars so the painted pixel width is
  // constant, which is what makes setTextColor(fg, bg) flicker-free here.
  String title = haveGame ? currentGame : String("NO GAME LOADED");
  const int titleChars = 18;   // 216 px; the rest of the row is the indicator
  if (gameInfoTitleScroll.fullText != title ||
      gameInfoTitleScroll.maxChars != titleChars) {
    initScrollText(&gameInfoTitleScroll, title, titleChars);
  }
  String titleShown = getScrolledText(&gameInfoTitleScroll);
  while ((int)titleShown.length() < titleChars) titleShown += ' ';

  Lcd.setTextWrap(false);
  Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
  Lcd.setTextSize(2);
  Lcd.setCursor(10, 40);
  Lcd.print(titleShown);
  Lcd.drawFastHLine(10, 58, 300, THEME_CYAN);

  if (!haveGame || !currentMeta.loaded) {
    gameInfoSubPage = 0;                 // no second subpage without metadata
    Lcd.setTextColor(THEME_GRAY);
    Lcd.setTextSize(1);
    Lcd.setCursor(10, 68);
    Lcd.print(haveGame ? "NO METADATA AVAILABLE" : "LOAD A GAME ON THE MISTER");
    if (haveGame) {
      Lcd.setCursor(10, 82);
      Lcd.print("(game not identified on ScreenScraper)");
    }
    drawPageIndicators();
    drawFooter();
    return;
  }

  // Only offer a synopsis subpage when there actually is a synopsis.
  bool hasSynopsis = (currentMeta.synopsis.length() > 0);
  if (gameInfoSubPage == 1 && !hasSynopsis) gameInfoSubPage = 0;

  // ---- subpage indicator / toggle (top-right of the title row, tappable) ------
  // 6 chars at size 2 = 72 px, right-aligned to x=310. No frame: the arrows
  // carry the affordance. Hitbox in the touch handler: x>=230, y 36..60.
  if (hasSynopsis) {
    Lcd.setTextColor(THEME_CYAN, THEME_BLACK);
    Lcd.setTextSize(2);
    Lcd.setCursor(238, 40);
    Lcd.print(gameInfoSubPage == 0 ? "1/2>>" : "<<2/2");
  }

  if (gameInfoSubPage == 0) {
    // ---- SUBPAGE 1/2: metadata fields -----------------------------------------
    // size 1.5: glyph advance 9 px, height 12 px. Label column 10..73
    // ("PLAYERS" = 7 x 9 px), value column from x=80 with 25 chars of room.
    // Rows with an empty value are skipped, so the block height depends on the
    // game: count the visible rows first, then centre them vertically in the
    // band between the title rule (y=58) and the footer band (y=205).
    struct MetaRow { const char *label; String *val; };
    MetaRow rows[] = {
      { "YEAR",    &currentMeta.year      },
      { "DEV",     &currentMeta.developer },
      { "PUB",     &currentMeta.publisher },
      { "GENRE",   &currentMeta.genre     },
      { "PLAYERS", &currentMeta.players   },
      { "RATING",  &currentMeta.rating    },
    };
    const int BAND_TOP = 64, BAND_BOT = 200;   // usable content band
    const int ROW_STEP = 20, ROW_H = 12;       // row pitch, glyph height at 1.5

    int visible = 0;
    for (int r = 0; r < 6; r++) if (rows[r].val->length() > 0) visible++;

    // Block height = n pitches minus the trailing gap below the last row.
    int blockH = visible * ROW_STEP - (ROW_STEP - ROW_H);
    int y = BAND_TOP + ((BAND_BOT - BAND_TOP) - blockH) / 2;
    if (y < BAND_TOP) y = BAND_TOP;

    Lcd.setTextWrap(false);
    for (int r = 0; r < 6; r++) {
      gameInfoRowShown[r] = (rows[r].val->length() > 0);
      if (!gameInfoRowShown[r]) continue;

      Lcd.setTextColor(THEME_CYAN, THEME_BLACK);
      Lcd.setTextSize(1.5);
      Lcd.setCursor(10, y);
      Lcd.print(rows[r].label);

      // Value: scrolls horizontally when longer than the column. Padding to a
      // constant width keeps the painted pixel width stable, which is what
      // makes setTextColor(fg, bg) flicker-free on every scroll step.
      if (gameInfoRowScroll[r].fullText != *rows[r].val ||
          gameInfoRowScroll[r].maxChars != GI_VAL_CHARS) {
        initScrollText(&gameInfoRowScroll[r], *rows[r].val, GI_VAL_CHARS);
      }
      String v = getScrolledText(&gameInfoRowScroll[r]);
      while ((int)v.length() < GI_VAL_CHARS) v += ' ';

      Lcd.setTextColor(THEME_WHITE, THEME_BLACK);
      Lcd.setCursor(GI_VAL_X, y);
      Lcd.print(v);

      gameInfoRowY[r] = y;
      y += ROW_STEP;
    }
  } else {
    // ---- SUBPAGE 2/2: synopsis (wrapped, vertical auto-scroll) ----------------
    Lcd.setTextColor(THEME_CYAN);
    Lcd.setTextSize(1);
    Lcd.setCursor(10, 66);
    Lcd.print("SYNOPSIS");
    drawGameInfoSynopsis();
  }

  drawPageIndicators();
  drawFooter();
}

String getPageTitle() {
  switch(currentPage) {
    case 0: return "MAIN HUD";
    case 1: return "SYS MONITOR";
    case 2: return "STORAGE";
    case 3: return "NETWORK";
    case 4: return "DEVICES";
    case 5: return "GAME INFO";
    default: return "SYSTEM";
  }
}

String getPageSubtitle() {
  switch(currentPage) {
    case 0: return "CORE STATUS";
    case 1: return "PERFORMANCE";
    case 2: return "DISK ARRAY";
    case 3: return "TERMINAL";
    case 4: return "SCANNER";
    case 5: return "NOW PLAYING";
    default: return "ONLINE";
  }
}

float extractFloatValue(String json, String key) {
  String searchKey = "\"" + key + "\":";
  int index = json.indexOf(searchKey);
  if (index == -1) return 0.0;
  
  int start = index + searchKey.length();
  int end = json.indexOf(",", start);
  if (end == -1) end = json.indexOf("}", start);
  
  return json.substring(start, end).toFloat();
}

int extractIntValue(String json, String key) {
  String searchKey = "\"" + key + "\":";
  int index = json.indexOf(searchKey);
  if (index == -1) return 0;
  
  int start = index + searchKey.length();
  int end = json.indexOf(",", start);
  if (end == -1) end = json.indexOf("}", start);
  
  return json.substring(start, end).toInt();
}

String extractStringValue(String json, String key) {
  String searchKey = "\"" + key + "\":\"";
  int index = json.indexOf(searchKey);
  if (index == -1) {
    // Try without quotes (for non-string values that are treated as strings)
    searchKey = "\"" + key + "\":";
    index = json.indexOf(searchKey);
    if (index == -1) return "N/A";
    
    int start = index + searchKey.length();
    // Skip whitespace
    while (start < json.length() && json.charAt(start) == ' ') {
      start++;
    }
    
    // Check if it starts with quote (string value)
    if (json.charAt(start) == '"') {
      start++; // Skip opening quote
      int end = json.indexOf("\"", start);
      if (end == -1) return "N/A";
      return json.substring(start, end);
    } else {
      // Non-quoted value (number, boolean, etc.) - read until comma, }, or space
      int end = start;
      while (end < json.length() && 
             json.charAt(end) != ',' && 
             json.charAt(end) != '}' && 
             json.charAt(end) != ' ' &&
             json.charAt(end) != '\n' &&
             json.charAt(end) != '\r') {
        end++;
      }
      if (end > start) {
        return json.substring(start, end);
      }
      return "N/A";
    }
  }
  
  // Original logic for quoted strings
  int start = index + searchKey.length();
  int end = json.indexOf("\"", start);
  if (end == -1) return "N/A";
  
  return json.substring(start, end);
}

bool extractBoolValue(String json, String key) {
  String searchKey = "\"" + key + "\":";
  int index = json.indexOf(searchKey);
  if (index == -1) return false;
  
  int start = index + searchKey.length();
  // Skip whitespace
  while (start < json.length() && json.charAt(start) == ' ') {
    start++;
  }
  
  // Check if it's true or false
  if (json.substring(start, start + 4) == "true") {
    return true;
  }
  return false;
}

bool isValidHash(String hash, String type) {
  hash.trim();
  
  if (hash.length() == 0 || hash == "N/A") {
    return false;
  }
  
  if (type == "crc32" && hash.length() != 8) {
    return false;
  }
  
  if (type == "md5" && hash.length() != 32) {
    return false;
  }
  
  if (type == "sha1" && hash.length() != 40) {
    return false;
  }
  
  // Check if all characters are hexadecimal
  for (int i = 0; i < hash.length(); i++) {
    char c = hash.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  
  return true;
}

void showSDCardError() {
  Lcd.fillScreen(THEME_BLACK);
  
  Lcd.setTextColor(THEME_RED);
  Lcd.setTextSize(2);
  Lcd.setCursor(80, 60);
  Lcd.print("SD CARD");
  Lcd.setCursor(90, 80);
  Lcd.print("ERROR");
  
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(40, 110);
  Lcd.print("Check SD card:");
  Lcd.setCursor(40, 125);
  Lcd.print("1. Card inserted properly");
  Lcd.setCursor(40, 140);
  Lcd.print("2. Formatted as FAT32");
  Lcd.setCursor(40, 155);
  Lcd.print("3. Create /cores folder");
  Lcd.setCursor(40, 170);
  Lcd.print("4. Add 320x240 JPG images");
  
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setCursor(40, 200);
  Lcd.print("Continuing without core images...");
  
  delay(4000);
}

void showImageNotFound(String coreName) {
  Lcd.fillScreen(THEME_BLACK);
  
  // Frame
  Lcd.drawRect(20, 40, 280, 160, THEME_ORANGE);
  Lcd.drawRect(21, 41, 278, 158, THEME_ORANGE);
  
  // "Image not found" icon
  Lcd.setTextColor(THEME_ORANGE);
  Lcd.setTextSize(4);
  Lcd.setCursor(140, 80);
  Lcd.print("?");
  
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(110, 110);
  Lcd.print("IMAGE NOT FOUND");
  
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setCursor(130, 130);
  Lcd.print("CORE:");
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setCursor(100, 145);
  String displayCore = coreName.length() > 12 ? coreName.substring(0, 12) : coreName;
  String coreDisplay = displayCore;
    if (coreDisplay.equalsIgnoreCase("arcade")) {
      coreDisplay = "Arcade";
    }
    Lcd.print(coreDisplay);
  
  // Additional info
  Lcd.setTextColor(THEME_GRAY);
  Lcd.setTextSize(1);
  Lcd.setCursor(40, 170);
  Lcd.printf("Expected: %s/A/*.jpg or /#/*.jpg", CORE_IMAGES_PATH);
  
  // Header and footer
  drawMiSTerLogo(10, 5);
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(120, 15);
  Lcd.print("CORE IMAGE SYSTEM");
  
  drawStatusIndicator(300, 15, connected ? THEME_GREEN : THEME_RED, connected);
  
  Lcd.setTextColor(THEME_GREEN);
  Lcd.setCursor(60, 225);
  Lcd.print("Press any button to continue");
}

bool findGameImageExact(String coreName, String gameName, String &imagePath) {
  if (!sdCardAvailable) {
    Serial.println("SD not available");
    return false;
  }
  
  // Use the exact game name as it comes from the API
  String exactFileName = getExactFileName(gameName);
  String searchCore = coreName;
  searchCore.toLowerCase();
  // Sanitize after lowercasing — '/' in friendly names breaks SD paths.
  searchCore = sanitizeCoreFilename(searchCore);
  
  bool isArcade = isArcadeCore(coreName);
  
  Serial.printf("\n=== Searching %sEXACT game image ===\n", isArcade ? "ARCADE " : "");
  Serial.printf("Core: '%s' | Exact filename: '%s'\n", searchCore.c_str(), exactFileName.c_str());
  
  // List of supported extensions
  String extensions[] = {".jpg", ".jpeg", ".JPG", ".JPEG"};
  
  // Function to try different paths
  auto tryPath = [&](String basePath, String name) -> bool {
    Serial.printf("Trying in directory: %s\n", basePath.c_str());
    
    for (String ext : extensions) {
      String fullPath = basePath + "/" + name + ext;
      Serial.printf("  Checking: %s\n", fullPath.c_str());
      
      if (SD.exists(fullPath)) {
        File testFile = SD.open(fullPath);
        if (testFile && testFile.size() > 0) {
          imagePath = fullPath;
          testFile.close();
          Serial.printf("EXACT image found: %s (%d bytes)\n", 
                       isArcade ? "ARCADE " : "", fullPath.c_str(), testFile.size());
          return true;
        }
        if (testFile) testFile.close();
      }
    }
    return false;
  };
  
  if (isArcade && ENABLE_ALPHABETICAL_FOLDERS) {
    // SPECIAL SEARCH FOR ARCADE: /cores/A/game.jpg
    Serial.println("ARCADE: Searching in alphabetical structure by game name");
    
    if (exactFileName.length() > 0) {
      char firstChar = exactFileName.charAt(0);
      String alphabetPath;
      
      // Determine alphabetical folder based on the first letter of the game
      if (firstChar >= '0' && firstChar <= '9') {
        alphabetPath = String(CORE_IMAGES_PATH) + "/#";
      } else if (firstChar >= 'a' && firstChar <= 'z') {
        alphabetPath = String(CORE_IMAGES_PATH) + "/" + String((char)(firstChar - 32));
      } else if (firstChar >= 'A' && firstChar <= 'Z') {
        alphabetPath = String(CORE_IMAGES_PATH) + "/" + String(firstChar);
      } else {
        alphabetPath = String(CORE_IMAGES_PATH) + "/#";
      }
      
      Serial.printf("ARCADE alphabetical search: %s\n", alphabetPath.c_str());
      if (tryPath(alphabetPath, exactFileName)) return true;
    }
  } else if (ENABLE_ALPHABETICAL_FOLDERS && searchCore.length() > 0) {
    // STANDARD SEARCH: /cores/A/corename/game.jpg
    String alphabetPath = getAlphabeticalPath(searchCore);
    String gamePath = alphabetPath + "/" + searchCore;
    
    Serial.printf("1. Searching in alphabetical structure: %s\n", gamePath.c_str());
    if (tryPath(gamePath, exactFileName)) return true;
  }
  
  // Search in direct structure: /cores/corename/exactgamename.jpg
  String directPath = String(CORE_IMAGES_PATH) + "/" + searchCore;
  Serial.printf("2. Searching in direct structure: %s\n", directPath.c_str());
  if (tryPath(directPath, exactFileName)) return true;
  
  Serial.printf("No %sEXACT image found for: '%s'\n", 
               isArcade ? "ARCADE " : "", gameName.c_str());
  return false;
}

// ========== FUNCTION TO GET EXACT FILENAME ==========

String getExactFileName(String gameName) {
  String exactName = gameName;
  
  // Only remove characters that are NOT valid for filenames
  // KEEP everything else exactly the same
  exactName.replace("/", "_");
  exactName.replace("\\", "_");
  exactName.replace(":", "_");
  exactName.replace("*", "_");
  exactName.replace("?", "_");
  exactName.replace("\"", "_");
  exactName.replace("<", "_");
  exactName.replace(">", "_");
  exactName.replace("|", "_");
  
  // DO NOT remove parentheses, brackets, spaces, etc.
  // User wants exact name!
  
  return exactName;
}

// ========== SANITIZE CORE NAME FOR USE AS FILE/DIR COMPONENT ==========
// MiSTer friendly core names sometimes contain '/' (e.g. "Nintendo NES/Famicom",
// "Sega Genesis/Mega Drive", "TurboGrafx-16/PC Engine"). Using them as-is in
// SD paths creates phantom subdirectories that mkdir() cannot create
// recursively, breaking both image lookup and download save.
//
// Mirror getExactFileName() but for core names. Keep visual look intact;
// only replace path-hostile characters with '_'.
String sanitizeCoreFilename(String name) {
  String safe = name;
  safe.replace("/",  "_");
  safe.replace("\\", "_");
  safe.replace(":",  "_");
  safe.replace("*",  "_");
  safe.replace("?",  "_");
  safe.replace("\"", "_");
  safe.replace("<",  "_");
  safe.replace(">",  "_");
  safe.replace("|",  "_");
  return safe;
}

// ========== MISTER TO SCREENSCRAPER SYSTEM MAPPING ==========

String getScreenScraperSystemId(String coreName) {
  String core = coreName;
  
  Serial.printf("Mapping MiSTer core '%s' to ScreenScraper system ID\n", core.c_str());
  
  // === EXACT NAME-BASED MAPPING (returned by /status/core) ===
  
  // Nintendo systems
  if (core == "Nintendo Entertainment System" || core == "Nintendo NES/Famicom") return "3";
  if (core == "Super Nintendo Entertainment System" || core == "Super Nintendo" || core == "Super Nintendo/Super Famicom") return "4";
  if (core == "Nintendo 64") return "14";
  if (core == "Nintendo Game Boy" || core == "Game Boy") return "9";
  if (core == "Nintendo Game Boy Color" || core == "Game Boy Color") return "10";
  if (core == "Nintendo Game Boy Advance" || core == "Game Boy Advance" || core == "Nintendo Game Boy Advance 2P") return "12";
  if (core == "Famicom Disk System" || core == "Family Computer Disk System") return "106";
  if (core == "Nintendo Super Game Boy" || core == "Super Game Boy") return "127";
  if (core == "Nintendo Game & Watch" || core == "Game & Watch") return "52";
  if (core == "Virtual Boy") return "11";
  
  // Sega systems
  if (core == "Sega Genesis/Mega Drive" || core == "Megadrive") return "1";
  if (core == "Megadrive 32X" || core == "Sega Genesis/Megadrive 32X") return "19";
  if (core == "Sega Master System" || core == "Master System") return "2";
  if (core == "Sega Game Gear" || core == "Game Gear") return "21";
  if (core == "Sega Saturn" || core == "Saturn") return "22";
  if (core == "Sega Mega-CD" || core == "Sega CD/Mega CD" || core == "MegaCD") return "20";
  if (core == "Sega SG-1000" || core == "SG-1000") return "109";
  
  // Sony systems
  if (core == "PlayStation" || core == "Sony PlayStation") return "57";
  
  // PC Engine / TurboGrafx
  if (core == "TurboGrafx-16/PC Engine" || core == "PC Engine") return "31";
  if (core == "PC Engine CD-Rom" || core == "TurboGrafx-16/PC Engine CD-Rom") return "114";
  if (core == "PC Engine SuperGrafx" || core == "SuperGrafx") return "105";
  
  // Neo-Geo
  if (core == "Neo-Geo") return "142";
  if (core == "Neo-Geo CD") return "70";
  if (core == "Neo Geo Pocket" || core == "Neo-Geo Pocket") return "25";
  if (core == "Neo Geo Pocket Color" || core == "Neo-Geo Pocket Color") return "82";
  
  // Arcade — accept all known aliases that may arrive from server or SAM
  if (core == "Arcade" ||
      core == "mame"   ||
      core == "MAME"   ||
      core == "Multiple Arcade Machine Emulator") return "75";
  
  // Atari systems
  if (core == "Atari 2600") return "26";
  if (core == "Atari 5200") return "40";
  if (core == "Atari 7800") return "41";
  if (core == "Atari Lynx") return "28";
  if (core == "Atari Jaguar" || core == "Jaguar") return "27";
  if (core == "Atari ST/STE" || core == "Atari ST") return "42";
  if (core == "Atari 8bit") return "43";
  
  // Commodore / Amiga
  if (core == "Commodore Amiga") return "64";
  if (core == "Amiga CD32") return "130";
  if (core == "Commodore 64" || core == "Commodore 128") return "66";
  if (core == "Vic-20" || core == "Commodore VIC-20" || core == "Commodore Vic-20") return "73";
  if (core == "PET" || core == "Commodore PET") return "240";
  if (core == "C16") return "99";
  
  // PC / DOS
  if (core == "PC Dos") return "135";
  
  // British micros
  if (core == "ZX Spectrum") return "76";
  if (core == "ZX81") return "77";
  if (core == "Amstrad CPC" || core == "Amstrad GX4000" || core == "CPC") return "65";
  if (core == "Acorn Electron" || core == "Electron") return "85";
  if (core == "Acorn Atom" || core == "Atom") return "36";
  if (core == "Acorn Archimedes" || core == "Archimedes") return "84";
  if (core == "BBC Micro") return "37";
  if (core == "MGT SAM Coup\xc3\xa9" || core == "SAM Coup\xc3\xa9") return "213";
  
  // MSX
  if (core == "MSX" || core == "MSX1") return "113";
  if (core == "MSX2 Computer" || core == "MSX2") return "116";
  if (core == "MSX2+ Computer" || core == "MSX2Plus") return "116";
  
  // Other
  if (core == "BK") return "93";
  if (core == "EG2000 Colour Genie") return "92";
  if (core == "Camputers Lynx") return "88";
  if (core == "NEC PC-8801") return "221";
  if (core == "PC-9801") return "120";
  if (core == "FM-7") return "97";
  if (core == "Spectravideo SVI-328") return "218";
  if (core == "TI-99/4A") return "205";
  if (core == "Sharp X68000") return "79";
  
  // Apple
  if (core == "Apple II") return "86";
  if (core == "Macintosh Plus" || core == "Mac OS") return "146";
  
  // Misc consoles / handhelds
  if (core == "Vectrex") return "102";
  if (core == "Intellivision") return "115";
  if (core == "Colecovision") return "48";
  if (core == "WonderSwan") return "45";
  if (core == "WonderSwan Color" || core == "WonderSwanColor") return "46";
  if (core == "Oric 1 / Atmos") return "131";
  if (core == "Videopac G7000" || core == "Videopac G7000/Odyssey 2") return "104";
  if (core == "CreatiVision") return "241";
  if (core == "Channel F") return "80";
  if (core == "Astrocade") return "44";
  if (core == "Arcadia 2001") return "94";
  if (core == "Adventure Vision") return "78";
  if (core == "Adam") return "89";
  if (core == "PV-1000" || core == "Casio PV-1000") return "74";
  if (core == "CD-i" || core == "Philips CD-i" || core == "Phillips CD-i") return "133";
  if (core == "3DO" || core == "Panasonic 3DO") return "29";
  if (core == "Super Cassette Vision" || core == "SCV") return "67";
  if (core == "Gamate" || core == "Bit Corporation Gamate") return "266";
  if (core == "Mega Duck") return "90";
  if (core == "Pocket Challenge V2") return "237";
  if (core == "Pokemon Mini") return "211";
  if (core == "Watara Supervision") return "207";
  if (core == "VC 4000" || core == "Interton VC 4000") return "281";
  
  // TRS-80 / Tandy systems
  if (core == "TRS-80 Color Computer" ||
      core == "TRS-80 Color Computer 2" ||
      core == "TRS-80 Color Computer 3") return "144";
  
  // Special cases
  if (core == "Menu") return "";
  
  // === FALLBACK: lowercase variants and raw CORENAMEs ===
  // Used when the server returns an unmapped CORENAME directly, or for
  // alternate spellings from other sources.
  String coreLower = core;
  coreLower.toLowerCase();
  
  if (coreLower == "nes" || coreLower == "nintendo") return "3";
  if (coreLower == "snes" || coreLower == "supernintendo") return "4";
  if (coreLower == "n64" || coreLower == "nintendo64") return "14";
  if (coreLower == "gameboy" || coreLower == "gb") return "9";
  if (coreLower == "gbc" || coreLower == "gameboycolor") return "10";
  if (coreLower == "gba" || coreLower == "gameboyadvance") return "12";
  if (coreLower == "fds") return "106";
  if (coreLower == "sgb") return "127";
  if (coreLower == "genesis" || coreLower == "megadrive" || coreLower == "md") return "1";
  if (coreLower == "s32x") return "19";
  if (coreLower == "mastersystem" || coreLower == "sms") return "2";
  if (coreLower == "gg") return "21";
  if (coreLower == "saturn") return "22";
  if (coreLower == "megacd" || coreLower == "segacd") return "20";
  if (coreLower == "psx" || coreLower == "playstation") return "57";
  if (coreLower == "tgfx16" || coreLower == "pcengine" ||
      coreLower == "turbografx16") return "31";
  if (coreLower == "neogeo" || coreLower == "neo-geo") return "142";
  if (coreLower == "arcade" || coreLower == "mame" ||
      coreLower == "multiple arcade machine emulator") return "75";
  if (coreLower == "atari2600") return "26";
  if (coreLower == "atari5200") return "40";
  if (coreLower == "atari7800") return "41";
  if (coreLower == "atarilynx") return "28";   // Camputers Lynx is "lynx48"
  if (coreLower == "atarist") return "42";
  if (coreLower == "amiga" || coreLower == "minimig") return "64";
  if (coreLower == "amigacd32") return "130";
  if (coreLower == "c64" || coreLower == "commodore64" || coreLower == "c128") return "66";
  if (coreLower == "ao486" || coreLower == "pc dos" || coreLower == "pcxt") return "135";
  if (coreLower == "amstrad" || coreLower == "cpc") return "65";
  if (coreLower == "sam" || coreLower == "samcoupe") return "213";
  if (coreLower == "x68000") return "79";
  if (coreLower == "wonderswan") return "45";
  if (coreLower == "wonderswancolor") return "46";
  if (coreLower == "vectrex") return "102";
  if (coreLower == "coleco") return "48";
  if (coreLower == "intellivision") return "115";
  if (coreLower == "3do") return "29";
  if (coreLower == "supergrafx") return "105";
  if (coreLower == "ngp") return "25";
  if (coreLower == "ngpc") return "82";
  if (coreLower == "gba2p") return "12";
  if (coreLower == "scv") return "67";
  if (coreLower == "jaguar") return "27";
  if (coreLower == "scv") return "67";
  if (coreLower == "jaguar") return "27";
  if (coreLower == "menu" || coreLower == "main") return "";
  
  Serial.printf("Core '%s' not mapped to ScreenScraper system\n", coreName.c_str());
  return ""; // Unsupported system
}

// ========== IMAGE DOWNLOAD WITH AUTOMATIC RESIZING ==========

bool downloadImageFromScreenScraper(String imageUrl, String savePath) {
  // Add resizing parameters to ScreenScraper URL
  String resizedUrl = imageUrl;
  if (resizedUrl.indexOf("?") == -1) {
    resizedUrl += "?";
  } else {
    resizedUrl += "&";
  }
  
  // ScreenScraper can resize automatically
  resizedUrl += "maxwidth=" + String(TARGET_WIDTH);
  resizedUrl += "&maxheight=" + String(IMAGE_AREA_HEIGHT);  // Use 645 instead of 720
  resizedUrl += "&outputformat=jpg";
  
  Serial.printf("Downloading resized image: %s\n", redactScreenScraperUrl(resizedUrl).c_str());
  
  HTTPClient http;
  http.begin(resizedUrl);
  http.setTimeout(DOWNLOAD_TIMEOUT);
  http.addHeader("User-Agent", SCREENSCRAPER_SOFTWARE);
  
  int httpCode = http.GET();
  g_lastSSHttpCode = httpCode;

  if (httpCode != 200) {
    Serial.printf("Download failed: %d\n", httpCode);
    showDownloadProgress(50, ssHudMessage(httpCode));
    delay(2000);
    http.end();
    return false;
  }
  
  int contentLength = http.getSize();
  if (contentLength > MAX_IMAGE_SIZE) {
    Serial.printf("Image too large: %d bytes (max %d)\n", contentLength, MAX_IMAGE_SIZE);
    http.end();
    return false;
  }
  
  if (contentLength <= 0) {
    Serial.println("Invalid content length");
    http.end();
    return false;
  }
  
  showDownloadProgress(50, "Downloading...");
  
  // Download to buffer (PSRAM-aware: images can be up to 500KB)
  uint8_t* buffer = (uint8_t*)psramMalloc(contentLength);
  if (!buffer) {
    Serial.println("No memory for download");
    http.end();
    return false;
  }
  
  WiFiClient* stream = http.getStreamPtr();
  size_t downloaded = 0;
  unsigned long downloadStart = millis();
  
  while (downloaded < (size_t)contentLength) {
    // Safety: abort if download hangs beyond DOWNLOAD_TIMEOUT
    if (millis() - downloadStart > DOWNLOAD_TIMEOUT) {
      Serial.println("[DOWNLOAD] Timeout waiting for stream data");
      break;
    }
    size_t available = stream->available();
    if (available > 0) {
      size_t toRead = min(available, (size_t)(contentLength - downloaded));
      size_t read = stream->readBytes(buffer + downloaded, toRead);
      downloaded += read;
      
      // Update progress
      int progress = 50 + (downloaded * 40 / contentLength);
      showDownloadProgress(progress, "Downloading...");
    }
    delay(1);  // Yield to FreeRTOS / feed WDT every iteration
  }
  
  http.end();
  
  Serial.printf("Downloaded %d bytes\n", downloaded);
  
  // Verify it's a valid JPEG
  if (downloaded < 4 || buffer[0] != 0xFF || buffer[1] != 0xD8) {
    Serial.println("Not a valid JPEG");
    free(buffer);
    return false;
  }
  
  showDownloadProgress(90, "Saving...");
  
  // Save file
  File file = SD.open(savePath, FILE_WRITE);
  if (!file) {
    Serial.printf("Cannot create file: %s\n", savePath.c_str());
    free(buffer);
    return false;
  }
  
  size_t written = file.write(buffer, downloaded);
  file.close();
  free(buffer);
  
  if (written == downloaded) {
    Serial.printf("File saved: %s (%d bytes)\n", savePath.c_str(), written);
    showDownloadProgress(100, "Complete!");
    return true;
  } else {
    Serial.printf("Write error: %d/%d bytes\n", written, downloaded);
    SD.remove(savePath);
    return false;
  }
}

// ========== DISPLAY WITH AUTOMATIC CENTERING ==========

bool displayCoreImageCentered(String imagePath) {
  if (!sdCardAvailable || !SD.exists(imagePath)) {
    Serial.printf("Image doesn't exist: %s\n", imagePath.c_str());
    return false;
  }
  
  Serial.printf("Displaying image with auto-centering: %s\n", imagePath.c_str());
  
  // Clear screen with black
  Lcd.fillScreen(THEME_BLACK);
  
  // Open and verify image file
  File imageFile = SD.open(imagePath);
  if (!imageFile) {
    Serial.println("Error opening image file");
    return false;
  }
  
  size_t fileSize = imageFile.size();
  if (fileSize == 0 || fileSize > 500000) {
    Serial.println("Invalid image file size");
    imageFile.close();
    return false;
  }
  
  // Read image to buffer (PSRAM-aware to preserve internal heap for ESP32 stack/variables)
  uint8_t *buffer = (uint8_t*)psramMalloc(fileSize);
  if (!buffer) {
    Serial.println("No memory for image buffer");
    imageFile.close();
    return false;
  }
  
  size_t bytesRead = imageFile.read(buffer, fileSize);
  imageFile.close();
  
  if (bytesRead != fileSize) {
    Serial.println("Error reading image file");
    free(buffer);
    return false;
  }
  
  // Verify valid JPEG
  if (buffer[0] != 0xFF || buffer[1] != 0xD8) {
    Serial.println("Not a valid JPEG file");
    free(buffer);
    return false;
  }
  
  Serial.println("=== JPEG DECODE DIAGNOSTIC ===");
  Serial.printf("File: %s\n", imagePath.c_str());
  Serial.printf("Buffer size: %d bytes\n", fileSize);
  Serial.printf("JPEG signature: %02X %02X %02X %02X\n", buffer[0], buffer[1], buffer[2], buffer[3]);
  Serial.printf("Free heap before openRAM: %d bytes\n", ESP.getFreeHeap());
  
  // Ensure clean state
  jpeg.close();
  
  // Decode with automatic centering
  Serial.println("Calling jpeg.openRAM()...");
  if (jpeg.openRAM(buffer, fileSize, jpegDrawCallback)) {
    Serial.println("jpeg.openRAM() SUCCESS");
    int imgW = jpeg.getWidth();
    int imgH = jpeg.getHeight();
    
    // Calculate automatic centering
    int offsetX = (TARGET_WIDTH - imgW) / 2;
    int offsetY = (IMAGE_AREA_HEIGHT - imgH) / 2;

    Serial.printf("Image dimensions: %dx%d\n", imgW, imgH);
    Serial.printf("Available area: %dx%d\n", TARGET_WIDTH, IMAGE_AREA_HEIGHT);
    Serial.printf("Calculated offset: X=%d, Y=%d\n", offsetX, offsetY);
    Serial.printf("Final position: X=%d to X=%d, Y=%d to Y=%d\n", 
                  offsetX, offsetX + imgW, offsetY, offsetY + imgH);
    
    // Ensure offsets are not negative
    if (offsetX < 0) offsetX = 0;
    if (offsetY < 0) offsetY = 0;
    
    // Verify image won't overlap footer
    if (offsetY + imgH > IMAGE_AREA_HEIGHT) {
      Serial.printf("WARNING: Image extends into footer area, adjusting...\n");
      offsetY = IMAGE_AREA_HEIGHT - imgH;
      if (offsetY < 0) offsetY = 0;
    }
    
    // Set global offsets for callback-based centering
    // JPEGDEC doesn't accept large offsets in decode(), so we apply them in the callback
    g_jpegOffsetX = offsetX;
    g_jpegOffsetY = offsetY;
    Serial.printf("Set global callback offsets: (%d,%d)\n", g_jpegOffsetX, g_jpegOffsetY);
    
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    
    Serial.printf("Image: %dx%d, Buffer: %d bytes\n", imgW, imgH, fileSize);
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    
    // ALWAYS decode at (0,0) - centering is applied in callback
    Serial.println("Calling jpeg.decode(0, 0, 0) - centering via callback...");
    bool success = jpeg.decode(0, 0, 0);
    
    // Clear global offsets
    g_jpegOffsetX = 0;
    g_jpegOffsetY = 0;
    
    Serial.printf("Result: %s\n", success ? "SUCCESS" : "FAILED");
    Serial.printf("Heap after: %d bytes\n", ESP.getFreeHeap());
    
    jpeg.close();
    free(buffer);
    
    if (success) {
      Serial.printf("Image displayed centered: %dx%d at (%d,%d)\n", 
                    imgW, imgH, offsetX, offsetY);
      return true;
    } else {
      Serial.println("Error decoding JPEG");
      return false;
    }
  } else {
    Serial.println("Error opening JPEG decoder");
    free(buffer);
    return false;
  }
}

GameInfo extractGameInfoFromJeuInfos(String& response, String originalFilename) {
  GameInfo result;
  result.found = false;
  
  Serial.printf("Extracting game info from JSON response (%d bytes)\n", response.length());
  Serial.printf("Free heap before JSON parsing: %d bytes\n", ESP.getFreeHeap());
  
  // OPTIMIZATION: Memory verification and truncation if necessary
  if (response.length() > 8000 && ESP.getFreeHeap() < 100000) {
    Serial.printf("Large JSON + low memory, truncating response\n");
    response = response.substring(0, 8000);
  }
  
  // EXTRACT GAME ID - FIXED: Search specifically in "jeu" section
  int jeuStart = response.indexOf("\"jeu\"");
  if (jeuStart == -1) {
    jeuStart = response.indexOf("\"jeu\" :");
  }
  
  if (jeuStart != -1) {
    // Look for "id" specifically after "jeu" section starts
    int idStart = response.indexOf("\"id\":\"", jeuStart);
    if (idStart == -1) {
      idStart = response.indexOf("\"id\": \"", jeuStart);
    }
    
    if (idStart != -1) {
      idStart += (response.charAt(idStart + 5) == ' ') ? 7 : 6;
      int idEnd = response.indexOf("\"", idStart);
      if (idEnd != -1) {
        result.gameId = response.substring(idStart, idEnd);
        Serial.printf("Game ID extracted from jeu section: %s\n", result.gameId.c_str());
      }
    }
  }
  
  // Fallback: if no game ID found in jeu section, try to find it differently
  if (result.gameId.length() == 0) {
    Serial.printf("No game ID found in jeu section, trying alternative extraction\n");
    
    // Look for pattern: "jeu" : { "id": "XXXXX"
    int jeuPatternStart = response.indexOf("\"jeu\":");
    if (jeuPatternStart == -1) {
      jeuPatternStart = response.indexOf("\"jeu\" :");
    }
    
    if (jeuPatternStart != -1) {
      int searchStart = jeuPatternStart + 6; // Start after "jeu":
      int idPos = response.indexOf("\"id\":", searchStart);
      if (idPos != -1 && idPos < jeuPatternStart + 200) { // Within reasonable range of jeu section
        int valueStart = response.indexOf("\"", idPos + 5) + 1;
        int valueEnd = response.indexOf("\"", valueStart);
        if (valueStart > 0 && valueEnd > valueStart) {
          result.gameId = response.substring(valueStart, valueEnd);
          Serial.printf("Game ID extracted via alternative method: %s\n", result.gameId.c_str());
        }
      }
    }
  }
  
  // EXTRACT SYSTEME ID - CRITICAL FOR ARCADE DETECTION
  int systemeStart = response.indexOf("\"systeme\":{\"id\":\"");
  if (systemeStart == -1) {
    systemeStart = response.indexOf("\"systeme\": {\"id\": \"");
  }
  
  if (systemeStart != -1) {
    int idStart = response.indexOf("\"id\":\"", systemeStart);
    if (idStart == -1) {
      idStart = response.indexOf("\"id\": \"", systemeStart);
    }
    
    if (idStart != -1) {
      idStart += (response.charAt(idStart + 5) == ' ') ? 7 : 6;
      int idEnd = response.indexOf("\"", idStart);
      if (idEnd != -1) {
        result.systemeId = response.substring(idStart, idEnd);
        Serial.printf("Systeme ID extracted: %s\n", result.systemeId.c_str());
      }
    }
  }
  if (result.systemeId.length() > 0) {
  lastArcadeSystemeId = result.systemeId;
  Serial.printf("Stored arcade subsystem ID: %s\n", lastArcadeSystemeId.c_str());
  }

  // EXTRACT GAME NAME with multiple patterns
  String namePatterns[] = {"\"nom\":", "\"name\":", "\"nom_wor\":", "\"nom_us\":", "\"nom_eu\":", "\"title\":"};
  for (int i = 0; i < 6; i++) {
    int nomPos = response.indexOf(namePatterns[i]);
    if (nomPos != -1) {
      int nomStart = nomPos + namePatterns[i].length();
      
      // Skip to opening quote
      while (nomStart < response.length() && response.charAt(nomStart) != '"') nomStart++;
      nomStart++; // Skip opening quote
      
      String gameName = "";
      int pos = nomStart;
      while (pos < response.length() && response.charAt(pos) != '"') {
        gameName += response.charAt(pos);
        pos++;
        if (gameName.length() > 150) break; // Safety limit
      }
      
      if (gameName.length() > 0) {
        result.gameName = gameName;
        Serial.printf("Game Name extracted: %s (pattern: %s)\n", 
                      gameName.c_str(), namePatterns[i].c_str());
        break;
      }
    }
  }
  
  // FALLBACK: Use original filename if no name found
  if (result.gameName.length() == 0) {
    result.gameName = originalFilename;
    Serial.printf("Using original filename as game name: %s\n", originalFilename.c_str());
  }
  
  // FINAL STATUS
  if (result.gameId.length() > 0) {
    result.found = true;
    Serial.printf("JSON EXTRACTION SUCCESS!\n");
    Serial.printf("   Game ID: %s\n", result.gameId.c_str());
    Serial.printf("   Systeme ID: %s\n", result.systemeId.c_str());
    Serial.printf("   Game Name: %s\n", result.gameName.c_str());
  } else {
    Serial.printf("JSON extraction failed - no valid game ID found\n");
  }
  
  Serial.printf("Free heap after JSON extraction: %d bytes\n", ESP.getFreeHeap());
  return result;
}

bool isArcadeCore(String coreName) {
  String core = coreName;
  core.toLowerCase();
  return (core == "arcade" ||
          core == "mame"   ||
          core == "multiple arcade machine emulator");
}

// ULTRA OPTIMIZED VERSION - NO LARGE STACK ARRAYS
bool downloadImageFromMediaJeu(String mediaUrl, String savePath) {
  g_mediaSawNoMedia   = false;
  g_mediaSawValidJpeg = false;
  g_mediaAttemptCount = 0;
  Serial.printf("Downloading from mediaJeu.php: %s\n", redactScreenScraperUrl(mediaUrl).c_str());
  
  // CRITICAL: Check memory first
  int freeHeap = ESP.getFreeHeap();
  Serial.printf("Free heap at start: %d bytes\n", freeHeap);
  
  if (freeHeap < 80000) {
    Serial.printf("CRITICAL: Insufficient memory (%d bytes), aborting download\n", freeHeap);
    return false;
  }
  
  // MINIMAL ARCADE DETECTION - NO ARRAYS (same as before)
  String lowerUrl = mediaUrl;
  lowerUrl.toLowerCase();
  bool isArcadeGeneric = (lowerUrl.indexOf("systemeid=75") != -1);
  bool isArcadeSpecific = false;
  
  // CHECK SPECIFIC ARCADE IDs ONE BY ONE - NO ARRAY (same as before)
  if (!isArcadeSpecific && (lowerUrl.indexOf("systemeid=6&") != -1 || lowerUrl.endsWith("systemeid=6"))) isArcadeSpecific = true;
  if (!isArcadeSpecific && (lowerUrl.indexOf("systemeid=7&") != -1 || lowerUrl.endsWith("systemeid=7"))) isArcadeSpecific = true;
  if (!isArcadeSpecific && (lowerUrl.indexOf("systemeid=8&") != -1 || lowerUrl.endsWith("systemeid=8"))) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=47") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=49") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=53") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=54") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=55") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=56") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=69") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=112") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=147") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=148") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=149") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=150") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=151") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=152") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=153") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=154") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=155") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=156") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=157") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=158") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=159") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=160") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=161") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=162") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=163") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=164") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=165") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=166") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=167") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=168") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=169") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=170") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=173") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=174") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=175") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=176") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=177") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=178") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=179") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=180") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=181") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=182") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=183") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=184") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=185") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=186") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=187") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=188") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=189") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=190") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=191") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=192") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=193") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=194") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=195") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=196") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=209") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=227") != -1) isArcadeSpecific = true;
  if (!isArcadeSpecific && lowerUrl.indexOf("systemeid=230") != -1) isArcadeSpecific = true;
  
  bool isArcade = isArcadeGeneric || isArcadeSpecific;
  
  if (isArcadeSpecific) {
    Serial.println("ARCADE SPECIFIC SYSTEM: Using arcade-specific media priority order");
  } else if (isArcadeGeneric) {
    Serial.println("ARCADE GENERIC SYSTEM: Using arcade-specific media priority order");
  } else {
    Serial.println("OTHER SYSTEM DETECTED: Using standard media priority order");
  }
  
  // Extract base URL once
  String baseUrl = mediaUrl.substring(0, mediaUrl.lastIndexOf("&media="));
  if (baseUrl == mediaUrl) {
    baseUrl = mediaUrl;
  }
  
  // CONFIGURABLE DOWNLOAD ORDER — driven by config.ini [images] section
  String& orderStr = isArcade ? ARCADE_MEDIA_ORDER_STR : GAME_MEDIA_ORDER_STR;
  bool result = applyMediaOrderAndDownload(baseUrl, savePath, orderStr);

  if (!result) {
    Serial.printf("[MEDIA] All types failed for %s\n", isArcade ? "ARCADE" : "OTHER SYSTEM");
    Serial.printf("[MEDIA] Free heap: %d bytes\n", ESP.getFreeHeap());
  }
  return result;
}
// HELPER FUNCTION - MINIMAL STACK USAGE
bool tryDownloadMediaTypeWorking(String baseUrl, String savePath, const char* mediaType, const char* mediaName) {
  // Check memory before each attempt
  int currentHeap = ESP.getFreeHeap();
  if (currentHeap < 60000) {
    Serial.printf("Low memory before %s (%d bytes), stopping\n", mediaName, currentHeap);
    return false;
  }
  
  String currentUrl = baseUrl + "&media=" + String(mediaType);
  currentUrl += "&maxwidth=" + String(TARGET_WIDTH);
  currentUrl += "&maxheight=" + String(IMAGE_AREA_HEIGHT);  // Use 645 instead of 720
  currentUrl += "&outputformat=jpg&crc=&md5=&sha1=";
  
  Serial.printf("Trying: %s\n", mediaName);
  // Live HUD: show WHICH media type is being tried and inch the bar forward.
  // A no-artwork game walks ~30 types; a frozen "Downloading image..." at 50%
  // looked like a hang for the whole scan.
  g_mediaAttemptCount++;
  int mediaProgress = 50 + g_mediaAttemptCount;
  if (mediaProgress > 90) mediaProgress = 90;
  // Fixed CYAN: this bar tracks search-space coverage, not likelihood of
  // success. Only the actual byte transfer earns the stoplight gradient.
  showDownloadProgressColored(mediaProgress, String("Trying ") + mediaName + "...",
                              THEME_CYAN);
  Serial.printf("   URL: %s\n", redactScreenScraperUrl(currentUrl).c_str());
  
  HTTPClient http;
  http.begin(currentUrl);
  http.setTimeout(25000);
  http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");
  http.addHeader("Accept", "image/jpeg,image/png,image/*");
  
  int httpCode = http.GET();
  Serial.printf("   HTTP Response: %d\n", httpCode);
  g_lastSSHttpCode = httpCode;
  
  if (httpCode == 200) {
    // METHOD FROM BACKUP: Get complete response first
    String completeResponse = http.getString();
    Serial.printf("   Complete response: %d bytes\n", completeResponse.length());
    
    if (completeResponse.length() > 0) {
      // Check if it's a valid JPEG (from backup method)
      if (completeResponse.length() >= 3) {
        uint8_t byte1 = completeResponse.charAt(0);
        uint8_t byte2 = completeResponse.charAt(1);
        uint8_t byte3 = completeResponse.charAt(2);
        
        if (byte1 == 0xFF && byte2 == 0xD8 && byte3 == 0xFF) {
          g_mediaSawValidJpeg = true;
          // Valid JPEG - save to file
          if (completeResponse.length() > 100) { // Reasonable size check
            Serial.printf("   Valid JPEG detected, saving %s...\n", mediaName);
            
            File file = SD.open(savePath, FILE_WRITE);
            if (file) {
              // Write complete response to file (method from backup)
              size_t bytesWritten = file.write((const uint8_t*)completeResponse.c_str(), completeResponse.length());
              file.close();
              
              if (bytesWritten == completeResponse.length()) {
                Serial.printf("SUCCESS: Downloaded %s (%d bytes)\n", mediaName, bytesWritten);
                Serial.printf("Final free heap: %d bytes\n", ESP.getFreeHeap());
                
                // Clean up
                completeResponse = "";
                currentUrl = "";
                http.end();
                
                return true;
              } else {
                Serial.printf("Write error: expected %d bytes, wrote %d\n", completeResponse.length(), bytesWritten);
                SD.remove(savePath); // Clean up partial file
              }
            } else {
              Serial.printf("Failed to create file: %s\n", savePath.c_str());
            }
          } else {
            Serial.printf("Invalid image size: %d bytes\n", completeResponse.length());
          }
        } else {
          Serial.printf("Not a JPEG image (bytes: %02X %02X %02X)\n", byte1, byte2, byte3);
          
          // Check if it's a ScreenScraper text response (from backup)
          if (completeResponse.indexOf("NOMEDIA") != -1) {
            g_mediaSawNoMedia = true;
            Serial.printf("No %s media available in database\n", mediaName);
          } else if (completeResponse.indexOf("erreur") != -1) {
            Serial.printf("ScreenScraper error: %s\n", completeResponse.substring(0, 100).c_str());
          } else if (completeResponse.length() < 100) {
            Serial.printf("ScreenScraper response: %s\n", completeResponse.c_str());
          } else {
            Serial.printf("Unknown response (first 100 chars): %s\n", completeResponse.substring(0, 100).c_str());
          }
        }
      } else {
        Serial.printf("Response too short: %d bytes\n", completeResponse.length());
      }
    } else {
      Serial.println("Empty response");
    }
    
    // Clean up
    completeResponse = "";
  } else {
    Serial.printf("HTTP %d for %s\n", httpCode, mediaName);
    
    String errorResponse = http.getString();
    if (errorResponse.length() > 0 && errorResponse.length() < 500) {
      Serial.printf("   Error: %s\n", errorResponse.c_str());
    }
    errorResponse = "";
  }
  
  http.end();
  currentUrl = "";
  
  // Delay between attempts
  delay(1000);
  
  return false;
}

// =============================================================================
// tryMediaTypeWithRegions()
//
// Tries a media type with all region suffixes in user-preferred order.
// The preferred region comes from config.ini [screenscraper] region=
//
// Order: preferred region first, then the remaining three in fixed order
//        (wor -> us -> eu -> jp skipping the preferred one), then generic
//        (no suffix) if includeGeneric is true.
//
// Example with region=eu and mediaBase="box-3D":
//   box-3D(eu)  box-3D(wor)  box-3D(us)  box-3D(jp)  box-3D
//
// includeGeneric=false is used when the generic variant was already tried
// before calling this function (e.g. marquee), or when it does not exist
// in ScreenScraper (e.g. box-2D has no generic variant).
// =============================================================================
bool tryMediaTypeWithRegions(String baseUrl, String savePath,
                              const char* mediaBase, const char* mediaLabel,
                              bool includeGeneric) {
  const char* ALL_REGIONS[] = {"wor", "us", "eu", "jp"};
  String pref = _boxart_region_str;  // from config.ini region=

  // 1. Preferred region first
  String type  = String(mediaBase) + "(" + pref + ")";
  String label = String(mediaLabel) + " " + pref;
  if (tryDownloadMediaTypeWorking(baseUrl, savePath, type.c_str(), label.c_str())) return true;

  // 2. Remaining regions in fixed order, skipping the preferred one
  for (int i = 0; i < 4; i++) {
    if (String(ALL_REGIONS[i]) != pref) {
      type  = String(mediaBase) + "(" + String(ALL_REGIONS[i]) + ")";
      label = String(mediaLabel) + " " + String(ALL_REGIONS[i]);
      if (tryDownloadMediaTypeWorking(baseUrl, savePath, type.c_str(), label.c_str())) return true;
    }
  }

  // 3. Generic (no region suffix)
  if (includeGeneric) {
    if (tryDownloadMediaTypeWorking(baseUrl, savePath, mediaBase, mediaLabel)) return true;
  }

  return false;
}

// =============================================================================
// tryMediaTypesForToken()
//
// Expands a config.ini token to actual ScreenScraper &media= strings.
// Regional variants are tried in user-preferred order via tryMediaTypeWithRegions.
//
// Tokens without regional variants (fanart, screenshot, photo, illustration)
// map directly to a single API string.
//
// marquee is special: the generic "marquee" key is the most common variant
// in ScreenScraper, so it is tried first before the regional ones.
// box2d has no generic variant in the API, so includeGeneric=false.
// =============================================================================
bool tryMediaTypesForToken(String baseUrl, String savePath, String token) {
  token.trim();
  token.toLowerCase();

  if      (token == "wheel-steel")   return tryMediaTypeWithRegions(baseUrl, savePath, "wheel-steel",  "Wheel Steel");
  else if (token == "wheel-carbon")  return tryMediaTypeWithRegions(baseUrl, savePath, "wheel-carbon", "Wheel Carbon");
  else if (token == "wheel")         return tryMediaTypeWithRegions(baseUrl, savePath, "wheel",        "Wheel");
  else if (token == "box3d")         return tryMediaTypeWithRegions(baseUrl, savePath, "box-3D",       "3D Box");
  else if (token == "box2d")         return tryMediaTypeWithRegions(baseUrl, savePath, "box-2D",       "2D Box",    false);
  else if (token == "mix")           return tryMediaTypeWithRegions(baseUrl, savePath, "mixrbv",       "MixRBV");
  else if (token == "marquee") {
    // Generic "marquee" is the most common variant -- try it before regional ones
    if (tryDownloadMediaTypeWorking(baseUrl, savePath, "marquee", "Marquee")) return true;
    return tryMediaTypeWithRegions(baseUrl, savePath, "marquee", "Marquee", false);
  }
  else if (token == "fanart")        return tryDownloadMediaTypeWorking(baseUrl, savePath, "fanart",        "Fanart");
  else if (token == "screenshot")    return tryDownloadMediaTypeWorking(baseUrl, savePath, "sstitle",       "Screenshot");
  else if (token == "photo")         return tryDownloadMediaTypeWorking(baseUrl, savePath, "photo",         "Photo");
  else if (token == "illustration")  return tryDownloadMediaTypeWorking(baseUrl, savePath, "illustration",  "Illustration");
  else Serial.printf("[MEDIA] Unknown token: '%s' -- skipping\n", token.c_str());

  return false;
}

// =============================================================================
// applyMediaOrderAndDownload() -- iterates order string, tries token by token
// =============================================================================
bool applyMediaOrderAndDownload(String baseUrl, String savePath, String orderStr) {
  Serial.printf("[MEDIA] Order: %s\n", orderStr.c_str());
  int start = 0;
  while (start < (int)orderStr.length()) {
    int comma = orderStr.indexOf(',', start);
    String token = (comma == -1) ? orderStr.substring(start)
                                 : orderStr.substring(start, comma);
    token.trim();
    if (token.length() > 0) {
      Serial.printf("[MEDIA] Trying token: %s\n", token.c_str());
      if (tryMediaTypesForToken(baseUrl, savePath, token)) return true;
    }
    if (comma == -1) break;
    start = comma + 1;
  }
  Serial.println("[MEDIA] All tokens exhausted -- no image found");
  return false;
}

String getSavePath(String exactFileName, String searchCore) {
  String savePath;
  
  // Detect arcade FIRST
  bool isArcade = isArcadeCore(searchCore);
  // Now sanitize for path construction
  searchCore = sanitizeCoreFilename(searchCore);
  
  Serial.printf("Building save path for: core='%s', file='%s', arcade=%s\n", 
                searchCore.c_str(), exactFileName.c_str(), isArcade ? "YES" : "NO");
  
  if (isArcade && ENABLE_ALPHABETICAL_FOLDERS) {
    // SPECIAL ARCADE CASE: Save directly to /cores/A/game.jpg
    // Detect first letter of game name for alphabetical folder
    String alphabetPath;
    if (exactFileName.length() > 0) {
      char firstChar = exactFileName.charAt(0);
      
      // If it starts with a number (0-9), use the "#" folder.
      if (firstChar >= '0' && firstChar <= '9') {
        alphabetPath = String(CORE_IMAGES_PATH) + "/#";
      }
      // If it starts with a letter, use that letter in uppercase.
      else if (firstChar >= 'a' && firstChar <= 'z') {
        alphabetPath = String(CORE_IMAGES_PATH) + "/" + String((char)(firstChar - 32));
      }
      else if (firstChar >= 'A' && firstChar <= 'Z') {
        alphabetPath = String(CORE_IMAGES_PATH) + "/" + String(firstChar);
      }
      // For other characters, use "#"
      else {
        alphabetPath = String(CORE_IMAGES_PATH) + "/#";
      }
    } else {
      alphabetPath = String(CORE_IMAGES_PATH) + "/A"; // Fallback
    }
    
    Serial.printf("ARCADE: Using alphabet path: %s\n", alphabetPath.c_str());
    
    // Ensure that the alphabetical directory exists
    if (!SD.exists(alphabetPath)) {
      if (SD.mkdir(alphabetPath)) {
        Serial.printf("Created arcade alphabet dir: %s\n", alphabetPath.c_str());
      } else {
        Serial.printf("Failed to create arcade alphabet dir: %s\n", alphabetPath.c_str());
      }
    }
    
    // Save directly to /cores/A/game.jpg (NOT /cores/A/arcade/game.jpg)
    savePath = alphabetPath + "/" + exactFileName + ".jpg";
  } else {
    // STANDARD CASE: Use original logic
    if (ENABLE_ALPHABETICAL_FOLDERS) {
      String alphabetPath = getAlphabeticalPath(searchCore);
      String coreDir = alphabetPath + "/" + searchCore;
      
      Serial.printf("Standard structure: %s\n", coreDir.c_str());
      
      // Create directories if they do not exist
      if (!SD.exists(alphabetPath)) {
        if (SD.mkdir(alphabetPath)) {
          Serial.printf("Created alphabet dir: %s\n", alphabetPath.c_str());
        } else {
          Serial.printf("Failed to create: %s\n", alphabetPath.c_str());
        }
      }
      
      if (!SD.exists(coreDir)) {
        if (SD.mkdir(coreDir)) {
          Serial.printf("Created core dir: %s\n", coreDir.c_str());
        } else {
          Serial.printf("Failed to create: %s\n", coreDir.c_str());
        }
      }
      
      savePath = coreDir + "/" + exactFileName + ".jpg";
    } else {
      // Direct structure: /cores/corename/
      String coreDir = String(CORE_IMAGES_PATH) + "/" + searchCore;
      
      Serial.printf("Direct structure: %s\n", coreDir.c_str());
      
      if (!SD.exists(coreDir)) {
        if (SD.mkdir(coreDir)) {
          Serial.printf("Created core dir: %s\n", coreDir.c_str());
        } else {
          Serial.printf("Failed to create: %s\n", coreDir.c_str());
        }
      }
      
      savePath = coreDir + "/" + exactFileName + ".jpg";
    }
  }
  
  Serial.printf("Final save path: %s\n", savePath.c_str());
  return savePath;
}

void showGameImageScreenCorrected(String coreName, String gameName) {
  // backgroundLoaded = false;
  String imagePath;
  
  Serial.printf("\n=== STREAMING-SAFE GAME SCREEN: %s - %s ===\n", coreName.c_str(), gameName.c_str());
  
  if (!sdCardAvailable) {
    Serial.println("SD not available, showing SD error");
    showSDCardError();
    return;
  }
  
  if (gameName.length() == 0) {
    Serial.println("No game name, showing core image instead");
    lastGameImageOK = false;
    showCoreImageScreen(coreName);
    return;
  }
  
  // Check memory before processing
  if (ESP.getFreeHeap() < 80000) {
    Serial.printf("Low memory for game screen (%d bytes), showing core image\n", ESP.getFreeHeap());
    lastGameImageOK = false;
    showCoreImageScreen(coreName);
    return;
  }
  
  // Check existing image and force download setting
  bool imageExists = findGameImageExact(coreName, gameName, imagePath);
  bool shouldDownload = false;
  
  if (FORCE_GAME_REDOWNLOAD) {
    Serial.println("FORCE_GAME_REDOWNLOAD enabled - will download regardless of existing image");
    shouldDownload = true;
    
    // If image exists and we're forcing download, back it up
    if (imageExists) {
      String backupPath = imagePath + ".backup";
      if (SD.exists(backupPath)) {
        SD.remove(backupPath);
      }
      SD.rename(imagePath, backupPath);
      Serial.printf("Existing game image backed up to: %s\n", backupPath.c_str());
    }
  } else if (!imageExists) {
    Serial.println("No game image found - will attempt download");
    shouldDownload = true;
  } else {
    Serial.printf("Game image found: %s\n", imagePath.c_str());
    
    if (displayCoreImageCentered(imagePath)) {
      Serial.println("Game image displayed correctly");
      lastGameImageOK = true;
      addGameImageFooter(gameName);
      return;
    }
  }
  
  // AUTO-DOWNLOAD with SAFE STREAMING
  if (shouldDownload && ENABLE_AUTO_DOWNLOAD && WiFi.status() == WL_CONNECTED && !downloadInProgress) {
    // Two different "don't retry" reasons, deliberately kept apart:
    //   lastSearchedGame       — this game was already fetched successfully.
    //   lastGameSearchExhausted — ScreenScraper answered cleanly: not in the DB.
    // A TRANSIENT failure (network down, SS busy) sets neither, so it retries.
    // This screen calls the download directly, bypassing processCrcRecurrent's
    // own exhaustion gate — without the check below, every redraw of a
    // not-in-DB game would relaunch the full search.
    if ((lastSearchedGame != gameName && !lastGameSearchExhausted) || FORCE_GAME_REDOWNLOAD) {
      Serial.println("Attempting STREAMING-SAFE ScreenScraper download...");
      
      // Final memory check before download
      if (ESP.getFreeHeap() < 120000) {
        Serial.printf("Insufficient memory for download (%d bytes)\n", ESP.getFreeHeap());
        Serial.println("Falling back to core image");
        lastGameImageOK = false;
        showCoreImageScreen(coreName);
        return;
      }
      
      downloadHudEnabled = true;   // interactive load: user is watching, show progress
      showDownloadingScreen(coreName, gameName);
      
      // *** USE FUNCTION STREAMING-SAFE ***
      bool interactiveOk = downloadGameBoxartStreamingSafeJSON(coreName, gameName);
      downloadHudEnabled = false;  // leave global state clean for the recurrent path
      if (interactiveOk) {
        Serial.println("STREAMING-SAFE download successful! Displaying...");
        
        if (findGameImageExact(coreName, gameName, imagePath) && displayCoreImageCentered(imagePath)) {
          Serial.println("Downloaded streaming-safe image displayed successfully");
          
          // Only update cache on successful download AND display
          if (!FORCE_GAME_REDOWNLOAD) {
            lastSearchedGame = gameName;
            Serial.printf("Cache updated: lastSearchedGame = '%s'\n", gameName.c_str());
          }
          
          lastGameImageOK = true;
          addGameImageFooter(gameName);
          return;
        }
      }
      // Do NOT update lastSearchedGame on failure: retry pacing is owned by
      // the CRC-recurrent layer (10 s cadence, 30-attempt cap, exhaustion
      // flag). Updating the cache here silently blocked every retry until a
      // forced scan — the exact contradiction seen in the field logs.
      Serial.println("STREAMING-SAFE download failed - NOT updating cache to allow retry");
    } else if (getScreenScraperSystemId(coreName).length() == 0) {
      // lastGameSearchExhausted is overloaded: startCrcRecurrentForGame() also
      // sets it when the CORE has no ScreenScraper system. Report that cause
      // separately — the game may well exist in the DB; we simply cannot ask.
      Serial.printf("Core '%s' has no ScreenScraper system - artwork impossible\n",
                    coreName.c_str());
      ssNotifyUnsupportedCore(coreName);
    } else if (lastGameSearchExhausted) {
      if (lastGameFoundNoMedia) {
        Serial.printf("Game '%s' catalogued but has no artwork - not retrying\n", gameName.c_str());
        ssNotifyOnce(coreName + "|" + gameName, "GAME HAS NO ARTWORK IN SS", gameName);
      } else {
        Serial.printf("Game '%s' known absent from ScreenScraper - not retrying\n", gameName.c_str());
        ssNotifyOnce(coreName + "|" + gameName, "GAME NOT IN SS DATABASE", gameName);
      }
    } else {
      Serial.printf("Game '%s' already searched recently, skipping\n", gameName.c_str());
    }
  }
  
  // FALLBACK: show core image
  Serial.println("Streaming-safe fallback to core image");
  lastGameImageOK = false;
  showCoreImageScreen(coreName);
}

void forceMemoryCleanup() {
  Serial.printf("Force memory cleanup - Free heap before: %d bytes\n", ESP.getFreeHeap());
  
  // Force garbage collection
  delay(100);
  
  Serial.printf("Force memory cleanup - Free heap after: %d bytes\n", ESP.getFreeHeap());
  
  if (ESP.getFreeHeap() < 50000) {
    Serial.println("CRITICAL: Memory critically low - consider restart");
  }
}

bool downloadCoreImageStreamingSafe(String baseUrl, String savePath) {
  Serial.printf("=== STREAMING-SAFE CORE DOWNLOAD ===\n");
  Serial.printf("Target: %s\n", savePath.c_str());
  
  // List of media types to test for cores (systems)
  // wheel-steel is tried first as it gives the best visual result for system images
  String mediaTypes[] = {
    "wheel-steel(wor)",
    "wheel-steel(us)",
    "wheel-steel(eu)",
    "wheel-steel(jp)",
    "wheel-steel",
    "wheel-carbon(wor)",
    "wheel-carbon(us)",
    "wheel-carbon(eu)",
    "wheel-carbon(jp)",
    "wheel(wor)",
    "wheel(us)",
    "wheel(eu)",
    "wheel(jp)",
    "illustration(wor)",
    "illustration(us)",
    "illustration(eu)",
    "illustration(jp)",
    "photo(wor)",
    "background(wor)",
    "screenmarquee(wor)",
  };
  String mediaNames[] = {
    "Steel Wheel World",
    "Steel Wheel USA",
    "Steel Wheel Europe",
    "Steel Wheel Japan",
    "Steel Wheel",
    "Carbon Wheel World",
    "Carbon Wheel USA",
    "Carbon Wheel Europe",
    "Carbon Wheel Japan",
    "Wheel World",
    "Wheel USA",
    "Wheel Europe",
    "Wheel Japan",
    "Illustration World",
    "Illustration USA",
    "Illustration Europe",
    "Illustration Japan",
    "Photo",
    "Background",
    "Screen Marquee",
  };
  int mediaCount = 20;
  
  for (int i = 0; i < mediaCount; i++) {
    Serial.printf("Trying media type %d: %s\n", i + 1, mediaNames[i].c_str());
    
    // Build complete URL with media type and resize parameters
    String currentUrl = baseUrl + "&media=" + mediaTypes[i];
    currentUrl += "&maxwidth=" + String(TARGET_WIDTH);
    currentUrl += "&maxheight=" + String(IMAGE_AREA_HEIGHT);  // Use 645 instead of 720
    currentUrl += "&outputformat=jpg";
    
    Serial.printf("   URL: %s\n", redactScreenScraperUrl(currentUrl).c_str());
    
    HTTPClient http;
    http.begin(currentUrl);
    http.setTimeout(25000);
    http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");
    http.addHeader("Accept", "image/jpeg,image/png,image/*");
    
    int httpCode = http.GET();
    g_lastSSHttpCode = httpCode;
    
    Serial.printf("HTTP Response: %d\n", httpCode);
    
    if (httpCode == 200) {
      String contentType = http.header("Content-Type");
      int contentLength = http.getSize();
      
      Serial.printf("Content-Type: '%s'\n", contentType.c_str());
      Serial.printf("Content-Length: %d\n", contentLength);
      
      // Read the COMPLETE response first (SAFE STREAMING)
      String completeResponse = http.getString();
      Serial.printf("Response size: %d bytes\n", completeResponse.length());
      
      if (completeResponse.length() > 0) {
        // Show first bytes as hex for debugging
        Serial.printf("First 10 bytes (hex): ");
        for (int j = 0; j < min(10, (int)completeResponse.length()); j++) {
          Serial.printf("%02X ", (uint8_t)completeResponse.charAt(j));
        }
        Serial.println();
        
        // Check for JPEG signature in the complete response
        if (completeResponse.length() >= 3) {
          uint8_t byte1 = (uint8_t)completeResponse.charAt(0);
          uint8_t byte2 = (uint8_t)completeResponse.charAt(1);
          uint8_t byte3 = (uint8_t)completeResponse.charAt(2);
          
          bool isJPEG = (byte1 == 0xFF && byte2 == 0xD8 && byte3 == 0xFF);
          
          if (isJPEG) {
            Serial.printf("JPEG signature detected! Size: %d bytes\n", completeResponse.length());
            
            // Validate reasonable image size for core images
            if (completeResponse.length() > 1000 && completeResponse.length() < 300000) {
              // Create directory if needed
              String dir = savePath.substring(0, savePath.lastIndexOf('/'));
              if (!SD.exists(dir)) {
                Serial.printf("Creating directory: %s\n", dir.c_str());
                SD.mkdir(dir);
              }
              
              // Save the image file
              File file = SD.open(savePath, FILE_WRITE);
              if (file) {
                // Write complete response as binary data
                file.write((uint8_t*)completeResponse.c_str(), completeResponse.length());
                file.close();
                
                // Verify the saved file
                if (SD.exists(savePath)) {
                  File verifyFile = SD.open(savePath);
                  if (verifyFile) {
                    size_t savedSize = verifyFile.size();
                    verifyFile.close();
                    
                    if (savedSize == completeResponse.length()) {
                      Serial.printf("SUCCESS: Downloaded %s (%d bytes)\n", mediaNames[i].c_str(), savedSize);
                      Serial.printf("Saved to: %s\n", savePath.c_str());
                      http.end();
                      return true;
                    } else {
                      Serial.printf("File size mismatch: %d vs %d\n", savedSize, completeResponse.length());
                    }
                  }
                }
              } else {
                Serial.printf("Cannot create file: %s\n", savePath.c_str());
              }
            } else {
              Serial.printf("Invalid image size: %d bytes\n", completeResponse.length());
            }
          } else {
            Serial.printf("Not a JPEG image (bytes: %02X %02X %02X)\n", byte1, byte2, byte3);
            
            // Check if it's a ScreenScraper text response
            if (completeResponse.indexOf("NOMEDIA") != -1) {
            g_mediaSawNoMedia = true;
              Serial.printf("No %s media available in database\n", mediaNames[i].c_str());
            } else if (completeResponse.indexOf("erreur") != -1) {
              Serial.printf("ScreenScraper error: %s\n", completeResponse.substring(0, 100).c_str());
            } else if (completeResponse.length() < 100) {
              Serial.printf("ScreenScraper response: %s\n", completeResponse.c_str());
            } else {
              Serial.printf("Unknown response (first 100 chars): %s\n", completeResponse.substring(0, 100).c_str());
            }
          }
        } else {
          Serial.printf("Response too short: %d bytes\n", completeResponse.length());
        }
      } else {
        Serial.println("Empty response");
      }
    } else {
      Serial.printf("HTTP %d for %s\n", httpCode, mediaNames[i].c_str());
      
      String errorResponse = http.getString();
      if (errorResponse.length() > 0 && errorResponse.length() < 500) {
        Serial.printf("   Error: %s\n", errorResponse.c_str());
      }
    }
    
    http.end();
    delay(1000); // Delay between attempts
  }
  
  Serial.println("All media types failed - no suitable core image found");
  return false;
}

String buildCorrectMediaJeuUrl(String gameId, String systemId, String mediaType, String specificSystemeId) {
  // ========== INITIAL DEBUG ==========
  Serial.printf("=== buildCorrectMediaJeuUrl DEBUG ===\n");
  Serial.printf("   gameId: '%s'\n", gameId.c_str());
  Serial.printf("   systemId (generic): '%s'\n", systemId.c_str());
  Serial.printf("   mediaType: '%s'\n", mediaType.c_str());
  Serial.printf("   specificSystemeId: '%s' (length: %d)\n", specificSystemeId.c_str(), specificSystemeId.length());
  
  // ========== BASIC VALIDATION ==========
  if (gameId.length() == 0) {
    Serial.println("Cannot build mediaJeu URL: gameId is empty");
    return "";
  }
  
  // ========== SYSTEM ID SELECTION LOGIC ==========
  // Use the specific systemeId if available, otherwise use the generic one.
  String finalSystemId = (specificSystemeId.length() > 0) ? specificSystemeId : systemId;
  
  Serial.printf("   finalSystemId: '%s'\n", finalSystemId.c_str());
  
  // ========== URL CONSTRUCTION ==========
  String mediaUrl = "https://api.screenscraper.fr/api2/mediaJeu.php";
  mediaUrl += "?devid=" + String(SCREENSCRAPER_DEV_USER);
  mediaUrl += "&devpassword=" + String(SCREENSCRAPER_DEV_PASS);
  mediaUrl += "&softname=M5Stack-MiSTer-Monitor";
  mediaUrl += "&ssid=" + String(SCREENSCRAPER_USER);
  mediaUrl += "&sspassword=" + String(SCREENSCRAPER_PASS);
  
  // REQUIRED PARAMETERS BUT THEY CAN BE EMPTY
  mediaUrl += "&crc=";     // Empty but required
  mediaUrl += "&md5=";     // Empty but required  
  mediaUrl += "&sha1=";    // Empty but required
  
  // ========== CRITICAL PARAMETER: systemeid ==========
  mediaUrl += "&systemeid=" + finalSystemId;  // USE THE SPECIFIC ID
  mediaUrl += "&jeuid=" + gameId;
  mediaUrl += "&media=" + mediaType;
  mediaUrl += "&maxwidth=" + String(TARGET_WIDTH);
  mediaUrl += "&maxheight=" + String(IMAGE_AREA_HEIGHT);  // Use 645 instead of 720
  mediaUrl += "&outputformat=jpg";
  
  // ========== DETAILED DEBUG ==========
  if (specificSystemeId.length() > 0) {
    Serial.printf("Using SPECIFIC systeme ID: %s (instead of generic: %s)\n", 
                 specificSystemeId.c_str(), systemId.c_str());
    Serial.printf("ARCADE SYSTEM DETECTED! Using systeme-specific ID\n");
  } else {
    Serial.printf("Using generic system ID: %s\n", systemId.c_str());
    Serial.printf("No specific systeme ID available, using generic\n");
  }
  
  // ========== FINAL URL DEBUG ==========
  Serial.printf("FINAL MEDIAJEU URL:\n");
  Serial.printf("   %s\n", redactScreenScraperUrl(mediaUrl).c_str());
  
  // Extract and highlight the systemeid parameter for verification
  int systemeidPos = mediaUrl.indexOf("systemeid=");
  if (systemeidPos != -1) {
    int systemeidEnd = mediaUrl.indexOf("&", systemeidPos);
    if (systemeidEnd == -1) systemeidEnd = mediaUrl.length();
    String systemeidParam = mediaUrl.substring(systemeidPos, systemeidEnd);
    Serial.printf("SYSTEMEID PARAMETER: %s\n", systemeidParam.c_str());
  }
  
  return mediaUrl;
}

void initScrollText(ScrollTextState* state, String text, int maxDisplayChars) {
  state->fullText = text;
  state->maxChars = maxDisplayChars;
  state->scrollPos = 0;
  state->lastScrollTime = millis();
  state->pauseStartTime = millis();
  state->isPaused = true;
  state->needsScroll = (text.length() > maxDisplayChars);
  state->pauseAtEnd = false;  // Always start with pause at beginning
  
  Serial.printf("Init scroll: '%s' (len:%d, max:%d, needsScroll:%s)\n", 
                text.c_str(), text.length(), maxDisplayChars, 
                state->needsScroll ? "YES" : "NO");
}

String getScrolledText(ScrollTextState* state) {
  // If text doesn't need scrolling, return as-is
  if (!state->needsScroll || state->fullText.length() == 0) {
    return state->fullText;
  }
  
  unsigned long currentTime = millis();
  
  // Handle pauses
  if (state->isPaused) {
    // Determine pause duration based on whether we're at start or end
    unsigned long pauseDuration = state->pauseAtEnd ? SCROLL_PAUSE_END_MS : SCROLL_PAUSE_START_MS;
    
    if (currentTime - state->pauseStartTime >= pauseDuration) {
      state->isPaused = false;
      state->lastScrollTime = currentTime;
      
      // If we were pausing at end, reset to beginning for next cycle
      if (state->pauseAtEnd) {
        state->scrollPos = 0;
        state->pauseAtEnd = false;
        // Will start pause at beginning on next cycle
        state->isPaused = true;
        state->pauseStartTime = currentTime;
      }
    }
  } else {
    // Handle scrolling
    if (currentTime - state->lastScrollTime >= SCROLL_SPEED_MS) {
      state->scrollPos++;
      
      // Calculate maximum scroll position
      int maxScrollPos = state->fullText.length() - state->maxChars;
      
      // Check if we've reached the end
      if (state->scrollPos >= maxScrollPos) {
        // Don't go past the end, stay at maxScrollPos and start end pause
        state->scrollPos = maxScrollPos;
        state->isPaused = true;
        state->pauseAtEnd = true;
        state->pauseStartTime = currentTime;
        
        Serial.printf("Reached end, starting end pause (pos:%d, maxPos:%d)\n", 
                      state->scrollPos, maxScrollPos);
      }
      
      state->lastScrollTime = currentTime;
    }
  }
  
  // Extract visible portion of text
  if (state->scrollPos + state->maxChars > state->fullText.length()) {
    return state->fullText.substring(state->scrollPos);
  } else {
    return state->fullText.substring(state->scrollPos, state->scrollPos + state->maxChars);
  }
}

GameInfo searchWithJeuInfosPreciseJSON(String coreName, RomDetails romDetails) {
  GameInfo result;
  result.found = false;

  Serial.printf("=== JSON PRECISE SEARCH ===\n");
  Serial.printf("Core: %s | ROM: %s | CRC: %s\n",
                coreName.c_str(), romDetails.filename.c_str(), romDetails.crc32.c_str());

  int startHeap = ESP.getFreeHeap();
  Serial.printf("Starting heap: %d bytes\n", startHeap);

  // OPTIMIZATION: Critical memory verification
  if (startHeap < 100000) {
    Serial.printf("CRITICAL: Insufficient memory for JSON search (%d bytes)\n", startHeap);
    return result;
  }

  String systemId = getScreenScraperSystemId(coreName);
  if (systemId.length() == 0) {
    Serial.printf("System '%s' not supported by ScreenScraper\n", coreName.c_str());
    return result;
  }

  // Build URL
  String url = "https://api.screenscraper.fr/api2/jeuInfos.php";
  url += "?devid=" + String(SCREENSCRAPER_DEV_USER);
  url += "&devpassword=" + String(SCREENSCRAPER_DEV_PASS);
  url += "&softname=M5Stack-MiSTer-Monitor";
  url += "&output=json";
  url += "&ssid=" + urlEncode(String(SCREENSCRAPER_USER));
  url += "&sspassword=" + urlEncode(String(SCREENSCRAPER_PASS));
  url += "&systemeid=" + systemId;
  url += "&romtype=rom";
  url += "&romnom=" + urlEncode(romDetails.filename);
  url += "&crc=" + romDetails.crc32;
  url += "&romtaille=" + String(romDetails.filesize);
  url += "&md5=" + romDetails.md5;
  url += "&sha1=";

  Serial.printf("JSON Search URL: %s\n", redactScreenScraperUrl(url).c_str());
  logCredentialShape("ss_pass", String(SCREENSCRAPER_PASS));
  Serial.printf("[SS] devid='%s' devpassword=[%s]\n",
                String(SCREENSCRAPER_DEV_USER).c_str(),
                String(SCREENSCRAPER_DEV_PASS).length() > 0 ? "set" : "EMPTY");

  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);
  http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");
  http.addHeader("Accept", "application/json");

  int httpCode = http.GET();
  Serial.printf("HTTP Response: %d\n", httpCode);
  g_lastSSHttpCode = httpCode;

  if (httpCode == 200) {
    int contentLength = http.getSize();
    Serial.printf("Content length: %d bytes\n", contentLength);

    int currentHeap = ESP.getFreeHeap();

    // Streaming when: unknown length + tight memory, OR known large body,
    // OR low memory right now.
    bool shouldUseStreaming = (contentLength == -1 && currentHeap < 150000) ||
                              (contentLength > 20000) ||
                              (currentHeap < 130000);

    if (shouldUseStreaming) {
      // -------- EARLY-EXIT BOUNDED SCAN --------
      // The fields we need (jeu.id, jeu.noms, jeu.systeme.id) sit in the
      // first few KB of response.jeu. Everything that makes the body huge
      // (synopsis, genres, medias[], roms[]) comes AFTER them. So we read
      // only a bounded prefix of the raw stream and stop. This avoids
      // pulling ~100 KB over slow TLS and never needs a complete/valid
      // JSON document.
      //
      // Raw substring scanning is immune to Transfer-Encoding: chunked
      // because chunk-size markers only appear BETWEEN large chunks and
      // never split the short field patterns we look for.
      const size_t CAP_BYTES = 12000;   // generous: fields are well within this
      Serial.printf("Bounded streaming scan (cap=%u): contentLength=%d, heap=%d\n",
                    (unsigned)CAP_BYTES, contentLength, currentHeap);

      WiFiClient* stream = http.getStreamPtr();
      String body;
      body.reserve(CAP_BYTES + 256);

      char tmp[513];
      unsigned long lastDataMs = millis();

      while (body.length() < CAP_BYTES) {
        // End conditions: connection closed and nothing left to read,
        // low memory, or a read stall (no bytes for >8 s).
        if (!http.connected() && stream->available() == 0) break;
        if (ESP.getFreeHeap() < 60000) {
          Serial.printf("Low memory during scan, stopping early\n");
          break;
        }

        int avail = stream->available();
        if (avail > 0) {
          int toRead = avail;
          if (toRead > (int)sizeof(tmp) - 1) toRead = sizeof(tmp) - 1;
          int n = stream->readBytes((uint8_t*)tmp, toRead);
          if (n > 0) {
            tmp[n] = '\0';
            body += tmp;
            lastDataMs = millis();
          }
        } else {
          if (millis() - lastDataMs > 8000) {
            Serial.printf("Stream stalled (no data 8s), stopping with %d bytes\n",
                          body.length());
            break;
          }
          delay(10);
        }
      }

      Serial.printf("Scan collected %d bytes, heap after: %d\n",
                    body.length(), ESP.getFreeHeap());

      if (body.length() == 0) {
        Serial.println("Empty body during streaming scan");
      } else {
        // Reuse the proven indexOf parser already used by the traditional
        // branch. It handles "jeu".id, "systeme":{"id"...} and the noms
        // region preference / nom fallback.
        result = extractGameInfoFromJeuInfos(body, romDetails.filename);
        body = "";  // free immediately

        if (result.found && result.gameId.length() > 0) {
          String detectedMediaType = "box-3D(wor)";
          result.boxartUrl = buildCorrectMediaJeuUrl(
              result.gameId, systemId, detectedMediaType, result.systemeId);
          if (result.boxartUrl.length() == 0) {
            Serial.println("Failed to build mediaJeu URL");
            result.found = false;
          }
        } else {
          Serial.printf("Bounded scan did not yield a game id "
                        "(game not in DB, or fields beyond %u bytes)\n",
                        (unsigned)CAP_BYTES);
        }
      }
      // -------- END EARLY-EXIT BOUNDED SCAN --------

    } else {
      Serial.printf("Using traditional processing: contentLength=%d, heap=%d\n",
                    contentLength, currentHeap);

      if (currentHeap < 90000) {
        Serial.printf("Insufficient memory for traditional processing (need 90000+, have %d)\n",
                      currentHeap);
        http.end();
        return result;
      }

      String response = http.getString();
      Serial.printf("Response received: %d bytes\n", response.length());
      Serial.printf("Memory after getString: %d bytes\n", ESP.getFreeHeap());

      if (response.length() == 0) {
        Serial.printf("Empty response received despite successful HTTP 200\n");
        http.end();
        return result;
      }

      int memoryAfterResponse = ESP.getFreeHeap();
      if (response.length() > 10000 && memoryAfterResponse < 80000) {
        Serial.printf("Large response (%d bytes) + low memory (%d bytes), truncating to 8000\n",
                      response.length(), memoryAfterResponse);
        response = response.substring(0, 8000);
        Serial.printf("Response truncated to: %d bytes\n", response.length());
      }

      if (response.length() > 0) {
        Serial.printf("Processing JSON response of %d bytes...\n", response.length());
        result = extractGameInfoFromJeuInfos(response, romDetails.filename);

        if (result.found && result.gameId.length() > 0) {
          String detectedMediaType = "box-3D(wor)";
          result.boxartUrl = buildCorrectMediaJeuUrl(result.gameId, systemId,
                                                     detectedMediaType, result.systemeId);
          if (result.boxartUrl.length() > 0) {
            Serial.printf("BUILT MEDIAJEU URL:\n   %s\n", redactScreenScraperUrl(result.boxartUrl).c_str());
          } else {
            Serial.println("Failed to build mediaJeu URL");
            result.found = false;
          }
        } else {
          Serial.printf("Failed to extract game info from response\n");
        }
        response = "";
        Serial.printf("Memory after processing: %d bytes\n", ESP.getFreeHeap());
      } else {
        Serial.printf("Response became empty during processing\n");
      }
    }
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
  }

  http.end();

  int finalHeap = ESP.getFreeHeap();
  Serial.printf("Final heap: %d bytes (change: %+d)\n", finalHeap, finalHeap - startHeap);

  return result;
}

// Systems whose game containers may carry no ScreenScraper-matchable identity
// (0MHz DOS packs build per-pack VHDs/CHDs). Text search is the fallback of
// last resort there. Extend deliberately, one system at a time, after field
// validation — the allowlist is what prevents a transient hash failure on a
// CRC-capable system from degrading into a fuzzy search with wrong artwork.
bool isNameSearchSystem(const String& systemId) {
  return systemId == "135";   // DOS (0MHz packs)
}

// Text-search fallback for hash-less containers. Same response shape as
// jeuInfos.php but the game object arrives inside a "jeux":[...] array.
// We take the FIRST element (documented policy) by rebranding the array as a
// single "jeu" object and reusing the proven jeuInfos indexOf parser.
GameInfo searchWithJeuRechercheJSON(String coreName, String cleanName) {
  GameInfo result;
  result.found = false;

  Serial.printf("=== JSON NAME SEARCH (jeuRecherche) ===\n");
  Serial.printf("Core: %s | Query: '%s'\n", coreName.c_str(), cleanName.c_str());

  if (ESP.getFreeHeap() < 100000) {
    Serial.printf("CRITICAL: Insufficient memory for name search (%d bytes)\n",
                  ESP.getFreeHeap());
    return result;
  }

  String systemId = getScreenScraperSystemId(coreName);
  if (systemId.length() == 0 || cleanName.length() == 0) {
    Serial.println("Name search: missing system id or query");
    return result;
  }

  String url = "https://api.screenscraper.fr/api2/jeuRecherche.php";
  url += "?devid=" + String(SCREENSCRAPER_DEV_USER);
  url += "&devpassword=" + String(SCREENSCRAPER_DEV_PASS);
  url += "&softname=MiSTer-Monitor";
  url += "&output=json";
  url += "&ssid=" + urlEncode(String(SCREENSCRAPER_USER));
  url += "&sspassword=" + urlEncode(String(SCREENSCRAPER_PASS));
  url += "&systemeid=" + systemId;
  url += "&recherche=" + urlEncode(cleanName);

  Serial.printf("Name Search URL: %s\n", redactScreenScraperUrl(url).c_str());

  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);
  http.addHeader("User-Agent", "MiSTer-Monitor");
  http.addHeader("Accept", "application/json");

  int httpCode = http.GET();
  Serial.printf("HTTP Response: %d\n", httpCode);
  g_lastSSHttpCode = httpCode;

  if (httpCode == 200) {
    // Bounded prefix scan, same rationale as the jeuInfos streaming branch:
    // jeux[0]'s id/noms/systeme sit in the first few KB, after the
    // serveurs/ssuser preamble. Chunked transfer is harmless to substring
    // scanning of short field patterns.
    const size_t CAP_BYTES = 12000;
    WiFiClient* stream = http.getStreamPtr();
    String body;
    body.reserve(CAP_BYTES + 256);

    char tmp[513];
    unsigned long lastDataMs = millis();

    while (body.length() < CAP_BYTES) {
      if (!http.connected() && stream->available() == 0) break;
      if (ESP.getFreeHeap() < 60000) {
        Serial.printf("Low memory during name-search scan, stopping early\n");
        break;
      }
      int avail = stream->available();
      if (avail > 0) {
        int toRead = avail;
        if (toRead > (int)sizeof(tmp) - 1) toRead = sizeof(tmp) - 1;
        int n = stream->readBytes((uint8_t*)tmp, toRead);
        if (n > 0) {
          tmp[n] = '\0';
          body += tmp;
          lastDataMs = millis();
        }
      } else {
        if (millis() - lastDataMs > 8000) {
          Serial.printf("Stream stalled (no data 8s), stopping with %d bytes\n",
                        body.length());
          break;
        }
        delay(10);
      }
    }

    Serial.printf("Name-search scan collected %d bytes, heap: %d\n",
                  body.length(), ESP.getFreeHeap());

    int jx = body.indexOf("\"jeux\"");
    if (jx == -1) {
      Serial.println("No 'jeux' array in response (no results or error body)");
    } else {
      int arrStart = body.indexOf('[', jx);
      if (arrStart != -1) {
        // Rebrand jeux[...] as a single "jeu" object. The indexOf parser
        // stops at the FIRST id/systeme/noms it finds — i.e. jeux[0].
        String rebranded = "\"jeu\":" + body.substring(arrStart + 1);
        body = "";
        result = extractGameInfoFromJeuInfos(rebranded, cleanName);

        if (result.found && result.gameId.length() > 0) {
          String detectedMediaType = "box-3D(wor)";
          result.boxartUrl = buildCorrectMediaJeuUrl(
              result.gameId, systemId, detectedMediaType, result.systemeId);
          if (result.boxartUrl.length() == 0) {
            Serial.println("Failed to build mediaJeu URL");
            result.found = false;
          }
        } else {
          Serial.println("jeuRecherche yielded no usable game id");
        }
      }
    }
  } else {
    Serial.printf("Name search HTTP error: %d\n", httpCode);
  }

  http.end();
  Serial.printf("=== NAME SEARCH %s ===\n", result.found ? "HIT" : "MISS");
  return result;
}

// Shared tail of the name-search fallback: search by clean title, download
// the artwork, prefetch GAME INFO metadata. Mirrors the CRC success path.
// Runs inside downloadGameBoxartStreamingSafeJSON: the DownloadFlagGuard of
// the caller keeps downloadInProgress set for the whole attempt.
bool tryNameSearchFallback(String coreName, String gameName, RomDetails romDetails) {
  String cleanName = romDetails.searchName.length() > 0 ? romDetails.searchName
                                                        : gameName;
  Serial.printf("Name-search fallback for '%s' (server hint: %s)\n",
                cleanName.c_str(), romDetails.nameSearchHint ? "YES" : "NO");
  showDownloadProgress(40, "Name search...");

  // Snapshot the diagnostic code the CRC path produced. jeuRecherche answers
  // "200 with zero results" for an unknown title, which would overwrite a far
  // more informative 404 and turn the on-screen reason from
  // "NOT IN SS DATABASE (404)" into a generic "Download failed".
  int codeBeforeNameSearch = g_lastSSHttpCode;

  GameInfo gameInfo = searchWithJeuRechercheJSON(coreName, cleanName);
  if (!(gameInfo.found && gameInfo.boxartUrl.length() > 0)) {
    Serial.println("Name search found no results");
    // Restore the CRC path's diagnostic on a clean 200-but-empty miss. A real
    // error from the name search (429, -1, ...) is newer and more relevant, so
    // it is kept.
    if (g_lastSSHttpCode == 200 && codeBeforeNameSearch != 0 &&
        codeBeforeNameSearch != 200) {
      g_lastSSHttpCode = codeBeforeNameSearch;
      Serial.printf("Preserved CRC-path diagnostic code %d for the HUD\n",
                    codeBeforeNameSearch);
    }
    return false;
  }

  // Ambiguity policy: first jeux[] element, chosen name always logged.
  Serial.printf("NAME SEARCH CHOSE: '%s' (id %s)\n",
                gameInfo.gameName.c_str(), gameInfo.gameId.c_str());

  String exactFileName = getExactFileName(gameName);
  String searchCore = coreName;
  searchCore.toLowerCase();
  String savePath = getSavePath(exactFileName, searchCore);

  showDownloadProgress(60, "Downloading image...");
  bool ok = downloadImageFromMediaJeu(gameInfo.boxartUrl, savePath);
  Serial.printf("Name-search MediaJeu download: %s\n", ok ? "SUCCESS" : "FAILED");

  if (ok) {
    // GAME INFO panel: metadata prefetch, same hook as the CRC path.
    String metaPath = getMetaPathFromImagePath(savePath);
    if (!SD.exists(metaPath)) {
      showDownloadProgress(92, "Fetching game info...");
      GameMeta meta;
      if (fetchGameMetadataJSON(gameInfo.gameId, coreName, romDetails, meta)) {
        saveGameMeta(metaPath, meta);
        if (gameName == currentGame) { meta.forGame = gameName; currentMeta = meta; }
      }
    }
    showDownloadProgress(100, "Name search complete!");
    delay(1500);
  }
  return ok;
}

bool downloadGameBoxartStreamingSafeJSON(String coreName, String gameName) {
  if (downloadInProgress) {
    Serial.println("Download already in progress");
    return false;
  }
  
  DownloadFlagGuard dlGuard;
  g_lastSSHttpCode = 0;
  bool success = false;
  bool gameWasFound = false;   // a jeu was resolved (id present), media may still be missing

  // OPTIONAL: Detect if it is a recurring search
  bool isRecurrent = crcRecurrentActive && (gameName == currentGameForCrc);
  
  Serial.printf("=== JSON-BASED SCREENSCRAPER DOWNLOAD ===\n");
  Serial.printf("Game: '%s' | Core: '%s'\n", gameName.c_str(), coreName.c_str());
  Serial.printf("Free heap at start: %d bytes\n", ESP.getFreeHeap());
  
  // Early exit: if the core isn't mapped to a ScreenScraper system, there's
  // no point asking MiSTer for ROM details 
  // the search would fail anyway. Mark search exhausted.
  String systemId = getScreenScraperSystemId(coreName);
  if (systemId.length() == 0) {
    Serial.printf("Core '%s' not mapped to any ScreenScraper system — skipping search\n",
                  coreName.c_str());
    lastGameSearchExhausted = true;
    return false;
  }
  
  // Memory check
  if (ESP.getFreeHeap() < 100000) {
    Serial.printf("Insufficient memory: %d bytes\n", ESP.getFreeHeap());
    showDownloadProgress(0, "Low memory");
    delay(2000);
    return false;
  }
  
  showDownloadProgress(5, "Starting JSON search...");
  
  // STEP 1: JSON-based precise search (much simpler than XML streaming)
  Serial.printf("STEP 1: JSON precise search with CRC...\n");
  RomDetails romDetails = getCurrentRomDetails();
  
  Serial.printf("ROM Details check:\n");
  Serial.printf("  Available: %s\n", romDetails.available ? "YES" : "NO");
  Serial.printf("  Hash calculated: %s\n", romDetails.hashCalculated ? "YES" : "NO");
  Serial.printf("  CRC32 length: %d\n", romDetails.crc32.length());
  
  if (romDetails.available && romDetails.hashCalculated && romDetails.crc32.length() > 0) {
    lastRomHasCrc = true;
    lastRomCrcChecked = true;
    Serial.println("ROM data available - using JSON search");
    showDownloadProgress(20, "JSON CRC search...");
    
    Serial.printf("Calling searchWithJeuInfosPreciseJSON()...\n");
    GameInfo gameInfo = searchWithJeuInfosPreciseJSON(coreName, romDetails);
    
    Serial.printf("JSON search result:\n");
    Serial.printf("  Found: %s\n", gameInfo.found ? "YES" : "NO");
    Serial.printf("  Game ID: '%s'\n", gameInfo.gameId.c_str());
    Serial.printf("  Game Name: '%s'\n", gameInfo.gameName.c_str());
    Serial.printf("  Boxart URL length: %d\n", gameInfo.boxartUrl.length());
    
    if (gameInfo.found && gameInfo.boxartUrl.length() > 0) {
      Serial.printf("JSON SEARCH SUCCESS!\n");
      gameWasFound = true;
      Serial.printf("   Using mediaJeu.php download method\n");
      
      String exactFileName = getExactFileName(gameName);
      String searchCore = coreName;
      searchCore.toLowerCase();
      String savePath = getSavePath(exactFileName, searchCore);
      
      Serial.printf("Save path: %s\n", savePath.c_str());
      
      showDownloadProgress(50, "Downloading image...");
      Serial.printf("Calling downloadImageFromMediaJeu()...\n");
      success = downloadImageFromMediaJeu(gameInfo.boxartUrl, savePath);
      
      Serial.printf("MediaJeu download result: %s\n", success ? "SUCCESS" : "FAILED");
      
      if (success) {
        Serial.printf("JSON-BASED DOWNLOAD SUCCESS!\n");

        // --- GAME INFO panel: metadata prefetch. Same operation, warm
        // connection, gameId already resolved. A failed metadata fetch
        // NEVER affects the artwork result.
        {
          String metaPath = getMetaPathFromImagePath(savePath);
          if (!SD.exists(metaPath)) {
            showDownloadProgress(92, "Fetching game info...");
            GameMeta meta;
            if (fetchGameMetadataJSON(gameInfo.gameId, coreName, romDetails, meta)) {
              saveGameMeta(metaPath, meta);
              if (gameName == currentGame) { meta.forGame = gameName; currentMeta = meta; }
            }
          }
        }

        showDownloadProgress(100, "JSON download complete!");
        delay(1500);
        Serial.println("=== JSON DOWNLOAD COMPLETE ===\n");
        return true;
      } else {
        Serial.println("MediaJeu download failed");
      }
    } else {
      Serial.println("First JSON search found no results");
      
      // SECOND TRY: Retry CRC search with longer delay  
      Serial.printf("ATTEMPTING SECOND CRC SEARCH...\n");
      showDownloadProgress(35, "Retrying CRC search...");
      
      // WDT-safe wait: yields to OS in 100ms slices instead of blocking 10s
      Serial.printf("Waiting 10s before CRC retry (WDT-safe, yielding every 100ms)...\n");
      {
        unsigned long _waitStart = millis();
        while (millis() - _waitStart < 10000) {
          M5.update();
          screenshotServer.handleClient();
          delay(100);
        }
      }
      
      // Check memory before second attempt
      if (ESP.getFreeHeap() < 90000) {
        Serial.printf("Low memory for second attempt (%d bytes), skipping retry\n", ESP.getFreeHeap());
      } else {
        Serial.printf("Second attempt: Calling searchWithJeuInfosPreciseJSON()...\n");
        GameInfo gameInfoRetry = searchWithJeuInfosPreciseJSON(coreName, romDetails);
        
        Serial.printf("Second CRC search result:\n");
        Serial.printf("  Found: %s\n", gameInfoRetry.found ? "YES" : "NO");
        Serial.printf("  Game ID: '%s'\n", gameInfoRetry.gameId.c_str());
        Serial.printf("  Game Name: '%s'\n", gameInfoRetry.gameName.c_str());
        Serial.printf("  Boxart URL length: %d\n", gameInfoRetry.boxartUrl.length());
        
        if (gameInfoRetry.found && gameInfoRetry.boxartUrl.length() > 0) {
          Serial.printf("SECOND ATTEMPT SUCCESS!\n");
          gameWasFound = true;
          Serial.printf("   Using mediaJeu.php download method\n");
          
          String exactFileName = getExactFileName(gameName);
          String searchCore = coreName;
          searchCore.toLowerCase();
          String savePath = getSavePath(exactFileName, searchCore);
          
          showDownloadProgress(50, "Downloading image...");
          Serial.printf("Calling downloadImageFromMediaJeu() on retry...\n");
          success = downloadImageFromMediaJeu(gameInfoRetry.boxartUrl, savePath);
          
          Serial.printf("Retry MediaJeu download result: %s\n", success ? "SUCCESS" : "FAILED");
          
          if (success) {
            Serial.printf("RETRY-BASED DOWNLOAD SUCCESS!\n");

            // --- GAME INFO panel: metadata prefetch (see first-attempt hook)
            {
              String metaPath = getMetaPathFromImagePath(savePath);
              if (!SD.exists(metaPath)) {
                showDownloadProgress(92, "Fetching game info...");
                GameMeta meta;
                if (fetchGameMetadataJSON(gameInfoRetry.gameId, coreName, romDetails, meta)) {
                  saveGameMeta(metaPath, meta);
                  if (gameName == currentGame) { meta.forGame = gameName; currentMeta = meta; }
                }
              }
            }

            showDownloadProgress(100, "Retry download complete!");
            delay(1500);
            Serial.println("=== RETRY DOWNLOAD COMPLETE ===\n");
            return true;
          } else {
            Serial.println("Retry MediaJeu download failed");
          }
        } else {
          Serial.println("Second CRC search also found no results");
          // Second leg of the name-search fallback (F4): a valid-but-unindexed
          // hash (e.g. pack-built CHDs) reaches this point with a CRC that
          // ScreenScraper will never match. On allowlisted systems, try ONE
          // text search before giving up.
          if (isNameSearchSystem(systemId)) {
            success = tryNameSearchFallback(coreName, gameName, romDetails);
          }
          if (!success) {
            // ScreenScraper returned clean responses with no match (CRC twice,
            // plus name where applicable). Mark search as exhausted.
            lastGameSearchExhausted = true;
            Serial.println("Marked search as exhausted");
          }
        }
      }
    }
  } else {
    lastRomHasCrc = false;
    lastRomCrcChecked = true;
    Serial.println("ROM CRC not available for JSON search");

    // Hash-less fallback (F4, first leg): no usable CRC on an allowlisted
    // system. The allowlist is the guardian — on CRC-capable systems a
    // transient hash failure keeps behaving exactly as before.
    if (isNameSearchSystem(systemId)) {
      success = tryNameSearchFallback(coreName, gameName, romDetails);
      if (!success) {
        // Clean miss on a name-search system: stop the 10 s hammering.
        lastGameSearchExhausted = true;
        Serial.println("Name search found nothing - marked search as exhausted");
      }
    }
  }
  
  if (!success) {
    Serial.println("JSON method failed");
    // Game catalogued in ScreenScraper but with ZERO artwork of any configured
    // type: every mediaJeu attempt answered a clean NOMEDIA. That is a
    // definitive state — retrying every 10 s cannot change it and burns ~30
    // requests of the user's daily quota per cycle. Mark exhausted and say so.
    if (gameWasFound && g_mediaSawNoMedia && !g_mediaSawValidJpeg) {
      lastGameSearchExhausted = true;
      lastGameFoundNoMedia    = true;
      Serial.println("Game exists in SS but has no artwork of any type - marked exhausted");
      showDownloadProgress(0, "GAME HAS NO ARTWORK IN SS");
    } else if (g_lastSSHttpCode != 0 && g_lastSSHttpCode != 200) {
      showDownloadProgress(0, ssHudMessage(g_lastSSHttpCode));
    } else {
      showDownloadProgress(0, "Download failed");
    }
    delay(6000);
  }
  
  Serial.printf("Final result: %s\n", success ? "SUCCESS" : "FAILED");
  Serial.printf("Free heap at end: %d bytes\n", ESP.getFreeHeap());
  Serial.println("=== JSON DOWNLOAD COMPLETE ===\n");
  
  return success;
}

void updateArcadeSubsystemForCurrentGame(String coreName, String gameName) {
  Serial.printf("=== ENHANCED ARCADE SUBSYSTEM UPDATE ===\n");
  Serial.printf("Core: '%s'\n", coreName.c_str());
  Serial.printf("Game: '%s'\n", gameName.c_str());
  Serial.printf("Current lastArcadeSystemeId: '%s'\n", lastArcadeSystemeId.c_str());
  
  // Only process for Arcade cores with active game
  String coreNameLower = coreName;
  coreNameLower.toLowerCase();
  if (coreNameLower != "arcade" || gameName.length() == 0) {
    Serial.printf("Not Arcade or no game - skipping subsystem update\n");
    return;
  }
  
  // Perform lightweight search only to get systemeId (without downloading image)
  Serial.printf("Searching for subsystem ID for game: %s\n", gameName.c_str());
  
  RomDetails romDetails = getCurrentRomDetails();
  if (romDetails.available && romDetails.hashCalculated && romDetails.crc32.length() > 0) {
    Serial.printf("ROM data available, searching with CRC\n");
    
    // Use existing search function
    GameInfo gameInfo = searchWithJeuInfosPreciseJSON(coreName, romDetails);
    
    if (gameInfo.found && gameInfo.systemeId.length() > 0) {
      String oldSubsystemId = lastArcadeSystemeId;
      lastArcadeSystemeId = gameInfo.systemeId;
      
      Serial.printf("SUBSYSTEM UPDATE SUCCESS!\n");
      Serial.printf("   Previous subsystem: '%s'\n", oldSubsystemId.c_str());
      Serial.printf("   New subsystem: '%s'\n", lastArcadeSystemeId.c_str());
      Serial.printf("   Game: '%s'\n", gameInfo.gameName.c_str());
    } else {
      Serial.printf("Could not find subsystem ID for game: %s\n", gameName.c_str());
      
      // Don't clear immediately - preserve existing subsystem if available
      if (lastArcadeSystemeId.length() == 0) {
        Serial.printf("No existing subsystem to preserve\n");
      } else {
        Serial.printf("Preserving existing subsystem ID: %s (search failed for current game)\n", 
                     lastArcadeSystemeId.c_str());
        Serial.printf("   This prevents falling back to generic Arcade image\n");
      }
    }
  } else {
    Serial.printf("ROM CRC not available\n");
    
    // Don't clear subsystem on missing ROM data
    if (lastArcadeSystemeId.length() > 0) {
      Serial.printf("Preserving existing subsystem ID despite missing ROM data: %s\n", 
                   lastArcadeSystemeId.c_str());
    } else {
      Serial.printf("No existing subsystem to preserve\n");
    }
  }
  
  Serial.printf("=== ENHANCED SUBSYSTEM UPDATE COMPLETE ===\n");
}

void updateArcadeSubsystemForCurrentGameEnhanced(String coreName, String gameName, bool forceUpdate = false) {
  Serial.printf("=== ENHANCED ARCADE SUBSYSTEM UPDATE v2 ===\n");
  Serial.printf("Core: '%s'\n", coreName.c_str());
  Serial.printf("Game: '%s'\n", gameName.c_str());
  Serial.printf("Force update: %s\n", forceUpdate ? "YES" : "NO");
  Serial.printf("Current lastArcadeSystemeId: '%s'\n", lastArcadeSystemeId.c_str());
  Serial.printf("Last processed game: '%s'\n", lastProcessedGame.c_str());
  
  // Only process for MAME cores with active game
  String coreNameLower = coreName;
  coreNameLower.toLowerCase();
  if (coreNameLower != "arcade" || gameName.length() == 0) {
    Serial.printf("Not Arcade or no game - skipping subsystem update\n");
    return;
  }
  
  // Check if this is a different game or forced update
  bool isDifferentGame = (gameName != lastProcessedGame);
  bool shouldUpdate = forceUpdate || isDifferentGame;
  
  if (isDifferentGame) {
    Serial.printf("DIFFERENT GAME DETECTED: '%s' -> '%s'\n", lastProcessedGame.c_str(), gameName.c_str());
    Serial.printf("Clearing previous subsystem state for new game\n");
    lastArcadeSystemeId = "";
  }
  
  if (forceUpdate) {
    Serial.printf("FORCED UPDATE: bypassing all checks\n");
  }
  
  if (!shouldUpdate) {
    Serial.printf("No update needed - same game, no force\n");
    return;
  }
  
  // Perform search for subsystem ID
  Serial.printf("Searching for subsystem ID for game: %s\n", gameName.c_str());
  
  RomDetails romDetails = getCurrentRomDetails();
  if (romDetails.available && romDetails.hashCalculated && romDetails.crc32.length() > 0) {
    Serial.printf("ROM data available, searching with CRC: %s\n", romDetails.crc32.c_str());
    
    // Use existing search function
    GameInfo gameInfo = searchWithJeuInfosPreciseJSON(coreName, romDetails);
    
    if (gameInfo.found && gameInfo.systemeId.length() > 0) {
      String oldSubsystemId = lastArcadeSystemeId;
      lastArcadeSystemeId = gameInfo.systemeId;
      lastProcessedGame = gameName;
      
      Serial.printf("SUBSYSTEM UPDATE SUCCESS!\n");
      Serial.printf("   Previous subsystem: '%s'\n", oldSubsystemId.c_str());
      Serial.printf("   New subsystem: '%s'\n", lastArcadeSystemeId.c_str());
      Serial.printf("   Game: '%s'\n", gameInfo.gameName.c_str());
      Serial.printf("   Game processed: '%s'\n", lastProcessedGame.c_str());
      
      // If this is a different subsystem, log the change
      if (oldSubsystemId.length() > 0 && oldSubsystemId != lastArcadeSystemeId) {
        Serial.printf("SUBSYSTEM CHANGED: %s -> %s\n", oldSubsystemId.c_str(), lastArcadeSystemeId.c_str());
      }
    } else {
      Serial.printf("Could not find subsystem ID for game: %s\n", gameName.c_str());
      
      // Mark this game as processed even if no subsystem found
      lastProcessedGame = gameName;
      
      // For forced updates or different games, clear subsystem if not found
      if (forceUpdate || isDifferentGame) {
        Serial.printf("No subsystem found for new game - clearing\n");
        lastArcadeSystemeId = "";
      }
    }
  } else {
    Serial.printf("ROM CRC not available for game: %s\n", gameName.c_str());
    
    // Mark as processed even without CRC data
    lastProcessedGame = gameName;
    
    // For game changes without CRC, clear subsystem to force re-detection
    if (isDifferentGame || forceUpdate) {
      Serial.printf("Game change without CRC - clearing subsystem for re-detection\n");
      lastArcadeSystemeId = "";
    }
  }
  
  Serial.printf("=== ENHANCED SUBSYSTEM UPDATE COMPLETE ===\n");
  Serial.printf("Final state - Subsystem: '%s', Processed: '%s'\n", 
                lastArcadeSystemeId.c_str(), lastProcessedGame.c_str());
}