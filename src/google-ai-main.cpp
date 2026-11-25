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
 You've been helping me with this project. I'm not including all files, but the main
 ones for now can be at

 https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/src/google-ai-main.cpp
 https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/src/weather.cpp
 https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/src/audio.cpp
 https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/src/eyes.cpp
 https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/include/maldives.h
 https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/platformio.ini
 
 
  Can you confirm you can see these files. If so, please confirm what they do so I can be sure
 you can help me update it. 
 
 The target device is an ESP32 AZ-Delivery DevKit V4 with a 2.8" AZ-Touch MOD display. 
 
 The project is a holiday countdown timer with OTA firmware update, weather info, 
 and birthday celebration features.
 */

char otaHostname[24];
WebServer otaServer(80);
bool otaInProgress = false;

extern void updateScreenSaverEyes(bool);                          // eyes.cpp
extern bool handleScreenSaverTap();                               // eyes.cpp
extern void releaseCrabSprite();                                  // eyes.cpp
extern void drawScreenSaverClock(int days, int hours, int mins, int secs, bool showColon, bool isArrived, time_t rawlocal, int driftOffset); // eyes.cpp
extern int getScreenSaverDriftOffset();                           // eyes.cpp
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
constexpr int16_t TOP_BAR_H = 24; // Full 24px black gap above photo (y = 0..23)
constexpr int16_t TOP_LINE_Y = 24;
constexpr int16_t BG_IMAGE_Y = 24;
constexpr int16_t BG_IMAGE_MAX_H = 209;
constexpr int16_t COUNTDOWN_Y = 158;
constexpr int16_t COUNTDOWN_H = 48;
constexpr int16_t BOT_LINE_Y = 216;
constexpr int16_t BOT_TEXT_Y = 216; // Full 24px black gap below photo (y = 216..239)

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
bool gRainAsPercent = false;
int gBdayMonth = 1;
int gBdayDay = 1;
int gLastCelebratedBdayYear = -1;
bool isBirthdayPopupOpen = false;
unsigned long birthdayPopupOpenedMs = 0;
bool triggerTestBirthday = false;
bool triggerTestScreensaver = false;
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
void drawOtaProgressScreen(const char *statusText, int percent);

