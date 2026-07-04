// ============================================================================
//  Build settings (Arduino IDE -> Tools menu):
//    Board:           ESP32 Dev Module
//    Partition:       Huge APP (3MB No OTA/1MB SPIFFS)
//    Flash Size:      4MB (32Mb)
//    PSRAM:           Disabled
//    Upload Speed:    921600
// ============================================================================

#include <XPT2046_Touchscreen.h>
#include <LovyanGFX.hpp>
#include "board_hal.h"
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

// Brownout register: available on ESP32/S2/S3, NOT on ESP32-P4.
// __has_include lets the code compile on any chip variant without errors.
#if __has_include("soc/rtc_cntl_reg.h")
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
  #define HAS_BROWNOUT_REG 1
#else
  #define HAS_BROWNOUT_REG 0
#endif

// CYD: dedicated HSPI bus for the SD card (TFT owns VSPI).
//   SD pinout:  SCK=18  MISO=19  MOSI=23  CS=5
SPIClass sdSPI(HSPI);

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
#define SCREENSCRAPER_SOFTWARE   "MiSTer-Monitor"
#define SAFE_JSON_BUFFER_SIZE    8192
#define MAX_RESPONSE_SIZE        50000

// Hardware constants — CYD ESP32-2432S028R (native 320x240)
#define TFCARD_CS_PIN     5      // CYD: SD card on HSPI, CS=GPIO5
#define TARGET_WIDTH      480
#define TARGET_HEIGHT     320
#define IMAGE_AREA_HEIGHT 270    // image area above the 50px footer band
#define ORIGINAL_WIDTH    480    // source design width (logical)
#define ORIGINAL_HEIGHT   320    // source design height (logical)
#define DISPLAY_WIDTH     480
#define DISPLAY_HEIGHT    320
#define SCALE_X           ((float)DISPLAY_WIDTH  / ORIGINAL_WIDTH)   // = 1.0
#define SCALE_Y           ((float)DISPLAY_HEIGHT / ORIGINAL_HEIGHT)  // = 1.0

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
bool wasConnected = false;
bool sdCardAvailable = false;
int currentPage = 0;
const int totalPages = 5;  // Back to 5 pages
unsigned long lastUpdate = 0;
unsigned long lastPageChange = 0;
unsigned long animTimer = 0;
int blinkState = 0;
bool needsRedraw = true;

// Screensaver variables
unsigned long lastButtonPress = 0;
const unsigned long SCREENSAVER_TIMEOUT = 30000;  // 30 seconds of inactivity

// Variables for automatic download
bool downloadInProgress = false;
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
  html += "Resolution: " + String(display.width()) + "x" + String(display.height()) + " &nbsp;|&nbsp; ";
  html += "IP: " + ip + ":8080</p>";
  html += "</body></html>";
  screenshotServer.send(200, "text/html", html);
}

void handleScreenshot() {
  int w = display.width();   // 320
  int h = display.height();  // 240

  // Standard 24-bit RGB888 BMP — universally supported by all browsers and viewers
  // Row stride must be padded to 4-byte boundary
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
  uint16_t* rowSrc = (uint16_t*)malloc(w * 2);
  uint8_t*  rowDst = (uint8_t*) malloc(ROW_STRIDE);

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
    display.readRect(0, y, w, 1, rowSrc);

    // Convert RGB565 → RGB888 in-place into rowDst
    // BMP stores pixels as B, G, R (reversed byte order)
    for (int x = 0; x < w; x++) {
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
void showBootSequence();
void connectWithAnimation();
void buttonPressFeedback(TouchButton* btn, void (*soundFn)());
void testMiSTerConnectivity(bool discovered);
void showReconnectBanner();
void updateMiSTerData();
void getCurrentCore();
void getCurrentGame();
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
void drawFooter();
void drawMiSTerLogo(int x, int y);
void drawPanel(int x, int y, int w, int h, uint16_t color);
void drawMiniPanel(int x, int y, int w, int h, String label, String value, uint16_t color);
void drawProgressBar(int x, int y, int w, int h, float percent);
void drawStorageBar(int x, int y, int w, int h, float percent);
void drawDigitalClock(int x, int y, String time, String label);
void drawStatusIndicator(int x, int y, uint16_t color, bool active);
void drawRadarScan(int centerX, int centerY, int radius, int angle);
void drawWiFiProgressCircles(int currentAttempt, bool connected, int maxAttempts = 30);
void drawPortArray(int x, int y, int usbCount, int serialCount);
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
void showCoreImageScreenWithAutoDownload(String coreName);
void showCoreDownloadingScreen(String coreName);
bool downloadCoreImageStreamingSafe(String baseUrl, String savePath);
String extractMediaUrl(String response, String mediaKey);
void showMenuImageWithCoreOverlay(String coreName);
void forceMemoryCleanup();
String buildCorrectMediaJeuUrl(String gameId, String systemId, String mediaType, String specificSystemeId = "");
void initScrollText(ScrollTextState* state, String text, int maxDisplayChars);
String getScrolledText(ScrollTextState* state);

// ========== SCREEN RENDERING OPTIMIZATION ==========
bool backgroundLoaded = false;  // For frame02.jpg (interface screens)
bool bootFrameLoaded = false;   // For frame01.jpg (boot/connection screens)

GameInfo searchWithJeuInfosPreciseJSON(String coreName, RomDetails romDetails);
bool downloadGameBoxartStreamingSafeJSON(String coreName, String gameName);

String currentGameForCrc = "";         // Current game that needs a CRC
String currentCoreForCrc = "";         // Core of the current game  
unsigned long lastCrcRecurrentTime = 0; // Last recurrent CRC attempt
bool crcRecurrentActive = false;       // Whether the recurrent search is active
int crcRecurrentAttempts = 0;          // Recurring attempt counter
bool lastRomHasCrc           = false;  // true when current game's ROM has a valid CRC
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

class ScaledDisplay {
private:
  // CYD: native 320x240 panel — no scaling, no offset needed.
  // The wrapper is kept for API compatibility with the Tab5 codebase.
  static constexpr float SCALE_FACTOR = 1.0f;
  static constexpr int   OFFSET_X     = 0;
  static constexpr int   OFFSET_Y     = 0;
  
  // Internal scaling helper functions
  // These convert logical coordinates to physical coordinates
  inline int scaleX(int x) { return (int)(x * SCALE_FACTOR) + OFFSET_X; }
  inline int scaleY(int y) { return (int)(y * SCALE_FACTOR) + OFFSET_Y; }
  inline int scaleW(int w) { return (int)(w * SCALE_FACTOR); }
  inline int scaleH(int h) { return (int)(h * SCALE_FACTOR); }
  
public:
  // ========== DRAWING PRIMITIVES WITH AUTO-SCALING ==========
  
  void fillRect(int x, int y, int w, int h, uint16_t color) {
    display.fillRect(scaleX(x), scaleY(y), scaleW(w), scaleH(h), color);
  }
  
  void drawRect(int x, int y, int w, int h, uint16_t color) {
    display.drawRect(scaleX(x), scaleY(y), scaleW(w), scaleH(h), color);
  }
  
  void fillScreen(uint16_t color) {
    display.fillScreen(color);
  }
  
  void drawFastHLine(int x, int y, int w, uint16_t color) {
    display.drawFastHLine(scaleX(x), scaleY(y), scaleW(w), color);
  }
  
  void drawFastVLine(int x, int y, int h, uint16_t color) {
    display.drawFastVLine(scaleX(x), scaleY(y), scaleH(h), color);
  }
  
  void drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    display.drawLine(scaleX(x0), scaleY(y0), scaleX(x1), scaleY(y1), color);
  }
  
  void fillCircle(int x, int y, int r, uint16_t color) {
    display.fillCircle(scaleX(x), scaleY(y), (int)(r * SCALE_FACTOR), color);
  }
  
  void drawCircle(int x, int y, int r, uint16_t color) {
    display.drawCircle(scaleX(x), scaleY(y), (int)(r * SCALE_FACTOR), color);
  }
  
  // ========== TEXT OPERATIONS WITH AUTO-SCALING ==========
  
  void setCursor(int x, int y) {
    display.setCursor(scaleX(x), scaleY(y));
  }
  
  void setTextColor(uint16_t color) {
    display.setTextColor(color);
  }
  
  void setTextColor(uint16_t fg, uint16_t bg) {
    display.setTextColor(fg, bg);
  }
  
  void setTextSize(float size) {
    // LovyanGFX supports fractional text scaling on this panel, so labels can
    // grow "a little" (e.g. 1.5x) instead of jumping to the next integer step.
    // SCALE_FACTOR stays 1.0; existing integer call sites (setTextSize(2)/(3))
    // implicitly convert to float and render identically to before.
    display.setTextSize(size * SCALE_FACTOR);
  }

  void setTextWrap(bool wrap) {
    display.setTextWrap(wrap);
  }
  
  void print(const char* text) {
    display.print(text);
  }
  
  void print(String text) {
    display.print(text);
  }
  
  void print(char c) {
    display.print(c);
  }
  
  void print(int num) {
    display.print(num);
  }
  
  void print(float num, int decimalPlaces = 2) {
    display.print(num, decimalPlaces);
  }
  
  void println(const char* text) {
    display.println(text);
  }
  
  void println(String text) {
    display.println(text);
  }
  
  void println(char c) {
    display.println(c);
  }
  
  void println(int num) {
    display.println(num);
  }
  
  void println(float num, int decimalPlaces = 2) {
    display.println(num, decimalPlaces);
  }
  
  void printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    display.print(buffer);
  }
  
  // ========== IMAGE OPERATIONS (PASS THROUGH WITHOUT SCALING) ==========
  // JPEG images are already at correct resolution, so we use Board.Display directly
  
  void pushImage(int x, int y, int w, int h, uint16_t* data) {
    // Images are NOT scaled - they're already at target resolution
    display.pushImage(x, y, w, h, data);
  }
};

