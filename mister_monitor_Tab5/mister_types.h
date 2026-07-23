#pragma once

// ========== TYPE DEFINITIONS ==========
// Separated to avoid Arduino IDE auto-forward-declaration ordering issues

struct GameInfo {
  String gameId;
  String gameName;
  String boxartUrl;
  String systemeId;
  bool found;
};

struct RomDetails {
  String filename;
  String crc32;
  String md5;
  String sha1;
  long filesize;
  bool available;
  bool hashCalculated;
  bool fileTooLarge;
  String error;
  String path;
  unsigned long timestamp;
  String searchName;       // clean title from the server for name-based search (S6)
  bool   nameSearchHint;   // server says the CRC route cannot work for this container
  bool   noHash;           // server says the CRC can NEVER arrive (unindexable, mutable
                           // container). Distinct from !hashCalculated, which is also
                           // true while a slow hash is still in flight.
  bool   containerImage;   // search_name denotes a container image, not a game
                           // (boot.vhd, BOOT-DOS98.vhd). Never text-search these.
  String ssRomnom;         // server-resolved romnom override ('mslug2.zip'), or "".
                           // NeoGeo ships .neo containers that ScreenScraper does
                           // not index, so its CRC can never match and the lookup
                           // falls back to fuzzy-matching the filename — which
                           // misses whenever the pack's title differs from SS's
                           // (48 of 281 romsets carry a subtitle). The romset id
                           // matches exactly instead. "" = use filename, i.e. the
                           // previous behaviour.
  bool   noRomOnDisk;      // server says this game has a NAME but NO rom file on
                           // disk (SAM name-only content: Amiga demos, WHDLoad,
                           // some MGL). The name search may still run, but if it
                           // misses the firmware shows a stable NOT-IN-DATABASE
                           // card instead of the DOWNLOADING->failed flash.
                           // Added last so existing positional initializers
                           // (which stop before ssRomnom) stay valid.
};

// GAME INFO panel — metadata for the currently loaded game, extracted from
// the same ScreenScraper jeuInfos.php response used for artwork and cached
// on the microSD as a .meta sidecar next to the game image.
struct GameMeta {
  bool   loaded = false;   // fields below are valid
  String forGame;          // game name this metadata belongs to
  String lang;             // ScreenScraper language this sidecar was fetched in
  String year;             // "1990"
  String developer;        // developpeur.text
  String publisher;        // editeur.text
  String players;          // joueurs.text, e.g. "1-2"
  String rating;           // note.text + "/20"
  String genre;            // up to two genre names, "A / B"
  String synopsis;         // ASCII-folded, whitespace-collapsed, capped
};


// RETROACHIEVEMENTS panel — flat mirror of /status/retroachievements.
// Every field is a scalar; the endpoint JSON is deliberately flat so the
// firmware's extract*Value() helpers can parse it without ArduinoJson.
struct RAStatus {
  bool    enabled;             // server has ra_credentials.ini
  bool    supported;           // active core maps to an RA console
  bool    gameMatched;         // hash or corroborated fallback resolved a game
  String  status;              // "ok" | "not_configured" | "core_not_supported"
                               // | "no_game_loaded" | "rom_not_recognized" | ...
  String  matchMethod;         // "index" | "lastgame"
  String  gameTitle;
  int     total;               // achievements in the set
  int     unlocked;            // earned (soft or hardcore)
  int     unlockedHardcore;    // earned in hardcore
  int     pointsEarned;
  int     pointsTotal;
  int     pointsHardcore;
  String  core;                // friendly core name (for status messages)
  int     eventCounter;        // monotonic unlock counter (popup trigger)
  String  lastUnlockTitle;
  int     lastUnlockPoints;
  bool    lastUnlockHardcore;
  bool    valid;               // last fetch parsed successfully
};


struct ScrollTextState {
  String fullText;
  int maxChars;
  int scrollPos;
  unsigned long lastScrollTime;
  unsigned long pauseStartTime;
  bool isPaused;
  bool needsScroll;
  bool pauseAtEnd;
};

// Note: requires M5Unified.h and THEME_* defines to be included before this header
struct TouchButton {
  // Button geometry in PHYSICAL coordinates (1280x720 space for M5Tab)
  int x;
  int y;
  int w;
  int h;
  
  const char* label;
  uint16_t color;
  bool isPressed;
  
  bool contains(int touch_x, int touch_y) {
    bool insideHorizontally = (touch_x >= x) && (touch_x < (x + w));
    bool insideVertically   = (touch_y >= y) && (touch_y < (y + h));
    return insideHorizontally && insideVertically;
  }
  
  void draw(uint16_t textColor = 0x07FF /*THEME_CYAN*/) {
    int textLength = strlen(label);
    int approximateTextWidth = textLength * 18;
    int textX = x + (w / 2) - (approximateTextWidth / 2);
    int textY = y + (h / 2) - 12;
    M5.Display.setTextColor(0x0000 /*THEME_BLACK*/);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(textX + 2, textY + 2);
    M5.Display.print(label);
    M5.Display.setTextColor(textColor);
    M5.Display.setCursor(textX, textY);
    M5.Display.print(label);
  }
};