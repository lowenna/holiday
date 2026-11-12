#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <XPT2046_Touchscreen.h>
#include "audio.h"
#include "maldives.h" // Hexdump of 320x240 JPG background

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

// NTP Timezone (UK GMT/BST)
const char *MY_TZ = "GMT0BST,M3.5.0/1,M10.5.0";

// ============================================================================
// Global Objects & State
// ============================================================================
XPT2046_Touchscreen touch(AZTOUCH_TOUCH_CS, AZTOUCH_TOUCH_IRQ);
TFT_eSPI tft = TFT_eSPI();

// Persistent sprites to avoid heap fragmentation and repeated JPG decoding
TFT_eSprite topTimeSprite(&tft);
TFT_eSprite countdownBgSprite(&tft);
TFT_eSprite countdownFgSprite(&tft);

Preferences prefs;
bool shouldSaveConfig = false;

// Default holiday configuration (overwritten by NVS if saved)
char gHour[3] = "10";
char gMins[3] = "00";
char gDay[3] = "05";
char gMonth[3] = "12";
char gYear[5] = "2026";
char gWhere[21] = "Maldives";

time_t rawhol = 0;

// ============================================================================
// Touch & Display Helpers
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

// Decoder callback for main background image
bool tftOutputMainBg(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= BG_IMAGE_MAX_H)
    return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// Decoder callback for caching the 320x48 background slice behind the countdown
bool tftOutputCountdownBg(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= COUNTDOWN_H)
    return false;
  countdownBgSprite.pushImage(x, y, w, h, bitmap);
  return true;
}

// Formats a time_t cleanly without ctime()'s trailing newline
void formatTimeNoNewline(time_t t, char *buf, size_t len)
{
  strlcpy(buf, ctime(&t), len);
  size_t slen = strlen(buf);
  if (slen > 0 && buf[slen - 1] == '\n')
  {
    buf[slen - 1] = '\0';
  }
}

void initSprites()
{
  topTimeSprite.setColorDepth(8);
  topTimeSprite.createSprite(SCREEN_W, TOP_BAR_H);

  // Cache the background image slice behind the countdown once
  countdownBgSprite.setColorDepth(16);
  countdownBgSprite.createSprite(SCREEN_W, COUNTDOWN_H);
  countdownBgSprite.setSwapBytes(true);
  countdownBgSprite.fillSprite(TFT_BLACK);
  TJpgDec.setCallback(tftOutputCountdownBg);
  TJpgDec.drawJpg(0, BG_IMAGE_Y - COUNTDOWN_Y, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));

  // Working foreground sprite for the countdown digits
  countdownFgSprite.setColorDepth(16);
  countdownFgSprite.createSprite(SCREEN_W, COUNTDOWN_H);
  countdownFgSprite.setSwapBytes(true);
}

void displayCurrentTimeSprite(time_t rawLocal)
{
  char timeStr[32];
  formatTimeNoNewline(rawLocal, timeStr, sizeof(timeStr));

  topTimeSprite.fillSprite(TFT_BLACK);
  topTimeSprite.setTextColor(TFT_GREEN, TFT_BLACK);
  topTimeSprite.drawCentreString(timeStr, SCREEN_W / 2, 0, 2);
  topTimeSprite.pushSprite(0, 0);
}

void displayLines()
{
  tft.drawLine(0, TOP_LINE_Y, SCREEN_W - 1, TOP_LINE_Y, TFT_WHITE);
  tft.drawLine(0, BOT_LINE_Y, SCREEN_W - 1, BOT_LINE_Y, TFT_WHITE);
}

void displaySSIDAndWhen()
{
  char holStr[32];
  formatTimeNoNewline(rawhol, holStr, sizeof(holStr));

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(WiFi.SSID(), 0, BOT_TEXT_Y, 2);

  tft.setTextDatum(TR_DATUM);
  tft.drawString(holStr, SCREEN_W - 1, BOT_TEXT_Y, 2);
}

void drawStaticBackgroundImage()
{
  TJpgDec.setCallback(tftOutputMainBg);
  TJpgDec.drawJpg(0, BG_IMAGE_Y, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));
}

void drawWhere()
{
  TFT_eSprite t(&tft);
  t.createSprite(SCREEN_W, WHERE_H);
  t.fillSprite(TFT_BLACK);
  t.setTextColor(TFT_WHITE);
  t.drawCentreString(gWhere, SCREEN_W / 2, 0, 4);
  t.pushSprite(0, WHERE_Y, TFT_BLACK);
  t.deleteSprite();
}

