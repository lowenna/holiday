#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <array>

extern TFT_eSPI tft;
extern char gWhere[];
extern bool gWeatherEnabled;
extern bool gIslandPlusOne;
extern bool gRainAsPercent;
extern void fillSpriteWithMaldivesSlice(TFT_eSprite &spr, int16_t screenY, int16_t sliceH);
extern void displaySSIDAndWhen();
extern void drawStaticBackgroundImage();
extern void drawWhere(bool isArrived);

int16_t WHERE_Y = 40;
int16_t WHERE_H = 26;

static constexpr int16_t WX_SCREEN_W = 320;
static constexpr int16_t HOURLY_Y = 91;
static constexpr int16_t HOURLY_H = 61;

String weatherTempStr = "";
String weatherCondStr = "";
long destUtcOffsetSec = 0;
bool hasDestUtcOffset = false;
float cachedLat = 0.0f;
float cachedLon = 0.0f;
bool hasCachedCoords = false;
unsigned long lastWeatherFetchMs = 0;
unsigned long WEATHER_INTERVAL_MS = 1800000UL;

struct ForecastSlot
{
    String label;
    int tempC;
    int wmoCode;
    bool isDay;
    float precipMm;
    int precipProb;
    bool valid;
};

std::array<ForecastSlot, 24> hourlyForecast;
std::array<ForecastSlot, 16> dailyForecast;
int hourlyForecastCount = 0;
int dailyForecastCount = 0;
int forecastViewMode = 0;

// Page 1 Popup Metrics
int weatherFeelsLikeC = 0;
int weatherHumidityPct = 0;
int weatherWindKmh = 0;
int weatherWindDirDeg = 0;
float weatherUvMax = 0.0f;
String weatherSunriseStr = "--:--";
String weatherSunsetStr = "--:--";

// Page 2 Popup Metrics (Extended & Marine)
int weatherWindGustsKmh = 0;
int weatherCloudCoverPct = 0;
int weatherPressureHpa = 0;
float weatherSeaTempC = 0.0f;
float weatherWaveHeightM = 0.0f;
bool hasMarineData = false;

bool isWeatherPopupOpen = false;
int weatherPopupPage = 0; // 0 = Current & Sun, 1 = Marine & Atmosphere
unsigned long weatherPopupOpenedMs = 0;

String degToCompass(int deg)
{
    const char *dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int idx = (int)roundf(((deg % 360) / 45.0f)) % 8;
    if (idx < 0) idx += 8;
    return String(dirs[idx]);
}

String calcDaylightDuration(const String &rise, const String &set)
{
    if (rise.length() < 5 || set.length() < 5 || rise.startsWith("--") || set.startsWith("--"))
        return "--h --m";
    int rMins = rise.substring(0, 2).toInt() * 60 + rise.substring(3, 5).toInt();
    int sMins = set.substring(0, 2).toInt() * 60 + set.substring(3, 5).toInt();
    int diff = sMins - rMins;
    if (diff < 0) diff += 1440;
    return String(diff / 60) + "h " + String(diff % 60) + "m";
}


