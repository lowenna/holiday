#include <WiFiManager.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <XPT2046_Touchscreen.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "audio.h"
#include "maldives.h" // Hexdump of 320x240 JPG background
#include "NotoSansBold36.h"

char otaHostname[24];
WebServer otaServer(80);
bool otaInProgress = false;

extern void updateScreenSaverEyes(bool);                          // eyes.cpp
extern void drawWeatherBadge();                                   // weather.cpp
extern void fetchDestinationWeather();                            // weather.cpp
extern bool handleWeatherForecastTouch(uint16_t tx, uint16_t ty); // weather.cpp
extern String weatherTempStr;                                     // weather.cpp
extern String weatherCondStr;                                     // weather.cpp
extern bool hasCachedCoords;                                      // weather.cpp
extern long destUtcOffsetSec;                                     // weather.cpp
extern bool hasDestUtcOffset;                                     // weather.cpp
bool showDestLocalTime = false;

// ============================================================================
// Hardware & Layout Constants
// ============================================================================
constexpr uint8_t AZTOUCH_BEEPER = 21;
constexpr uint8_t AZTOUCH_TOUCH_CS = 14;
constexpr uint8_t AZTOUCH_TOUCH_IRQ = 27;

// AZ-Touch MOD V01-03, 2.8" display fixed calibration values
constexpr int16_t TOUCH_MIN_X = 370;
constexpr int16_t TOUCH_MIN_Y = 470;
constexpr int16_t TOUCH_MAX_X = 3700;
constexpr int16_t TOUCH_MAX_Y = 3600;
constexpr int16_t TOUCH_MIN_Z = 10;

// Screen layout coordinates (320x240 landscape)
constexpr int16_t SCREEN_W = 320;
constexpr int16_t SCREEN_H = 240;
constexpr int16_t TOP_BAR_H = 16;
constexpr int16_t TOP_LINE_Y = 17;
constexpr int16_t BG_IMAGE_Y = 24;
constexpr int16_t BG_IMAGE_MAX_H = 209;
constexpr int16_t COUNTDOWN_Y = 158;
constexpr int16_t COUNTDOWN_H = 48;
constexpr int16_t BOT_LINE_Y = 222;
constexpr int16_t BOT_TEXT_Y = 223;
extern int16_t WHERE_Y;
extern unsigned long WEATHER_INTERVAL_MS;
extern unsigned long lastWeatherFetchMs;

// ============================================================================
// Blinking Eyes Screen Saver (Activates after SCREENSAVER_TIMEROUT_MS of no touch)
// ============================================================================
constexpr unsigned long SCREENSAVER_TIMEOUT_MS = 300000UL * 12; // 5 mins is 300000UL
bool inScreenSaver = false;
unsigned long lastInteractionMillis = 0;

// NTP Timezone (UK GMT/BST) & OTA Hostname
const char *MY_TZ = "GMT0BST,M3.5.0/1,M10.5.0";

// ============================================================================
// Global Objects & State
// ============================================================================
XPT2046_Touchscreen touch(AZTOUCH_TOUCH_CS, AZTOUCH_TOUCH_IRQ);
TFT_eSPI tft = TFT_eSPI();
// WebServer otaServer(80);

// Persistent sprites to avoid heap fragmentation and repeated JPG decoding
TFT_eSprite topTimeSprite(&tft);
TFT_eSprite countdownBgSprite(&tft);
TFT_eSprite countdownFgSprite(&tft);

Preferences prefs;
bool shouldSaveConfig = false;
bool holidayCelebrated = false;
bool touchWasDown = false;

// Default holiday configuration (overwritten by NVS if saved)
char gHour[3] = "10";
char gMins[3] = "00";
char gDay[3] = "05";
char gMonth[3] = "12";
char gYear[5] = "2026";
char gWhere[21] = "Maldives";
bool gScreensaverEnabled = true;
bool gWeatherEnabled = true;
bool gDimmingEnabled = true;
bool gIslandPlusOne = false;
time_t rawhol = 0;

// ============================================================================
// Error & Status Reporting
// ============================================================================
void showScreenError(const char *title, const char *detail, bool fullScreen = true, unsigned long pauseMs = 3000)
{
  Serial.printf("[ERROR] %s: %s\n", title, detail);

  if (fullScreen)
  {
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, SCREEN_W, 24, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(title, SCREEN_W / 2, 12, 2);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setCursor(8, 36);
    tft.println(detail);
  }
  else
  {
    topTimeSprite.fillSprite(TFT_RED);
    topTimeSprite.setTextColor(TFT_WHITE, TFT_RED);
    topTimeSprite.drawCentreString(detail, SCREEN_W / 2, 0, 2);
    topTimeSprite.pushSprite(0, 0);
  }

  if (pauseMs > 0)
  {
    delay(pauseMs);
  }
}

void handleOTAUpload()
{
  HTTPUpload &upload = otaServer.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    otaInProgress = true;
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawCentreString("OTA UPDATE IN PROGRESS...", 160, 100, 4);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Update.printError(Serial);
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.printError(Serial);
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (Update.end(true))
    {
      otaServer.send(200, "text/plain", "Update OK! Rebooting...");
      delay(1000);
      ESP.restart();
    }
    else
    {
      otaInProgress = false;
      showScreenError("OTA FAILED", "Firmware flash failed!", true, 3000);
    }
  }
}

