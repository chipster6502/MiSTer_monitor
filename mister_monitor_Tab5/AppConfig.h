// =============================================================================
// AppConfig.h  —  Configuration loader for MiSTer Monitor
// =============================================================================
// Reads /config.ini from the root of the microSD card at boot.
// Call loadConfig(appConfig) in setup() AFTER SD.begin() and BEFORE WiFi.begin().
//
// Any key absent from config.ini keeps the default value defined in the struct.
// The file format is a simple INI with [sections], key=value pairs, and
// comment lines starting with ; or #.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <SD.h>

// -----------------------------------------------------------------------------
// AppConfig — holds every user-configurable parameter.
// Default values here are used when config.ini is missing or a key is absent.
// -----------------------------------------------------------------------------
struct AppConfig {

  // --- WiFi ------------------------------------------------------------------
  String ssid         = "YOUR_WIFI_SSID";
  String wifiPass     = "YOUR_WIFI_PASSWORD";

  // --- MiSTer ----------------------------------------------------------------
  String misterIP     = "192.168.1.100";

  // --- ScreenScraper credentials ---------------------------------------------
  String ssUser       = "";
  String ssPass       = "";
  String ssDevUser    = "";
  String ssDevPass    = "";

  // --- ScreenScraper behaviour -----------------------------------------------
  String boxartRegion = "wor";   // wor=world  us  eu  jp
  int    ssTimeout    = 30000;
  int    ssRetries    = 2;
  bool   ssUseHttps   = false;

  // --- Image paths -----------------------------------------------------------
  String coreImagesPath    = "/cores";
  String defaultCoreImage  = "/cores/menu.jpg";

  // --- Image behaviour -------------------------------------------------------
  int    coreImageTimeout      = 30000;
  bool   alphabeticalFolders   = true;
  bool   autoDownload          = true;
  int    maxImageSize          = 500000;
  int    downloadTimeout       = 30000;
  bool   forceCoreRedownload   = false;
  bool   forceGameRedownload   = false;

  // --- Media type search order -----------------------------------------------
  // Comma-separated list of media type tokens tried in priority order.
  //
  // Available tokens:
  //
  //   Wheels (logo artwork on a coloured background):
  //     wheel-steel   Steel/metallic background wheel
  //                   API: wheel-steel(wor/us/eu/jp), wheel-steel
  //     wheel-carbon  Carbon fibre background wheel
  //                   API: wheel-carbon(wor/us/eu/jp), wheel-carbon
  //     wheel         Plain/transparent background wheel
  //                   API: wheel(wor/us/eu/jp), wheel
  //
  //   Boxes:
  //     box3d         3-D rendered box art
  //                   API: box-3D(wor/us/eu/jp), box-3D
  //     box2d         2-D flat box scan
  //                   API: box-2D(wor/us/eu/jp)
  //
  //   Other types:
  //     fanart        Fan art / promotional artwork
  //     marquee       Arcade cabinet marquee header
  //     screenshot    In-game title screenshot  (API: sstitle)
  //     photo         Real photograph of hardware or cartridge
  //     illustration  Promotional illustration / poster art
  //     mix           MixRBV composite image
  //                   API: mixrbv(wor/us/eu/jp), mixrbv
  //
  // Within each token the region fallback order is fixed:
  //   wor → us → eu → jp → generic
  //
  // Three separate lists for the three download contexts:
  //
  //   game_media_order   — non-arcade games
  //   arcade_media_order — arcade games
  //   core_media_order   — system-level core artwork (non-arcade systems
  //                        AND arcade subsystems)

  // Non-arcade games: boxes first, then wheels, then other types
  String gameMediaOrder   = "box3d,box2d,wheel-carbon,wheel-steel,wheel,fanart,marquee,screenshot";

  // Arcade games (ROM level): logo art before boxes (no physical box exists for most)
  String arcadeMediaOrder = "fanart,marquee,wheel-carbon,wheel-steel,wheel,box3d,box2d,screenshot";

  // Arcade subsystems (hardware platform level, e.g. CPS1, Neo Geo, Konami):
  // wheel art only — subsystem images are logos, not game-specific artwork
  String arcadeSubsystemMediaOrder = "wheel-steel,wheel-carbon,wheel";

  // Non-arcade system cores: steel wheel first for a clean HUD look,
  // then photo/illustration, then box art as last resort
  String coreMediaOrder   = "wheel-steel,wheel-carbon,wheel,photo,illustration,box3d,box2d,marquee,fanart,screenshot";

