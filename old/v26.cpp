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

// Renders one hyper-realistic large eye into a reusable 148x124 sprite (~35.8KB RAM)
void renderAndPushOneEye(TFT_eSprite &spr, int screenX, int screenY, int lookX, int lookY, float openness, bool isLeftEye)
{
  const int cx = 74, cy = 70;
  const int halfW = 60, maxUpperH = 30, maxLowerH = 24;

  spr.fillSprite(TFT_BLACK);

  int upperH = (int)(maxUpperH * openness);
  int lowerH = (int)(maxLowerH * openness);
  int lidMeetY = cy + (int)((1.0f - openness) * 7.0f);

  // 1. Expressive eyebrow arch above the eye
  int browLift = lookY / 2;
  for (int dx = -halfW - 4; dx <= halfW + 4; dx++)
  {
    float nx = (float)dx / (float)(halfW + 4);
    float browArch = max(0.0f, 1.0f - nx * nx);
    float outerLift = isLeftEye ? (-nx * 4.0f) : (nx * 4.0f);
    int browY = 22 - (int)(10.0f * browArch + outerLift) + browLift;

    for (int h = -3; h <= 3; h++)
    {
      float edgeFade = (1.0f - fabsf(nx) * 0.65f) * (1.0f - (float)abs(h) / 4.0f);
      uint8_t alpha = (uint8_t)constrain((int)(edgeFade * 150.0f), 0, 255);
      if (((abs(dx * 7 + h * 13)) % 5) == 0)
        alpha = (uint8_t)(alpha * 0.6f);
      spr.drawPixel(cx + dx, browY + h, blend565(TFT_BLACK, 0x39A6, alpha));
    }
  }

  // 2. Soft upper eyelid crease
  for (int dx = -halfW + 3; dx <= halfW - 3; dx++)
  {
    float nx = (float)dx / (float)halfW;
    float arch = max(0.0f, 1.0f - nx * nx);
    int creaseY = lidMeetY - (int)((upperH + 10) * arch);
    for (int dy = -2; dy <= 2; dy++)
    {
      uint8_t a = (uint8_t)(130 - abs(dy) * 45);
      spr.drawPixel(cx + dx, creaseY + dy, blend565(TFT_BLACK, 0x4A28, a));
    }
  }
  // 3. Shaded sclera + radial textured iris + pupil
  if (openness > 0.06f)
  {
    int irisX = cx + lookX;
    int irisY = cy + lookY;
    const float irisR = 22.0f, pupilR = 9.5f;

    for (int dx = -halfW; dx <= halfW; dx++)
    {
      float nx = (float)dx / (float)halfW;
      float arch = max(0.0f, 1.0f - nx * nx);
      float fTopY = (float)lidMeetY - (float)upperH * arch;
      float fBotY = (float)lidMeetY + (float)lowerH * arch;

      for (int y = (int)floorf(fTopY); y <= (int)ceilf(fBotY); y++)
      {
        float lidCov = min(constrain((float)y - fTopY + 0.5f, 0.0f, 1.0f),
                           constrain(fBotY - (float)y + 0.5f, 0.0f, 1.0f));
        if (lidCov <= 0.01f)
          continue;

        uint16_t pixelCol = 0xF7BE;
        bool nearInner = isLeftEye ? (dx > halfW - 11) : (dx < -halfW + 11);

        if (nearInner && abs(y - lidMeetY) <= 4)
        {
          pixelCol = 0xBAEC; // Tear duct
        }
        else
        {
          float fdx = (float)(cx + dx) - (float)irisX;
          float fdy = (float)y - (float)irisY;
          float dist = sqrtf(fdx * fdx + fdy * fdy);

          float distFromTop = (float)y - fTopY;
          float sphereShade = constrain(1.0f - (nx * nx * 0.35f) - max(0.0f, (7.0f - distFromTop) * 0.06f), 0.45f, 1.0f);
          uint16_t scleraCol = blend565(0x632C, 0xFFDF, (uint8_t)(sphereShade * 255.0f));

          if (dist <= irisR + 1.0f)
          {
            if (dist <= pupilR - 0.6f)
            {
              pixelCol = TFT_BLACK;
            }
            else if (dist <= pupilR + 0.6f)
            {
              uint8_t pa = (uint8_t)((dist - (pupilR - 0.6f)) / 1.2f * 255.0f);
              pixelCol = blend565(TFT_BLACK, 0x8B82, pa);
            }
            else
            {
              float angle = atan2f(fdy, fdx);
              float normR = (dist - pupilR) / (irisR - pupilR);
              float fibreVal = constrain(0.5f + 0.3f * sinf(angle * 26.0f + sinf(angle * 5.0f) * 1.5f) + 0.2f * cosf(angle * 41.0f - normR * 4.0f), 0.0f, 1.0f);
              uint16_t baseIris = blend565(0x0292, 0x3E5F, (uint8_t)(fibreVal * 255.0f));
              if (normR < 0.32f)
                baseIris = blend565(baseIris, 0x9B83, (uint8_t)((1.0f - normR / 0.32f) * 140.0f));
              if (normR > 0.78f)
                baseIris = blend565(baseIris, 0x00A3, (uint8_t)(((normR - 0.78f) / 0.22f) * 220.0f));
              if (distFromTop < 6.0f)
                baseIris = blend565(0x0082, baseIris, (uint8_t)(constrain(distFromTop / 6.0f, 0.35f, 1.0f) * 255.0f));

              pixelCol = (dist > irisR - 1.0f)
                             ? blend565(scleraCol, baseIris, (uint8_t)(constrain((irisR + 1.0f - dist) / 2.0f, 0.0f, 1.0f) * 255.0f))
                             : baseIris;
            }
          }
          else
          {
            pixelCol = scleraCol;
          }
        }
        spr.drawPixel(cx + dx, y, (lidCov < 0.99f) ? blend565(0x18C3, pixelCol, (uint8_t)(lidCov * 255.0f)) : pixelCol);
      }
    }

    // 4. Glossy corneal window catchlights
    if (openness > 0.30f)
    {
      spr.fillCircle(irisX - 7, irisY - 7, 3, TFT_WHITE);
      spr.fillCircle(irisX - 4, irisY - 9, 2, TFT_WHITE);
      spr.fillCircle(irisX + 6, irisY + 6, 1, 0xE73C);
    }
  }

  // 5. Lash rims & curved eyelashes
  for (int dx = -halfW; dx <= halfW; dx++)
  {
    float nx = (float)dx / (float)halfW;
    float arch = max(0.0f, 1.0f - nx * nx);
    int topY = lidMeetY - (int)(upperH * arch);
    int botY = lidMeetY + (int)(lowerH * arch);

    spr.drawPixel(cx + dx, topY, 0x1082);
    spr.drawPixel(cx + dx, topY - 1, 0x2104);
    spr.drawPixel(cx + dx, botY, 0x4208);

    if (openness > 0.20f && (dx % 5 == 0) && abs(dx) < halfW - 3)
    {
      float side = isLeftEye ? -nx : nx;
      int lashLen = max(3, (int)(6.0f + side * 4.0f));
      int curlX = (int)(side * 5.0f);
      for (int s = 1; s <= lashLen; s++)
      {
        float t = (float)s / (float)lashLen;
        spr.drawPixel(cx + dx + (int)(curlX * t * t), topY - 1 - s, blend565(TFT_BLACK, 0x2945, (uint8_t)((1.0f - t * 0.65f) * 230.0f)));
      }
    }
  }

  spr.pushSprite(screenX, screenY);
}

