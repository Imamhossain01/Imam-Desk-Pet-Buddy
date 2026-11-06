/*
 * ============================================================================
 *  IMAM BUDDY v7.3 - SOS Panic + WiFi Repeater + Status Icon (ESP8266)
 * ============================================================================
 *  Board: "NodeMCU 1.0 (ESP-12E Module)"  (ESP8266 core 3.x)
 *  Tools -> lwIP Variant: "v2 Lower Memory" (default) or "v2 Higher Bandwidth"
 * ============================================================================
 */
/*
 * ============================================================================
 *  IMAM BUDDY v5.0 - Multi-Touch + Settings Menu + Auto Sleep (ESP8266)
 * ============================================================================
 *  Libraries (Library Manager): Blynk, Adafruit GFX, Adafruit SSD1306, NTPClient
 *  Board: "NodeMCU 1.0 (ESP-12E Module)"  (ESP8266 core 3.x recommended)
 *
 *  Wiring:
 *    OLED SDA        -> D2 (GPIO4)       OLED SCL      -> D1 (GPIO5)  (0x3C)
 *    Head touch wire -> D5 (GPIO14)
 *    Left cheek wire -> D7 (GPIO13)      <-- swapped in v5
 *    Right cheek wire-> D6 (GPIO12)      <-- swapped in v5
 *    Buzzer (+)      -> D0 (GPIO16), Buzzer (-) -> GND
 *    (D3/GPIO0, D4/GPIO2, D8/GPIO15 are boot-strapping pins -> deliberately unused)
 *
 *  ---------------------------- BLYNK VIRTUAL PINS ---------------------------
 *    V0 = String  INPUT   Incoming message (scrolls + loud alert beep, wakes pet)
 *    V1 = LED/Int OUTPUT  Unread indicator: 255 = unread message, 0 = read/cleared
 *    V2 = String  OUTPUT  Live date + day   "DD/MM/YYYY Day"  e.g. "02/11/2026 Monday"
 *    V3 = String  OUTPUT  Live weather      "28C Clear" (device pushes live data;
 *                                           a manual push is accepted until next update)
 *    V4 = String  IN/OUT  Alarm time        "07:30 AM" / "07:30 PM" / "OFF"
 *                                           (24h "19:30" is also accepted as input;
 *                                           device always writes back the normalized text,
 *                                           and also updates it when edited in the menu)
 *    V5 = String  OUTPUT  Pet mood          CRY / SAD / NORMAL / HAPPY / SLEEPING
 *    V6 = Button (PUSH mode)  Virtual HEAD touch
 *    V7 = Button (PUSH mode)  Virtual LEFT cheek touch
 *    V8 = Button (PUSH mode)  Virtual RIGHT cheek touch
 *  ---------------------------------------------------------------------------
 *
 *  Head  (D5): 1 tap mood up | 2 taps trick | 3-4 taps scared | hold 2.5 s sleep
 *  Cheeks    : L or R tap = SLAP reaction | both together = pinch (super happy)
 *              hold either cheek 2 s = open / close SETTINGS MENU
 *  Menu      : L tap prev/decrease | R tap next/increase | Head tap select/confirm
 *              Alarm edit steps: ON/OFF -> Hour -> Minute -> AM/PM (Head tap = next/save)
 *              Minute: single tap = 1 min, quick repeated taps = 5 min
 *  Idle      : auto sleep after timeout (default 1 h): OLED off, Wi-Fi/NTP/Blynk stay on
 *              (modem light sleep + CPU idle delay)
 *
 *  BUZZER NOTE: GPIO16 (D0) has NO hardware PWM and NO tone() support on the
 *  ESP8266, so v5 drives it from Timer1 (hardware timer interrupt) as a clean
 *  fixed-frequency square wave. Volume is set ONLY by the duty cycle (pulse
 *  width) - the period (pitch) is never touched, so notes stay in tune on all
 *  5 levels. Duty values are chosen so the fundamental amplitude is linear:
 *  sin(pi*duty) = level/5  ->  6%, 13%, 20%, 30%, 50%.  50% is the maximum
 *  possible from a single GPIO. For extra loudness drive the buzzer through an
 *  NPN transistor (e.g. 2N2222 / BC547 + 1k base resistor) from 5V (VIN).
 *
 *  Do NOT touch the wires while the pet boots (touch calibration).
 * ============================================================================
 */
 

#define BLYNK_TEMPLATE_ID    "TMPL6gBnqXvGj"
#define BLYNK_TEMPLATE_NAME  "Imam Buddy"
#define BLYNK_AUTH_TOKEN     "PqAVyR_e80ZggsUyAUalvXLkNRq3N5qT"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <BlynkSimpleEsp8266.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NTPClient.h>

extern "C" {
  #include "user_interface.h"
  #include "lwip/ip_addr.h"
}

// Low-level DHCP C-SDK binding for ESP8266 Core 3.x
extern "C" void dhcps_set_dns(uint8_t num, ip_addr_t *dnsserver) __attribute__((weak));

// NAPT (only in lwIP variants "with features"; default variant has it)
#if LWIP_FEATURES
  #include <lwip/napt.h>
  #include <lwip/dns.h>
  #define HAVE_NAPT 1
#else
  #define HAVE_NAPT 0
#endif

// ----------------------------- User Settings --------------------------------
const char* WIFI_SSID = "IMAM";
const char* WIFI_PASS = "imam121#";

#define AP_SSID          "Buddy"      // repeater network name (password = WIFI_PASS)
#define AP_MAX_CLIENTS   4
#define NAPT_ENTRIES     200          // small table keeps heap use low
#define NAPT_PORTMAPS    4

// ----------------------------- Hardware Pins --------------------------------
#define OLED_W           128
#define OLED_H           64
#define OLED_ADDR        0x3C
#define PIN_SDA          4      // D2
#define PIN_SCL          5      // D1
#define TOUCH_PIN_HEAD   14     // D5
#define TOUCH_PIN_LEFT   13     // D7
#define TOUCH_PIN_RIGHT  12     // D6
#define BUZZER_PIN       16     // D0

#define BUZZER_PASSIVE   true   // true = passive buzzer, false = active
#define ALERT_FREQ       2700   // Hz

// ----------------------------- Channels -------------------------------------
#define CH_HEAD   0
#define CH_LEFT   1
#define CH_RIGHT  2
#define NUM_CH    3

// ----------------------------- Timing Constants -----------------------------
const uint32_t MOOD_DECAY_MS      = 5UL * 60UL * 1000UL;
const uint32_t STANDBY_FACE_MS    = 5000;
const uint32_t STANDBY_CLOCK_MS   = 3000;
const uint32_t STANDBY_DATE_MS    = 3000;
const uint32_t STANDBY_WEATHER_MS = 3000;
const uint32_t INTERACT_WINDOW    = 10000;
const uint32_t MSG_READ_WINDOW    = 30000;
const uint32_t SLEEP_DISP_OFF_MS  = 10000;
const uint32_t AUTO_SLEEP_OFF_MS  = 1500;
const uint32_t FRAME_MS           = 50;
const uint32_t ALARM_MAX_MS       = 120000;

const uint32_t GESTURE_GAP_MS     = 400;
const uint32_t LONG_PRESS_MS      = 2500;
const uint32_t MENU_HOLD_MS       = 2000;
const uint32_t PINCH_WINDOW_MS    = 600;
const uint32_t MENU_TIMEOUT_MS    = 30000;
const uint32_t FX_TRICK_MS        = 3000;
const uint32_t FX_SCARED_MS       = 3000;
const uint32_t FX_WAKE_MS         = 2500;
const uint32_t FX_SLAP_MS         = 2600;
const uint32_t FX_PINCH_MS        = 3500;

// SOS
const uint32_t SOS_WINDOW_MS      = 300;
const uint32_t SOS_ACK_GUARD_MS   = 2000;
const uint32_t SOS_SIREN_MAX_MS   = 5UL * 60UL * 1000UL;
const uint32_t SOS_RESEND_MS      = 60000;
const uint32_t SOS_PENDING_MAX_MS = 10UL * 60UL * 1000UL;
const uint32_t SOS_RETRY_MS       = 3000;

const uint32_t WEATHER_OK_INTERVAL   = 15UL * 60UL * 1000UL;
const uint32_t WEATHER_STALE_MS      = 3UL * 60UL * 60UL * 1000UL;

const uint32_t SLEEP_OPTS_MS[4]   = { 900000UL, 1800000UL, 3600000UL, 0UL };

// ----------------------------- Touch Tuning ---------------------------------
#define TOUCH_SAMPLES          16
#define TOUCH_INTERVAL_MS      15
#define TOUCH_PRESS_SAMPLES    2
#define TOUCH_RELEASE_SAMPLES  2
#define TOUCH_STUCK_MS         15000
#define VIRT_STUCK_MS          15000
const float    TOUCH_RATIO     = 0.30f;
const uint32_t TOUCH_MIN_DELTA = 120;

// ----------------------------- Types ----------------------------------------
enum Mood : uint8_t { MOOD_CRY = 0, MOOD_SAD = 1, MOOD_NORMAL = 2, MOOD_HAPPY = 3 };
enum DispMode : uint8_t { DM_STANDBY, DM_INTERACT, DM_MESSAGE, DM_FX, DM_SLEEP, DM_ALARM, DM_MENU };
enum FxMode : uint8_t { FX_NONE, FX_TRICK, FX_SCARED, FX_WAKE, FX_SLAP, FX_PINCH, FX_SOS };
enum WeatherIcon : uint8_t { WI_SUN, WI_PARTLY, WI_CLOUD, WI_RAIN, WI_STORM };
struct Note { uint16_t f; uint16_t d; };
struct TouchCh {
  float    baseline;
  uint32_t pressStart;
  uint8_t  above, below;
  bool     phys, virt, active, longFired;
};
struct CityRec { char n[13]; int16_t la; int16_t lo; };