void handleOTAUpload()
{
  static size_t otaExpectedSize = 0;
  static int lastOtaPct = -1;

  HTTPUpload &upload = otaServer.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    Serial.printf("OTA Update Start: %s\n", upload.filename.c_str());
    otaInProgress = false; // Ensure drawOtaProgressScreen initializes the header & card
    lastOtaPct = -1;
    otaExpectedSize = otaServer.hasHeader("Content-Length")
                          ? (size_t)otaServer.header("Content-Length").toInt()
                          : 0;

    drawOtaProgressScreen("Uploading firmware...", 0);
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
    else if (otaExpectedSize > 0)
    {
      int pct = (int)min(99UL, (unsigned long)((upload.totalSize * 100UL) / otaExpectedSize));
      if (pct != lastOtaPct)
      {
        lastOtaPct = pct;
        drawOtaProgressScreen("Flashing firmware...", pct);
      }
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (Update.end(true))
    {
      Serial.printf("OTA Update Success: %u bytes\n", upload.totalSize);
      drawOtaProgressScreen("Update complete! Rebooting...", 100);
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
    #otaStatus, #bdayStatus, #ssaverStatus { font-size: 13px; color: #aaa; margin-top: 8px; text-align: center; }
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

      <label class="field">Birthday (Day &amp; Month)</label>
      <input type="date" name="bday" value=")rawliteral" + String(bdayBuf) + R"rawliteral(">

      <div class="grid">
        <label class="chk"><input type="checkbox" name="ssaver" value="1" )rawliteral" + String(gScreensaverEnabled ? "checked" : "") + R"rawliteral(>Screensaver</label>
        <label class="chk"><input type="checkbox" name="dim" value="1" )rawliteral" + String(gDimmerEnabled ? "checked" : "") + R"rawliteral(>Auto-Dim</label>
        <label class="chk"><input type="checkbox" name="wxen" value="1" )rawliteral" + String(gWeatherEnabled ? "checked" : "") + R"rawliteral(>Weather</label>
        <label class="chk"><input type="checkbox" name="isl1h" value="1" )rawliteral" + String(gIslandPlusOne ? "checked" : "") + R"rawliteral(>Resort Time +1h</label>
        <label class="chk"><input type="checkbox" name="bdayen" value="1" )rawliteral" + String(gBirthdayEnabled ? "checked" : "") + R"rawliteral(>Birthday</label>
        <label class="chk"><input type="checkbox" name="rainpct" value="1" )rawliteral" + String(gRainAsPercent ? "checked" : "") + R"rawliteral(>Rain Chance (%)</label>
      </div>

      <button type="submit">Save &amp; Restart</button>
    </form>
    <button type="button" class="test-btn" id="testBdayBtn">Preview Birthday Screen</button>
    <div id="bdayStatus"></div>
    <button type="button" class="test-btn" id="testSsaverBtn" style="background:#00695c;">Preview Screensaver</button>
    <div id="ssaverStatus"></div>
  </div>
  <div class="card">
    <h2>Software Update</h2>
    <p style="font-size:13px;color:#aaa;margin-top:0">Device address: <b>)rawliteral" + String(otaHostname) + R"rawliteral(.local</b></p>
    <form id="otaForm">
      <input type="file" id="fwFile" name="update" accept=".bin">
      <button type="submit" class="ota-btn" id="uploadBtn">Install Update</button>
    </form>
    <div id="progWrap"><div id="progBar"></div></div>
    <div id="otaStatus"></div>
  </div>

  <script>
    document.getElementById('testBdayBtn').addEventListener('click', function() {
      const bStatus = document.getElementById('bdayStatus');
      bStatus.textContent = 'Showing birthday preview on screen...';
      fetch('/testbday', { method: 'POST' })
        .then(function(r) { return r.text(); })
        .then(function(txt) { bStatus.textContent = txt; })
        .catch(function() { bStatus.textContent = 'Triggered!'; });
    });

    document.getElementById('testSsaverBtn').addEventListener('click', function() {
      const sStatus = document.getElementById('ssaverStatus');
      sStatus.textContent = 'Starting hermit crab screensaver...';
      fetch('/testssaver', { method: 'POST' })
        .then(function(r) { return r.text(); })
        .then(function(txt) { sStatus.textContent = txt; })
        .catch(function() { sStatus.textContent = 'Triggered!'; });
    });

    const fwInput = document.getElementById('fwFile');
    const statusEl = document.getElementById('otaStatus');
    const progWrap = document.getElementById('progWrap');
    const progBar = document.getElementById('progBar');

    document.getElementById('otaForm').addEventListener('submit', function(e) {
      e.preventDefault();
      if (!fwInput.files || fwInput.files.length === 0) {
        statusEl.textContent = 'Please select an update file (.bin) first.';
        return;
      }
      const formData = new FormData();
      formData.append('update', fwInput.files[0]);

      progWrap.style.display = 'block';
      progBar.style.width = '0%';
      statusEl.textContent = 'Uploading update...';
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
          statusEl.textContent = 'Update complete! Restarting device...';
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

  // Trigger an immediate Screensaver test on the display
  otaServer.on("/testssaver", HTTP_POST, []()
               {
    triggerTestScreensaver = true;
    otaServer.send(200, "text/plain", "Screensaver triggered!"); });

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
    gRainAsPercent = otaServer.hasArg("rainpct");

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
    prefs.putBool("rainpct", gRainAsPercent);
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
  Serial.printf("Setup at: http://%s.local (IP: %s)\n", otaHostname, WiFi.localIP().toString().c_str());
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
    tft.drawString("UPDATE", SCREEN_W / 2, 13, 2);
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
  // 1. Draw 8 thick rectangular teeth around the perimeter (outer radius ~6.5px)
  for (int i = 0; i < 8; i++)
  {
    float rad = (i * 45.0f) * 0.0174533f;
    int tx = cx + (int)roundf(cosf(rad) * 5.2f);
    int ty = cy + (int)roundf(sinf(rad) * 5.2f);
    spr.fillRect(tx - 1, ty - 1, 3, 3, col);
  }

  // 2. Main gear wheel body & inner rim highlight
  spr.fillCircle(cx, cy, 5, col);
  spr.drawCircle(cx, cy, 4, TFT_DARKGREY);

  // 3. Hollow centre axle hole
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
  const int16_t popW = 288;
  const int16_t popH = 172;
  const int16_t popX = (SCREEN_W - popW) / 2;
  const int16_t popY = 34;

  // Deep midnight-navy card with double gold & magenta festive border
  tft.fillRoundRect(popX, popY, popW, popH, 12, TFT_NAVY);
  tft.drawRoundRect(popX, popY, popW, popH, 12, TFT_GOLD);
  tft.drawRoundRect(popX + 2, popY + 2, popW - 4, popH - 4, 10, TFT_MAGENTA);

  // Decorative confetti & sparkles around the edges (keeping the centre text zone clear)
  const uint16_t confettiCols[] = {TFT_GOLD, TFT_MAGENTA, TFT_CYAN, TFT_GREEN, TFT_ORANGE, TFT_YELLOW};
  for (int i = 0; i < 34; i++)
  {
    int cx = popX + 12 + ((i * 61) % (popW - 24));
    int cy = popY + 10 + ((i * 43) % (popH - 20));

    // Skip the central text zone so lettering stays crisp
    if (cx > popX + 48 && cx < popX + popW - 48 && cy > popY + 66 && cy < popY + 144)
      continue;

    uint16_t col = confettiCols[i % 6];
    if (i % 3 == 0)
    {
      // Tiny diamond / streamer
      tft.fillRect(cx, cy, 4, 2, col);
    }
    else if (i % 3 == 1)
    {
      // 4-point sparkle
      tft.drawPixel(cx, cy, TFT_WHITE);
      tft.drawLine(cx - 2, cy, cx + 2, cy, col);
      tft.drawLine(cx, cy - 2, cx, cy + 2, col);
    }
    else
    {
      tft.fillCircle(cx, cy, 2, col);
    }
  }

  // Left & Right Glossy Balloons with knots and wavy strings
  const int bx[] = {popX + 46, popX + popW - 46};
  const int by[] = {popY + 34, popY + 34};
  const uint16_t bCol[] = {TFT_RED, TFT_CYAN};

  for (int i = 0; i < 2; i++)
  {
    int x = bx[i];
    int y = by[i];

    // Balloon body & subtle border
    tft.fillCircle(x, y, 16, bCol[i]);
    tft.drawCircle(x, y, 16, TFT_WHITE);

    // Glossy shine highlight on upper-left of balloon
    tft.fillCircle(x - 5, y - 6, 3, TFT_WHITE);

    // Balloon knot triangle
    tft.fillTriangle(x - 3, y + 16, x + 3, y + 16, x, y + 20, bCol[i]);

    // Gentle S-curve string
    tft.drawLine(x, y + 20, x - 3, y + 28, TFT_SILVER);
    tft.drawLine(x - 3, y + 28, x + 2, y + 36, TFT_SILVER);
    tft.drawLine(x + 2, y + 36, x - 1, y + 44, TFT_SILVER);
  }

  // Centre Birthday Cake & Lit Candle (between the two balloons)
  int cakeCx = popX + (popW / 2);
  int cakeBaseY = popY + 58;

  // Cake plate
  tft.fillRoundRect(cakeCx - 32, cakeBaseY, 64, 4, 2, TFT_SILVER);

  // Bottom tier (strawberry pink/magenta with white icing trim)
  tft.fillRoundRect(cakeCx - 26, cakeBaseY - 14, 52, 14, 3, TFT_MAGENTA);
  tft.fillRect(cakeCx - 26, cakeBaseY - 14, 52, 4, TFT_WHITE);

  // Top tier (golden sponge with cyan icing trim)
  tft.fillRoundRect(cakeCx - 18, cakeBaseY - 26, 36, 12, 3, TFT_ORANGE);
  tft.fillRect(cakeCx - 18, cakeBaseY - 26, 36, 3, TFT_CYAN);

  // Candle & glowing flame
  tft.fillRect(cakeCx - 2, cakeBaseY - 38, 4, 12, TFT_WHITE);
  tft.drawFastHLine(cakeCx - 2, cakeBaseY - 34, 4, TFT_RED);
  tft.drawFastHLine(cakeCx - 2, cakeBaseY - 30, 4, TFT_RED);

  // Outer orange glow + inner bright yellow flame
  tft.fillCircle(cakeCx, cakeBaseY - 43, 4, TFT_ORANGE);
  tft.fillTriangle(cakeCx - 3, cakeBaseY - 42, cakeCx + 3, cakeBaseY - 42, cakeCx, cakeBaseY - 50, TFT_YELLOW);
  tft.fillCircle(cakeCx, cakeBaseY - 43, 2, TFT_WHITE);

  // Bold Celebration Text with subtle drop shadow
  tft.setFreeFont(nullptr);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_BLACK, TFT_NAVY);
  tft.drawString("HAPPY", cakeCx + 2, popY + 86, 4);
  tft.setTextColor(TFT_GOLD, TFT_NAVY);
  tft.drawString("HAPPY", cakeCx, popY + 84, 4);

  tft.setTextColor(TFT_BLACK, TFT_NAVY);
  tft.drawString("BIRTHDAY!", cakeCx + 2, popY + 118, 4);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("BIRTHDAY!", cakeCx, popY + 116, 4);

  // Subtle footer hint
  tft.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
  tft.drawString("Tap anywhere to close", cakeCx, popY + popH - 13, 1);
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
  topTimeSprite.setTextDatum(MC_DATUM);

  int centerX = (SCREEN_W - 20) / 2;
  int centerY = TOP_BAR_H / 2;

  // On your birthday, alternate "HAPPY BIRTHDAY!" every 4 seconds with the live clock
  if (isBirthdayToday(rawLocal) && ((rawLocal / 4) % 2 == 1))
  {
    topTimeSprite.setTextColor(TFT_GOLD, TFT_BLACK);
    topTimeSprite.drawString("HAPPY BIRTHDAY!", centerX, centerY, 2);
  }
  else if (showDestLocalTime && hasDestUtcOffset)
  {
    time_t destEpoch = rawLocal + getEffectiveDestOffsetSec();
    struct tm destTm;
    gmtime_r(&destEpoch, &destTm);

    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    int dDay = destTm.tm_mday;
    const char *dSuffix = "th";
    if (dDay % 100 < 11 || dDay % 100 > 13)
    {
      if (dDay % 10 == 1)
        dSuffix = "st";
      else if (dDay % 10 == 2)
        dSuffix = "nd";
      else if (dDay % 10 == 3)
        dSuffix = "rd";
    }

    String timeStr = String(gWhere) + " " + String(days[destTm.tm_wday]) + " " +
                     String(dDay) + dSuffix + " " + String(months[destTm.tm_mon]) + " " +
                     (destTm.tm_hour < 10 ? "0" : "") + String(destTm.tm_hour) + ":" +
                     (destTm.tm_min < 10 ? "0" : "") + String(destTm.tm_min) + ":" +
                     (destTm.tm_sec < 10 ? "0" : "") + String(destTm.tm_sec);

    topTimeSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    topTimeSprite.drawString(timeStr, centerX, centerY, 2);
  }
  else
  {
    topTimeSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    topTimeSprite.drawString(formatPrettyDateTime(rawLocal, true), centerX, centerY, 2);
  }

  topTimeSprite.setTextDatum(TL_DATUM);

  // Realistic settings gear aligned vertically with the centred text
  drawSettingsGearIcon(topTimeSprite, SCREEN_W - 11, centerY, TFT_SILVER);

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


// ============================================================================
// Unified Modern UI Design System (Settings, Web Info & Wi-Fi Portal)
// ============================================================================
constexpr uint16_t UI_BG         = 0x0000; // Deep black canvas
constexpr uint16_t UI_CARD       = 0x18E3; // Elevated dark slate card
constexpr uint16_t UI_CARD_ALT   = 0x10A2; // Subtle recessed container
constexpr uint16_t UI_BORDER     = 0x39E7; // Crisp slate hairline border
constexpr uint16_t UI_ACCENT     = 0x067F; // Modern sky-cyan accent
constexpr uint16_t UI_ACCENT_DK  = 0x02B0; // Deep cyan button fill
constexpr uint16_t UI_SUCCESS    = 0x07E8; // Vibrant mint-green
constexpr uint16_t UI_SUCCESS_DK = 0x0386; // Deep emerald button fill
constexpr uint16_t UI_DANGER     = 0xF986; // Soft coral-red
constexpr uint16_t UI_DANGER_DK  = 0x6000; // Deep crimson button fill
constexpr uint16_t UI_TEXT_MUTED = 0x9CD3; // Muted cool grey for secondary labels

bool gMdnsOk = false;

void drawModernHeader(const char *title, const char *badgeText = nullptr)
{
  (void)badgeText; // Unused; kept in signature so existing callers need no changes
  tft.fillScreen(UI_BG);
  tft.fillRect(0, 0, SCREEN_W, 28, UI_CARD);
  tft.drawFastHLine(0, 28, SCREEN_W, UI_BORDER);

  // Left cyan accent bar
  tft.fillRoundRect(8, 6, 4, 16, 2, UI_ACCENT);

  tft.setFreeFont(nullptr);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, UI_CARD);
  tft.drawString(title, 18, 14, 2);
  tft.setTextDatum(TL_DATUM);
}

void drawModernButton(int16_t x, int16_t y, int16_t w, int16_t h,
                      const String &label, uint16_t fillCol, uint16_t borderCol, uint16_t textCol = TFT_WHITE)
{
  tft.fillRoundRect(x, y, w, h, 7, fillCol);
  tft.drawRoundRect(x, y, w, h, 7, borderCol);
  tft.setFreeFont(nullptr);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(textCol, fillCol);
  tft.drawString(label, x + (w / 2), y + (h / 2), 2);
  tft.setTextDatum(TL_DATUM);
}
void drawToggleTile(int16_t tileX, int16_t tileY, int16_t tileW, const char *label, bool isEnabled,
                    const char *onText = "Active", const char *offText = "Off", bool isModeSelector = false)
{
  const int16_t tileH = 44;

  uint16_t cardBorder = isModeSelector ? UI_ACCENT_DK : (isEnabled ? UI_SUCCESS_DK : UI_BORDER);
  tft.fillRoundRect(tileX, tileY, tileW, tileH, 8, UI_CARD);
  tft.drawRoundRect(tileX, tileY, tileW, tileH, 8, cardBorder);

  // Feature title (top-left)
  tft.setFreeFont(nullptr);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, UI_CARD);
  tft.drawString(label, tileX + 10, tileY + 5, 2);

  // State sub-label (bottom-left)
  uint16_t stateCol = isModeSelector ? UI_ACCENT : (isEnabled ? UI_SUCCESS : UI_TEXT_MUTED);
  tft.setTextColor(stateCol, UI_CARD);
  tft.drawString(isEnabled ? onText : offText, tileX + 10, tileY + 24, 1);

  // Modern iOS-style pill toggle switch (bottom-right: 40x18)
  const int16_t swW = 40;
  const int16_t swH = 18;
  const int16_t swX = tileX + tileW - swW - 9;
  const int16_t swY = tileY + 20;

  uint16_t trackCol  = isModeSelector ? UI_ACCENT_DK : (isEnabled ? UI_SUCCESS_DK : UI_CARD_ALT);
  uint16_t trackEdge = isModeSelector ? UI_ACCENT    : (isEnabled ? UI_SUCCESS    : UI_BORDER);

  tft.fillRoundRect(swX, swY, swW, swH, swH / 2, trackCol);
  tft.drawRoundRect(swX, swY, swW, swH, swH / 2, trackEdge);

  int16_t knobR = 6;
  int16_t knobY = swY + (swH / 2);
  int16_t knobX = isEnabled ? (swX + swW - knobR - 3) : (swX + knobR + 3);
  tft.fillCircle(knobX, knobY, knobR, TFT_WHITE);
}

void showWebUpdateInfoScreen(bool mdnsOk)
{
  drawModernHeader("Web Settings & Update");

  // Concise single-line subtitle that fits comfortably within 320px
  tft.setFreeFont(nullptr);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(UI_TEXT_MUTED, UI_BG);
  tft.drawString("On a phone or PC, browse to one of the below :", 12, 36, 2);

  // Option A: Primary .local mDNS Address (y = 60..116)
  tft.fillRoundRect(10, 60, SCREEN_W - 20, 56, 8, UI_CARD);
  tft.drawRoundRect(10, 60, SCREEN_W - 20, 56, 8, mdnsOk ? UI_ACCENT_DK : UI_BORDER);

  tft.fillCircle(28, 88, 10, mdnsOk ? UI_ACCENT_DK : UI_CARD_ALT);
  tft.drawCircle(28, 88, 10, mdnsOk ? UI_ACCENT : UI_BORDER);
  tft.fillCircle(28, 88, 3, mdnsOk ? UI_ACCENT : UI_TEXT_MUTED);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(UI_TEXT_MUTED, UI_CARD);
  tft.drawString("BY DEVICE NAME", 48, 68, 1);

  if (mdnsOk)
  {
    String hostUrl = "http://" + String(otaHostname) + ".local";
    tft.setTextColor(UI_ACCENT, UI_CARD);
    tft.drawString(hostUrl, 48, 85, 2);
  }
  else
  {
    tft.setTextColor(TFT_SILVER, UI_CARD);
    tft.drawString("mDNS unavailable - use IP below", 48, 85, 2);
  }

  // Option B: Direct IP Address (y = 124..180)
  tft.fillRoundRect(10, 124, SCREEN_W - 20, 56, 8, UI_CARD);
  tft.drawRoundRect(10, 124, SCREEN_W - 20, 56, 8, UI_ACCENT_DK);

  tft.fillCircle(28, 152, 10, UI_ACCENT_DK);
  tft.drawCircle(28, 152, 10, UI_ACCENT);
  tft.fillCircle(28, 152, 3, UI_ACCENT);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(UI_TEXT_MUTED, UI_CARD);
  tft.drawString("BY IP ADDRESS", 48, 132, 1);

  String ipUrl = "http://" + WiFi.localIP().toString();
  tft.setTextColor(UI_ACCENT, UI_CARD);
  tft.drawString(ipUrl, 48, 149, 2);

  // Shared Bottom Action Bar (y = 197..231) - Right-aligned "Back" button matching Wi-Fi screen
  tft.drawFastHLine(10, 189, SCREEN_W - 20, UI_BORDER);
  const int16_t backX = 224;
  const int16_t backY = 197;
  const int16_t backW = 86;
  const int16_t backH = 34;
  drawModernButton(backX, backY, backW, backH, "Back", UI_ACCENT_DK, UI_ACCENT, TFT_WHITE);

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
        tone(AZTOUCH_BEEPER, 880, 60);
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
  drawModernHeader("Settings & Options", "v2026");

  // 2x3 Elevated Toggle Cards (y = 36..180)
  drawToggleTile(col1X, row1Y, tileW, "Screensaver", gScreensaverEnabled);
  drawToggleTile(col2X, row1Y, tileW, "Auto-Dimmer", gDimmerEnabled);

  drawToggleTile(col1X, row2Y, tileW, "Weather", gWeatherEnabled);
  drawToggleTile(col2X, row2Y, tileW, "Resort +1hr", gIslandPlusOne);

  drawToggleTile(col1X, row3Y, tileW, "Birthday", gBirthdayEnabled);
  drawToggleTile(col2X, row3Y, tileW, "Rain Units", gRainAsPercent, "Percent (%)", "Depth (mm)", true);

  // Subtle divider above bottom action bar
  tft.drawFastHLine(10, 189, SCREEN_W - 20, UI_BORDER);

  // Bottom 3-Button Action Bar (y = 197..231)
  drawModernButton(setupX, btnY, btnW, btnH, "Wi-Fi", UI_CARD, UI_ACCENT, UI_ACCENT);
  drawModernButton(updateX, btnY, btnW, btnH, "Web Setup", UI_CARD, UI_ACCENT, TFT_WHITE);
  drawModernButton(startX, btnY, btnW, btnH, "Start (10s)", UI_SUCCESS_DK, UI_SUCCESS, TFT_WHITE);
}

bool areWeConfiguring(bool mdnsOk)
{
  unsigned long startAt = millis();
  bool triggered = false;
  bool exitNow = false;
  int lastSecsLeft = -1;
  bool wasTouching = true;

  const int16_t col1X = 10;
  const int16_t col2X = 164;
  const int16_t tileW = 146;
  const int16_t row1Y = 36;
  const int16_t row2Y = 86;
  const int16_t row3Y = 136;
  const int16_t tileH = 44;

  const int16_t btnY = 197;
  const int16_t btnH = 34;
  const int16_t btnW = 95;
  const int16_t setupX = 10;
  const int16_t updateX = 112;
  const int16_t startX = 215;

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
      drawModernButton(startX, btnY, btnW, btnH, "Start (" + String(secsLeft) + "s)", UI_SUCCESS_DK, UI_SUCCESS, TFT_WHITE);
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
      // Row 1 Right: Auto-Dimmer
      else if (tx >= col2X && tx <= (col2X + tileW) && ty >= row1Y && ty <= (row1Y + tileH))
      {
        gDimmerEnabled = !gDimmerEnabled;
        prefs.begin("hol", false);
        prefs.putBool("dim", gDimmerEnabled);
        prefs.end();

        drawToggleTile(col2X, row1Y, tileW, "Auto-Dimmer", gDimmerEnabled);
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
      // Row 2 Right: Resort +1hr
      else if (tx >= col2X && tx <= (col2X + tileW) && ty >= row2Y && ty <= (row2Y + tileH))
      {
        gIslandPlusOne = !gIslandPlusOne;
        prefs.begin("hol", false);
        prefs.putBool("isl1h", gIslandPlusOne);
        prefs.end();

        drawToggleTile(col2X, row2Y, tileW, "Resort +1hr", gIslandPlusOne);
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
      // Row 3 Right: Rain Units (% vs mm)
      else if (tx >= col2X && tx <= (col2X + tileW) && ty >= row3Y && ty <= (row3Y + tileH))
      {
        gRainAsPercent = !gRainAsPercent;
        prefs.begin("hol", false);
        prefs.putBool("rainpct", gRainAsPercent);
        prefs.end();

        drawToggleTile(col2X, row3Y, tileW, "Rain Units", gRainAsPercent, "Percent (%)", "Depth (mm)", true);
        tone(AZTOUCH_BEEPER, gRainAsPercent ? 880 : 440, 80);

        startAt = millis();
        lastSecsLeft = -1;
      }
      // Button 1: Wi-Fi
      else if (tx >= setupX && tx <= (setupX + btnW) && ty >= btnY && ty <= (btnY + btnH))
      {
        tone(AZTOUCH_BEEPER, 880, 60);
        triggered = true;
      }
      // Button 2: Web Setup
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
      // Button 3: Start
      else if (tx >= startX && tx <= (startX + btnW) && ty >= btnY && ty <= (btnY + btnH))
      {
        tone(AZTOUCH_BEEPER, 880, 60);
        exitNow = true;
      }
    }
    wasTouching = touching;
    delay(25);
  }

  tft.fillScreen(TFT_BLACK);
  return triggered;
}


void displayLines()
{
  return; // Removed this
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
  const uint8_t textAlpha = 190;
  const uint8_t shadowAlpha = 110;

  // 1. Decode the exact Maldives photo slice into a single temporary background sprite
  TFT_eSprite bg(&tft);
  bg.setColorDepth(16);
  if (bg.createSprite(SCREEN_W, bannerH) == nullptr)
  {
    Serial.println("[ERR] drawWhere bg sprite alloc failed");
    return;
  }
  bg.setSwapBytes(true);
  bg.fillSprite(TFT_BLACK);

  gWhereBgSpritePtr = &bg;
  TJpgDec.setCallback(tftOutputWhereBg);
  TJpgDec.drawJpg(0, BG_IMAGE_Y - WHERE_Y, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));
  gWhereBgSpritePtr = nullptr;

  // 2. Reuse pre-allocated countdownFgSprite (320x48) for text rendering to avoid heap fragmentation
  countdownFgSprite.fillSprite(TFT_BLACK);

  String displayText = isArrived ? (String(gWhere) + "!") : String(gWhere);
  countdownFgSprite.setTextDatum(MC_DATUM);

  uint16_t mainCol = isArrived ? TFT_GOLD : TFT_WHITE;
  uint16_t shadowCol = TFT_NAVY;

#ifdef SMOOTH_FONT
  countdownFgSprite.loadFont(NotoSansBold36);
  countdownFgSprite.setTextColor(shadowCol, TFT_BLACK, false);
  countdownFgSprite.drawString(displayText, (SCREEN_W / 2) + 2, 22);
  countdownFgSprite.setTextColor(mainCol, TFT_BLACK, false);
  countdownFgSprite.drawString(displayText, SCREEN_W / 2, 20);
  countdownFgSprite.unloadFont();
#else
  countdownFgSprite.setFreeFont(isArrived ? &FreeSansBold12pt7b : &FreeSansBold18pt7b);
  countdownFgSprite.setTextColor(shadowCol);
  countdownFgSprite.drawString(displayText, (SCREEN_W / 2) + 2, 22);
  countdownFgSprite.setTextColor(mainCol);
  countdownFgSprite.drawString(displayText, SCREEN_W / 2, 20);
#endif
  countdownFgSprite.setTextDatum(TL_DATUM);

  // 3. Alpha-blend non-black text/shadow pixels over the Maldives photo pixels
  for (int16_t y = 0; y < bannerH; y++)
  {
    for (int16_t x = 0; x < SCREEN_W; x++)
    {
      uint16_t fgPix = countdownFgSprite.readPixel(x, y);
      if (fgPix != TFT_BLACK)
      {
        uint16_t bgPix = bg.readPixel(x, y);
        uint8_t alpha = (fgPix == shadowCol) ? shadowAlpha : textAlpha;
        bg.drawPixel(x, y, tft.alphaBlend(alpha, fgPix, bgPix));
      }
    }
  }

  // 4. Push the blended result and free bg immediately
  bg.pushSprite(0, WHERE_Y);
  bg.deleteSprite();
}

constexpr unsigned long BL_CLOCK_ONLY_DELAY_MS = 300000UL; // 5 mins now - 30s after max dim, hide everything except the clock
bool inDimClockMode = false;

void drawCountdownLeft(int days, int hours, int mins, int secs, bool showColon, bool isArrived, time_t rawlocal)
{
  const uint8_t digitAlpha = 195; // 0 = invisible, 255 = solid (180-210 blends nicely with the water)

  // Always draw the 7-segment digits onto a clean black canvas first
  countdownFgSprite.fillSprite(TFT_BLACK);
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

  // In normal mode, alpha-blend the digits over the pre-decoded Maldives water slice
  if (!inDimClockMode)
  {
    for (int16_t y = 0; y < COUNTDOWN_H; y++)
    {
      for (int16_t x = 0; x < SCREEN_W; x++)
      {
        uint16_t bgPix = countdownBgSprite.readPixel(x, y);
        uint16_t fgPix = countdownFgSprite.readPixel(x, y);
        if (fgPix != TFT_BLACK)
        {
          countdownFgSprite.drawPixel(x, y, tft.alphaBlend(digitAlpha, fgPix, bgPix));
        }
        else
        {
          countdownFgSprite.drawPixel(x, y, bgPix);
        }
      }
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
  gRainAsPercent = prefs.getBool("rainpct", false);
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

// ============================================================================
// Minimal Boot Status Screen (Wi-Fi & NTP Sync with Icons + Progressive Dots)
// ============================================================================
enum BootStepState
{
  BOOT_STEP_PENDING,
  BOOT_STEP_ACTIVE,
  BOOT_STEP_WARN,
  BOOT_STEP_DONE
};

void drawBootWifiIcon(int cx, int cy, uint16_t col)
{
  tft.fillRect(cx - 18, cy - 18, 36, 36, TFT_BLACK);
  // Concentric Wi-Fi signal arcs facing upwards (225 deg to 315 deg)
  tft.drawArc(cx, cy + 10, 22, 19, 135, 225, col, TFT_BLACK);
  tft.drawArc(cx, cy + 10, 15, 12, 135, 225, col, TFT_BLACK);
  tft.drawArc(cx, cy + 10, 8, 5, 135, 225, col, TFT_BLACK);
  tft.fillCircle(cx, cy + 10, 3, col);
}

void drawBootClockIcon(int cx, int cy, uint16_t col)
{
  tft.fillRect(cx - 18, cy - 18, 36, 36, TFT_BLACK);
  tft.drawCircle(cx, cy, 15, col);
  tft.drawCircle(cx, cy, 14, col);
  // Clock hands (10:10 position)
  tft.drawLine(cx, cy, cx - 6, cy - 6, col);
  tft.drawLine(cx + 1, cy, cx - 5, cy - 6, col);
  tft.drawLine(cx, cy, cx + 8, cy - 7, col);
  tft.drawLine(cx, cy + 1, cx + 8, cy - 6, col);
  tft.fillCircle(cx, cy, 2, col);
}

void updateBootStepRow(int rowIndex, BootStepState state, const String &line1, const String &line2 = "")
{
  const int rowY = (rowIndex == 0) ? 58 : 138;
  const int iconX = 48;
  const int textX = 84;

  uint16_t iconCol = TFT_DARKGREY;
  uint16_t titleCol = TFT_SILVER;
  uint16_t subCol = TFT_LIGHTGREY;

  if (state == BOOT_STEP_ACTIVE)
  {
    iconCol = TFT_CYAN;
    titleCol = TFT_WHITE;
    subCol = TFT_CYAN;
  }
  else if (state == BOOT_STEP_WARN)
  {
    iconCol = TFT_YELLOW;
    titleCol = TFT_YELLOW;
    subCol = TFT_WHITE;
  }
  else if (state == BOOT_STEP_DONE)
  {
    iconCol = TFT_GREEN;
    titleCol = TFT_GREEN;
    subCol = TFT_GREEN;
  }

  // Clear text area for clean redraws
  tft.fillRect(textX - 4, rowY - 22, SCREEN_W - textX, 48, TFT_BLACK);

  if (rowIndex == 0)
  {
    drawBootWifiIcon(iconX, rowY, iconCol);
  }
  else
  {
    drawBootClockIcon(iconX, rowY, iconCol);
  }

  tft.setFreeFont(nullptr);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);

  if (line2.length() > 0)
  {
    tft.setTextColor(titleCol, TFT_BLACK);
    tft.drawString(line1, textX, rowY - 17, 2);
    tft.setTextColor(subCol, TFT_BLACK);
    tft.drawString(line2, textX, rowY + 3, 2);
  }
  else
  {
    tft.setTextColor(titleCol, TFT_BLACK);
    tft.drawString(line1, textX, rowY - 8, 2);
  }
}

void drawBootWelcomeScreen(const String &savedSsid)
{
  tft.fillScreen(TFT_BLACK);

  if (savedSsid.length() > 0)
  {
    updateBootStepRow(0, BOOT_STEP_ACTIVE, "Connecting to Wi-Fi", savedSsid + ".");
  }
  else
  {
    updateBootStepRow(0, BOOT_STEP_WARN, "No saved Wi-Fi", "Starting " + String(otaHostname) + "...");
  }

  updateBootStepRow(1, BOOT_STEP_PENDING, "Waiting for network...", "Time sync pending");

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Touch & hold screen for Settings", SCREEN_W / 2, 224, 1);
  tft.setTextDatum(TL_DATUM);
}

void drawPortalStepRow(int stepNum, int y, const char *title, const char *highlight, const char *suffix = "")
{
  // Circular step badge
  tft.fillCircle(26, y + 10, 10, UI_ACCENT_DK);
  tft.drawCircle(26, y + 10, 10, UI_ACCENT);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, UI_ACCENT_DK);
  tft.drawString(String(stepNum), 26, y + 10, 2);

  // Step instruction text
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, UI_CARD);
  tft.setCursor(44, y + 2);
  tft.print(title);

  if (highlight != nullptr && highlight[0] != '\0')
  {
    tft.setTextColor(UI_ACCENT, UI_CARD);
    tft.print(highlight);
  }
  if (suffix != nullptr && suffix[0] != '\0')
  {
    tft.setTextColor(TFT_WHITE, UI_CARD);
    tft.print(suffix);
  }
}

void drawSetupPortalScreen(const String &apName, bool showTimeoutHint)
{
  drawModernHeader("Wi-Fi Setup Hotspot", "192.168.4.1");

  // Elevated Step-by-Step Guide Card (y = 36..180)
  tft.fillRoundRect(10, 36, SCREEN_W - 20, 144, 8, UI_CARD);
  tft.drawRoundRect(10, 36, SCREEN_W - 20, 144, 8, UI_BORDER);

  tft.setFreeFont(nullptr);
  tft.setTextFont(2);
  tft.setTextSize(1);

  // Step 1
  drawPortalStepRow(1, 46, "Join Wi-Fi: ", apName.c_str());
  tft.setTextColor(UI_TEXT_MUTED, UI_CARD);
  tft.drawString("Open Wi-Fi settings on your phone or PC", 44, 66, 1);

  tft.drawFastHLine(20, 82, SCREEN_W - 40, UI_BORDER);

  // Step 2
  drawPortalStepRow(2, 90, "Open ", "192.168.4.1", " in browser");
  tft.setTextColor(UI_TEXT_MUTED, UI_CARD);
  tft.drawString("Appears automatically on most phones", 44, 110, 1);

  tft.drawFastHLine(20, 126, SCREEN_W - 40, UI_BORDER);

  // Step 3
  drawPortalStepRow(3, 134, "Tap ", "Configure WiFi", " & Save");
  tft.setTextColor(UI_TEXT_MUTED, UI_CARD);
  tft.drawString("Select your home network & enter password", 44, 154, 1);

  // Shared Bottom Action Bar (y = 197..231)
  tft.drawFastHLine(10, 189, SCREEN_W - 20, UI_BORDER);

  // Left status pill for live countdown timer
  tft.fillRoundRect(10, 197, 206, 34, 7, UI_CARD);
  tft.drawRoundRect(10, 197, 206, 34, 7, UI_BORDER);
  if (showTimeoutHint)
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(UI_TEXT_MUTED, UI_CARD);
    tft.drawString("Timeout: 10:00", 113, 214, 2);
    tft.setTextDatum(TL_DATUM);
  }

  // Right tactile Back button (matches Web Settings & Update screen)
  drawModernButton(224, 197, 86, 34, "Back", UI_ACCENT_DK, UI_ACCENT, TFT_WHITE);
}

