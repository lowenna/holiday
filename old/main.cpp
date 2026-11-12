#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <XPT2046_Touchscreen.h>
#include "audio.h"


// GPIOs for AZ Touch. Self explanatory.
#define AZTOUCH_BEEPER 21
#define AZTOUCH_TOUCH_CS 14
#define AZTOUCH_TOUCH_IRQ 27

XPT2046_Touchscreen touch(AZTOUCH_TOUCH_CS, AZTOUCH_TOUCH_IRQ);
TS_Point touchPoint;

// AZ-Touch MOD V01-03, 2.8" display only.
// These are the proven fixed values from the AZ_TOUCH_MOD_BIG_TFT
// branch of the Zihatec code-lock example. No user calibration needed.
#define TOUCH_MIN_X     370
#define TOUCH_MIN_Y     470
#define TOUCH_MAX_X    3700
#define TOUCH_MAX_Y    3600
#define TOUCH_MIN_Z      10

static bool touchWasDown = false;

// Returns display pixel coordinates directly: x=0..319, y=0..239.
// Uses the same fixed pressure threshold and raw limits as the proven
// AZ-Touch MOD 2.8" code-lock example. No runtime calibration.
static bool getTouchPixel(uint16_t &x, uint16_t &y) {
    touchPoint = touch.getPoint();
    delay(1);

    if (touchPoint.z <= TOUCH_MIN_Z) return false;

    // AZ_TOUCH_MOD_BIG_TFT raw conversion, adapted directly to
    // the LCARS 320x240 landscape coordinate system.
    long px = map((long)touchPoint.x, TOUCH_MIN_X, TOUCH_MAX_X, 320, 0);
    long py = map((long)touchPoint.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 240, 0);

    px = constrain(px, 0L, 319L);
    py = constrain(py, 0L, 239L);
    x = (uint16_t)px;
    y = (uint16_t)py;
    return true;
}




// To get background image
// 1. google, save
// 2. image resizer (powertoys for Windows) to 320x240  (320x209 should be fine)
// 3. xxd -i <file.jpg> (use git bash or possibly wsl, I used git bash)
// 4. paste hex into maldives.h
#include "maldives.h" // Hexdump of JPG background

// Custom settings for next holiday when not setup at all.
// If already in NVR, overwritten by that.
// Think can clear with prefs.begin(); prefs.remove("hol"); prefs.end();  Haven't tested
char gHour[3] = "10";
char gMins[3] = "0";
char gDay[3] = "5";
char gMonth[3] = "12";
char gYear[5] = "2026";
char gWhere[21] = "Maldives";

// Global value calculated once for when going on holiday
time_t rawhol;

// Callback from Wifimanager sets this flag to indicate data needs to be saved
bool shouldSaveConfig = false;

// Preferences from NVR
Preferences prefs;

// For NTP
const char* MY_TZ = "GMT0BST,M3.5.0/1,M10.5.0";  

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite countdownSprite = TFT_eSprite(&tft); 


// JJH - same as tft_output except stops at 209 rows
// This function is called by the decoder for every block of pixels (MCU)
bool tft_output2(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  // Stop drawing if the image goes off the bottom of the screen
  //if (y >= tft.height()) return false;
  if (y >= 209) return false;
  // Push the decoded block of pixels to the TFT screen
  tft.pushImage(x, y, w, h, bitmap);
  return true; // Return true to keep decoding
}

// JJH - same as tft_output except stops at 48 rows and pushes to a countdownSprite, not the TFT
// This function is called by the decoder for every block of pixels (MCU)
bool tft_output3(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  // Stop drawing if the image goes off the bottom of the screen
  //if (y >= tft.height()) return false;
  if (y >= 48) return false;
  // Push the decoded block of pixels to the TFT screen
  countdownSprite.pushImage(x, y, w, h, bitmap);
  return true; // Return true to keep decoding
}


/*
 SPRITE AREA
   0--15    Current time 16 px tall     // displayCurrentTimeSprite()
   16       Gap                         // implied
   17       Horizontal line             // displayLines()
   18       Gap                         // implied
   19..228  Picture, where & how long left area. This is 209 px tall
 
 FIXED AREA  
  229      Gap                          // implied
  230      Horizontal line              // displayLines()
  231      Gap                          // implied
  232..239 SSID 8px tall on left, When 8px tall right-aligned  
*/

