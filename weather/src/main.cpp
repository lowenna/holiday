//#include <Arduino.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
//#include "NTPClient.h"


// Custom settings for next holiday when not setup at all.
// If already in NVR, overwritten by that.
// I think can clear with prefs.begin(); prefs.remove("hol"); prefs.end();  Haven't tested
char gHour[3] = "10";
char gDay[3] = "5";
char gMonth[3] = "12";
char gYear[5] = "2026";
char gWho[21] = "Sandra & John";
char gWhere[21] = "Maldives";

// Callback from Wifimanager sets this flag to indicate data needs to be saved
bool shouldSaveConfig = false;

// Preferences from NVR
Preferences prefs;

// For NTP
//NTPClient n = NTPClient("pool.ntp.org", 0);
const char* MY_TZ = "GMT0BST,M3.5.0/1,M10.5.0";  

// Wifimanager callback notifying us of the need to save config
void saveConfigCallback()
{
  prefs.remove("hol");
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

bool areWeConfiguring() {
  unsigned long startAt = millis();
  bool pressed = false; 
  Serial.println("5 sec countdown. Press outside button for 1 second to configure");
  do {
    if (0 == digitalRead(0)) pressed = true;
    delay(1000);
    Serial.printf("%d %d\n", millis()-startAt, pressed); 
  } while ((millis() - startAt < 2000) &(!pressed));
  return pressed;
}

void readPreferences() {
  prefs.begin("hol",true);
  String lHour = prefs.getString("hour","");
  String lDay = prefs.getString("day","");
  String lMonth = prefs.getString("month","");
  String lYear = prefs.getString("year","");
  String lWho = prefs.getString("who","");
  String lWhere = prefs.getString("where","");
  prefs.end();

  if ((lHour.length() > 0) && (strcmp(lHour.c_str(), gHour))) strcpy(gHour, lHour.c_str());
  if ((lDay.length() > 0) && (strcmp(lDay.c_str(), gDay))) strcpy(gDay, lDay.c_str());
  if ((lMonth.length() > 0) && (strcmp(lMonth.c_str(), gMonth))) strcpy(gMonth, lMonth.c_str());
  if ((lYear.length() > 0) && (strcmp(lYear.c_str(), gYear))) strcpy(gYear, lYear.c_str());
  if ((lWho.length() > 0) && (strcmp(lWho.c_str(), gWho))) strcpy(gWho, lWho.c_str());
  if ((lWhere.length() > 0) && (strcmp(lWhere.c_str(), gWhere))) strcpy(gWhere,lWhere.c_str());   

  Serial.printf("Pref: At %02s:00 on %02s/%02s/%04s %s are going to %s\n", gHour, gDay, gMonth, gYear, gWho, gWhere);
}

void writePreferences(String hour, String day, String month, String year, String who, String where) {
  prefs.begin("hol", false);
  prefs.putString("hour", hour);
  prefs.putString("day", day);
  prefs.putString("month", month);
  prefs.putString("year", year);
  prefs.putString("who", who);
  prefs.putString("where", where);
  prefs.end();
}



void setup()
{
  Serial.begin(115200);
  while (!Serial)
    continue;

  bool configuring = areWeConfiguring();

  readPreferences() ;

  WiFiManagerParameter custom_hol_hour("hour", "Hour", gHour, 2);
  WiFiManagerParameter custom_hol_day("day", "Day", gDay, 2);
  WiFiManagerParameter custom_hol_month("month", "Month", gMonth, 2);
  WiFiManagerParameter custom_hol_year("year", "Year", gYear, 4);
  WiFiManagerParameter custom_hol_where("where", "Where", gWhere, 20);
  WiFiManagerParameter custom_hol_who("who", "Who", gWho, 20);

  // WiFiManager
  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // add all your parameters here
  wifiManager.addParameter(&custom_hol_hour);
  wifiManager.addParameter(&custom_hol_day);
  wifiManager.addParameter(&custom_hol_month);
  wifiManager.addParameter(&custom_hol_year);
  wifiManager.addParameter(&custom_hol_where);
  wifiManager.addParameter(&custom_hol_who);

  boolean wmResult;
  if (configuring) {
    wmResult = wifiManager.startConfigPortal() ;
  } else {
    wmResult = wifiManager.autoConnect();
  }

  if (!wmResult)
  {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    // reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(5000);
  }

  // if you get here you have connected to the WiFi
  Serial.println("Connected to WiFi!");

  // read updated parameters
  String hour =  custom_hol_hour.getValue();
  String day =  custom_hol_day.getValue();
  String month = custom_hol_month.getValue();
  String year = custom_hol_year.getValue();
  String who = custom_hol_who.getValue();
  String where = custom_hol_where.getValue();

  // save the custom parameters to NVR
  if (shouldSaveConfig) writePreferences(hour,day,month,year,who,where);

  // NTP
  configTzTime(MY_TZ, "pool.ntp.org");
  
  Serial.println("setup() complete");
  
}

void loop()
{

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
   Serial.println("Failed to get localtime!");
  } 

  struct tm holtimeinfo;
  memset(&holtimeinfo,0,sizeof(struct tm));
  holtimeinfo.tm_hour=atoi(gHour);
  holtimeinfo.tm_mday=atoi(gDay);
  holtimeinfo.tm_mon=atoi(gMonth)-1;
  holtimeinfo.tm_year=atoi(gYear)-1900; 
  holtimeinfo.tm_isdst=-1;

  time_t rawlocal = mktime(&timeinfo); //1792242810 when developing in oct 2026
  time_t rawhol = mktime(&holtimeinfo);
  double diff_secs = difftime(rawhol, rawlocal);
  int diffDays = int(diff_secs / 86400);
  diff_secs -=  (double) diffDays*86400;
  int diffHours = int(diff_secs / 3600);
  diff_secs -= (double) diffHours*3600;
  int diffMins = int(diff_secs / 60);
  diff_secs -= (double) diffMins*60;
  Serial.printf("%s are going to %s in %d Days %02d Hours %02d Minutes %02d Seconds 🌞\n", gWho, gWhere, diffDays, diffHours, diffMins, int(diff_secs) );


  delay(5000);
}