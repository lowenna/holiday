#include <TFT_eSPI.h>
#include <HTTPClient.h>

extern TFT_eSPI tft;
extern char *gWhere;

int16_t WHERE_Y = 35;
int16_t WHERE_H = 26; // These 2 were constexpr but can't use as extern

// Weather state & cached coordinates
String weatherText = "";
float cachedLat = 0.0f;
float cachedLon = 0.0f;
bool hasCachedCoords = false;
unsigned long lastWeatherFetchMs = 0;
unsigned long WEATHER_INTERVAL_MS = 1800000UL; // 30 minutes

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

void drawWeatherBadge()
{
    if (weatherText.length() == 0)
    {
        Serial.println("Have no weather text");
        return;
    }
    Serial.printf("Weather: %s\n", weatherText.c_str());
    //    return;

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
        else
        {
            Serial.printf("Failed to get weather from %s\n", geoUrl.c_str());
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