void displayCurrentTimeSprite() {
  TFT_eSprite s = TFT_eSprite(&tft);
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  time_t rawLocal = mktime(&timeinfo); 
  s.setColorDepth(8); 
  s.setTextColor(TFT_GREEN);//, TFT_DARKCYAN);
  s.setTextSize(1);
  s.createSprite(320, 16);
  s.drawCentreString(ctime(&rawLocal), (320/2), 0, 2);
  s.pushSprite(0,0);
  s.deleteSprite();
}

void displayLines() {
  #define TOP_LINE 16+1       //16 being text height. 
  #define BOT_LINE 239-16-1
  tft.drawLine(0,TOP_LINE,319,TOP_LINE,TFT_WHITE);
  tft.drawLine(0,BOT_LINE,319,BOT_LINE,TFT_WHITE);
}

void displaySSIDAndWhen() {
  tft.setTextColor(TFT_GREEN);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(WiFi.SSID(),0,239-16,2);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(ctime(&rawhol),319,239-16,2);
}

void drawStaticBackgroundImage() {
  //return; // BUGBUG HACK for now
  TJpgDec.setCallback(tft_output2);   // Point to our screen render function
  TJpgDec.drawJpg(0, 24, maldives__esp32__jpg, sizeof(maldives__esp32__jpg));
}

void drawWhere() {
    // For text elements
  TFT_eSprite t = TFT_eSprite(&tft);
  //t.setColorDepth(8);
  t.setTextDatum(CC_DATUM);
  t.fillScreen(TFT_BLACK);
  t.setTextColor(TFT_WHITE); 
  t.createSprite(320, 26); // Font 4 is 26px high
  t.drawCentreString(gWhere, 320/2, 0, 4);
  t.pushSprite(0, 35, TFT_BLACK);
  t.deleteSprite();
}

// OK, needs some explaining. I can't have a straight background image as would get overwritten by text unless has solid block behind.
// So, instead the background picture is originally painted static once during setup.
// For the time left, we have a sprite offset to overlay onto the backround with the same image so no "tear" visible
// This sprite is only tall enough to fit the DD HH MM SS bit.
void drawCountdownLeft(int days, int hours, int mins, int secs) {
  countdownSprite.setColorDepth(16); 
  countdownSprite.createSprite(320,48);
  countdownSprite.setSwapBytes(true);
  countdownSprite.fillSprite(TFT_BLACK);
  TJpgDec.setCallback(tft_output3);   // Point to our screen render function
  TJpgDec.drawJpg(0, 24-150, maldives__esp32__jpg, sizeof(maldives__esp32__jpg)); // 24 is top of static background. 150 is top row of sprite location


  static bool colon = false;
  countdownSprite.setTextColor(TFT_DARKCYAN);
  countdownSprite.setTextSize(1);
  countdownSprite.setTextFont(7); 

  int x = 15;
  countdownSprite.setCursor(x,0);
  countdownSprite.printf("%02d.", days);

  colon=!colon;
  x+=80;
  countdownSprite.setCursor(x,0);
  countdownSprite.printf("%02d%c", hours, (colon?':':'\0'));

  x+=75;
  countdownSprite.setCursor(x,0);
  countdownSprite.printf("%02d%c", mins, (colon?':':'\0'));

  x+=75;
  countdownSprite.setCursor(x,0);
  countdownSprite.printf("%02d", secs);

  countdownSprite.pushSprite(0, 150, TFT_BLACK);
  countdownSprite.deleteSprite();
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

    // For text elements

  Serial.println("5 sec countdown. Press outside button for 1 second to configure");

  tft.fillScreen(TFT_BLACK);
  //tft.setTextDatum(CC_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.println("To re-configure, tap this button within 5 seconds.");
  tft.println(" - Take the back cover off to find it!");
  tft.println(" - It's tiny and black-on-silver, not yellow.");
  tft.println(" - It's next to the micro-USB power connector.");
  tft.println(" - You will need a phone to complete this...");

  // Outside
  tft.drawRect(50,100,220,110, TFT_WHITE);
  tft.fillRect(51,101,218,108, TFT_DARKGREEN);

  // Top row of pins
  int x = 57;
  while(x < 270) {
  tft.drawCircle(x,107,3, TFT_SILVER);
  x += 12;
  }

  // Bottom row of pins
  x = 57;
  while(x < 270) {
  tft.drawCircle(x,203,3, TFT_SILVER);
  x += 12;
  }

  tft.fillRect(51,120,70,70, TFT_DARKGREY); // The ESP32 chip
  tft.fillRect(270-31,140,30,30, TFT_DARKGREY); // The USB power
  tft.fillRect(250,180,10,10,TFT_DARKGREY); // The yellow button
  tft.fillRect(250,120,10,10, TFT_YELLOW); // The reset button
  tft.setCursor(60,147);
  tft.setTextColor(TFT_BLACK);
  tft.print("ESP32");
  tft.setCursor(242,147);
  tft.print("USB");

  tft.setCursor(212,120);
  tft.setTextColor(TFT_YELLOW);
  tft.print("Here!");
  tft.setCursor(0,220);
  tft.setTextColor(TFT_YELLOW);
  tft.print("(C) John Howard 2026");
    do {
    if (0 == digitalRead(0)) pressed = true;
    delay(100);
    
    //Serial.printf("%d %d\n", int((millis()-startAt)/1000), pressed); 
  } while ((millis() - startAt < 5000) &(!pressed));

  tft.fillScreen(TFT_BLACK);

  return pressed;
}

