/*
 * ============================================================================
 *  IMAM BUDDY v3.4 - Low Power Smart Sleep Edition (ESP8266)
 * ============================================================================
 *  Wiring:
 *    OLED SDA -> D2 (GPIO4)      OLED SCL -> D1 (GPIO5)    (addr 0x3C)
 *    Touch wire -> D5 (GPIO14)
 *    Buzzer (+) -> D6 (GPIO12), Buzzer (-) -> GND
 *
 *  Blynk Datastreams:
 *    V0 = String   Incoming Message (Scrolls + Loud Alert, Wakes Pet)
 *    V1 = LED      Unread Indicator (ON on new message, OFF on touch)
 *    V2 = String   Current Date & Day Output ("DD/MM/YYYY Day")
 *    V3 = String   Weather Info Display / Manual Push
 *    V4 = String   Daily Alarm Time Input ("07:30 AM", "19:30", "OFF")
 *    V5 = String   Pet Current Mood Output ("CRY", "SAD", "NORMAL", "HAPPY", "SLEEPING")
 * ============================================================================
 */

#define BLYNK_TEMPLATE_ID    "TMPL6gBnqXvGj"
#define BLYNK_TEMPLATE_NAME "Imam Buddy"
#define BLYNK_AUTH_TOKEN    "PqAVyR_e80ZggsUyAUalvXLkNRq3N5qT"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <BlynkSimpleEsp8266.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NTPClient.h>

extern "C" {
  #include "user_interface.h"
}

// ----------------------------- User Settings --------------------------------
const char* WIFI_SSID = "IMAM";
const char* WIFI_PASS = "imam121#";
#define WEATHER_CITY  "Chattogram"

// ----------------------------- Hardware Pins --------------------------------
#define OLED_W        128
#define OLED_H        64
#define OLED_ADDR     0x3C
#define PIN_SDA       4      // D2
#define PIN_SCL       5      // D1
#define TOUCH_PIN     14     // D5
#define BUZZER_PIN    12     // D6

#define BUZZER_PASSIVE  true 
#define ALERT_FREQ      2700 

// ----------------------------- Timing Constants -----------------------------
const uint32_t MOOD_DECAY_MS      = 5UL * 60UL * 1000UL; 
const uint32_t STANDBY_FACE_MS    = 5000;                
const uint32_t STANDBY_CLOCK_MS   = 3000;                
const uint32_t STANDBY_DATE_MS    = 3000;                
const uint32_t STANDBY_WEATHER_MS = 3000;                
const uint32_t INTERACT_WINDOW    = 10000;               
const uint32_t MSG_READ_WINDOW    = 30000;               
const uint32_t SLEEP_DISP_OFF_MS  = 10000; // Display off 10s after sleep
const uint32_t FRAME_MS           = 50;                  

const uint32_t GESTURE_GAP_MS     = 400;                 
const uint32_t LONG_PRESS_MS      = 2500;                
const uint32_t FX_TRICK_MS        = 3000;
const uint32_t FX_SCARED_MS       = 3000;
const uint32_t FX_WAKE_MS         = 2500;

// ----------------------------- Touch Tuning ---------------------------------
#define TOUCH_SAMPLES          16
#define TOUCH_INTERVAL_MS      15
#define TOUCH_PRESS_SAMPLES    2
#define TOUCH_RELEASE_SAMPLES  2
#define TOUCH_STUCK_MS         15000
const float    TOUCH_RATIO     = 0.30f;
const uint32_t TOUCH_MIN_DELTA = 120;

// ----------------------------- Types & Structures ---------------------------
enum Mood : uint8_t { MOOD_CRY = 0, MOOD_SAD = 1, MOOD_NORMAL = 2, MOOD_HAPPY = 3 };
enum DispMode : uint8_t { DM_STANDBY, DM_INTERACT, DM_MESSAGE, DM_FX, DM_SLEEP, DM_ALARM };
enum FxMode : uint8_t { FX_NONE, FX_TRICK, FX_SCARED, FX_WAKE };
enum WeatherIcon : uint8_t { WI_SUN, WI_PARTLY, WI_CLOUD, WI_RAIN, WI_STORM };
struct Note { uint16_t f; uint16_t d; };

const char* DAYS_OF_WEEK[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// ----------------------------- Melodies -------------------------------------
const Note MEL_TRICK[]      = { {784,80},{988,80},{1319,80},{1568,80},{0,40},{1319,80},{1568,160},{0,120} };
const Note MEL_SCARED[]     = { {2200,50},{1400,50},{2200,50},{1400,50},{2200,50},{1400,50},{0,90} };
const Note MEL_WAKE[]       = { {523,80},{659,80},{784,140} };
#define NOTES(a) ((uint8_t)(sizeof(a) / sizeof(a[0])))

// ----------------------------- Global Objects -------------------------------
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 21600, 60000); // UTC+6

// ----------------------------- State Variables ------------------------------
Mood     mood        = MOOD_CRY;
uint32_t lastDecayMs = 0;
uint32_t lastTouchMs = 0;
bool     hasTouched  = false;
bool     sleeping    = false;
uint32_t sleepStartMs = 0;
bool     displayIsOn = true;