  // --- UI / scroll -----------------------------------------------------------
  int    scrollSpeedMs      = 300;
  int    scrollPauseStartMs = 2000;
  int    scrollPauseEndMs   = 3000;

  // --- Debug -----------------------------------------------------------------
  bool   debugMode      = false;
};

// -----------------------------------------------------------------------------
// parseBool() — accepts "true"/"1"/"yes" (case-insensitive) as true
// -----------------------------------------------------------------------------
inline bool parseBool(const String& val) {
  String v = val;
  v.toLowerCase();
  return (v == "true" || v == "1" || v == "yes");
}

// -----------------------------------------------------------------------------
// loadConfig() — reads /config.ini from SD and populates cfg.
// -----------------------------------------------------------------------------
inline void loadConfig(AppConfig& cfg) {
  const char* CONFIG_PATH = "/config.ini";

  if (!SD.exists(CONFIG_PATH)) {
    Serial.println("[CONFIG] /config.ini not found — using built-in defaults");
    return;
  }

  File f = SD.open(CONFIG_PATH, FILE_READ);
  if (!f) {
    Serial.println("[CONFIG] Failed to open /config.ini — using built-in defaults");
    return;
  }

  Serial.println("[CONFIG] Reading /config.ini …");
  int keysLoaded = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() == 0 || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;

    int eqPos = line.indexOf('=');
    if (eqPos < 1) continue;

    String key = line.substring(0, eqPos);
    String val = line.substring(eqPos + 1);
    key.trim();
    val.trim();
    if (key.length() == 0 || val.length() == 0) continue;

    // [wifi]
    if      (key == "ssid")                   { cfg.ssid = val; }
    else if (key == "password")               { cfg.wifiPass = val; }
    // [mister]
    else if (key == "ip")                     { cfg.misterIP = val; }
    // [screenscraper]
    else if (key == "ss_user")                { cfg.ssUser = val; }
    else if (key == "ss_pass")                { cfg.ssPass = val; }
    else if (key == "ss_dev_user")            { cfg.ssDevUser = val; }
    else if (key == "ss_dev_pass")            { cfg.ssDevPass = val; }
    else if (key == "region")                 { cfg.boxartRegion = val; }
    else if (key == "timeout")                { cfg.ssTimeout = val.toInt(); }
    else if (key == "retries")                { cfg.ssRetries = val.toInt(); }
    else if (key == "use_https")              { cfg.ssUseHttps = parseBool(val); }
    // [images]
    else if (key == "base_path")              { cfg.coreImagesPath = val; }
    else if (key == "default_image")          { cfg.defaultCoreImage = val; }
    else if (key == "core_image_timeout")     { cfg.coreImageTimeout = val.toInt(); }
    else if (key == "alphabetical_folders")   { cfg.alphabeticalFolders = parseBool(val); }
    else if (key == "auto_download")          { cfg.autoDownload = parseBool(val); }
    else if (key == "max_image_size")         { cfg.maxImageSize = val.toInt(); }
    else if (key == "download_timeout")       { cfg.downloadTimeout = val.toInt(); }
    else if (key == "force_core_redownload")  { cfg.forceCoreRedownload = parseBool(val); }
    else if (key == "force_game_redownload")  { cfg.forceGameRedownload = parseBool(val); }
    else if (key == "game_media_order")              { cfg.gameMediaOrder = val; }
    else if (key == "arcade_media_order")            { cfg.arcadeMediaOrder = val; }
    else if (key == "arcade_subsystem_media_order")  { cfg.arcadeSubsystemMediaOrder = val; }
    else if (key == "core_media_order")              { cfg.coreMediaOrder = val; }
    // [ui]
    else if (key == "scroll_speed_ms")        { cfg.scrollSpeedMs = val.toInt(); }
    else if (key == "scroll_pause_start_ms")  { cfg.scrollPauseStartMs = val.toInt(); }
    else if (key == "scroll_pause_end_ms")    { cfg.scrollPauseEndMs = val.toInt(); }
    // [debug]
    else if (key == "debug")                  { cfg.debugMode = parseBool(val); }
    else {
      Serial.printf("[CONFIG] Unknown key ignored: '%s'\n", key.c_str());
      continue;
    }

    keysLoaded++;
  }

  f.close();
  Serial.printf("[CONFIG] Loaded %d parameter(s) from %s\n", keysLoaded, CONFIG_PATH);
}
