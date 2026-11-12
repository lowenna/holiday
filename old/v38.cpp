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
#include "audio.h"
#include "maldives.h" // Hexdump of 320x240 JPG background

#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>

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

// Fast 16-bit RGB565 linear colour blend (alpha: 0 = bg, 255 = fg)
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t alpha)
{
  if (alpha == 0)
    return bg;
  if (alpha == 255)
    return fg;
  uint32_t bgRB = bg & 0xF81F;
  uint32_t bgG = bg & 0x07E0;
  uint32_t fgRB = fg & 0xF81F;
  uint32_t fgG = fg & 0x07E0;
  uint32_t rb = (bgRB + (((fgRB - bgRB) * alpha) >> 8)) & 0xF81F;
  uint32_t g = (bgG + (((fgG - bgG) * alpha) >> 8)) & 0x07E0;
  return (uint16_t)(rb | g);
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

void displaySSIDAndWhen()
{
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(WiFi.SSID(), 0, BOT_TEXT_Y, 2);

  tft.setTextDatum(TR_DATUM);
  tft.drawString(formatPrettyDateTime(rawhol), SCREEN_W - 1, BOT_TEXT_Y, 2);
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

  String displayText = isArrived ? ("TIME FOR " + String(gWhere) + "!") : String(gWhere);

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

  // Top section: OTA Update URLs
  tft.setCursor(0, 6);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.println("OTA Wireless Update Ready:");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  if (mdnsOk)
  {
    tft.printf("  http://%s.local\n", otaHostname);
  }
  tft.printf("  http://%s\n", WiFi.localIP().toString().c_str());

  // Bottom section: Properly sized button using Font 2
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
  while ((millis() - startAt < 5000) && !triggered)
  {
    int secsLeft = 5 - (int)((millis() - startAt) / 1000);
    if (secsLeft < 1)
      secsLeft = 1;

    // Update the countdown text inside the button once per second
    if (secsLeft != lastSecsLeft)
    {
      lastSecsLeft = secsLeft;
      char countBuf[28];
      snprintf(countBuf, sizeof(countBuf), "Starting in %ds...", secsLeft);
      tft.fillRect(40, 142, 240, 22, TFT_NAVY);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_YELLOW, TFT_NAVY);
      tft.drawString(countBuf, 160, 152, 2);
      tft.setTextDatum(TL_DATUM);
    }

    if (getTouchPixel(tx, ty))
    {
      triggered = true;
    }
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
// Blinking Eyes Screen Saver (Activates after 10s of no touch)
// ============================================================================
constexpr unsigned long SCREENSAVER_TIMEOUT_MS = 10000UL; // 10 seconds
bool inScreenSaver = false;
unsigned long lastInteractionMillis = 0;

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
// Pre-baked 48x48 high-detail iris texture (~4.6 KB RAM) computed once at startup
constexpr int IRIS_TEX_SIZE = 48;
uint16_t irisTex[IRIS_TEX_SIZE * IRIS_TEX_SIZE];
bool irisTexReady = false;

void buildIrisTexture()
{
  if (irisTexReady)
    return;
  const float half = (IRIS_TEX_SIZE - 1) * 0.5f;

  for (int y = 0; y < IRIS_TEX_SIZE; y++)
  {
    float dy = ((float)y - half) / half; // -1.0 .. +1.0
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
      // Multi-harmonic wavy stroma fibres + organic Fuchs' crypts
      float twist = sinf(r * 7.0f) * 0.14f;
      float a = angle + twist;
      float f1 = sinf(a * 32.0f + sinf(a * 7.0f) * 1.6f);
      float f2 = cosf(a * 53.0f - r * 9.0f);
      float f3 = sinf(a * 17.0f + cosf(r * 12.0f) * 1.2f);
      float crypt = sinf(a * 11.0f + 1.3f) * sinf(r * 14.0f);

      float fibre = constrain(0.50f + 0.24f * f1 + 0.15f * f2 + 0.11f * f3 - max(0.0f, crypt) * 0.25f, 0.0f, 1.0f);

      // Deep ocean sapphire-turquoise base palette
      uint16_t col = blend565(0x018C, 0x4EFF, (uint8_t)(fibre * 255.0f));

      // Warm golden-amber inner collarette ring around the pupil (r = 0.36 .. 0.62)
      float collarDist = fabsf(r - 0.46f);
      if (collarDist < 0.16f)
      {
        float collarAmt = (1.0f - collarDist / 0.16f) * (0.55f + 0.45f * f1);
        col = blend565(col, 0xC443, (uint8_t)constrain((int)(collarAmt * 195.0f), 0, 255));
      }

      // Dark outer limbal ring (r = 0.80 .. 1.0)
      if (r > 0.80f)
      {
        float limbal = (r - 0.80f) / 0.20f;
        col = blend565(col, 0x0042, (uint8_t)(limbal * 240.0f));
      }

      irisTex[y * IRIS_TEX_SIZE + x] = col;
    }
  }
  irisTexReady = true;
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

void renderAndPushOneEye(int screenX, int screenY, float lookX, float lookY, float openness, float pupilScale, bool isLeftEye)
{
  if (!ensureEyeSprite())
    return;
  buildIrisTexture();

  const int sprW = 152, sprH = 128;
  const int cx = 76, cy = 72;
  const int halfW = 62, maxUpperH = 31, maxLowerH = 24;

  eyeSpr.fillSprite(TFT_BLACK);
  uint16_t *ptr = (uint16_t *)eyeSpr.getPointer();
  if (!ptr)
    return;

  float lidTrack = constrain(1.0f - (lookY * 0.028f), 0.78f, 1.16f);
  int upperH = (int)(maxUpperH * openness * lidTrack);
  int lowerH = (int)(maxLowerH * openness * (1.0f + (lookY * 0.012f)));
  int lidMeetY = cy + (int)((1.0f - openness) * 7.5f) + (int)(lookY * 0.25f);

  // 1. Soft upper eyelid fold + faint lower infraorbital crease
  for (int dx = -halfW + 2; dx <= halfW - 2; dx++)
  {
    float nx = (float)dx * (1.0f / 62.0f);
    float arch = 1.0f - nx * nx;
    int creaseY = lidMeetY - (int)((upperH + 10) * arch);
    for (int dy = -2; dy <= 2; dy++)
    {
      int py = creaseY + dy;
      if (py >= 0 && py < sprH)
      {
        int idx = py * sprW + (cx + dx);
        ptr[idx] = swap16(blend565(swap16(ptr[idx]), 0x4A07, (uint8_t)(150 - abs(dy) * 50)));
      }
    }
    int lowerFoldY = lidMeetY + (int)((lowerH + 8) * arch);
    if (lowerFoldY >= 0 && lowerFoldY < sprH && fabsf(nx) < 0.78f)
    {
      int idx = lowerFoldY * sprW + (cx + dx);
      ptr[idx] = swap16(blend565(swap16(ptr[idx]), 0x3164, (uint8_t)((1.0f - fabsf(nx)) * 95.0f)));
    }
  }

  // 2. Spherical eyeball, bilinear iris texture, scleral micro-veins & wet waterline
  if (openness > 0.05f)
  {
    float irisX = (float)cx + lookX;
    float irisY = (float)cy + lookY;
    const float irisR = 24.0f, invIrisR = 1.0f / irisR;
    const float pupilNorm = 0.39f * pupilScale;
    float gazeSquashX = 1.0f + fabsf(lookX) * 0.009f;

    for (int dx = -halfW; dx <= halfW; dx++)
    {
      float nx = (float)dx * (1.0f / 62.0f);
      float asym = isLeftEye ? (nx * 0.08f) : (-nx * 0.08f);
      float arch = 1.0f - nx * nx;

      float fTopY = (float)lidMeetY - (float)upperH * arch + asym * 8.0f;
      float fBotY = (float)lidMeetY + (float)lowerH * arch + asym * 5.0f;

      int yStart = max(0, (int)floorf(fTopY) - 1);
      int yEnd = min(sprH - 1, (int)ceilf(fBotY) + 2);

      for (int y = yStart; y <= yEnd; y++)
      {
        int idx = y * sprW + (cx + dx);
        uint16_t bgCol = swap16(ptr[idx]);

        // Glistening rosy-ivory lower waterline
        float distBelowBot = (float)y - fBotY;
        if (distBelowBot > 0.0f && distBelowBot <= 2.3f && fabsf(nx) < 0.89f)
        {
          uint8_t wlAlpha = (uint8_t)((1.0f - distBelowBot * 0.43f) * 205.0f * openness);
          ptr[idx] = swap16(blend565(bgCol, (distBelowBot < 0.85f) ? 0xEE79 : 0x92EA, wlAlpha));
          continue;
        }

        float lidCov = min(constrain((float)y - fTopY + 0.5f, 0.0f, 1.0f),
                           constrain(fBotY - (float)y + 0.5f, 0.0f, 1.0f));
        if (lidCov <= 0.01f)
          continue;

        uint16_t pixelCol;
        float innerDist = isLeftEye ? (float)(halfW - dx) : (float)(dx + halfW);

        // Inner caruncle & semilunar fold (plica semilunaris)
        if (innerDist < 11.0f && abs(y - (int)((fTopY + fBotY) * 0.5f)) <= 4)
        {
          pixelCol = (innerDist > 8.5f) ? 0xD4B2 : 0xB2CA;
        }
        else
        {
          float u = ((float)(cx + dx) - irisX) * gazeSquashX * invIrisR;
          float v = ((float)y - irisY) * invIrisR;
          float rSq = u * u + v * v;

          float distFromTop = (float)y - fTopY;
          float lidShadow = constrain(distFromTop * 0.11f, 0.34f, 1.0f);
          float sphereShade = constrain((1.0f - nx * nx * 0.34f) * lidShadow, 0.30f, 1.0f);
          uint16_t scleraCol = blend565(0x4A69, 0xFFDF, (uint8_t)(sphereShade * 255.0f));

          // Delicate organic scleral micro-capillaries near corners
          if (fabsf(nx) > 0.50f && rSq > 1.15f)
          {
            float vein = sinf((float)dx * 0.38f + sinf((float)y * 0.48f) * 2.1f);
            if (vein > 0.93f)
            {
              uint8_t va = (uint8_t)((fabsf(nx) - 0.50f) * 145.0f);
              scleraCol = blend565(scleraCol, 0xBAEC, va);
            }
          }

          if (rSq <= 1.08f)
          {
            float r = sqrtf(rSq);
            if (r <= pupilNorm - 0.03f)
            {
              pixelCol = 0x0000;
            }
            else if (r <= pupilNorm + 0.03f)
            {
              uint8_t pa = (uint8_t)(((r - (pupilNorm - 0.03f)) / 0.06f) * 255.0f);
              pixelCol = blend565(0x0000, 0xA3A2, pa); // Golden pupillary ruff
            }
            else
            {
              uint16_t baseIris = sampleIrisBilinear(u, v);

              // Subtle lower-right corneal light caustic (3D concave iris bowl effect)
              float caustic = constrain((u + v) * 0.24f, 0.0f, 0.32f);
              if (caustic > 0.01f)
              {
                baseIris = blend565(baseIris, 0x8F7F, (uint8_t)(caustic * 255.0f));
              }

              // Upper-lid cast shadow across top of iris
              baseIris = blend565(0x0041, baseIris, (uint8_t)(constrain(distFromTop * 0.125f, 0.26f, 1.0f) * 255.0f));

              pixelCol = (r > 0.94f)
                             ? blend565(scleraCol, baseIris, (uint8_t)(constrain((1.04f - r) * 10.0f, 0.0f, 1.0f) * 255.0f))
                             : baseIris;
            }
          }
          else
          {
            pixelCol = scleraCol;
          }
        }

        ptr[idx] = swap16((lidCov < 0.99f) ? blend565(0x1082, pixelCol, (uint8_t)(lidCov * 255.0f)) : pixelCol);
      }
    }

    // 3. Curved corneal catchlight (sky window + turquoise sea glint + eyelash shadow break)
    if (openness > 0.28f)
    {
      int hx = (int)(irisX - 7.0f - lookX * 0.15f);
      int hy = (int)(irisY - 7.0f - lookY * 0.15f);
      eyeSpr.fillCircle(hx, hy, 3, TFT_WHITE);
      eyeSpr.fillCircle(hx + 3, hy - 2, 2, TFT_WHITE);
      eyeSpr.fillCircle(hx - 2, hy + 3, 2, 0xD7FF);
      eyeSpr.drawLine(hx - 1, hy - 3, hx + 1, hy + 1, 0x18E3);
      eyeSpr.fillCircle((int)(irisX + 7.0f - lookX * 0.10f), (int)(irisY + 6.0f - lookY * 0.10f), 1, 0x57FF);
    }
  }

  // 4. Anti-aliased upper & lower lashes
  for (int dx = -halfW; dx <= halfW; dx++)
  {
    float nx = (float)dx * (1.0f / 62.0f);
    float asym = isLeftEye ? (nx * 0.08f) : (-nx * 0.08f);
    float arch = 1.0f - nx * nx;
    int topY = (int)roundf((float)lidMeetY - (float)upperH * arch + asym * 8.0f);
    int botY = (int)roundf((float)lidMeetY + (float)lowerH * arch + asym * 5.0f);

    if (topY >= 1 && topY < sprH)
    {
      ptr[topY * sprW + (cx + dx)] = swap16(0x0841);
      ptr[(topY - 1) * sprW + (cx + dx)] = swap16(0x18C3);
    }

    // Curved upper eyelashes
    if (openness > 0.18f && (dx % 4 == 0) && abs(dx) < halfW - 3)
    {
      float side = isLeftEye ? -nx : nx;
      int lashLen = max(4, (int)(7.5f + side * 5.5f));
      float curlX = side * 7.0f;

      for (int s = 1; s <= lashLen; s++)
      {
        float t = (float)s / (float)lashLen;
        float fx = (float)(cx + dx) + curlX * t * t;
        int ix = (int)floorf(fx);
        float frac = fx - (float)ix;
        int ly = topY - 1 - s;
        if (ly < 0 || ix < 0 || ix + 1 >= sprW)
          break;

        uint8_t alpha = (uint8_t)((1.0f - t * 0.72f) * 245.0f);
        int lidx = ly * sprW + ix;
        ptr[lidx] = swap16(blend565(swap16(ptr[lidx]), 0x1082, (uint8_t)(alpha * (1.0f - frac))));
        ptr[lidx + 1] = swap16(blend565(swap16(ptr[lidx + 1]), 0x1082, (uint8_t)(alpha * frac)));
      }
    }

    // Delicate lower eyelashes
    if (openness > 0.45f && (dx % 7 == 0) && abs(dx) < halfW - 10)
    {
      float side = isLeftEye ? -nx : nx;
      int curlX = (int)(side * 2.5f);
      for (int s = 1; s <= 3; s++)
      {
        int ly = botY + 1 + s;
        int lx = cx + dx + (curlX * s) / 3;
        if (ly < sprH && lx >= 0 && lx < sprW)
        {
          int lidx = ly * sprW + lx;
          ptr[lidx] = swap16(blend565(swap16(ptr[lidx]), 0x2104, (uint8_t)(190 - s * 45)));
        }
      }
    }
  }

  eyeSpr.pushSprite(screenX, screenY);
}

void drawEyePair(float openness, float pupilOffsetX, float pupilOffsetY, float pupilScale)
{
  renderAndPushOneEye(8, 56, pupilOffsetX, pupilOffsetY, openness, pupilScale, true);
  renderAndPushOneEye(160, 56, pupilOffsetX, pupilOffsetY, openness, pupilScale, false);
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

  // Fast downward snap (real blinks close ~2x faster than they open)
  const float blinkCurve[] = {1.0f, 0.45f, 0.0f, 0.28f, 0.65f, 0.90f};
  unsigned long now = millis();

  if (forceInit)
  {
    tft.fillScreen(TFT_BLACK);
    blinkStep = 0;
    doubleBlink = false;
    curX = curY = tgtX = tgtY = 0.0f;
    curPupil = tgtPupil = 1.0f;
    squintFactor = tgtSquint = 1.0f;
    drawEyePair(1.0f, 0.0f, 0.0f, 1.0f);
    nextActionMs = now + random(1600, 3200);
    lastMicroMs = now;
    return;
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

  // Subtle living ocular micro-drift & pupil breathing every ~90ms even while stationary
  float microX = 0.0f, microY = 0.0f, breathPupil = 0.0f;

  if (blinkStep == 0 && (now - lastMicroMs >= 90))
  {
    lastMicroMs = now;
    float tSec = (float)now * 0.001f;
    microX = sinf(tSec * 2.1f) * 0.45f;
    microY = cosf(tSec * 1.7f) * 0.30f;
    breathPupil = sinf(tSec * 1.4f) * 0.035f;
    needsRedraw = true;
  }
  if (needsRedraw && blinkStep == 0)
  {
    drawEyePair(squintFactor, curX + microX, curY + microY, curPupil + breathPupil);
  }
  if (now >= nextActionMs)
  {
    if (blinkStep == 0)
    {
      int roll = random(10);
      if (roll < 5)
      {
        // Natural blink (1 in 4 is a quick double-blink)
        blinkStep = 1;
        doubleBlink = (random(4) == 0);
        drawEyePair(blinkCurve[blinkStep] * squintFactor, curX, curY, curPupil);
        nextActionMs = now + 22;
      }
      else if (roll < 8)
      {
        // Quick conversational glance left/right/up/down
        tgtX = (float)random(-17, 18);
        tgtY = (float)random(-6, 6);
        tgtPupil = 0.86f + (float)random(0, 28) * 0.01f;
        tgtSquint = (random(4) == 0) ? 0.84f : 1.0f; // Occasional curious slight squint
        nextActionMs = now + random(1400, 3200);
      }
      else
      {
        // Return to attentive centre gaze
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
        drawEyePair(squintFactor, curX, curY, curPupil);
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
        drawEyePair(blinkCurve[blinkStep] * squintFactor, curX, curY, curPupil);
        nextActionMs = now + (blinkStep == 2 ? 55 : 24);
      }
    }
  }
}

void wakeFromScreenSaver(bool isArrived)
{
  inScreenSaver = false;
  tft.fillScreen(TFT_BLACK);
  displayLines();
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  drawWhere(isArrived);
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
  tft.println("Syncing NTP time...");

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
    if (freshTap)
    {
      wakeFromScreenSaver(isArrived);
      lastRenderedSec = 0;
    }
    else
    {
      updateScreenSaverEyes(false);
    }
    yield();
    return;
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
  }

  delay(25);
}
