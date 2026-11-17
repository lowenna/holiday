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

/*
 AI Prompt
 You've been helping me with this project. I'm not including all files, but the two main
 ones for now can be at

 https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/src/google-ai-main.cpp and
 https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/src/weather.cpp

 Can you confirm you can see these files. If so, please confirm what they do so I can be sure
 you can help me update it.
 */

char otaHostname[24];
WebServer otaServer(80);
bool otaInProgress = false;

extern void updateScreenSaverEyes(bool);                          // eyes.cpp
extern void drawWeatherBadge();                                   // weather.cpp
extern void fetchDestinationWeather();                            // weather.cpp
extern bool handleWeatherForecastTouch(uint16_t tx, uint16_t ty); // weather.cpp
extern void closeWeatherDetailPopup(bool isArrived);              // weather.cpp
extern bool isWeatherPopupOpen;                                   // weather.cpp
extern unsigned long weatherPopupOpenedMs;                        // weather.cpp
extern String weatherTempStr;                                     // weather.cpp
extern String weatherCondStr;                                     // weather.cpp
extern bool hasCachedCoords;                                      // weather.cpp
extern long destUtcOffsetSec;                                     // weather.cpp
extern bool hasDestUtcOffset;                                     // weather.cpp
bool showDestLocalTime = false;
bool showCountdownAsLocalTime = false;

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
constexpr int16_t TOUCH_MIN_Z = 250;

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
char gWhere[21] = "Horubadhoo";
bool gScreensaverEnabled = true;
bool gWeatherEnabled = true;
bool gDimmerEnabled = true;
bool gIslandPlusOne = false;
bool gBirthdayEnabled = false;
int gBdayMonth = 1;
int gBdayDay = 1;
int gLastCelebratedBdayYear = -1;
bool isBirthdayPopupOpen = false;
unsigned long birthdayPopupOpenedMs = 0;
bool triggerTestBirthday = false;
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

// Forward declarations for functions defined later in this file
bool isValidDateTime(int hour, int mins, int day, int month, int year);
void writePreferences(String hour, String mins, String day, String month, String year, String where);
void displaySSIDAndWhen();
void drawStaticBackgroundImage();
void displayLines();
void drawWhere(bool isArrived);

void handleOTAUpload()
{
  HTTPUpload &upload = otaServer.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    Serial.printf("OTA Update Start: %s\n", upload.filename.c_str());
    otaInProgress = true;
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawCentreString("OTA UPDATE IN PROGRESS...", 160, 100, 4);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (Update.end(true))
    {
      Serial.printf("OTA Update Success: %u bytes\n", upload.totalSize);
    }
    else
    {
      Update.printError(Serial);
      otaInProgress = false;
      showScreenError("OTA FAILED", "Firmware flash failed!", true, 3000);
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    Update.end();
    otaInProgress = false;
    Serial.println("OTA Update Aborted");
  }
}