String wmoCodeToText(int code, bool isDay)
{
    if (code == 0)
        return isDay ? "Sunny" : "Clear";
    if (code == 1)
        return isDay ? "Mainly Sunny" : "Mainly Clear";
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
    String token = String(char(34)) + key + String(char(34)) + ":";
    int kPos = json.indexOf(token);
    if (kPos < 0)
        return false;
    int start = kPos + token.length();
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

int extractJsonNumberArray(const String &json, const String &key, float *outArr, int maxCount)
{
    String token = String(char(34)) + key + String(char(34)) + ":[";
    int kPos = json.indexOf(token);
    if (kPos < 0)
        return 0;
    int pos = kPos + token.length();
    int endBracket = json.indexOf(']', pos);
    if (endBracket < 0)
        return 0;

    int count = 0;
    while (pos < endBracket && count < maxCount)
    {
        while (pos < endBracket && (json[pos] == ' ' || json[pos] == ','))
            pos++;
        int numEnd = pos;
        while (numEnd < endBracket && (isdigit(json[numEnd]) || json[numEnd] == '.' || json[numEnd] == '-'))
            numEnd++;
        if (numEnd > pos)
        {
            outArr[count++] = json.substring(pos, numEnd).toFloat();
            pos = numEnd;
        }
        else
        {
            break;
        }
    }
    return count;
}

int extractJsonHourArray(const String &json, const String &key, int *outHours, int maxCount)
{
    String token = String(char(34)) + key + String(char(34)) + ":[";
    int kPos = json.indexOf(token);
    if (kPos < 0)
        return 0;
    int pos = kPos + token.length();
    int endBracket = json.indexOf(']', pos);
    if (endBracket < 0)
        return 0;

    int count = 0;
    while (pos < endBracket && count < maxCount)
    {
        int tPos = json.indexOf('T', pos);
        if (tPos < 0 || tPos >= endBracket)
            break;
        outHours[count++] = json.substring(tPos + 1, tPos + 3).toInt();
        int nextQuote = json.indexOf(char(34), tPos);
        if (nextQuote < 0 || nextQuote >= endBracket)
            break;
        pos = nextQuote + 1;
    }
    return count;
}

// Parse 3-letter weekday names ("Mon", "Tue", ...) from ISO dates like "2026-11-16"
int extractJsonDayNameArray(const String &json, const String &key, String *outNames, int maxCount)
{
    const char *dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    String token = String(char(34)) + key + String(char(34)) + ":[";
    int kPos = json.indexOf(token);
    if (kPos < 0)
        return 0;
    int pos = kPos + token.length();
    int endBracket = json.indexOf(']', pos);
    if (endBracket < 0)
        return 0;

    int count = 0;
    while (pos < endBracket && count < maxCount)
    {
        int q1 = json.indexOf(char(34), pos);
        if (q1 < 0 || q1 >= endBracket)
            break;
        int q2 = json.indexOf(char(34), q1 + 1);
        if (q2 < 0 || q2 >= endBracket)
            break;

        String dateStr = json.substring(q1 + 1, q2); // YYYY-MM-DD
        if (dateStr.length() >= 10)
        {
            struct tm tmDate = {};
            tmDate.tm_year = dateStr.substring(0, 4).toInt() - 1900;
            tmDate.tm_mon = dateStr.substring(5, 7).toInt() - 1;
            tmDate.tm_mday = dateStr.substring(8, 10).toInt();
            tmDate.tm_hour = 12;
            mktime(&tmDate);
            int wday = constrain(tmDate.tm_wday, 0, 6);
            outNames[count++] = String(dayNames[wday]);
        }
        pos = q2 + 1;
    }
    return count;
}
String extractJsonFirstTimeHHMM(const String &json, const String &key, bool plusOneHour)
{
    String token = String(char(34)) + key + String(char(34)) + ":[";
    int kPos = json.indexOf(token);
    if (kPos < 0)
        return "--:--";
    int tPos = json.indexOf('T', kPos + token.length());
    if (tPos < 0 || tPos + 5 >= (int)json.length())
        return "--:--";

    int hr = json.substring(tPos + 1, tPos + 3).toInt();
    int mn = json.substring(tPos + 4, tPos + 6).toInt();
    if (plusOneHour)
    {
        hr = (hr + 1) % 24;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hr, mn);
    return String(buf);
}

void drawMiniWeatherIcon(TFT_eSprite &spr, int cx, int cy, int code, bool isDay)
{
    if (code == 0 || code == 1)
    {
        if (isDay)
        {
            spr.fillCircle(cx, cy, 4, TFT_YELLOW);
            for (int a = 0; a < 360; a += 45)
            {
                float rad = a * 0.0174533f;
                int x1 = cx + (int)roundf(cosf(rad) * 5.5f);
                int y1 = cy + (int)roundf(sinf(rad) * 5.5f);
                int x2 = cx + (int)roundf(cosf(rad) * 7.2f);
                int y2 = cy + (int)roundf(sinf(rad) * 7.2f);
                spr.drawLine(x1, y1, x2, y2, TFT_GOLD);
            }
        }
        else
        {
            spr.fillCircle(cx, cy, 4, TFT_YELLOW);
            spr.fillCircle(cx + 2, cy - 1, 3, TFT_NAVY);
        }
        return;
    }

    if (code == 2)
    {
        if (isDay)
            spr.fillCircle(cx - 3, cy - 2, 3, TFT_YELLOW);
        else
            spr.fillCircle(cx - 3, cy - 2, 3, TFT_LIGHTGREY);
    }

    uint16_t cloudCol = (code >= 51) ? TFT_LIGHTGREY : TFT_WHITE;
    spr.fillCircle(cx - 3, cy + 1, 3, cloudCol);
    spr.fillCircle(cx + 1, cy - 1, 4, cloudCol);
    spr.fillCircle(cx + 4, cy + 1, 3, cloudCol);
    spr.fillRect(cx - 3, cy + 1, 8, 4, cloudCol);

    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
    {
        spr.drawLine(cx - 2, cy + 6, cx - 3, cy + 8, TFT_CYAN);
        spr.drawLine(cx + 1, cy + 6, cx, cy + 8, TFT_CYAN);
        spr.drawLine(cx + 4, cy + 6, cx + 3, cy + 8, TFT_CYAN);
    }
    else if (code >= 71 && code <= 77)
    {
        spr.drawPixel(cx - 2, cy + 7, TFT_WHITE);
        spr.drawPixel(cx + 1, cy + 8, TFT_WHITE);
        spr.drawPixel(cx + 4, cy + 7, TFT_WHITE);
    }
    else if (code >= 95)
    {
        spr.drawLine(cx + 1, cy + 5, cx - 1, cy + 7, TFT_YELLOW);
        spr.drawLine(cx - 1, cy + 7, cx + 2, cy + 7, TFT_YELLOW);
        spr.drawLine(cx + 2, cy + 7, cx, cy + 9, TFT_YELLOW);
    }
}

void drawHourlyForecastBar()
{
    if (!gWeatherEnabled)
        return;

    bool isDaily = (forecastViewMode == 3 || forecastViewMode == 4) && (dailyForecastCount > 0);
    if (!isDaily && hourlyForecastCount == 0)
        return;

    int startIndex = 0;
    if (forecastViewMode == 1 || forecastViewMode == 4)
        startIndex = 8;
    else if (forecastViewMode == 2)
        startIndex = 16;

    const uint8_t cardBgAlpha = 135;
    const uint8_t detailAlpha = 225;

    TFT_eSprite hSpr(&tft);
    hSpr.setColorDepth(16);
    if (hSpr.createSprite(WX_SCREEN_W, HOURLY_H) == nullptr)
    {
        Serial.println("[WX] hSpr alloc failed");
        return;
    }
    fillSpriteWithMaldivesSlice(hSpr, HOURLY_Y, HOURLY_H);

    TFT_eSprite fgSpr(&tft);
    fgSpr.setColorDepth(8);
    if (fgSpr.createSprite(WX_SCREEN_W, HOURLY_H) == nullptr)
    {
        hSpr.deleteSprite();
        Serial.println("[WX] fgSpr alloc failed");
        return;
    }
    fgSpr.fillSprite(TFT_BLACK);

    const int slotW = 38;
    const int startX = 8;
    const int boxH = 51;

    uint16_t borderCol = TFT_DARKCYAN;
    uint16_t labelCol = TFT_CYAN;
    uint16_t activeDotCol = TFT_CYAN;

    if (forecastViewMode == 1)
    {
        borderCol = TFT_BLUE;
        labelCol = TFT_SKYBLUE;
        activeDotCol = TFT_SKYBLUE;
    }
    else if (forecastViewMode == 2)
    {
        borderCol = TFT_PURPLE;
        labelCol = TFT_MAGENTA;
        activeDotCol = TFT_MAGENTA;
    }
    else if (forecastViewMode == 3)
    {
        borderCol = TFT_GOLD;
        labelCol = TFT_YELLOW;
        activeDotCol = TFT_GOLD;
    }
    else if (forecastViewMode == 4)
    {
        borderCol = TFT_DARKGREEN;
        labelCol = TFT_GREEN;
        activeDotCol = TFT_GREEN;
    }

    fgSpr.setTextDatum(TC_DATUM);
    fgSpr.setTextFont(1);
    fgSpr.setTextSize(1);

    for (int i = 0; i < 8; i++)
    {
        int idx = startIndex + i;
        if (isDaily && idx >= 16)
            continue;
        if (!isDaily && idx >= 24)
            continue;

        const ForecastSlot &slot = isDaily ? dailyForecast.at(idx) : hourlyForecast.at(idx);
        if (!slot.valid)
            continue;

        int sx = startX + (i * slotW);
        int cx = sx + (slotW / 2);

        fgSpr.fillRoundRect(sx + 1, 0, slotW - 2, boxH, 5, TFT_NAVY);
        fgSpr.drawRoundRect(sx + 1, 0, slotW - 2, boxH, 5, borderCol);

        // 1. Header label ("18h" or "Mon")
        fgSpr.setTextColor(labelCol, TFT_NAVY);
        fgSpr.drawString(slot.label, cx, 2, 1);

        // 2. Weather icon
        drawMiniWeatherIcon(fgSpr, cx, 17, slot.wmoCode, slot.isDay);

        // 3. Temperature + tiny degree dot ("27°")
        String tStr = String(slot.tempC);
        int tw = fgSpr.textWidth(tStr, 1);
        int tx = cx - ((tw + 3) / 2);
        fgSpr.setTextDatum(TL_DATUM);
        fgSpr.setTextColor(TFT_WHITE, TFT_NAVY);
        fgSpr.drawString(tStr, tx, 28, 1);
        fgSpr.drawCircle(tx + tw + 1, 29, 1, TFT_WHITE);
        fgSpr.setTextDatum(TC_DATUM);

        // 4. Rain Probability (%) or Rainfall (mm) on both Hourly & Daily cards
        if (gRainAsPercent)
        {
            if (slot.precipProb >= 10)
            {
                fgSpr.setTextColor(TFT_CYAN, TFT_NAVY);
                fgSpr.drawString(String(slot.precipProb) + "%", cx, 39, 1);
            }
        }
        else
        {
            if (slot.precipMm > 0.1f)
            {
                int roundedMm = (int)roundf(slot.precipMm);
                String pStr = (roundedMm >= 1) ? (String(roundedMm) + "mm") : "<1mm";
                fgSpr.setTextColor(TFT_CYAN, TFT_NAVY);
                fgSpr.drawString(pStr, cx, 39, 1);
            }
        }
    }

    // 5. Page indicator dots
    int hourlyPages = (hourlyForecastCount > 16) ? 3 : ((hourlyForecastCount > 8) ? 2 : ((hourlyForecastCount > 0) ? 1 : 0));
    int dailyPages = (dailyForecastCount > 8) ? 2 : ((dailyForecastCount > 0) ? 1 : 0);
    int totalPages = hourlyPages + dailyPages;

    if (totalPages > 1)
    {
        const int dotSpacing = 12;
        int startDotX = (WX_SCREEN_W / 2) - (((totalPages - 1) * dotSpacing) / 2);
        int dotY = boxH + 5;

        for (int p = 0; p < totalPages; p++)
        {
            int dx = startDotX + (p * dotSpacing);
            if (p == forecastViewMode)
            {
                fgSpr.fillCircle(dx, dotY, 3, activeDotCol);
            }
            else
            {
                fgSpr.fillCircle(dx, dotY, 3, TFT_NAVY);
                fgSpr.drawCircle(dx, dotY, 3, TFT_LIGHTGREY);
            }
        }
    }

    for (int16_t y = 0; y < HOURLY_H; y++)
    {
        for (int16_t x = 0; x < WX_SCREEN_W; x++)
        {
            uint16_t fgPix = fgSpr.readPixel(x, y);
            if (fgPix != TFT_BLACK)
            {
                uint16_t bgPix = hSpr.readPixel(x, y);
                uint8_t alpha = (fgPix <= 0x001F) ? cardBgAlpha : detailAlpha;
                hSpr.drawPixel(x, y, tft.alphaBlend(alpha, fgPix, bgPix));
            }
        }
    }

    fgSpr.deleteSprite();
    hSpr.pushSprite(0, HOURLY_Y);
    hSpr.deleteSprite();
}


void drawWeatherDetailPopup()
{
    // Shared Dark-Slate UI Palette
    constexpr uint16_t WX_CARD       = 0x18E3; // Elevated dark slate card
    constexpr uint16_t WX_CARD_ALT   = 0x10A2; // Recessed header/tile fill
    constexpr uint16_t WX_BORDER     = 0x39E7; // Crisp slate hairline border
    constexpr uint16_t WX_ACCENT     = 0x067F; // Modern sky-cyan accent
    constexpr uint16_t WX_ACCENT_DK  = 0x02B0; // Deep cyan border/fill
    constexpr uint16_t WX_SUCCESS    = 0x07E8; // Vibrant mint-green
    constexpr uint16_t WX_DANGER     = 0xF986; // Soft coral-red
    constexpr uint16_t WX_TEXT_MUTED = 0x9CD3; // Muted cool grey labels

    const int16_t popW = 292;
    const int16_t popH = 172;
    const int16_t popX = (WX_SCREEN_W - popW) / 2;
    const int16_t popY = 34;

    // 1. Elevated Dark-Slate Card Container
    tft.fillRoundRect(popX, popY, popW, popH, 8, WX_CARD);
    tft.drawRoundRect(popX, popY, popW, popH, 8, WX_ACCENT_DK);

    // 2. Unified Card Header Bar (26px tall with left cyan accent pill)
    tft.fillRoundRect(popX + 1, popY + 1, popW - 2, 26, 7, WX_CARD_ALT);
    tft.fillRect(popX + 1, popY + 18, popW - 2, 9, WX_CARD_ALT); // Square bottom corners of header
    tft.drawFastHLine(popX + 1, popY + 27, popW - 2, WX_BORDER);
    tft.fillRoundRect(popX + 8, popY + 6, 4, 15, 2, WX_ACCENT);

    tft.setFreeFont(nullptr);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, WX_CARD_ALT);
    tft.drawString(String(gWhere) + " Weather", popX + 18, popY + 14, 2);

    // Page indicator dots on right of header (Page 1 / Page 2)
    int16_t dot1X = popX + popW - 26;
    int16_t dot2X = popX + popW - 14;
    int16_t dotY  = popY + 14;
    tft.fillCircle(dot1X, dotY, 3, (weatherPopupPage == 0) ? WX_ACCENT : WX_BORDER);
    tft.fillCircle(dot2X, dotY, 3, (weatherPopupPage == 1) ? WX_ACCENT : WX_BORDER);

    // 3. Centered Current Temp (with crisp degree circle) & Condition Summary
    String condOnly = weatherCondStr;
    if (condOnly.startsWith("C "))
    {
        condOnly = condOnly.substring(2);
    }

    int tempW = tft.textWidth(weatherTempStr, 2);
    String suffixStr = "C  " + condOnly;
    int sufW = tft.textWidth(suffixStr, 2);
    int totalW = tempW + 6 + sufW;
    int startX = popX + ((popW - totalW) / 2);
    int sumY = popY + 41;

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_GOLD, WX_CARD);
    tft.drawString(weatherTempStr, startX, sumY, 2);
    tft.drawCircle(startX + tempW + 2, sumY - 4, 2, TFT_GOLD);
    tft.drawString(suffixStr, startX + tempW + 6, sumY, 2);

    tft.drawFastHLine(popX + 12, popY + 54, popW - 24, WX_BORDER);

    // 4. Two-Column Metric Grid
    tft.setTextDatum(TL_DATUM);
    const int16_t col1X = popX + 14;
    const int16_t col2X = popX + 152;
    int16_t rowY = popY + 62;
    const int16_t rowStep = 26;

    // Subtle vertical divider between the two columns
    tft.drawFastVLine(popX + (popW / 2), popY + 60, 74, WX_BORDER);

    if (weatherPopupPage == 0)
    {
        // PAGE 1: Feels Like, Humidity, Wind + Compass, Max UV, Sunrise & Sunset
        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Feels:", col1X, rowY, 2);
        tft.setTextColor(TFT_WHITE, WX_CARD);
        String feelsStr = String(weatherFeelsLikeC);
        tft.drawString(feelsStr, col1X + 46, rowY, 2);
        int fw = tft.textWidth(feelsStr, 2);
        tft.drawCircle(col1X + 46 + fw + 2, rowY + 3, 2, TFT_WHITE);
        tft.drawString("C", col1X + 46 + fw + 6, rowY, 2);

        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Humid:", col2X, rowY, 2);
        tft.setTextColor(WX_ACCENT, WX_CARD);
        tft.drawString(String(weatherHumidityPct) + "%", col2X + 48, rowY, 2);

        rowY += rowStep;
        int windMph = (int)roundf(weatherWindKmh * 0.621371f);
        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Wind:", col1X, rowY, 2);
        tft.setTextColor(TFT_WHITE, WX_CARD);
        tft.drawString(String(windMph) + "mph " + degToCompass(weatherWindDirDeg), col1X + 42, rowY, 2);

        uint16_t uvCol = WX_SUCCESS;
        if (weatherUvMax >= 8.0f) uvCol = WX_DANGER;
        else if (weatherUvMax >= 6.0f) uvCol = TFT_ORANGE;
        else if (weatherUvMax >= 3.0f) uvCol = TFT_YELLOW;

        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Max UV:", col2X, rowY, 2);
        tft.setTextColor(uvCol, WX_CARD);
        tft.drawString(String(weatherUvMax, 1), col2X + 54, rowY, 2);

        rowY += rowStep;
        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Rise:", col1X, rowY, 2);
        tft.setTextColor(TFT_GOLD, WX_CARD);
        tft.drawString(weatherSunriseStr, col1X + 46, rowY, 2);

        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Set:", col2X, rowY, 2);
        tft.setTextColor(TFT_ORANGE, WX_CARD);
        tft.drawString(weatherSunsetStr, col2X + 48, rowY, 2);

        // Footer hint bar
        tft.drawFastHLine(popX + 12, popY + popH - 22, popW - 24, WX_BORDER);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Tap for Marine & Sky details", popX + (popW / 2), popY + popH - 11, 1);
    }
    else
    {
        // PAGE 2: Sea Temp, Wave Height, Gusts, Clouds, Pressure & Daylight Length
        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Sea:", col1X, rowY, 2);
        tft.setTextColor(WX_ACCENT, WX_CARD);
        if (hasMarineData)
        {
            String seaStr = String(weatherSeaTempC, 1);
            tft.drawString(seaStr, col1X + 42, rowY, 2);
            int sw = tft.textWidth(seaStr, 2);
            tft.drawCircle(col1X + 42 + sw + 2, rowY + 3, 2, WX_ACCENT);
            tft.drawString("C", col1X + 42 + sw + 6, rowY, 2);
        }
        else
        {
            tft.drawString("N/A", col1X + 42, rowY, 2);
        }

        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Waves:", col2X, rowY, 2);
        tft.setTextColor(TFT_SKYBLUE, WX_CARD);
        tft.drawString(hasMarineData ? (String(weatherWaveHeightM, 1) + " m") : "N/A", col2X + 50, rowY, 2);

        rowY += rowStep;
        int gustMph = (int)roundf(weatherWindGustsKmh * 0.621371f);
        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Gusts:", col1X, rowY, 2);
        tft.setTextColor(TFT_WHITE, WX_CARD);
        tft.drawString(String(gustMph) + " mph", col1X + 46, rowY, 2);

        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Cloud:", col2X, rowY, 2);
        tft.setTextColor(TFT_WHITE, WX_CARD);
        tft.drawString(String(weatherCloudCoverPct) + "%", col2X + 50, rowY, 2);

        rowY += rowStep;
        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Press:", col1X, rowY, 2);
        tft.setTextColor(WX_SUCCESS, WX_CARD);
        tft.drawString(String(weatherPressureHpa) + "hPa", col1X + 46, rowY, 2);

        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Light:", col2X, rowY, 2);
        tft.setTextColor(TFT_GOLD, WX_CARD);
        tft.drawString(calcDaylightDuration(weatherSunriseStr, weatherSunsetStr), col2X + 46, rowY, 2);

        // Footer hint bar
        tft.drawFastHLine(popX + 12, popY + popH - 22, popW - 24, WX_BORDER);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(WX_TEXT_MUTED, WX_CARD);
        tft.drawString("Tap anywhere to close", popX + (popW / 2), popY + popH - 11, 1);
    }

    tft.setTextDatum(TL_DATUM);
}

