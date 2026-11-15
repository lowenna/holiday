#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <array>

extern TFT_eSPI tft;
extern char gWhere[];
extern bool gWeatherEnabled;
extern void fillSpriteWithMaldivesSlice(TFT_eSprite &spr, int16_t screenY, int16_t sliceH);
extern void displaySSIDAndWhen();

int16_t WHERE_Y = 40;
int16_t WHERE_H = 26;

static constexpr int16_t WX_SCREEN_W = 320;
static constexpr int16_t HOURLY_Y = 98;
static constexpr int16_t HOURLY_H = 44;

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
    bool valid;
};

std::array<ForecastSlot, 8> hourlyForecast;
std::array<ForecastSlot, 8> dailyForecast;
bool hasHourlyForecast = false;
bool hasDailyForecast = false;
bool showDailyMode = false;

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

    bool activeDaily = (showDailyMode && hasDailyForecast);
    if (!activeDaily && !hasHourlyForecast)
        return;

    const std::array<ForecastSlot, 8> &slots = activeDaily ? dailyForecast : hourlyForecast;

    TFT_eSprite hSpr(&tft);
    hSpr.setColorDepth(16);
    hSpr.createSprite(WX_SCREEN_W, HOURLY_H);

    fillSpriteWithMaldivesSlice(hSpr, HOURLY_Y, HOURLY_H);

    const int slotW = 38;
    const int startX = 8;

    // Gold border & header text in Daily mode; Cyan border & header text in Hourly mode
    uint16_t borderCol = activeDaily ? TFT_GOLD : TFT_DARKCYAN;
    uint16_t labelCol = activeDaily ? TFT_YELLOW : TFT_CYAN;

    hSpr.setTextDatum(TC_DATUM);
    hSpr.setTextFont(1);
    hSpr.setTextSize(1);

    for (int i = 0; i < 8; i++)
    {
        if (!slots.at(i).valid)
            continue;

        int sx = startX + (i * slotW);
        int cx = sx + (slotW / 2);

        hSpr.fillRoundRect(sx + 1, 0, slotW - 2, HOURLY_H, 5, TFT_NAVY);
        hSpr.drawRoundRect(sx + 1, 0, slotW - 2, HOURLY_H, 5, borderCol);

        hSpr.setTextColor(labelCol, TFT_NAVY);
        hSpr.drawString(slots.at(i).label, cx, 3, 1);

        drawMiniWeatherIcon(hSpr, cx, 20, slots.at(i).wmoCode, slots.at(i).isDay);

        String tStr = String(slots.at(i).tempC);
        int tw = hSpr.textWidth(tStr, 1);
        int tx = cx - ((tw + 3) / 2);
        hSpr.setTextDatum(TL_DATUM);
        hSpr.setTextColor(TFT_WHITE, TFT_NAVY);
        hSpr.drawString(tStr, tx, 33, 1);
        hSpr.drawCircle(tx + tw + 1, 34, 1, TFT_WHITE);
        hSpr.setTextDatum(TC_DATUM);
    }

    hSpr.pushSprite(0, HOURLY_Y);
    hSpr.deleteSprite();
}

