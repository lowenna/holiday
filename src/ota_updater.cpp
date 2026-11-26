#include "ota_updater.h"
#include "fw_version.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <TFT_eSPI.h>
#include <array>

extern TFT_eSPI tft;
static constexpr uint8_t UPD_BEEPER = 21;

extern bool otaInProgress;

extern void drawModernHeader(const char *title, const char *badgeText = nullptr);
extern void drawModernButton(int16_t x, int16_t y, int16_t w, int16_t h,
                             const String &label, uint16_t fillCol, uint16_t borderCol, uint16_t textCol = TFT_WHITE);
extern void drawOtaProgressScreen(const char *statusText, int percent);
extern void showScreenError(const char *title, const char *detail, bool fullScreen = true, unsigned long pauseMs = 3000);
extern bool getTouchPixel(uint16_t &x, uint16_t &y);

// Shared Dark-Slate UI Palette
static constexpr uint16_t UPD_BG         = 0x0000;
static constexpr uint16_t UPD_CARD       = 0x18E3;
static constexpr uint16_t UPD_BORDER     = 0x39E7;
static constexpr uint16_t UPD_ACCENT     = 0x067F;
static constexpr uint16_t UPD_ACCENT_DK  = 0x02B0;
static constexpr uint16_t UPD_SUCCESS    = 0x07E8;
static constexpr uint16_t UPD_SUCCESS_DK = 0x0386;
static constexpr uint16_t UPD_TEXT_MUTED = 0x9CD3;

static const char *VERSION_JSON_URL =
    "https://raw.githubusercontent.com/lowenna/holiday/refs/heads/master/firmware/version.json";

static String extractJsonString(const String &json, const String &key)
{
  String token = String('"') + key + String('"');
  int kPos = json.indexOf(token);
  if (kPos < 0)
    return "";
  int colonPos = json.indexOf(':', kPos + token.length());
  if (colonPos < 0)
    return "";
  int q1 = json.indexOf('"', colonPos + 1);
  if (q1 < 0)
    return "";
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0)
    return "";
  return json.substring(q1 + 1, q2);
}