void drawOtaProgressScreen(const char *statusText, int percent)
{
  if (!otaInProgress)
  {
    otaInProgress = true;
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, SCREEN_W, 26, TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("FIRMWARE OTA UPDATE", SCREEN_W / 2, 13, 2);
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.fillRect(0, 50, SCREEN_W, 30, TFT_BLACK);
  tft.drawString(statusText, SCREEN_W / 2, 65, 2);

  // Progress bar
  int barW = 260;
  int barH = 22;
  int barX = (SCREEN_W - barW) / 2;
  int barY = 105;
  tft.drawRect(barX, barY, barW, barH, TFT_WHITE);
  int fillW = map(constrain(percent, 0, 100), 0, 100, 0, barW - 4);
  tft.fillRect(barX + 2, barY + 2, fillW, barH - 4, TFT_GREEN);

  char pctBuf[16];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", percent);
  tft.fillRect(0, 140, SCREEN_W, 26, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(pctBuf, SCREEN_W / 2, 152, 4);
}

// ============================================================================
// Web & ArduinoOTA Setup (Matches F1-Info-display style)
// ============================================================================
const char *otaIndexHtml = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Holiday Countdown OTA</title>
  <style>
    body { font-family: sans-serif; background: #111; color: #eee; text-align: center; padding: 30px; }
    .card { background: #1e1e1e; max-width: 400px; margin: auto; padding: 24px; border-radius: 10px; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
    h2 { color: #4dd0e1; margin-top: 0; }
    input[type=file] { margin: 18px 0; color: #ccc; }
    button { background: #00acc1; color: #fff; border: none; padding: 12px 24px; font-size: 16px; border-radius: 6px; cursor: pointer; }
    button:hover { background: #00838f; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Holiday Countdown OTA</h2>
    <p>Select compiled firmware (<b>.bin</b>) to flash wirelessly:</p>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="update" accept=".bin" required><br>
      <button type="submit">Upload &amp; Flash</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

bool setupOTA()
{
  bool mdnsOk = MDNS.begin(otaHostname);
  if (mdnsOk)
  {
    MDNS.addService("http", "tcp", 80);
  }

  otaServer.on("/", HTTP_GET, []()
               {
    String html = "<html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
                  "<h2>" + String(gWhere) + " Countdown OTA</h2>"
                  "<p>Host: <b>" + String(otaHostname) + ".local</b></p>"
                  "<form method='POST' action='/update' enctype='multipart/form-data'>"
                  "<input type='file' name='update' accept='.bin' required><br><br>"
                  "<button type='submit'>Upload &amp; Flash</button></form></body></html>";
    otaServer.send(200, "text/html", html); });

  otaServer.on("/update", HTTP_POST, []()
               {
    otaServer.send(200, "text/plain", Update.hasError() ? "Update Failed" : "Update OK! Rebooting...");
    delay(1000);
    ESP.restart(); }, handleOTAUpload);

  otaServer.begin();
  Serial.printf("OTA Ready: http://%s.local (IP: %s)\n", otaHostname, WiFi.localIP().toString().c_str());
  return mdnsOk;
}

long getEffectiveDestOffsetSec()
{
  return destUtcOffsetSec + (gIslandPlusOne ? 3600L : 0L);
}

// ============================================================================
// Touch, Audio & Display Helpers
// ============================================================================
static bool getTouchPixel(uint16_t &x, uint16_t &y)
{
  if (!touch.touched())
    return false;
  TS_Point p = touch.getPoint();
  if (p.z <= TOUCH_MIN_Z)
    return false;

  long px = map((long)p.x, TOUCH_MIN_X, TOUCH_MAX_X, SCREEN_W, 0);
  long py = map((long)p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, SCREEN_H, 0);

  x = (uint16_t)constrain(px, 0L, (long)(SCREEN_W - 1));
  y = (uint16_t)constrain(py, 0L, (long)(SCREEN_H - 1));
  return true;
}

void playHolidayFanfare()
{
  LcarsAudio::beepBoot();
  unsigned long start = millis();
  while (millis() - start < 450)
  {
    LcarsAudio::update();
  }

  const int notes[] = {523, 659, 784, 1047};
  const int durations[] = {140, 140, 140, 420};
  for (int i = 0; i < 4; i++)
  {
    tone(AZTOUCH_BEEPER, notes[i], durations[i]);
    delay(durations[i] + 40);
  }
  noTone(AZTOUCH_BEEPER);
}

bool tftOutputMainBg(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= BG_IMAGE_MAX_H)
    return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

bool tftOutputCountdownBg(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= COUNTDOWN_H)
    return false;
  countdownBgSprite.pushImage(x, y, w, h, bitmap);
  return true;
}
// Formats a time_t as "Sat 5th Dec 10AM" or "Sat 5th Dec 10:05AM"
// Formats a time_t as "Sat 5th Dec 10AM" (or "Sat 5th Dec 10:05:32AM" when includeSeconds = true)
// Formats a time_t as "Sat 5th Dec 10AM" (when includeSeconds = false)
// or 24-hour "Sat 5th Dec 20:05:32" (when includeSeconds = true)
String formatPrettyDateTime(time_t t, bool includeSeconds = false)
{
  struct tm tmVal;
  localtime_r(&t, &tmVal);

  const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  int day = tmVal.tm_mday;
  const char *suffix = "th";
  if (day % 100 < 11 || day % 100 > 13)
  {
    if (day % 10 == 1)
      suffix = "st";
    else if (day % 10 == 2)
      suffix = "nd";
    else if (day % 10 == 3)
      suffix = "rd";
  }
  suffix = ""; // :( JJH as too big

  String res = String(days[tmVal.tm_wday]) + " " + String(day) + suffix + " " +
               String(months[tmVal.tm_mon]) + " ";

  if (includeSeconds)
  {
    // 24-hour format with leading zeros and no AM/PM (e.g., "20:05:32")
    res += (tmVal.tm_hour < 10 ? "0" : "") + String(tmVal.tm_hour);
    res += (tmVal.tm_min < 10 ? ":0" : ":") + String(tmVal.tm_min);
    res += (tmVal.tm_sec < 10 ? ":0" : ":") + String(tmVal.tm_sec);
  }
  else
  {
    // 12-hour compact format for bottom-right target date (e.g., "10AM" or "10:05AM")
    int hour12 = tmVal.tm_hour % 12;
    if (hour12 == 0)
      hour12 = 12;
    const char *ampm = (tmVal.tm_hour >= 12) ? "PM" : "AM";

    res += String(hour12);
    if (tmVal.tm_min > 0)
    {
      res += (tmVal.tm_min < 10 ? ":0" : ":") + String(tmVal.tm_min);
    }
    res += ampm;
  }

  return res;
}

void displayCurrentTimeSprite(time_t rawLocal)
{
  topTimeSprite.fillSprite(TFT_BLACK);
  topTimeSprite.setFreeFont(nullptr);
  topTimeSprite.setTextFont(2);
  topTimeSprite.setTextSize(1);

  if (showDestLocalTime && hasDestUtcOffset)
  {
    time_t destEpoch = rawLocal + getEffectiveDestOffsetSec();
    struct tm destTm;
    gmtime_r(&destEpoch, &destTm);

    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    String timeStr = String(gWhere) + " " + String(days[destTm.tm_wday]) + " " +
                     String(destTm.tm_mday) + " " + String(months[destTm.tm_mon]) + " " +
                     (destTm.tm_hour < 10 ? "0" : "") + String(destTm.tm_hour) + ":" +
                     (destTm.tm_min < 10 ? "0" : "") + String(destTm.tm_min) + ":" +
                     (destTm.tm_sec < 10 ? "0" : "") + String(destTm.tm_sec);

    topTimeSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    topTimeSprite.drawCentreString(timeStr, SCREEN_W / 2, 0, 2);
  }
  else
  {
    topTimeSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    topTimeSprite.drawCentreString(formatPrettyDateTime(rawLocal, true), SCREEN_W / 2, 0, 2);
  }

  topTimeSprite.pushSprite(0, 0);
}

void initSprites()
{
  topTimeSprite.setColorDepth(8);
  topTimeSprite.createSprite(SCREEN_W, TOP_BAR_H);
  countdownBgSprite.setColorDepth(16);
  countdownBgSprite.createSprite(SCREEN_W, COUNTDOWN_H);
  countdownBgSprite.setSwapBytes(true);
  countdownBgSprite.fillSprite(TFT_BLACK);
  TJpgDec.setCallback(tftOutputCountdownBg);
  TJpgDec.drawJpg(0, BG_IMAGE_Y - COUNTDOWN_Y, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));
  countdownFgSprite.setColorDepth(16);
  countdownFgSprite.createSprite(SCREEN_W, COUNTDOWN_H);
  countdownFgSprite.setSwapBytes(true);
}
void displayLines()
{
  tft.drawLine(0, TOP_LINE_Y, SCREEN_W - 1, TOP_LINE_Y, TFT_WHITE);
  tft.drawLine(0, BOT_LINE_Y, SCREEN_W - 1, BOT_LINE_Y, TFT_WHITE);
}
void drawStaticBackgroundImage()
{
  TJpgDec.setCallback(tftOutputMainBg);
  TJpgDec.drawJpg(0, BG_IMAGE_Y, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));
}

// Decoder callback for redrawing the 320x38 background slice behind the destination text
// Global pointer so the JPEG decoder callback can draw the background slice straight into our sprite
static TFT_eSprite *gWhereBgSpritePtr = nullptr;

bool tftOutputWhereBg(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= 42 || gWhereBgSpritePtr == nullptr)
    return false;
  gWhereBgSpritePtr->pushImage(x, y, w, h, bitmap);
  return true;
}

static TFT_eSprite *gSliceSpritePtr = nullptr;
static int16_t gSliceMaxH = 0;

bool tftOutputBgSlice(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= gSliceMaxH || gSliceSpritePtr == nullptr)
    return false;
  gSliceSpritePtr->pushImage(x, y, w, h, bitmap);
  return true;
}

void fillSpriteWithMaldivesSlice(TFT_eSprite &spr, int16_t screenY, int16_t sliceH)
{
  spr.setSwapBytes(true);
  spr.fillSprite(TFT_BLACK);
  gSliceSpritePtr = &spr;
  gSliceMaxH = sliceH;
  TJpgDec.setCallback(tftOutputBgSlice);
  TJpgDec.drawJpg(0, BG_IMAGE_Y - screenY, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));
  gSliceSpritePtr = nullptr;
}

void drawWhere(bool isArrived = false)
{
  const int16_t bannerH = 42;

  // 1. Create a 16-bit sprite and decode the exact Maldives photo slice into it first
  TFT_eSprite bg(&tft);
  bg.setColorDepth(16);
  bg.createSprite(SCREEN_W, bannerH);
  bg.setSwapBytes(true);
  bg.fillSprite(TFT_BLACK);

  gWhereBgSpritePtr = &bg;
  TJpgDec.setCallback(tftOutputWhereBg);
  TJpgDec.drawJpg(0, BG_IMAGE_Y - WHERE_Y, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));
  gWhereBgSpritePtr = nullptr;

  String displayText = isArrived ? (String(gWhere) + "!") : String(gWhere);
  bg.setTextDatum(MC_DATUM);

#ifdef SMOOTH_FONT
  // Load anti-aliased smooth font; TFT_eSPI automatically blends edges against existing sprite pixels
  bg.loadFont(NotoSansBold36);

  // Draw anti-aliased drop shadow, then anti-aliased foreground text
  bg.setTextColor(TFT_NAVY, TFT_BLACK, false);
  bg.drawString(displayText, (SCREEN_W / 2) + 2, 22);

  bg.setTextColor(isArrived ? TFT_GOLD : TFT_WHITE, TFT_BLACK, false);
  bg.drawString(displayText, SCREEN_W / 2, 20);

  bg.unloadFont();
#else
  bg.setFreeFont(isArrived ? &FreeSansBold12pt7b : &FreeSansBold18pt7b);
  bg.setTextColor(TFT_NAVY);
  bg.drawString(displayText, (SCREEN_W / 2) + 2, 22);
  bg.setTextColor(isArrived ? TFT_GOLD : TFT_WHITE);
  bg.drawString(displayText, SCREEN_W / 2, 20);
#endif

  bg.pushSprite(0, WHERE_Y);
  bg.deleteSprite();
}

void drawCountdownLeft(int days, int hours, int mins, int secs, bool showColon, bool isArrived, time_t rawlocal)
{
  countdownBgSprite.pushToSprite(&countdownFgSprite, 0, 0);
  countdownFgSprite.setTextSize(1);
  countdownFgSprite.setTextFont(7);

  if (isArrived)
  {
    // Once arrived, show live local time at the destination in crisp cyan (HH:MM:SS)
    time_t destEpoch = hasDestUtcOffset ? (rawlocal + getEffectiveDestOffsetSec()) : rawlocal;

    struct tm destTm;
    gmtime_r(&destEpoch, &destTm);

    char sep = showColon ? ':' : ' ';
    countdownFgSprite.setTextColor(TFT_DARKCYAN);
    countdownFgSprite.setCursor(54, 0);
    countdownFgSprite.printf("%02d%c", destTm.tm_hour, sep);
    countdownFgSprite.setCursor(132, 0);
    countdownFgSprite.printf("%02d%c", destTm.tm_min, sep);
    countdownFgSprite.setCursor(210, 0);
    countdownFgSprite.printf("%02d", destTm.tm_sec);
  }
  else
  {
    countdownFgSprite.setTextColor(TFT_DARKCYAN);
    char sep = showColon ? ':' : ' ';
    countdownFgSprite.setCursor(15, 0);
    countdownFgSprite.printf("%02d.", days);
    countdownFgSprite.setCursor(95, 0);
    countdownFgSprite.printf("%02d%c", hours, sep);
    countdownFgSprite.setCursor(170, 0);
    countdownFgSprite.printf("%02d%c", mins, sep);
    countdownFgSprite.setCursor(245, 0);
    countdownFgSprite.printf("%02d", secs);
  }

  countdownFgSprite.pushSprite(0, COUNTDOWN_Y);
}

// ============================================================================
// Configuration & Preferences
// ============================================================================
void saveConfigCallback()
{
  Serial.println("WiFiManager config changed; flagging for NVS save.");
  shouldSaveConfig = true;
}
void drawToggleTile(int16_t tileX, int16_t tileY, int16_t tileW, const char *label, bool isEnabled)
{
  const int16_t tileH = 42;

  tft.fillRoundRect(tileX, tileY, tileW, tileH, 6, TFT_BLACK);
  tft.drawRoundRect(tileX, tileY, tileW, tileH, 6, isEnabled ? TFT_DARKGREEN : TFT_DARKGREY);

  // Label on top-left of tile
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(label, tileX + 8, tileY + 3, 2);

  // "ON" / "OFF" status on bottom-left
  tft.setTextColor(isEnabled ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.drawString(isEnabled ? "ON" : "OFF", tileX + 8, tileY + 21, 2);

  // Compact sliding pill switch on bottom-right (46x18)
  const int16_t swW = 46;
  const int16_t swH = 18;
  const int16_t swX = tileX + tileW - swW - 8;
  const int16_t swY = tileY + 20;

  uint16_t trackCol = isEnabled ? TFT_DARKGREEN : TFT_DARKGREY;
  uint16_t borderCol = isEnabled ? TFT_GREEN : TFT_LIGHTGREY;

  tft.fillRoundRect(swX, swY, swW, swH, swH / 2, trackCol);
  tft.drawRoundRect(swX, swY, swW, swH, swH / 2, borderCol);

  int16_t knobR = 6;
  int16_t knobY = swY + (swH / 2);
  int16_t knobX = isEnabled ? (swX + swW - knobR - 3) : (swX + knobR + 3);
  tft.fillCircle(knobX, knobY, knobR, TFT_WHITE);
}

bool areWeConfiguring(bool mdnsOk)
{
  unsigned long startAt = millis();
  bool triggered = false;
  int lastSecsLeft = -1;
  bool wasTouching = true; // Ignore held finger from long-press until lifted

  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.println("To update firmware, browse to:");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  if (mdnsOk)
    tft.println("  http://" + String(otaHostname) + ".local");
  tft.println("  http://" + WiFi.localIP().toString());

  // Smaller, centred Wi-Fi / Holiday Config Button (just below y = 85)
  const int16_t cfgW = 180;
  const int16_t cfgH = 26;
  const int16_t cfgX = (SCREEN_W - cfgW) / 2; // x = 70..250
  const int16_t cfgY = 86;
  tft.drawRoundRect(cfgX, cfgY, cfgW, cfgH, 6, TFT_CYAN);
  tft.fillRoundRect(cfgX + 2, cfgY + 2, cfgW - 4, cfgH - 4, 5, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("Tap to configure", SCREEN_W / 2, cfgY + (cfgH / 2), 2);
  tft.setTextDatum(TL_DATUM);

  // Balanced 2x2 Grid Coordinates (y = 118..207)
  const int16_t col1X = 12;
  const int16_t col2X = 166;
  const int16_t tileW = 142;
  const int16_t row1Y = 118;
  const int16_t row2Y = 165;
  const int16_t tileH = 42;

  // Left Column (2 toggles)
  drawToggleTile(col1X, row1Y, tileW, "Screensaver", gScreensaverEnabled);
  drawToggleTile(col1X, row2Y, tileW, "Weather", gWeatherEnabled);

  // Right Column (2 toggles)
  drawToggleTile(col2X, row1Y, tileW, "Night Dim", gDimmingEnabled);
  drawToggleTile(col2X, row2Y, tileW, "Island +1h", gIslandPlusOne);

  // Bottom line: Copyright left-aligned, Countdown right-aligned
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("(C) John Howard 2026", 4, 224, 2);

  uint16_t tx, ty;
  while ((millis() - startAt < 10000) && !triggered)
  {
    int secsLeft = max(1, 10 - (int)((millis() - startAt) / 1000));
    if (secsLeft != lastSecsLeft)
    {
      lastSecsLeft = secsLeft;
      tft.fillRect(175, 222, 145, 18, TFT_BLACK);
      tft.setTextDatum(TR_DATUM);
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.drawString("Starting in " + String(secsLeft) + "s...", SCREEN_W - 4, 224, 2);
      tft.setTextDatum(TL_DATUM);
    }

    bool touching = getTouchPixel(tx, ty);
    if (touching && !wasTouching)
    {
      // Top-Left: Screensaver
      if (tx >= col1X && tx <= (col1X + tileW) && ty >= row1Y && ty <= (row1Y + tileH))
      {
        gScreensaverEnabled = !gScreensaverEnabled;
        prefs.begin("hol", false);
        prefs.putBool("ssaver", gScreensaverEnabled);
        prefs.end();

        drawToggleTile(col1X, row1Y, tileW, "Screensaver", gScreensaverEnabled);
        tone(AZTOUCH_BEEPER, gScreensaverEnabled ? 880 : 440, 80);

        startAt = millis();
        lastSecsLeft = -1;
      }
      // Bottom-Left: Weather
      else if (tx >= col1X && tx <= (col1X + tileW) && ty >= row2Y && ty <= (row2Y + tileH))
      {
        gWeatherEnabled = !gWeatherEnabled;
        prefs.begin("hol", false);
        prefs.putBool("wxen", gWeatherEnabled);
        prefs.end();

        drawToggleTile(col1X, row2Y, tileW, "Weather", gWeatherEnabled);
        tone(AZTOUCH_BEEPER, gWeatherEnabled ? 880 : 440, 80);

        startAt = millis();
        lastSecsLeft = -1;
      }
      // Top-Right: Night Dim
      else if (tx >= col2X && tx <= (col2X + tileW) && ty >= row1Y && ty <= (row1Y + tileH))
      {
        gDimmingEnabled = !gDimmingEnabled;
        prefs.begin("hol", false);
        prefs.putBool("dim", gDimmingEnabled);
        prefs.end();

        drawToggleTile(col2X, row1Y, tileW, "Night Dim", gDimmingEnabled);
        tone(AZTOUCH_BEEPER, gDimmingEnabled ? 880 : 440, 80);

        startAt = millis();
        lastSecsLeft = -1;
      }
      // Bottom-Right: Island +1h
      else if (tx >= col2X && tx <= (col2X + tileW) && ty >= row2Y && ty <= (row2Y + tileH))
      {
        gIslandPlusOne = !gIslandPlusOne;
        prefs.begin("hol", false);
        prefs.putBool("isl1h", gIslandPlusOne);
        prefs.end();

        drawToggleTile(col2X, row2Y, tileW, "Island +1h", gIslandPlusOne);
        tone(AZTOUCH_BEEPER, gIslandPlusOne ? 880 : 440, 80);

        startAt = millis();
        lastSecsLeft = -1;
      }
      // Main Config Button ("Tap to configure")
      else if (tx >= cfgX && tx <= (cfgX + cfgW) && ty >= cfgY && ty <= (cfgY + cfgH))
      {
        triggered = true;
      }
    }
    wasTouching = touching;
    delay(25);
  }

  tft.fillScreen(TFT_BLACK);
  return triggered;
}

// ============================================================================
// Hostname, Preferences & Boot Status Display
// ============================================================================
enum PrefStatus
{
  PREF_NOT_CONFIGURED,
  PREF_INVALID,
  PREF_VALID
};

PrefStatus gPrefStatus = PREF_NOT_CONFIGURED;

void initHostname()
{
  uint64_t chipId = ESP.getEfuseMac();
  uint16_t shortId = (uint16_t)(chipId >> 32);
  snprintf(otaHostname, sizeof(otaHostname), "holiday-%04x", shortId);
}

bool isValidDateTime(int hour, int mins, int day, int month, int year)
{
  if (hour < 0 || hour > 23)
    return false;
  if (mins < 0 || mins > 59)
    return false;
  if (month < 1 || month > 12)
    return false;
  if (day < 1 || day > 31)
    return false;
  if (year < 2024 || year > 2100)
    return false;
  return true;
}

void readPreferences()
{
  // Open in read/write mode (false) so NVS doesn't fail if the namespace hasn't been initialized yet
  prefs.begin("hol", false);
  String lHour = prefs.getString("hour", "");
  String lMins = prefs.getString("mins", "");
  String lDay = prefs.getString("day", "");
  String lMonth = prefs.getString("month", "");
  String lYear = prefs.getString("year", "");
  String lWhere = prefs.getString("where", "");
  gScreensaverEnabled = prefs.getBool("ssaver", true);
  gWeatherEnabled = prefs.getBool("wxen", true);
  gDimmingEnabled = prefs.getBool("dim", true);
  gIslandPlusOne = prefs.getBool("isl1h", false);
  prefs.end();

  if (lHour.length() == 0 && lMins.length() == 0 && lDay.length() == 0 &&
      lMonth.length() == 0 && lYear.length() == 0 && lWhere.length() == 0)
  {
    gPrefStatus = PREF_NOT_CONFIGURED;
    Serial.println("Pref: Not yet configured");
    return;
  }

  if (lHour.length() == 0 || lMins.length() == 0 || lDay.length() == 0 ||
      lMonth.length() == 0 || lYear.length() == 0 ||
      !isValidDateTime(lHour.toInt(), lMins.toInt(), lDay.toInt(), lMonth.toInt(), lYear.toInt()))
  {
    gPrefStatus = PREF_INVALID;
    Serial.println("Pref: Invalid settings found in NVS!");
    return;
  }

  strlcpy(gHour, lHour.c_str(), sizeof(gHour));
  strlcpy(gMins, lMins.c_str(), sizeof(gMins));
  strlcpy(gDay, lDay.c_str(), sizeof(gDay));
  strlcpy(gMonth, lMonth.c_str(), sizeof(gMonth));
  strlcpy(gYear, lYear.c_str(), sizeof(gYear));
  if (lWhere.length() > 0)
  {
    strlcpy(gWhere, lWhere.c_str(), sizeof(gWhere));
  }

  gPrefStatus = PREF_VALID;
  Serial.printf("Pref: At %s:%s on %s/%s/%s - %s\n", gHour, gMins, gDay, gMonth, gYear, gWhere);
}

String getSavedWifiSSID()
{
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK)
  {
    const char *ssid = reinterpret_cast<const char *>(conf.sta.ssid);
    if (ssid[0] != '\0')
    {
      return String(ssid);
    }
  }
  return "";
}

void drawBootWelcomeScreen(const String &savedSsid)
{
  tft.fillScreen(TFT_BLACK);

  // Welcome Header Bar
  tft.fillRect(0, 0, SCREEN_W, 28, TFT_NAVY);
  tft.drawFastHLine(0, 28, SCREEN_W, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("WELCOME - HOLIDAY COUNTDOWN", SCREEN_W / 2, 14, 2);

  // Preferences Card
  tft.drawRoundRect(8, 36, SCREEN_W - 16, 94, 8, TFT_DARKCYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, 42);
  tft.print("Holiday Preferences:");

  if (gPrefStatus == PREF_VALID)
  {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(16, 64);
    tft.printf("Destination: %s", gWhere);

    tft.setCursor(16, 84);
    tft.printf("Departure:   %s/%s/%s at %s:%s", gDay, gMonth, gYear, gHour, gMins);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(16, 104);
    tft.print("Status: Configured");
  }
  else if (gPrefStatus == PREF_NOT_CONFIGURED)
  {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(16, 68);
    tft.print("Status: Not yet configured");

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(16, 92);
    tft.printf("Fallback default: %s (%s/%s/%s)", gWhere, gDay, gMonth, gYear);
  }
  else
  {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(16, 68);
    tft.print("Status: Invalid settings found");

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(16, 92);
    tft.print("Please reconfigure in setup portal");
  }

  // Wi-Fi Connection Status Card
  tft.drawRoundRect(8, 138, SCREEN_W - 16, 92, 8, TFT_DARKGREY);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, 146);
  tft.print("Wi-Fi Status:");

  if (savedSsid.length() > 0)
  {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(16, 168);
    tft.print("Attempting to connect to: ");
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print(savedSsid);

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(16, 192);
    tft.print("Please wait...");
  }
  else
  {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(16, 168);
    tft.print("No saved Wi-Fi network found.");

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(16, 192);
    tft.printf("Starting hotspot %s...", otaHostname);
  }
}

void drawSetupPortalScreen(const String &apName, bool showTimeoutHint)
{
  tft.fillScreen(TFT_BLACK);

  // Header bar
  tft.fillRect(0, 0, SCREEN_W, 26, TFT_NAVY);
  tft.drawFastHLine(0, 26, SCREEN_W, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("SETUP MODE - WI-FI & HOLIDAY", SCREEN_W / 2, 13, 2);

  // Styled instruction card
  tft.drawRoundRect(8, 34, 304, 174, 8, TFT_DARKCYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);

  int y = 42;
  const int step = 22;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, y);
  tft.print("1.");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(" Open Wi-Fi settings on your phone");
  y += step;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, y);
  tft.print("2.");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(" Connect to ");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.print(apName);
  y += step;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, y);
  tft.print("3.");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(" Wait for portal (or browse ");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.print("192.168.4.1");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(")");
  y += step;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, y);
  tft.print("4.");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(" Tap ");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.print("Configure WiFi");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(" & pick network");
  y += step;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, y);
  tft.print("5.");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(" Enter Wi-Fi password & holiday info");
  y += step;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, y);
  tft.print("6.");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(" Scroll down and tap ");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.print("Save");

  if (showTimeoutHint)
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_SILVER, TFT_BLACK);
    tft.drawString("Portal timeout: 10 minutes", SCREEN_W / 2, 224, 2);
    tft.setTextDatum(TL_DATUM);
  }
}