const char* const DAYS_OF_WEEK[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

const CityRec CITIES[] PROGMEM = {
  {"Chattogram", 2234, 9183},
  {"Dhaka",      2381, 9041},
  {"Sylhet",     2490, 9187},
  {"Rajshahi",   2437, 8860},
  {"Khulna",     2282, 8955},
  {"Barishal",   2270, 9037},
  {"Rangpur",    2575, 8925},
  {"Mymensingh", 2475, 9041}
};
const uint8_t NUM_CITIES = sizeof(CITIES) / sizeof(CITIES[0]);

// ----------------------------- Melodies -------------------------------------
const Note MEL_TRICK[]  = { {784,80},{988,80},{1319,80},{1568,80},{0,40},{1319,80},{1568,160},{0,120} };
const Note MEL_SCARED[] = { {2200,50},{1400,50},{2200,50},{1400,50},{2200,50},{1400,50},{0,90} };
const Note MEL_WAKE[]   = { {523,80},{659,80},{784,140} };
const Note MEL_PINCH[]  = { {1319,70},{1568,70},{2093,70},{2637,70},{0,40},
                            {2093,70},{2637,70},{3136,200} };
const Note MEL_SLAP[]   = { {400,30},{650,30},{900,30},{1250,30},{1650,30},{2100,30},{2600,30},
                            {4000,14},{1100,14},{3600,14},{800,14},{3000,14},{600,16},{2500,16},{0,30},
                            {1600,30},{1350,30},{1100,30},{900,35},{700,40},{520,50},{380,70},{0,40} };
#define NOTES(a) ((uint8_t)(sizeof(a) / sizeof(a[0])))

const uint8_t VOL_DUTY_PCT[5] = { 6, 13, 20, 30, 50 };
#define BZ_TIMER_HZ  5000000UL

// ----------------------------- Global Objects -------------------------------
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 21600, 60000);

// ----------------------------- Persistent Settings --------------------------
#define EE_MAGIC 0xB9
uint8_t  cityIdx      = 0;
uint8_t  sleepOpt     = 2;
uint8_t  volume       = 5;
uint8_t  moodLock     = 0;
bool     alarmEnabled = false;
uint16_t alarmMin     = 7 * 60 + 30;
bool     repeaterEnabled = false;
char     cityName[13];
bool     settingsDirty = false;
uint32_t settingsDirtyMs = 0;

// ----------------------------- Pet State ------------------------------------
Mood     mood         = MOOD_CRY;
uint32_t lastDecayMs  = 0;
uint32_t lastTouchMs  = 0;
uint32_t lastActivityMs = 0;
bool     hasTouched   = false;
bool     sleeping     = false;
uint32_t sleepStartMs = 0;
uint32_t sleepDispOffDelay = SLEEP_DISP_OFF_MS;
bool     displayIsOn  = true;

// Alarm
bool     alarmTriggered = false;
uint32_t alarmStartMs   = 0;
int      lastAlarmDay   = -1;

// SOS
bool     sosActive      = false;
bool     sosPending     = false;
bool     sosSent        = false;
bool     sosEverSent    = false;
uint32_t sosT0          = 0;
uint32_t sosTriggerMs   = 0;
uint32_t sosNextTryMs   = 0;
uint32_t lastSosSendMs  = 0;

// WiFi repeater
bool      repeaterUp      = false;
bool      repeaterNoNapt  = false;
bool      naptInited      = false;
uint32_t  repeaterNextMs  = 0;
IPAddress repeaterStaIp;

// Effects
FxMode      fxMode       = FX_NONE;
uint32_t    fxStartMs    = 0;
uint32_t    fxDurationMs = 0;
const Note* fxMelody     = nullptr;
uint8_t     fxSide       = CH_LEFT;

// Message
String   msg;
bool     msgActive   = false;
bool     msgUnread   = false;
uint32_t msgScrollT0 = 0;
uint32_t msgReadT0   = 0;
uint32_t alertT0     = 0;
int      msgPixelW   = 0;

// Weather
bool        weatherValid  = false;
char        weatherTemp[8]  = "";
char        weatherDesc[14] = "";
WeatherIcon weatherIcon   = WI_PARTLY;
uint32_t    weatherNextFetchMs = 0;
uint32_t    weatherOkMs   = 0;
uint8_t     weatherFails  = 0;
bool        weatherPreferHttps = false;

// Blynk outbound sync flags
int      ledTarget     = 0;
bool     ledSynced     = true;
bool     dateSynced    = false;
bool     moodSynced    = false;
bool     weatherSynced = false;
bool     alarmSynced   = false;
uint32_t outRetryMs    = 0;

// Buzzer engine
uint16_t    curFreq    = 0;
uint8_t     curVol     = 0;
const Note* melPtr     = nullptr;
uint8_t     melLen     = 0;
uint8_t     melIdx     = 0;
bool        melLoop    = false;
uint32_t    melNoteEnd = 0;
Note        chirpNote[1];
volatile uint32_t bzHighTicks = 0, bzLowTicks = 0;
volatile bool     bzPhaseHigh = false;
bool              bzRunning   = false;

// Touch / gestures
TouchCh  tch[NUM_CH];
const uint8_t TOUCH_PINS[NUM_CH] = { TOUCH_PIN_HEAD, TOUCH_PIN_LEFT, TOUCH_PIN_RIGHT };
uint32_t touchMaxCycles   = 0;
uint32_t lastTouchSampleMs = 0;
uint8_t  headTaps         = 0;
uint32_t headLastPress    = 0;

// Menu
#define MENU_ITEMS 6
enum { MI_ALARM = 0, MI_CITY, MI_SLEEP, MI_MOOD, MI_VOL, MI_REPEATER };
bool     menuActive = false, menuEdit = false;
uint8_t  menuIdx = 0, menuField = 0;
uint32_t menuLastInputMs = 0;
uint8_t  wCity, wSleep, wMood, wVol, wH12, wM;
bool     wAlarmOn, wPM, wRepeater;
uint32_t lastAdjustMs = 0;
int8_t   lastAdjustDir = 0;
const char* const SLEEP_LABELS[4] = { "15 min", "30 min", "1 hour", "OFF" };
const char* const MOOD_LABELS[5]  = { "AUTO", "CRY", "SAD", "NORMAL", "HAPPY" };

// Display / network
DispMode lastMode = DM_STANDBY;
uint32_t standbyT0 = 0;
uint32_t lastFrameMs = 0;
uint32_t ntpNextMs = 0;
uint32_t blynkRetryMs = 0;

// ----------------------------- Prototypes -----------------------------------
void setDisplayPower(bool turnOn);
void setChannel(uint8_t c, bool newPhys, bool newVirt, uint32_t now);
void chPress(uint8_t c, uint32_t now);
void chRelease(uint8_t c, uint32_t now);
void wakeUp(uint32_t now, bool withEffect);
void goToSleep(uint32_t now, bool autoMode);
void startEffect(FxMode fx, uint32_t now);
void cancelEffect();
void startMelody(const Note* m, uint8_t n, bool loop);
void stopMelodyIf(const Note* m);
void parseWeather(String s);
void openMenu(uint32_t now);
void closeMenu(uint32_t now);
void markSettingsDirty(uint32_t now);
void saveSettings();
void stopAlarm();
void triggerSos(uint32_t now);
void endSos(uint32_t now);

// ============================================================================
//  SETTINGS (EEPROM)
// ============================================================================
void cityNameOf(uint8_t i, char* out) {
  CityRec r;
  memcpy_P(&r, &CITIES[i], sizeof(r));
  strncpy(out, r.n, 12);
  out[12] = 0;
}

void cityCoordsOf(uint8_t i, float* lat, float* lon) {
  CityRec r;
  memcpy_P(&r, &CITIES[i], sizeof(r));
  *lat = r.la / 100.0f;
  *lon = r.lo / 100.0f;
}

void loadSettings() {
  EEPROM.begin(16);
  if (EEPROM.read(0) == EE_MAGIC) {
    cityIdx      = EEPROM.read(1);
    sleepOpt     = EEPROM.read(2);
    volume       = EEPROM.read(3);
    moodLock     = EEPROM.read(4);
    alarmEnabled = EEPROM.read(5) != 0;
    alarmMin     = EEPROM.read(6) | ((uint16_t)EEPROM.read(7) << 8);
    repeaterEnabled = EEPROM.read(8) != 0;
    if (cityIdx >= NUM_CITIES)       cityIdx = 0;
    if (sleepOpt > 3)                sleepOpt = 2;
    if (volume < 1 || volume > 5)    volume = 5;
    if (moodLock > 4)                moodLock = 0;
    if (alarmMin >= 1440)            alarmMin = 450;
  }
  cityNameOf(cityIdx, cityName);
  if (moodLock) mood = (Mood)(moodLock - 1);
}

void saveSettings() {
  EEPROM.write(0, EE_MAGIC);
  EEPROM.write(1, cityIdx);
  EEPROM.write(2, sleepOpt);
  EEPROM.write(3, volume);
  EEPROM.write(4, moodLock);
  EEPROM.write(5, alarmEnabled ? 1 : 0);
  EEPROM.write(6, alarmMin & 0xFF);
  EEPROM.write(7, alarmMin >> 8);
  EEPROM.write(8, repeaterEnabled ? 1 : 0);
  EEPROM.commit();
  settingsDirty = false;
}

void markSettingsDirty(uint32_t now) {
  settingsDirty = true;
  settingsDirtyMs = now;
}