// Alarm System State
int      alarmTimeMinutes = -1; 
bool     alarmTriggered   = false;
int      lastAlarmDay     = -1;

// Effects
FxMode   fxMode       = FX_NONE;
uint32_t fxStartMs    = 0;
uint32_t fxDurationMs = 0;
const Note* fxMelody  = nullptr;

// Message State
String   msg;
bool     msgActive    = false;
bool     msgUnread    = false;
uint32_t msgScrollT0  = 0;
uint32_t msgReadT0    = 0;
uint32_t alertT0      = 0;
int      msgPixelW    = 0;

// Weather State
bool        weatherValid = false;
String      weatherTemp;
String      weatherDesc;
WeatherIcon weatherIcon  = WI_PARTLY;
uint32_t    weatherNextFetchMs = 0;

// Blynk Outbound Sync
int      ledTarget   = 0;
bool     ledSynced   = true;
bool     dateSynced  = false;
bool     moodSynced  = false;
bool     weatherSynced = false;
uint32_t outRetryMs  = 0;

// Buzzer Engine
uint16_t    curFreq    = 0;
const Note* melPtr     = nullptr;
uint8_t     melLen     = 0;
uint8_t     melIdx     = 0;
bool        melLoop    = false;
uint32_t    melNoteEnd = 0;
Note        chirpNote[1];

// Touch State
float    touchBaseline = 0;
uint32_t touchMaxCycles = 0;
uint32_t lastTouchSampleMs = 0;
bool     touchActive = false;
uint8_t  aboveCnt = 0, belowCnt = 0;
uint32_t pressStartMs = 0;
bool     longFired = false;
uint8_t  tapCount = 0;
uint32_t lastTapPressMs = 0;

// Display State
DispMode lastMode = DM_STANDBY;
uint32_t standbyT0 = 0;
uint32_t lastFrameMs = 0;

// Network Timers
uint32_t ntpNextMs = 0;
uint32_t blynkRetryMs = 0;

// Prototypes
void wakeUp(uint32_t now, bool withEffect);
void goToSleep(uint32_t now);
void startEffect(FxMode fx, uint32_t now);
void cancelEffect();
void parseWeather(String s);
void startMelody(const Note* m, uint8_t n, bool loop);
void stopMelodyIf(const Note* m);
void setDisplayPower(bool turnOn);

// ============================================================================
//  DISPLAY POWER CONTROL
// ============================================================================
void setDisplayPower(bool turnOn) {
  if (displayIsOn == turnOn) return;
  displayIsOn = turnOn;
  if (turnOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
  } else {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }
}

// ============================================================================
//  BLYNK HANDLERS
// ============================================================================
BLYNK_CONNECTED() {
  ledSynced  = false;
  dateSynced = false;
  moodSynced = false;
  weatherSynced = false;
  Blynk.syncVirtual(V3);
  Blynk.syncVirtual(V4);
}

BLYNK_WRITE(V0) {
  String s = param.asStr();
  s.trim();
  if (s.length() == 0) return;
  s.replace('\n', ' ');
  s.replace('\r', ' ');
  if (s.length() > 200) s = s.substring(0, 200);

  uint32_t now = millis();
  if (sleeping) wakeUp(now, false);
  setDisplayPower(true);
  cancelEffect();

  msg          = s;
  msgActive    = true;
  msgUnread    = true;
  msgScrollT0  = now;
  alertT0      = now;
  msgPixelW    = msg.length() * 18;

  ledTarget = 255;
  ledSynced = false;
}

BLYNK_WRITE(V3) {
  parseWeather(String(param.asStr()));
}

BLYNK_WRITE(V4) {
  String tStr = param.asStr();
  tStr.trim();
  tStr.toUpperCase();

  if (tStr.length() >= 4 && tStr.indexOf(':') > 0) {
    int colonIdx = tStr.indexOf(':');
    int hh = tStr.substring(0, colonIdx).toInt();
    int mm = tStr.substring(colonIdx + 1, colonIdx + 3).toInt();

    if (tStr.indexOf("PM") > 0) {
      if (hh < 12) hh += 12;
    } else if (tStr.indexOf("AM") > 0) {
      if (hh == 12) hh = 0;
    }

    alarmTimeMinutes = hh * 60 + mm;
  } else {
    alarmTimeMinutes = -1; // OFF
  }
}

// ============================================================================
//  BUZZER & MELODY ENGINE
// ============================================================================
void buzzerTone(uint16_t f) {
  if (f == curFreq) return;
  curFreq = f;
  if (BUZZER_PASSIVE) {
    if (f) tone(BUZZER_PIN, f);
    else { noTone(BUZZER_PIN); digitalWrite(BUZZER_PIN, LOW); }
  } else {
    digitalWrite(BUZZER_PIN, f ? HIGH : LOW);
  }
}

void startMelody(const Note* m, uint8_t n, bool loop) {
  melPtr = m; melLen = n; melIdx = 0; melLoop = loop;
  melNoteEnd = millis() + m[0].d;
  buzzerTone(m[0].f);
}

void stopMelodyIf(const Note* m) {
  if (m && melPtr == m) melPtr = nullptr;
}