bool handleWeatherForecastTouch(uint16_t tx, uint16_t ty)
{
    if (!gWeatherEnabled || (!hasHourlyForecast && !hasDailyForecast))
        return false;

    if (tx >= 8 && tx <= 312 && ty >= HOURLY_Y && ty <= (HOURLY_Y + HOURLY_H))
    {
        if (hasDailyForecast && hasHourlyForecast)
        {
            showDailyMode = !showDailyMode;
            drawHourlyForecastBar();
            return true;
        }
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
        Serial.printf("[WX] Geocoding URL: %s\n", geoUrl.c_str());

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
                   "&current_weather=true&hourly=temperature_2m,weathercode,is_day&forecast_hours=9" +
                   "&daily=weathercode,temperature_2m_max&forecast_days=9&timezone=auto";
    Serial.printf("[WX] Forecast URL: %s\n", wxUrl.c_str());

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
                Serial.printf("[WX] Destination UTC offset: %ld seconds\n", destUtcOffsetSec);
            }

            // 1. Parse current weather
            String cwKey = String(char(34)) + "current_weather" + String(char(34)) + ":{";
            int cwIdx = wxPayload.indexOf(cwKey);
            String cwSection = (cwIdx >= 0) ? wxPayload.substring(cwIdx) : wxPayload;

            float tempC = 0.0f, wCode = 0.0f, isDayVal = 1.0f;
            if (extractJsonNumber(cwSection, "temperature", tempC) &&
                extractJsonNumber(cwSection, "weathercode", wCode))
            {
                extractJsonNumber(cwSection, "is_day", isDayVal);
                bool isDay = ((int)roundf(isDayVal) == 1);

                weatherTempStr = String((int)roundf(tempC));
                weatherCondStr = "C " + wmoCodeToText((int)wCode, isDay);
                Serial.println("[WX] Updated destination weather: " + weatherTempStr + " deg " + weatherCondStr);
            }

            // 2. Parse next 8 hours
            String hrKey = String(char(34)) + "hourly" + String(char(34)) + ":{";
            int hrIdx = wxPayload.indexOf(hrKey);
            if (hrIdx >= 0)
            {
                String hrSection = wxPayload.substring(hrIdx);
                std::array<int, 9> hours = {};
                std::array<float, 9> temps = {};
                std::array<float, 9> codes = {};
                std::array<float, 9> days = {};

                int nH = extractJsonHourArray(hrSection, "time", hours.data(), 9);
                int nT = extractJsonNumberArray(hrSection, "temperature_2m", temps.data(), 9);
                int nC = extractJsonNumberArray(hrSection, "weathercode", codes.data(), 9);
                int nD = extractJsonNumberArray(hrSection, "is_day", days.data(), 9);

                int available = min(min(nH, nT), min(nC, nD));
                int startOffset = (available >= 9) ? 1 : 0;

                for (int i = 0; i < 8; i++)
                {
                    int src = i + startOffset;
                    if (src < available)
                    {
                        int hr = hours.at(src);
                        hourlyForecast.at(i).label = (hr < 10 ? "0" : "") + String(hr) + "h";
                        hourlyForecast.at(i).tempC = (int)roundf(temps.at(src));
                        hourlyForecast.at(i).wmoCode = (int)roundf(codes.at(src));
                        hourlyForecast.at(i).isDay = ((int)roundf(days.at(src)) == 1);
                        hourlyForecast.at(i).valid = true;
                        hasHourlyForecast = true;
                    }
                    else
                    {
                        hourlyForecast.at(i).valid = false;
                    }
                }
            }

            // 3. Parse next 8 days (index 0 is today, indices 1..8 are next 8 days)
            String dyKey = String(char(34)) + "daily" + String(char(34)) + ":{";
            int dyIdx = wxPayload.indexOf(dyKey);
            if (dyIdx >= 0)
            {
                String dySection = wxPayload.substring(dyIdx);
                std::array<String, 9> dayNames;
                std::array<float, 9> dCodes = {};
                std::array<float, 9> dTemps = {};

                int nN = extractJsonDayNameArray(dySection, "time", dayNames.data(), 9);
                int nC = extractJsonNumberArray(dySection, "weathercode", dCodes.data(), 9);
                int nT = extractJsonNumberArray(dySection, "temperature_2m_max", dTemps.data(), 9);

                int available = min(nN, min(nC, nT));
                int startOffset = (available >= 9) ? 1 : 0;

                for (int i = 0; i < 8; i++)
                {
                    int src = i + startOffset;
                    if (src < available)
                    {
                        dailyForecast.at(i).label = dayNames.at(src);
                        dailyForecast.at(i).tempC = (int)roundf(dTemps.at(src));
                        dailyForecast.at(i).wmoCode = (int)roundf(dCodes.at(src));
                        dailyForecast.at(i).isDay = true; // Daytime icon for daily forecast
                        dailyForecast.at(i).valid = true;
                        hasDailyForecast = true;
                    }
                    else
                    {
                        dailyForecast.at(i).valid = false;
                    }
                }
            }
        }
        else
        {
            Serial.printf("[WX] Weather HTTP error: %d (%s)\n", code, http.errorToString(code).c_str());
        }
        http.end();
    }
}