void serviceSettings(uint32_t now) {
  if (settingsDirty && (now - settingsDirtyMs) > 2000) saveSettings();
}

void formatAlarm(char* buf, size_t n) {
  if (!alarmEnabled) { snprintf(buf, n, "OFF"); return; }
  int h24 = alarmMin / 60;
  int h12 = h24 % 12; if (h12 == 0) h12 = 12;
  snprintf(buf, n, "%02d:%02d %s", h12, (int)(alarmMin % 60), h24 >= 12 ? "PM" : "AM");
}

// ============================================================================
//  DISPLAY POWER
// ============================================================================
void setDisplayPower(bool turnOn) {
  if (displayIsOn == turnOn) return;
  displayIsOn = turnOn;
  if (turnOn) display.ssd1306_command(SSD1306_DISPLAYON);
  else        display.ssd1306_command(SSD1306_DISPLAYOFF);
}

// ============================================================================
//  BLYNK HANDLERS
// ============================================================================
BLYNK_CONNECTED() {
  ledSynced = false;
  dateSynced = false;
  moodSynced = false;
  weatherSynced = false;
  alarmSynced = false;
}

BLYNK_WRITE(V0) {
  String s = param.asStr();
  s.trim();
  if (s.length() == 0) return;
  s.replace('\n', ' ');
  s.replace('\r', ' ');
  if (s.length() > 200) s = s.substring(0, 200);

  uint32_t now = millis();
  if (menuActive) closeMenu(now);
  if (sleeping) wakeUp(now, false);
  setDisplayPower(true);
  cancelEffect();
  lastActivityMs = now;

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
  String s = param.asStr();
  s.trim();
  s.toUpperCase();
  int ci = s.indexOf(':');
  if (ci > 0 && s.indexOf("OFF") < 0) {
    int hh = s.substring(0, ci).toInt();
    int mm = s.substring(ci + 1, ci + 3).toInt();
    if (s.indexOf("PM") > 0)      { if (hh < 12) hh += 12; }
    else if (s.indexOf("AM") > 0) { if (hh == 12) hh = 0; }
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return;
    alarmMin = hh * 60 + mm;
    alarmEnabled = true;
  } else {
    alarmEnabled = false;
  }
  lastAlarmDay = -1;
  alarmSynced = false;
  markSettingsDirty(millis());
}

BLYNK_WRITE(V6) { setChannel(CH_HEAD,  tch[CH_HEAD].phys,  param.asInt() != 0, millis()); }
BLYNK_WRITE(V7) { setChannel(CH_LEFT,  tch[CH_LEFT].phys,  param.asInt() != 0, millis()); }
BLYNK_WRITE(V8) { setChannel(CH_RIGHT, tch[CH_RIGHT].phys, param.asInt() != 0, millis()); }

// ============================================================================
//  BUZZER & MELODY ENGINE
// ============================================================================
void IRAM_ATTR bzISR() {
  if (bzPhaseHigh) {
    digitalWrite(BUZZER_PIN, LOW);
    bzPhaseHigh = false;
    timer1_write(bzLowTicks);
  } else {
    digitalWrite(BUZZER_PIN, HIGH);
    bzPhaseHigh = true;
    timer1_write(bzHighTicks);
  }
}