void chirpForMood() {
  if (sleeping) return;
  static const uint16_t f[4] = { 500, 900, 1600, 2600 };
  chirpNote[0].f = f[mood];
  chirpNote[0].d = (mood == MOOD_HAPPY) ? 110 : 70;
  startMelody(chirpNote, 1, false);
}

void serviceBuzzer(uint32_t now) {
  if ((msgActive && msgUnread) || alarmTriggered) {
    uint8_t slot = ((now - alertT0) / 100) % 8;
    buzzerTone((slot == 0 || slot == 2 || slot == 4) ? ALERT_FREQ : 0);
    return;
  }
  if (melPtr) {
    if ((int32_t)(now - melNoteEnd) >= 0) {
      melIdx++;
      if (melIdx >= melLen) {
        if (melLoop) melIdx = 0;
        else { melPtr = nullptr; buzzerTone(0); return; }
      }
      melNoteEnd = now + melPtr[melIdx].d;
      buzzerTone(melPtr[melIdx].f);
    }
    return;
  }
  buzzerTone(0);
}

// ============================================================================
//  TOUCH SENSING (Single Wire D5)
// ============================================================================
uint32_t touchSample() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < TOUCH_SAMPLES; i++) {
    pinMode(TOUCH_PIN, OUTPUT);
    digitalWrite(TOUCH_PIN, LOW);
    delayMicroseconds(3);
    noInterrupts();
    pinMode(TOUCH_PIN, INPUT_PULLUP);
    uint32_t t0 = ESP.getCycleCount();
    uint32_t d  = 0;
    while (!digitalRead(TOUCH_PIN)) {
      d = ESP.getCycleCount() - t0;
      if (d > touchMaxCycles) break;
    }
    d = ESP.getCycleCount() - t0;
    interrupts();
    sum += d;
  }
  return sum;
}

void calibrateTouch() {
  touchMaxCycles = (uint32_t)ESP.getCpuFreqMHz() * 40;
  uint32_t acc = 0;
  const uint8_t N = 40;
  for (uint8_t i = 0; i < N; i++) {
    acc += touchSample();
    uint32_t t = millis();
    while (millis() - t < 10) yield();
  }
  touchBaseline = (float)acc / N;
}

void onTouchPress(uint32_t now);
void onTouchRelease(uint32_t now);

void serviceTouch(uint32_t now) {
  if (now - lastTouchSampleMs < TOUCH_INTERVAL_MS) return;
  lastTouchSampleMs = now;

  uint32_t raw = touchSample();
  float thr = touchBaseline * TOUCH_RATIO;
  if (thr < TOUCH_MIN_DELTA) thr = TOUCH_MIN_DELTA;
  bool above = (float)raw > touchBaseline + thr;

  if (!touchActive) {
    if (above) {
      if (++aboveCnt >= TOUCH_PRESS_SAMPLES) {
        aboveCnt = 0; belowCnt = 0;
        touchActive = true;
        onTouchPress(now);
      }
    } else {
      aboveCnt = 0;
      touchBaseline += ((float)raw - touchBaseline) * 0.02f;
    }
  } else {
    bool below = (float)raw < touchBaseline + thr * 0.5f;
    if (below) {
      if (++belowCnt >= TOUCH_RELEASE_SAMPLES) {
        belowCnt = 0; touchActive = false;
        onTouchRelease(now);
      }
    } else {
      belowCnt = 0;
    }
    if (touchActive && (now - pressStartMs) > TOUCH_STUCK_MS) {
      touchBaseline = (float)raw;
      touchActive = false;
      longFired = true;
      belowCnt = 0;
    }
  }
}

// ============================================================================
//  GESTURE HANDLERS
// ============================================================================
void markInteraction(uint32_t now) {
  hasTouched  = true;
  lastTouchMs = now;
  lastDecayMs = now;
}

void readMessage(uint32_t now) {
  msgUnread = false;
  msgReadT0 = now;
  ledTarget = 0;
  ledSynced = false;
}

void onSingleTap(uint32_t now) {
  markInteraction(now);
  if (mood < MOOD_HAPPY) {
    mood = (Mood)(mood + 1);
    moodSynced = false;
  }
  chirpForMood();
}

void onDoubleTap(uint32_t now) {
  markInteraction(now);
  startEffect(FX_TRICK, now);
}

void onRapidTap(uint32_t now) {
  markInteraction(now);
  startEffect(FX_SCARED, now);
}

void resolveGestures(uint32_t now) {
  uint8_t n = tapCount;
  tapCount = 0;
  if (sleeping || n == 0) return;
  if (n == 1)      onSingleTap(now);
  else if (n == 2) onDoubleTap(now);
  else             onRapidTap(now);
}

void onTouchPress(uint32_t now) {
  pressStartMs = now;
  longFired = false;

  setDisplayPower(true);

  if (alarmTriggered) {
    alarmTriggered = false;
    alertT0 = 0;
    buzzerTone(0);
    return;
  }

  if (sleeping) {
    wakeUp(now, true);
    return;
  }

  if (tapCount > 0 && (now - lastTapPressMs) > GESTURE_GAP_MS) resolveGestures(now);
  lastTapPressMs = now;

  if (msgActive && msgUnread) readMessage(now);
}

