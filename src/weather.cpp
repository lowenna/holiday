#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>

extern TFT_eSPI tft;
extern char gWhere[];

int16_t WHERE_Y = 35;
int16_t WHERE_H = 26;

// Weather state & cached coordinates
String weatherTempStr = "";
String weatherCondStr = "";
float cachedLat = 0.0f;
float cachedLon = 0.0f;
bool hasCachedCoords = false;
unsigned long lastWeatherFetchMs = 0;
unsigned long WEATHER_INTERVAL_MS = 1800000UL; // 30 minutes

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

void drawWeatherBadge()
{
    if (weatherTempStr.length() == 0)
    {
        Serial.println("[WX] Have no weather text to draw");
        return;
    }
    Serial.printf("[WX] Drawing badge: %s deg %s\n", weatherTempStr.c_str(), weatherCondStr.c_str());

    TFT_eSprite wSpr(&tft);
    wSpr.setColorDepth(16);
    wSpr.createSprite(180, 22);
    wSpr.fillSprite(TFT_BLACK);

    // Measure left part ("27"), degree circle gap (6px), and right part ("C Sunny")
    int tempW = wSpr.textWidth(weatherTempStr, 2);
    const int degSpace = 6;
    int condW = wSpr.textWidth(weatherCondStr, 2);
    int totalTextW = tempW + degSpace + condW;

    int pillW = constrain(totalTextW + 20, 64, 176);
    int pillX = (180 - pillW) / 2;

    wSpr.fillRoundRect(pillX, 1, pillW, 20, 9, TFT_NAVY);
    wSpr.drawRoundRect(pillX, 1, pillW, 20, 9, TFT_CYAN);

    // Draw temperature, crisp small degree circle, and condition
    int startX = 90 - (totalTextW / 2);
    wSpr.setTextDatum(ML_DATUM);
    wSpr.setTextColor(TFT_WHITE, TFT_NAVY);
    wSpr.drawString(weatherTempStr, startX, 11, 2);

    int degX = startX + tempW + 2;
    wSpr.drawCircle(degX, 6, 2, TFT_WHITE);

    wSpr.drawString(weatherCondStr, startX + tempW + degSpace, 11, 2);
    wSpr.setTextDatum(TL_DATUM);

    wSpr.pushSprite(70, WHERE_Y + 42, TFT_BLACK);
    wSpr.deleteSprite();
}

void fetchDestinationWeather()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WX] WiFi not connected");
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(6000);

    // Step 1: Resolve destination name (gWhere) to lat/lon once
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
                else
                {
                    Serial.printf("[WX] No coordinates in JSON: %s\n", geoPayload.c_str());
                }
            }
            else
            {
                Serial.printf("[WX] Geocode HTTP error: %d (%s)\n", code, http.errorToString(code).c_str());
            }
            http.end();
        }
        else
        {
            Serial.printf("[WX] http.begin failed for %s\n", geoUrl.c_str());
        }
    }

    if (!hasCachedCoords)
    {
        Serial.printf("[WX] Could not geocode destination: '%s'\n", gWhere);
        return;
    }

    // Step 2: Fetch live current weather at those coordinates
    String wxUrl = String("http://") + "api." + "open-meteo.com" +
                   "/v1/forecast?latitude=" + String(cachedLat, 4) +
                   "&longitude=" + String(cachedLon, 4) + "&current_weather=true";
    Serial.printf("[WX] Forecast URL: %s\n", wxUrl.c_str());

    if (http.begin(client, wxUrl))
    {
        int code = http.GET();
        if (code == HTTP_CODE_OK)
        {
            String wxPayload = http.getString();

            // Skip past "current_weather_units" so we parse the numeric "current_weather" block
            int cwIdx = wxPayload.indexOf("\"current_weather\":{");
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
            else
            {
                Serial.printf("[WX] Could not parse weather JSON: %s\n", wxPayload.c_str());
            }
        }
        else
        {
            Serial.printf("[WX] Weather HTTP error: %d (%s)\n", code, http.errorToString(code).c_str());
        }
        http.end();
    }
}