static void performCloudOTA(const String &binUrl)
{
  otaInProgress = false; // Force drawOtaProgressScreen to draw header & card
  drawOtaProgressScreen("Connecting to GitHub...", 0);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (!http.begin(client, binUrl))
  {
    otaInProgress = false;
    showScreenError("UPDATE ERROR", "Could not connect to server.", true, 3000);
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    http.end();
    otaInProgress = false;
    String err = "Download failed (HTTP " + String(httpCode) + ")";
    showScreenError("UPDATE ERROR", err.c_str(), true, 3000);
    return;
  }

  int totalLen = http.getSize();
  if (totalLen <= 0)
  {
    http.end();
    otaInProgress = false;
    showScreenError("UPDATE ERROR", "Invalid firmware file size.", true, 3000);
    return;
  }

  if (!Update.begin((size_t)totalLen))
  {
    http.end();
    otaInProgress = false;
    showScreenError("UPDATE ERROR", "Not enough flash space.", true, 3000);
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  std::array<uint8_t, 512> buff;
  size_t written = 0;
  int lastPct = -1;
  unsigned long lastDataMs = millis();

  while (http.connected() && written < (size_t)totalLen)
  {
    size_t avail = stream->available();
    if (avail > 0)
    {
      size_t toRead = min(avail, buff.size());
      int c = stream->readBytes(buff.data(), toRead);
      if (c > 0)
      {
        if (Update.write(buff.data(), c) != (size_t)c)
        {
          break;
        }
        written += c;
        lastDataMs = millis();

        int pct = (int)((written * 100UL) / (size_t)totalLen);
        if (pct != lastPct)
        {
          lastPct = pct;
          drawOtaProgressScreen("Downloading firmware...", pct);
        }
      }
    }
    else
    {
      if (millis() - lastDataMs > 15000UL)
      {
        break; // Stall timeout
      }
      delay(5);
    }
  }

  http.end();

  if (written == (size_t)totalLen && Update.end(true))
  {
    drawOtaProgressScreen("Update complete! Rebooting...", 100);
    delay(1200);
    ESP.restart();
  }
  else
  {
    Update.abort();
    otaInProgress = false;
    showScreenError("OTA FAILED", "Firmware download incomplete.", true, 3000);
  }
}

void checkAndPromptFirmwareUpdate()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  Serial.printf("[OTA] Current version: %s. Checking GitHub...\n", FW_VERSION);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(5000);

  if (!http.begin(client, VERSION_JSON_URL))
    return;

  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("[OTA] Version check HTTP %d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  String remoteVer = extractJsonString(payload, "version");
  String binUrl = extractJsonString(payload, "url");

  if (remoteVer.length() == 0 || binUrl.length() == 0)
    return;

  Serial.printf("[OTA] Remote version: %s\n", remoteVer.c_str());

  // Fixed-width YYYY.MM.DD.HHMM strings compare lexicographically
  if (remoteVer <= String(FW_VERSION))
  {
    Serial.println("[OTA] Firmware is up to date.");
    return;
  }

  // Newer firmware found! Show sleek prompt screen
  drawModernHeader("Firmware Update");

  tft.fillRoundRect(10, 38, 300, 142, 8, UPD_CARD);
  tft.drawRoundRect(10, 38, 300, 142, 8, UPD_ACCENT_DK);

  tft.setFreeFont(nullptr);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(UPD_ACCENT, UPD_CARD);
  tft.drawString("A newer version is available!", 160, 58, 2);

  tft.drawFastHLine(24, 76, 272, UPD_BORDER);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(UPD_TEXT_MUTED, UPD_CARD);
  tft.drawString("Installed:", 28, 90, 2);
  tft.setTextColor(TFT_WHITE, UPD_CARD);
  tft.drawString("v" + String(FW_VERSION), 118, 90, 2);

  tft.setTextColor(UPD_TEXT_MUTED, UPD_CARD);
  tft.drawString("Available:", 28, 118, 2);
  tft.setTextColor(UPD_SUCCESS, UPD_CARD);
  tft.drawString("v" + remoteVer, 118, 118, 2);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(UPD_TEXT_MUTED, UPD_CARD);
  tft.drawString("Install now over Wi-Fi?", 160, 154, 2);
  tft.setTextDatum(TL_DATUM);

  // Bottom Action Bar (y = 197..231)
  tft.drawFastHLine(10, 189, 300, UPD_BORDER);
  const int16_t skipX = 10;
  const int16_t instX = 166;
  const int16_t btnY = 197;
  const int16_t btnW = 144;
  const int16_t btnH = 34;

  drawModernButton(skipX, btnY, btnW, btnH, "Skip (15s)", UPD_CARD, UPD_BORDER, UPD_TEXT_MUTED);
  drawModernButton(instX, btnY, btnW, btnH, "Install Now", UPD_SUCCESS_DK, UPD_SUCCESS, TFT_WHITE);

  unsigned long promptStart = millis();
  int lastSec = -1;
  bool wasTouching = true;
  uint16_t tx, ty;

  while (millis() - promptStart < 15000UL)
  {
    int secsLeft = max(1, 15 - (int)((millis() - promptStart) / 1000));
    if (secsLeft != lastSec)
    {
      lastSec = secsLeft;
      drawModernButton(skipX, btnY, btnW, btnH, "Skip (" + String(secsLeft) + "s)", UPD_CARD, UPD_BORDER, UPD_TEXT_MUTED);
    }

    bool touching = getTouchPixel(tx, ty);
    if (touching && !wasTouching)
    {
      if (tx >= skipX && tx <= (skipX + btnW) && ty >= btnY && ty <= (btnY + btnH))
      {
tone(UPD_BEEPER, 880, 60);
        break;
      }
      if (tx >= instX && tx <= (instX + btnW) && ty >= btnY && ty <= (btnY + btnH))
      {
tone(UPD_BEEPER, 880, 60);
        performCloudOTA(binUrl);
        break;
      }
    }
    wasTouching = touching;
    delay(25);
  }

  tft.fillScreen(UPD_BG);
}

void triggerCloudOTAFromWeb(const String &binUrl)
{
  if (binUrl.length() == 0)
    return;
  performCloudOTA(binUrl);
}

String fetchRemoteVersionJson()
{
  if (WiFi.status() != WL_CONNECTED)
    return "";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(6000);

  String checkUrl = String(VERSION_JSON_URL) + "?t=" + String(esp_random());
  if (!http.begin(client, checkUrl))
    return "";

  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    http.end();
    return "";
  }

  String payload = http.getString();
  http.end();
  return payload;
}