bool runNonBlockingPortal(WiFiManager &wm, unsigned long durationMs = 600000UL, bool *wasCancelled = nullptr)
{
  if (wasCancelled != nullptr)
  {
    *wasCancelled = false;
  }

  bool timedOut = false;
  unsigned long portalStartMs = millis();
  int lastSecShown = -1;
  bool wasTouching = true;

  while (wm.getConfigPortalActive())
  {
    wm.process();

    // Check for Cancel button tap (x = 224..310, y = 197..231)
    uint16_t tx, ty;
    bool touching = getTouchPixel(tx, ty);
    if (touching && !wasTouching)
    {
      if (tx >= 224 && tx <= 310 && ty >= 197 && ty <= 231)
      {
        tone(AZTOUCH_BEEPER, 880, 60);
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

      char timerBuf[28];
      snprintf(timerBuf, sizeof(timerBuf), "Expires in %02d:%02d", mins, secs);

      // Update cleanly inside the left bottom timer pill
      tft.fillRoundRect(12, 199, 202, 30, 5, UI_CARD);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(secsLeft <= 30 ? UI_DANGER : UI_TEXT_MUTED, UI_CARD);
      tft.drawString(timerBuf, 113, 214, 2);
      tft.setTextDatum(TL_DATUM);
    }

    delay(10);
  }

  return timedOut;
}


void configModeCallback(WiFiManager *myWiFiManager)
{
  String apName = myWiFiManager->getConfigPortalSSID();
  Serial.printf("WiFi connect failed; entered AP mode: %s\n", apName.c_str());
  drawSetupPortalScreen(apName, false);
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

  // Confirm save visually using the unified dark-slate card design
  drawModernHeader("Settings Saved");

  tft.fillRoundRect(12, 44, SCREEN_W - 24, 148, 8, UI_CARD);
  tft.drawRoundRect(12, 44, SCREEN_W - 24, 148, 8, UI_SUCCESS_DK);

  // Green checkmark badge
  int16_t cx = SCREEN_W / 2;
  int16_t cy = 76;
  tft.fillCircle(cx, cy, 16, UI_SUCCESS_DK);
  tft.drawCircle(cx, cy, 16, UI_SUCCESS);
  tft.drawLine(cx - 7, cy, cx - 2, cy + 5, TFT_WHITE);
  tft.drawLine(cx - 6, cy, cx - 2, cy + 4, TFT_WHITE);
  tft.drawLine(cx - 2, cy + 5, cx + 8, cy - 6, TFT_WHITE);
  tft.drawLine(cx - 2, cy + 4, cx + 8, cy - 7, TFT_WHITE);

  tft.setFreeFont(nullptr);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(UI_SUCCESS, UI_CARD);
  tft.drawString("Preferences Saved!", cx, 108, 2);

  String destLabel = where.length() > 0 ? where : "Holiday";
  String summary = destLabel + " - " + day + "/" + month + "/" + year + " " + hour + ":" + mins;
  tft.setTextColor(TFT_WHITE, UI_CARD);
  tft.drawString(summary, cx, 134, 2);

  tft.setTextColor(UI_TEXT_MUTED, UI_BG);
  tft.drawString("Restarting device...", cx, 214, 2);
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
  const int16_t barH = SCREEN_H - BOT_TEXT_Y;
  const int16_t midY = BOT_TEXT_Y + (barH / 2);

  tft.fillRect(0, BOT_TEXT_Y, SCREEN_W, barH, TFT_BLACK);
  drawWifiSignalBars(2, midY - 7);

  tft.setFreeFont(nullptr);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(ML_DATUM);

  if (gWeatherEnabled && weatherTempStr.length() > 0)
  {
    int x = 23;
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(weatherTempStr, x, midY, 2);

    int tempW = tft.textWidth(weatherTempStr, 2);
    tft.drawCircle(x + tempW + 2, midY - 4, 2, TFT_CYAN);

    // Keep "C Partly cloudy" in TFT_CYAN to match the temperature and degree symbol
    tft.drawString(weatherCondStr, x + tempW + 6, midY, 2);
  }
  else
  {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(WiFi.SSID(), 23, midY, 2);
  }

  // Show target holiday date/time before departure, or "Day X of Holiday" once arrived
  time_t nowEpoch = time(nullptr);
  if (nowEpoch > 1700000000L)
  {
    double elapsedSecs = difftime(nowEpoch, rawhol);
    if (elapsedSecs < 0)
    {
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.setTextDatum(MR_DATUM);
      tft.drawString(formatPrettyDateTime(rawhol), SCREEN_W - 2, midY, 2);
    }
    else
    {
      int holidayDayNum = ((int)elapsedSecs / 86400) + 1;
      String dayLabel = "Day " + String(holidayDayNum); 
      tft.setTextColor(TFT_GOLD, TFT_BLACK);
      tft.setTextDatum(MR_DATUM);
      tft.drawString(dayLabel, SCREEN_W - 2, midY, 2);
    }
  }
  tft.setTextDatum(TL_DATUM);
}
void wakeFromScreenSaver(bool isArrived)
{
  inScreenSaver = false;
  releaseCrabSprite();
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
  tone(AZTOUCH_BEEPER, 880, 60);

  bool wasWeatherEnabled = gWeatherEnabled;
  bool wasIslandPlusOne = gIslandPlusOne;

  // Check if mDNS is active
  while (areWeConfiguring(gMdnsOk))
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
  // 1. Wake display & turn on backlight FIRST for instant visual feedback
  tft.init();
  initBacklightPWM();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);

  initHostname();
  WiFi.mode(WIFI_STA);
  String savedSsid = getSavedWifiSSID();
  drawBootWelcomeScreen(savedSsid);

  // 2. Initialize audio pin silently (no startup chime)
  LcarsAudio::begin(AZTOUCH_BEEPER);

  Serial.begin(115200);
  touch.begin();
  touch.setRotation(1);

  // 3. Load & validate saved holiday preferences
  readPreferences();

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

  // 4. Connect to saved Wi-Fi with live animated progressive dots (12s timeout)
  bool wifiConnected = false;
  if (savedSsid.length() > 0)
  {
    WiFi.setHostname(otaHostname);
    WiFi.begin(); // Uses credentials saved in ESP32 NVS

    unsigned long wifiStartMs = millis();
    unsigned long lastDotMs = 0;
    int dotStep = 0;

    while (WiFi.status() != WL_CONNECTED && (millis() - wifiStartMs < 12000UL))
    {
      unsigned long now = millis();
      if (now - lastDotMs >= 300UL)
      {
        lastDotMs = now;
        dotStep = (dotStep % 3) + 1;
        String dots = (dotStep == 1) ? "." : ((dotStep == 2) ? ".." : "...");
        updateBootStepRow(0, BOOT_STEP_ACTIVE, "Connecting to Wi-Fi" + dots, savedSsid);
      }
      delay(20);
    }
    wifiConnected = (WiFi.status() == WL_CONNECTED);
  }

  // Fall back to WiFiManager AP hotspot if no saved Wi-Fi or connection failed
  if (!wifiConnected)
  {
    Serial.println("Saved Wi-Fi failed or missing; starting setup hotspot...");
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_AP_STA);

    if (!wifiManager.getConfigPortalActive())
    {
      wifiManager.startConfigPortal(otaHostname);
    }

    bool portalCancelled = false;
    bool timedOut = runNonBlockingPortal(wifiManager, 600000UL, &portalCancelled);
    wifiConnected = (WiFi.status() == WL_CONNECTED);

    if (!wifiConnected)
    {
      showScreenError("WI-FI ERROR",
                      timedOut ? "Setup portal timed out.\nRestarting in 5 seconds..."
                               : "Wi-Fi setup cancelled.\nRestarting in 5 seconds...",
                      true, 5000);
      ESP.restart();
    }
  }

  wifiManager.setConfigPortalBlocking(true);
  // Immediately turn Wi-Fi icon & text GREEN upon connection
  updateBootStepRow(0, BOOT_STEP_DONE, "Connected to Wi-Fi", WiFi.SSID());

  // 5. Start OTA; only show the 10s Settings screen if unconfigured or user holds the screen
  gMdnsOk = setupOTA();
  bool mdnsOk = gMdnsOk;

  bool portalRan = false;
  bool portalTimedOut = false;

  uint16_t bootTx, bootTy;
  bool userHeldScreen = getTouchPixel(bootTx, bootTy);

  if (userHeldScreen || gPrefStatus != PREF_VALID)
  {
    if (userHeldScreen)
    {
      tone(AZTOUCH_BEEPER, 880, 60);
    }

    while (areWeConfiguring(mdnsOk))
    {
      portalRan = true;
      otaServer.stop();

      String apName = String(otaHostname);
      drawSetupPortalScreen(apName, false);

      wifiManager.setConfigPortalBlocking(false);
      wifiManager.startConfigPortal(apName.c_str());

      bool portalCancelled = false;
      portalTimedOut = runNonBlockingPortal(wifiManager, 600000UL, &portalCancelled);

      wifiManager.setConfigPortalBlocking(true);
      otaServer.begin();

      if (portalCancelled)
      {
        portalRan = false;
        continue;
      }
      break;
    }

    // Re-draw minimal boot screen with green Wi-Fi row before starting NTP sync
    tft.fillScreen(TFT_BLACK);
    updateBootStepRow(0, BOOT_STEP_DONE, "Connected to Wi-Fi", WiFi.SSID());
  }
  else
  {
    Serial.println("Preferences valid & no touch held; skipping 10s boot config screen.");
  }

  // 6. Save updated holiday details if changed OR if portal was submitted before timing out
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

  // 7. Sync NTP time with smooth 300ms animated progressive dots
  configTzTime(MY_TZ, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  unsigned long ntpStartMs = millis();
  unsigned long lastNtpDotMs = 0;
  int ntpDotStep = 0;
  bool timeSynced = false;

  while (millis() - ntpStartMs < 6000UL)
  {
    if (getLocalTime(&timeinfo, 50))
    {
      timeSynced = true;
      break;
    }
    unsigned long now = millis();
    if (now - lastNtpDotMs >= 300UL)
    {
      lastNtpDotMs = now;
      ntpDotStep = (ntpDotStep % 3) + 1;
      String dots = (ntpDotStep == 1) ? "." : ((ntpDotStep == 2) ? ".." : "...");
      updateBootStepRow(1, BOOT_STEP_ACTIVE, "Getting Internet time" + dots, "Syncing UK clock (GMT/BST)");
    }
  }

  if (!timeSynced)
  {
    updateBootStepRow(1, BOOT_STEP_WARN, "Time sync delayed", "Retrying in background...");
    delay(1500);
  }
  else
  {
    String timeReadyStr = "Synced: " +
                          String(timeinfo.tm_hour < 10 ? "0" : "") + String(timeinfo.tm_hour) + ":" +
                          String(timeinfo.tm_min < 10 ? "0" : "") + String(timeinfo.tm_min) + ":" +
                          String(timeinfo.tm_sec < 10 ? "0" : "") + String(timeinfo.tm_sec);
    updateBootStepRow(1, BOOT_STEP_DONE, "Internet time synced", timeReadyStr);
    delay(650); // Brief pause so both green icons are clearly visible
  }

  tft.fillScreen(TFT_BLACK);

  // 8. Calculate target holiday timestamp
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

  // 9. Render static UI elements & persistent sprites immediately
  initSprites();
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  displayLines();
  drawWhere(false);

  lastWeatherFetchMs = millis() - WEATHER_INTERVAL_MS;
  lastInteractionMillis = millis();
  lastBacklightBoostMs = millis();

  Serial.println("setup() complete");
}
void loop()
{
  static time_t lastRenderedSec = 0;
  static bool blinkState = false;
  static int lastHolidayDayShown = -1;

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

  // Handle web dashboard "Test Screensaver" button
  if (triggerTestScreensaver)
  {
    triggerTestScreensaver = false;
    isWeatherPopupOpen = false;
    isBirthdayPopupOpen = false;
    inDimClockMode = false;
    inScreenSaver = true;
    updateScreenSaverEyes(true);
    lastRenderedSec = 0; // Force immediate LED clock draw on screensaver start
    yield();
    return;
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
      // Tapping the 7-segment LED clock in the night sky toggles countdown vs local time
      if (ty <= 78 && !isArrived && hasDestUtcOffset)
      {
        showCountdownAsLocalTime = !showCountdownAsLocalTime;
        tone(AZTOUCH_BEEPER, showCountdownAsLocalTime ? 1047 : 784, 45);
        lastRenderedSec = 0; // Force immediate clock redraw below
      }
      else if (handleScreenSaverTap())
      {
        tone(AZTOUCH_BEEPER, 1047, 50);
        wakeFromScreenSaver(isArrived);
        lastRenderedSec = 0;
      }
      else
      {
        // Cute startled "bloop" chirp when crab ducks into its shell
        tone(AZTOUCH_BEEPER, 520, 45);
        delay(50);
        tone(AZTOUCH_BEEPER, 330, 70);
        updateScreenSaverEyes(false);
        yield();
        return;
      }
    }
    else
    {
      updateScreenSaverEyes(false);
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
  // 1. Larger top-right gear icon zone (x >= 255, y <= 40) opens setup
  // 2. On your birthday, tapping the top bar replays the celebration
  // 3. Otherwise, rest of top bar (x < 255, y <= 40) toggles UK / Destination time
  if (freshTap && !inScreenSaver && !inDimClockMode && !isWeatherPopupOpen && !isBirthdayPopupOpen && ty <= 40)
  {
    if (tx >= 255)
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

  if (gScreensaverEnabled && !inScreenSaver && (millis() - lastInteractionMillis >= SCREENSAVER_TIMEOUT_MS))
  {
    isWeatherPopupOpen = false;
    isBirthdayPopupOpen = false;
    inDimClockMode = false;
    inScreenSaver = true;
    updateScreenSaverEyes(true);
    lastRenderedSec = 0;
    yield();
    return;
  }

  if (freshTap && !inScreenSaver && !inDimClockMode && !isBirthdayPopupOpen && handleWeatherForecastTouch(tx, ty))
  {
    tone(AZTOUCH_BEEPER, 880, 45);
    lastRenderedSec = 0;
    freshTap = false;
  }

  // Main countdown area touch handling (before arrival):
  // Toggle between countdown (DD.HH:MM:SS) and destination local time (HH:MM:SS in cyan)
  if (freshTap && !inScreenSaver && !inDimClockMode && !isWeatherPopupOpen && !isBirthdayPopupOpen && !isArrived && hasDestUtcOffset &&
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

      // Refresh the bottom-right "Day X of Holiday" label whenever the day number changes
      int currentHolidayDay = ((int)difftime(rawlocal, rawhol) / 86400) + 1;
      if (currentHolidayDay != lastHolidayDayShown)
      {
        lastHolidayDayShown = currentHolidayDay;
        if (!inDimClockMode && !inScreenSaver && !isWeatherPopupOpen && !isBirthdayPopupOpen)
        {
          displaySSIDAndWhen();
        }
      }

      if (!holidayCelebrated)
      {
        holidayCelebrated = true;
        isWeatherPopupOpen = false;
        isBirthdayPopupOpen = false;
        if (inDimClockMode)
        {
          wakeFromDimClockMode(true);
        }
        else if (inScreenSaver)
        {
          wakeFromScreenSaver(true);
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
      if (inScreenSaver)
      {
        drawScreenSaverClock(diffDays, diffHours, diffMins, remSecs, blinkState, isArrived, rawlocal, getScreenSaverDriftOffset());
      }
      else
      {
        drawCountdownLeft(diffDays, diffHours, diffMins, remSecs, blinkState, isArrived, rawlocal);
      }
    }

    if (!inDimClockMode && !inScreenSaver)
    {
      displayCurrentTimeSprite(rawlocal);

      if (rawlocal % 10 == 0)
      {
        drawWifiSignalBars(2, 221);
      }
    }
  }
  delay(25);
}