void drawEyePair(float openness, int pupilOffsetX = 0, int pupilOffsetY = 0)
{
  TFT_eSprite eyeSpr(&tft);
  eyeSpr.setColorDepth(16);
  eyeSpr.createSprite(148, 124);

  renderAndPushOneEye(eyeSpr, 10, 58, pupilOffsetX, pupilOffsetY, openness, true);
  renderAndPushOneEye(eyeSpr, 162, 58, pupilOffsetX, pupilOffsetY, openness, false);

  eyeSpr.deleteSprite();
}

void updateScreenSaverEyes(bool forceInit = false)
{
  static unsigned long nextActionMs = 0;
  static int blinkPhase = 0; // 0=open, 1=closing, 2=closed, 3=opening
  static int lookX = 0;
  static int lookY = 0;

  unsigned long now = millis();

  if (forceInit)
  {
    tft.fillScreen(TFT_BLACK);
    blinkPhase = 0;
    lookX = 0;
    lookY = 0;
    drawEyePair(1.0f, 0, 0);
    nextActionMs = now + random(2000, 4500);
    return;
  }

  if (now >= nextActionMs)
  {
    if (blinkPhase == 0)
    {
      // 70% chance to start a smooth blink, 30% chance to glance naturally
      if (random(10) < 7)
      {
        blinkPhase = 1;
        drawEyePair(0.45f, lookX, lookY); // Half-closed
        nextActionMs = now + 45;
      }
      else
      {
        lookX = random(-11, 12);
        lookY = random(-3, 4);
        drawEyePair(1.0f, lookX, lookY);
        nextActionMs = now + random(1800, 3800);
      }
    }
    else if (blinkPhase == 1)
    {
      blinkPhase = 2;
      drawEyePair(0.0f, lookX, lookY); // Fully closed
      nextActionMs = now + 90;
    }
    else if (blinkPhase == 2)
    {
      blinkPhase = 3;
      drawEyePair(0.55f, lookX, lookY); // Half-open
      nextActionMs = now + 45;
    }
    else
    {
      blinkPhase = 0;
      drawEyePair(1.0f, lookX, lookY); // Fully open
      nextActionMs = now + random(2500, 5500);
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

  // Check for screen touch
  uint16_t tx, ty;
  bool touching = getTouchPixel(tx, ty);
  bool freshTap = touching && !touchWasDown;
  touchWasDown = touching;

  if (touching)
  {
    lastInteractionMillis = millis();
  }

  // If currently in screen saver mode
  if (inScreenSaver)
  {
    if (freshTap)
    {
      wakeFromScreenSaver(isArrived);
      lastRenderedSec = 0; // Force immediate countdown & clock redraw on wake
    }
    else
    {
      updateScreenSaverEyes(false);
    }
    delay(25);
    return;
  }

  // Enter screen saver after 10 seconds of inactivity (unless celebratory arrival is active)
  if (!isArrived && (millis() - lastInteractionMillis >= SCREENSAVER_TIMEOUT_MS))
  {
    inScreenSaver = true;
    updateScreenSaverEyes(true);
    delay(25);
    return;
  }

  // Replay fanfare if tapped after arrival
  if (isArrived && freshTap && (millis() - lastReplayMillis > 1500))
  {
    lastReplayMillis = millis();
    playHolidayFanfare();
  }

  // Normal 1-second countdown & clock update
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