void configModeCallback(WiFiManager *myWiFiManager)
{
  String apName = myWiFiManager->getConfigPortalSSID();
  Serial.printf("WiFi connect failed; entered AP mode: %s\n", apName.c_str());
  drawSetupPortalScreen(apName, false);
}

// Runs the WiFiManager config portal non-blockingly while updating the live MM:SS countdown
bool runNonBlockingPortal(WiFiManager &wm, unsigned long durationMs = 600000UL)
{
  bool timedOut = false;
  unsigned long portalStartMs = millis();
  int lastSecShown = -1;

  while (wm.getConfigPortalActive())
  {
    wm.process();

    unsigned long elapsed = millis() - portalStartMs;
    int secsLeft = (elapsed < durationMs) ? (int)((durationMs - elapsed) / 1000) : 0;
    if (secsLeft == 0)
      timedOut = true;

    if (secsLeft != lastSecShown)
    {
      lastSecShown = secsLeft;
      int mins = secsLeft / 60;
      int secs = secsLeft % 60;

      String timerText = "Time remaining: " +
                         String(mins < 10 ? "0" : "") + String(mins) + ":" +
                         String(secs < 10 ? "0" : "") + String(secs);

      tft.fillRect(20, 214, 280, 22, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(secsLeft <= 30 ? TFT_RED : TFT_SILVER, TFT_BLACK);
      tft.drawString(timerText, SCREEN_W / 2, 224, 2);
      tft.setTextDatum(TL_DATUM);
    }

    delay(10);
  }

  return timedOut;
}

void writePreferences(String hour, String mins, String day,
                      String month, String year, String where)
{
  hour.trim();
  mins.trim();
  day.trim();
  month.trim();
  year.trim();
  where.trim();

  int h = hour.toInt();
  int m = mins.toInt();
  int d = day.toInt();
  int mo = month.toInt();
  int y = year.toInt();

  if (!isValidDateTime(h, m, d, mo, y))
  {
    String err = "Bad date/time: " + hour + ":" + mins + " " + day + "/" + month + "/" + year;
    showScreenError("INVALID INPUT", err.c_str(), true, 4000);
    return;
  }

  if (!prefs.begin("hol", false))
  {
    showScreenError("STORAGE ERROR", "Failed to open NVS flash for saving!", true, 4000);
    return;
  }

  prefs.putString("hour", hour);
  prefs.putString("mins", mins);
  prefs.putString("day", day);
  prefs.putString("month", month);
  prefs.putString("year", year);
  prefs.putString("where", where.length() > 0 ? where : "Holiday");
  prefs.end();
  hasCachedCoords = false; // Force re-geocoding if destination changed

  // Confirm save visually on screen before rebooting
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Settings saved!", 160, 95, 4);
  String summary = where + " - " + day + "/" + month + "/" + year + " " + hour + ":" + mins;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(summary, 160, 130, 2);
  tft.setTextDatum(TL_DATUM);

  delay(2000);
  ESP.restart();
}

void drawWifiSignalBars(int x, int y)
{
  int32_t rssi = WiFi.RSSI();
  int bars = 0;
  uint16_t activeCol = TFT_GREEN;

  if (WiFi.status() == WL_CONNECTED)
  {
    if (rssi >= -55)
    {
      bars = 4;
      activeCol = TFT_GREEN;
    }
    else if (rssi >= -67)
    {
      bars = 3;
      activeCol = TFT_GREEN;
    }
    else if (rssi >= -78)
    {
      bars = 2;
      activeCol = TFT_YELLOW;
    }
    else
    {
      bars = 1;
      activeCol = TFT_RED;
    }
  }

  tft.fillRect(x, y, 18, 14, TFT_BLACK);
  for (int i = 0; i < 4; i++)
  {
    int barH = 3 + (i * 3);
    int barX = x + (i * 4);
    int barY = y + (13 - barH);
    uint16_t col = (i < bars) ? activeCol : TFT_DARKGREY;
    tft.fillRect(barX, barY, 3, barH, col);
  }
}

void displaySSIDAndWhen()
{
  tft.fillRect(0, BOT_TEXT_Y, SCREEN_W, 17, TFT_BLACK);
  drawWifiSignalBars(2, BOT_TEXT_Y + 1);

  tft.setFreeFont(nullptr);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);

  if (gWeatherEnabled && weatherTempStr.length() > 0)
  {
    int x = 23;
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(weatherTempStr, x, BOT_TEXT_Y, 2);

    int tempW = tft.textWidth(weatherTempStr, 2);
    tft.drawCircle(x + tempW + 2, BOT_TEXT_Y + 3, 2, TFT_CYAN);

    tft.drawString(weatherCondStr, x + tempW + 6, BOT_TEXT_Y, 2);
  }
  else
  {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(WiFi.SSID(), 23, BOT_TEXT_Y, 2);
  }

  // Only show target holiday date/time on the bottom right if the countdown hasn't elapsed yet
  time_t nowEpoch = time(nullptr);
  bool timerElapsed = (nowEpoch > 1700000000L) && (difftime(rawhol, nowEpoch) <= 0);

  if (!timerElapsed)
  {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(formatPrettyDateTime(rawhol), SCREEN_W - 1, BOT_TEXT_Y, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void wakeFromScreenSaver(bool isArrived)
{
  inScreenSaver = false;
  tft.fillScreen(TFT_BLACK);
  displayLines();
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  drawWhere(isArrived);
  drawWeatherBadge();
}

// ============================================================================
// Automatic Nighttime Backlight Dimming (Smooth 30s PWM Fade)
// ============================================================================
#ifndef TFT_BL
constexpr uint8_t TFT_BL_PIN = 15; // AZ-Touch MOD default backlight GPIO
#else
constexpr uint8_t TFT_BL_PIN = TFT_BL;
#endif

constexpr uint8_t BL_PWM_CHANNEL = 0;
constexpr uint8_t BL_BRIGHT_DAY = 0;           // Active-LOW: 0 = 100% full brightness
constexpr uint8_t BL_BRIGHT_NIGHT = 246;       // Active-LOW: 246 = ~3.5% very dim night mode
constexpr unsigned long BL_BOOST_MS = 15000UL; // Hold full brightness for 15s after touch
constexpr unsigned long BL_FADE_MS = 30000UL;  // Fade gradually over 30 seconds

unsigned long lastBacklightBoostMs = 0;
uint8_t currentBacklightLevel = BL_BRIGHT_DAY;

void initBacklightPWM()
{
  ledcSetup(BL_PWM_CHANNEL, 5000, 8); // 5 kHz, 8-bit resolution (0..255)
  ledcAttachPin(TFT_BL_PIN, BL_PWM_CHANNEL);
  ledcWrite(BL_PWM_CHANNEL, BL_BRIGHT_DAY);
  currentBacklightLevel = BL_BRIGHT_DAY;
}

void updateAutoBacklight(time_t rawlocal, const struct tm &ukTm)
{
  if (!gDimmingEnabled)
  {
    if (currentBacklightLevel != BL_BRIGHT_DAY)
    {
      currentBacklightLevel = BL_BRIGHT_DAY;
      ledcWrite(BL_PWM_CHANNEL, currentBacklightLevel);
    }
    return;
  }

  int hour = ukTm.tm_hour;
  int minute = ukTm.tm_min;

  // Use destination local time when available
  if (hasDestUtcOffset)
  {
    time_t destEpoch = rawlocal + getEffectiveDestOffsetSec();
    struct tm destTm;
    gmtime_r(&destEpoch, &destTm);
    hour = destTm.tm_hour;
    minute = destTm.tm_min;
  }

  int minsOfDay = (hour * 60) + minute;
  // Nighttime window at destination: 22:30 through 06:59
  bool isNight = (minsOfDay >= (22 * 60 + 30)) || (minsOfDay < (7 * 60));

  uint8_t targetLevel = BL_BRIGHT_DAY;

  if (isNight)
  {
    unsigned long now = millis();
    if (lastBacklightBoostMs == 0)
    {
      lastBacklightBoostMs = now;
    }

    unsigned long sinceTouch = now - lastBacklightBoostMs;
    if (sinceTouch < BL_BOOST_MS)
    {
      targetLevel = BL_BRIGHT_DAY;
    }
    else if (sinceTouch < (BL_BOOST_MS + BL_FADE_MS))
    {
      unsigned long fadeElapsed = sinceTouch - BL_BOOST_MS;
      targetLevel = (uint8_t)map((long)fadeElapsed, 0L, (long)BL_FADE_MS, (long)BL_BRIGHT_DAY, (long)BL_BRIGHT_NIGHT);
    }
    else
    {
      targetLevel = BL_BRIGHT_NIGHT;
    }
  }

  if (targetLevel != currentBacklightLevel)
  {
    currentBacklightLevel = targetLevel;
    ledcWrite(BL_PWM_CHANNEL, currentBacklightLevel);
  }
}

// ============================================================================
// Setup & Loop
// ============================================================================
void setup()
{
  LcarsAudio::begin(AZTOUCH_BEEPER);
  LcarsAudio::beepBoot();
  unsigned long bootStart = millis();
  while (millis() - bootStart < 400)
  {
    LcarsAudio::update();
  }

  Serial.begin(115200);

  // Initialize display & touchscreen
  tft.init();
  initBacklightPWM();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);

  touch.begin();
  touch.setRotation(1);

  // 1. Build "holiday-xxxx" hostname immediately so AP never defaults to "ESP_xxxxxx"
  initHostname();

  // 2. Load & validate saved holiday preferences
  readPreferences();

  // 3. Read stored Wi-Fi SSID from NVS before attempting connection
  WiFi.mode(WIFI_STA);
  String savedSsid = getSavedWifiSSID();

  // 4. Display Welcome Screen with Preferences & target Wi-Fi network
  drawBootWelcomeScreen(savedSsid);
  delay(2000); // Allow user time to read welcome & preferences screen

  WiFiManagerParameter custom_hol_hour("hour", "Hour (0-23)", gHour, 2);
  WiFiManagerParameter custom_hol_mins("mins", "Mins (0-59)", gMins, 2);
  WiFiManagerParameter custom_hol_day("day", "Day (1-31)", gDay, 2);
  WiFiManagerParameter custom_hol_month("month", "Month (1-12)", gMonth, 2);
  WiFiManagerParameter custom_hol_year("year", "Year (YYYY)", gYear, 4);
  WiFiManagerParameter custom_hol_where("where", "Destination", gWhere, 20);

  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setSaveParamsCallback(saveConfigCallback);
  wifiManager.setBreakAfterConfig(true);   // Exit portal immediately after Save is clicked
  wifiManager.setConfigPortalTimeout(600); // 10 minutes (600 seconds)
  wifiManager.setConfigPortalBlocking(false);

  wifiManager.addParameter(&custom_hol_hour);
  wifiManager.addParameter(&custom_hol_mins);
  wifiManager.addParameter(&custom_hol_day);
  wifiManager.addParameter(&custom_hol_month);
  wifiManager.addParameter(&custom_hol_year);
  wifiManager.addParameter(&custom_hol_where);

  // 5. Connect to saved Wi-Fi, or start non-blocking AP named "holiday-xxxx"
  bool wifiConnected = wifiManager.autoConnect(otaHostname);
  if (!wifiConnected)
  {
    // Drive the non-blocking AP portal with the live MM:SS countdown
    runNonBlockingPortal(wifiManager, 600000UL);
    wifiConnected = (WiFi.status() == WL_CONNECTED);
  }

  wifiManager.setConfigPortalBlocking(true);

  if (!wifiConnected)
  {
    showScreenError("WI-FI ERROR", "Failed to connect to Wi-Fi.\nRestarting in 5 seconds...", true, 5000);
    ESP.restart();
  }

  // Save holiday details immediately if entered during initial autoConnect setup portal
  if (shouldSaveConfig)
  {
    writePreferences(
        custom_hol_hour.getValue(),
        custom_hol_mins.getValue(),
        custom_hol_day.getValue(),
        custom_hol_month.getValue(),
        custom_hol_year.getValue(),
        custom_hol_where.getValue());
  }

  // 6. Start OTA, then show combined OTA + "Tap Screen to Configure" screen for 10s
  bool mdnsOk = setupOTA();
  bool portalRan = false;
  bool portalTimedOut = false;

  if (areWeConfiguring(mdnsOk))
  {
    portalRan = true;
    otaServer.stop();

    String apName = String(otaHostname);
    drawSetupPortalScreen(apName, false);

    // Run config portal non-blocking with live MM:SS countdown
    wifiManager.setConfigPortalBlocking(false);
    wifiManager.startConfigPortal(apName.c_str());

    portalTimedOut = runNonBlockingPortal(wifiManager, 600000UL);

    wifiManager.setConfigPortalBlocking(true);
    otaServer.begin();
  }

  tft.fillScreen(TFT_BLACK);

  // 7. Save updated holiday details if changed OR if portal was submitted before timing out
  if (shouldSaveConfig || (portalRan && !portalTimedOut))
  {
    writePreferences(
        custom_hol_hour.getValue(),
        custom_hol_mins.getValue(),
        custom_hol_day.getValue(),
        custom_hol_month.getValue(),
        custom_hol_year.getValue(),
        custom_hol_where.getValue());
  }

  // 8. Sync NTP time
  configTzTime(MY_TZ, "pool.ntp.org", "time.nist.gov");

  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("Syncing time from the Internet...");

  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo, 500) && retry < 10)
  {
    Serial.println("Waiting for NTP time sync...");
    tft.print(".");
    retry++;
  }

  if (retry >= 10)
  {
    showScreenError("NTP TIME WARNING", "Could not sync time with NTP servers yet.\nWill keep retrying in background...", true, 2500);
  }

  tft.fillScreen(TFT_BLACK);

  // 9. Calculate target holiday timestamp
  struct tm holtimeinfo = {};
  holtimeinfo.tm_hour = atoi(gHour);
  holtimeinfo.tm_min = atoi(gMins);
  holtimeinfo.tm_mday = atoi(gDay);
  holtimeinfo.tm_mon = atoi(gMonth) - 1;
  holtimeinfo.tm_year = atoi(gYear) - 1900;
  holtimeinfo.tm_isdst = -1;
  rawhol = mktime(&holtimeinfo);

  if (rawhol == (time_t)-1)
  {
    showScreenError("DATE ERROR", "Could not parse holiday target date.\nPlease re-run setup.", true, 4000);
  }

  // 10. Render static UI elements, persistent sprites & live weather
  initSprites();
  displayLines();
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  drawWhere(false);

  fetchDestinationWeather();
  drawWeatherBadge();
  lastWeatherFetchMs = millis();

  lastInteractionMillis = millis();
  lastBacklightBoostMs = millis();

  Serial.println("setup() complete");
}

