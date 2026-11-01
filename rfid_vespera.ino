// RFID -> light pattern (NeoPixel + Vespera over MQTT)
// Arduino MKR WiFi 1010 + PN532 (I2C) + buzzer + green LED + NeoPixel LED
// Wi-Fi/MQTT parts based on the CASA0014 "mkr1010_mqtt_simple" sketch.
//
// Each tap:
//   1. beep + green LED on pin 4
//   2. NeoPixel flashes green quickly ("card read")
//   3. the card's unique colour pattern plays on the NeoPixel
//      (and on the Vespera too, if Wi-Fi + MQTT are connected)
//
// Works without Wi-Fi/MQTT: the reader, buzzer and LEDs keep working,
// and it quietly retries the connection in the background.
//
// Wiring:
//   PN532 (DIP 1 ON, 2 OFF = I2C): GND-GND, VCC-VCC, SDA-11, SCL-12
//   Buzzer:    + (or S) -> pin 5,  - -> GND
//   Green LED: pin 4 -> 220 ohm resistor -> long leg,  short leg -> GND
//   NeoPixel:  DIN -> pin 6,  5V -> 5V,  GND -> GND
//
// Libraries: Adafruit PN532, Adafruit NeoPixel, PubSubClient, WiFiNINA

#include <SPI.h>
#include <Wire.h>
#include <WiFiNINA.h>
#include <PubSubClient.h>
#include <Adafruit_PN532.h>
#include <Adafruit_NeoPixel.h>
#include <utility/wifi_drv.h>   // onboard RGB status LED
#include "arduino_secrets.h"

// ---------- Wi-Fi on/off ----------
// true  = try to connect to Wi-Fi + MQTT (on campus)
// false = skip Wi-Fi completely (at home) - starts instantly, no pauses
const bool WIFI_ENABLED = true;
const unsigned long WIFI_RETRY_MS = 60000;   // when offline, retry Wi-Fi every 60 s
const unsigned long MQTT_RETRY_MS = 10000;   // when Wi-Fi is up, retry MQTT every 10 s

// ---------- Wi-Fi / MQTT (same as the workshop sketch) ----------
const char* ssid          = SECRET_SSID;
const char* password      = SECRET_PASS;
const char* mqtt_username = SECRET_MQTTUSER;
const char* mqtt_password = SECRET_MQTTPASS;
const char* mqtt_server   = "mqtt.cetools.org";
const int   mqtt_port     = 1884;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

String lightId    = "21";   // your Vespera number
String mqtt_topic = "student/CASA0014/luminaire/" + lightId;
String clientId   = "";

// Vespera: 72 LEDs x 3 bytes (R, G, B)
const int num_leds     = 72;
const int payload_size = num_leds * 3;
byte RGBpayload[payload_size];

unsigned long lastConnectAttempt = 0;

// ---------- Pins ----------
const uint8_t PN532_IRQ   = 2;   // placeholders, not wired
const uint8_t PN532_RESET = 3;
const uint8_t LED_PIN     = 4;   // plain green LED
const uint8_t BUZZER_PIN  = 5;
const uint8_t PIXEL_PIN   = 6;   // NeoPixel DIN

// ---------- NeoPixel settings ----------
const uint16_t NUM_PIXELS       = 1;    // change if your LED has more than one pixel
const uint8_t  PIXEL_BRIGHTNESS = 80;   // 0-255

Adafruit_NeoPixel pixels(NUM_PIXELS, PIXEL_PIN, NEO_GRB + NEO_KHZ800);

// ---------- Buzzer settings ----------
const bool     ACTIVE_BUZZER = false;  // true = active buzzer, false = passive
const uint16_t BUZZER_FREQ   = 2700;   // passive only: loudest frequency from the test
const uint16_t BEEP_MS       = 150;    // beep length in milliseconds

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// ---------- Tap detection ----------
uint8_t lastUid[10];
uint8_t lastLen = 0;
unsigned long lastSeen = 0;
const unsigned long SAME_TAP_MS = 1500;   // card gone this long = new tap

// ---------- Green LED (pin 4) timing ----------
const unsigned long LED_ON_MS = 1000;
unsigned long ledOnSince = 0;
bool ledOn = false;

// ---------- Pattern settings ----------
struct Colour { const char* name; uint8_t r, g, b; };