void readPreferences() {
  prefs.begin("hol",true);
  String lHour = prefs.getString("hour","");
  String lMins = prefs.getString("mins","");
  String lDay = prefs.getString("day","");
  String lMonth = prefs.getString("month","");
  String lYear = prefs.getString("year","");
  String lWhere = prefs.getString("where","");
  prefs.end();

  if ((lHour.length() > 0) && (strcmp(lHour.c_str(), gHour))) strcpy(gHour, lHour.c_str());
  if ((lMins.length() > 0) && (strcmp(lMins.c_str(), gMins))) strcpy(gMins, lMins.c_str());
  if ((lDay.length() > 0) && (strcmp(lDay.c_str(), gDay))) strcpy(gDay, lDay.c_str());
  if ((lMonth.length() > 0) && (strcmp(lMonth.c_str(), gMonth))) strcpy(gMonth, lMonth.c_str());
  if ((lYear.length() > 0) && (strcmp(lYear.c_str(), gYear))) strcpy(gYear, lYear.c_str());
  if ((lWhere.length() > 0) && (strcmp(lWhere.c_str(), gWhere))) strcpy(gWhere,lWhere.c_str());   

  Serial.printf("Pref: At %02s:%02s on %02s/%02s/%04s - %s\n", gHour, gMins, gDay, gMonth, gYear, gWhere);
}

void writePreferences(String hour, String mins, String day, String month, String year, String where) {
  prefs.begin("hol", false);
  prefs.putString("hour", hour);
  prefs.putString("mins", mins);
  prefs.putString("day", day);
  prefs.putString("month", month);
  prefs.putString("year", year);
  prefs.putString("where", where);
  prefs.end();
  ESP.restart();
}