void onTouchRelease(uint32_t now) {
  (void)now;
  if (sleeping || longFired) return;
  tapCount++;
  if (tapCount >= 4) resolveGestures(millis());
}

void serviceGestures(uint32_t now) {
  if (touchActive && !longFired && (now - pressStartMs) >= LONG_PRESS_MS) {
    longFired = true;
    tapCount = 0;
    if (sleeping) wakeUp(now, true);
    else          goToSleep(now);
  }
  if (!touchActive && tapCount > 0 && (now - lastTapPressMs) > GESTURE_GAP_MS) {
    resolveGestures(now);
  }
}

// ============================================================================
//  SLEEP / WAKE & EFFECTS
// ============================================================================
void goToSleep(uint32_t now) {
  sleeping = true;
  sleepStartMs = now;
  cancelEffect();
  melPtr = nullptr;
  tapCount = 0;
  moodSynced = false;
  if (msgActive) {
    msgActive = false; msgUnread = false; msg = "";
    ledTarget = 0; ledSynced = false;
  }
}

void wakeUp(uint32_t now, bool withEffect) {
  sleeping = false;
  lastDecayMs = now;
  moodSynced = false;
  setDisplayPower(true);
  if (withEffect) {
    hasTouched = true;
    lastTouchMs = now;
    startEffect(FX_WAKE, now);
  }
}

void startEffect(FxMode fx, uint32_t now) {
  cancelEffect();
  fxMode = fx;
  fxStartMs = now;
  switch (fx) {
    case FX_TRICK:  fxDurationMs = FX_TRICK_MS;  fxMelody = MEL_TRICK;  startMelody(MEL_TRICK,  NOTES(MEL_TRICK),  true);  break;
    case FX_SCARED: fxDurationMs = FX_SCARED_MS; fxMelody = MEL_SCARED; startMelody(MEL_SCARED, NOTES(MEL_SCARED), true);  break;
    case FX_WAKE:   fxDurationMs = FX_WAKE_MS;   fxMelody = MEL_WAKE;   startMelody(MEL_WAKE,   NOTES(MEL_WAKE),   false); break;
    default: break;
  }
}

void cancelEffect() {
  stopMelodyIf(fxMelody);
  fxMelody = nullptr;
  fxMode = FX_NONE;
}

void serviceEffects(uint32_t now) {
  if (fxMode != FX_NONE && (now - fxStartMs) >= fxDurationMs) cancelEffect();
}

// ============================================================================
//  ALARM SERVICE
// ============================================================================
void serviceAlarm(uint32_t now) {
  if (timeClient.isTimeSet() && alarmTimeMinutes >= 0) {
    int currentMin = timeClient.getHours() * 60 + timeClient.getMinutes();
    int currentDay = timeClient.getDay();

    if (currentMin == alarmTimeMinutes && lastAlarmDay != currentDay) {
      alarmTriggered = true;
      alertT0 = now;
      lastAlarmDay = currentDay;
      if (sleeping) wakeUp(now, false);
      setDisplayPower(true);
    }
  }
}

// ============================================================================
//  AUTO WEATHER FETCH (wttr.in)
// ============================================================================
void parseWeather(String s) {
  s.trim();
  if (s.length() == 0) { weatherValid = false; return; }
  
  int spaceIdx = s.indexOf(' ');
  if (spaceIdx > 0) {
    weatherTemp = s.substring(0, spaceIdx);
    weatherDesc = s.substring(spaceIdx + 1);
  } else {
    weatherTemp = s;
    weatherDesc = "Clear";
  }
  weatherIcon = WI_SUN;
  weatherValid = true;
}

void fetchLiveWeather(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClient client;
  HTTPClient http;
  String url = "http://wttr.in/Chattogram?format=%t+%C";
  
  if (http.begin(client, url)) {
    http.setTimeout(4000);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      payload.trim();
      payload.replace("+", "");
      payload.replace("°", "");
      
      if (payload.length() > 0 && payload.length() < 30) {
        int spaceIdx = payload.indexOf(' ');
        if (spaceIdx > 0) {
          weatherTemp = payload.substring(0, spaceIdx);
          weatherDesc = payload.substring(spaceIdx + 1);
        } else {
          weatherTemp = payload;
          weatherDesc = "Clear";
        }
        
        String descLower = weatherDesc;
        descLower.toLowerCase();
        if (descLower.indexOf("rain") >= 0 || descLower.indexOf("drizzle") >= 0) weatherIcon = WI_RAIN;
        else if (descLower.indexOf("cloud") >= 0 || descLower.indexOf("overcast") >= 0) weatherIcon = WI_PARTLY;
        else if (descLower.indexOf("thunder") >= 0 || descLower.indexOf("storm") >= 0) weatherIcon = WI_STORM;
        else weatherIcon = WI_SUN;
        
        weatherValid = true;
        weatherSynced = false;
      }
    }
    http.end();
  }
  weatherNextFetchMs = now + 900000UL;
}

void serviceWeatherSync(uint32_t now) {
  if ((int32_t)(now - weatherNextFetchMs) >= 0) {
    fetchLiveWeather(now);
  }
}