void buzzerTone(uint16_t f, bool loud = false) {
  uint8_t vol = loud ? 5 : volume;
  if (f == curFreq && vol == curVol) return;
  curFreq = f;
  curVol  = vol;

  if (!BUZZER_PASSIVE) {
    digitalWrite(BUZZER_PIN, f ? HIGH : LOW);
    return;
  }

  if (f == 0) {
    if (bzRunning) { timer1_disable(); bzRunning = false; }
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  uint32_t period = BZ_TIMER_HZ / f;
  uint32_t hi = (period * VOL_DUTY_PCT[vol - 1]) / 100UL;
  if (hi < 30) hi = 30;
  uint32_t lo = (period > hi) ? (period - hi) : 30;
  if (lo < 30) lo = 30;

  noInterrupts();
  bzHighTicks = hi;
  bzLowTicks  = lo;
  interrupts();

  if (!bzRunning) {
    bzRunning = true;
    bzPhaseHigh = true;
    digitalWrite(BUZZER_PIN, HIGH);
    timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);
    timer1_write(hi);
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

void menuBeep(uint16_t f) {
  chirpNote[0].f = f;
  chirpNote[0].d = 60;
  startMelody(chirpNote, 1, false);
}

void chirpForMood() {
  if (sleeping) return;
  static const uint16_t f[4] = { 500, 900, 1600, 2600 };
  chirpNote[0].f = f[mood];
  chirpNote[0].d = (mood == MOOD_HAPPY) ? 110 : 70;
  startMelody(chirpNote, 1, false);
}

void serviceBuzzer(uint32_t now) {
  if (sosActive) {
    if ((now - sosT0) < SOS_SIREN_MAX_MS) {
      uint32_t ph  = (now - sosT0) % 1200;
      uint32_t tri = (ph < 600) ? ph : (1200 - ph);
      buzzerTone((uint16_t)(1400 + tri * 3), true);
    } else {
      buzzerTone(0);
    }
    return;
  }
  if ((msgActive && msgUnread) || alarmTriggered) {
    uint8_t slot = ((now - alertT0) / 100) % 8;
    buzzerTone((slot == 0 || slot == 2 || slot == 4) ? ALERT_FREQ : 0, true);
    return;
  }
  if (melPtr) {
    while (melPtr && (int32_t)(now - melNoteEnd) >= 0) {
      melIdx++;
      if (melIdx >= melLen) {
        if (melLoop) melIdx = 0;
        else { melPtr = nullptr; buzzerTone(0); return; }
      }
      melNoteEnd += melPtr[melIdx].d;
    }
    if (melPtr) buzzerTone(melPtr[melIdx].f);
    return;
  }
  buzzerTone(0);
}

// ============================================================================
//  TOUCH SENSING
// ============================================================================
uint32_t touchSample(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < TOUCH_SAMPLES; i++) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delayMicroseconds(3);
    noInterrupts();
    pinMode(pin, INPUT_PULLUP);
    uint32_t t0 = ESP.getCycleCount();
    uint32_t d  = 0;
    while (!digitalRead(pin)) {
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
  uint32_t acc[NUM_CH] = { 0, 0, 0 };
  const uint8_t N = 40;
  for (uint8_t i = 0; i < N; i++) {
    for (uint8_t c = 0; c < NUM_CH; c++) acc[c] += touchSample(TOUCH_PINS[c]);
    uint32_t t = millis();
    while (millis() - t < 10) yield();
  }
  for (uint8_t c = 0; c < NUM_CH; c++) {
    memset(&tch[c], 0, sizeof(TouchCh));
    tch[c].baseline = (float)acc[c] / N;
  }
}

void setChannel(uint8_t c, bool newPhys, bool newVirt, uint32_t now) {
  TouchCh &t = tch[c];
  bool before = t.active;
  t.phys = newPhys;
  t.virt = newVirt;
  t.active = newPhys || newVirt;
  if (!before && t.active)      chPress(c, now);
  else if (before && !t.active) chRelease(c, now);
}

void serviceTouch(uint32_t now) {
  if (now - lastTouchSampleMs < TOUCH_INTERVAL_MS) return;
  lastTouchSampleMs = now;

  for (uint8_t c = 0; c < NUM_CH; c++) {
    TouchCh &t = tch[c];
    uint32_t raw = touchSample(TOUCH_PINS[c]);
    float thr = t.baseline * TOUCH_RATIO;
    if (thr < TOUCH_MIN_DELTA) thr = TOUCH_MIN_DELTA;
    bool above = (float)raw > t.baseline + thr;

    if (!t.phys) {
      if (above) {
        if (++t.above >= TOUCH_PRESS_SAMPLES) {
          t.above = 0; t.below = 0;
          setChannel(c, true, t.virt, now);
        }
      } else {
        t.above = 0;
        t.baseline += ((float)raw - t.baseline) * 0.02f;
      }
    } else {
      bool below = (float)raw < t.baseline + thr * 0.5f;
      if (below) {
        if (++t.below >= TOUCH_RELEASE_SAMPLES) {
          t.below = 0;
          setChannel(c, false, t.virt, now);
        }
      } else {
        t.below = 0;
      }
      if (t.phys && (now - t.pressStart) > TOUCH_STUCK_MS) {
        t.baseline = (float)raw;
        t.longFired = true;
        t.below = 0;
        setChannel(c, false, t.virt, now);
      }
    }
    if (t.virt && !t.phys && (now - t.pressStart) > VIRT_STUCK_MS) {
      t.longFired = true;
      setChannel(c, t.phys, false, now);
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

void stopAlarm() {
  alarmTriggered = false;
  alertT0 = 0;
}

void onSingleTap(uint32_t now) {
  markInteraction(now);
  if (!moodLock && mood < MOOD_HAPPY) {
    mood = (Mood)(mood + 1);
    moodSynced = false;
  }
  chirpForMood();
}

void onDoubleTap(uint32_t now) { markInteraction(now); startEffect(FX_TRICK, now); }
void onRapidTap(uint32_t now)  { markInteraction(now); startEffect(FX_SCARED, now); }

void onCheekTap(uint8_t c, uint32_t now) {
  markInteraction(now);
  fxSide = c;
  startEffect(FX_SLAP, now);
}

void onPinch(uint32_t now) {
  markInteraction(now);
  if (!moodLock && mood != MOOD_HAPPY) { mood = MOOD_HAPPY; moodSynced = false; }
  startEffect(FX_PINCH, now);
}

void resolveHeadGesture(uint32_t now) {
  uint8_t n = headTaps;
  headTaps = 0;
  if (sleeping || menuActive || n == 0) return;
  if (n == 1)      onSingleTap(now);
  else if (n == 2) onDoubleTap(now);
  else             onRapidTap(now);
}

// ============================================================================
//  SOS PANIC ALERT
// ============================================================================
bool sosTripleDetected(uint32_t now) {
  uint32_t oldest = now;
  for (uint8_t c = 0; c < NUM_CH; c++) {
    if (!tch[c].active) return false;
    if ((int32_t)(tch[c].pressStart - oldest) < 0) oldest = tch[c].pressStart;
  }
  return (now - oldest) <= SOS_WINDOW_MS;
}

void triggerSos(uint32_t now) {
  if (menuActive) closeMenu(now);
  stopAlarm();
  if (sleeping) wakeUp(now, false);
  setDisplayPower(true);
  headTaps = 0;
  melPtr = nullptr;
  cancelEffect();

  for (uint8_t c = 0; c < NUM_CH; c++) tch[c].longFired = true;

  sosActive      = true;
  sosT0          = now;
  sosTriggerMs   = now;
  fxMode         = FX_SOS;
  fxStartMs      = now;
  fxDurationMs   = 0;
  hasTouched     = true;
  lastTouchMs    = now;
  lastDecayMs    = now;
  lastActivityMs = now;
  moodSynced     = false;

  if (sosEverSent && (now - lastSosSendMs) < SOS_RESEND_MS) {
    sosSent = true;
  } else {
    sosSent      = false;
    sosPending   = true;
    sosNextTryMs = now;
  }
}

void endSos(uint32_t now) {
  sosActive = false;
  fxMode = FX_NONE;
  fxMelody = nullptr;
  melPtr = nullptr;
  hasTouched = true;
  lastTouchMs = now;
  lastDecayMs = now;
  lastActivityMs = now;
  moodSynced = false;
  setDisplayPower(true);
  menuBeep(1500);
}

void serviceSos(uint32_t now) {
  if (!sosPending) return;
  if ((now - sosTriggerMs) > SOS_PENDING_MAX_MS) { sosPending = false; return; }
  if (!Blynk.connected() || (int32_t)(now - sosNextTryMs) < 0) return;

  Blynk.logEvent("sos_alert", "Emergency Please Call Home");
  sosPending    = false;
  sosSent       = true;
  sosEverSent   = true;
  lastSosSendMs = now;
  sosNextTryMs  = now + SOS_RETRY_MS;
}

// ============================================================================
//  MENU
// ============================================================================
void loadWorking() {
  wCity = cityIdx; wSleep = sleepOpt; wMood = moodLock; wVol = volume;
  wAlarmOn = alarmEnabled;
  wRepeater = repeaterEnabled;
  int h24 = alarmMin / 60;
  wPM = (h24 >= 12);
  wH12 = h24 % 12; if (wH12 == 0) wH12 = 12;
  wM = alarmMin % 60;
}

void openMenu(uint32_t now) {
  menuActive = true;
  menuEdit = false;
  menuIdx = 0;
  menuLastInputMs = now;
  headTaps = 0;
  cancelEffect();
  menuBeep(1500);
}

void closeMenu(uint32_t now) {
  menuActive = false;
  menuEdit = false;
  hasTouched = true;
  lastTouchMs = now;
  if (settingsDirty) saveSettings();
  menuBeep(1200);
}

void commitAlarm(uint32_t now) {
  (void)now;
  int h24 = (wH12 % 12) + (wPM ? 12 : 0);
  alarmMin = h24 * 60 + wM;
  alarmEnabled = wAlarmOn;
  lastAlarmDay = -1;
  alarmSynced = false;
  saveSettings();
}

void menuAdjust(int8_t dir, uint32_t now) {
  bool fast = ((now - lastAdjustMs) < 350) && (dir == lastAdjustDir);
  lastAdjustMs = now;
  lastAdjustDir = dir;

  switch (menuIdx) {
    case MI_ALARM:
      if (menuField == 0)      wAlarmOn = !wAlarmOn;
      else if (menuField == 1) wH12 = ((wH12 - 1 + dir + 12) % 12) + 1;
      else if (menuField == 2) { int step = fast ? 5 : 1; wM = (wM + dir * step + 60) % 60; }
      else                     wPM = !wPM;
      break;
    case MI_CITY:  wCity  = (wCity + NUM_CITIES + dir) % NUM_CITIES; break;
    case MI_SLEEP: wSleep = (wSleep + 4 + dir) % 4;                  break;
    case MI_MOOD:  wMood  = (wMood + 5 + dir) % 5;                   break;
    case MI_VOL: {
      int v = (int)wVol + dir;
      if (v < 1) v = 1;
      if (v > 5) v = 5;
      wVol = v;
      break;
    }
    case MI_REPEATER: wRepeater = !wRepeater; break;
  }
  if (menuIdx == MI_VOL) {
    uint8_t keep = volume; volume = wVol;
    menuBeep(2000);
    volume = keep;
  } else {
    menuBeep(1800);
  }
}

void menuMove(int8_t dir, uint32_t now) {
  if (menuEdit) { menuAdjust(dir, now); return; }
  menuIdx = (menuIdx + MENU_ITEMS + dir) % MENU_ITEMS;
  menuBeep(1800);
}

void menuHeadTap(uint32_t now) {
  if (!menuEdit) {
    loadWorking();
    menuEdit = true;
    menuField = 0;
    menuBeep(2200);
    return;
  }
  switch (menuIdx) {
    case MI_ALARM:
      if (menuField == 0 && !wAlarmOn) { commitAlarm(now); menuEdit = false; }
      else if (++menuField > 3)        { commitAlarm(now); menuEdit = false; }
      break;
    case MI_CITY:
      cityIdx = wCity;
      cityNameOf(cityIdx, cityName);
      weatherValid = false;
      weatherFails = 0;
      weatherNextFetchMs = now + 1500;
      markSettingsDirty(now);
      menuEdit = false;
      break;
    case MI_SLEEP:
      sleepOpt = wSleep;
      markSettingsDirty(now);
      menuEdit = false;
      break;
    case MI_MOOD:
      moodLock = wMood;
      if (moodLock) mood = (Mood)(moodLock - 1);
      moodSynced = false;
      markSettingsDirty(now);
      menuEdit = false;
      break;
    case MI_VOL:
      volume = wVol;
      markSettingsDirty(now);
      menuEdit = false;
      break;
    case MI_REPEATER:
      repeaterEnabled = wRepeater;
      repeaterNextMs = now;
      saveSettings();
      menuEdit = false;
      break;
  }
  menuBeep(2400);
}

// ============================================================================
//  PRESS / RELEASE DISPATCH
// ============================================================================
void chPress(uint8_t c, uint32_t now) {
  TouchCh &t = tch[c];
  t.pressStart = now;
  t.longFired = false;
  lastActivityMs = now;

  if (sosActive) {
    t.longFired = true;
    if ((now - sosT0) >= SOS_ACK_GUARD_MS) endSos(now);
    return;
  }

  if (sosTripleDetected(now)) { triggerSos(now); return; }

  if (alarmTriggered) { stopAlarm(); t.longFired = true; return; }
  if (sleeping)       { wakeUp(now, true); t.longFired = true; return; }
  setDisplayPower(true);
  if (menuActive)     { menuLastInputMs = now; return; }

  if (msgActive && msgUnread) readMessage(now);

  if (c == CH_HEAD) {
    if (headTaps > 0 && (now - headLastPress) > GESTURE_GAP_MS) resolveHeadGesture(now);
    headLastPress = now;
  } else {
    uint8_t o = (c == CH_LEFT) ? CH_RIGHT : CH_LEFT;
    if (tch[o].active && !tch[o].longFired && (now - tch[o].pressStart) <= PINCH_WINDOW_MS) {
      tch[o].longFired = true;
      t.longFired = true;
      onPinch(now);
    }
  }
}

void chRelease(uint8_t c, uint32_t now) {
  TouchCh &t = tch[c];
  if (t.longFired || sleeping) return;
  lastActivityMs = now;

  if (menuActive) {
    menuLastInputMs = now;
    if (c == CH_HEAD)      menuHeadTap(now);
    else if (c == CH_LEFT) menuMove(-1, now);
    else                   menuMove(+1, now);
    return;
  }
  if (c == CH_HEAD) {
    headTaps++;
    if (headTaps >= 4) resolveHeadGesture(now);
  } else {
    onCheekTap(c, now);
  }
}

void serviceGestures(uint32_t now) {
  TouchCh &h = tch[CH_HEAD];

  if (h.active && !h.longFired && (now - h.pressStart) >= LONG_PRESS_MS) {
    h.longFired = true;
    headTaps = 0;
    if (!menuActive && !sleeping && !sosActive) goToSleep(now, false);
  }

  for (uint8_t c = CH_LEFT; c <= CH_RIGHT; c++) {
    TouchCh &t = tch[c];
    if (t.active && !t.longFired && !sleeping && !sosActive && (now - t.pressStart) >= MENU_HOLD_MS) {
      t.longFired = true;
      if (menuActive) closeMenu(now);
      else            openMenu(now);
    }
  }

  if (!h.active && headTaps > 0 && (now - headLastPress) > GESTURE_GAP_MS) resolveHeadGesture(now);
  if (menuActive && (now - menuLastInputMs) > MENU_TIMEOUT_MS) closeMenu(now);
}

// ============================================================================
//  SLEEP / WAKE / AUTO SLEEP / EFFECTS
// ============================================================================
void goToSleep(uint32_t now, bool autoMode) {
  if (sosActive) return;
  sleeping = true;
  sleepStartMs = now;
  sleepDispOffDelay = autoMode ? AUTO_SLEEP_OFF_MS : SLEEP_DISP_OFF_MS;
  cancelEffect();
  melPtr = nullptr;
  headTaps = 0;
  moodSynced = false;
  if (msgActive) {
    msgActive = false; msgUnread = false; msg = "";
    ledTarget = 0; ledSynced = false;
  }
}

void wakeUp(uint32_t now, bool withEffect) {
  sleeping = false;
  lastDecayMs = now;
  lastActivityMs = now;
  moodSynced = false;
  setDisplayPower(true);
  if (withEffect) {
    hasTouched = true;
    lastTouchMs = now;
    startEffect(FX_WAKE, now);
  }
}

void serviceAutoSleep(uint32_t now) {
  if (sleeping) return;
  bool busy = sosActive || alarmTriggered || menuActive || msgActive || fxMode != FX_NONE ||
              tch[CH_HEAD].active || tch[CH_LEFT].active || tch[CH_RIGHT].active;
  if (busy) { lastActivityMs = now; return; }
  uint32_t to = SLEEP_OPTS_MS[sleepOpt];
  if (to && (now - lastActivityMs) >= to) goToSleep(now, true);
}

void startEffect(FxMode fx, uint32_t now) {
  if (sosActive) return;
  uint8_t keepSide = fxSide;
  cancelEffect();
  fxSide = keepSide;
  fxMode = fx;
  fxStartMs = now;
  switch (fx) {
    case FX_TRICK:  fxDurationMs = FX_TRICK_MS;  fxMelody = MEL_TRICK;  startMelody(MEL_TRICK,  NOTES(MEL_TRICK),  true);  break;
    case FX_SCARED: fxDurationMs = FX_SCARED_MS; fxMelody = MEL_SCARED; startMelody(MEL_SCARED, NOTES(MEL_SCARED), true);  break;
    case FX_WAKE:   fxDurationMs = FX_WAKE_MS;   fxMelody = MEL_WAKE;   startMelody(MEL_WAKE,   NOTES(MEL_WAKE),   false); break;
    case FX_SLAP:   fxDurationMs = FX_SLAP_MS;   fxMelody = MEL_SLAP;   startMelody(MEL_SLAP,   NOTES(MEL_SLAP),   false); break;
    case FX_PINCH:  fxDurationMs = FX_PINCH_MS;  fxMelody = MEL_PINCH;  startMelody(MEL_PINCH,  NOTES(MEL_PINCH),  false); break;
    default: break;
  }
}

void cancelEffect() {
  if (sosActive) return;
  stopMelodyIf(fxMelody);
  fxMelody = nullptr;
  fxMode = FX_NONE;
}

void serviceEffects(uint32_t now) {
  if (fxMode != FX_NONE && fxMode != FX_SOS && (now - fxStartMs) >= fxDurationMs) cancelEffect();
}

// ============================================================================
//  ALARM
// ============================================================================
void serviceAlarm(uint32_t now) {
  if (alarmTriggered && (now - alarmStartMs) > ALARM_MAX_MS) stopAlarm();

  if (!sosActive && timeClient.isTimeSet() && alarmEnabled) {
    int currentMin = timeClient.getHours() * 60 + timeClient.getMinutes();
    int currentDay = timeClient.getDay();
    if (currentMin == (int)alarmMin && lastAlarmDay != currentDay) {
      alarmTriggered = true;
      alarmStartMs = now;
      alertT0 = now;
      lastAlarmDay = currentDay;
      lastActivityMs = now;
      if (menuActive) closeMenu(now);
      if (sleeping) wakeUp(now, false);
      setDisplayPower(true);
    }
  }
}

// ============================================================================
//  WIFI REPEATER (SoftAP "Buddy" + Station, NAPT)
// ============================================================================
void applyApDns() {
#if HAVE_NAPT
  if (dhcps_set_dns != nullptr) {
    ip_addr_t d1;
    uint32_t dnsRaw = (uint32_t)WiFi.dnsIP(0);
    ip4_addr_set_u32(ip_2_ip4(&d1), dnsRaw);
    dhcps_set_dns(0, &d1);
  }
#endif
}

bool startRepeater() {
#if !HAVE_NAPT
  Serial.println(F("Repeater: this lwIP variant has no NAPT (pick a variant WITH features)"));
  repeaterNoNapt = true;
  return false;
#else
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(172, 30, 77, 1), IPAddress(172, 30, 77, 1), IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(AP_SSID, WIFI_PASS, WiFi.channel(), false, AP_MAX_CLIENTS)) {
    Serial.println(F("Repeater: softAP failed (WIFI_PASS must be 8..63 chars)"));
    WiFi.mode(WIFI_STA);
    return false;
  }
  applyApDns();
  err_t r = ERR_OK;
  if (!naptInited) {
    r = ip_napt_init(NAPT_ENTRIES, NAPT_PORTMAPS);
    if (r == ERR_OK) naptInited = true;
  }
  if (r == ERR_OK) r = ip_napt_enable_no(SOFTAP_IF, 1);
  if (r != ERR_OK) {
    Serial.println(F("Repeater: NAPT init failed (low heap?)"));
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    return false;
  }
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  repeaterStaIp = WiFi.localIP();
  repeaterUp = true;
  Serial.printf("Repeater ON: SSID %s, heap %u\n", AP_SSID, (unsigned)ESP.getFreeHeap());
  return true;
#endif
}

void stopRepeater() {
#if HAVE_NAPT
  if (naptInited) ip_napt_enable_no(SOFTAP_IF, 0);
#endif
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_LIGHT_SLEEP);
  repeaterUp = false;
  Serial.println(F("Repeater OFF"));
}

void serviceRepeater(uint32_t now) {
  if (repeaterEnabled) {
    if (!repeaterUp) {
      if (repeaterNoNapt) return;
      if (WiFi.status() != WL_CONNECTED) return;
      if ((int32_t)(now - repeaterNextMs) < 0) return;
      if (!startRepeater()) repeaterNextMs = millis() + 30000UL;
    } else {
      if (WiFi.getSleepMode() != WIFI_NONE_SLEEP) WiFi.setSleepMode(WIFI_NONE_SLEEP);
      if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        if (ip != repeaterStaIp) { repeaterStaIp = ip; applyApDns(); }
      }
    }
  } else if (repeaterUp) {
    stopRepeater();
  }
}

// ============================================================================
//  WEATHER
// ============================================================================
void parseWeather(String s) {
  s.trim();
  s.replace('\n', ' ');
  s.replace('\r', ' ');
  if (s.length() > 40) s = s.substring(0, 40);
  if (s.length() == 0) { weatherValid = false; return; }

  String low = s;
  low.toLowerCase();
  if (low.indexOf("thunder") >= 0 || low.indexOf("storm") >= 0)                                   weatherIcon = WI_STORM;
  else if (low.indexOf("rain") >= 0 || low.indexOf("drizzle") >= 0 || low.indexOf("shower") >= 0) weatherIcon = WI_RAIN;
  else if (low.indexOf("partly") >= 0 || low.indexOf("scattered") >= 0)                           weatherIcon = WI_PARTLY;
  else if (low.indexOf("cloud") >= 0 || low.indexOf("overcast") >= 0 || low.indexOf("haze") >= 0 ||
           low.indexOf("fog") >= 0 || low.indexOf("mist") >= 0)                                   weatherIcon = WI_CLOUD;
  else if (low.indexOf("sun") >= 0 || low.indexOf("clear") >= 0 || low.indexOf("hot") >= 0)       weatherIcon = WI_SUN;
  else                                                                                            weatherIcon = WI_PARTLY;

  String temp = "", desc = "";
  int start = 0;
  while (start < (int)s.length()) {
    int sp = s.indexOf(' ', start);
    if (sp < 0) sp = s.length();
    String tok = s.substring(start, sp);
    if (tok.length()) {
      bool hasDigit = false;
      for (unsigned i = 0; i < tok.length(); i++) if (isdigit((unsigned char)tok[i])) { hasDigit = true; break; }
      if (temp.length() == 0 && hasDigit) temp = tok;
      else { if (desc.length()) desc += ' '; desc += tok; }
    }
    start = sp + 1;
  }
  desc.replace(cityName, "");
  desc.trim();

  strncpy(weatherTemp, temp.c_str(), sizeof(weatherTemp) - 1);
  weatherTemp[sizeof(weatherTemp) - 1] = 0;
  strncpy(weatherDesc, desc.c_str(), 11);
  weatherDesc[11] = 0;
  weatherValid = true;
  weatherOkMs = millis();
}

void applyWeather(int code, float tempC) {
  snprintf(weatherTemp, sizeof(weatherTemp), "%dC", (int)roundf(tempC));
  const __FlashStringHelper* d;
  WeatherIcon ic;
  if (code == 0)                         { d = F("Clear");       ic = WI_SUN;    }
  else if (code == 1)                    { d = F("Mainly Clr");  ic = WI_SUN;    }
  else if (code == 2)                    { d = F("Part Cloudy"); ic = WI_PARTLY; }
  else if (code == 3)                    { d = F("Overcast");    ic = WI_CLOUD;  }
  else if (code == 45 || code == 48)     { d = F("Fog");         ic = WI_CLOUD;  }
  else if (code >= 51 && code <= 57)     { d = F("Drizzle");     ic = WI_RAIN;   }
  else if (code == 66 || code == 67)     { d = F("Frzg Rain");   ic = WI_RAIN;   }
  else if (code >= 61 && code <= 65)     { d = F("Rain");        ic = WI_RAIN;   }
  else if (code >= 71 && code <= 77)     { d = F("Snow");        ic = WI_CLOUD;  }
  else if (code >= 80 && code <= 82)     { d = F("Showers");     ic = WI_RAIN;   }
  else if (code == 85 || code == 86)     { d = F("Snow Shwr");   ic = WI_CLOUD;  }
  else if (code >= 95)                   { d = F("Thunder");     ic = WI_STORM;  }
  else                                   { d = F("Unknown");     ic = WI_PARTLY; }
  strncpy_P(weatherDesc, (PGM_P)d, sizeof(weatherDesc) - 1);
  weatherDesc[sizeof(weatherDesc) - 1] = 0;
  weatherIcon = ic;
}

bool parseOpenMeteo(const String& b, float& t, int& wc) {
  static const char K_CUR[]  = "\"current\":";
  static const char K_TEMP[] = "\"temperature_2m\":";
  static const char K_CODE[] = "\"weather_code\":";
  int c = b.indexOf(K_CUR);
  if (c < 0) return false;
  int ti = b.indexOf(K_TEMP, c);
  int wi = b.indexOf(K_CODE, c);
  if (ti < 0 || wi < 0) return false;
  t  = b.substring(ti + strlen(K_TEMP)).toFloat();
  wc = b.substring(wi + strlen(K_CODE)).toInt();
  return (t > -60.0f && t < 70.0f && wc >= 0 && wc <= 99);
}

int weatherDoRequest(WiFiClient& cl, const char* url) {
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(5000);
  http.useHTTP10(true);
  int result = 0;
  if (http.begin(cl, url)) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      String body = http.getString();
      float t; int wc;
      if (parseOpenMeteo(body, t, wc)) { applyWeather(wc, t); result = 1; }
    } else if (code == 301 || code == 302 || code == 307 || code == 308 || code == -1) {
      result = -1;
    }
    http.end();
  }
  return result;
}

