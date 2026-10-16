#include <FS.h>
#include <Arduino.h>
#include "TimeClient.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <SPIFFS.h>
// From example for saving additional parameters in wifimanager
#include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson

// define your default values here, if there are different values in config.json, they are overwritten.
char hol_day[3] = "5";
char hol_month[3] = "12";
char hol_year[5] = "2026";
char hol_who[21] = "Sandra & John";
char hol_where[21] = "Maldives";

// flag for saving data
bool shouldSaveConfig = false;
TimeClient tc((float)0);

// Wifimanager callback notifying us of the need to save config
void saveConfigCallback()
{
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup()
{

  Serial.begin(115200);
  while (!Serial)
    continue;

  // read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin())
  {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json"))
    {
      // file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);

        //JsonDocument json(1024);
        JsonDocument json;
        auto deserializeError = deserializeJson(json, buf.get());
        serializeJson(json, Serial);
        if (!deserializeError)
        {
          Serial.println("\nparsed json");
          strcpy(hol_day, json["hol_day"]);
          strcpy(hol_month, json["hol_month"]);
          strcpy(hol_year, json["hol_year"]);
          strcpy(hol_where, json["hol_where"]);
          strcpy(hol_who, json["hol_who"]);
        }
        else
        {
          Serial.println("failed to load json config");
        }
        configFile.close();
      }
    }
  }
  else
  {
    Serial.println("failed to mount FS");
  }
  // end read

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_hol_day("day", "Day", hol_day, 2);
  WiFiManagerParameter custom_hol_month("month", "Month", hol_month, 2);
  WiFiManagerParameter custom_hol_year("year", "Year", hol_year, 4);
  WiFiManagerParameter custom_hol_where("where", "Where", hol_where, 20);
  WiFiManagerParameter custom_hol_who("who", "Who", hol_who, 20);

  // WiFiManager
  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // add all your parameters here
  wifiManager.addParameter(&custom_hol_day);
  wifiManager.addParameter(&custom_hol_month);
  wifiManager.addParameter(&custom_hol_year);
  wifiManager.addParameter(&custom_hol_where);
  wifiManager.addParameter(&custom_hol_who);

  if (!wifiManager.autoConnect())
  {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    // reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(5000);
  }

  // if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");

  // read updated parameters
  strcpy(hol_day, custom_hol_day.getValue());
  strcpy(hol_month, custom_hol_month.getValue());
  strcpy(hol_year, custom_hol_year.getValue());
  strcpy(hol_who, custom_hol_who.getValue());
  strcpy(hol_where, custom_hol_where.getValue());
  Serial.println("The values in the file are: ");
  Serial.println("\thol_day : " + String(hol_day));

  // save the custom parameters to FS
  if (shouldSaveConfig)
  {
    Serial.println("saving config");
    JsonDocument json;//(1024);
    json["hol_day"] = hol_day;
    json["hol_month"] = hol_month;
    json["hol_year"] = hol_year;
    json["hol_who"] = hol_who;
    json["hol_where"] = hol_where;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile)
    {
      Serial.println("failed to open config file for writing");
    }

    serializeJson(json, Serial);
    serializeJson(json, configFile);
    configFile.close();
    // end save

    // tc.updateTime();

    //   Serial.println("Starting autoConnect");
    //   wm.autoConnect();

    Serial.println("setup() complete");
  }
}

  void loop()
  {

    Serial.printf("%d\n", digitalRead(0));
    delay(100);
  }