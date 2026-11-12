#include <WiFiManager.h>
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

#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>

// Weather state & cached coordinates
String weatherText = "";
float cachedLat = 0.0f;
float cachedLon = 0.0f;
bool hasCachedCoords = false;
unsigned long lastWeatherFetchMs = 0;
constexpr unsigned long WEATHER_INTERVAL_MS = 1800000UL; // 30 minutes

// Pre-baked 48x48 high-detail iris texture (~4.6 KB RAM) computed once at startup
constexpr int IRIS_TEX_SIZE = 48;
uint16_t irisTex[IRIS_TEX_SIZE * IRIS_TEX_SIZE];
bool irisTexReady = false;

char otaHostname[24];
WebServer otaServer(80);
bool otaInProgress = false;

// ============================================================================
// Hardware & Layout Constants
// ============================================================================
constexpr uint8_t AZTOUCH_BEEPER = 21;
constexpr uint8_t AZTOUCH_TOUCH_CS = 14;
constexpr uint8_t AZTOUCH_TOUCH_IRQ = 27;
constexpr uint8_t BOOT_BUTTON_PIN = 0;

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
constexpr int16_t WHERE_Y = 35;
constexpr int16_t WHERE_H = 26;
constexpr int16_t COUNTDOWN_Y = 150;
constexpr int16_t COUNTDOWN_H = 48;
constexpr int16_t BOT_LINE_Y = 222;
constexpr int16_t BOT_TEXT_Y = 223;

// ============================================================================
// Blinking Eyes Screen Saver (Activates after SCREENSAVER_TIMEROUT_MS of no touch)
// ============================================================================
constexpr unsigned long SCREENSAVER_TIMEOUT_MS = 10000UL; // 5 mins is 300000UL
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

time_t rawhol = 0;

void drawWeatherBadge()
{
  if (weatherText.length() == 0)
    return;

  TFT_eSprite wSpr(&tft);
  wSpr.setColorDepth(16);
  wSpr.createSprite(180, 20);
  wSpr.fillSprite(TFT_BLACK);

  int textW = wSpr.textWidth(weatherText, 2);
  int pillW = constrain(textW + 18, 60, 176);
  int pillX = (180 - pillW) / 2;

  wSpr.fillRoundRect(pillX, 1, pillW, 18, 8, TFT_NAVY);
  wSpr.drawRoundRect(pillX, 1, pillW, 18, 8, TFT_CYAN);
  wSpr.setTextDatum(MC_DATUM);
  wSpr.setTextColor(TFT_WHITE, TFT_NAVY);
  wSpr.drawString(weatherText, 90, 10, 2);

  wSpr.pushSprite(70, WHERE_Y + 38, TFT_BLACK);
  wSpr.deleteSprite();
}

// Fast 16-bit RGB565 linear colour blend (alpha: 0 = bg, 255 = fg)
// Fixed 16-bit RGB565 linear colour blend using signed channel deltas (prevents yellow underflow)
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t alpha)
{
  if (alpha == 0)
    return bg;
  if (alpha == 255)
    return fg;

  int32_t bgR = (bg >> 11) & 0x1F;
  int32_t bgG = (bg >> 5) & 0x3F;
  int32_t bgB = bg & 0x1F;

  int32_t fgR = (fg >> 11) & 0x1F;
  int32_t fgG = (fg >> 5) & 0x3F;
  int32_t fgB = fg & 0x1F;

  uint16_t r = (uint16_t)(bgR + (((fgR - bgR) * alpha) >> 8));
  uint16_t g = (uint16_t)(bgG + (((fgG - bgG) * alpha) >> 8));
  uint16_t b = (uint16_t)(bgB + (((fgB - bgB) * alpha) >> 8));

  return (r << 11) | (g << 5) | b;
}

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
  uint64_t chipId = ESP.getEfuseMac();
  uint16_t shortId = (uint16_t)(chipId >> 32);
  snprintf(otaHostname, sizeof(otaHostname), "holiday-%04x", shortId);

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
  topTimeSprite.setTextColor(TFT_GREEN, TFT_BLACK);
  topTimeSprite.drawCentreString(formatPrettyDateTime(rawLocal, true), SCREEN_W / 2, 0, 2);
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
bool tftOutputWhereBg(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= 38)
    return false;
  tft.pushImage(x, WHERE_Y + y, w, h, bitmap);
  return true;
}

void drawWhere(bool isArrived = false)
{
  const int16_t bannerH = 38;

  // Repaint the background slice first when arriving so old letters are wiped cleanly
  if (isArrived)
  {
    TJpgDec.setCallback(tftOutputWhereBg);
    TJpgDec.drawJpg(0, BG_IMAGE_Y - WHERE_Y, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));
  }

  TFT_eSprite t(&tft);
  t.createSprite(SCREEN_W, bannerH);
  t.fillSprite(TFT_BLACK);
  t.setTextDatum(MC_DATUM);

  String displayText = isArrived ? (String(gWhere) + "!") : String(gWhere);

#ifdef LOAD_GFXFF
  t.setFreeFont(isArrived ? &FreeSansBold12pt7b : &FreeSansBold18pt7b);
#else
  t.setTextFont(2);
  t.setTextSize(2);