void closeWeatherDetailPopup(bool isArrived)
{
    if (!isWeatherPopupOpen)
        return;

    isWeatherPopupOpen = false;
    weatherPopupPage = 0;
    tft.fillScreen(TFT_BLACK);
    displaySSIDAndWhen();
    drawStaticBackgroundImage();
    drawWhere(isArrived);
    drawHourlyForecastBar();
}

bool handleWeatherForecastTouch(uint16_t tx, uint16_t ty)
{
    if (!gWeatherEnabled || (hourlyForecastCount == 0 && dailyForecastCount == 0))
        return false;

    // 1. If popup is open: advance from Page 1 -> Page 2, then close on next tap
    if (isWeatherPopupOpen)
    {
        if (weatherPopupPage == 0)
        {
            weatherPopupPage = 1;
            weatherPopupOpenedMs = millis(); // Reset auto-close timer
            drawWeatherDetailPopup();
        }
        else
        {
            closeWeatherDetailPopup(false);
        }
        return true;
    }

    // 2. Tapping the bottom-left current weather summary opens Page 1 of the detail popup
    if (weatherTempStr.length() > 0 && ty >= 220 && tx >= 20 && tx <= 185)
    {
        isWeatherPopupOpen = true;
        weatherPopupPage = 0;
        weatherPopupOpenedMs = millis();
        drawWeatherDetailPopup();
        return true;
    }

    // 3. Tapping the 8-slot forecast bar cycles through the 5 forecast pages
    if (tx >= 8 && tx <= 312 && ty >= HOURLY_Y && ty <= (HOURLY_Y + HOURLY_H))
    {
        for (int step = 0; step < 5; step++)
        {
            forecastViewMode = (forecastViewMode + 1) % 5;

            if (forecastViewMode == 0 && hourlyForecastCount > 0)
                break;
            if (forecastViewMode == 1 && hourlyForecastCount > 8)
                break;
            if (forecastViewMode == 2 && hourlyForecastCount > 16)
                break;
            if (forecastViewMode == 3 && dailyForecastCount > 0)
                break;
            if (forecastViewMode == 4 && dailyForecastCount > 8)
                break;
        }

        drawHourlyForecastBar();
        return true;
    }
    return false;
}