// ============================================================================
//  EXACT DATE CONVERSION
// ============================================================================
String getFormattedDateStr() {
  if (!timeClient.isTimeSet()) return "--/--/----";
  
  unsigned long epoch = timeClient.getEpochTime();
  uint32_t days = epoch / 86400UL;
  
  uint32_t y = 1970;
  while (true) {
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    uint16_t daysInYear = leap ? 366 : 365;
    if (days >= daysInYear) {
      days -= daysInYear;
      y++;
    } else {
      break;
    }
  }
  
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  uint8_t monthDays[] = {31, (uint8_t)(leap ? 29 : 28), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  
  uint8_t m = 0;
  for (m = 0; m < 12; m++) {
    if (days >= monthDays[m]) {
      days -= monthDays[m];
    } else {
      break;
    }
  }
  
  uint8_t d = days + 1;
  m = m + 1;
  
  char dateBuf[16];
  snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04lu", d, m, (unsigned long)y);
  return String(dateBuf);
}

void serviceMood(uint32_t now) {
  if (sleeping) { lastDecayMs = now; return; }
  if (mood > MOOD_CRY && (now - lastDecayMs) >= MOOD_DECAY_MS) {
    mood = (Mood)(mood - 1);
    lastDecayMs = now;
    moodSynced = false;
  }
}

void serviceMessage(uint32_t now) {
  if (msgActive && !msgUnread && (now - msgReadT0) >= MSG_READ_WINDOW) {
    msgActive = false;
    msg = "";
  }
}

void serviceBlynkOut(uint32_t now) {
  if (!Blynk.connected() || (int32_t)(now - outRetryMs) < 0) return;
  
  if (!ledSynced) {
    Blynk.virtualWrite(V1, ledTarget);
    ledSynced = true;
    outRetryMs = now + 300;
    return;
  }
  if (!dateSynced && timeClient.isTimeSet()) {
    String dateStr = getFormattedDateStr() + " " + String(DAYS_OF_WEEK[timeClient.getDay()]);
    Blynk.virtualWrite(V2, dateStr);
    dateSynced = true;
    outRetryMs = now + 300;
    return;
  }
  if (!weatherSynced && weatherValid) {
    String wStr = weatherTemp + " " + weatherDesc;
    Blynk.virtualWrite(V3, wStr);
    weatherSynced = true;
    outRetryMs = now + 300;
    return;
  }
  if (!moodSynced) {
    String mStr = "CRY";
    if (sleeping) mStr = "SLEEPING";
    else if (mood == MOOD_SAD) mStr = "SAD";
    else if (mood == MOOD_NORMAL) mStr = "NORMAL";
    else if (mood == MOOD_HAPPY) mStr = "HAPPY";
    
    Blynk.virtualWrite(V5, mStr);
    moodSynced = true;
    outRetryMs = now + 300;
  }
}

void serviceNetwork(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return;

  if ((int32_t)(now - ntpNextMs) >= 0) {
    timeClient.update();
    ntpNextMs = now + (timeClient.isTimeSet() ? 60000UL : 10000UL);
    dateSynced = false;
  }

  if (Blynk.connected()) {
    Blynk.run();
  } else if ((int32_t)(now - blynkRetryMs) >= 0) {
    blynkRetryMs = now + 15000UL;
    Blynk.connect(3000);
  }
}

// ============================================================================
//  DRAWING HELPERS & SCREENS
// ============================================================================
void drawArc(int cx, int cy, int r, int a0, int a1, int thick) {
  for (int a = a0; a <= a1; a += 3) {
    float rad = a * 0.0174533f;
    int x = cx + (int)roundf(cosf(rad) * r);
    int y = cy + (int)roundf(sinf(rad) * r);
    display.fillCircle(x, y, thick, WHITE);
  }
}

void thickLine(int x0, int y0, int x1, int y1, int w) {
  for (int i = 0; i < w; i++) display.drawLine(x0, y0 + i, x1, y1 + i, WHITE);
}

void drawTear(int x, int y) {
  display.fillCircle(x, y, 2, WHITE);
  display.drawPixel(x, y - 3, WHITE);
}

void drawNote(int x, int y) {
  display.fillCircle(x, y, 2, WHITE);
  display.drawLine(x + 2, y, x + 2, y - 8, WHITE);
  display.drawLine(x + 2, y - 8, x + 5, y - 6, WHITE);
}

void drawFaceNormal(uint32_t t) {
  bool blink = (t % 4000) < 150;
  int h = blink ? 4 : 26;
  int ey = 28 - h / 2;
  display.fillRoundRect(26, ey, 22, h, 6, WHITE);
  display.fillRoundRect(80, ey, 22, h, 6, WHITE);
  if (!blink) {
    int off = (int)(sinf(t / 1500.0f) * 4);
    display.fillCircle(37 + off, 28, 5, BLACK);
    display.fillCircle(91 + off, 28, 5, BLACK);
  }
  display.fillRoundRect(52, 51, 24, 4, 2, WHITE);
}

void drawFaceHappy(uint32_t t) {
  int b = (int)(sinf(t / 200.0f) * 2);
  display.fillCircle(64, 42 + b, 15, WHITE);
  display.fillRect(46, 24 + b, 36, 18, BLACK);
  display.fillCircle(64, 56 + b, 5, BLACK);
  display.fillCircle(64, 55 + b, 4, WHITE);
  drawArc(38, 36 + b, 12, 200, 340, 1);
  drawArc(90, 36 + b, 12, 200, 340, 1);
  if ((t / 300) % 2 == 0) {
    display.drawLine(8, 10, 8, 18, WHITE);  display.drawLine(4, 14, 12, 14, WHITE);
    display.drawLine(120, 8, 120, 16, WHITE); display.drawLine(116, 12, 124, 12, WHITE);
  }
}

void drawFaceSad(uint32_t t) {
  display.fillCircle(38, 32, 10, WHITE);
  display.fillCircle(90, 32, 10, WHITE);
  display.fillCircle(38, 35, 5, BLACK);
  display.fillCircle(90, 35, 5, BLACK);
  thickLine(24, 20, 50, 12, 3);
  thickLine(78, 12, 104, 20, 3);
  drawArc(64, 66, 14, 215, 325, 1);
  int ty = 44 + (int)((t / 90) % 14);
  drawTear(98, ty);
}

void drawFaceCry(uint32_t t) {
  for (int i = 0; i < 2; i++) {
    display.drawLine(26 + i, 16, 46 + i, 28, WHITE);
    display.drawLine(46 + i, 28, 26 + i, 40, WHITE);
    display.drawLine(102 - i, 16, 82 - i, 28, WHITE);
    display.drawLine(82 - i, 28, 102 - i, 40, WHITE);
  }
  for (int k = 0; k < 3; k++) {
    int ty = 38 + (int)(((t / 28) + k * 9) % 26);
    drawTear(30, ty);
    drawTear(98, ty);
  }
  int wob = (int)((t / 110) % 2);
  drawArc(64, 63 + wob, 12, 205, 335, 1);
  drawArc(64, 63 + wob, 9, 215, 325, 1);
}

void drawFace(uint32_t t) {
  switch (mood) {
    case MOOD_CRY:    drawFaceCry(t);    break;
    case MOOD_SAD:    drawFaceSad(t);    break;
    case MOOD_NORMAL: drawFaceNormal(t); break;
    case MOOD_HAPPY:  drawFaceHappy(t);  break;
  }
}

void drawFaceTrick(uint32_t t) {
  int dx = (int)(sinf(t / 90.0f) * 8);
  int dy = -(int)fabsf(sinf(t / 130.0f) * 5);
  display.fillCircle(64 + dx, 46 + dy, 13, WHITE);
  display.fillRect(50 + dx, 30 + dy, 28, 16, BLACK);
  display.fillRoundRect(26 + dx, 16 + dy, 22, 22, 6, WHITE);
  display.fillCircle(37 + dx + (int)(sinf(t / 200.0f) * 3), 27 + dy, 5, BLACK);
  drawArc(91 + dx, 34 + dy, 11, 200, 340, 1);
  drawNote(8,   58 - (int)((t / 30) % 44));
  drawNote(114, 58 - (int)(((t / 30) + 22) % 44));
}

void drawFaceScared(uint32_t t) {
  int jx = ((t / 40) % 2) ? 2 : -2;
  int jy = ((t / 55) % 2) ? 1 : -1;
  int pj = ((t / 70) % 2) ? 1 : -1;
  display.fillCircle(36 + jx, 30 + jy, 15, WHITE);
  display.fillCircle(92 + jx, 30 + jy, 15, WHITE);
  display.fillCircle(36 + jx + pj, 30 + jy, 3, BLACK);
  display.fillCircle(92 + jx - pj, 30 + jy, 3, BLACK);
  thickLine(22 + jx, 10, 48 + jx, 5, 2);
  thickLine(80 + jx, 5, 106 + jx, 10, 2);
  for (int i = 0; i < 5; i++) {
    int y0 = (i % 2) ? 58 : 52;
    int y1 = (i % 2) ? 52 : 58;
    display.drawLine(44 + i * 8 + jx, y0 + jy, 52 + i * 8 + jx, y1 + jy, WHITE);
    display.drawLine(44 + i * 8 + jx, y0 + jy + 1, 52 + i * 8 + jx, y1 + jy + 1, WHITE);
  }
  drawTear(120, 14 + (int)((t / 60) % 12));
}

void drawFaceWake(uint32_t e) {
  uint32_t ec = (e > 1200) ? 1200 : e;
  int h = 3 + (int)(ec * 23UL / 1200UL);
  int ey = 28 - h / 2;
  display.fillRoundRect(26, ey, 22, h, 6, WHITE);
  display.fillRoundRect(80, ey, 22, h, 6, WHITE);
  if (h > 14) {
    display.fillCircle(37, 28, 5, BLACK);
    display.fillCircle(91, 28, 5, BLACK);
  }
  if (e < 1500) {
    int mh = 4 + (int)(12.0f * sinf(3.14159f * e / 1500.0f));
    display.fillRoundRect(54, 62 - mh - 6, 20, mh + 4, 6, WHITE);
  } else {
    drawArc(64, 42, 12, 35, 145, 1);
  }
  if (e < 1900) {
    thickLine(14, 62, 4, 34, 2);
    thickLine(113, 62, 123, 34, 2);
    display.fillCircle(4, 32, 3, WHITE);
    display.fillCircle(123, 32, 3, WHITE);
  }
}

void drawSleep(uint32_t t) {
  drawArc(38, 30, 10, 20, 160, 1);
  drawArc(90, 30, 10, 20, 160, 1);
  int r = 2 + (int)((sinf(t / 600.0f) + 1.0f) * 1.5f);
  display.drawCircle(64, 50, r, WHITE);
  for (int k = 0; k < 3; k++) {
    int ph = (int)(((t / 50) + k * 15) % 45);
    int x = 76 + k * 10 + ph / 3;
    int y = 44 - ph;
    display.setTextColor(WHITE);
    display.setTextSize(k == 0 ? 1 : 2);
    display.setCursor(x, y);
    display.print(k == 0 ? 'z' : 'Z');
  }
}

void drawClock(uint32_t t) {
  char buf[8];
  char ampm[3];

  if (timeClient.isTimeSet()) {
    bool colon = ((t / 500) % 2) == 0;
    int hours24 = timeClient.getHours();
    int hours12 = hours24 % 12;
    if (hours12 == 0) hours12 = 12;

    snprintf(ampm, sizeof(ampm), "%s", (hours24 >= 12) ? "PM" : "AM");
    snprintf(buf, sizeof(buf), "%2d%c%02d", hours12, colon ? ':' : ' ', timeClient.getMinutes());
  } else {
    snprintf(buf, sizeof(buf), "--:--");
    snprintf(ampm, sizeof(ampm), "  ");
  }

  display.setTextSize(3);
  display.setTextColor(WHITE);
  display.setCursor(4, 24);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(102, 36);
  display.print(ampm);

  display.setCursor(31, 56);
  display.print("Imam Buddy");
}

void drawDateScreen(uint32_t t) {
  (void)t;
  display.setTextColor(WHITE);
  
  display.setTextSize(2);
  String dayStr = timeClient.isTimeSet() ? String(DAYS_OF_WEEK[timeClient.getDay()]) : "----";
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(dayStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((OLED_W - w) / 2, 12);
  display.print(dayStr);

  display.setTextSize(2);
  String dateStr = getFormattedDateStr();
  display.getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((OLED_W - w) / 2, 34);
  display.print(dateStr);

  display.setTextSize(1);
  display.setCursor(31, 56);
  display.print("Imam Buddy");
}

void drawCloud(int cx, int cy, int g, uint16_t color) {
  display.fillCircle(cx - 9,  cy + 3, 6 + g, color);
  display.fillCircle(cx + 1,  cy - 3, 9 + g, color);
  display.fillCircle(cx + 11, cy + 3, 6 + g, color);
  display.fillRect(cx - 9 - g, cy + 3, 21 + 2 * g, 6 + g, color);
}

void drawSunRays(int cx, int cy, int r1, int r2, uint32_t t) {
  int rot = (int)((t / 40) % 360);
  for (int i = 0; i < 8; i++) {
    float a = (rot + i * 45) * 0.0174533f;
    display.drawLine(cx + (int)(cosf(a) * r1), cy + (int)(sinf(a) * r1),
                     cx + (int)(cosf(a) * r2), cy + (int)(sinf(a) * r2), WHITE);
  }
}

void drawWeatherIcon(WeatherIcon ic, int cx, int cy, uint32_t t) {
  switch (ic) {
    case WI_SUN:
      display.fillCircle(cx, cy, 9, WHITE);
      drawSunRays(cx, cy, 13, 18, t);
      break;
    case WI_PARTLY:
      display.fillCircle(cx + 7, cy - 7, 7, WHITE);
      drawSunRays(cx + 7, cy - 7, 10, 14, t);
      drawCloud(cx - 2, cy + 7, 2, BLACK);
      drawCloud(cx - 2, cy + 7, 0, WHITE);
      break;
    case WI_CLOUD:
      drawCloud(cx + (int)(sinf(t / 700.0f) * 2), cy, 0, WHITE);
      break;
    case WI_RAIN:
      drawCloud(cx, cy - 6, 0, WHITE);
      for (int k = 0; k < 3; k++) {
        int x = cx - 8 + k * 9;
        int y = cy + 8 + (int)(((t / 70) + k * 4) % 12);
        display.drawLine(x, y, x - 2, y + 4, WHITE);
      }
      break;
    case WI_STORM:
      drawCloud(cx, cy - 6, 0, WHITE);
      if (((t / 200) % 4) != 3) {
        for (int i = 0; i < 2; i++) {
          display.drawLine(cx + 3 + i, cy + 5, cx - 3 + i, cy + 13, WHITE);
          display.drawLine(cx - 3 + i, cy + 13, cx + 1 + i, cy + 13, WHITE);
          display.drawLine(cx + 1 + i, cy + 13, cx - 4 + i, cy + 23, WHITE);
        }
      }
      break;
  }
}

void drawWeather(uint32_t t) {
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(WEATHER_CITY);

  drawWeatherIcon(weatherIcon, 26, 34, t);

  String temp = weatherTemp.length() ? weatherTemp : String("--");
  display.setTextSize(temp.length() <= 3 ? 3 : 2);
  display.setCursor(60, temp.length() <= 3 ? 18 : 22);
  display.print(temp);

  display.setTextSize(1);
  display.setCursor(60, 48);
  display.print(weatherDesc);
}

void drawAlarmScreen(uint32_t now) {
  if ((now / 300) % 2 == 0) {
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.setCursor(24, 10);
    display.print("ALARM!");
  }
  display.setTextSize(1);
  display.setCursor(20, 35);
  display.print("Wake Up Imam!");
  display.setCursor(12, 52);
  display.print("Touch wire to stop");
}

void drawMessage(uint32_t now) {
  display.setTextWrap(false);
  display.setTextColor(WHITE);
  display.setTextSize(1);

  if (msgUnread) {
    if ((now / 400) % 2 == 0) {
      display.setCursor(16, 0);
      display.print("** NEW MESSAGE **");
    }
    display.setCursor(22, 56);
    display.print("Touch to silence");
  } else {
    uint32_t left = (MSG_READ_WINDOW - (now - msgReadT0) + 999) / 1000;
    display.setCursor(0, 0);
    display.print("Message");
    display.setCursor(86, 0);
    display.printf("%2lus left", (unsigned long)left);
    display.setCursor(31, 56);
    display.print("Imam Buddy");
  }

  int span = OLED_W + msgPixelW;
  int x = OLED_W - (int)((((now - msgScrollT0) * 90UL) / 1000UL) % (uint32_t)span);
  display.setTextSize(3);
  display.setCursor(x, 22);
  display.print(msg);
}

void drawEffect(uint32_t now) {
  uint32_t e = now - fxStartMs;
  switch (fxMode) {
    case FX_TRICK:  drawFaceTrick(now);  break;
    case FX_SCARED: drawFaceScared(now); break;
    case FX_WAKE:   drawFaceWake(e);     break;
    default: break;
  }
}

// ============================================================================
//  DISPLAY SERVICE
// ============================================================================
void serviceDisplay(uint32_t now) {
  if (sleeping) {
    if (now - sleepStartMs >= SLEEP_DISP_OFF_MS) {
      setDisplayPower(false);
      return;
    }
  } else {
    setDisplayPower(true);
  }

  if (!displayIsOn) return;
  if (now - lastFrameMs < FRAME_MS) return;
  lastFrameMs = now;

  DispMode mode;
  if (alarmTriggered)                                           mode = DM_ALARM;
  else if (sleeping)                                            mode = DM_SLEEP;
  else if (fxMode != FX_NONE)                                   mode = DM_FX;
  else if (msgActive)                                           mode = DM_MESSAGE;
  else if (hasTouched && (now - lastTouchMs) < INTERACT_WINDOW) mode = DM_INTERACT;
  else                                                          mode = DM_STANDBY;

  if (mode != lastMode) {
    if (mode == DM_STANDBY) standbyT0 = now;
    lastMode = mode;
  }

  display.clearDisplay();
  switch (mode) {
    case DM_ALARM:    drawAlarmScreen(now); break;
    case DM_SLEEP:    drawSleep(now);       break;
    case DM_FX:       drawEffect(now);      break;
    case DM_MESSAGE:  drawMessage(now);     break;
    case DM_INTERACT: drawFace(now);        break;
    case DM_STANDBY: {
      uint32_t tClock = STANDBY_FACE_MS + STANDBY_CLOCK_MS;
      uint32_t tDate  = tClock + STANDBY_DATE_MS;
      uint32_t cycle  = tDate + (weatherValid ? STANDBY_WEATHER_MS : 0);

      uint32_t ph = (now - standbyT0) % cycle;
      if (ph < STANDBY_FACE_MS)      drawFace(now);
      else if (ph < tClock)          drawClock(now);
      else if (ph < tDate)           drawDateScreen(now);
      else                           drawWeather(now);
      break;
    }
  }
  display.display();
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Set Light Sleep for ESP8266 Modem
  wifi_set_sleep_type(LIGHT_SLEEP_T);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 init failed"));
    for (;;) { yield(); }
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(18, 22);
  display.print("Imam Buddy waking");
  display.setCursor(18, 36);
  display.print("Don't touch me...");
  display.display();

  calibrateTouch();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Blynk.config(BLYNK_AUTH_TOKEN);
  timeClient.begin();

  uint32_t now = millis();
  lastDecayMs = now;
  lastTouchMs = now;
  standbyT0   = now;
}

void loop() {
  uint32_t now = millis();
  serviceNetwork(now);
  serviceTouch(now);
  serviceGestures(now);
  serviceMood(now);
  serviceMessage(now);
  serviceAlarm(now);
  serviceWeatherSync(now);
  serviceEffects(now);
  serviceBuzzer(now);
  serviceBlynkOut(now);
  serviceDisplay(now);

  // Power Saving Delay during Display-OFF period
  if (sleeping && !displayIsOn) {
    delay(20);
  }
}