#endif

  // Slightly higher vertical centre (y = 16, shadow = 18) so bottom edges never clip
  t.setTextColor(TFT_NAVY);
  t.drawString(displayText, (SCREEN_W / 2) + 2, 18);
  t.setTextColor(isArrived ? TFT_GOLD : TFT_WHITE);
  t.drawString(displayText, SCREEN_W / 2, 16);

  t.pushSprite(0, WHERE_Y, TFT_BLACK);
  t.deleteSprite();
}
void drawCountdownLeft(int days, int hours, int mins, int secs, bool showColon, bool isArrived)
{
  countdownBgSprite.pushToSprite(&countdownFgSprite, 0, 0);
  uint16_t digitColor = TFT_DARKCYAN;
  if (isArrived)
  {
    digitColor = showColon ? TFT_GOLD : TFT_CYAN;
  }
  countdownFgSprite.setTextColor(digitColor);
  countdownFgSprite.setTextSize(1);
  countdownFgSprite.setTextFont(7);
  char sep = (showColon || isArrived) ? ':' : ' ';
  countdownFgSprite.setCursor(15, 0);
  countdownFgSprite.printf("%02d.", days);
  countdownFgSprite.setCursor(95, 0);
  countdownFgSprite.printf("%02d%c", hours, sep);
  countdownFgSprite.setCursor(170, 0);
  countdownFgSprite.printf("%02d%c", mins, sep);
  countdownFgSprite.setCursor(245, 0);
  countdownFgSprite.printf("%02d", secs);
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
bool areWeConfiguring(bool mdnsOk)
{
  unsigned long startAt = millis();
  bool triggered = false;
  int lastSecsLeft = -1;

  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setCursor(0, 6);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.println("To update firmware, go to one of:");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  if (mdnsOk)
    tft.println("  http://" + String(otaHostname) + ".local");
  tft.println("  http://" + WiFi.localIP().toString());

  tft.drawRoundRect(20, 100, 280, 76, 10, TFT_CYAN);
  tft.fillRoundRect(22, 102, 276, 72, 8, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("TAP SCREEN TO CONFIGURE", 160, 126, 2);
  tft.setTextDatum(TL_DATUM);

  tft.setCursor(0, 220);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.print("(C) John Howard 2026");

  uint16_t tx, ty;
  while ((millis() - startAt < 10000) && !triggered)
  {
    int secsLeft = max(1, 10 - (int)((millis() - startAt) / 1000));
    if (secsLeft != lastSecsLeft)
    {
      lastSecsLeft = secsLeft;
      tft.fillRect(40, 142, 240, 22, TFT_NAVY);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_YELLOW, TFT_NAVY);
      tft.drawString("Starting in " + String(secsLeft) + "s...", 160, 152, 2);
      tft.setTextDatum(TL_DATUM);
    }
    if (getTouchPixel(tx, ty))
      triggered = true;
    delay(25);
  }

  tft.fillScreen(TFT_BLACK);
  return triggered;
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
  prefs.end();
  if (lHour.length() > 0)
    strlcpy(gHour, lHour.c_str(), sizeof(gHour));
  if (lMins.length() > 0)
    strlcpy(gMins, lMins.c_str(), sizeof(gMins));
  if (lDay.length() > 0)
    strlcpy(gDay, lDay.c_str(), sizeof(gDay));
  if (lMonth.length() > 0)
    strlcpy(gMonth, lMonth.c_str(), sizeof(gMonth));
  if (lYear.length() > 0)
    strlcpy(gYear, lYear.c_str(), sizeof(gYear));
  if (lWhere.length() > 0)
    strlcpy(gWhere, lWhere.c_str(), sizeof(gWhere));
  Serial.printf("Pref: At %s:%s on %s/%s/%s - %s\n", gHour, gMins, gDay, gMonth, gYear, gWhere);
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

  // Confirm save visually on screen before rebooting
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("SAVED HOLIDAY SETTINGS!", 160, 95, 4);
  String summary = where + " - " + day + "/" + month + "/" + year + " " + hour + ":" + mins;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(summary, 160, 130, 2);
  tft.setTextDatum(TL_DATUM);

  delay(2000);
  ESP.restart();
}

// ============================================================================
// Blinking Eyes Screen Saver (Activates after SCREENSAVER_TIMEROUT_MS of no touch)
// ============================================================================

// Persistent off-screen sprite (~38.9 KB) allocated once to avoid heap fragmentation
TFT_eSprite eyeSpr(&tft);
bool eyeSprReady = false;

// TFT_eSprite eyeSpr(&tft);
TFT_eSprite eyeBgLeft(&tft);
TFT_eSprite eyeBgRight(&tft);
bool eyeAssetsReady = false;

bool ensureEyeSprite()
{
  if (!eyeSprReady)
  {
    eyeSpr.setColorDepth(16);
    if (eyeSpr.createSprite(152, 128) == nullptr)
    {
      Serial.println("Failed to allocate eyeSpr - low heap!");
      return false;
    }
    eyeSprReady = true;
  }
  return true;
}

static inline uint16_t swap16(uint16_t c)
{
  return (uint16_t)((c << 8) | (c >> 8));
}

// Fast bilinear texture lookup for smooth sub-pixel iris rendering
static inline uint16_t sampleIrisBilinear(float u, float v)
{
  float fx = constrain((u * 0.5f + 0.5f) * (IRIS_TEX_SIZE - 1), 0.0f, (float)(IRIS_TEX_SIZE - 2));
  float fy = constrain((v * 0.5f + 0.5f) * (IRIS_TEX_SIZE - 1), 0.0f, (float)(IRIS_TEX_SIZE - 2));
  int ix = (int)fx, iy = (int)fy;
  uint8_t wx = (uint8_t)((fx - ix) * 255.0f);
  uint8_t wy = (uint8_t)((fy - iy) * 255.0f);

  int idx = iy * IRIS_TEX_SIZE + ix;
  uint16_t top = blend565(irisTex[idx], irisTex[idx + 1], wx);
  uint16_t bot = blend565(irisTex[idx + IRIS_TEX_SIZE], irisTex[idx + IRIS_TEX_SIZE + 1], wx);
  return blend565(top, bot, wy);
}

// Pre-baked 48x48 multi-zone anatomical iris texture (~4.6 KB RAM)

void buildIrisTexture()
{
  if (irisTexReady)
    return;
  const float half = (IRIS_TEX_SIZE - 1) * 0.5f;

  for (int y = 0; y < IRIS_TEX_SIZE; y++)
  {
    float dy = ((float)y - half) / half;
    for (int x = 0; x < IRIS_TEX_SIZE; x++)
    {
      float dx = ((float)x - half) / half;
      float r = sqrtf(dx * dx + dy * dy);
      if (r > 1.0f)
      {
        irisTex[y * IRIS_TEX_SIZE + x] = 0x0000;
        continue;
      }

      float angle = atan2f(dy, dx);
      float twist = sinf(r * 6.5f) * 0.12f;
      float a = angle + twist;

      // Fine radial stroma trabeculae + deep anatomical Fuchs' crypts
      float f1 = sinf(a * 36.0f + sinf(a * 7.0f) * 1.5f);
      float f2 = cosf(a * 61.0f - r * 11.0f);
      float f3 = sinf(a * 19.0f + cosf(r * 14.0f) * 1.3f);
      float crypt = sinf(a * 13.0f + 0.9f) * sinf(r * 15.0f - 0.5f);

      float fibre = constrain(0.50f + 0.25f * f1 + 0.15f * f2 + 0.10f * f3 - max(0.0f, crypt) * 0.30f, 0.0f, 1.0f);

      // Rich deep ocean-sapphire to luminous cerulean-teal stroma
      uint16_t col = blend565(0x014A, 0x573F, (uint8_t)(fibre * 255.0f));

      // Jagged anatomical collarette ridge (r ~ 0.44 with star-like peaks)
      float collarRadius = 0.44f + 0.045f * sinf(a * 14.0f) + 0.025f * cosf(a * 23.0f);
      float collarDist = fabsf(r - collarRadius);
      if (collarDist < 0.14f)
      {
        float collarAmt = (1.0f - collarDist / 0.14f) * (0.60f + 0.40f * f1);
        col = blend565(col, 0xCC84, (uint8_t)constrain((int)(collarAmt * 210.0f), 0, 255));
      }

      // Subtle dark stromal furrow rings near outer iris
      if (r > 0.65f && r < 0.82f)
      {
        float furrow = max(0.0f, sinf(r * 38.0f));
        col = blend565(col, 0x00C6, (uint8_t)(furrow * 90.0f));
      }

      // Soft dark outer limbal ring
      if (r > 0.79f)
      {
        float limbal = (r - 0.79f) / 0.21f;
        col = blend565(col, 0x0042, (uint8_t)(limbal * 245.0f));
      }

      irisTex[y * IRIS_TEX_SIZE + x] = col;
    }
  }
  irisTexReady = true;
}

void renderAndPushOneEye(int screenX, int screenY, float lookX, float lookY, float openness, float pupilScale, bool isLeftEye)
{
  if (!ensureEyeSprite())
    return;
  buildIrisTexture();

  const int sprW = 152, sprH = 128;
  const int cx = 76, cy = 64;
  const int halfW = 60, maxUpperH = 29, maxLowerH = 22;

  uint16_t *ptr = (uint16_t *)eyeSpr.getPointer();
  if (!ptr)
    return;

  float lidTrack = constrain(1.0f - (lookY * 0.028f), 0.78f, 1.16f);
  int upperH = (int)(maxUpperH * openness * lidTrack);
  int lowerH = (int)(maxLowerH * openness * (1.0f + (lookY * 0.012f)));
  int lidMeetY = cy + (int)((1.0f - openness) * 7.5f) + (int)(lookY * 0.25f);

  // 1. Soft photographic skin vignette around the eye (eliminates the "floating in black void" look)
  for (int y = 0; y < sprH; y++)
  {
    float ny = (float)(y - cy) * (1.0f / 64.0f);
    float nySq = ny * ny;
    // Fade smoothly to black at top/bottom sprite borders so there is never a hard box edge
    float vMask = max(0.0f, 1.0f - nySq * nySq);
    int rowOffset = y * sprW;

    for (int x = 0; x < sprW; x++)
    {
      float nx = (float)(x - cx) * (1.0f / 76.0f);
      float nxSq = nx * nx;
      float hMask = max(0.0f, 1.0f - nxSq * nxSq);

      // Orbital socket shadow + soft warm light on upper lid & lower cheekbone
      float distFromLid = fabsf((float)(y - lidMeetY)) * (1.0f / 42.0f);
      float browHighlight = max(0.0f, 1.0f - fabsf(ny + 0.58f) * 2.2f);
      float cheekHighlight = max(0.0f, 1.0f - fabsf(ny - 0.52f) * 2.4f);
      float skinLight = constrain((0.22f + browHighlight * 0.42f + cheekHighlight * 0.36f + distFromLid * 0.15f) * vMask * hMask, 0.0f, 1.0f);

      // Subtle micro-grain so skin looks organic rather than plastic
      int grain = ((x * 19 + y * 37) & 7) - 3;
      uint8_t alpha = (uint8_t)constrain((int)(skinLight * 210.0f) + grain, 0, 255);

      // Warm low-key skin tone (0x5227) blended seamlessly into black vignette
      ptr[rowOffset + x] = swap16(blend565(0x0000, 0x6289, alpha));
    }
  }

  // 2. Organic upper eyelid crease & soft lower-lid bag shadow
  for (int dx = -halfW + 3; dx <= halfW - 3; dx++)
  {
    float nx = (float)dx * (1.0f / 60.0f);
    float arch = 1.0f - nx * nx;
    int creaseY = lidMeetY - (int)((upperH + 9) * arch);
    for (int dy = -2; dy <= 2; dy++)
    {
      int py = creaseY + dy;
      if (py >= 0 && py < sprH)
      {
        int idx = py * sprW + (cx + dx);
        uint8_t shadowA = (uint8_t)((165 - abs(dy) * 55) * arch);
        ptr[idx] = swap16(blend565(swap16(ptr[idx]), 0x18A2, shadowA));
      }
    }
    int lowerBagY = lidMeetY + (int)((lowerH + 9) * arch);
    if (lowerBagY >= 0 && lowerBagY < sprH)
    {
      int idx = lowerBagY * sprW + (cx + dx);
      ptr[idx] = swap16(blend565(swap16(ptr[idx]), 0x20E3, (uint8_t)(arch * 110.0f)));
    }
  }

  // 3. Spherical eyeball, recessed pupil refraction & lit fleshy lower lid rim
  if (openness > 0.05f)
  {
    float irisX = (float)cx + lookX;
    float irisY = (float)cy + lookY;
    const float irisR = 23.5f, invIrisR = 1.0f / irisR;
    const float pupilNorm = 0.37f * pupilScale;
    float gazeSquashX = 1.0f + fabsf(lookX) * 0.010f;
    float pupilShiftU = -lookX * 0.0055f;
    float pupilShiftV = -lookY * 0.0055f;

    for (int dx = -halfW; dx <= halfW; dx++)
    {
      float nx = (float)dx * (1.0f / 60.0f);
      // Organic asymmetric human eyelid curve (peaks slightly toward inner-centre)
      float peakShift = isLeftEye ? (nx + 0.14f) : (nx - 0.14f);
      float upperArch = max(0.0f, 1.0f - peakShift * peakShift * 1.15f);
      float lowerArch = max(0.0f, 1.0f - nx * nx);

      float fTopY = (float)lidMeetY - (float)upperH * upperArch;
      float fBotY = (float)lidMeetY + (float)lowerH * lowerArch;

      int yStart = max(0, (int)floorf(fTopY) - 1);
      int yEnd = min(sprH - 1, (int)ceilf(fBotY) + 3);

      for (int y = yStart; y <= yEnd; y++)
      {
        int idx = y * sprW + (cx + dx);
        uint16_t bgCol = swap16(ptr[idx]);

        // Fleshy lower eyelid rim (tarsal margin) catching overhead light
        float distBelowBot = (float)y - fBotY;
        if (distBelowBot > 0.0f && distBelowBot <= 3.0f && fabsf(nx) < 0.92f)
        {
          float rimFade = (1.0f - distBelowBot * 0.33f) * (1.0f - nx * nx * 0.4f);
          uint16_t rimCol = (distBelowBot < 1.1f) ? 0xCE59 : 0x834B; // Wet highlight edge + warm skin rim
          ptr[idx] = swap16(blend565(bgCol, rimCol, (uint8_t)(rimFade * 215.0f * openness)));
          continue;
        }

        float lidCov = min(constrain((float)y - fTopY + 0.5f, 0.0f, 1.0f),
                           constrain(fBotY - (float)y + 0.5f, 0.0f, 1.0f));
        if (lidCov <= 0.01f)
          continue;

        uint16_t pixelCol;
        float innerDist = isLeftEye ? (float)(halfW - dx) : (float)(dx + halfW);

        // Soft shaded inner corner (plica & caruncle)
        if (innerDist < 9.5f && abs(y - (int)((fTopY + fBotY) * 0.5f)) <= 3)
        {
          pixelCol = (innerDist > 6.5f) ? 0xB596 : 0x8C51;
        }
        else
        {
          float u = ((float)(cx + dx) - irisX) * gazeSquashX * invIrisR;
          float v = ((float)y - irisY) * invIrisR;
          float rSq = u * u + v * v;

          // Deep upper-lid shadow gradient across the spherical sclera
          float distFromTop = (float)y - fTopY;
          float lidShadow = constrain(distFromTop * 0.095f, 0.28f, 1.0f);
          float sphereShade = constrain((1.0f - nx * nx * 0.35f) * lidShadow, 0.26f, 1.0f);
          uint16_t scleraCol = blend565(0x4228, 0xF7BE, (uint8_t)(sphereShade * 255.0f));

          if (rSq <= 1.08f)
          {
            float r = sqrtf(rSq);
            float pu = u - pupilShiftU;
            float pv = v - pupilShiftV;
            float pr = sqrtf(pu * pu + pv * pv);

            if (pr <= pupilNorm - 0.03f)
            {
              pixelCol = 0x0000;
            }
            else if (pr <= pupilNorm + 0.04f)
            {
              uint8_t pa = (uint8_t)(((pr - (pupilNorm - 0.03f)) / 0.07f) * 255.0f);
              pixelCol = blend565(0x0000, 0x9B42, pa);
            }
            else
            {
              uint16_t baseIris = sampleIrisBilinear(u, v);

              // Soft directional light caustic on lower-right iris bowl
              float caustic = constrain((u + v) * 0.25f, 0.0f, 0.35f);
              if (caustic > 0.01f)
              {
                baseIris = blend565(baseIris, 0x9F9F, (uint8_t)(caustic * 255.0f));
              }

              // Soft eyelash & upper-lid shadow cast across top third of iris
              baseIris = blend565(0x0021, baseIris, (uint8_t)(constrain(distFromTop * 0.105f, 0.18f, 1.0f) * 255.0f));

              pixelCol = (r > 0.92f)
                             ? blend565(scleraCol, baseIris, (uint8_t)(constrain((1.05f - r) * 7.69f, 0.0f, 1.0f) * 255.0f))
                             : baseIris;
            }
          }
          else
          {
            pixelCol = scleraCol;
          }
        }

        // Blend softly into dark lashline shadow along eyelid border
        ptr[idx] = swap16((lidCov < 0.99f) ? blend565(0x18A2, pixelCol, (uint8_t)(lidCov * 255.0f)) : pixelCol);
      }
    }

    // 4. Soft-edged studio window catchlight + subtle secondary glint
    if (openness > 0.28f)
    {
      int hx = (int)(irisX - 7.0f - lookX * 0.16f);
      int hy = (int)(irisY - 7.0f - lookY * 0.16f);
      eyeSpr.fillCircle(hx, hy, 3, TFT_WHITE);
      eyeSpr.fillCircle(hx + 3, hy - 2, 2, 0xEF7D);
      eyeSpr.fillCircle(hx - 2, hy + 3, 2, 0xD71C);
      // Eyelash shadow line cutting organically through top of catchlight
      eyeSpr.drawLine(hx - 1, hy - 3, hx + 1, hy + 1, 0x2104);
      eyeSpr.fillCircle((int)(irisX + 7.0f - lookX * 0.10f), (int)(irisY + 6.0f - lookY * 0.10f), 1, 0xB5F6);
    }
  }

  // 5. Organic, slightly irregular upper & lower eyelashes (avoids uniform "comb" look)
  for (int dx = -halfW; dx <= halfW; dx++)
  {
    float nx = (float)dx * (1.0f / 60.0f);
    float peakShift = isLeftEye ? (nx + 0.14f) : (nx - 0.14f);
    float upperArch = max(0.0f, 1.0f - peakShift * peakShift * 1.15f);
    float lowerArch = max(0.0f, 1.0f - nx * nx);

    int topY = (int)roundf((float)lidMeetY - (float)upperH * upperArch);
    int botY = (int)roundf((float)lidMeetY + (float)lowerH * lowerArch);

    if (topY >= 1 && topY < sprH)
    {
      int idx1 = topY * sprW + (cx + dx);
      int idx2 = (topY - 1) * sprW + (cx + dx);
      ptr[idx1] = swap16(blend565(swap16(ptr[idx1]), 0x1082, 220));
      ptr[idx2] = swap16(blend565(swap16(ptr[idx2]), 0x2104, 150));
    }

    // Irregular organic lash clusters (varies spacing, length, and curl slightly per hair)
    int hash = abs((dx * 17 + (isLeftEye ? 11 : 29)) % 13);
    if (openness > 0.18f && (hash == 0 || hash == 4 || hash == 9) && abs(dx) < halfW - 3)
    {
      float side = isLeftEye ? -nx : nx;
      int lenJitter = (hash == 4) ? 2 : ((hash == 9) ? -1 : 0);
      int lashLen = max(4, (int)(7.0f + side * 5.0f) + lenJitter);
      float curlX = side * 6.5f + ((hash == 0) ? 1.2f : -0.8f);

      for (int s = 1; s <= lashLen; s++)
      {
        float t = (float)s / (float)lashLen;
        float fx = (float)(cx + dx) + curlX * t * t;
        int ix = (int)floorf(fx);
        float frac = fx - (float)ix;
        int ly = topY - 1 - s;
        if (ly < 0 || ix < 0 || ix + 1 >= sprW)
          break;

        uint8_t alpha = (uint8_t)((1.0f - t * 0.75f) * 235.0f);
        int lidx = ly * sprW + ix;
        ptr[lidx] = swap16(blend565(swap16(ptr[lidx]), 0x1082, (uint8_t)(alpha * (1.0f - frac))));
        ptr[lidx + 1] = swap16(blend565(swap16(ptr[lidx + 1]), 0x1082, (uint8_t)(alpha * frac)));
      }
    }

    // Sparse, natural lower lashes
    int lowHash = abs((dx * 23 + (isLeftEye ? 7 : 19)) % 17);
    if (openness > 0.45f && (lowHash == 2 || lowHash == 11) && abs(dx) < halfW - 9)
    {
      float side = isLeftEye ? -nx : nx;
      int curlX = (int)(side * 2.5f);
      for (int s = 1; s <= 3; s++)
      {
        int ly = botY + 2 + s;
        int lx = cx + dx + (curlX * s) / 3;
        if (ly < sprH && lx >= 0 && lx < sprW)
        {
          int lidx = ly * sprW + lx;
          ptr[lidx] = swap16(blend565(swap16(ptr[lidx]), 0x2945, (uint8_t)(165 - s * 40)));
        }
      }
    }
  }
  eyeSpr.pushSprite(screenX, screenY);
}
void drawEyePair(float openness, float pupilOffsetX, float pupilOffsetY, float pupilScale, int driftX = 0, int driftY = 0)
{
  int leftX = constrain(8 + driftX, 0, 168);
  int rightX = constrain(160 + driftX, 0, 168);
  int posY = constrain(56 + driftY, 0, 112);

  renderAndPushOneEye(leftX, posY, pupilOffsetX, pupilOffsetY, openness, pupilScale, true);
  renderAndPushOneEye(rightX, posY, pupilOffsetX, pupilOffsetY, openness, pupilScale, false);
}

void updateScreenSaverEyes(bool forceInit = false)
{
  static unsigned long nextActionMs = 0;
  static unsigned long lastMicroMs = 0;
  static int blinkStep = 0;
  static bool doubleBlink = false;
  static float curX = 0.0f, curY = 0.0f;
  static float tgtX = 0.0f, tgtY = 0.0f;
  static float curPupil = 1.0f, tgtPupil = 1.0f;
  static float squintFactor = 1.0f, tgtSquint = 1.0f;
  static int lastDriftX = 0, lastDriftY = 0;

  const float blinkCurve[] = {1.0f, 0.45f, 0.0f, 0.28f, 0.65f, 0.90f};
  unsigned long now = millis();

  // Compute slow Lissajous burn-in drift across the 320x240 display
  float tSec = (float)now * 0.001f;
  int driftX = (int)roundf(sinf(tSec * 0.17f) * 8.0f);  // ±8px horizontal drift
  int driftY = (int)roundf(cosf(tSec * 0.11f) * 48.0f); // ±48px vertical drift

  if (forceInit)
  {
    tft.fillScreen(TFT_BLACK);
    blinkStep = 0;
    doubleBlink = false;
    curX = curY = tgtX = tgtY = 0.0f;
    curPupil = tgtPupil = 1.0f;
    squintFactor = tgtSquint = 1.0f;
    lastDriftX = driftX;
    lastDriftY = driftY;
    drawEyePair(1.0f, 0.0f, 0.0f, 1.0f, driftX, driftY);
    nextActionMs = now + random(1600, 3200);
    lastMicroMs = now;
    return;
  }

  // Clear thin 2px edge strips whenever the sprite shifts position to prevent trails
  if (driftX != lastDriftX || driftY != lastDriftY)
  {
    int oldLeftX = constrain(8 + lastDriftX, 0, 168);
    int oldRightX = constrain(160 + lastDriftX, 0, 168);
    int oldPosY = constrain(56 + lastDriftY, 0, 112);
    int newLeftX = constrain(8 + driftX, 0, 168);
    int newRightX = constrain(160 + driftX, 0, 168);
    int newPosY = constrain(56 + driftY, 0, 112);

    if (newPosY > oldPosY)
    {
      tft.fillRect(0, oldPosY, 320, newPosY - oldPosY, TFT_BLACK);
    }
    else if (newPosY < oldPosY)
    {
      tft.fillRect(0, newPosY + 128, 320, oldPosY - newPosY, TFT_BLACK);
    }
    if (newLeftX > oldLeftX)
    {
      tft.fillRect(oldLeftX, oldPosY, newLeftX - oldLeftX, 128, TFT_BLACK);
      tft.fillRect(oldRightX, oldPosY, newRightX - oldRightX, 128, TFT_BLACK);
    }
    else if (newLeftX < oldLeftX)
    {
      tft.fillRect(newLeftX + 152, oldPosY, oldLeftX - newLeftX, 128, TFT_BLACK);
      tft.fillRect(newRightX + 152, oldPosY, oldRightX - newRightX, 128, TFT_BLACK);
    }

    lastDriftX = driftX;
    lastDriftY = driftY;
  }

  // Smooth saccadic gaze glide + subtle squint & pupil transitions
  bool needsRedraw = false;
  if (fabsf(tgtX - curX) > 0.20f || fabsf(tgtY - curY) > 0.20f)
  {
    curX += (tgtX - curX) * 0.48f;
    curY += (tgtY - curY) * 0.48f;
    if (fabsf(tgtX - curX) <= 0.20f)
      curX = tgtX;
    if (fabsf(tgtY - curY) <= 0.20f)
      curY = tgtY;
    needsRedraw = true;
  }
  if (fabsf(tgtPupil - curPupil) > 0.008f || fabsf(tgtSquint - squintFactor) > 0.01f)
  {
    curPupil += (tgtPupil - curPupil) * 0.22f;
    squintFactor += (tgtSquint - squintFactor) * 0.20f;
    needsRedraw = true;
  }

  // Subtle living ocular micro-drift & pupil breathing every ~90ms
  float microX = 0.0f, microY = 0.0f, breathPupil = 0.0f;
  if (blinkStep == 0 && (now - lastMicroMs >= 90))
  {
    lastMicroMs = now;
    microX = sinf(tSec * 2.1f) * 0.45f;
    microY = cosf(tSec * 1.7f) * 0.30f;
    breathPupil = sinf(tSec * 1.4f) * 0.035f;
    needsRedraw = true;
  }

  if (needsRedraw && blinkStep == 0)
  {
    drawEyePair(squintFactor, curX + microX, curY + microY, curPupil + breathPupil, driftX, driftY);
  }

  if (now >= nextActionMs)
  {
    if (blinkStep == 0)
    {
      int roll = random(10);
      if (roll < 5)
      {
        blinkStep = 1;
        doubleBlink = (random(4) == 0);
        drawEyePair(blinkCurve[blinkStep] * squintFactor, curX, curY, curPupil, driftX, driftY);
        nextActionMs = now + 22;
      }
      else if (roll < 8)
      {
        tgtX = (float)random(-17, 18);
        tgtY = (float)random(-6, 6);
        tgtPupil = 0.86f + (float)random(0, 28) * 0.01f;
        tgtSquint = (random(4) == 0) ? 0.84f : 1.0f;
        nextActionMs = now + random(1400, 3200);
      }
      else
      {
        tgtX = 0.0f;
        tgtY = 0.0f;
        tgtPupil = 1.0f;
        tgtSquint = 1.0f;
        nextActionMs = now + random(2000, 4200);
      }
    }
    else
    {
      blinkStep++;
      if (blinkStep >= 6)
      {
        blinkStep = 0;
        drawEyePair(squintFactor, curX, curY, curPupil, driftX, driftY);
        if (doubleBlink)
        {
          doubleBlink = false;
          nextActionMs = now + 120;
        }
        else
        {
          nextActionMs = now + random(2200, 5000);
        }
      }
      else
      {
        drawEyePair(blinkCurve[blinkStep] * squintFactor, curX, curY, curPupil, driftX, driftY);
        nextActionMs = now + (blinkStep == 2 ? 55 : 24);
      }
    }
  }
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
  tft.fillRect(0, BOT_TEXT_Y, SCREEN_W, 16, TFT_BLACK);
  drawWifiSignalBars(2, BOT_TEXT_Y + 1);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(WiFi.SSID(), 22, BOT_TEXT_Y, 2);

  tft.setTextDatum(TR_DATUM);
  tft.drawString(formatPrettyDateTime(rawhol), SCREEN_W - 1, BOT_TEXT_Y, 2);
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

String wmoCodeToText(int code)
{
  if (code == 0)
    return "Clear";
  if (code <= 3)
    return "Partly Cloudy";
  if (code == 45 || code == 48)
    return "Foggy";
  if (code >= 51 && code <= 67)
    return "Rain";
  if (code >= 71 && code <= 77)
    return "Snow";
  if (code >= 80 && code <= 82)
    return "Showers";
  if (code >= 95)
    return "Thunder";
  return "Cloudy";
}

bool extractJsonNumber(const String &json, const String &key, float &outVal)
{
  int kPos = json.indexOf("\"" + key + "\":");
  if (kPos < 0)
    return false;
  int start = kPos + key.length() + 3;
  while (start < (int)json.length() && (json[start] == ' ' || json[start] == '['))
    start++;
  int end = start;
  while (end < (int)json.length() && (isdigit(json[end]) || json[end] == '.' || json[end] == '-'))
    end++;
  if (end <= start)
    return false;
  outVal = json.substring(start, end).toFloat();
  return true;
}

void fetchDestinationWeather()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  WiFiClientSecure client;
  client.setInsecure(); // Skip root CA certificate bundle to save ~40KB RAM

  HTTPClient http;
  http.setTimeout(6000);

  // Step 1: Resolve destination name (gWhere) to lat/lon once
  if (!hasCachedCoords)
  {
    String query = String(gWhere);
    query.trim();
    query.replace(" ", "%20");

    String geoUrl = "https://open-meteo.com" + query + "&count=1&format=json";
    if (http.begin(client, geoUrl))
    {
      int code = http.GET();
      if (code == HTTP_CODE_OK)
      {
        String geoPayload = http.getString();
        if (extractJsonNumber(geoPayload, "latitude", cachedLat) &&
            extractJsonNumber(geoPayload, "longitude", cachedLon))
        {
          hasCachedCoords = true;
          Serial.printf("Geocoded '%s' -> %.4f, %.4f\n", gWhere, cachedLat, cachedLon);
        }
      }
      http.end();
    }
  }

  if (!hasCachedCoords)
  {
    Serial.printf("Could not geocode destination: %s\n", gWhere);
    return;
  }

  // Step 2: Fetch live current weather at those coordinates
  String wxUrl = "https://open-meteo.com" + String(cachedLat, 4) +
                 "&longitude=" + String(cachedLon, 4) + "&current_weather=true";
  if (http.begin(client, wxUrl))
  {
    int code = http.GET();
    if (code == HTTP_CODE_OK)
    {
      String wxPayload = http.getString();
      float tempC = 0.0f, wCode = 0.0f;
      if (extractJsonNumber(wxPayload, "temperature", tempC) &&
          extractJsonNumber(wxPayload, "weathercode", wCode))
      {
        weatherText = String((int)roundf(tempC)) + "C " + wmoCodeToText((int)wCode);
        Serial.println("Updated destination weather: " + weatherText);
      }
    }
    http.end();
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
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);

  touch.begin();
  touch.setRotation(1);

  // 1. Load saved preferences first
  readPreferences();

  WiFiManagerParameter custom_hol_hour("hour", "Hour (0-23)", gHour, 2);
  WiFiManagerParameter custom_hol_mins("mins", "Mins (0-59)", gMins, 2);
  WiFiManagerParameter custom_hol_day("day", "Day (1-31)", gDay, 2);
  WiFiManagerParameter custom_hol_month("month", "Month (1-12)", gMonth, 2);
  WiFiManagerParameter custom_hol_year("year", "Year (YYYY)", gYear, 4);
  WiFiManagerParameter custom_hol_where("where", "Destination", gWhere, 20);

  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setSaveParamsCallback(saveConfigCallback);
  wifiManager.setBreakAfterConfig(true);   // Exit portal immediately after Save is clicked
  wifiManager.setConfigPortalTimeout(600); // 10 minutes (600 seconds)

  wifiManager.addParameter(&custom_hol_hour);
  wifiManager.addParameter(&custom_hol_mins);
  wifiManager.addParameter(&custom_hol_day);
  wifiManager.addParameter(&custom_hol_month);
  wifiManager.addParameter(&custom_hol_year);
  wifiManager.addParameter(&custom_hol_where);

  // 2. Connect to saved Wi-Fi first so we have an IP and OTA hostname
  if (!wifiManager.autoConnect())
  {
    showScreenError("WI-FI ERROR", "Failed to connect to Wi-Fi.\nRestarting in 5 seconds...", true, 5000);
    ESP.restart();
  }

  // 3. Start OTA, then show combined OTA + "Tap Screen to Configure" screen for 5s
  bool mdnsOk = setupOTA();
  bool portalRan = false;
  bool portalTimedOut = false;

  if (areWeConfiguring(mdnsOk))
  {
    portalRan = true;
    otaServer.stop();

    String apName = String(otaHostname);

    tft.fillScreen(TFT_BLACK);

    // Header bar
    tft.fillRect(0, 0, 320, 26, TFT_NAVY);
    tft.drawFastHLine(0, 26, 320, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("SETUP MODE", 160, 13, 2);

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

    // Run config portal non-blocking so we can update the 10-minute MM:SS countdown
    wifiManager.setConfigPortalBlocking(false);
    wifiManager.startConfigPortal(apName.c_str());

    const unsigned long portalDurationMs = 600000UL; // 10 minutes
    unsigned long portalStartMs = millis();
    int lastSecShown = -1;

    while (wifiManager.getConfigPortalActive())
    {
      wifiManager.process();

      unsigned long elapsed = millis() - portalStartMs;
      int secsLeft = (elapsed < portalDurationMs) ? (int)((portalDurationMs - elapsed) / 1000) : 0;
      if (secsLeft == 0)
        portalTimedOut = true;

      if (secsLeft != lastSecShown)
      {
        lastSecShown = secsLeft;
        int mins = secsLeft / 60;
        int secs = secsLeft % 60;

        String timerText = "Time remaining: " + String(mins < 10 ? "0" : "") + String(mins) + ":" + String(secs < 10 ? "0" : "") + String(secs);

        tft.fillRect(20, 214, 280, 22, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(secsLeft <= 30 ? TFT_RED : TFT_SILVER, TFT_BLACK);
        tft.drawString(timerText, 160, 224, 2);
        tft.setTextDatum(TL_DATUM);
      }

      delay(10);
    }

    wifiManager.setConfigPortalBlocking(true);
    otaServer.begin();
  }

  tft.fillScreen(TFT_BLACK);

  // 4. Save updated holiday details if changed OR if portal was submitted before timing out
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

  // 5. Sync NTP time
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

  // 6. Calculate target holiday timestamp
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

  // 7. Render static UI elements & initialize persistent sprites
  initSprites();
  displayLines();
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  drawWhere(false);

  lastInteractionMillis = millis();

  Serial.println("setup() complete");
}
void loop()
{
  static time_t lastRenderedSec = 0;
  static bool blinkState = false;
  static unsigned long lastReplayMillis = 0;

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
  }

  if (inScreenSaver)
  {
    if (freshTap || isArrived)
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

  if (!isArrived && (millis() - lastInteractionMillis >= SCREENSAVER_TIMEOUT_MS))
  {
    inScreenSaver = true;
    updateScreenSaverEyes(true);
    yield();
    return;
  }

  if (isArrived && freshTap && (millis() - lastReplayMillis > 1500))
  {
    lastReplayMillis = millis();
    playHolidayFanfare();
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
        lastReplayMillis = millis();
      }
    }

    int totalSecs = (int)diffSecs;
    int diffDays = totalSecs / 86400;
    int diffHours = (totalSecs % 86400) / 3600;
    int diffMins = (totalSecs % 3600) / 60;
    int remSecs = totalSecs % 60;

    drawCountdownLeft(diffDays, diffHours, diffMins, remSecs, blinkState, isArrived);
    displayCurrentTimeSprite(rawlocal);

    // Refresh 4-bar Wi-Fi signal icon every 10 seconds
    if (rawlocal % 10 == 0)
    {
      drawWifiSignalBars(2, BOT_TEXT_Y + 1);
    }

    // Refresh destination weather every 30 minutes
    if (millis() - lastWeatherFetchMs >= WEATHER_INTERVAL_MS)
    {
      lastWeatherFetchMs = millis();
      fetchDestinationWeather();
      drawWeatherBadge();
    }
  }

  delay(25);
}