int weatherRequest(bool secure) {
  float lat, lon;
  cityCoordsOf(cityIdx, &lat, &lon);
  char url[170];
  snprintf(url, sizeof(url),
           "%s://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
           "&current=temperature_2m,weather_code&timezone=auto",
           secure ? "https" : "http", (double)lat, (double)lon);
  if (!secure) {
    WiFiClient c;
    return weatherDoRequest(c, url);
  }
  if (ESP.getMaxFreeBlockSize() < 20000) return 0;
  BearSSL::WiFiClientSecure c;
  c.setInsecure();
  return weatherDoRequest(c, url);
}

bool fetchLiveWeather() {
  int r = 0;
  if (!weatherPreferHttps) {
    r = weatherRequest(false);
    if (r == 1) return true;
  }
  if (weatherPreferHttps || r == -1) {
    r = weatherRequest(true);
    if (r == 1) { weatherPreferHttps = true; return true; }
  }
  return false;
}

void serviceWeatherSync(uint32_t now) {
  if (weatherValid && (now - weatherOkMs) > WEATHER_STALE_MS) weatherValid = false;
  if (WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(now - weatherNextFetchMs) < 0) return;

  bool busy = sosActive || menuActive || alarmTriggered || (msgActive && msgUnread) || fxMode != FX_NONE ||
              tch[CH_HEAD].active || tch[CH_LEFT].active || tch[CH_RIGHT].active;
  if (busy) { weatherNextFetchMs = now + 3000; return; }

  if (fetchLiveWeather()) {
    weatherValid = true;
    weatherOkMs = millis();
    weatherFails = 0;
    weatherSynced = false;
    weatherNextFetchMs = millis() + WEATHER_OK_INTERVAL;
  } else {
    if (weatherFails < 250) weatherFails++;
    weatherNextFetchMs = millis() + (weatherFails < 3 ? 60000UL : 300000UL);
  }
}