const Colour PALETTE[] = {
  {"red",    255,   0,   0},
  {"orange", 255, 100,   0},
  {"yellow", 255, 220,   0},
  {"green",    0, 255,   0},
  {"cyan",     0, 255, 255},
  {"blue",     0,   0, 255},
  {"purple", 150,   0, 255},
  {"pink",   255,   0, 150},
};
const uint8_t PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);

const uint8_t  PATTERN_STEPS = 5;     // how many colours in a pattern
const uint16_t SHORT_MS = 300;        // short step length
const uint16_t LONG_MS  = 700;        // long step length

const uint16_t ACK_MS = 250;          // quick green "card read" flash on the NeoPixel
const uint16_t GAP_MS = 150;          // dark pause between the green flash and the pattern

struct Step {
  uint8_t  colour;   // index into PALETTE
  uint16_t ms;       // how long to show it
};

// ---------- Playback state ----------
// phase: 0 = idle, 1 = green flash, 2 = gap, 3 = playing pattern steps
Step pattern[PATTERN_STEPS];
uint8_t phase = 0;
uint8_t stepIndex = 0;
unsigned long phaseStarted = 0;

// =====================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  Serial.println(F("Starting RFID -> light pattern..."));

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pixels.begin();
  pixels.setBrightness(PIXEL_BRIGHTNESS);
  pixels.clear();
  pixels.show();

  // Onboard RGB status LED (blue = connecting, green = connected, red = offline)
  WiFiDrv::pinMode(25, OUTPUT);
  WiFiDrv::pinMode(26, OUTPUT);
  WiFiDrv::pinMode(27, OUTPUT);
  LedRed();

  // RFID reader
  nfc.begin();
  uint32_t versiondata = 0;
  for (int i = 0; i < 5 && !versiondata; i++) {
    versiondata = nfc.getFirmwareVersion();
    if (!versiondata) delay(200);
  }
  if (!versiondata) {
    Serial.println(F("PN532 not found - check wiring and DIP switches (1 ON, 2 OFF)"));
    while (true) {}
  }
  Serial.print(F("Found PN5"));
  Serial.println((versiondata >> 24) & 0xFF, HEX);
  nfc.SAMConfig();

  // MQTT setup (doesn't connect yet)
  byte mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
  clientId = "MKR1010_RFID_" + String(macStr);
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);

  if (WIFI_ENABLED) {
    Serial.print(F("Vespera topic: "));
    Serial.println(mqtt_topic);
    maintainConnection();          // first attempt
  } else {
    Serial.println(F("Wi-Fi disabled - running offline"));
  }

  Serial.println(F("Ready - tap a card..."));
}

void loop() {
  maintainConnection();
  if (mqttClient.connected()) mqttClient.loop();

  updateGreenLed();
  updatePlayback();

  // Ignore the reader while a pattern is playing
  if (phase != 0) return;

  uint8_t uid[10];
  uint8_t uidLength;

  // Short timeout so the loop keeps running
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
    return;   // no card this time
  }

  bool sameCard  = (uidLength == lastLen) && memcmp(uid, lastUid, uidLength) == 0;
  bool stillHeld = (millis() - lastSeen) < SAME_TAP_MS;
  lastSeen = millis();

  if (sameCard && stillHeld) {
    delay(100);
    return;   // same card still sitting on the reader - ignore
  }

  // ----- New tap -----
  memcpy(lastUid, uid, uidLength);
  lastLen = uidLength;

  greenOn();
  setPixels(0, 255, 0);     // quick green "card read" flash on the NeoPixel
  beep();

  printUid(uid, uidLength);
  makePattern(uid, uidLength, pattern);
  printPattern(pattern);

  phase = 1;
  phaseStarted = millis();
}

// =====================================================================
// Connection handling - never blocks the reader for long when offline

void maintainConnection() {
  if (!WIFI_ENABLED) return;
  if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) return;

  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  unsigned long wait = wifiUp ? MQTT_RETRY_MS : WIFI_RETRY_MS;
  if (lastConnectAttempt != 0 && millis() - lastConnectAttempt < wait) return;
  lastConnectAttempt = millis();

  if (!wifiUp) {
    startWifi();
  }
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    reconnectMQTT();
  }
  if (!mqttClient.connected()) {
    Serial.println(F("Offline - reader still works, will retry later"));
  }
}

// =====================================================================
// Pattern generation

// Mix the UID bytes into one 32-bit number (FNV-1a hash)
uint32_t fnv1a(const uint8_t* data, uint8_t len) {
  uint32_t h = 2166136261UL;
  for (uint8_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619UL;
  }
  return h;
}

