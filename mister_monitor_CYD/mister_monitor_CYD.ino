#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <JPEGDEC.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include "mister_types.h"
#include "AppConfig.h"

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

// -------------------------------------------------------
// psramMalloc(): safe allocator for large buffers.
//
// On ESP32-P4 (M5Tab), heap_caps_malloc(MALLOC_CAP_SPIRAM)
// returns addresses in the 0x500xxxxx region which causes
// Store Access Faults (MCAUSE=7). The PSRAM cache is not
// mapped for normal CPU load/store without specific sdkconfig
// settings that the M5Tab Arduino board package does not set.
//
// The M5Tab internal heap has ~510 KB free at boot, which
// is more than enough for all JPEG buffers (50-300 KB each).
// We therefore use plain malloc() and leave the PSRAM
// available implicitly for Arduino/WiFi/IDF internals.
// -------------------------------------------------------
inline uint8_t* psramMalloc(size_t size) {
  uint8_t* ptr = (uint8_t*)malloc(size);
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
const char* misterIP = "192.168.1.100";

// ScreenScraper runtime strings.
String _ss_dev_user_str;
String _ss_dev_pass_str;
String _ss_user_str;
String _ss_pass_str;
String _boxart_region_str       = "wor";
String _core_images_path_str    = "/cores";
String _default_core_image_str  = "/cores/menu.jpg";

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
void testMiSTerConnectivity();
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
void drawPageIndicators();
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

void addGameImageFooter(String gameName);
void drawCoreImageFooter();
enum RescanButtonState { RESCAN_IDLE, RESCAN_SCANNING };
bool isRescanVisible();
void drawRescanButton(RescanButtonState state);
void redrawCurrentFooter();

// GameInfo searchWithJeuInfosPrecise(String coreName, RomDetails romDetails);
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

String currentGameForCrc = "";         // Juego actual que necesita CRC
String currentCoreForCrc = "";         // Core del juego actual  
unsigned long lastCrcRecurrentTime = 0; // Ultimo intento CRC recurrente
bool crcRecurrentActive = false;       // Si busqueda recurrente esta activa
int crcRecurrentAttempts = 0;          // Contador de intentos recurrentes
bool lastRomHasCrc           = false;  // true when current game's ROM has a valid CRC
bool lastGameImageOK         = false;  // true when a game-specific image is displayed
                                        // (either cached or freshly downloaded)
bool lastGameSearchExhausted = false;  // true when ScreenScraper search with valid
                                        // CRC completed without finding an image
                                        // (system in DB, but game not). RESCAN won't help.
unsigned long rescanForceVisibleUntil = 0;  // millis() until which the RESCAN button
                                             // is force-visible on user request
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

// Function declaration (add this with other function declarations)
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

// Global variables to add (add these at the top of your .ino file)
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
  static constexpr int OFFSET_Y = 200;  // Offset normal para pantallas del monitor
  
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
  
  void setTextSize(uint8_t size) {
    // Text size also needs to be scaled to look proportional
    M5.Display.setTextSize((uint8_t)(size * SCALE_FACTOR));
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
  
  // ========== LEFT PANEL CONTENT (COORDENADAS FÍSICAS con OFFSET_Y=130) ==========
  // Constantes para este diseño específico
  const int PANEL_OFFSET_X = 90;
  const int PANEL_OFFSET_Y = 130;  // Offset específico para pantalla de descarga
  const int SCALE = 2;  // Escala 2x
  
  // Macro para convertir coordenadas lógicas a físicas
  #define PHYS_X(x) (PANEL_OFFSET_X + ((x) * SCALE))
  #define PHYS_Y(y) (PANEL_OFFSET_Y + ((y) * SCALE))
  
  // Clear the left panel content area (físicas: 90,130 tamaño 640x480)
  M5.Display.fillRect(PANEL_OFFSET_X, PANEL_OFFSET_Y, 640, 480, THEME_BLACK);
  
  // Header with ScreenScraper logo area
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(4);  // Tamaño 2 lógico = 4 físico
  M5.Display.setCursor(PHYS_X(40), PHYS_Y(5));
  M5.Display.print("SCREENSCRAPER");
  
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(2);  // Tamaño 1 lógico = 2 físico
  M5.Display.setCursor(PHYS_X(65), PHYS_Y(25));
  M5.Display.print("AUTO-DOWNLOAD");
  
  // Decorative line
  M5.Display.drawFastHLine(PHYS_X(20), PHYS_Y(40), 560, THEME_CYAN);  // 280*2=560
  
  // Main download panel (usando drawPanel requeriría modificarlo, hacemos rect directo)
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

void showDownloadProgress(int progress, String text) {
  // ========== UPDATE PROGRESS BAR (COORDENADAS FÍSICAS con OFFSET_Y=130) ==========
  const int PANEL_OFFSET_X = 90;
  const int PANEL_OFFSET_Y = 130;
  const int SCALE = 2;
  
  #define PHYS_X(x) (PANEL_OFFSET_X + ((x) * SCALE))
  #define PHYS_Y(y) (PANEL_OFFSET_Y + ((y) * SCALE))
  
  // Clear progress area (debajo del STATUS que está en Y=113 lógico)
  M5.Display.fillRect(PHYS_X(10), PHYS_Y(130), 600, 180, THEME_BLACK);
  
  // Progress bar
  M5.Display.drawRect(PHYS_X(20), PHYS_Y(140), 560, 40, THEME_WHITE);
  M5.Display.fillRect(PHYS_X(20)+2, PHYS_Y(140)+2, 556, 36, THEME_BLACK);
  
  // Fill progress bar
  if (progress > 0) {
    int fillWidth = (progress * 552) / 100;  // 276*2 = 552
    uint16_t barColor = (progress < 30) ? THEME_YELLOW : 
                       (progress < 70) ? THEME_CYAN : THEME_GREEN;
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

// ========== RESCAN GAME BUTTON HELPERS ==========
// The RESCAN GAME button appears in the footer of fullscreen image screens
// (both core and game) when the ROM hasn't been positively identified.
//
// Visibility logic: a game is loaded AND either (a) the server hasn't given
// us a CRC for it yet, OR (b) we aren't displaying a game-specific image.
// Once both conditions are clear (CRC known and game image showing), the
// button hides.
bool isRescanVisible() {
  // Prerequisite: a game must be loaded
  if (currentGame.length() == 0) return false;
  
  // User-triggered pulse (touch outside button on image screen): show for 5s
  if (millis() < rescanForceVisibleUntil) return true;
  
  // Hide if ScreenScraper search was completed cleanly with no game match.
  // Re-rescanning won't help, the title simply isn't in the database for this system.
  if (lastGameSearchExhausted) return false;
  
  // Show if CRC is missing — rescan can recover it
  if (!lastRomHasCrc) return true;
  
  // Show if CRC is present but image hasn't been displayed (network failure,
  // timeout, etc.). Manual rescan can retry the download.
  if (!lastGameImageOK) return true;
  
  // Everything looks fine: hide the button to keep the footer clean.
  return false;
}

// Draws the RESCAN GAME button at fixed coordinates inside the footer.
// 'state' = RESCAN_IDLE during normal display, RESCAN_SCANNING during the
// blocking HTTP call to /status/rom/details?force=1 so the user sees that
// the press registered.
void drawRescanButton(RescanButtonState state) {
  uint16_t borderColor = (state == RESCAN_SCANNING) ? THEME_YELLOW : THEME_CYAN;
  uint16_t textColor   = borderColor;
  const char* label    = (state == RESCAN_SCANNING) ? "[  SCANNING... ]" : "[ RESCAN GAME ]";

  M5.Display.fillRect(840, 632, 420, 55, THEME_BLACK);
  M5.Display.drawRect(840, 632, 420, 55, borderColor);
  M5.Display.setTextColor(textColor);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(918, 645);
  M5.Display.print(label);
}

// Repaints the active footer (core image vs game image) without redrawing
// the whole image. Used after a failed rescan so we don't lose the picture.
void redrawCurrentFooter() {
  if (lastGameImageOK && currentGame.length() > 0) {
    addGameImageFooter(currentGame);
  } else {
    drawCoreImageFooter();
  }
}

// Visible window (in characters) for the game name in the image footer.
// At text size 3 each char is ~18 px wide.
//   - Without rescan button: x=200 to x=1280  →  1080 / 18 ≈ 60 chars; use 58 for margin.
//   - With    rescan button: x=200 to x=820   →   620 / 18 ≈ 34 chars; use 30 for margin.
#define GAME_FOOTER_VISIBLE_CHARS_FULL    58
#define GAME_FOOTER_VISIBLE_CHARS_RESCAN  30

void addGameImageFooter(String gameName) {
  // Footer in PHYSICAL coordinates (full screen width, below image area at Y=620)
  
  // Draw separator line at top of footer
  M5.Display.drawFastHLine(0, 620, 1280, THEME_GREEN);
  M5.Display.fillRect(0, 621, 1280, 99, THEME_BLACK);
  
  // Decide whether the rescan button takes the right side of the footer
  bool showRescan   = isRescanVisible();
  int  visibleChars = showRescan ? GAME_FOOTER_VISIBLE_CHARS_RESCAN
                                  : GAME_FOOTER_VISIBLE_CHARS_FULL;
  
  M5.Display.setTextWrap(false);
  
  // Game name label (upper line of footer)
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(40, 638);
  M5.Display.print("GAME:");
  
  // Initialize the scroll state for this game name. Re-init if either the
  // text OR the visible window changes (the latter happens when the rescan
  // button toggles on/off mid-display).
  if (imageFooterScroll.fullText != gameName ||
      imageFooterScroll.maxChars != visibleChars) {
    initScrollText(&imageFooterScroll, gameName, visibleChars);
  }
  
  // Print whatever the scroll state currently exposes (full name on first
  // draw, scrolled window thereafter). The refresh block in loop() will
  // animate it.
  String displayGame = getScrolledText(&imageFooterScroll);
  // Pad to maxChars for stable width across scroll frames
  while ((int)displayGame.length() < imageFooterScroll.maxChars) {
    displayGame += ' ';
  }
  M5.Display.setTextColor(THEME_YELLOW, THEME_BLACK);  // bg color → flicker-free
  M5.Display.setTextSize(3);
  M5.Display.setCursor(200, 638);
  M5.Display.print(displayGame);
  
  // Instructions (lower line of footer) — shifted left when button is visible
  M5.Display.setTextColor(THEME_GREEN);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(showRescan ? 40 : 250, 685);
  M5.Display.print("Touch the screen to show MiSTer monitor");
  
  // Draw rescan button on top if conditions warrant it
  if (showRescan) {
    drawRescanButton(RESCAN_IDLE);
  }
}

// Footer for fullscreen core image screens.
// Layout (footer band: y=621..720, height 99):
//   - If game is active: line 1 (y=638) "GAME: <name>" with scroll, line 2 (y=685) "Touch..."
//   - If no game:        single centered "Touch..." (y=660), as before
// When a game is active but its CRC could not be detected, also shows RESCAN GAME button.
void drawCoreImageFooter() {
  M5.Display.drawFastHLine(0, 620, 1280, THEME_GREEN);
  M5.Display.fillRect(0, 621, 1280, 99, THEME_BLACK);
  
  bool showRescan = isRescanVisible();
  bool hasGame    = (currentGame.length() > 0);
  
  M5.Display.setTextWrap(false);
  
  if (hasGame) {
    // === Line 1: GAME: <name> ===
    int visibleChars = showRescan ? GAME_FOOTER_VISIBLE_CHARS_RESCAN
                                   : GAME_FOOTER_VISIBLE_CHARS_FULL;
    
    // Initialize the scroll state. Re-init if either the text OR the visible
    // window changed (the rescan button toggles on/off may change visibleChars).
    if (imageFooterScroll.fullText != currentGame ||
        imageFooterScroll.maxChars != visibleChars) {
      initScrollText(&imageFooterScroll, currentGame, visibleChars);
    }
    String displayGame = getScrolledText(&imageFooterScroll);
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
    M5.Display.setCursor(200, 638);
    M5.Display.print(displayGame);
    
    // === Line 2: hint ===
    M5.Display.setTextColor(THEME_GREEN);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(showRescan ? 40 : 250, 685);
    M5.Display.print("Touch the screen to show MiSTer monitor");
    
  } else {
    // No game: just the centered hint, like before
    M5.Display.setTextColor(THEME_GREEN);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(showRescan ? 40 : 250, 660);
    M5.Display.print("Touch the screen to show MiSTer monitor");
  }
  
  // Rescan button on top
  if (showRescan) {
    drawRescanButton(RESCAN_IDLE);
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
  //   - Each char at size 2 is 12 px wide physically (6 lógical × 2 font scale).
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
  http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");
  
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
        M5.update();                    // Keep touch state fresh
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
    httpRetry.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");
    
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
  http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");

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
      // Convertir a hex
      encoded += '%';
      if (c < 16) encoded += '0';
      encoded += String(c, HEX);
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
  baseUrl += "&ssid=" + String(SCREENSCRAPER_USER);
  baseUrl += "&sspassword=" + String(SCREENSCRAPER_PASS);
  baseUrl += "&systemeid=" + systemId;
  
  // Empty hash parameters (required by API)
  baseUrl += "&crc=";
  baseUrl += "&md5=";
  baseUrl += "&sha1=";
  
  Serial.printf("Base URL: %s\n", baseUrl.c_str());
  
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
      // Opcionalmente, renombrar archivo existente como backup
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

  // ========== LEFT PANEL CONTENT (COORDENADAS FÍSICAS con OFFSET_Y=130) ==========
  const int PANEL_OFFSET_X = 90;
  const int PANEL_OFFSET_Y = 130;
  const int SCALE = 2;
  
  #define PHYS_X(x) (PANEL_OFFSET_X + ((x) * SCALE))
  #define PHYS_Y(y) (PANEL_OFFSET_Y + ((y) * SCALE))
  
  // Clear the left panel content area
  M5.Display.fillRect(PANEL_OFFSET_X, PANEL_OFFSET_Y, 640, 480, THEME_BLACK);
  
  // Header with ScreenScraper logo area
  M5.Display.setTextColor(THEME_CYAN);
  M5.Display.setTextSize(4);  // Tamaño 2 lógico = 4 físico
  M5.Display.setCursor(PHYS_X(40), PHYS_Y(5));
  M5.Display.print("SCREENSCRAPER");
  
  M5.Display.setTextColor(THEME_ORANGE);
  M5.Display.setTextSize(2);  // Tamaño 1 lógico = 2 físico
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
        M5.Display.fillRect(btnScan.x + 10, btnScan.y + 24,
                            btnScan.w - 20, 32, THEME_BLACK);
        btnScan.draw(THEME_YELLOW);   // SCANNING in yellow to stand out
        
        // === Run the actual operation ===
        // Step 1: refresh all MiSTer state (core, game, system, network, etc.)
        updateMiSTerData();
        
        // Step 2: if a game is active, force a fresh ROM details fetch.
        if (currentGame.length() > 0) {
          Serial.println("Game active — forcing ROM details rescan");
          RomDetails fresh = getCurrentRomDetailsForced();
          bool hadCrc = lastRomHasCrc;
          lastRomHasCrc = fresh.available && fresh.hashCalculated && fresh.crc32.length() > 0;
          Serial.printf("After SCAN rescan: CRC available = %s\n",
                        lastRomHasCrc ? "YES" : "NO");
          
          // If CRC newly arrived, clear caches so the next image lookup uses it
          if (lastRomHasCrc && !hadCrc) {
            lastSearchedGame        = "";
            lastGameImageOK         = false;
            lastGameSearchExhausted = false;
          }
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

  // ========== INICIAR SERIAL PRIMERO ==========
  Serial.begin(115200);
  delay(500);  // Dar tiempo al Serial

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
  M5.Speaker.tone(988, 100);  // B (octava alta)
  delay(110);
  M5.Speaker.tone(740, 100);  // F#
  delay(110);
  M5.Speaker.tone(622, 100);  // D#
  delay(110);
  M5.Speaker.tone(494, 100);  // B
  delay(110);
  M5.Speaker.tone(740, 100);  // F#
  delay(110);
  M5.Speaker.tone(622, 150);  // D# (más largo)
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
  M5.Speaker.setVolume(100);  // Volumen alto (0-255)*/
  
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
  misterIP = appConfig.misterIP.c_str();

  _ss_dev_user_str        = appConfig.ssDevUser;
  _ss_dev_pass_str        = appConfig.ssDevPass;
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
  // Start screenshot HTTP server (only if WiFi connected)
  if (WiFi.status() == WL_CONNECTED) {
    setupScreenshotServer();
  }
}

void loop() {
  M5.update();
  screenshotServer.handleClient();  // Non-blocking screenshot server poll
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

    // === Touch on RESCAN button ===
    if (isRescanVisible() && tx >= 840 && tx < 1260 && ty >= 632 && ty < 687) {
      Serial.println("RESCAN GAME button pressed - fetching fresh ROM details");
      
      // Visual feedback: flip button to SCANNING state before the blocking
      // HTTP call (which can take up to 15s on timeout).
      drawRescanButton(RESCAN_SCANNING);
      rescanForceVisibleUntil = 0;   // pulse no longer needed; consume it
      
      RomDetails fresh = getCurrentRomDetailsForced();
      lastRomHasCrc = fresh.available && fresh.hashCalculated && fresh.crc32.length() > 0;
      Serial.printf("After rescan: CRC available = %s\n", lastRomHasCrc ? "YES" : "NO");
      
      if (lastRomHasCrc) {
        Serial.printf("CRC obtained — clearing search cache and retrying download\n");
        lastSearchedGame        = "";
        lastGameImageOK         = false;
        lastGameSearchExhausted = false;  // give the new CRC a fresh chance
        showGameImageScreen(currentCore, currentGame);
        coreImageStartTime = millis();
      } else {
        redrawCurrentFooter();
      }
      lastButtonPress = millis();
      return;
    }
    
    // === Touch outside RESCAN button, but a game is active and something
    //     looks off (no CRC OR no image OR pulse already running) ===
    // Instead of exiting to monitor, surface the RESCAN button for 5 seconds
    // so the user can recover from a desynced display. A second touch outside
    // the button (after the pulse expires) exits to monitor as usual.
    bool somethingMightBeWrong = (currentGame.length() > 0 &&
                                  !lastGameSearchExhausted &&
                                  (!lastRomHasCrc || !lastGameImageOK));
    if (somethingMightBeWrong && millis() >= rescanForceVisibleUntil) {
      Serial.println("Touch outside button — surfacing RESCAN for 5s");
      rescanForceVisibleUntil = millis() + 5000;
      redrawCurrentFooter();
      lastButtonPress = millis();
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
    rescanForceVisibleUntil  = 0;   // pulse cancelled by exiting
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
            
            // Y/X coordinates depend on which screen we're on.
            int gameLineY = showingGameImage ? 638
                          : (currentCore == coreDownloadFailedFor ? 668 : 638);
            int textCursorX = showingGameImage ? 200
                          : (currentCore == coreDownloadFailedFor ? 120 : 200);
            
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
  if (!showingCoreImage && sdCardAvailable && (millis() - lastButtonPress > SCREENSAVER_TIMEOUT)
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
    getCurrentCore();
    getCurrentGame();
    
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
    
    // ENHANCED: Update subsystem with special handling for Arcade
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
      // × 2 scale × 1 (size 3 doubles per glyph at 6px lógical → 12 logical
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
      drawStatusIndicator(290, 15, connected ? THEME_GREEN : THEME_RED, connected && blinkState < 2);
    }
    animTimer = millis();
  }
  
  delay(needsRedraw || showingCoreImage ? 10 : 100);
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

/* ORIGINAL CODE - temporarily disabled for testing
bool displayCoreImage_ORIGINAL(String imagePath) {
  if (!sdCardAvailable || !SD.exists(imagePath)) {
    Serial.printf("Image doesn't exist: %s\n", imagePath.c_str());
    return false;
  }
  
  Serial.printf("Displaying image: %s\n", imagePath.c_str());
  
  File imageFile = SD.open(imagePath);
  if (!imageFile) {
    Serial.println("Error opening file");
    return false;
  }
  
  size_t fileSize = imageFile.size();
  Serial.printf("Size: %d bytes\n", fileSize);
  
  if (fileSize == 0 || fileSize > 500000) {
    Serial.println("Invalid file size");
    imageFile.close();
    return false;
  }
  
  // Read into smaller buffer if possible
  uint8_t *buffer = (uint8_t*)malloc(fileSize);
  if (!buffer) {
    Serial.println("No memory for image");
    imageFile.close();
    return false;
  }
  
  size_t bytesRead = imageFile.read(buffer, fileSize);
  imageFile.close();
  
  if (bytesRead != fileSize) {
    Serial.println("Error reading file");
    free(buffer);
    return false;
  }
  
  // Verify basic JPEG header
  if (buffer[0] != 0xFF || buffer[1] != 0xD8) {
    Serial.println("Not a valid JPEG");
    free(buffer);
    return false;
  }
  
  // Clear screen with black (same as displayCoreImageCentered which works)
  Lcd.fillScreen(THEME_BLACK);
  
  // Ensure JPEG decoder is in clean state
  jpeg.close();
  
  // Decode with jpegDrawCallback (same as displayCoreImageCentered which works)
  Serial.printf("About to call jpeg.openRAM() with buffer size %d\n", fileSize);
  if (jpeg.openRAM(buffer, fileSize, jpegDrawCallback)) {
    Serial.println("jpeg.openRAM() succeeded");
    int imgW = jpeg.getWidth();
    int imgH = jpeg.getHeight();
    
    // FIXED: Use physical coordinates for both X and Y (images are not scaled)
    int offsetX = (TARGET_WIDTH - imgW) / 2;      // Physical: 1280
    int offsetY = (IMAGE_AREA_HEIGHT - imgH) / 2; // Physical: 645
    
    // Debug logging to verify centering
    Serial.printf("=== IMAGE CENTERING DEBUG ===\n");
    Serial.printf("Image dimensions: %dx%d\n", imgW, imgH);
    Serial.printf("Available area: %dx%d\n", TARGET_WIDTH, IMAGE_AREA_HEIGHT);
    Serial.printf("Calculated offsets: X=%d, Y=%d\n", offsetX, offsetY);
    Serial.printf("Image position: X[%d-%d] Y[%d-%d]\n", 
                  offsetX, offsetX+imgW, offsetY, offsetY+imgH);
    
    if (offsetX < 0) {
      Serial.printf("WARNING: Image too wide, clipping\n");
      offsetX = 0;
    }
    if (offsetY < 0) {
      Serial.printf("WARNING: Image too tall, clipping\n");
      offsetY = 0;
    }
    
    if (offsetY + imgH > IMAGE_AREA_HEIGHT) {
      Serial.printf("ERROR: Image extends into footer (Y=%d, footer at Y=%d)\n", 
                    offsetY + imgH, IMAGE_AREA_HEIGHT);
    } else {
      Serial.printf("OK: Image centered correctly\n");
    }
    Serial.printf("=============================\n");
    
    Serial.println("Setting pixel type to RGB565_BIG_ENDIAN...");
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    
    Serial.printf("About to decode with offsets: X=%d, Y=%d\n", offsetX, offsetY);
    Serial.printf("Free heap before decode: %d bytes\n", ESP.getFreeHeap());
    
    bool success = jpeg.decode(offsetX, offsetY, 0);
    
    Serial.printf("jpeg.decode() returned: %s\n", success ? "SUCCESS" : "FAILED");
    Serial.printf("Free heap after decode: %d bytes\n", ESP.getFreeHeap());
    
    jpeg.close();
    free(buffer);
    
    if (success) {
      Serial.printf("Image displayed: %dx%d\n", imgW, imgH);
      return true;
    } else {
      Serial.println("Error decoding");
      return false;
    }
  } else {
    Serial.println("Error opening JPEG");
    free(buffer);
    return false;
  }
}
*/ // End of ORIGINAL CODE - temporarily disabled for testing

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
  
  // IMPROVED: Show menu image with core overlay instead of "image not found"
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
  bool showRescan = isRescanVisible();
  bool hasGame    = (currentGame.length() > 0);
  
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
    int visibleChars = showRescan ? GAME_FOOTER_VISIBLE_CHARS_RESCAN
                                   : GAME_FOOTER_VISIBLE_CHARS_FULL;
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
  
  // === Hint (bottom-right, size 2, only when no rescan button) ===
  if (!showRescan) {
    M5.Display.setTextColor(THEME_CYAN);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(870, 695);
    M5.Display.print("Touch to show monitor");
  }
  
  // Rescan button if applicable
  if (showRescan) {
    drawRescanButton(RESCAN_IDLE);
  }
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
 * This serves as the base cyberpunk frame for the entire interface
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
 * Draw cyberpunk frame - now just loads frame01.jpg
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
    
    Serial.println("=== TESTING MiSTer CONNECTIVITY ===");
    testMiSTerConnectivity();
    
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

void testMiSTerConnectivity() {
  HTTPClient http;
  String url = String("http://") + misterIP + ":8081/status/core";
  
  Serial.printf("Testing connectivity to: %s\n", url.c_str());
  
  http.begin(url);
  http.setTimeout(5000);
  
  int code = http.GET();
  
  // Clear left panel area for result
  int animOffsetX = 110;
  int animOffsetY = 140;
  int scale = 2;
  
  // Calculate safe width to avoid overlapping right panel circles
  // Left panel ends at X=640, circles start at X=670
  // Safe width = 640 - animOffsetX - margin = 640 - 110 - 10 = 520
  int safeWidth = 520;
  
  M5.Display.fillRect(animOffsetX, animOffsetY + 60*scale, 
                      safeWidth, 140*scale, THEME_BLACK);
  
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
    Serial.printf("MiSTer not responding (code: %d)\n", code);
    
    M5.Display.setTextColor(THEME_RED);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 95*scale);
    M5.Display.print("MiSTer: OFFLINE");
    
    M5.Display.setCursor(animOffsetX + 30*scale, animOffsetY + 115*scale);
    M5.Display.printf("Check: %s:8081", misterIP);
    
    connected = false;
  }
  
  http.end();
  delay(2000);
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
  lastGameImageOK          = false;
  rescanForceVisibleUntil  = 0;
  
  // Short-circuit: if the core is not in ScreenScraper's DB, do not activate
  // the recurrent. ScreenScraper requires a system ID, and asking MiSTer for
  // ROM details (which calculates CRC32 over the file) is wasted work.
  // Mark the search as exhausted from the start so the RESCAN button stays hidden.
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
  lastGameSearchExhausted  = false; // Re-enable RESCAN for the new game
  
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
  // will produce a hit. Mark search exhausted (hides RESCAN button) and stop.
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
  // Stop the recurrent — only a manual RESCAN GAME (which clears this flag)
  // can re-enable searches.
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
  
  drawStatusIndicator(290, 15, connected ? THEME_GREEN : THEME_RED, connected);
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
  
  Serial.printf("Downloading resized image: %s\n", resizedUrl.c_str());
  
  HTTPClient http;
  http.begin(resizedUrl);
  http.setTimeout(DOWNLOAD_TIMEOUT);
  http.addHeader("User-Agent", SCREENSCRAPER_SOFTWARE);
  
  int httpCode = http.GET();
  
  if (httpCode != 200) {
    Serial.printf("Download failed: %d\n", httpCode);
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
  Serial.printf("Downloading from mediaJeu.php: %s\n", mediaUrl.c_str());
  
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
  Serial.printf("   URL: %s\n", currentUrl.c_str());
  
  HTTPClient http;
  http.begin(currentUrl);
  http.setTimeout(25000);
  http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");
  http.addHeader("Accept", "image/jpeg,image/png,image/*");
  
  int httpCode = http.GET();
  Serial.printf("   HTTP Response: %d\n", httpCode);
  
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
  
  // ENHANCED LOGIC: Check existing image and force download setting
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
    
    Serial.printf("   URL: %s\n", currentUrl.c_str());
    
    HTTPClient http;
    http.begin(currentUrl);
    http.setTimeout(25000);
    http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");
    http.addHeader("Accept", "image/jpeg,image/png,image/*");
    
    int httpCode = http.GET();
    Serial.printf("HTTP Response: %d\n", httpCode);
    
    if (httpCode == 200) {
      String contentType = http.header("Content-Type");
      int contentLength = http.getSize();
      
      Serial.printf("Content-Type: '%s'\n", contentType.c_str());
      Serial.printf("Content-Length: %d\n", contentLength);
      
      // Read the COMPLETE response first (STREAMING SEGURO)
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
  Serial.printf("   %s\n", mediaUrl.c_str());
  
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
  
  // Use the correct function
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
  url += "&ssid=" + String(SCREENSCRAPER_USER);
  url += "&sspassword=" + String(SCREENSCRAPER_PASS);
  url += "&systemeid=" + systemId;
  url += "&romtype=rom";
  url += "&romnom=" + urlEncode(romDetails.filename);
  
  // Use correct RomDetails fields
  url += "&crc=" + romDetails.crc32;  // It's already a String, it doesn't need toUpperCase()
  url += "&romtaille=" + String(romDetails.filesize);  // filesize, no size
  url += "&md5=" + romDetails.md5;   // It's already a String, it doesn't need toUpperCase()
  url += "&sha1="; // Empty but required
  
  Serial.printf("JSON Search URL: %s\n", url.c_str());
  
  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);
  http.addHeader("User-Agent", "M5Stack-MiSTer-Monitor");
  http.addHeader("Accept", "application/json");
  
  int httpCode = http.GET();
  Serial.printf("HTTP Response: %d\n", httpCode);
  
  if (httpCode == 200) {
    int contentLength = http.getSize();
    Serial.printf("Content length: %d bytes\n", contentLength);
    
    int currentHeap = ESP.getFreeHeap();
    
    // OPTIMIZATION: Use streaming more conservatively
    // For ScreenScraper, which typically returns large responses, use streaming if:
    // 1. Unknown Content-Length (-1) and memory < 150000, or
    // 2. Known Content-Length > 20000, or  
    // 3. Actual memory < 130000
    bool shouldUseStreaming = (contentLength == -1 && currentHeap < 150000) || 
                             (contentLength > 20000) || 
                             (currentHeap < 130000);
    
    if (shouldUseStreaming) {
      Serial.printf("Using streaming: contentLength=%d, heap=%d\n", contentLength, currentHeap);
      
      // Streaming processing inline (without new function)
      WiFiClient* stream = http.getStreamPtr();
      String searchBuffer = "";
      String gameId = "";
      String gameName = "";
      String systemeId = "";
      bool foundGame = false;
      
      while (http.connected() && stream->available()) {
        if (ESP.getFreeHeap() < 60000) {
          Serial.printf("Low memory during streaming, stopping\n");
          break;
        }
        
        String chunk = stream->readString();
        if (chunk.length() == 0) break;
        
        searchBuffer += chunk;
        if (searchBuffer.length() > 1000) {
          searchBuffer = searchBuffer.substring(searchBuffer.length() - 500);
        }
        
        // Extract game ID - FIXED: Look specifically in jeu section
        if (gameId.length() == 0) {
          // First try to find jeu section
          int jeuPos = searchBuffer.indexOf("\"jeu\":");
          if (jeuPos == -1) jeuPos = searchBuffer.indexOf("\"jeu\" :");
          
          if (jeuPos != -1) {
            // Look for "id" after jeu section
            int idStart = searchBuffer.indexOf("\"id\":\"", jeuPos);
            if (idStart != -1) {
              idStart += 6;
              int idEnd = searchBuffer.indexOf("\"", idStart);
              if (idEnd != -1) {
                gameId = searchBuffer.substring(idStart, idEnd);
                Serial.printf("Streamed Game ID from jeu section: %s\n", gameId.c_str());
              }
            }
          }
        }
        
        // Extract systeme ID
        if (systemeId.length() == 0) {
          int systemeStart = searchBuffer.indexOf("\"systeme\":{\"id\":\"");
          if (systemeStart != -1) {
            int idStart = searchBuffer.indexOf("\"id\":\"", systemeStart);
            if (idStart != -1) {
              idStart += 6;
              int idEnd = searchBuffer.indexOf("\"", idStart);
              if (idEnd != -1) {
                systemeId = searchBuffer.substring(idStart, idEnd);
                Serial.printf("Streamed Systeme ID: %s\n", systemeId.c_str());
              }
            }
          }
        }
        
        // Extract game name
        if (gameName.length() == 0) {
          int nomPos = searchBuffer.indexOf("\"nom\":");
          if (nomPos != -1) {
            int nomStart = nomPos + 6;
            while (nomStart < searchBuffer.length() && searchBuffer.charAt(nomStart) != '"') nomStart++;
            nomStart++;
            
            String extractedName = "";
            int pos = nomStart;
            while (pos < searchBuffer.length() && searchBuffer.charAt(pos) != '"' && extractedName.length() < 100) {
              extractedName += searchBuffer.charAt(pos);
              pos++;
            }
            
            if (extractedName.length() > 0) {
              gameName = extractedName;
              Serial.printf("Streamed Game Name: %s\n", gameName.c_str());
            }
          }
        }
        
        // Stop if we have everything
        if (gameId.length() > 0 && systemeId.length() > 0 && gameName.length() > 0) {
          foundGame = true;
          break;
        }
        
        delay(10);
      }
      
      // Set streaming results
      if (foundGame && gameId.length() > 0) {
        result.gameId = gameId;
        result.gameName = gameName.length() > 0 ? gameName : romDetails.filename;
        result.systemeId = systemeId;
        result.found = true;
        
        Serial.printf("STREAMING SUCCESS!\n");
        String detectedMediaType = "box-3D(wor)";
        result.boxartUrl = buildCorrectMediaJeuUrl(result.gameId, systemId, detectedMediaType, result.systemeId);
      } else {
        Serial.printf("Streaming extraction failed - gameId: '%s', systemeId: '%s'\n", 
                      gameId.c_str(), systemeId.c_str());
      }
      
    } else {
      // OPTIMIZATION: Traditional response but with improved verifications
      Serial.printf("Using traditional processing: contentLength=%d, heap=%d\n", contentLength, currentHeap);
      
      // Check if we have minimum memory for response processing
      if (currentHeap < 90000) {
        Serial.printf("Insufficient memory for traditional processing (need 90000+, have %d)\n", currentHeap);
        http.end();
        return result;
      }
      
      String response = http.getString();
      Serial.printf("Response received: %d bytes\n", response.length());
      Serial.printf("Memory after getString: %d bytes\n", ESP.getFreeHeap());
      
      // CRITICAL: Check if response was actually received
      if (response.length() == 0) {
        Serial.printf("Empty response received despite successful HTTP 200\n");
        http.end();
        return result;
      }
      
      // MEMORY SAFETY: If response is too large for current memory, truncate carefully
      int memoryAfterResponse = ESP.getFreeHeap();
      if (response.length() > 10000 && memoryAfterResponse < 80000) {
        Serial.printf("Large response (%d bytes) + low memory (%d bytes), truncating to 8000\n", 
                      response.length(), memoryAfterResponse);
        response = response.substring(0, 8000);
        Serial.printf("Response truncated to: %d bytes\n", response.length());
      }
      
      // Process the response
      if (response.length() > 0) {
        Serial.printf("Processing JSON response of %d bytes...\n", response.length());
        result = extractGameInfoFromJeuInfos(response, romDetails.filename);
        
        if (result.found && result.gameId.length() > 0) {
          String detectedMediaType = "box-3D(wor)";
          result.boxartUrl = buildCorrectMediaJeuUrl(result.gameId, systemId, detectedMediaType, result.systemeId);
          
          if (result.boxartUrl.length() > 0) {
            Serial.printf("BUILT MEDIAJEU URL:\n   %s\n", result.boxartUrl.c_str());
          } else {
            Serial.println("Failed to build mediaJeu URL");
            result.found = false;
          }
        } else {
          Serial.printf("Failed to extract game info from response\n");
        }
        
        // Clear response explicitly to free memory
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
  bool success = false;

  // OPTIONAL: Detect if it is a recurring search
  bool isRecurrent = crcRecurrentActive && (gameName == currentGameForCrc);
  
  Serial.printf("=== JSON-BASED SCREENSCRAPER DOWNLOAD ===\n");
  Serial.printf("Game: '%s' | Core: '%s'\n", gameName.c_str(), coreName.c_str());
  Serial.printf("Free heap at start: %d bytes\n", ESP.getFreeHeap());
  
  // Early exit: if the core isn't mapped to a ScreenScraper system, there's
  // no point asking MiSTer for ROM details 
  // the search would fail anyway. Mark search exhausted so the
  // RESCAN button hides itself and the recurrent stops.
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
          // Re-rescanning won't change this — mark search as exhausted.
          lastGameSearchExhausted = true;
          Serial.println("Marked search as exhausted (no rescan suggested)");
        }
      }
    }
  } else {
    lastRomHasCrc = false;
    Serial.println("ROM CRC not available for JSON search");
  }
  
  if (!success) {
    Serial.println("JSON method failed");
    showDownloadProgress(0, "Download failed");
    delay(2000);
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
      
      // ENHANCED: Don't clear immediately - preserve existing subsystem if available
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
    
    // ENHANCED: Don't clear subsystem on missing ROM data
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
  
  // ENHANCED: Check if this is a different game or forced update
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
      
      // ENHANCED: If this is a different subsystem, log the change
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
    
    // ENHANCED: For game changes without CRC, clear subsystem to force re-detection
    if (isDifferentGame || forceUpdate) {
      Serial.printf("Game change without CRC - clearing subsystem for re-detection\n");
      lastArcadeSystemeId = "";
    }
  }
  
  Serial.printf("=== ENHANCED SUBSYSTEM UPDATE COMPLETE ===\n");
  Serial.printf("Final state - Subsystem: '%s', Processed: '%s'\n", 
                lastArcadeSystemeId.c_str(), lastProcessedGame.c_str());
}