// ============================================================================
//  DATE / NIGHT
// ============================================================================
void getDateStr(char* buf, size_t n) {
  if (!timeClient.isTimeSet()) { snprintf(buf, n, "--/--/----"); return; }

  uint32_t days = timeClient.getEpochTime() / 86400UL;
  uint32_t y = 1970;
  while (true) {
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    uint16_t diy = leap ? 366 : 365;
    if (days >= diy) { days -= diy; y++; } else break;
  }
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  uint8_t md[] = {31, (uint8_t)(leap ? 29 : 28), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t m;
  for (m = 0; m < 12; m++) {
    if (days >= md[m]) days -= md[m]; else break;
  }
  snprintf(buf, n, "%02d/%02d/%04lu", (int)(days + 1), (int)(m + 1), (unsigned long)y);
}

bool isNightNow() {
  if (!timeClient.isTimeSet()) return false;
  int h = timeClient.getHours();
  return (h < 6 || h >= 18);
}

// ============================================================================
//  SERVICES
// ============================================================================
void serviceMood(uint32_t now) {
  if (sleeping || moodLock || sosActive) { lastDecayMs = now; return; }
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

  char buf[40];
  if (!ledSynced) {
    Blynk.virtualWrite(V1, ledTarget);
    ledSynced = true; outRetryMs = now + 300; return;
  }
  if (!dateSynced && timeClient.isTimeSet()) {
    char d[16];
    getDateStr(d, sizeof(d));
    snprintf(buf, sizeof(buf), "%s %s", d, DAYS_OF_WEEK[timeClient.getDay()]);
    Blynk.virtualWrite(V2, buf);
    dateSynced = true; outRetryMs = now + 300; return;
  }
  if (!weatherSynced && weatherValid) {
    snprintf(buf, sizeof(buf), "%s %s", weatherTemp, weatherDesc);
    Blynk.virtualWrite(V3, buf);
    weatherSynced = true; outRetryMs = now + 300; return;
  }
  if (!alarmSynced) {
    formatAlarm(buf, sizeof(buf));
    Blynk.virtualWrite(V4, buf);
    alarmSynced = true; outRetryMs = now + 300; return;
  }
  if (!moodSynced) {
    const char* m = "CRY";
    if (sosActive)                m = "SOS";
    else if (sleeping)            m = "SLEEPING";
    else if (mood == MOOD_SAD)    m = "SAD";
    else if (mood == MOOD_NORMAL) m = "NORMAL";
    else if (mood == MOOD_HAPPY)  m = "HAPPY";
    Blynk.virtualWrite(V5, m);
    moodSynced = true; outRetryMs = now + 300;
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
//  DRAWING HELPERS
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

void drawHeart(int x, int y, int s) {
  display.fillCircle(x - s / 2, y, s / 2 + 1, WHITE);
  display.fillCircle(x + s / 2, y, s / 2 + 1, WHITE);
  display.fillTriangle(x - s - 1, y + 1, x + s + 1, y + 1, x, y + s + 3, WHITE);
}

void drawBlush(int x, int y) {
  for (int i = 0; i < 3; i++) display.drawLine(x + i * 4, y + 6, x + i * 4 + 3, y, WHITE);
}

void drawStar(int x, int y) {
  display.drawLine(x - 2, y, x + 2, y, WHITE);
  display.drawLine(x, y - 2, x, y + 2, WHITE);
  display.drawPixel(x - 1, y - 1, WHITE);
  display.drawPixel(x + 1, y + 1, WHITE);
}

void drawRepeaterIcon(uint32_t now) {
  display.fillRect(113, 0, 15, 10, BLACK);
  if (!repeaterUp && ((now / 500) % 2)) return;
  display.fillCircle(121, 8, 1, WHITE);
  for (int r = 3; r <= 7; r += 2) {
    for (int a = 230; a <= 310; a += 5) {
      float rad = a * 0.0174533f;
      display.drawPixel(121 + (int)roundf(cosf(rad) * r), 8 + (int)roundf(sinf(rad) * r), WHITE);
    }
  }
}

// ---------------------------- Standard faces --------------------------------
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

// ---------------------------- Effect faces ----------------------------------
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

void drawHand(int tip, bool fromLeft) {
  int palmX = fromLeft ? tip - 26 : tip + 12;
  int fingX = fromLeft ? tip - 12 : tip;
  int thmX  = fromLeft ? tip - 22 : tip + 14;
  display.fillRoundRect(palmX - 2, 12, 18, 34, 4, BLACK);
  display.fillRect(fingX - 2, 20, 16, 26, BLACK);
  display.fillRoundRect(palmX, 22, 14, 20, 3, WHITE);
  for (int i = 0; i < 4; i++)
    display.fillRoundRect(fingX, 22 + i * 5, 12, 4, 2, WHITE);
  display.fillRoundRect(thmX, 14, 9, 7, 3, WHITE);
}

void drawFaceSlap(uint32_t e, uint32_t t, uint8_t side) {
  const uint32_t HIT = 220;
  bool fromLeft = (side == CH_LEFT);
  int dir = fromLeft ? 1 : -1;

  if (e < HIT) {
    display.fillRoundRect(26, 15, 22, 26, 6, WHITE);
    display.fillRoundRect(80, 15, 22, 26, 6, WHITE);
    display.fillCircle(37 + (fromLeft ? -4 : 4), 28, 5, BLACK);
    display.fillCircle(91 + (fromLeft ? -4 : 4), 28, 5, BLACK);
    display.drawCircle(64, 52, 4, WHITE);
    int move = (int)((e * 30UL) / HIT);
    int tip = fromLeft ? (-4 + move) : (131 - move);
    drawHand(tip, fromLeft);
    return;
  }

  uint32_t a = e - HIT;
  float k = 1.0f - (a / 1600.0f);
  if (k < 0.0f) k = 0.0f;
  int dx = dir * (int)(14.0f * k) + (int)(sinf(a / 40.0f) * 5.0f * k);
  int dy = (int)(sinf(a / 55.0f) * 2.0f * k);

  for (int i = 0; i < 2; i++) {
    int cx = (i == 0 ? 37 : 91) + dx;
    int cy = 28 + dy;
    display.drawCircle(cx, cy, 10, WHITE);
    display.drawCircle(cx, cy, 5, WHITE);
    float ang = (a / 90.0f) * (i == 0 ? 1.0f : -1.0f);
    display.drawLine(cx, cy, cx + (int)(cosf(ang) * 10), cy + (int)(sinf(ang) * 10), WHITE);
  }

  int px = 0, py = 0;
  for (int x = -14; x <= 14; x += 2) {
    int y = 52 + dy + (int)(sinf((x + a / 35.0f) * 0.7f) * 3.0f);
    if (x > -14) display.drawLine(64 + dx + px, py, 64 + dx + x, y, WHITE);
    px = x; py = y;
  }

  if (a < 1500 || ((a / 120) % 2) == 0) {
    int x0 = (fromLeft ? 26 : 88) + dx;
    for (int i = 0; i < 4; i++) {
      display.drawLine(x0 + i * 3, 44 + dy, x0 + i * 3 + 1, 58 + dy, WHITE);
      if (i % 2) display.drawPixel(x0 + i * 3 - 1, 50 + dy, WHITE);
    }
    display.drawLine(x0 - 3, 56 + dy, x0 + 11, 61 + dy, WHITE);
  }

  for (int s = 0; s < 3; s++) {
    float ang = (a / 150.0f) + s * 2.094f;
    drawStar(64 + dx + (int)(cosf(ang) * 28.0f), 6 + (int)(sinf(ang) * 4.0f));
  }

  if (a < 360) {
    int tipHit = fromLeft ? 26 : 101;
    int back = (int)((a * 60UL) / 360UL);
    int tip = fromLeft ? (tipHit - back) : (tipHit + back);
    drawHand(tip, fromLeft);
  }

  if (a < 300) {
    int ix = fromLeft ? 30 : 97;
    int len = 6 + (int)(a / 25);
    for (int i = 0; i < 8; i++) {
      float ang = i * 0.7854f;
      display.drawLine(ix + (int)(cosf(ang) * 6), 32 + (int)(sinf(ang) * 6),
                       ix + (int)(cosf(ang) * len), 32 + (int)(sinf(ang) * len), WHITE);
    }
  }

  if (a < 700 && ((a / 100) % 2) == 0) {
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(fromLeft ? 90 : 2, 56);
    display.print(F("WHACK!"));
  }
}

void drawFacePinch(uint32_t t) {
  int b = (int)(sinf(t / 110.0f) * 3);
  display.fillCircle(64, 42 + b, 15, WHITE);
  display.fillRect(46, 24 + b, 36, 18, BLACK);
  display.fillCircle(64, 56 + b, 5, BLACK);
  display.fillCircle(64, 55 + b, 4, WHITE);
  drawArc(38, 34 + b, 12, 200, 340, 1);
  drawArc(90, 34 + b, 12, 200, 340, 1);
  drawBlush(2, 42 + b);
  drawBlush(115, 42 + b);
  for (int k = 0; k < 6; k++) {
    int x = (k < 3) ? (6 + k * 6) : (116 - (k - 3) * 6);
    int y = 62 - (int)(((t / 30) + k * 13) % 60);
    if (y > 4) drawHeart(x, y, 3);
  }
  if ((t / 200) % 2 == 0) {
    display.drawLine(64, 2, 64, 8, WHITE);  display.drawLine(61, 5, 67, 5, WHITE);
  }
}

void drawWarnIcon(int cx, int cy, uint16_t fg, uint16_t bg) {
  display.fillTriangle(cx - 8, cy + 7, cx + 8, cy + 7, cx, cy - 8, fg);
  display.fillRect(cx - 1, cy - 3, 2, 6, bg);
  display.fillRect(cx - 1, cy + 4, 2, 2, bg);
}

void drawSosScreen(uint32_t now) {
  uint32_t e = now - sosT0;
  bool inv = ((e / 350) % 2) == 0;
  uint16_t fg = inv ? BLACK : WHITE;
  uint16_t bg = inv ? WHITE : BLACK;
  if (inv) display.fillRect(0, 0, OLED_W, OLED_H, WHITE);

  display.drawRect(0, 0, OLED_W, OLED_H, fg);
  display.drawRect(1, 1, OLED_W - 2, OLED_H - 2, fg);

  drawWarnIcon(14, 14, fg, bg);
  drawWarnIcon(114, 14, fg, bg);

  display.setTextColor(fg);
  display.setTextSize(2);
  display.setCursor(46, 6);
  display.print(F("SOS"));

  bool alt = ((e / 700) % 2) == 1;
  const char* big = alt ? (sosSent ? "SOS SENT!" : "SENDING...") : "EMERGENCY!";
  int w = strlen(big) * 12;
  display.setTextSize(2);
  display.setCursor((OLED_W - w) / 2, 26);
  display.print(big);

  display.setTextSize(1);
  const char* st = sosSent ? "Alert sent to phone"
                           : (Blynk.connected() ? "Sending alert..." : "Offline - retrying");
  display.setCursor((OLED_W - strlen(st) * 6) / 2, 44);
  display.print(st);

  if (e >= SOS_ACK_GUARD_MS) {
    display.setCursor((OLED_W - 13 * 6) / 2, 54);
    display.print(F("Touch to stop"));
  }
}

void drawSleep(uint32_t t) {
  drawArc(38, 30, 10, 20, 160, 1);
  drawArc(90, 30, 10, 20, 160, 1);
  int r = 2 + (int)((sinf(t / 600.0f) + 1.0f) * 1.5f);
  display.drawCircle(64, 50, r, WHITE);
  for (int k = 0; k < 3; k++) {
    int ph = (int)(((t / 50) + k * 15) % 45);
    display.setTextColor(WHITE);
    display.setTextSize(k == 0 ? 1 : 2);
    display.setCursor(76 + k * 10 + ph / 3, 44 - ph);
    display.print(k == 0 ? 'z' : 'Z');
  }
}

// ---------------------------- Info screens ----------------------------------
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
  display.print(F("Imam Buddy"));
}

void drawDateScreen(uint32_t t) {
  (void)t;
  display.setTextColor(WHITE);
  display.setTextSize(2);

  const char* day = timeClient.isTimeSet() ? DAYS_OF_WEEK[timeClient.getDay()] : "----";
  int w = strlen(day) * 12;
  display.setCursor((OLED_W - w) / 2, 12);
  display.print(day);

  char d[16];
  getDateStr(d, sizeof(d));
  w = strlen(d) * 12;
  display.setCursor(w >= OLED_W ? 0 : (OLED_W - w) / 2, 34);
  display.print(d);

  display.setTextSize(1);
  display.setCursor(31, 56);
  display.print(F("Imam Buddy"));
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

void drawMoon(int cx, int cy, int r) {
  display.fillCircle(cx, cy, r, WHITE);
  display.fillCircle(cx + r / 2 + 1, cy - r / 3 - 1, r - 1, BLACK);
}

void drawNightStars(int cx, int cy, uint32_t t) {
  if (((t / 600) % 2) == 0) drawStar(cx + 15, cy - 11);
  if (((t / 600) % 2) == 1) drawStar(cx + 17, cy + 3);
  display.drawPixel(cx + 12, cy + 12, WHITE);
  display.drawPixel(cx - 14, cy - 10, WHITE);
}

void drawWeatherIcon(WeatherIcon ic, int cx, int cy, uint32_t t, bool night) {
  switch (ic) {
    case WI_SUN:
      if (night) {
        drawMoon(cx, cy, 11);
        drawNightStars(cx, cy, t);
      } else {
        display.fillCircle(cx, cy, 9, WHITE);
        drawSunRays(cx, cy, 13, 18, t);
      }
      break;
    case WI_PARTLY:
      if (night) {
        drawMoon(cx + 7, cy - 7, 8);
        if (((t / 700) % 2) == 0) display.drawPixel(cx - 8, cy - 14, WHITE);
      } else {
        display.fillCircle(cx + 7, cy - 7, 7, WHITE);
        drawSunRays(cx + 7, cy - 7, 10, 14, t);
      }
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
  bool night = isNightNow();

  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(cityName);

  drawWeatherIcon(weatherIcon, 26, 34, t, night);

  const char* temp = weatherTemp[0] ? weatherTemp : "--";
  size_t tl = strlen(temp);
  display.setTextSize(tl <= 3 ? 3 : 2);
  display.setCursor(60, tl <= 3 ? 18 : 22);
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
    display.print(F("ALARM!"));
  }
  display.setTextSize(1);
  display.setCursor(20, 35);
  display.print(F("Wake Up Imam!"));
  display.setCursor(22, 52);
  display.print(F("Touch to stop"));
}

void drawMessage(uint32_t now) {
  display.setTextWrap(false);
  display.setTextColor(WHITE);
  display.setTextSize(1);

  if (msgUnread) {
    if ((now / 400) % 2 == 0) {
      display.setCursor(16, 0);
      display.print(F("** NEW MESSAGE **"));
    }
    display.setCursor(22, 56);
    display.print(F("Touch to silence"));
  } else {
    uint32_t left = (MSG_READ_WINDOW - (now - msgReadT0) + 999) / 1000;
    display.setCursor(0, 0);
    display.print(F("Message"));
    display.setCursor(86, 0);
    display.printf("%2lus left", (unsigned long)left);
    display.setCursor(31, 56);
    display.print(F("Imam Buddy"));
  }

  int span = OLED_W + msgPixelW;
  int x = OLED_W - (int)((((now - msgScrollT0) * 90UL) / 1000UL) % (uint32_t)span);
  display.setTextSize(3);
  display.setCursor(x, 32);
  display.print(msg);
}

void drawEffect(uint32_t now) {
  uint32_t e = now - fxStartMs;
  switch (fxMode) {
    case FX_TRICK:  drawFaceTrick(now);          break;
    case FX_SCARED: drawFaceScared(now);         break;
    case FX_WAKE:   drawFaceWake(e);             break;
    case FX_SLAP:   drawFaceSlap(e, now, fxSide); break;
    case FX_PINCH:  drawFacePinch(now);          break;
    case FX_SOS:    drawSosScreen(now);          break;
    default: break;
  }
}

// ---------------------------- Menu screens ----------------------------------
void printHL(const char* s, int x, int y, uint8_t size, bool hl) {
  int w = strlen(s) * 6 * size, h = 8 * size;
  if (hl) {
    display.fillRect(x - 2, y - 1, w + 3, h + 2, WHITE);
    display.setTextColor(BLACK);
  } else {
    display.setTextColor(WHITE);
  }
  display.setTextSize(size);
  display.setCursor(x, y);
  display.print(s);
  display.setTextColor(WHITE);
}

void printCentered(const char* s, int y) {
  uint8_t size = (strlen(s) * 12 <= 116) ? 2 : 1;
  int w = strlen(s) * 6 * size;
  display.setTextSize(size);
  display.setCursor((OLED_W - w) / 2, size == 2 ? y : y + 4);
  display.print(s);
}

void drawArrows() {
  display.fillTriangle(2, 36, 8, 31, 8, 41, WHITE);
  display.fillTriangle(125, 36, 119, 31, 119, 41, WHITE);
}

void drawMenu(uint32_t now) {
  (void)now;
  display.setTextColor(WHITE);
  display.setTextWrap(false);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print(F("MENU"));
  display.setCursor(32, 0);
  display.print(F("hold=exit"));
  char b[24];
  snprintf(b, sizeof(b), "%u/%u", (unsigned)(menuIdx + 1), (unsigned)MENU_ITEMS);
  display.setCursor(OLED_W - strlen(b) * 6, 0);
  display.print(b);
  display.drawFastHLine(0, 9, OLED_W, WHITE);

  display.setCursor(0, 13);
  switch (menuIdx) {
    case MI_ALARM:    display.print(F("Alarm Setting")); break;
    case MI_CITY:     display.print(F("Weather City"));  break;
    case MI_SLEEP:    display.print(F("Sleep Timeout")); break;
    case MI_MOOD:     display.print(F("Mood Lock"));     break;
    case MI_VOL:      display.print(F("Beep Volume"));   break;
    case MI_REPEATER: display.print(F("WiFi Repeater")); break;
  }

  switch (menuIdx) {
    case MI_ALARM: {
      bool on, pm; int h12, m;
      if (menuEdit) { on = wAlarmOn; pm = wPM; h12 = wH12; m = wM; }
      else {
        on = alarmEnabled;
        int h24 = alarmMin / 60;
        pm = (h24 >= 12);
        h12 = h24 % 12; if (h12 == 0) h12 = 12;
        m = alarmMin % 60;
      }
      char hh[4], mm[4];
      snprintf(hh, sizeof(hh), "%02d", h12);
      snprintf(mm, sizeof(mm), "%02d", m);
      printHL(hh, 4, 25, 2, menuEdit && menuField == 1);
      display.setTextColor(WHITE);
      display.setTextSize(2);
      display.setCursor(30, 25);
      display.print(':');
      printHL(mm, 44, 25, 2, menuEdit && menuField == 2);
      printHL(pm ? "PM" : "AM", 82, 25, 2, menuEdit && menuField == 3);
      display.setTextSize(1);
      display.setCursor(4, 44);
      display.print(F("Alarm:"));
      printHL(on ? "ON " : "OFF", 46, 44, 1, menuEdit && menuField == 0);
      if (menuEdit) {
        const char* st = (menuField == 0) ? "step ON/OFF" :
                         (menuField == 1) ? "step HOUR"   :
                         (menuField == 2) ? "step MINUTE" : "step AM/PM";
        display.setCursor(OLED_W - strlen(st) * 6, 44);
        display.print(st);
      }
      break;
    }
    case MI_CITY: {
      char nm[13];
      cityNameOf(menuEdit ? wCity : cityIdx, nm);
      printCentered(nm, 26);
      display.setTextSize(1);
      snprintf(b, sizeof(b), "%u / %u", (unsigned)((menuEdit ? wCity : cityIdx) + 1), (unsigned)NUM_CITIES);
      display.setCursor((OLED_W - strlen(b) * 6) / 2, 46);
      display.print(b);
      if (menuEdit) drawArrows();
      break;
    }
    case MI_SLEEP:
      printCentered(SLEEP_LABELS[menuEdit ? wSleep : sleepOpt], 26);
      if (menuEdit) drawArrows();
      break;
    case MI_MOOD: {
      uint8_t v = menuEdit ? wMood : moodLock;
      printCentered(MOOD_LABELS[v], 26);
      display.setTextSize(1);
      const char* hint = (v == 0) ? "Mood changes freely" : "Mood stays fixed";
      display.setCursor((OLED_W - strlen(hint) * 6) / 2, 46);
      display.print(hint);
      if (menuEdit) drawArrows();
      break;
    }
    case MI_VOL: {
      uint8_t v = menuEdit ? wVol : volume;
      for (int i = 0; i < 5; i++) {
        int h = 4 + i * 3;
        int x = 22 + i * 20;
        if (i < v) display.fillRect(x, 42 - h, 14, h, WHITE);
        else       display.drawRect(x, 42 - h, 14, h, WHITE);
      }
      display.setTextSize(1);
      snprintf(b, sizeof(b), "Level %u/5", (unsigned)v);
      display.setCursor((OLED_W - strlen(b) * 6) / 2, 46);
      display.print(b);
      break;
    }
    case MI_REPEATER: {
      bool v = menuEdit ? wRepeater : repeaterEnabled;
      printCentered(v ? "ON" : "OFF", 26);
      display.setTextSize(1);
      const char* st;
      if (menuEdit)                st = "SSID: Buddy";
      else if (!repeaterEnabled)   st = "SSID: Buddy";
      else if (repeaterUp)         st = "Active: Buddy";
      else if (repeaterNoNapt)     st = "No NAPT in core";
      else                         st = "Starting...";
      display.setCursor((OLED_W - strlen(st) * 6) / 2, 46);
      display.print(st);
      if (menuEdit) drawArrows();
      break;
    }
  }

  display.setTextSize(1);
  display.setTextColor(WHITE);
  const char* hint;
  if (menuEdit) {
    bool lastStep = (menuIdx != MI_ALARM) || (menuField == 3) || (menuField == 0 && !wAlarmOn);
    hint = lastStep ? "L- R+  H:SAVE" : "L- R+  H:next";
  } else {
    hint = "H:Edit  L<  R>";
  }
  display.setCursor((OLED_W - strlen(hint) * 6) / 2, 56);
  display.print(hint);
}

// ============================================================================
//  DISPLAY SERVICE
// ============================================================================
void serviceDisplay(uint32_t now) {
  if (sleeping) {
    if (now - sleepStartMs >= sleepDispOffDelay) {
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
  if (sosActive)                                                mode = DM_FX;
  else if (alarmTriggered)                                      mode = DM_ALARM;
  else if (menuActive)                                          mode = DM_MENU;
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
    case DM_MENU:     drawMenu(now);        break;
    case DM_SLEEP:    drawSleep(now);       break;
    case DM_FX:       drawEffect(now);      break;
    case DM_MESSAGE:  drawMessage(now);     break;
    case DM_INTERACT: drawFace(now);        break;
    case DM_STANDBY: {
      uint32_t tClock = STANDBY_FACE_MS + STANDBY_CLOCK_MS;
      uint32_t tDate  = tClock + STANDBY_DATE_MS;
      uint32_t cycle  = tDate + (weatherValid ? STANDBY_WEATHER_MS : 0);
      uint32_t ph = (now - standbyT0) % cycle;
      if (ph < STANDBY_FACE_MS) drawFace(now);
      else if (ph < tClock)     drawClock(now);
      else if (ph < tDate)      drawDateScreen(now);
      else                      drawWeather(now);
      break;
    }
  }

  if (repeaterEnabled && (mode == DM_STANDBY || mode == DM_INTERACT)) drawRepeaterIcon(now);

  display.display();
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  timer1_attachInterrupt(bzISR);

  loadSettings();

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
  display.print(F("Imam Buddy waking"));
  display.setCursor(18, 36);
  display.print(F("Don't touch me..."));
  display.display();

  calibrateTouch();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_LIGHT_SLEEP);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Blynk.config(BLYNK_AUTH_TOKEN);
  timeClient.begin();

  uint32_t now = millis();
  lastDecayMs    = now;
  lastTouchMs    = now;
  lastActivityMs = now;
  standbyT0      = now;
  repeaterNextMs = now;
  weatherNextFetchMs = now + 10000;
}

void loop() {
  uint32_t now = millis();
  serviceNetwork(now);
  serviceRepeater(now);
  serviceTouch(now);
  serviceGestures(now);
  serviceMood(now);
  serviceMessage(now);
  serviceSos(now);
  serviceAlarm(now);
  serviceAutoSleep(now);
  serviceWeatherSync(now);
  serviceEffects(now);
  serviceBuzzer(now);
  serviceBlynkOut(now);
  serviceSettings(now);
  serviceDisplay(now);

  if (sleeping && !displayIsOn) delay(repeaterUp ? 1 : 20);
}
