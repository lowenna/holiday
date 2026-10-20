#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <SPI.h>
#include <TFT_eSPI.h>


// Custom settings for next holiday when not setup at all.
// If already in NVR, overwritten by that.
// Think can clear with prefs.begin(); prefs.remove("hol"); prefs.end();  Haven't tested
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
const char* MY_TZ = "GMT0BST,M3.5.0/1,M10.5.0";  

// Define console dimensions (matching your 320x200 display)
#define CONSOLE_WIDTH  320
#define CONSOLE_HEIGHT 200
#define FONT_HEIGHT    48 // Default font 1 is 8 pixels high
// 2 is 16, 4 is 24, 6 is numbers only 48px

TFT_eSPI tft = TFT_eSPI();

#define SPRITE_CURSOR_X 4  // Inside the sprite
#define SPRITE_CURSOR_Y 12
#define SPRITE_GAP 80 // Between start of left boundary between boxes
#define SPRITE_Y_TOP 64
#define SPRITE_WIDTH 70
#define SPRITE_HEIGHT 72 // With a 48px font height within, so 72-48 = 24. 24/2 (top/bottom), so 12 offset from top

void displayTimeLeft(int days, int hours, int mins, int secs ) {

  // Top area
  tft.setTextColor(TFT_GREEN);
  tft.drawLine(0,16,319,16,TFT_WHITE);
  tft.fillRect(0,15,319,15,TFT_BLACK);
  tft.drawString("Top status stuff here", 0, 0, 2);
  
  // Bottom area
  tft.drawLine(0,218,319,218,TFT_WHITE);
  tft.fillRect(0,219,319,239,TFT_BLACK);
  tft.drawString("Status stuff will go here",0,221,2);

  // For text elements
  TFT_eSprite t = TFT_eSprite(&tft);
  t.setColorDepth(8);
  t.setTextDatum(CC_DATUM);
  t.setTextColor(TFT_WHITE); 
  t.createSprite(SPRITE_WIDTH, 20); // 2px padding top/bottom for 16px font
 
  t.drawCentreString("Days", SPRITE_WIDTH/2, 0, 2);
  t.pushSprite(5,SPRITE_Y_TOP+SPRITE_HEIGHT, TFT_BLACK);

  t.fillScreen(TFT_BLACK);
  t.drawCentreString("Hours", SPRITE_WIDTH/2, 0, 2);
  t.pushSprite(SPRITE_GAP - SPRITE_WIDTH + SPRITE_GAP - 5,SPRITE_Y_TOP+SPRITE_HEIGHT, TFT_BLACK);

  t.fillScreen(TFT_BLACK);
  t.drawCentreString("Minutes", SPRITE_WIDTH/2, 0, 2);
  t.pushSprite(SPRITE_GAP - SPRITE_WIDTH + (SPRITE_GAP*2) - 5,SPRITE_Y_TOP+SPRITE_HEIGHT, TFT_BLACK);

  t.fillScreen(TFT_BLACK);
  t.drawCentreString("Seconds", SPRITE_WIDTH/2, 0, 2);
  t.pushSprite(SPRITE_GAP - SPRITE_WIDTH + (SPRITE_GAP*3) - 5,SPRITE_Y_TOP+SPRITE_HEIGHT, TFT_BLACK);


  int x = 5;
  TFT_eSprite s = TFT_eSprite(&tft);
  s.setColorDepth(8); 
  s.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  s.setTextSize(1);
  s.setTextFont(7); 
  s.createSprite(SPRITE_WIDTH, SPRITE_HEIGHT);
  

  s.fillSprite(TFT_DARKCYAN);
  s.setCursor(SPRITE_CURSOR_X, SPRITE_CURSOR_Y);
  s.printf("%02d", days);
  s.pushSprite(x, SPRITE_Y_TOP);

  x += SPRITE_GAP;
  s.fillSprite(TFT_DARKCYAN);
  s.setCursor(SPRITE_CURSOR_X, SPRITE_CURSOR_Y);
  s.printf("%02d", hours);
  s.pushSprite(x,SPRITE_Y_TOP);

  x += SPRITE_GAP;
  s.fillSprite(TFT_DARKCYAN);
  s.setCursor(SPRITE_CURSOR_X, SPRITE_CURSOR_Y);
  s.printf("%02d", mins);
  s.pushSprite(x,SPRITE_Y_TOP);

  x += SPRITE_GAP;
  s.fillSprite(TFT_DARKCYAN);
  s.setCursor(SPRITE_CURSOR_X, SPRITE_CURSOR_Y);
  s.printf("%02d", secs);
  s.pushSprite(x,SPRITE_Y_TOP);

}

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
  } while ((millis() - startAt < 1000) &(!pressed));
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

  // Initialize the physical display
  tft.init();
  tft.setRotation(1); // Set landscape orientation
  tft.fillScreen(TFT_BLACK);


  // So use setTextFont(7) for days/hours/mins left. Is like a 7-segment display
  //spriteDays.setTextFont(4);
  //displayConsoleLog("Days      Hours     Mins");

  bool configuring = areWeConfiguring();

  readPreferences() ;

  WiFiManagerParameter custom_hol_hour("hour", "Hour", gHour, 2);
  WiFiManagerParameter custom_hol_day("day", "Day", gDay, 2);
  WiFiManagerParameter custom_hol_month("month", "Month", gMonth, 2);
  WiFiManagerParameter custom_hol_year("year", "Year", gYear, 4);
  WiFiManagerParameter custom_hol_where("where", "Where", gWhere, 20);
  WiFiManagerParameter custom_hol_who("who", "Who", gWho, 20);

  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // custom parameters
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

  // Build a struct tm containing date/time when going on holiday
  struct tm holtimeinfo;
  memset(&holtimeinfo,0,sizeof(struct tm));
  holtimeinfo.tm_hour=atoi(gHour);
  holtimeinfo.tm_mday=atoi(gDay);
  holtimeinfo.tm_mon=atoi(gMonth)-1;
  holtimeinfo.tm_year=atoi(gYear)-1900; 
  holtimeinfo.tm_isdst=-1;

  // Convert our struct tms to time_t. Can use ctime() on these to print if necessary
  time_t rawlocal = mktime(&timeinfo); 
  time_t rawhol = mktime(&holtimeinfo);

  // Now time_t raw* can be subtracted to get seconds between
  double diff_secs = difftime(rawhol, rawlocal);

  // Now a question of getting days, hours, mins and remaining seconds
  int diffDays = int(diff_secs / 86400);
  diff_secs -=  (double) diffDays*86400;
  int diffHours = int(diff_secs / 3600);
  diff_secs -= (double) diffHours*3600;
  int diffMins = int(diff_secs / 60);
  diff_secs -= (double) diffMins*60;

  displayTimeLeft(diffDays, diffHours, diffMins, diff_secs);
  // Debug but can go to screen.
  //Serial.printf("%s are going to %s in %d Days %02d Hours %02d Minutes %02d Seconds 🌞\n", gWho, gWhere, diffDays, diffHours, diffMins, int(diff_secs) );

  // And around we go again
  delay(1000);
}