void drawWeatherBadge()
{
    if (!gWeatherEnabled)
        return;

    if (weatherTempStr.length() == 0)
    {
        Serial.println("[WX] Have no weather text to draw");
        return;
    }
    Serial.printf("[WX] Updating bottom weather & forecast bar: %s deg %s\n",
                  weatherTempStr.c_str(), weatherCondStr.c_str());

    displaySSIDAndWhen();
    drawHourlyForecastBar();
}

void fetchDestinationWeather()
{
    if (!gWeatherEnabled)
        return;

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WX] WiFi not connected");
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(6000);

    if (!hasCachedCoords)
    {
        String query = String(gWhere);
        query.trim();
        query.replace(" ", "%20");

        String geoUrl = String("http://") + "geocoding-api." + "open-meteo.com" +
                        "/v1/search?name=" + query + "&count=1&format=json";

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
                    Serial.printf("[WX] Geocoded '%s' -> %.4f, %.4f\n", gWhere, cachedLat, cachedLon);
                }
            }
            http.end();
        }
    }

    if (!hasCachedCoords)
    {
        Serial.printf("[WX] Could not geocode destination: '%s'\n", gWhere);
        return;
    }

    String wxUrl = String("http://") + "api." + "open-meteo.com" +
                   "/v1/forecast?latitude=" + String(cachedLat, 4) +
                   "&longitude=" + String(cachedLon, 4) +
                   "&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,wind_direction_10m,wind_gusts_10m,cloud_cover,surface_pressure,weather_code,is_day" +
                   "&hourly=temperature_2m,weathercode,is_day,precipitation_probability,precipitation&forecast_hours=25" +
                   "&daily=weathercode,temperature_2m_max,precipitation_sum,precipitation_probability_max,uv_index_max,sunrise,sunset&forecast_days=16&timezone=auto";

    if (http.begin(client, wxUrl))
    {
        int code = http.GET();
        if (code == HTTP_CODE_OK)
        {
            String wxPayload = http.getString();

            float utcOff = 0.0f;
            if (extractJsonNumber(wxPayload, "utc_offset_seconds", utcOff))
            {
                destUtcOffsetSec = (long)roundf(utcOff);
                hasDestUtcOffset = true;
            }

            // 1. Parse current weather + extra metrics
            String curKey = String(char(34)) + "current" + String(char(34)) + ":{";
            int curIdx = wxPayload.indexOf(curKey);
            bool hasCurrent = false;
            String baseCondText = "";

            if (curIdx >= 0)
            {
                String curSection = wxPayload.substring(curIdx);
                float tempC = 0.0f, wCode = 0.0f, isDayVal = 1.0f;
                float appTemp = 0.0f, hum = 0.0f, wind = 0.0f, wDir = 0.0f;
                float gusts = 0.0f, clouds = 0.0f, press = 0.0f;

                if (extractJsonNumber(curSection, "temperature_2m", tempC) &&
                    extractJsonNumber(curSection, "weather_code", wCode))
                {
                    extractJsonNumber(curSection, "is_day", isDayVal);
                    bool isDay = ((int)roundf(isDayVal) == 1);

                    weatherTempStr = String((int)roundf(tempC));
                    baseCondText = wmoCodeToText((int)wCode, isDay);
                    hasCurrent = true;
                }

                if (extractJsonNumber(curSection, "apparent_temperature", appTemp))
                    weatherFeelsLikeC = (int)roundf(appTemp);
                if (extractJsonNumber(curSection, "relative_humidity_2m", hum))
                    weatherHumidityPct = (int)roundf(hum);
                if (extractJsonNumber(curSection, "wind_speed_10m", wind))
                    weatherWindKmh = (int)roundf(wind);
                if (extractJsonNumber(curSection, "wind_direction_10m", wDir))
                    weatherWindDirDeg = (int)roundf(wDir);
                if (extractJsonNumber(curSection, "wind_gusts_10m", gusts))
                    weatherWindGustsKmh = (int)roundf(gusts);
                if (extractJsonNumber(curSection, "cloud_cover", clouds))
                    weatherCloudCoverPct = (int)roundf(clouds);
                if (extractJsonNumber(curSection, "surface_pressure", press))
                    weatherPressureHpa = (int)roundf(press);
            }

            // 2. Parse next 24 hours (including precipitation probability & mm)
            String hrKey = String(char(34)) + "hourly" + String(char(34)) + ":{";
            int hrIdx = wxPayload.indexOf(hrKey);
            if (hrIdx >= 0)
            {
                String hrSection = wxPayload.substring(hrIdx);
                std::array<int, 25> hours = {};
                std::array<float, 25> temps = {};
                std::array<float, 25> codes = {};
                std::array<float, 25> days = {};
                std::array<float, 25> probs = {};
                std::array<float, 25> precips = {};

                int nH = extractJsonHourArray(hrSection, "time", hours.data(), 25);
                int nT = extractJsonNumberArray(hrSection, "temperature_2m", temps.data(), 25);
                int nC = extractJsonNumberArray(hrSection, "weathercode", codes.data(), 25);
                int nD = extractJsonNumberArray(hrSection, "is_day", days.data(), 25);
                int nP = extractJsonNumberArray(hrSection, "precipitation_probability", probs.data(), 25);
                int nM = extractJsonNumberArray(hrSection, "precipitation", precips.data(), 25);

                int available = min(min(nH, nT), min(nC, nD));
                int startOffset = (available >= 25) ? 1 : 0;
                hourlyForecastCount = 0;

                for (int i = 0; i < 24; i++)
                {
                    int src = i + startOffset;
                    if (src < available)
                    {
                        int hr = (hours.at(src) + (gIslandPlusOne ? 1 : 0)) % 24;
                        hourlyForecast.at(i).label = (hr < 10 ? "0" : "") + String(hr) + "h";
                        hourlyForecast.at(i).tempC = (int)roundf(temps.at(src));
                        hourlyForecast.at(i).wmoCode = (int)roundf(codes.at(src));
                        hourlyForecast.at(i).isDay = ((int)roundf(days.at(src)) == 1);
                        hourlyForecast.at(i).precipProb = (src < nP) ? (int)roundf(probs.at(src)) : 0;
                        hourlyForecast.at(i).precipMm = (src < nM) ? precips.at(src) : 0.0f;
                        hourlyForecast.at(i).valid = true;
                        hourlyForecastCount++;
                    }
                    else
                    {
                        hourlyForecast.at(i).valid = false;
                    }
                }
            }

            // 3. Parse up to 16 days
            String dyKey = String(char(34)) + "daily" + String(char(34)) + ":{";
            int dyIdx = wxPayload.indexOf(dyKey);
            if (dyIdx >= 0)
            {
                String dySection = wxPayload.substring(dyIdx);
                std::array<String, 16> dayNames;
                std::array<float, 16> dCodes = {};
                std::array<float, 16> dTemps = {};
                std::array<float, 16> dPrecip = {};
                std::array<float, 16> dProb = {};
                std::array<float, 16> dUv = {};

                int nN = extractJsonDayNameArray(dySection, "time", dayNames.data(), 16);
                int nC = extractJsonNumberArray(dySection, "weathercode", dCodes.data(), 16);
                int nT = extractJsonNumberArray(dySection, "temperature_2m_max", dTemps.data(), 16);
                int nP = extractJsonNumberArray(dySection, "precipitation_sum", dPrecip.data(), 16);
                int nPr = extractJsonNumberArray(dySection, "precipitation_probability_max", dProb.data(), 16);
                int nU = extractJsonNumberArray(dySection, "uv_index_max", dUv.data(), 16);

                if (nU > 0)
                    weatherUvMax = dUv.at(0);

                weatherSunriseStr = extractJsonFirstTimeHHMM(dySection, "sunrise", gIslandPlusOne);
                weatherSunsetStr = extractJsonFirstTimeHHMM(dySection, "sunset", gIslandPlusOne);

                int available = min(nN, min(nC, nT));
                dailyForecastCount = 0;

                for (int i = 0; i < 16; i++)
                {
                    if (i < available)
                    {
                        dailyForecast.at(i).label = dayNames.at(i);
                        dailyForecast.at(i).tempC = (int)roundf(dTemps.at(i));
                        dailyForecast.at(i).wmoCode = (int)roundf(dCodes.at(i));
                        dailyForecast.at(i).isDay = true;
                        dailyForecast.at(i).precipMm = (i < nP) ? dPrecip.at(i) : 0.0f;
                        dailyForecast.at(i).precipProb = (i < nPr) ? (int)roundf(dProb.at(i)) : 0;
                        dailyForecast.at(i).valid = true;
                        dailyForecastCount++;
                    }
                    else
                    {
                        dailyForecast.at(i).valid = false;
                    }
                }
            }

            if (hasCurrent)
            {
                weatherCondStr = "C " + baseCondText;
                Serial.println("[WX] Updated destination weather: " + weatherTempStr + " deg " + weatherCondStr);
            }
        }
        http.end();
    }

    // 4. Fetch Marine Data (Sea Surface Temp & Wave Height)
    String marineUrl = String("http://") + "marine-api." + "open-meteo.com" +
                       "/v1/marine?latitude=" + String(cachedLat, 4) +
                       "&longitude=" + String(cachedLon, 4) +
                       "&current=sea_surface_temperature,wave_height";

    hasMarineData = false;
    if (http.begin(client, marineUrl))
    {
        if (http.GET() == HTTP_CODE_OK)
        {
String mPayload = http.getString();
String curKey = String(char(34)) + "current" + String(char(34)) + ":{";
int curIdx = mPayload.indexOf(curKey);
if (curIdx >= 0)
{
String curSec = mPayload.substring(curIdx);
float seaT = 0.0f, waveH = 0.0f;
if (extractJsonNumber(curSec, "sea_surface_temperature", seaT) &&
extractJsonNumber(curSec, "wave_height", waveH))
{
weatherSeaTempC = seaT;
weatherWaveHeightM = waveH;
hasMarineData = true;
Serial.printf("[WX] Marine: Sea %.1f C, Waves %.1f m\n", weatherSeaTempC, weatherWaveHeightM);
}
}
}
http.end();
}
}
