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
    display.setTextColor(0x0000 /*THEME_BLACK*/);
    display.setTextSize(3);
    display.setCursor(textX + 2, textY + 2);
    display.print(label);
    display.setTextColor(textColor);
    display.setCursor(textX, textY);
    display.print(label);
  }
};