// Simple repeatable random number generator (xorshift)
uint32_t nextRandom(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// Build the pattern for a card - same UID always gives the same pattern
void makePattern(const uint8_t* uid, uint8_t len, Step* out) {
  uint32_t state = fnv1a(uid, len);
  if (state == 0) state = 1;   // generator can't start at 0

  uint8_t prev = 255;
  for (uint8_t i = 0; i < PATTERN_STEPS; i++) {
    uint8_t c;
    do {
      c = nextRandom(state) % PALETTE_SIZE;
    } while (c == prev);       // never the same colour twice in a row
    prev = c;

    out[i].colour = c;
    out[i].ms = ((nextRandom(state) >> 16) & 1) ? LONG_MS : SHORT_MS;
  }
}

// =====================================================================
// Playback (non-blocking): green flash -> gap -> pattern steps -> off

void showStep(uint8_t i) {
  const Colour& c = PALETTE[pattern[i].colour];
  setPixels(c.r, c.g, c.b);
  send_colour_to_all(c.r, c.g, c.b);
  phaseStarted = millis();
}

void updatePlayback() {
  if (phase == 0) return;
  unsigned long elapsed = millis() - phaseStarted;

  if (phase == 1) {                       // green flash
    if (elapsed < ACK_MS) return;
    setPixels(0, 0, 0);
    phase = 2;
    phaseStarted = millis();
  } else if (phase == 2) {                // short dark gap
    if (elapsed < GAP_MS) return;
    phase = 3;
    stepIndex = 0;
    showStep(0);
  } else if (phase == 3) {                // pattern steps
    if (elapsed < pattern[stepIndex].ms) return;
    stepIndex++;
    if (stepIndex < PATTERN_STEPS) {
      showStep(stepIndex);
    } else {
      setPixels(0, 0, 0);
      send_all_off();
      phase = 0;
      lastSeen = millis();   // a card still on the reader won't retrigger straight away
      Serial.println(F("Pattern finished"));
    }
  }
}

// Set every pixel of the local NeoPixel LED to one colour
void setPixels(uint8_t r, uint8_t g, uint8_t b) {
  for (uint16_t i = 0; i < NUM_PIXELS; i++) {
    pixels.setPixelColor(i, pixels.Color(r, g, b));
  }
  pixels.show();
}

// Sets all 72 Vespera LEDs to (r, g, b) - silently skipped when offline
void send_colour_to_all(int r, int g, int b) {
  if (!mqttClient.connected()) return;
  for (int pixel = 0; pixel < num_leds; pixel++) {
    RGBpayload[pixel * 3 + 0] = (byte)constrain(r, 0, 255);
    RGBpayload[pixel * 3 + 1] = (byte)constrain(g, 0, 255);
    RGBpayload[pixel * 3 + 2] = (byte)constrain(b, 0, 255);
  }
  mqttClient.publish(mqtt_topic.c_str(), RGBpayload, payload_size);
}

// Turns all 72 Vespera LEDs off
void send_all_off() {
  send_colour_to_all(0, 0, 0);
}

// =====================================================================
// Buzzer, green LED, printing

void beep() {
  if (ACTIVE_BUZZER) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(BEEP_MS);
    digitalWrite(BUZZER_PIN, LOW);
  } else {
    tone(BUZZER_PIN, BUZZER_FREQ, BEEP_MS);
  }
}

void greenOn() {
  digitalWrite(LED_PIN, HIGH);
  ledOn = true;
  ledOnSince = millis();
}

void updateGreenLed() {
  if (ledOn && millis() - ledOnSince >= LED_ON_MS) {
    digitalWrite(LED_PIN, LOW);
    ledOn = false;
  }
}

void printUid(const uint8_t* uid, uint8_t len) {
  Serial.print(F("UID ("));
  Serial.print(len);
  Serial.print(F(" bytes): "));
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] < 0x10) Serial.print('0');
    Serial.print(uid[i], HEX);
    if (i < len - 1) Serial.print(':');
  }
  Serial.println();
}

void printPattern(const Step* p) {
  Serial.print(F("Pattern: "));
  for (uint8_t i = 0; i < PATTERN_STEPS; i++) {
    Serial.print(PALETTE[p[i].colour].name);
    Serial.print(p[i].ms == LONG_MS ? " (long)" : " (short)");
    if (i < PATTERN_STEPS - 1) Serial.print(F("  ->  "));
  }
  Serial.println();
}