// Create global instance of scaled display
// This replaces all Board.Lcd calls in the original code
ScaledDisplay Lcd;

// ========== TOUCH BUTTON INSTANCES ==========
// CYD35C: three horizontal tap zones in the footer band (Y=270..319).
// Left / center / right thirds of the 480px-wide panel.
TouchButton btnPrev = {   0, 270, 160, 50, "PRV", THEME_GREEN, false };
TouchButton btnScan = { 160, 270, 160, 50, "SCN", THEME_GREEN, false };
TouchButton btnNext = { 320, 270, 160, 50, "NXT", THEME_GREEN, false };

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
  Lcd.fillScreen(THEME_BLACK);

  // Header (matches the taller drawHeader style: logo + 2 lines + status)
  drawMiSTerLogo(10, 6);
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1);
  Lcd.setCursor(120, 8);
  Lcd.print("SCREENSCRAPER");
  Lcd.setCursor(120, 21);
  Lcd.print("AUTO-DOWNLOAD");
  drawStatusIndicator(462, 18, THEME_GREEN, true);
  Lcd.drawFastHLine(0, 42, 480, THEME_YELLOW);
  Lcd.drawFastHLine(0, 43, 480, THEME_YELLOW);

  // Main panel (blue = game artwork download) — centered: Y 88..192
  drawPanel(10, 88, 460, 104, THEME_BLUE);

  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1.0f);
  Lcd.setCursor(20, 98);
  Lcd.print("DOWNLOADING GAME IMAGE");

  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(20, 118);
  Lcd.printf("CORE: %s", coreName.c_str());

  // 480px width fits a much longer game name than the 25-char CYD limit
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setCursor(20, 140);
  String displayGame = gameName.length() > 44 ?
    gameName.substring(0, 44) + "..." : gameName;
  Lcd.printf("GAME: %s", displayGame.c_str());

  Lcd.setTextColor(THEME_GREEN);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(20, 164);
  Lcd.print("Searching ScreenScraper database...");

  // Bottom hint (no nav footer on this modal)
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(10, 296);
  Lcd.print("Please wait - downloading from ScreenScraper.fr");
}

void showDownloadProgress(int progress, String text) {
  // Clear ONLY the progress strip — below the panel (ends Y=192) and above
  // the bottom hint (Y=296). Must not erase either.
  Lcd.fillRect(10, 198, 460, 92, THEME_BLACK);

  // Bar frame — Y 204..228, full 440px wide
  Lcd.drawRect(20, 204, 440, 24, THEME_WHITE);
  Lcd.fillRect(21, 205, 438, 22, THEME_BLACK);

  // Bar fill with stoplight gradient
  if (progress > 0) {
    int fillWidth = (progress * 434) / 100;   // inner width = 460-26 ≈ 434
    uint16_t barColor = (progress < 30) ? THEME_YELLOW :
                        (progress < 70) ? THEME_CYAN   : THEME_GREEN;
    Lcd.fillRect(22, 206, fillWidth, 20, barColor);
  }

  // Percentage centered in the bar
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(228, 211);
  Lcd.printf("%d%%", progress);

  // Status line under the bar
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(20, 240);
  Lcd.print(text);

  // Wipe any leftover characters from a longer previous status (9 px/char at 1.5x)
  int textPx = text.length() * 9;
  if (textPx < 440) {
    Lcd.fillRect(20 + textPx, 240, 440 - textPx, 14, THEME_BLACK);
  }
}

// Visible window (in characters) for the game name in the image footer.
// 46 chars x 9 px (1.5x) ~= 414 px, fits after the "GAME:" label on 480 px.
#define GAME_FOOTER_VISIBLE_CHARS_FULL    46

void addGameImageFooter(String gameName) {
  Lcd.fillRect(0, 270, 480, 50, THEME_BLACK);
  Lcd.drawFastHLine(0, 270, 480, THEME_CYAN);

  Lcd.setTextWrap(false);

  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(10, 279);
  Lcd.print("GAME:");

  const int visibleChars = GAME_FOOTER_VISIBLE_CHARS_FULL;
  if (imageFooterScroll.fullText != gameName ||
      imageFooterScroll.maxChars != visibleChars) {
    initScrollText(&imageFooterScroll, gameName, visibleChars);
  }
  String displayGame = getScrolledText(&imageFooterScroll);
  while ((int)displayGame.length() < imageFooterScroll.maxChars) {
    displayGame += ' ';
  }

  Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
  Lcd.setCursor(64, 279);
  Lcd.print(displayGame);

  // "Touch screen for MiSTer monitor" = 31 chars x 9 px (1.5x) = 279 px -> centered
  Lcd.setTextColor(THEME_GREEN, THEME_BLACK);
  Lcd.setCursor(100, 301);
  Lcd.print("Touch screen for MiSTer monitor");
}

void drawCoreImageFooter() {
  Lcd.fillRect(0, 270, 480, 50, THEME_BLACK);
  Lcd.drawFastHLine(0, 270, 480, THEME_GREEN);

  bool hasGame = (currentGame.length() > 0);
  Lcd.setTextWrap(false);

  if (hasGame) {
    Lcd.setTextColor(THEME_CYAN);
    Lcd.setTextSize(1.5f);
    Lcd.setCursor(10, 279);
    Lcd.print("GAME:");

    const int visibleChars = GAME_FOOTER_VISIBLE_CHARS_FULL;
    if (imageFooterScroll.fullText != currentGame ||
        imageFooterScroll.maxChars != visibleChars) {
      initScrollText(&imageFooterScroll, currentGame, visibleChars);
    }
    String displayGame = getScrolledText(&imageFooterScroll);
    while ((int)displayGame.length() < imageFooterScroll.maxChars) {
      displayGame += ' ';
    }

    Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
    Lcd.setCursor(64, 279);
    Lcd.print(displayGame);

    Lcd.setTextColor(THEME_GREEN, THEME_BLACK);
    Lcd.setCursor(100, 301);
    Lcd.print("Touch screen for MiSTer monitor");

  } else {
    // Centered single line when no game is loaded
    Lcd.setTextColor(THEME_GREEN, THEME_BLACK);
    Lcd.setTextSize(1.5f);
    Lcd.setCursor(100, 290);
    Lcd.print("Touch screen for MiSTer monitor");
  }
}