void openInAppSetup()
{
  LcarsAudio::beepBoot();
  bool wasWeatherEnabled = gWeatherEnabled;
  bool wasIslandPlusOne = gIslandPlusOne;

  // Check if mDNS is active
  bool mdnsOk = true;
  if (areWeConfiguring(mdnsOk))
  {
    otaServer.stop();

    WiFiManagerParameter custom_hol_hour("hour", "Hour (0-23)", gHour, 2);
    WiFiManagerParameter custom_hol_mins("mins", "Mins (0-59)", gMins, 2);
    WiFiManagerParameter custom_hol_day("day", "Day (1-31)", gDay, 2);
    WiFiManagerParameter custom_hol_month("month", "Month (1-12)", gMonth, 2);
    WiFiManagerParameter custom_hol_year("year", "Year (YYYY)", gYear, 4);
    WiFiManagerParameter custom_hol_where("where", "Destination", gWhere, 20);

    WiFiManager wifiManager;
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setSaveParamsCallback(saveConfigCallback);
    wifiManager.setBreakAfterConfig(true);
    wifiManager.setConfigPortalTimeout(600);

    wifiManager.addParameter(&custom_hol_hour);
    wifiManager.addParameter(&custom_hol_mins);
    wifiManager.addParameter(&custom_hol_day);
    wifiManager.addParameter(&custom_hol_month);
    wifiManager.addParameter(&custom_hol_year);
    wifiManager.addParameter(&custom_hol_where);

    String apName = String(otaHostname);
    drawSetupPortalScreen(apName, false);

    wifiManager.setConfigPortalBlocking(false);
    wifiManager.startConfigPortal(apName.c_str());

    bool portalTimedOut = runNonBlockingPortal(wifiManager, 600000UL);

    wifiManager.setConfigPortalBlocking(true);
    otaServer.begin();

    if (shouldSaveConfig || !portalTimedOut)
    {
      writePreferences(
          custom_hol_hour.getValue(),
          custom_hol_mins.getValue(),
          custom_hol_day.getValue(),
          custom_hol_month.getValue(),
          custom_hol_year.getValue(),
          custom_hol_where.getValue());
    }
  }

  // Redraw main countdown screen cleanly
  tft.fillScreen(TFT_BLACK);
  displayLines();
  drawStaticBackgroundImage();
  drawWhere(false);

  if (gWeatherEnabled && (!wasWeatherEnabled || gIslandPlusOne != wasIslandPlusOne))
  {
    fetchDestinationWeather();
    lastWeatherFetchMs = millis();
  }
  displaySSIDAndWhen();
  if (gWeatherEnabled)
  {
    drawWeatherBadge();
  }

  lastInteractionMillis = millis();
}