bool setupOTA()
{
  bool mdnsOk = MDNS.begin(otaHostname);
  if (mdnsOk)
  {
    MDNS.addService("http", "tcp", 80);
  }

  // Main Web Dashboard: Holiday Settings + Firmware OTA
  otaServer.on("/", HTTP_GET, []()
               {
    char dateBuf[12];
    snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", atoi(gYear), atoi(gMonth), atoi(gDay));

    char timeBuf[8];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", atoi(gHour), atoi(gMins));

    char bdayBuf[12];
    snprintf(bdayBuf, sizeof(bdayBuf), "%04d-%02d-%02d", atoi(gYear), gBdayMonth, gBdayDay);

    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Holiday Countdown</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #111; color: #eee; margin: 0; padding: 16px; }
    .card { background: #1c1c1e; max-width: 420px; margin: 0 auto 18px auto; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
    h2 { color: #4dd0e1; margin: 0 0 14px 0; font-size: 20px; }
    label.field { display: block; text-align: left; font-size: 13px; color: #aaa; margin: 12px 0 4px 0; }
    input[type=text], input[type=date], input[type=time] {
      width: 100%; box-sizing: border-box; padding: 10px; border-radius: 8px;
      border: 1px solid #333; background: #2c2c2e; color: #fff; font-size: 16px;
    }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 16px; text-align: left; }
    .chk { background: #2c2c2e; padding: 10px 12px; border-radius: 8px; display: flex; align-items: center; gap: 8px; font-size: 14px; cursor: pointer; }
    input[type=checkbox] { width: 18px; height: 18px; accent-color: #00acc1; }
    button { width: 100%; margin-top: 18px; background: #00acc1; color: #fff; border: none; padding: 12px; font-size: 16px; font-weight: bold; border-radius: 8px; cursor: pointer; }
    button:hover { background: #00838f; }
    .test-btn { background: #6a1b9a; margin-top: 10px; }
    .test-btn:hover { background: #4a148c; }
    .ota-btn { background: #37474f; }
    .ota-btn:hover { background: #455a64; }
    input[type=file] {
      display: block; width: 100%; box-sizing: border-box;
      padding: 12px; margin: 10px 0;
      background: #2c2c2e; border: 1px solid #444; border-radius: 8px;
      color: #eee; font-size: 16px;
    }
    input[type=file]::file-selector-button {
      background: #00acc1; color: #fff; border: none;
      padding: 8px 14px; margin-right: 12px;
      border-radius: 6px; font-size: 15px; font-weight: bold;
    }
    #progWrap { display: none; margin-top: 14px; background: #2c2c2e; border-radius: 8px; overflow: hidden; height: 22px; }
    #progBar { width: 0%; height: 100%; background: #00acc1; transition: width 0.2s; }
    #otaStatus, #bdayStatus { font-size: 13px; color: #aaa; margin-top: 8px; text-align: center; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Holiday Settings</h2>
    <form method="POST" action="/save">
      <label class="field">Destination</label>
      <input type="text" name="where" maxlength="20" value=")rawliteral" + String(gWhere) + R"rawliteral(" required>

      <label class="field">Departure Date</label>
      <input type="date" name="date" value=")rawliteral" + String(dateBuf) + R"rawliteral(" required>

      <label class="field">Departure Time (UK)</label>
      <input type="time" name="time" value=")rawliteral" + String(timeBuf) + R"rawliteral(" required>

      <label class="field">Birthday Date (Local Time)</label>
      <input type="date" name="bday" value=")rawliteral" + String(bdayBuf) + R"rawliteral(">

      <div class="grid">
        <label class="chk"><input type="checkbox" name="ssaver" value="1" )rawliteral" + String(gScreensaverEnabled ? "checked" : "") + R"rawliteral(>Screensaver</label>
        <label class="chk"><input type="checkbox" name="dim" value="1" )rawliteral" + String(gDimmerEnabled ? "checked" : "") + R"rawliteral(>Dimmer</label>
        <label class="chk"><input type="checkbox" name="wxen" value="1" )rawliteral" + String(gWeatherEnabled ? "checked" : "") + R"rawliteral(>Weather</label>
        <label class="chk"><input type="checkbox" name="isl1h" value="1" )rawliteral" + String(gIslandPlusOne ? "checked" : "") + R"rawliteral(>Island +1h</label>
        <label class="chk"><input type="checkbox" name="bdayen" value="1" )rawliteral" + String(gBirthdayEnabled ? "checked" : "") + R"rawliteral(>Birthday</label>
      </div>

      <button type="submit">Save &amp; Reboot</button>
    </form>
    <button type="button" class="test-btn" id="testBdayBtn">Test Birthday Celebration</button>
    <div id="bdayStatus"></div>
  </div>

  <div class="card">
    <h2>Firmware OTA Update</h2>
    <p style="font-size:13px;color:#aaa;margin-top:0">Host: <b>)rawliteral" + String(otaHostname) + R"rawliteral(.local</b></p>
    <form id="otaForm">
      <input type="file" id="fwFile" name="update">
      <button type="submit" class="ota-btn" id="uploadBtn">Upload &amp; Flash .bin</button>
    </form>
    <div id="progWrap"><div id="progBar"></div></div>
    <div id="otaStatus"></div>
  </div>

  <script>
    document.getElementById('testBdayBtn').addEventListener('click', function() {
      const bStatus = document.getElementById('bdayStatus');
      bStatus.textContent = 'Triggering birthday celebration on screen...';
      fetch('/testbday', { method: 'POST' })
        .then(function(r) { return r.text(); })
        .then(function(txt) { bStatus.textContent = txt; })
        .catch(function() { bStatus.textContent = 'Triggered!'; });
    });

    const fwInput = document.getElementById('fwFile');
    const statusEl = document.getElementById('otaStatus');
    const progWrap = document.getElementById('progWrap');
    const progBar = document.getElementById('progBar');

    document.getElementById('otaForm').addEventListener('submit', function(e) {
      e.preventDefault();
      if (!fwInput.files || fwInput.files.length === 0) {
        statusEl.textContent = 'Please choose a .bin file first.';
        return;
      }
      const formData = new FormData();
      formData.append('update', fwInput.files[0]);

      progWrap.style.display = 'block';
      progBar.style.width = '0%';
      statusEl.textContent = 'Uploading firmware...';
      document.getElementById('uploadBtn').disabled = true;

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);
      xhr.upload.onprogress = function(ev) {
        if (ev.lengthComputable) {
          const pct = Math.round((ev.loaded / ev.total) * 100);
          progBar.style.width = pct + '%';
          statusEl.textContent = 'Uploading: ' + pct + '%';
        }
      };
      xhr.onload = function() {
        if (xhr.status === 200) {
          progBar.style.width = '100%';
          statusEl.textContent = 'Update OK! Rebooting device...';
          setTimeout(function() { location.reload(); }, 6000);
        } else {
          statusEl.textContent = 'Update failed (' + xhr.status + ')';
          document.getElementById('uploadBtn').disabled = false;
        }
      };
      xhr.onerror = function() {
        statusEl.textContent = 'Upload complete or connection reset. Reloading...';
        setTimeout(function() { location.reload(); }, 6000);
      };
      xhr.send(formData);
    });
  </script>
</body>
</html>
)rawliteral";

    otaServer.send(200, "text/html", html); });
  // Trigger an immediate Birthday Celebration test on the display
  otaServer.on("/testbday", HTTP_POST, []()
               {
    triggerTestBirthday = true;
    otaServer.send(200, "text/plain", "Birthday celebration triggered!"); });

  // Handle Browser Settings Form Submission
  otaServer.on("/save", HTTP_POST, []()
               {
    String where = otaServer.arg("where");
    String dateStr = otaServer.arg("date"); // Expected format: YYYY-MM-DD
    String timeStr = otaServer.arg("time"); // Expected format: HH:MM
    String bdayStr = otaServer.arg("bday"); // Expected format: YYYY-MM-DD

    if (dateStr.length() < 10 || timeStr.length() < 5)
    {
      otaServer.send(400, "text/plain", "Invalid date or time format.");
      return;
    }

    String year = dateStr.substring(0, 4);
    String month = dateStr.substring(5, 7);
    String day = dateStr.substring(8, 10);
    String hour = timeStr.substring(0, 2);
    String mins = timeStr.substring(3, 5);

    if (!isValidDateTime(hour.toInt(), mins.toInt(), day.toInt(), month.toInt(), year.toInt()))
    {
      otaServer.send(400, "text/plain", "Date or time out of valid range.");
      return;
    }

    // Save toggle switches & birthday date to NVS first
    gScreensaverEnabled = otaServer.hasArg("ssaver");
    gDimmerEnabled = otaServer.hasArg("dim");
    gWeatherEnabled = otaServer.hasArg("wxen");
    gIslandPlusOne = otaServer.hasArg("isl1h");
    gBirthdayEnabled = otaServer.hasArg("bdayen");

    if (bdayStr.length() >= 10)
    {
      int bm = bdayStr.substring(5, 7).toInt();
      int bd = bdayStr.substring(8, 10).toInt();
      if (bm >= 1 && bm <= 12 && bd >= 1 && bd <= 31)
      {
        gBdayMonth = bm;
        gBdayDay = bd;
      }
    }

    prefs.begin("hol", false);
    prefs.putBool("ssaver", gScreensaverEnabled);
    prefs.putBool("dim", gDimmerEnabled);
    prefs.putBool("wxen", gWeatherEnabled);
    prefs.putBool("isl1h", gIslandPlusOne);
    prefs.putBool("bdayen", gBirthdayEnabled);
    prefs.putInt("bdaym", gBdayMonth);
    prefs.putInt("bdayd", gBdayDay);
    prefs.putInt("bdayyr", -1); // Reset yearly trigger flag on settings save
    prefs.end();

    // Send confirmation response to browser before ESP restarts
    String resp = "<html><body style='font-family:sans-serif;background:#111;color:#4dd0e1;text-align:center;padding:40px'>"
                  "<h2>Settings Saved!</h2><p style='color:#eee'>Rebooting device now...</p></body></html>";
    otaServer.send(200, "text/html", resp);
    delay(400);

    // Save holiday date/time/destination, show confirmation on TFT, and reboot
    writePreferences(hour, mins, day, month, year, where); });

  otaServer.on("/update", HTTP_POST, []()
               {
    bool ok = !Update.hasError() && otaInProgress;
    otaServer.sendHeader("Connection", "close");
    otaServer.send(200, "text/plain", ok ? "OK" : "FAIL");
    delay(1000);
    if (ok)
    {
      ESP.restart();
    } }, handleOTAUpload);

  otaServer.begin();
  Serial.printf("Web UI & OTA Ready: http://%s.local (IP: %s)\n", otaHostname, WiFi.localIP().toString().c_str());
  return mdnsOk;
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

void drawSettingsGearIcon(TFT_eSprite &spr, int cx, int cy, uint16_t col)
{
  // Outer gear teeth (8 spokes)
  for (int a = 0; a < 180; a += 45)
  {
    float rad = a * 0.0174533f;
    int dx = (int)roundf(cosf(rad) * 6.0f);
    int dy = (int)roundf(sinf(rad) * 6.0f);
    spr.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, col);
  }
  // Gear body & centre hole
  spr.fillCircle(cx, cy, 4, col);
  spr.fillCircle(cx, cy, 2, TFT_BLACK);
}

struct tm getActiveLocalTm(time_t rawLocal)
{
  struct tm resTm;
  if (hasDestUtcOffset)
  {
    time_t destEpoch = rawLocal + getEffectiveDestOffsetSec();
    gmtime_r(&destEpoch, &resTm);
  }
  else
  {
    localtime_r(&rawLocal, &resTm);
  }
  return resTm;
}

bool isBirthdayToday(time_t rawLocal)
{
  if (!gBirthdayEnabled)
    return false;

  struct tm localTm = getActiveLocalTm(rawLocal);
  return ((localTm.tm_mon + 1) == gBdayMonth) && (localTm.tm_mday == gBdayDay);
}

void playBirthdayTune()
{
  // Classic "Happy Birthday" melody (notes in Hz & durations in ms)
  const int notes[] = {
      262, 262, 294, 262, 349, 330,
      262, 262, 294, 262, 392, 349,
      262, 262, 523, 440, 349, 330, 294,
      466, 466, 440, 349, 392, 349};
  const int durs[] = {
      180, 120, 300, 300, 300, 550,
      180, 120, 300, 300, 300, 550,
      180, 120, 300, 300, 300, 300, 500,
      180, 120, 300, 300, 300, 600};

  const int count = sizeof(notes) / sizeof(notes[0]);
  for (int i = 0; i < count; i++)
  {
    tone(AZTOUCH_BEEPER, notes[i], durs[i]);
    delay(durs[i] + 35);
  }
  noTone(AZTOUCH_BEEPER);
}

void drawBirthdayPopupCard()
{
  const int16_t popW = 284;
  const int16_t popH = 166;
  const int16_t popX = (SCREEN_W - popW) / 2;
  const int16_t popY = 38;

  tft.fillRoundRect(popX, popY, popW, popH, 10, TFT_NAVY);
  tft.drawRoundRect(popX, popY, popW, popH, 10, TFT_GOLD);
  tft.drawRoundRect(popX + 1, popY + 1, popW - 2, popH - 2, 9, TFT_MAGENTA);

  // Festive confetti dots
  const uint16_t confettiCols[] = {TFT_GOLD, TFT_MAGENTA, TFT_CYAN, TFT_GREEN, TFT_ORANGE, TFT_YELLOW};
  for (int i = 0; i < 28; i++)
  {
    int cx = popX + 14 + ((i * 53) % (popW - 28));
    int cy = popY + 12 + ((i * 37) % (popH - 24));
    tft.fillCircle(cx, cy, (i % 2) + 1, confettiCols[i % 6]);
  }

  // Three festive balloons along the top
  const int bx[] = {popX + 55, popX + 142, popX + 229};
  const uint16_t bCol[] = {TFT_RED, TFT_MAGENTA, TFT_CYAN};
  for (int i = 0; i < 3; i++)
  {
    tft.fillCircle(bx[i], popY + 34, 14, bCol[i]);
    tft.drawCircle(bx[i], popY + 34, 14, TFT_WHITE);
    tft.drawLine(bx[i], popY + 48, bx[i] - 3, popY + 62, TFT_SILVER);
  }

  // Main celebratory text
  tft.setFreeFont(nullptr);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_GOLD, TFT_NAVY);
  tft.drawString("HAPPY", popX + (popW / 2), popY + 86, 4);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("BIRTHDAY!", popX + (popW / 2), popY + 118, 4);

  tft.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
  tft.drawString("Tap anywhere to close", popX + (popW / 2), popY + popH - 13, 1);
  tft.setTextDatum(TL_DATUM);
}

void triggerBirthdayCelebration(bool playSound)
{
  isWeatherPopupOpen = false;
  isBirthdayPopupOpen = true;
  birthdayPopupOpenedMs = millis();
  drawBirthdayPopupCard();
  if (playSound)
  {
    playBirthdayTune();
  }
}

void closeBirthdayCelebration(bool isArrived)
{
  if (!isBirthdayPopupOpen)
    return;

  isBirthdayPopupOpen = false;
  tft.fillScreen(TFT_BLACK);
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  displayLines();
  drawWhere(isArrived);
  drawWeatherBadge();
}

void displayCurrentTimeSprite(time_t rawLocal)
{
  topTimeSprite.fillSprite(TFT_BLACK);
  topTimeSprite.setFreeFont(nullptr);
  topTimeSprite.setTextFont(2);
  topTimeSprite.setTextSize(1);

  // On your birthday, alternate "HAPPY BIRTHDAY!" every 4 seconds with the live clock
  if (isBirthdayToday(rawLocal) && ((rawLocal / 4) % 2 == 1))
  {
    topTimeSprite.setTextColor(TFT_GOLD, TFT_BLACK);
    topTimeSprite.drawCentreString("HAPPY BIRTHDAY!", (SCREEN_W - 18) / 2, 0, 2);
  }
  else if (showDestLocalTime && hasDestUtcOffset)
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
    topTimeSprite.drawCentreString(timeStr, (SCREEN_W - 18) / 2, 0, 2);
  }
  else
  {
    topTimeSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    topTimeSprite.drawCentreString(formatPrettyDateTime(rawLocal, true), (SCREEN_W - 18) / 2, 0, 2);
  }

  // Subtle settings gear in the top-right corner
  drawSettingsGearIcon(topTimeSprite, SCREEN_W - 10, TOP_BAR_H / 2, TFT_SILVER);

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
  // Smooth 7-step RGB565 gradient bridging the black bars into the photo
  const uint16_t gradCols[7] = {
      0x0000, // Pure black (matches top/bottom bars)
      0x0841, // Very deep charcoal
      0x10A2, // Dark slate
      0x18E3, // Muted slate
      0x2145, // Soft slate-blue
      0x29A6, // Gentle dusk tint
      0x31E7  // Subtle edge highlight
  };

  // Top fade: y = 17 (black) down to y = 23 (photo top edge)
  for (int i = 0; i < 7; i++)
  {
    tft.drawFastHLine(0, TOP_LINE_Y + i, SCREEN_W, gradCols[i]);
  }

  // Bottom fade: y = 216 (photo bottom edge) down to y = 222 (black)
  for (int i = 0; i < 7; i++)
  {
    tft.drawFastHLine(0, BOT_LINE_Y - i, SCREEN_W, gradCols[i]);
  }
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

constexpr unsigned long BL_CLOCK_ONLY_DELAY_MS = 300000UL; // 30s after max dim, hide everything except the clock
bool inDimClockMode = false;

void drawCountdownLeft(int days, int hours, int mins, int secs, bool showColon, bool isArrived, time_t rawlocal)
{
  if (inDimClockMode)
  {
    countdownFgSprite.fillSprite(TFT_BLACK);
  }
  else
  {
    countdownBgSprite.pushToSprite(&countdownFgSprite, 0, 0);
  }

  countdownFgSprite.setTextSize(1);
  countdownFgSprite.setTextFont(7);
  countdownFgSprite.setTextColor(TFT_DARKCYAN);

  if (isArrived || (showCountdownAsLocalTime && hasDestUtcOffset))
  {
    // Show live local time at the destination in crisp cyan (HH:MM:SS)
    time_t destEpoch = hasDestUtcOffset ? (rawlocal + getEffectiveDestOffsetSec()) : rawlocal;

    struct tm destTm;
    gmtime_r(&destEpoch, &destTm);

    char sep = showColon ? ':' : ' ';
    if (inDimClockMode)
    {
      char buf[16];
      snprintf(buf, sizeof(buf), "%02d%c%02d%c%02d", destTm.tm_hour, sep, destTm.tm_min, sep, destTm.tm_sec);
      countdownFgSprite.drawCentreString(buf, SCREEN_W / 2, 0, 7);
    }
    else
    {
      countdownFgSprite.setCursor(54, 0);
      countdownFgSprite.printf("%02d%c", destTm.tm_hour, sep);
      countdownFgSprite.setCursor(132, 0);
      countdownFgSprite.printf("%02d%c", destTm.tm_min, sep);
      countdownFgSprite.setCursor(210, 0);
      countdownFgSprite.printf("%02d", destTm.tm_sec);
    }
  }
  else
  {
    char sep = showColon ? ':' : ' ';
    if (inDimClockMode)
    {
      char buf[20];
      snprintf(buf, sizeof(buf), "%02d.%02d%c%02d%c%02d", days, hours, sep, mins, sep, secs);
      countdownFgSprite.drawCentreString(buf, SCREEN_W / 2, 0, 7);
    }
    else
    {
      countdownFgSprite.setCursor(15, 0);
      countdownFgSprite.printf("%02d.", days);
      countdownFgSprite.setCursor(95, 0);
      countdownFgSprite.printf("%02d%c", hours, sep);
      countdownFgSprite.setCursor(170, 0);
      countdownFgSprite.printf("%02d%c", mins, sep);
      countdownFgSprite.setCursor(245, 0);
      countdownFgSprite.printf("%02d", secs);
    }
  }

  int16_t targetY = inDimClockMode ? ((SCREEN_H - COUNTDOWN_H) / 2) : COUNTDOWN_Y;
  countdownFgSprite.pushSprite(0, targetY);
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
  const int16_t tileH = 38;

  tft.fillRoundRect(tileX, tileY, tileW, tileH, 6, TFT_BLACK);
  tft.drawRoundRect(tileX, tileY, tileW, tileH, 6, isEnabled ? TFT_DARKGREEN : TFT_DARKGREY);

  // Label on top-left of tile
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(label, tileX + 8, tileY + 2, 2);

  // "ON" / "OFF" status on bottom-left
  tft.setTextColor(isEnabled ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.drawString(isEnabled ? "ON" : "OFF", tileX + 8, tileY + 19, 2);

  // Compact sliding pill switch on bottom-right (46x16)
  const int16_t swW = 46;
  const int16_t swH = 16;
  const int16_t swX = tileX + tileW - swW - 8;
  const int16_t swY = tileY + 19;

  uint16_t trackCol = isEnabled ? TFT_DARKGREEN : TFT_DARKGREY;
  uint16_t borderCol = isEnabled ? TFT_GREEN : TFT_LIGHTGREY;

  tft.fillRoundRect(swX, swY, swW, swH, swH / 2, trackCol);
  tft.drawRoundRect(swX, swY, swW, swH, swH / 2, borderCol);

  int16_t knobR = 5;
  int16_t knobY = swY + (swH / 2);
  int16_t knobX = isEnabled ? (swX + swW - knobR - 3) : (swX + knobR + 3);
  tft.fillCircle(knobX, knobY, knobR, TFT_WHITE);
}

void showWebUpdateInfoScreen(bool mdnsOk)
{
  tft.fillScreen(TFT_BLACK);

  // Matching Header Banner
  tft.fillRect(0, 0, SCREEN_W, 24, TFT_NAVY);
  tft.drawFastHLine(0, 24, SCREEN_W, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("Web UI & OTA Update", SCREEN_W / 2, 12, 2);

  // Instruction & URL Card
  tft.drawRoundRect(10, 34, SCREEN_W - 20, 134, 8, TFT_DARKCYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(18, 42);
  tft.print("Browse from phone or PC to");
  tft.setCursor(18, 60);
  tft.print("change settings or flash .bin:");

  int urlY = 90;
  if (mdnsOk)
  {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(22, urlY);
    tft.printf("http://%s.local", otaHostname);
    urlY += 24;
  }

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(22, urlY);
  tft.printf("http://%s", WiFi.localIP().toString().c_str());

  // Full-width Back Button (y = 180..212)
  const int16_t backX = 12;
  const int16_t backY = 180;
  const int16_t backW = SCREEN_W - 24;
  const int16_t backH = 32;
  tft.drawRoundRect(backX, backY, backW, backH, 6, TFT_CYAN);
  tft.fillRoundRect(backX + 2, backY + 2, backW - 4, backH - 4, 5, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("< Back", SCREEN_W / 2, backY + (backH / 2), 2);
  tft.setTextDatum(TL_DATUM);

  // Wait for user to tap Back
  bool wasTouching = true;
  uint16_t tx, ty;
  while (true)
  {
    otaServer.handleClient();
    bool touching = getTouchPixel(tx, ty);
    if (touching && !wasTouching)
    {
      if (tx >= backX && tx <= (backX + backW) && ty >= backY && ty <= (backY + backH))
      {
        tone(AZTOUCH_BEEPER, 784, 50);
        break;
      }
    }
    wasTouching = touching;
    delay(25);
  }
}

void drawConfigScreenBase(int16_t col1X, int16_t col2X, int16_t tileW,
                          int16_t row1Y, int16_t row2Y, int16_t row3Y,
                          int16_t setupX, int16_t updateX, int16_t startX,
                          int16_t btnY, int16_t btnW, int16_t btnH)
{
  tft.fillScreen(TFT_BLACK);

  // 1. Matching Top Header Banner (y = 0..24)
  tft.fillRect(0, 0, SCREEN_W, 24, TFT_NAVY);
  tft.drawFastHLine(0, 24, SCREEN_W, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("Settings & Options", SCREEN_W / 2, 12, 2);
  tft.setTextDatum(TL_DATUM);

  // 2. Toggle Tiles (y = 32..166)
  drawToggleTile(col1X, row1Y, tileW, "Screensaver", gScreensaverEnabled);
  drawToggleTile(col2X, row1Y, tileW, "Dimmer", gDimmerEnabled);

  drawToggleTile(col1X, row2Y, tileW, "Weather", gWeatherEnabled);
  drawToggleTile(col2X, row2Y, tileW, "Island +1h", gIslandPlusOne);

  drawToggleTile(col1X, row3Y, tileW, "Birthday", gBirthdayEnabled);

  // 3. Bottom Action Buttons: Setup | Update | Start (10s...)
  tft.drawRoundRect(setupX, btnY, btnW, btnH, 6, TFT_CYAN);
  tft.fillRoundRect(setupX + 2, btnY + 2, btnW - 4, btnH - 4, 5, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("Setup", setupX + (btnW / 2), btnY + (btnH / 2), 2);

  tft.drawRoundRect(updateX, btnY, btnW, btnH, 6, TFT_CYAN);
  tft.fillRoundRect(updateX + 2, btnY + 2, btnW - 4, btnH - 4, 5, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("Update", updateX + (btnW / 2), btnY + (btnH / 2), 2);

  tft.drawRoundRect(startX, btnY, btnW, btnH, 6, TFT_GREEN);
  tft.fillRoundRect(startX + 2, btnY + 2, btnW - 4, btnH - 4, 5, TFT_DARKGREEN);
  tft.setTextDatum(TL_DATUM);

  // 4. Footer Copyright
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("(C) John Howard 2026", SCREEN_W / 2, 226, 2);
  tft.setTextDatum(TL_DATUM);
}

bool areWeConfiguring(bool mdnsOk)
{
  unsigned long startAt = millis();
  bool triggered = false;
  bool exitNow = false;
  int lastSecsLeft = -1;
  bool wasTouching = true; // Ignore held finger until lifted

  // Toggle grid layout (y = 32..166)
  const int16_t col1X = 12;
  const int16_t col2X = 166;
  const int16_t tileW = 142;
  const int16_t row1Y = 32;
  const int16_t row2Y = 76;
  const int16_t row3Y = 120;
  const int16_t tileH = 38;

  // 3-Button Row layout (y = 172..204)
  const int16_t btnY = 172;
  const int16_t btnH = 32;
  const int16_t btnW = 94;
  const int16_t setupX = 12;
  const int16_t updateX = 113;
  const int16_t startX = 214;

  drawConfigScreenBase(col1X, col2X, tileW, row1Y, row2Y, row3Y,
                       setupX, updateX, startX, btnY, btnW, btnH);

  uint16_t tx, ty;
  while ((millis() - startAt < 10000) && !triggered && !exitNow)
  {
    otaServer.handleClient();

    int secsLeft = max(1, 10 - (int)((millis() - startAt) / 1000));
    if (secsLeft != lastSecsLeft)
    {
      lastSecsLeft = secsLeft;
      tft.fillRoundRect(startX + 2, btnY + 2, btnW - 4, btnH - 4, 5, TFT_DARKGREEN);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
      tft.drawString("Start (" + String(secsLeft) + "s)", startX + (btnW / 2), btnY + (btnH / 2), 2);
      tft.setTextDatum(TL_DATUM);
    }

    bool touching = getTouchPixel(tx, ty);
    if (touching && !wasTouching)
    {
      // Row 1 Left: Screensaver
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
      // Row 1 Right: Dimmer
      else if (tx >= col2X && tx <= (col2X + tileW) && ty >= row1Y && ty <= (row1Y + tileH))
      {
        gDimmerEnabled = !gDimmerEnabled;
        prefs.begin("hol", false);
        prefs.putBool("dim", gDimmerEnabled);
        prefs.end();

        drawToggleTile(col2X, row1Y, tileW, "Dimmer", gDimmerEnabled);
        tone(AZTOUCH_BEEPER, gDimmerEnabled ? 880 : 440, 80);

        startAt = millis();
        lastSecsLeft = -1;
      }
      // Row 2 Left: Weather
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
      // Row 2 Right: Island +1h
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
      // Row 3 Left: Birthday
      else if (tx >= col1X && tx <= (col1X + tileW) && ty >= row3Y && ty <= (row3Y + tileH))
      {
        gBirthdayEnabled = !gBirthdayEnabled;
        prefs.begin("hol", false);
        prefs.putBool("bdayen", gBirthdayEnabled);
        prefs.end();

        drawToggleTile(col1X, row3Y, tileW, "Birthday", gBirthdayEnabled);
        tone(AZTOUCH_BEEPER, gBirthdayEnabled ? 880 : 440, 80);

        startAt = millis();
        lastSecsLeft = -1;
      }
      // Button 1: Setup
      else if (tx >= setupX && tx <= (setupX + btnW) && ty >= btnY && ty <= (btnY + btnH))
      {
        tone(AZTOUCH_BEEPER, 880, 60);
        triggered = true;
      }
      // Button 2: Update (opens Web UI & OTA URLs sub-page)
      else if (tx >= updateX && tx <= (updateX + btnW) && ty >= btnY && ty <= (btnY + btnH))
      {
        tone(AZTOUCH_BEEPER, 880, 60);
        showWebUpdateInfoScreen(mdnsOk);
        drawConfigScreenBase(col1X, col2X, tileW, row1Y, row2Y, row3Y,
                             setupX, updateX, startX, btnY, btnW, btnH);
        startAt = millis();
        lastSecsLeft = -1;
        wasTouching = true;
      }
      // Button 3: Start (10s...) — starts immediately when tapped
      else if (tx >= startX && tx <= (startX + btnW) && ty >= btnY && ty <= (btnY + btnH))
      {
        tone(AZTOUCH_BEEPER, 1047, 60);
        exitNow = true;
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
  gDimmerEnabled = prefs.getBool("dim", true);
  gIslandPlusOne = prefs.getBool("isl1h", false);
  gBirthdayEnabled = prefs.getBool("bdayen", false);
  gBdayMonth = prefs.getInt("bdaym", 1);
  gBdayDay = prefs.getInt("bdayd", 1);
  gLastCelebratedBdayYear = prefs.getInt("bdayyr", -1);
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
  tft.fillRect(0, 0, SCREEN_W, 24, TFT_NAVY);
  tft.drawFastHLine(0, 24, SCREEN_W, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("Holiday Countdown", SCREEN_W / 2, 12, 2);

  // Preferences Card (y = 30..136)
  tft.drawRoundRect(8, 30, SCREEN_W - 16, 106, 8, TFT_DARKCYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, 34);
  tft.print("Holiday Preferences:");

  if (gPrefStatus == PREF_VALID)
  {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(16, 53);
    tft.printf("Destination: %s", gWhere);

    tft.setCursor(16, 72);
    tft.printf("Departure:   %s/%s/%s at %s:%s", gDay, gMonth, gYear, gHour, gMins);

    tft.setCursor(16, 91);
    tft.print("Birthday:    ");
    if (gBirthdayEnabled)
    {
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.printf("Enabled (%02d/%02d)", gBdayDay, gBdayMonth);
    }
    else
    {
      tft.setTextColor(TFT_SILVER, TFT_BLACK);
      tft.printf("Disabled (%02d/%02d)", gBdayDay, gBdayMonth);
    }

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(16, 111);
    tft.print("Status: Configured");
  }
  else if (gPrefStatus == PREF_NOT_CONFIGURED)
  {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(16, 56);
    tft.print("Status: Not yet configured");

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(16, 78);
    tft.printf("Default: %s (%s/%s/%s)", gWhere, gDay, gMonth, gYear);

    tft.setCursor(16, 100);
    tft.printf("Birthday: %s (%02d/%02d)", gBirthdayEnabled ? "Enabled" : "Disabled", gBdayDay, gBdayMonth);
  }
  else
  {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(16, 62);
    tft.print("Status: Invalid settings found");

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(16, 86);
    tft.print("Please reconfigure in setup portal");
  }

  // Wi-Fi Connection Status Card (y = 142..234)
  tft.drawRoundRect(8, 142, SCREEN_W - 16, 92, 8, TFT_DARKGREY);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(16, 148);
  tft.print("Wi-Fi Status:");

  if (savedSsid.length() > 0)
  {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(16, 169);
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
    tft.setCursor(16, 169);
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
  tft.drawString("Setup & Configure", SCREEN_W / 2, 13, 2);

  // Styled instruction card
  tft.drawRoundRect(8, 34, 304, 172, 8, TFT_DARKCYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);

  int y = 41;
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

  // Bottom-right Cancel button (x = 236..312, y = 213..235)
  const int16_t cancelX = 236;
  const int16_t cancelY = 213;
  const int16_t cancelW = 76;
  const int16_t cancelH = 22;
  tft.drawRoundRect(cancelX, cancelY, cancelW, cancelH, 5, TFT_RED);
  tft.fillRoundRect(cancelX + 1, cancelY + 1, cancelW - 2, cancelH - 2, 4, TFT_MAROON);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawString("Cancel", cancelX + (cancelW / 2), cancelY + (cancelH / 2), 2);
  tft.setTextDatum(TL_DATUM);

  if (showTimeoutHint)
  {
    tft.setTextColor(TFT_SILVER, TFT_BLACK);
    tft.drawString("Portal timeout: 10 mins", 12, 216, 2);
  }
}

void configModeCallback(WiFiManager *myWiFiManager)
{
  String apName = myWiFiManager->getConfigPortalSSID();
  Serial.printf("WiFi connect failed; entered AP mode: %s\n", apName.c_str());
  drawSetupPortalScreen(apName, false);
}

// Runs the WiFiManager config portal non-blockingly while updating the live MM:SS countdown
// Returns true if the portal timed out OR was cancelled by the user
bool runNonBlockingPortal(WiFiManager &wm, unsigned long durationMs = 600000UL, bool *wasCancelled = nullptr)
{
  if (wasCancelled != nullptr)
  {
    *wasCancelled = false;
  }

  bool timedOut = false;
  unsigned long portalStartMs = millis();
  int lastSecShown = -1;
  bool wasTouching = true; // Ignore held finger until lifted

  while (wm.getConfigPortalActive())
  {
    wm.process();

    // Check for Cancel button tap (bottom-right: x = 232..316, y = 210..239)
    uint16_t tx, ty;
    bool touching = getTouchPixel(tx, ty);
    if (touching && !wasTouching)
    {
      if (tx >= 232 && tx <= 316 && ty >= 210 && ty <= 239)
      {
        tone(AZTOUCH_BEEPER, 440, 80);
        wm.stopConfigPortal();
        if (wasCancelled != nullptr)
        {
          *wasCancelled = true;
        }
        return true;
      }
    }
    wasTouching = touching;

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

      // Draw left-aligned on bottom bar so it never overlaps the Cancel button
      tft.fillRect(8, 214, 222, 22, TFT_BLACK);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(secsLeft <= 30 ? TFT_RED : TFT_SILVER, TFT_BLACK);
      tft.drawString(timerText, 12, 216, 2);
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
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  displayLines();
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

constexpr uint8_t BL_PWM_CHANNEL = 6;          // Channel 6 (Timer 3) avoids collision with tone() on Channel 0
constexpr uint8_t BL_BRIGHT_DAY = 0;           // Active-LOW: 0 = 100% full brightness
constexpr uint8_t BL_BRIGHT_NIGHT = 170;       // Active-LOW: 195 = ~24% dim mode, ~25 less per 10% more brightness
constexpr unsigned long BL_BOOST_MS = 15000UL; // Hold full brightness for 15s after touch
constexpr unsigned long BL_FADE_MS = 30000UL;  // Fade gradually over 30 seconds

unsigned long lastBacklightBoostMs = 0;
uint8_t currentBacklightLevel = BL_BRIGHT_DAY;

void initBacklightPWM()
{
  ledcSetup(BL_PWM_CHANNEL, 31250, 8); // 31.25 kHz on dedicated Channel 6
  ledcAttachPin(15, BL_PWM_CHANNEL);   // AZ-Touch MOD backlight is GPIO 15
  ledcWrite(BL_PWM_CHANNEL, BL_BRIGHT_DAY);
  currentBacklightLevel = BL_BRIGHT_DAY;
}

void wakeFromDimClockMode(bool isArrived)
{
  if (!inDimClockMode)
    return;

  inDimClockMode = false;
  tft.fillScreen(TFT_BLACK);
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  displayLines();
  drawWhere(isArrived);
  drawWeatherBadge();
}

void updateAutoBacklight(bool isArrived)
{
  if (!gDimmerEnabled)
  {
    if (inDimClockMode)
    {
      wakeFromDimClockMode(isArrived);
    }
    if (currentBacklightLevel != BL_BRIGHT_DAY)
    {
      currentBacklightLevel = BL_BRIGHT_DAY;
      ledcWrite(BL_PWM_CHANNEL, currentBacklightLevel);
    }
    return;
  }

  unsigned long now = millis();
  if (lastBacklightBoostMs == 0)
  {
    lastBacklightBoostMs = now;
  }

  uint8_t targetLevel = BL_BRIGHT_DAY;
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
    if (!inDimClockMode && !inScreenSaver &&
        sinceTouch >= (BL_BOOST_MS + BL_FADE_MS + BL_CLOCK_ONLY_DELAY_MS))
    {
      inDimClockMode = true;
      tft.fillScreen(TFT_BLACK);
    }
  }

  if (targetLevel != currentBacklightLevel)
  {
    currentBacklightLevel = targetLevel;
    ledcWrite(BL_PWM_CHANNEL, currentBacklightLevel);
  }
}

void openInAppSetup()
{
  LcarsAudio::beepBoot();
  bool wasWeatherEnabled = gWeatherEnabled;
  bool wasIslandPlusOne = gIslandPlusOne;

  // Check if mDNS is active
  bool mdnsOk = true;
  while (areWeConfiguring(mdnsOk))
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

    bool portalCancelled = false;
    bool portalTimedOut = runNonBlockingPortal(wifiManager, 600000UL, &portalCancelled);

    wifiManager.setConfigPortalBlocking(true);
    otaServer.begin();

    if (portalCancelled)
    {
      continue; // Go back to the toggle buttons screen
    }

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
    break;
  }

  // Redraw main countdown screen cleanly
  tft.fillScreen(TFT_BLACK);
  drawStaticBackgroundImage();
  displayLines();
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

  // 6. Start OTA, then show combined OTA + "Tap to configure" screen for 10s
  bool mdnsOk = setupOTA();
  bool portalRan = false;
  bool portalTimedOut = false;

  while (areWeConfiguring(mdnsOk))
  {
    portalRan = true;
    otaServer.stop();

    String apName = String(otaHostname);
    drawSetupPortalScreen(apName, false);

    // Run config portal non-blocking with live MM:SS countdown
    wifiManager.setConfigPortalBlocking(false);
    wifiManager.startConfigPortal(apName.c_str());

    bool portalCancelled = false;
    portalTimedOut = runNonBlockingPortal(wifiManager, 600000UL, &portalCancelled);

    wifiManager.setConfigPortalBlocking(true);
    otaServer.begin();

    if (portalCancelled)
    {
      portalRan = false;
      continue; // Return to the toggle buttons screen
    }
    break;
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
  tft.println("Getting current time from the Internet...");

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
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  displayLines();
  drawWhere(false);

  fetchDestinationWeather();
  drawWeatherBadge();
  lastWeatherFetchMs = millis();

  lastInteractionMillis = millis();
  lastBacklightBoostMs = millis();

  Serial.println("setup() complete");
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
    if (!inScreenSaver && !inDimClockMode)
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

  if (freshTap)
  {
    lastInteractionMillis = millis();
    lastBacklightBoostMs = millis();
  }

  // Handle web dashboard "Test Birthday Celebration" button
  if (triggerTestBirthday)
  {
    triggerTestBirthday = false;
    lastInteractionMillis = millis();
    lastBacklightBoostMs = millis();
    if (inScreenSaver)
    {
      wakeFromScreenSaver(isArrived);
    }
    if (inDimClockMode)
    {
      wakeFromDimClockMode(isArrived);
    }
    updateAutoBacklight(isArrived);
    triggerBirthdayCelebration(true);
    lastRenderedSec = 0;
  }

  // Automatic 08:30 destination local time birthday celebration (once per year)
  if (gBirthdayEnabled && isBirthdayToday(rawlocal))
  {
    struct tm localTm = getActiveLocalTm(rawlocal);
    int curYear = localTm.tm_year + 1900;
    bool pastMorningWake = (localTm.tm_hour > 8) || (localTm.tm_hour == 8 && localTm.tm_min >= 30);

    if (pastMorningWake && gLastCelebratedBdayYear != curYear)
    {
      gLastCelebratedBdayYear = curYear;
      prefs.begin("hol", false);
      prefs.putInt("bdayyr", curYear);
      prefs.end();

      lastInteractionMillis = millis();
      lastBacklightBoostMs = millis();
      if (inScreenSaver)
      {
        wakeFromScreenSaver(isArrived);
      }
      if (inDimClockMode)
      {
        wakeFromDimClockMode(isArrived);
      }
      updateAutoBacklight(isArrived);
      triggerBirthdayCelebration(true);
      lastRenderedSec = 0;
    }
  }

  // Smoothly update PWM backlight & clock-only dim mode every loop tick (~40 Hz)
  updateAutoBacklight(isArrived);

  // Refresh destination weather every 30 minutes (even while screensaver or dim clock is active)
  if (gWeatherEnabled && (millis() - lastWeatherFetchMs >= WEATHER_INTERVAL_MS))
  {
    lastWeatherFetchMs = millis();
    fetchDestinationWeather();
    if (!inScreenSaver && !inDimClockMode && !isWeatherPopupOpen && !isBirthdayPopupOpen)
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

  // Wake from clock-only dim mode on any tap, absorbing the wake-up tap
  if (inDimClockMode && freshTap)
  {
    wakeFromDimClockMode(isArrived);
    lastRenderedSec = 0;
    freshTap = false;
  }

  // Close birthday popup on any tap or automatically after 20 seconds
  if (isBirthdayPopupOpen)
  {
    if (freshTap || (millis() - birthdayPopupOpenedMs >= 20000UL))
    {
      closeBirthdayCelebration(isArrived);
      lastRenderedSec = 0;
      freshTap = false;
    }
  }

  // Auto-close weather detail popup after 15 seconds
  if (isWeatherPopupOpen && (millis() - weatherPopupOpenedMs >= 15000UL))
  {
    closeWeatherDetailPopup(isArrived);
    lastRenderedSec = 0;
  }

  // Top bar touch handling:
  // 1. Top-right gear icon (x >= 285, y <= 35) opens setup
  // 2. On your birthday, tapping the top bar replays the celebration
  // 3. Otherwise, rest of top bar (x < 285, y <= 35) toggles UK / Destination time
  if (freshTap && !inScreenSaver && !inDimClockMode && !isWeatherPopupOpen && !isBirthdayPopupOpen && ty <= 35)
  {
    if (tx >= 285)
    {
      tone(AZTOUCH_BEEPER, 880, 45);
      openInAppSetup();
      lastRenderedSec = 0;
      return;
    }
    else if (isBirthdayToday(rawlocal))
    {
      triggerBirthdayCelebration(true);
      lastRenderedSec = 0;
      freshTap = false;
    }
    else if (hasDestUtcOffset)
    {
      showDestLocalTime = !showDestLocalTime;
      tone(AZTOUCH_BEEPER, showDestLocalTime ? 1047 : 784, 45);
      displayCurrentTimeSprite(rawlocal);
    }
  }

  if (gScreensaverEnabled && (millis() - lastInteractionMillis >= SCREENSAVER_TIMEOUT_MS))
  {
    isWeatherPopupOpen = false;
    isBirthdayPopupOpen = false;
    inDimClockMode = false;
    inScreenSaver = true;
    updateScreenSaverEyes(true);
    yield();
    return;
  }

  if (freshTap && !inDimClockMode && !isBirthdayPopupOpen && handleWeatherForecastTouch(tx, ty))
  {
    tone(AZTOUCH_BEEPER, 880, 45);
    lastRenderedSec = 0;
    freshTap = false;
  }

  // Main countdown area touch handling (before arrival):
  // Toggle between countdown (DD.HH:MM:SS) and destination local time (HH:MM:SS in cyan)
  if (freshTap && !inDimClockMode && !isWeatherPopupOpen && !isBirthdayPopupOpen && !isArrived && hasDestUtcOffset &&
      ty >= COUNTDOWN_Y && ty <= (COUNTDOWN_Y + COUNTDOWN_H))
  {
    showCountdownAsLocalTime = !showCountdownAsLocalTime;
    tone(AZTOUCH_BEEPER, showCountdownAsLocalTime ? 1047 : 784, 45);
    lastRenderedSec = 0;
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
        isWeatherPopupOpen = false;
        isBirthdayPopupOpen = false;
        if (inDimClockMode)
        {
          wakeFromDimClockMode(true);
        }
        else
        {
          drawWhere(true);
          drawWeatherBadge();
        }
        playHolidayFanfare();
      }
    }

    int totalSecs = (int)diffSecs;
    int diffDays = totalSecs / 86400;
    int diffHours = (totalSecs % 86400) / 3600;
    int diffMins = (totalSecs % 3600) / 60;
    int remSecs = totalSecs % 60;

    if (!isWeatherPopupOpen && !isBirthdayPopupOpen)
    {
      drawCountdownLeft(diffDays, diffHours, diffMins, remSecs, blinkState, isArrived, rawlocal);
    }

    if (!inDimClockMode)
    {
      displayCurrentTimeSprite(rawlocal);

      if (rawlocal % 10 == 0)
      {
        drawWifiSignalBars(2, BOT_TEXT_Y + 1);
      }
    }
  }

  delay(25);
}