void drawFooter() {
  // Footer band Y=270..319 (50px). Two text rows at 1.5x:
  //   row 1 (Y=279): live data — SYS uptime / GAME-CORE / IMG:OK
  //   row 2 (Y=301): touch navigation zones — <PRV  SCAN  NXT>
  Lcd.drawFastHLine(0, 270, 480, THEME_GREEN);
  Lcd.drawFastHLine(0, 271, 480, THEME_GREEN);

  // Clear both text rows (flicker-free repaint)
  Lcd.fillRect(0, 274, 480, 45, THEME_BLACK);

  Lcd.setTextWrap(false);
  Lcd.setTextSize(1.5f);

  // ===== ROW 1 (Y=279): live data =====================================

  // Left: uptime — skipped on the Network page (3), where the same info
  // already appears in the SESSION STATS block.
  if (currentPage != 3) {
    Lcd.setTextColor(THEME_CYAN, THEME_BLACK);
    Lcd.setCursor(8, 279);
    Lcd.printf("SYS:%02d:%02d",
               (int)((millis() / 60000) % 60),
               (int)((millis() / 1000)  % 60));
  }

  // Middle: GAME or CORE with scroll
  const int FOOTER_MID_X = 120;
  const int FOOTER_VIS   = 24;   // visible chars: 24 × 9 px (1.5x) = 216 px

  bool   hasGame = (currentGame.length() > 0);
  String label  = hasGame ? "G:" : "C:";
  String source = hasGame ? currentGame : currentCore;

  if (gameFooterScroll.fullText != source ||
      gameFooterScroll.maxChars != FOOTER_VIS) {
    initScrollText(&gameFooterScroll, source, FOOTER_VIS);
  }
  String displayName = getScrolledText(&gameFooterScroll);
  while ((int)displayName.length() < FOOTER_VIS) displayName += ' ';

  Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
  Lcd.setCursor(FOOTER_MID_X, 279);
  Lcd.print(label);
  Lcd.print(displayName);

  // Right: SD/image cache indicator
  if (sdCardAvailable) {
    Lcd.setTextColor(THEME_GREEN, THEME_BLACK);
    Lcd.setCursor(420, 279);
    Lcd.print("IMG:OK");
  }

  // ===== ROW 2 (Y=301): touch navigation zones ========================
  // Visual hint for btnPrev/Scan/Next — thirds of the 480px band.
  Lcd.drawFastVLine(160, 297, 18, 0x4208);
  Lcd.drawFastVLine(320, 297, 18, 0x4208);

  Lcd.setTextColor(THEME_GREEN, THEME_BLACK);
  Lcd.setCursor(62,  301);  Lcd.print("<PRV");   // left zone   (x 0..159)
  Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
  Lcd.setCursor(222, 301);  Lcd.print("SCAN");   // center zone (x 160..319)
  Lcd.setTextColor(THEME_GREEN, THEME_BLACK);
  Lcd.setCursor(382, 301);  Lcd.print("NXT>");   // right zone  (x 320..479)
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
  RomDetails details = {"", "", "", "", 0, false, false, false, "", "", 0};
  
  Serial.printf("=== GETTING ROM DETAILS ===\n");
  Serial.printf("Free heap before request: %d bytes\n", ESP.getFreeHeap());
  
  // Check memory before starting
  if (ESP.getFreeHeap() < 50000) {
    Serial.printf("Low memory for ROM details (%d bytes)\n", ESP.getFreeHeap());
    details.error = "Low memory";
    return details;
  }
  
  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/rom/details";
  
  Serial.printf("Requesting ROM details from: %s\n", url.c_str());
  
  http.begin(url);
  http.setTimeout(8000);
  http.addHeader("User-Agent", "MiSTer-Monitor");
  
  unsigned long requestStart = millis();
  int code = http.GET();
  unsigned long requestDuration = millis() - requestStart;
  
  Serial.printf("HTTP Response: %d (took %lu ms)\n", code, requestDuration);
  
  if (code == 200) {
    String response = http.getString();
    Serial.printf("ROM details response: %d bytes\n", response.length());
    
    // Show response preview
    Serial.printf("Response preview (first 200 chars):\n%s\n", 
                  response.substring(0, min(200, (int)response.length())).c_str());
    
    // SAFETY: Limit response size
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
    
  } else {
    Serial.printf("First attempt failed: HTTP %d\n", code);
    details.error = "HTTP " + String(code);
    
    String errorResponse = http.getString();
    if (errorResponse.length() > 0 && errorResponse.length() < 200) {
      Serial.printf("First attempt error response: %s\n", errorResponse.c_str());
    }
    
    http.end(); // Close first connection
    
    // SECOND ATTEMPT: Retry ROM details with longer delay
    Serial.printf("ATTEMPTING SECOND ROM DETAILS REQUEST...\n");
    Serial.printf("Waiting 20 seconds before retry...\n");
    
    // WDT-safe wait: yields to OS in 100ms slices instead of blocking 20s
    Serial.printf("Waiting 20s before retry (WDT-safe, yielding every 100ms)...\n");
    {
      unsigned long _waitStart = millis();
      while (millis() - _waitStart < 20000) {
        Board.update();                    // Keep touch state fresh
        screenshotServer.handleClient(); // Keep HTTP server alive
        delay(100);                     // Yield to FreeRTOS / feed WDT
      }
    }
    
    // Check memory before second attempt
    if (ESP.getFreeHeap() < 45000) {
      Serial.printf("Low memory for ROM details retry (%d bytes), skipping\n", ESP.getFreeHeap());
      details.error += " + Low memory for retry";
      Serial.printf("Free heap after ROM details: %d bytes\n", ESP.getFreeHeap());
      Serial.printf("=== ROM DETAILS COMPLETE (RETRY SKIPPED) ===\n\n");
      return details;
    }
    
    // Second attempt with increased timeout
    HTTPClient httpRetry;
    httpRetry.begin(url);
    httpRetry.setTimeout(12000); // Increased from 8000 to 12000
    httpRetry.addHeader("User-Agent", "MiSTer-Monitor");
    
    unsigned long retryStart = millis();
    int retryCode = httpRetry.GET();
    unsigned long retryDuration = millis() - retryStart;
    
    Serial.printf("Retry HTTP Response: %d (took %lu ms)\n", retryCode, retryDuration);
    
    if (retryCode == 200) {
      String retryResponse = httpRetry.getString();
      Serial.printf("ROM details retry response: %d bytes\n", retryResponse.length());
      
      // Show response preview
      Serial.printf("Retry response preview (first 200 chars):\n%s\n", 
                    retryResponse.substring(0, min(200, (int)retryResponse.length())).c_str());
      
      // SAFETY: Limit response size
      if (retryResponse.length() > 5000) {
        Serial.printf("Large ROM retry response (%d bytes), truncating\n", retryResponse.length());
        retryResponse = retryResponse.substring(0, 5000);
      }
      
      if (!_parseRomDetailsJson(retryResponse, details, "Retry ")) {
        httpRetry.end();
        Serial.printf("Free heap after ROM details: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("=== ROM DETAILS COMPLETE (RETRY MEMORY FAIL) ===\n\n");
        return details;
      }
      
    } else {
      Serial.printf("Retry also failed: HTTP %d\n", retryCode);
      details.error += " + Retry HTTP " + String(retryCode);
      
      String retryErrorResponse = httpRetry.getString();
      if (retryErrorResponse.length() > 0 && retryErrorResponse.length() < 200) {
        Serial.printf("Retry error response: %s\n", retryErrorResponse.c_str());
      }
    }
    
    httpRetry.end();
  }
  
  http.end(); // Ensure original connection is closed
  Serial.printf("Free heap after ROM details: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("=== ROM DETAILS COMPLETE ===\n\n");
  
  return details;
}

RomDetails getCurrentRomDetailsForced() {
  // Calls /status/rom/details?force=1 — the server bypasses timestamp checks
  // and reads CURRENTPATH/ACTIVEGAME directly to compute CRC.
  RomDetails details = {"", "", "", "", 0, false, false, false, "", "", 0};

  Serial.printf("=== FORCED ROM DETAILS (bypass timestamp) ===\n");

  if (ESP.getFreeHeap() < 50000) {
    details.error = "Low memory";
    return details;
  }

  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/rom/details?force=1";
  Serial.printf("Requesting forced ROM details from: %s\n", url.c_str());

  http.begin(url);
  http.setTimeout(15000);
  http.addHeader("User-Agent", "MiSTer-Monitor");

  int code = http.GET();
  Serial.printf("Forced HTTP Response: %d\n", code);

  if (code == 200) {
    String response = http.getString();
    Serial.printf("Forced response (%d bytes): %s\n", response.length(),
                  response.substring(0, min(200, (int)response.length())).c_str());

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
  } else {
    details.error = "Forced HTTP " + String(code);
    Serial.printf("Forced request failed: %d\n", code);
  }

  http.end();
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
  
  downloadInProgress = true;
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
    downloadInProgress = false;
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
        downloadInProgress = false;
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
    downloadInProgress = false;
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
      downloadInProgress = false;
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
  
  downloadInProgress = false;
  Serial.printf("Free heap after download: %d bytes\n", ESP.getFreeHeap());
  Serial.println("=== CORE IMAGE DOWNLOAD COMPLETE ===\n");
  
  return success;
}

// Core image download screen
void showCoreDownloadingScreen(String coreName) {
  Lcd.fillScreen(THEME_BLACK);

  // Header: text logo + label (same style as showDownloadingScreen)
  drawMiSTerLogo(10, 6);
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1.0f);
  Lcd.setCursor(120, 8);
  Lcd.print("SCREENSCRAPER");
  Lcd.setCursor(120, 21);
  Lcd.print("SYSTEM DOWNLOAD");
  drawStatusIndicator(462, 18, THEME_ORANGE, true);
  Lcd.drawFastHLine(0, 42, 480, THEME_YELLOW);
  Lcd.drawFastHLine(0, 43, 480, THEME_YELLOW);

  // Main panel (orange = system/core artwork download) — full width Y 48..152
  drawPanel(10, 48, 460, 104, THEME_ORANGE);

  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1.0f);
  Lcd.setCursor(20, 58);
  Lcd.print("DOWNLOADING SYSTEM IMAGE");

  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(20, 78);
  Lcd.printf("SYSTEM: %s", coreName.c_str());

  Lcd.setTextColor(THEME_GREEN);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(20, 102);
  Lcd.print("Searching ScreenScraper database...");

  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1.0f);
  Lcd.setCursor(20, 128);
  Lcd.print("Media: wheel > photo > illustration");

  // Footer hint
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(10, 296);
  Lcd.print("Downloading system artwork from ScreenScraper.fr");
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
  
  // CRITICAL FIX: For Arcade cores, ALWAYS ensure we have subsystem info
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
  
  // Decide if download is needed
  if (FORCE_CORE_REDOWNLOAD) {
    Serial.println("FORCE_CORE_REDOWNLOAD enabled - will download regardless of existing image");
    shouldDownload = true;
  } else if (!imageExists) {
    Serial.println("No core image found - will attempt download");
    shouldDownload = true;
  } else {
    Serial.println("Image exists - checking if it's the correct one...");
    
    // For Arcade, verify we have the right image
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
    
    // logic for subsystem images
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

void handleTouch() {
  // Step 1: Get current touch state from Board.Touch.getDetail
  // This returns a structure with all touch information
  auto touch = Board.Touch.getDetail();
  
  // Step 2: Check if this is a NEW touch event
  // wasPressed() is true only at the moment of initial contact
  // This prevents one touch from triggering multiple actions
  if (touch.wasPressed()) {
    
    // Step 3: Get physical touch coordinates (1280x720 space)
    int physicalX = touch.x;
    int physicalY = touch.y;
    
    // Step 4: Debug logging (helpful during development and testing)
    Serial.println("Touch detected!");
    Serial.printf("  Physical coordinates: (%d, %d)\n", physicalX, physicalY);
    // Serial.printf("  Logical coordinates: (%d, %d)\n", logicalX, logicalY);
    
    // Step 5: Check each button in sequence
    // Using if-else ensures only one button can be activated per touch
    
    // Check PREV button
    if (btnPrev.contains(physicalX, physicalY)) {
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
        Lcd.fillRect(btnScan.x + 10, btnScan.y + 24,
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
          Serial.printf("After SCAN rescan: CRC available = %s\n",
                        lastRomHasCrc ? "YES" : "NO");

          // SCAN ALWAYS forces a complete fresh image search, even when the
          // CRC was already known.
          lastSearchedGame        = "";
          lastGameImageOK         = false;
          lastGameSearchExhausted = false;

          // Re-run the game image pipeline now and show the result, instead
          // of just flagging a HUD redraw.
          btnScan.label   = originalLabel;
          scanInProgress  = false;
          lastButtonPress = millis();
          showGameImageScreen(currentCore, currentGame);
          showingCoreImage   = true;
          coreImageStartTime = millis();
          return;   // switched to the image screen; skip the HUD-redraw tail
        }
        
        // === Exit SCANNING state ===
        // Restore original label BEFORE the page redraw is triggered, so any
        // btnScan.draw() called from updateDisplay() picks up "SCAN" again.
        btnScan.label = originalLabel;
        scanInProgress = false;
        
        // Wipe the (now larger) "SCANNING" footprint and draw "SCAN"
        Lcd.fillRect(btnScan.x + 10, btnScan.y + 24,
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
  // ANTI-CRASH BLOCK — must run BEFORE Board.begin() and Serial
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
  auto cfg = Board.config();

  cfg.clear_display = true;
  cfg.output_power = true;
  cfg.internal_imu = false;
  cfg.external_imu = false;

  Board.begin(cfg);

  // ========== START SERIAL FIRST ==========
  Serial.begin(115200);
  delay(500);  // Give the serial time

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
  
  // CYD has no DAC speaker. Boot melody skipped; passive buzzer support
  // may be added in a later iteration.
  Serial.println("=== SPEAKER ===  (skipped: not present on CYD)");
  

  // Initialize speaker
  /*auto spk_cfg = Board.Speaker.config();
  spk_cfg.sample_rate = 48000;  // Sample rate
  spk_cfg.task_priority = 2;    // Task priority
  spk_cfg.task_pinned_core = PRO_CPU_NUM;
  spk_cfg.dma_buf_count = 8;
  spk_cfg.dma_buf_len = 256;
  
  Board.Speaker.config(spk_cfg);
  Board.Speaker.begin();
  Board.Speaker.setVolume(100);  // High volume (0-255)
  
  Serial.println("Speaker initialized");*/
  
  display.setRotation(3);      // 480x320 landscape flipped (microSD on top)
  display.setBrightness(128);
  display.setColorDepth(16);

  // CYD: confirm panel resolution after init so we can spot rotation/driver issues.
  Serial.printf("[DISPLAY] Resolution: %dx%d (expected 480x320 after rotation=3)\n",
                display.width(), display.height());
  Serial.printf("[DISPLAY] Color depth: %d bpp\n", display.getColorDepth());
  
  Serial.println("=== MiSTer Monitor with Core and Games Images Starting ===");
  Serial.printf("Display: %dx%d\n", display.width(), display.height());
  Serial.printf("Target MiSTer IP: %s\n", misterIP);
  
  Serial.println("=== INITIALIZING SD CARD ===");
  Serial.printf("Using CS pin: GPIO %d (HSPI bus)\n", TFCARD_CS_PIN);
  Serial.println("Mode: SPI at 25 MHz");

  // Start the dedicated HSPI bus for SD before SD.begin() can use it.
  sdSPI.begin(18 /*SCK*/, 19 /*MISO*/, 23 /*MOSI*/, TFCARD_CS_PIN);

  int sdRetries = 0;
  while (!SD.begin(TFCARD_CS_PIN, sdSPI, 25000000) && sdRetries < 5) {
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
  getCurrentCore();
  getCurrentGame();
  
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
  // Screenshot server intentionally NOT started on the CYD35C: its ST7796 panel
  // is write-only over SPI — framebuffer readback (RAMRD) returns 0x0000, so any
  // capture comes out all black. 
  // Re-enable only on a panel that supports readback.
  // if (WiFi.status() == WL_CONNECTED) {
  //   setupScreenshotServer();
  // }
}

void loop() {
  Board.update();
  screenshotServer.handleClient();  // Non-blocking screenshot server poll

  // --- Periodic MiSTer re-discovery while offline ---------------------------
  // Discovery runs once at boot. If the MiSTer wasn't on the network yet
  // (e.g. its IP was delayed by a CIFS mount), boot-time discovery fails and
  // the display would stay OFFLINE forever. While disconnected, re-broadcast
  // periodically to pick the MiSTer up once it appears, then refresh state.
  {
    static unsigned long lastRediscovery = 0;
    const unsigned long  REDISCOVERY_INTERVAL_MS = 10000;
    if (!connected && WiFi.status() == WL_CONNECTED &&
        millis() - lastRediscovery > REDISCOVERY_INTERVAL_MS) {
      lastRediscovery = millis();
      Serial.println("[REDISCOVERY] Offline — re-broadcasting for MiSTer...");
      if (discoverMister(2, 400)) {        // quick probe (<=0.8s); sets misterIP
        Serial.printf("[REDISCOVERY] Found at %s — back online\n", misterIP);
        updateMiSTerData();                // getCurrentCore() sets connected on HTTP 200
        needsRedraw = true;                // next loop repaints the normal UI over it
      }
    }
  }
  // --------------------------------------------------------------------------
  // Reconnect banner: fire on any OFFLINE -> ONLINE transition, no matter which
  // path restored the link (re-discovery, screensaver refresh, normal polling).
  if (connected && !wasConnected) {
    Serial.println("[RECONNECT] Link restored — showing banner");
    showReconnectBanner();
    delay(2400);
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
  auto touch = Board.Touch.getDetail();
  if (touch.wasPressed()) {
    int tx = touch.x;
    int ty = touch.y;
    
  // === Default: exit to monitor ===
    Serial.println("Touch detected - exiting core image to interface");
    // Full-width banner flush against the TOP edge of the screen
    Lcd.fillRect(0, 0, 480, 34, THEME_CYAN);
    Lcd.setTextSize(2);
    Lcd.setTextColor(THEME_BLACK);
    // "LOADING..." = 10 chars × 12 px (size 2) = 120 px -> centered on 480
    Lcd.setCursor(180, 9);
    Lcd.print("LOADING...");
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
      getCurrentCore();
      getCurrentGame();
      
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
            
            // CYD 3.5": match addGameImageFooter()/drawCoreImageFooter()
            // exactly — the GAME: value is drawn at x=64, y=279, size 1.5x.
            Lcd.setTextWrap(false);
            Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
            Lcd.setTextSize(1.5f);
            Lcd.setCursor(64, 279);
            Lcd.print(scrolledText);
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
  if (!showingCoreImage && sdCardAvailable && (millis() - lastButtonPress > SCREENSAVER_TIMEOUT)
      && (currentCore != coreDownloadFailedFor || currentGame.length() > 0 || FORCE_CORE_REDOWNLOAD)) {
    Serial.println("=== SCREENSAVER ACTIVATION ANALYSIS ===");
    Serial.printf("Current core: '%s'\n", currentCore.c_str());
    Serial.printf("Current game: '%s'\n", currentGame.c_str());
    Serial.printf("Connected: %s\n", connected ? "YES" : "NO");
    Serial.printf("Time since last button: %lu ms\n", millis() - lastButtonPress);
    
    // Force fresh data check before screensaver
    Serial.println("Forcing fresh data check before screensaver...");
    String oldCore = currentCore;
    String oldGame = currentGame;
    
    // Get fresh data from MiSTer
    getCurrentCore();
    getCurrentGame();
    
    if (oldCore != currentCore) {
      Serial.printf("Core changed during screensaver check: '%s' -> '%s'\n", oldCore.c_str(), currentCore.c_str());
    }
    if (oldGame != currentGame) {
      Serial.printf("Game changed during screensaver check: '%s' -> '%s'\n", oldGame.c_str(), currentGame.c_str());
    }
    
    Serial.println("Activating screensaver - showing image due to inactivity");
    
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
  
  // Only redraw when necessary
  if (needsRedraw) {
    updateDisplay();
    needsRedraw = false;
  }
  
  static unsigned long lastFooterScrollUpdate = 0;
  if (millis() - lastFooterScrollUpdate > 100) { // Update every 100ms
    if (!showingCoreImage && gameFooterScroll.needsScroll) {
      String textToShow = (currentGame.length() > 0) ? currentGame : currentCore;

      if (gameFooterScroll.fullText == textToShow) {
        // Match drawFooter() row 1 exactly: size 1.5x, the value starts right
        // after the 2-char "G:"/"C:" label at x = FOOTER_MID_X(120) + 2*9 = 138,
        // y = 279. Only the value zone is repainted (label is static);
        // flicker-free via setTextColor(fg, bg).
        const int nameX = 120 + 2 * 9;   // 138  (FOOTER_MID_X + "G:"/"C:" at 1.5x)

        String displayText = getScrolledText(&gameFooterScroll);
        while ((int)displayText.length() < gameFooterScroll.maxChars) {
          displayText += ' ';
        }

        Lcd.setTextWrap(false);
        Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
        Lcd.setTextSize(1.5f);
        Lcd.setCursor(nameX, 279);
        Lcd.print(displayText);
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
  
  // Subtle animations only for specific elements
  if (millis() - animTimer > 1000) {
    blinkState = (blinkState + 1) % 4;
    
    // Only update status indicators without clearing screen
    if (currentPage == 0) {
      drawStatusIndicator(462, 18, connected ? THEME_GREEN : THEME_RED, connected && blinkState < 2);
    }
    animTimer = millis();
  }
  
  delay(needsRedraw || showingCoreImage ? 10 : 100);
}

void initSDCard() {
  Serial.println("\n=== Initializing SD card ===");
  
  // Give WiFi time to stabilize
  delay(500);
  
  // Try to initialize SD
  if (SD.begin(TFCARD_CS_PIN, sdSPI, 25000000)) {
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
  http.addHeader("User-Agent", "MiSTer-Monitor");
  
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
  http.addHeader("User-Agent", "MiSTer-Monitor");
  
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


bool findCoreImage(String coreName, String &imagePath) {
  Serial.printf("=== ENHANCED CORE IMAGE FINDER ===\n");
  Serial.printf("Core: '%s'\n", coreName.c_str());
  Serial.printf("lastArcadeSystemeId: '%s' (length: %d)\n", lastArcadeSystemeId.c_str(), lastArcadeSystemeId.length());
  
  // prioritize subsystem image if available and confirmed
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
    // Use Board.Display directly (not Lcd) for image rendering
    Lcd.pushImage(finalX, finalY, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  } else if (finalY + pDraw->iHeight > IMAGE_AREA_HEIGHT &&
             finalX >= 0 && finalX + pDraw->iWidth <= TARGET_WIDTH) {
    // Expected: block clips into the footer band. Silenced — not an error.
  } else {
    // True out-of-bounds (wrong X or negative Y): log for diagnosis.
    Serial.printf("jpegDrawCallback: Block out of bounds at (%d,%d) size %dx%d\n",
                  finalX, finalY, pDraw->iWidth, pDraw->iHeight);
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
  
  // Show menu image with core overlay instead of "image not found"
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
    
    // CYD: delegate to the shared 320x240-native footer.
    drawCoreImageFooter();
    
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
  
  // ========== IMAGE DISPLAY AND OVERLAY ==========
  if (menuImageFound && displayCoreImage(menuImagePath)) {
    Serial.println("Menu image displayed, adding enhanced overlay");
    
    // INTEGRATED LOGIC: Determine overlay type based on multiple sources
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
      
      Lcd.fillRect(1100, 20, 160, 40, THEME_BLACK);
      Lcd.drawRect(1100, 20, 160, 40, THEME_RED);
      Lcd.setTextColor(THEME_RED);
      Lcd.setTextSize(2);
      Lcd.setCursor(1115, 32);
      
      if (displayError == "NO SERVER") {
        Lcd.print("NO SERVER");
      } else if (displayError == "TIMEOUT") {
        Lcd.print("TIMEOUT");
      } else if (displayError == "OFFLINE") {
        Lcd.print("OFFLINE");
      } else if (displayError == "DISCONNECTED") {
        Lcd.print("DISCONN.");
      } else if (displayError.startsWith("ERROR")) {
        Lcd.print("ERROR");
      } else {
        Lcd.print(displayError.substring(0, 10));
      }
      
      Serial.printf("Error overlay displayed: %s\n", displayError.c_str());
    }
    
  } else {
    // ========== FALLBACK IF THERE IS NO MENU IMAGE ==========
    Serial.println("No menu image found, showing main HUD");
    displayMainHUD();
  }

  // ========== FOOTER FOR NON-MENU CORES ==========
  // CYD: delegate to the shared 320x240-native footer. drawCoreImageFooter()
  // already handles the hasGame case (GAME: scrolling line) vs the no-game
  // case (single touch hint).
  drawCoreImageFooter();
}

void showCoreNotFoundScreen(String coreName) {
  Lcd.fillScreen(THEME_BLACK);

  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(2);
  Lcd.setCursor(10, 12);
  Lcd.print("MiSTer Monitor");

  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(10, 58);
  Lcd.print("Current Core:");

  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(2);
  Lcd.setCursor(10, 78);
  String displayCore = coreName.length() > 24 ? coreName.substring(0, 24) + "..." : coreName;
  if (displayCore.equalsIgnoreCase("arcade")) displayCore = "Arcade";
  Lcd.print(displayCore);

  bool shouldShowError = serverHasError || isErrorCore(coreName);
  if (shouldShowError) {
    Lcd.setTextColor(THEME_RED);
    Lcd.setTextSize(1);
    Lcd.setCursor(10, 120);
    Lcd.print("System Status: ERROR");

    // Error badge top-right (was off-screen at x=1100)
    Lcd.fillRect(360, 10, 110, 34, THEME_BLACK);
    Lcd.drawRect(360, 10, 110, 34, THEME_RED);
    Lcd.setTextColor(THEME_RED);
    Lcd.setTextSize(2);
    Lcd.setCursor(378, 20);
    Lcd.print("ERROR");
  } else {
    Lcd.setTextColor(THEME_GREEN);
    Lcd.setTextSize(1);
    Lcd.setCursor(10, 120);
    Lcd.print("System Status: ACTIVE");
  }

  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1);
  Lcd.setCursor(10, 160);
  Lcd.print("No menu image found");
  Lcd.setCursor(10, 176);
  Lcd.print("Check SD card: /cores/menu.jpg");

  // Footer (was Tab5 coords 0,621,1280,99)
  Lcd.drawFastHLine(0, 283, 480, THEME_GREEN);
  Lcd.fillRect(0, 285, 480, 35, THEME_BLACK);
  Lcd.setTextColor(THEME_GREEN);
  Lcd.setTextSize(1);
  Lcd.setCursor(110, 300);
  Lcd.print("Press any button for interface");
}


/**
 * Play button feedback sounds
 * Each button has a distinct tone for better UX
 */

// PREV button sound - lower pitch (800 Hz)
void playPrevButtonSound() {
  Board.Speaker.tone(800, 80);
  Serial.println("PREV sound");
}

void playScanButtonSound() {
  Board.Speaker.tone(1200, 80);
  Serial.println("SCAN sound");
}

void playNextButtonSound() {
  Board.Speaker.tone(1600, 80);
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

void showBootSequence() {
  Lcd.fillScreen(THEME_BLACK);

  // Terminal-style boot header (centered on the 480px width, size 3)
  Lcd.setTextColor(THEME_GREEN);
  Lcd.setTextSize(3);
  Lcd.setCursor(186, 50);
  Lcd.print("SYSTEM");

  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setCursor(150, 85);
  Lcd.print("INITIALIZE");

  // Boot lines with typing effect
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1);
  String bootLines[] = {
    "> Loading MiSTer interface...",
    "> Initializing SD card system...",
    "> Loading core image decoder...",
    "> Establishing connection protocols...",
    "> System ready - ONLINE"
  };

  for (int i = 0; i < 5; i++) {
    Lcd.setCursor(20, 140 + i * 18);
    for (int j = 0; j < (int)bootLines[i].length(); j++) {
      Lcd.print(bootLines[i].charAt(j));
      delay(20);
    }
    delay(100);
  }

  const int barX = 60, barY = 250, barW = 360, barH = 16;   // wider bar
  Lcd.drawRect(barX, barY, barW, barH, THEME_WHITE);
  Lcd.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, THEME_BLACK);

  for (int p = 0; p <= 100; p += 2) {
    int fillW = (p * (barW - 4)) / 100;
    if (fillW > 0) {
      Lcd.fillRect(barX + 2, barY + 2, fillW, barH - 4, THEME_GREEN);
      Lcd.drawFastHLine(barX + 2, barY + 2, fillW, THEME_WHITE);
    }
    delay(8);
  }
  delay(500);
}

// Draw WiFi connection progress circles to the RIGHT of the radar.
// Blue while attempting, all green on success (ported from the Tab5 layout).
// 10 circles per row, up to 3 rows = 30 attempts.
void drawWiFiProgressCircles(int currentAttempt, bool connected, int maxAttempts) {
  const int startX       = 205;   // just right of the radar (radar ends ~x170)
  const int startY       = 125;   // 3 rows centered on the radar's center (y=150)
  const int circleRadius = 8;
  const int cell         = circleRadius * 2 + 9;   // 25 px between centers
  const int perRow       = 10;

  int circlesToDraw = min(currentAttempt, maxAttempts);

  // Clear the whole 3-row grid so colours refresh cleanly (radar is at x<=170).
  Lcd.fillRect(startX - circleRadius - 1, startY - circleRadius - 1,
               perRow * cell + 4, 3 * cell + 4, THEME_BLACK);

  for (int i = 0; i < circlesToDraw; i++) {
    int row = i / perRow;
    int col = i % perRow;
    int x = startX + col * cell;
    int y = startY + row * cell;

    uint16_t fillColor = connected ? THEME_GREEN : THEME_BLUE;
    Lcd.fillCircle(x, y, circleRadius, fillColor);
    Lcd.drawCircle(x, y, circleRadius + 1, THEME_CYAN);   // border
    if (connected) {
      Lcd.fillCircle(x - 3, y - 3, 3, THEME_WHITE);       // 3D highlight
    }
  }
}

void connectWithAnimation() {
  Lcd.fillScreen(THEME_BLACK);

  // Header: text logo + "CONNECTING" + full-width divider (matches drawHeader)
  drawMiSTerLogo(10, 6);
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(2);
  Lcd.setCursor(130, 16);
  Lcd.print("CONNECTING");
  Lcd.drawFastHLine(0, 42, 480, THEME_YELLOW);
  Lcd.drawFastHLine(0, 43, 480, THEME_YELLOW);

  // WiFi scan diagnostic (serial only — unchanged)
  Serial.println("=== SCANNING NETWORKS ===");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    Serial.printf("  [%d] SSID: %s  RSSI: %d  Auth: %d\n",
      i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i));
  }
  Serial.println("=== END SCAN ===");
  Serial.printf("SSID: %s\n", ssid);

  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    // Radar on the LEFT
    drawRadarScan(110, 150, 60, attempts * 12);

    // Progress circles to the RIGHT of the radar (blue while attempting)
    drawWiFiProgressCircles(attempts + 1, false, 30);

    // SCAN counter, enlarged. setTextColor(fg, bg) repaints the background each
    // frame so the digits no longer pile up on top of each other.
    Lcd.setTextColor(THEME_YELLOW, THEME_BLACK);
    Lcd.setTextSize(2);
    Lcd.setCursor(180, 218);
    Lcd.printf("SCAN %02d/30", attempts + 1);

    Serial.printf("WiFi attempt %d/30...\n", attempts + 1);
    delay(500);
    attempts++;
  }

  // Clear result area (below the radar / circles / counter)
  Lcd.fillRect(0, 240, 480, 70, THEME_BLACK);

  if (WiFi.status() == WL_CONNECTED) {
    // Turn every attempted circle green
    drawWiFiProgressCircles(attempts, true, 30);

    Lcd.setTextColor(THEME_GREEN);
    Lcd.setTextSize(3);
    Lcd.setCursor(159, 252);
    Lcd.print("CONNECTED");

    Lcd.setTextColor(THEME_CYAN);
    Lcd.setTextSize(2);
    {
      String ipLine = "IP: " + WiFi.localIP().toString();
      Lcd.setCursor((480 - (int)ipLine.length() * 12) / 2, 282);
      Lcd.print(ipLine);
    }

    Serial.printf("Assigned IP: %s\n", WiFi.localIP().toString().c_str());

    drawStatusIndicator(440, 260, THEME_GREEN, true);

    delay(1000);
    Lcd.fillRect(0, 240, 480, 70, THEME_BLACK);
    Lcd.setTextColor(THEME_YELLOW);
    Lcd.setTextSize(2);
    Lcd.setCursor((480 - 17 * 12) / 2, 256);   // = 138
    Lcd.print("Testing MiSTer...");

    // Auto-discover the MiSTer server IP.
    // On success overwrites misterIP; on failure leaves the config.ini value unchanged.
    Serial.println("=== DISCOVERING MiSTer SERVER ===");
    bool discovered = discoverMister();

    Serial.println("=== TESTING MiSTer CONNECTIVITY ===");
    testMiSTerConnectivity(discovered);
  } else {
    Lcd.setTextColor(THEME_RED);
    Lcd.setTextSize(2);
    Lcd.setCursor(159, 256);
    Lcd.print("WIFI FAILED");
    Serial.printf("Error connecting WiFi!\n");
    drawStatusIndicator(440, 260, THEME_RED, false);
  }
  delay(2000);
}

void testMiSTerConnectivity(bool discovered) {
  const int CW   = 12;              // ancho de carácter a setTextSize(2), SCALE_FACTOR 1.0
  const int ROW1 = 244, ROW2 = 268; // dos filas separadas, debajo del radar

  if (strlen(misterIP) == 0) {
    Serial.println("MiSTer IP unknown: discovery failed and no ip= in config.ini");
    Lcd.fillRect(0, 214, 480, 96, THEME_BLACK);
    Lcd.setTextSize(2);
    Lcd.setTextColor(THEME_RED);
    const char* a = "MiSTer NOT FOUND";
    Lcd.setCursor((480 - (int)strlen(a) * CW) / 2, ROW1);
    Lcd.print(a);
    Lcd.setTextColor(THEME_YELLOW);
    const char* b = "Set ip= in config.ini";
    Lcd.setCursor((480 - (int)strlen(b) * CW) / 2, ROW2);
    Lcd.print(b);
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

  Lcd.fillRect(0, 214, 480, 96, THEME_BLACK);
  Lcd.setTextSize(2);

  if (code == 200) {
    Serial.printf("MiSTer responds correctly!\n");
    Lcd.setTextColor(THEME_GREEN);
    const char* a = "MiSTer: ONLINE";
    Lcd.setCursor((480 - (int)strlen(a) * CW) / 2, ROW1);
    Lcd.print(a);
    Lcd.setTextColor(THEME_CYAN);
    String b = "Server: " + String(misterIP) + ":8081";
    Lcd.setCursor((480 - (int)b.length() * CW) / 2, ROW2);
    Lcd.print(b);
    connected = true;
  } else {
    connected = false;
    Serial.printf("MiSTer not responding (code: %d)\n", code);
    Lcd.setTextColor(THEME_RED);
    if (discovered) {
      const char* a = "MiSTer FOUND, no reply";
      Lcd.setCursor((480 - (int)strlen(a) * CW) / 2, ROW1);
      Lcd.print(a);
      Lcd.setTextColor(THEME_YELLOW);
      const char* b = "Is the script running?";
      Lcd.setCursor((480 - (int)strlen(b) * CW) / 2, ROW2);
      Lcd.print(b);
    } else {
      const char* a = "MiSTer: OFFLINE";
      Lcd.setCursor((480 - (int)strlen(a) * CW) / 2, ROW1);
      Lcd.print(a);
      Lcd.setTextColor(THEME_YELLOW);
      String b = "Check IP: " + String(misterIP);
      Lcd.setCursor((480 - (int)b.length() * CW) / 2, ROW2);
      Lcd.print(b);
    }
  }

  http.end();
  delay(2000);
}

void showReconnectBanner() {
  // Footer-style banner in the image footer band (Y=270..320). Drawn inside the
  // band so the footer restore below fully covers it.
  const char* msg = "CONNECTED TO MiSTer";
  int tw = (int)strlen(msg) * 12;            // ~12 px/char at 1.5x
  Lcd.fillRect(0, 270, 480, 50, THEME_GREEN);
  Lcd.setTextSize(2);
  Lcd.setTextColor(THEME_BLACK, THEME_GREEN);
  Lcd.setCursor((480 - tw) / 2, 286);        // centered in the 50px band
  Lcd.print(msg);

  delay(2400);                               // hold long enough to read

  // Restore the image footer over the banner.
  if (currentGame.length() > 0) addGameImageFooter(currentGame);
  else                          drawCoreImageFooter();
}

void updateMiSTerData() {
  Serial.println("=== Updating MiSTer data ===");
  getCurrentCore();
  getCurrentGame();
  getSystemData();
  getStorageData();
  getUSBData();
  getNetworkAndSession();
  Serial.println("=== Update complete ===");
}

void getCurrentCore() {
  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/core";
  
  Serial.printf("Connecting to: %s\n", url.c_str());
  
  http.begin(url);
  http.setTimeout(8000);
  http.addHeader("User-Agent", "MiSTer-Monitor");
  
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
  lastGameImageOK          = false;
  
  // Short-circuit: if the core is not in ScreenScraper's DB, do not activate
  // the recurrent. ScreenScraper requires a system ID, and asking MiSTer for
  // ROM details (which calculates CRC32 over the file) is wasted work.
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
  
  // If a previous attempt confirmed that ScreenScraper has the system but not
  // the game (search exhausted), there is no point in retrying every 10 seconds.
  // Stop the recurrent.
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
  Lcd.fillScreen(THEME_BLACK);

  drawHeader(getPageTitle(), getPageSubtitle());

  switch (currentPage) {
    case 0: displayMainHUD();         break;
    case 1: displaySystemMonitor();   break;
    case 2: displayStorageArray();    break;
    case 3: displayNetworkTerminal(); break;
    case 4: displayDeviceScanner();   break;
  }

  drawFooter();
}

void displayMainHUD() {
  Lcd.fillRect(0, 44, 480, 226, THEME_BLACK);

  uint16_t panelColor = connected ? THEME_GREEN : THEME_YELLOW;
  drawPanel(10, 48, 460, 72, panelColor);

  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 58);
  Lcd.print("ACTIVE CORE");

  // Core name with scroll. A 24-char window at size 3 fits between x=20 and
  // x=470 (24 × 18px = 432px).
  String coreNormalized = currentCore;
  if (coreNormalized.equalsIgnoreCase("arcade")) coreNormalized = "Arcade";
  if (mainHUDCoreScroll.fullText != coreNormalized) {
    initScrollText(&mainHUDCoreScroll, coreNormalized, 24);
  }
  String coreDisplay = getScrolledText(&mainHUDCoreScroll);
  while ((int)coreDisplay.length() < 24) coreDisplay += ' ';

  Lcd.setTextColor(THEME_BLACK, panelColor);
  Lcd.setTextSize(3);
  Lcd.setCursor(20, 80);          // matches the loop() refresh (point 5b)
  Lcd.print(coreDisplay);

  // Uptime clock (below the panel)
  drawDigitalClock(10, 138, uptimeFormatted, "UPTIME");

  // Connection / image-cache hint — slightly enlarged (1.5x)
  Lcd.setTextColor(connected ? THEME_CYAN : THEME_YELLOW);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(10, 165);
  if (!connected) {
    Lcd.print("Reconnecting to MiSTer...");
  } else if (sdCardAvailable) {
    Lcd.print("Core changes show images automatically");
  }

  // Metric mini panels (CPU / RAM / USB) spread across the width
  drawMiniPanel( 10, 192, 145, 42, "CPU",
                 String(cpuUsage, 1) + "%", cpuUsage    > 80 ? THEME_RED : THEME_GREEN);
  drawMiniPanel(167, 192, 145, 42, "RAM",
                 String(memoryUsage, 1) + "%", memoryUsage > 80 ? THEME_RED : THEME_GREEN);
  drawMiniPanel(325, 192, 145, 42, "USB",
                 String(usbDeviceCount), THEME_CYAN);
}

void displaySystemMonitor() {
  Lcd.fillRect(0, 44, 480, 226, THEME_BLACK);

  // ===== CPU =====
  drawPanel(10, 48, 460, 50, THEME_GREEN);
  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 56);
  Lcd.print("CPU LOAD");
  Lcd.setTextSize(2);
  Lcd.setCursor(20, 72);
  Lcd.printf("%.1f%%", cpuUsage);
  drawProgressBar(180, 64, 270, 20, cpuUsage);   // bar 270px wide (was 160)

  // ===== MEMORY =====
  drawPanel(10, 108, 460, 50, THEME_CYAN);
  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 116);
  Lcd.print("MEMORY");
  Lcd.setTextSize(2);
  Lcd.setCursor(20, 132);
  Lcd.printf("%.1f%%", memoryUsage);
  drawProgressBar(180, 124, 270, 20, memoryUsage);

  // ===== SYSTEM STATUS — two columns between the MEMORY panel and the footer =====
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(10, 168);
  Lcd.print("SYSTEM STATUS:");

  // Left column (x=10)
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setCursor(10, 190);
  Lcd.printf("RUNTIME: %s", uptimeFormatted.c_str());

  Lcd.setCursor(10, 212);
  Lcd.printf("CONNECTION: %s", connected ? "ACTIVE" : "LOST");

  Lcd.setTextColor(THEME_GRAY);
  Lcd.setCursor(10, 234);
  Lcd.printf("REQUESTS: %d this session", requestsCount);

  // Right column (x=250)
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setCursor(250, 190);
  Lcd.printf("SCREENSCRAPER: %s", sdCardAvailable ? "READY" : "NO SD");

  Lcd.setTextColor(THEME_GRAY);
  Lcd.setCursor(250, 212);
  Lcd.printf("SD: %.1f / %.1f GB", sdUsedGB, sdTotalGB);
}

void displayStorageArray() {
  Lcd.fillRect(0, 44, 480, 226, THEME_BLACK);

  drawPanel(10, 48, 460, 92, THEME_ORANGE);

  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 58);
  Lcd.print("STORAGE ARRAY - SD CARD");

  Lcd.setTextSize(2);
  Lcd.setCursor(20, 82);
  Lcd.printf("%.1f GB", sdUsedGB);

  Lcd.setTextSize(1);
  Lcd.setCursor(20, 104);
  Lcd.printf("of %.1f GB total", sdTotalGB);

  Lcd.setCursor(20, 120);
  Lcd.printf("Free: %.1f GB", sdTotalGB - sdUsedGB);

  drawStorageBar(10, 156, 460, 28, sdUsagePercent);   // full-width bar

  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(10, 200);
  Lcd.printf("USAGE: %.0f%% | LOCAL SD: %s",
             sdUsagePercent, sdCardAvailable ? "OK" : "ERROR");

  Lcd.setTextColor(THEME_GRAY);
  Lcd.setCursor(10, 224);
  Lcd.print("PATH: /media/fat");
}

void displayNetworkTerminal() {
  Lcd.fillRect(0, 44, 480, 226, THEME_BLACK);

  drawPanel(10, 48, 460, 72, connected ? THEME_GREEN : THEME_RED);

  Lcd.setTextColor(THEME_BLACK);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 58);
  Lcd.print("DISPLAY <-> MISTER");

  Lcd.setTextSize(2);
  Lcd.setCursor(20, 76);
  Lcd.print(connected ? "CONNECTED" : "DISCONNECTED");

  Lcd.setTextSize(1);
  Lcd.setCursor(20, 100);
  Lcd.printf("Target: %s:8081", misterIP);

  // ===== NETWORK INFO =====
  Lcd.setTextSize(1.5f);
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setCursor(10, 130);
  Lcd.print("NETWORK INFO:");

  Lcd.setTextColor(THEME_WHITE);
  Lcd.setCursor(10, 150);
  Lcd.printf("MiSTer IP: %s",
             connected && networkIP != "N/A" ? networkIP.c_str() : misterIP);

  Lcd.setCursor(10, 170);
  Lcd.printf("Monitor IP: %s", WiFi.localIP().toString().c_str());

  if (connected && networkConnected) {
    Lcd.setTextColor(THEME_GREEN);
    Lcd.setCursor(10, 190);
    Lcd.print("MiSTer Network: ONLINE");
  } else if (connected) {
    Lcd.setTextColor(THEME_CYAN);
    Lcd.setCursor(10, 190);
    Lcd.print("MiSTer Network: Unknown");
  }

  // Faint divider between the two sections
  Lcd.drawFastHLine(10, 212, 460, THEME_GRAY);

  // ===== SESSION STATS =====
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setCursor(10, 220);
  Lcd.print("SESSION STATS:");

  if (connected) {
    Lcd.setTextColor(THEME_CYAN);
    Lcd.setCursor(10, 240);
    Lcd.printf("Session: %s | Requests: %d",
               sessionDuration.c_str(), requestsCount);
  } else {
    Lcd.setTextColor(THEME_RED);
    Lcd.setCursor(10, 240);
    Lcd.print("Check server & network settings");
  }
}

void displayDeviceScanner() {
  Lcd.fillRect(0, 44, 480, 226, THEME_BLACK);

  drawPanel(10, 48, 460, 50, THEME_BLUE);

  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(20, 58);
  Lcd.print("DEVICE SCANNER");

  Lcd.setTextSize(2);
  Lcd.setCursor(20, 74);
  Lcd.printf("USB: %d | SERIAL: %d", usbDeviceCount, serialPortCount);

  drawPortArray(10, 116, usbDeviceCount, serialPortCount);
}

void drawHeader(String title, String subtitle) {
  // Header band Y=0..43 (44px, a touch taller than before so the logo and the
  // two text lines can grow a little). Logo left, title/subtitle next to it,
  // page dots and status indicator at the right edge of the 480px panel.
  Lcd.fillRect(0, 0, 480, 44, THEME_BLACK);

  drawMiSTerLogo(10, 6);

  // Title (cyan) over subtitle (green), slightly enlarged (1.5x)
  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(132, 7);
  Lcd.print(title);

  Lcd.setTextColor(THEME_GREEN);
  Lcd.setCursor(132, 26);
  Lcd.print(subtitle);

  // Page dots near the right edge
  for (int i = 0; i < totalPages; i++) {
    uint16_t color = (i == currentPage) ? THEME_YELLOW : THEME_GRAY;
    Lcd.fillRect(390 + i * 9, 11, 7, 7, color);
    if (i == currentPage) {
      Lcd.drawRect(389 + i * 9, 10, 9, 9, THEME_YELLOW);
    }
  }

  // Status indicator, far right
  drawStatusIndicator(462, 20, connected ? THEME_GREEN : THEME_RED, connected);

  // Bottom divider (double line)
  Lcd.drawFastHLine(0, 42, 480, THEME_YELLOW);
  Lcd.drawFastHLine(0, 43, 480, THEME_YELLOW);
}

void drawMiSTerLogo(int x, int y) {
  // Slightly enlarged wordmark to suit the taller (44px) header.
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(2.4f);
  Lcd.setCursor(x, y);
  Lcd.print("MiSTer");

  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(1.3f);
  Lcd.setCursor(x, y + 22);
  Lcd.print("FPGA");

  for (int i = 0; i < 8; i++) {
    Lcd.fillCircle(x + 52 + i * 4, y + 27, 1, THEME_CYAN);
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
  Lcd.setTextSize(1.5f);
  Lcd.setCursor(x + 6, y + 5);
  Lcd.print(label);

  Lcd.setTextSize(1.5f);
  Lcd.setCursor(x + 6, y + 23);
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
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(x, y);
  Lcd.printf("USB DEVICES CONNECTED: %d", usbCount);
  Lcd.setCursor(x, y + 14);
  Lcd.printf("SERIAL PORTS ACTIVE: %d", serialCount);

  // USB grid: 6 columns x 2 rows = 12 slots (was 4x2 = 8 on the 320px CYD).
  // 12 is the sweet spot for 480px — 18 felt like too much empty grid.
  int maxDisplay = min(usbCount, 12);
  for (int i = 0; i < 12; i++) {
    uint16_t color = (i < maxDisplay) ? THEME_GREEN : THEME_GRAY;
    int posX = x + (i % 6) * 75;
    int posY = y + 40 + (i / 6) * 32;
    Lcd.drawRect(posX, posY, 60, 24, color);
    if (i < maxDisplay) Lcd.fillRect(posX + 2, posY + 2, 56, 20, color);
    Lcd.setTextColor(i < maxDisplay ? THEME_BLACK : THEME_WHITE);
    Lcd.setCursor(posX + 22, posY + 8);
    Lcd.printf("U%d", i + 1);
  }

  // Serial ports: row below the USB grid (S1/S2/S3 — no port names here,
  // so there's nothing to mis-place; the ttyUSB0 issue was mockup-only)
  for (int i = 0; i < 3; i++) {
    uint16_t color = (i < serialCount) ? THEME_CYAN : THEME_GRAY;
    int posX = x + i * 75;
    int posY = y + 110;
    Lcd.drawRect(posX, posY, 60, 24, color);
    if (i < serialCount) Lcd.fillRect(posX + 2, posY + 2, 56, 20, color);
    Lcd.setTextColor(i < serialCount ? THEME_BLACK : THEME_WHITE);
    Lcd.setCursor(posX + 22, posY + 8);
    Lcd.printf("S%d", i + 1);
  }

  if (usbCount > 12) {
    Lcd.setTextColor(THEME_YELLOW);
    Lcd.setCursor(x + 250, y + 118);
    Lcd.printf("+%d more", usbCount - 12);
  }
}

String getPageTitle() {
  switch(currentPage) {
    case 0: return "MAIN HUD";
    case 1: return "SYS MONITOR";
    case 2: return "STORAGE";
    case 3: return "NETWORK";
    case 4: return "DEVICES";
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
  Lcd.setTextSize(3);
  Lcd.setCursor(177, 70);
  Lcd.print("SD CARD");
  Lcd.setCursor(195, 105);
  Lcd.print("ERROR");

  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(60, 150);
  Lcd.print("Check SD card:");
  Lcd.setCursor(60, 170);
  Lcd.print("1. Card inserted properly");
  Lcd.setCursor(60, 187);
  Lcd.print("2. Formatted as FAT32");
  Lcd.setCursor(60, 204);
  Lcd.print("3. Create /cores folder");
  Lcd.setCursor(60, 221);
  Lcd.print("4. Add JPG images to /cores");

  Lcd.setTextColor(THEME_CYAN);
  Lcd.setCursor(60, 255);
  Lcd.print("Continuing without core images...");

  delay(4000);
}

void showImageNotFound(String coreName) {
  Lcd.fillScreen(THEME_BLACK);

  // Header
  drawMiSTerLogo(10, 6);
  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(1);
  Lcd.setCursor(120, 16);
  Lcd.print("CORE IMAGE SYSTEM");
  drawStatusIndicator(462, 18, connected ? THEME_GREEN : THEME_RED, connected);

  // Frame (centered, 400px wide)
  Lcd.drawRect(40, 55, 400, 195, THEME_ORANGE);
  Lcd.drawRect(41, 56, 398, 193, THEME_ORANGE);

  // "?" icon
  Lcd.setTextColor(THEME_ORANGE);
  Lcd.setTextSize(4);
  Lcd.setCursor(228, 90);
  Lcd.print("?");

  Lcd.setTextColor(THEME_WHITE);
  Lcd.setTextSize(2);
  Lcd.setCursor(150, 145);
  Lcd.print("IMAGE NOT FOUND");

  Lcd.setTextColor(THEME_CYAN);
  Lcd.setTextSize(1);
  Lcd.setCursor(200, 180);
  Lcd.print("CORE:");
  Lcd.setTextColor(THEME_YELLOW);
  Lcd.setTextSize(2);
  Lcd.setCursor(195, 195);
  String displayCore = coreName.length() > 14 ? coreName.substring(0, 14) : coreName;
  if (displayCore.equalsIgnoreCase("arcade")) displayCore = "Arcade";
  Lcd.print(displayCore);

  Lcd.setTextColor(THEME_GRAY);
  Lcd.setTextSize(1);
  Lcd.setCursor(55, 232);
  Lcd.printf("Expected: %s/A/*.jpg or /#/*.jpg", CORE_IMAGES_PATH);

  Lcd.setTextColor(THEME_GREEN);
  Lcd.setCursor(140, 288);
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
  if (core == "Nintendo Game Boy Advance" || core == "Game Boy Advance") return "12";
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
  
  // Neo-Geo
  if (core == "Neo-Geo") return "142";
  if (core == "Neo-Geo CD") return "70";
  
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
  if (core == "Atari Jaguar") return "27";
  if (core == "Atari ST/STE" || core == "Atari ST") return "42";
  if (core == "Atari 8bit") return "43";
  
  // Commodore / Amiga
  if (core == "Commodore Amiga") return "64";
  if (core == "Amiga CD32") return "130";
  if (core == "Commodore 64") return "66";
  if (core == "Vic-20" || core == "Commodore VIC-20") return "73";
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
  if (core == "CD-i" || core == "Phillips CD-i") return "133";
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
  if (coreLower == "c64" || coreLower == "commodore64") return "66";
  if (coreLower == "ao486" || coreLower == "pc dos" || coreLower == "pcxt") return "135";
  if (coreLower == "amstrad" || coreLower == "cpc") return "65";
  if (coreLower == "sam" || coreLower == "samcoupe") return "213";
  if (coreLower == "x68000") return "79";
  if (coreLower == "wonderswan") return "45";
  if (coreLower == "wonderswancolor") return "46";
  if (coreLower == "vectrex") return "102";
  if (coreLower == "coleco") return "48";
  if (coreLower == "intellivision") return "115";
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
  
  // Download to buffer 
  uint8_t* buffer = (uint8_t*)malloc(contentLength);
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
  
  // Read image to buffer
  uint8_t *buffer = (uint8_t*)malloc(fileSize);
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
  
  // EXTRACT GAME ID - Search specifically in "jeu" section
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
  Serial.printf("   URL: %s\n", redactScreenScraperUrl(currentUrl).c_str());
  
  HTTPClient http;
  http.begin(currentUrl);
  http.setTimeout(25000);
  http.addHeader("User-Agent", "MiSTer-Monitor");
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
    if (lastSearchedGame != gameName || FORCE_GAME_REDOWNLOAD) {
      Serial.println("Attempting STREAMING-SAFE ScreenScraper download...");
      
      // Final memory check before download
      if (ESP.getFreeHeap() < 120000) {
        Serial.printf("Insufficient memory for download (%d bytes)\n", ESP.getFreeHeap());
        Serial.println("Falling back to core image");
        lastGameImageOK = false;
        showCoreImageScreen(coreName);
        return;
      }
      
      showDownloadingScreen(coreName, gameName);
      
      // *** USE FUNCTION STREAMING-SAFE ***
      if (downloadGameBoxartStreamingSafeJSON(coreName, gameName)) {
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
        // Do NOT update cache if download failed
        Serial.println("STREAMING-SAFE download failed - NOT updating cache to allow retry");
      if (!FORCE_GAME_REDOWNLOAD) {
        lastSearchedGame = gameName; // Only update cache if not forcing downloads
      }
      Serial.println("STREAMING-SAFE download failed");
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
    Serial.printf("Trying media type %d: %s\n", i + 1, mediaNames[i]);
    
    // Build complete URL with media type and resize parameters
    String currentUrl = baseUrl + "&media=" + mediaTypes[i];
    currentUrl += "&maxwidth=" + String(TARGET_WIDTH);
    currentUrl += "&maxheight=" + String(IMAGE_AREA_HEIGHT);  // Use 645 instead of 720
    currentUrl += "&outputformat=jpg";
    
    Serial.printf("   URL: %s\n", redactScreenScraperUrl(currentUrl).c_str());
    
    HTTPClient http;
    http.begin(currentUrl);
    http.setTimeout(25000);
    http.addHeader("User-Agent", "MiSTer-Monitor");
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
                      Serial.printf("SUCCESS: Downloaded %s (%d bytes)\n", mediaNames[i], savedSize);
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
              Serial.printf("No %s media available in database\n", mediaNames[i]);
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
      Serial.printf("HTTP %d for %s\n", httpCode, mediaNames[i]);
      
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
  mediaUrl += "&softname=MiSTer-Monitor";
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
  url += "&softname=MiSTer-Monitor";
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
  http.addHeader("User-Agent", "MiSTer-Monitor");
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

bool downloadGameBoxartStreamingSafeJSON(String coreName, String gameName) {
  if (downloadInProgress) {
    Serial.println("Download already in progress");
    return false;
  }
  
  downloadInProgress = true;
  g_lastSSHttpCode = 0;
  bool success = false;

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
    downloadInProgress = false;
    return false;
  }
  
  // Memory check
  if (ESP.getFreeHeap() < 100000) {
    Serial.printf("Insufficient memory: %d bytes\n", ESP.getFreeHeap());
    downloadInProgress = false;
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
        showDownloadProgress(100, "JSON download complete!");
        delay(1500);
        downloadInProgress = false;
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
          Board.update();
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
            showDownloadProgress(100, "Retry download complete!");
            delay(1500);
            downloadInProgress = false;
            Serial.println("=== RETRY DOWNLOAD COMPLETE ===\n");
            return true;
          } else {
            Serial.println("Retry MediaJeu download failed");
          }
        } else {
          Serial.println("Second CRC search also found no results");
          // ScreenScraper returned a clean response twice with no game match.
          // Mark search as exhausted.
          lastGameSearchExhausted = true;
          Serial.println("Marked search as exhausted");
        }
      }
    }
  } else {
    lastRomHasCrc = false;
    Serial.println("ROM CRC not available for JSON search");
  }
  
  if (!success) {
    Serial.println("JSON method failed");
    if (g_lastSSHttpCode != 0 && g_lastSSHttpCode != 200) {
      showDownloadProgress(0, ssHudMessage(g_lastSSHttpCode));
    } else {
      showDownloadProgress(0, "Download failed");
    }
    delay(6000);
  }
  
  downloadInProgress = false;
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