void loop()
{
  static time_t lastRenderedSec = 0;
  static bool blinkState = false;

  otaServer.handleClient();
  if (otaInProgress)
    return;

  LcarsAudio::update();

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100))
  {
    if (!inScreenSaver)
    {
      showScreenError("NTP ERROR", "NTP TIME SYNC LOST - RETRYING...", false, 0);
    }
    delay(500);
    return;
  }

  time_t rawlocal = mktime(&timeinfo);
  double diffSecs = difftime(rawhol, rawlocal);
  bool isArrived = (diffSecs <= 0) && (rawlocal > 1700000000L);

  uint16_t tx, ty;
  bool touching = getTouchPixel(tx, ty);
  bool freshTap = touching && !touchWasDown;
  touchWasDown = touching;

  if (touching)
  {
    lastInteractionMillis = millis();
    lastBacklightBoostMs = millis();
  }

  // Smoothly update PWM backlight every loop tick (~40 Hz)
  updateAutoBacklight(rawlocal, timeinfo);

  // Refresh destination weather every 30 minutes (even while screensaver is active)
  if (gWeatherEnabled && (millis() - lastWeatherFetchMs >= WEATHER_INTERVAL_MS))
  {
    lastWeatherFetchMs = millis();
    fetchDestinationWeather();
    if (!inScreenSaver)
    {
      drawWeatherBadge();
    }
  }

  if (inScreenSaver)
  {
    if (freshTap)
    {
      wakeFromScreenSaver(isArrived);
      lastRenderedSec = 0;
    }
    else
    {
      updateScreenSaverEyes(false);
      yield();
      return;
    }
  }

  static unsigned long topHoldStartMs = 0;
  static unsigned long lastHoldSeenMs = 0;
  const unsigned long HOLD_DURATION_MS = 1500; // 1.5s hold = Setup, quick tap = Timezone toggle

  if (touching && !inScreenSaver && ty <= 85)
  {
    lastHoldSeenMs = millis();
    if (topHoldStartMs == 0)
    {
      topHoldStartMs = millis();
    }

    unsigned long heldFor = millis() - topHoldStartMs;
    if (heldFor >= 200)
    {
      int barW = map(constrain((int)heldFor, 200, (int)HOLD_DURATION_MS), 200, (int)HOLD_DURATION_MS, 0, SCREEN_W);
      tft.drawFastHLine(0, TOP_LINE_Y, barW, TFT_CYAN);
    }

    if (heldFor >= HOLD_DURATION_MS)
    {
      topHoldStartMs = 0;
      tft.drawFastHLine(0, TOP_LINE_Y, SCREEN_W, TFT_WHITE);
      openInAppSetup();
      lastRenderedSec = 0;
      return;
    }
  }
  else if (topHoldStartMs != 0 && (millis() - lastHoldSeenMs > 150))
  {
    unsigned long totalHeld = lastHoldSeenMs - topHoldStartMs;
    topHoldStartMs = 0;
    tft.drawFastHLine(0, TOP_LINE_Y, SCREEN_W, TFT_WHITE);

    // Quick tap (< 600ms) toggles between UK Time and Destination Local Time
    if (totalHeld < 600 && hasDestUtcOffset)
    {
      showDestLocalTime = !showDestLocalTime;
      tone(AZTOUCH_BEEPER, showDestLocalTime ? 1047 : 784, 45);
      displayCurrentTimeSprite(rawlocal);
    }
  }

  if (gScreensaverEnabled && (millis() - lastInteractionMillis >= SCREENSAVER_TIMEOUT_MS))
  {
    inScreenSaver = true;
    updateScreenSaverEyes(true);
    yield();
    return;
  }

  if (freshTap && handleWeatherForecastTouch(tx, ty))
  {
    tone(AZTOUCH_BEEPER, 880, 45);
  }

  if (rawlocal != lastRenderedSec)
  {
    lastRenderedSec = rawlocal;
    blinkState = !blinkState;

    if (isArrived)
    {
      diffSecs = 0;
      if (!holidayCelebrated)
      {
        holidayCelebrated = true;
        drawWhere(true);
        drawWeatherBadge();
        playHolidayFanfare();
      }
    }

    int totalSecs = (int)diffSecs;
    int diffDays = totalSecs / 86400;
    int diffHours = (totalSecs % 86400) / 3600;
    int diffMins = (totalSecs % 3600) / 60;
    int remSecs = totalSecs % 60;

    drawCountdownLeft(diffDays, diffHours, diffMins, remSecs, blinkState, isArrived, rawlocal);
    displayCurrentTimeSprite(rawlocal);

    if (rawlocal % 10 == 0)
    {
      drawWifiSignalBars(2, BOT_TEXT_Y + 1);
    }
  }

  delay(25);
}
