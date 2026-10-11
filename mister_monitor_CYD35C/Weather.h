// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
// Weather.h  —  outside temperature for the standby clock
// =============================================================================
// Fetches the current temperature from Open-Meteo (open-meteo.com): free, no
// account or API key, plain HTTP so no TLS on the ESP32. The location comes
// from config.ini, either as coordinates or as a place name that is geocoded
// once per boot. Nothing is fetched until a location is set.
//
// The sketch calls weatherTick() from the standby loop and from standbyEnter().
// The value is refreshed every WEATHER_REFRESH_MS; a failed attempt is retried
// after WEATHER_RETRY_MS. Each request blocks for up to its 3 s time-out,
// which standby tolerates: the offline state poll already does the same.
//
// config.ini keys (parsed in AppConfig.h):
//   [weather] weather_city = place name, looked up once (Open-Meteo geocoding)
//   [weather] weather_lat / weather_lon = coordinates; win over weather_city
//   [weather] weather_unit = c | f
//
// Fetch logic after the idle clock contributed by Rick van Soest.
// This file is byte-identical across all board folders.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include "AppConfig.h"

extern AppConfig appConfig;   // defined in the sketch, loaded at boot

static const unsigned long WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;
static const unsigned long WEATHER_RETRY_MS   =  2UL * 60UL * 1000UL;
static const uint32_t      WEATHER_TIMEOUT_MS = 3000;

static float         weatherTemp    = NAN;
static unsigned long weatherAt      = 0;       // last successful fetch
static unsigned long weatherTryAt   = 0;       // last attempt
static bool          weatherEver    = false;
static String        weatherGeoFor  = "";      // weather_city the lat/lon below belong to
static String        weatherGeoLat  = "";
static String        weatherGeoLon  = "";

// True when config.ini names a place or gives coordinates.
inline bool weatherConfigured() {
  String s = appConfig.weatherCity; s.trim();
  if (s.length() > 0) return true;
  String la = appConfig.weatherLat, lo = appConfig.weatherLon;
  la.trim(); lo.trim();
  return la.length() > 0 && lo.length() > 0;
}

inline bool  weatherHaveTemp()  { return weatherEver; }
inline float weatherTempValue() { return weatherTemp; }
inline char  weatherUnitLetter() {
  return appConfig.weatherUnit.equalsIgnoreCase("f") ? 'F' : 'C';
}

static String weatherUrlEncode(const String& s) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t) s[i];
    if (isalnum(c) || c == '-' || c == '.' || c == '_') out += (char) c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

// The number after "key": in body, searching from position from. Empty when absent.
static String weatherJsonNumber(const String& body, const char* key, int from) {
  String k = String("\"") + key + "\":";
  int i = body.indexOf(k, from);
  if (i < 0) return "";
  i += k.length();
  int j = i;
  while (j < (int) body.length() && (isdigit(body[j]) || body[j] == '-' || body[j] == '.')) j++;
  return body.substring(i, j);
}

// Coordinates from weather_lat/weather_lon, or else weather_city via geocoding.
static bool weatherLocation(String& lat, String& lon) {
  lat = appConfig.weatherLat; lon = appConfig.weatherLon;
  lat.trim(); lon.trim();
  if (lat.length() > 0 && lon.length() > 0) return true;
  String city = appConfig.weatherCity;
  city.trim();
  if (city.length() == 0) return false;
  if (city == weatherGeoFor && weatherGeoLat.length() > 0) {
    lat = weatherGeoLat; lon = weatherGeoLon;
    return true;
  }

  HTTPClient http;
  http.begin("http://geocoding-api.open-meteo.com/v1/search?count=1&format=json&name=" +
             weatherUrlEncode(city));
  http.setTimeout(WEATHER_TIMEOUT_MS);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    int res = body.indexOf("\"results\":[");
    if (res >= 0) {
      weatherGeoLat = weatherJsonNumber(body, "latitude", res);
      weatherGeoLon = weatherJsonNumber(body, "longitude", res);
      weatherGeoFor = city;
      Serial.printf("[WEATHER] %s -> %s, %s\n", city.c_str(), weatherGeoLat.c_str(), weatherGeoLon.c_str());
    } else {
      Serial.printf("[WEATHER] place '%s' not found\n", city.c_str());
    }
  } else {
    Serial.printf("[WEATHER] geocoding HTTP %d\n", code);
  }
  http.end();
  if (weatherGeoFor != city || weatherGeoLat.length() == 0) return false;
  lat = weatherGeoLat; lon = weatherGeoLon;
  return true;
}

// Refreshes the temperature when it is due. Cheap to call often: it returns
// at once unless a fetch is actually pending.
inline void weatherTick() {
  if (!weatherConfigured() || WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  bool due = !weatherEver ? (weatherTryAt == 0 || now - weatherTryAt > WEATHER_RETRY_MS)
                          : (now - weatherAt > WEATHER_REFRESH_MS &&
                             now - weatherTryAt > WEATHER_RETRY_MS);
  if (!due) return;
  weatherTryAt = now;
  String lat, lon;
  if (!weatherLocation(lat, lon)) return;

  HTTPClient http;
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + lat + "&longitude=" + lon +
               "&current=temperature_2m";
  if (weatherUnitLetter() == 'F') url += "&temperature_unit=fahrenheit";
  http.begin(url);
  http.setTimeout(WEATHER_TIMEOUT_MS);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    int cur = body.indexOf("\"current\":{");
    String v = cur >= 0 ? weatherJsonNumber(body, "temperature_2m", cur) : "";
    if (v.length() > 0) {
      weatherTemp = v.toFloat();
      weatherAt   = now;
      weatherEver = true;
      Serial.printf("[WEATHER] outside temperature %.1f %c\n", weatherTemp, weatherUnitLetter());
    }
  } else {
    Serial.printf("[WEATHER] Open-Meteo HTTP %d\n", code);
  }
  http.end();
}
