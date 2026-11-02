/*
 * ============================================================================
 *  IMAM BUDDY v3.3 - Interactive OLED Desk Pet  (NodeMCU V3 / ESP8266)
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
