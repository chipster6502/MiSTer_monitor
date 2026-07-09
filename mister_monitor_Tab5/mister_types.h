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
};

// GAME INFO panel — metadata for the currently loaded game, extracted from
// the same ScreenScraper jeuInfos.php response used for artwork and cached
// on the microSD as a .meta sidecar next to the game image.
struct GameMeta {
  bool   loaded = false;   // fields below are valid
  String forGame;          // game name this metadata belongs to
  String year;             // "1990"
  String developer;        // developpeur.text
  String publisher;        // editeur.text
  String players;          // joueurs.text, e.g. "1-2"
  String rating;           // note.text + "/20"
  String genre;            // up to two genre names, "A / B"
  String synopsis;         // ASCII-folded, whitespace-collapsed, capped
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