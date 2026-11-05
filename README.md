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