void drawCountdownLeft(int days, int hours, int mins, int secs, bool showColon)
{
  // Copy cached background slice directly into foreground sprite (fast, zero JPG decode)
  countdownBgSprite.pushToSprite(&countdownFgSprite, 0, 0);

  countdownFgSprite.setTextColor(TFT_DARKCYAN);
  countdownFgSprite.setTextSize(1);
  countdownFgSprite.setTextFont(7);

  char sep = showColon ? ':' : ' ';

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

bool areWeConfiguring()
{
  unsigned long startAt = millis();
  bool triggered = false;

  Serial.println("5 sec countdown: tap screen or press GPIO0 button to configure.");

  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.println("Tap the SCREEN (or rear button) within 5s to setup.");
  tft.println(" - Rear button is tiny, black-on-silver, next to USB.");
  tft.println(" - You will need a phone to complete setup...");

  // Board diagram
  tft.drawRect(50, 100, 220, 110, TFT_WHITE);
  tft.fillRect(51, 101, 218, 108, TFT_DARKGREEN);

  for (int x = 57; x < 270; x += 12)
  {
    tft.drawCircle(x, 107, 3, TFT_SILVER);
    tft.drawCircle(x, 203, 3, TFT_SILVER);
  }

  tft.fillRect(51, 120, 70, 70, TFT_DARKGREY);       // ESP32 module
  tft.fillRect(270 - 31, 140, 30, 30, TFT_DARKGREY); // USB power
  tft.fillRect(250, 180, 10, 10, TFT_DARKGREY);      // Other button
  tft.fillRect(250, 120, 10, 10, TFT_YELLOW);        // Boot/config button

  tft.setCursor(60, 147);
  tft.setTextColor(TFT_BLACK);
  tft.print("ESP32");
  tft.setCursor(242, 147);
  tft.print("USB");

  tft.setCursor(212, 120);
  tft.setTextColor(TFT_YELLOW);
  tft.print("Here!");
  tft.setCursor(0, 220);
  tft.print("(C) John Howard 2026");

  uint16_t tx, ty;
  while ((millis() - startAt < 5000) && !triggered)
  {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW || getTouchPixel(tx, ty))
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
  prefs.begin("hol", true);
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

void writePreferences(const String &hour, const String &mins, const String &day,
                      const String &month, const String &year, const String &where)
{
  int h = hour.toInt();
  int m = mins.toInt();
  int d = day.toInt();
  int mo = month.toInt();
  int y = year.toInt();

  if (!isValidDateTime(h, m, d, mo, y))
  {
    Serial.println("Invalid date/time entered; keeping previous preferences.");
    return;
  }

  prefs.begin("hol", false);
  prefs.putString("hour", hour);
  prefs.putString("mins", mins);
  prefs.putString("day", day);
  prefs.putString("month", month);
  prefs.putString("year", year);
  prefs.putString("where", where.length() > 0 ? where : "Holiday");
  prefs.end();
  delay(200);
  ESP.restart();
}
// ============================================================================
// Setup & Loop
// ============================================================================
void setup()
{
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
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
  bool configuring = areWeConfiguring();
  readPreferences();
  WiFiManagerParameter custom_hol_hour("hour", "Hour (0-23)", gHour, 2);
  WiFiManagerParameter custom_hol_mins("mins", "Mins (0-59)", gMins, 2);
  WiFiManagerParameter custom_hol_day("day", "Day (1-31)", gDay, 2);
  WiFiManagerParameter custom_hol_month("month", "Month (1-12)", gMonth, 2);
  WiFiManagerParameter custom_hol_year("year", "Year (YYYY)", gYear, 4);
  WiFiManagerParameter custom_hol_where("where", "Destination", gWhere, 20);
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setConfigPortalTimeout(180); // Prevent hanging forever if abandoned
  wifiManager.addParameter(&custom_hol_hour);
  wifiManager.addParameter(&custom_hol_mins);
  wifiManager.addParameter(&custom_hol_day);
  wifiManager.addParameter(&custom_hol_month);
  wifiManager.addParameter(&custom_hol_year);
  wifiManager.addParameter(&custom_hol_where);
  bool wmResult = false;
  if (configuring)
  {
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setCursor(0, 0);
    tft.setTextColor(TFT_WHITE);
    tft.println("Starting setup...");
    tft.println(" - On your phone, go to Settings > WiFi");
    tft.println(" - Connect to ESP_xxxxxxxxxx");
    tft.println(" - Wait a few seconds for captive portal");
    tft.println(" - Tap 'Configure WiFi' & select network");
    tft.println(" - Enter WiFi password & holiday details");
    tft.println(" - Tap 'Save' at the bottom");
    wmResult = wifiManager.startConfigPortal();
  }
  else
  {
    wmResult = wifiManager.autoConnect();
  }
  tft.fillScreen(TFT_BLACK);
  if (!wmResult)
  {
    tft.setCursor(0, 0);
    tft.setTextColor(TFT_RED);
    tft.println("Failed to connect to WiFi. Restarting...");
    Serial.println("Failed to connect and hit timeout");
    delay(5000);
    ESP.restart();
  }
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
  // Sync NTP time
  configTzTime(MY_TZ, "pool.ntp.org", "time.nist.gov");
  // Wait briefly for initial NTP sync so the first frame doesn't show 1970
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo, 500) && retry < 10)
  {
    Serial.println("Waiting for NTP time sync...");
    retry++;
  }
  // Calculate target holiday timestamp
  struct tm holtimeinfo = {};
  holtimeinfo.tm_hour = atoi(gHour);
  holtimeinfo.tm_min = atoi(gMins);
  holtimeinfo.tm_mday = atoi(gDay);
  holtimeinfo.tm_mon = atoi(gMonth) - 1;
  holtimeinfo.tm_year = atoi(gYear) - 1900;
  holtimeinfo.tm_isdst = -1;
  rawhol = mktime(&holtimeinfo);
  // Render static UI elements & initialize persistent sprites
  initSprites();
  displayLines();
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  drawWhere();
  Serial.println("setup() complete");
}
void loop()
{
  static time_t lastRenderedSec = 0;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100))
  {
    Serial.println("Failed to get localtime!");
    delay(500);
    return;
  }
  time_t rawlocal = mktime(&timeinfo);
  // Only redraw once per second when the clock ticks
  if (rawlocal != lastRenderedSec)
  {
    lastRenderedSec = rawlocal;
    double diffSecs = difftime(rawhol, rawlocal);
    if (diffSecs < 0)
      diffSecs = 0;
    int totalSecs = (int)diffSecs;
    int diffDays = totalSecs / 86400;
    int diffHours = (totalSecs % 86400) / 3600;
    int diffMins = (totalSecs % 3600) / 60;
    int remSecs = totalSecs % 60;
    bool showColon = (remSecs % 2) == 0;
    drawCountdownLeft(diffDays, diffHours, diffMins, remSecs, showColon);
    displayCurrentTimeSprite(rawlocal);
  }
  delay(50);
}