void setup()
{


  LcarsAudio::begin(AZTOUCH_BEEPER);
  LcarsAudio::beepBoot();
  while (millis() < 400) { LcarsAudio::update(); };

  Serial.begin(115200);
  while (!Serial)
    continue;

  // Initialize the physical display
  tft.init();
  tft.setRotation(1); // Set landscape orientation
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true); // THIS IS CRITICAL OTHERWISE BACKGROUND IMAGE DOESN'T RENDER PROPERLY AT ALL!!!!
 


  bool configuring = areWeConfiguring();

  readPreferences() ;

  WiFiManagerParameter custom_hol_hour("hour", "Hour", gHour, 2);
  WiFiManagerParameter custom_hol_mins("mins", "Mins", gMins, 2);
  WiFiManagerParameter custom_hol_day("day", "Day", gDay, 2);
  WiFiManagerParameter custom_hol_month("month", "Month", gMonth, 2);
  WiFiManagerParameter custom_hol_year("year", "Year", gYear, 4);
  WiFiManagerParameter custom_hol_where("where", "Where", gWhere, 20);

  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // custom parameters
  wifiManager.addParameter(&custom_hol_hour);
  wifiManager.addParameter(&custom_hol_mins);
  wifiManager.addParameter(&custom_hol_day);
  wifiManager.addParameter(&custom_hol_month);
  wifiManager.addParameter(&custom_hol_year);
  wifiManager.addParameter(&custom_hol_where);

  boolean wmResult;
  if (configuring) {

  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setCursor(0,0);
  tft.setTextColor(TFT_WHITE);
  tft.println("Starting setup...");
  tft.println(" - On your iPhone, go to Settings, WiFi");
  tft.println(" - Connect to ESP_xxxxxxxxxx");
  tft.println(" - Wait a few seconds for captive portal");
  tft.println(" - Tap the 'Configure' button");
  tft.println(" - Select WiFi network");
  tft.println(" - You must re-enter password (soz)");
  tft.println(" - Scroll down further");
  tft.println(" - Enter/update the holiday date/time/where");
  tft.println(" - Scroll down further and tap 'Save'");
  tft.println("IMPORTANT");
  tft.println(" - CHECK ALL FIELDS");
  tft.println(" - It may crash if you eg enter 13 for month");
  tft.println(" - Year MUST be 4 digits");
  
    wmResult = wifiManager.startConfigPortal() ;
  } else {
    wmResult = wifiManager.autoConnect();
  }

  tft.fillScreen(TFT_BLACK);


  if (!wmResult)
  {
    tft.setCursor(0,0);
    tft.println("Failed to connect to WiFi. Restarting...");
    Serial.println("failed to connect and hit timeout");
    delay(5000);
    // reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(5000);
  }

  // read updated parameters
  String hour =  custom_hol_hour.getValue();
  String mins = custom_hol_mins.getValue();
  String day =  custom_hol_day.getValue();
  String month = custom_hol_month.getValue();
  String year = custom_hol_year.getValue();
  String where = custom_hol_where.getValue();

  // save the custom parameters to NVR
  if (shouldSaveConfig) writePreferences(hour,mins,day,month,year,where);

  // NTP
  configTzTime(MY_TZ, "pool.ntp.org");

  // Store globally when going on holiday
  struct tm holtimeinfo;
  memset(&holtimeinfo,0,sizeof(struct tm));
  holtimeinfo.tm_hour=atoi(gHour);
  holtimeinfo.tm_min=atoi(gMins);
  holtimeinfo.tm_mday=atoi(gDay);
  holtimeinfo.tm_mon=atoi(gMonth)-1;
  holtimeinfo.tm_year=atoi(gYear)-1900; 
  holtimeinfo.tm_isdst=-1;

  rawhol = mktime(&holtimeinfo);

  displayLines();
  displaySSIDAndWhen();
  drawStaticBackgroundImage();
  drawWhere();

  Serial.println("setup() complete");  
}

void loop()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
   Serial.println("Failed to get localtime!");
  } 


  // Convert our struct tms to time_t. Can use ctime() on these to print if necessary
  time_t rawlocal = mktime(&timeinfo); 

  // Now time_t raw* can be subtracted to get seconds between
  double diff_secs = difftime(rawhol, rawlocal);
  if (diff_secs < 0) diff_secs = 0 ;

  // Now a question of getting days, hours, mins and remaining seconds
  int diffDays = int(diff_secs / 86400);
  diff_secs -=  (double) diffDays*86400;
  int diffHours = int(diff_secs / 3600);
  diff_secs -= (double) diffHours*3600;
  int diffMins = int(diff_secs / 60);
  diff_secs -= (double) diffMins*60;

  drawCountdownLeft(diffDays, diffHours, diffMins, diff_secs);
  displayCurrentTimeSprite();

  // And around we go again
  delay(250);
}