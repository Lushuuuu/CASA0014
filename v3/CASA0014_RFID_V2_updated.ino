// CASA0014 — RFID card to Vespera lighting
// MKR WiFi 1010 + PN532 (I2C) + NeoPixel (D6) + buzzer (D5)
// Requires arduino_secrets.h in the same sketch folder (do not upload secrets to GitHub).
// Libraries: WiFiNINA, PubSubClient, Adafruit PN532, Adafruit NeoPixel

#include <Wire.h>
#include <SPI.h>
#include <WiFiNINA.h>
#include <PubSubClient.h>
#include <Adafruit_PN532.h>
#include <Adafruit_NeoPixel.h>
#include <utility/wifi_drv.h>
#include "arduino_secrets.h"

// Configuration
const bool WIFI_ENABLED = true;
const char* SSID = SECRET_SSID;
const char* PASSWORD = SECRET_PASS;
const char* MQTT_USER = SECRET_MQTTUSER;
const char* MQTT_PASS = SECRET_MQTTPASS;
const char* MQTT_SERVER = "mqtt.cetools.org";
const uint16_t MQTT_PORT = 1884;
const char* MQTT_TOPIC = "student/CASA0014/luminaire/21";

const uint8_t BUZZER_PIN = 5;
const uint8_t PIXEL_PIN = 6;
const uint8_t PN532_IRQ = 2;     // Required by library constructor; not physically wired
const uint8_t PN532_RESET = 3;   // Required by library constructor; not physically wired
const uint8_t PATTERN_STEPS = 5;
const uint8_t PIXEL_BRIGHTNESS = 80;
const uint16_t ACK_MS = 250;
const uint16_t GAP_MS = 150;
const uint16_t SHORT_MS = 300;
const uint16_t LONG_MS = 700;
const unsigned long WIFI_RETRY_MS = 60000;
const unsigned long MQTT_RETRY_MS = 10000;
const unsigned long SAME_TAP_MS = 1500;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
Adafruit_NeoPixel pixel(1, PIXEL_PIN, NEO_GRB + NEO_KHZ800);
String clientId;

struct Colour { const char* name; uint8_t r, g, b; };
const Colour PALETTE[] = {
  {"red", 255, 0, 0}, {"orange", 255, 100, 0},
  {"yellow", 255, 220, 0}, {"green", 0, 255, 0},
  {"cyan", 0, 255, 255}, {"blue", 0, 0, 255},
  {"purple", 150, 0, 255}, {"pink", 255, 0, 150}
};
const uint8_t PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);
struct Step { uint8_t colour; uint16_t duration; };
Step pattern[PATTERN_STEPS];

// Playback: 0 idle, 1 acknowledgement flash, 2 dark gap, 3 pattern
uint8_t phase = 0;
uint8_t stepIndex = 0;
unsigned long phaseStarted = 0;
unsigned long lastConnectAttempt = 0;
uint8_t lastUid[10] = {};
uint8_t lastLen = 0;
unsigned long lastSeen = 0;

// MKR WiFi 1010 onboard status LED: red offline, blue connecting, green online.
void statusLED(uint8_t red, uint8_t green, uint8_t blue) {
  WiFiDrv::analogWrite(25, red);
  WiFiDrv::analogWrite(26, green);
  WiFiDrv::analogWrite(27, blue);
}

void setPixel(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// One MQTT message = 72 LEDs * 3 RGB bytes = 216 bytes.
void publishColour(uint8_t r, uint8_t g, uint8_t b) {
  if (!mqtt.connected()) return;
  byte payload[72 * 3];
  for (uint16_t i = 0; i < sizeof(payload); i += 3) {
    payload[i] = r;
    payload[i + 1] = g;
    payload[i + 2] = b;
  }
  mqtt.publish(MQTT_TOPIC, payload, sizeof(payload));
}

// Same FNV-1a hash and xorshift generator as Version 1.
uint32_t hashUid(const uint8_t* uid, uint8_t length) {
  uint32_t hash = 2166136261UL;
  for (uint8_t i = 0; i < length; i++) {
    hash ^= uid[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

void makePattern(const uint8_t* uid, uint8_t length) {
  uint32_t state = hashUid(uid, length);
  if (state == 0) state = 1;
  uint8_t previous = 255;
  for (uint8_t i = 0; i < PATTERN_STEPS; i++) {
    uint8_t colour;
    do {
      colour = nextRandom(state) % PALETTE_SIZE;
    } while (colour == previous);
    previous = colour;
    pattern[i].colour = colour;
    pattern[i].duration = ((nextRandom(state) >> 16) & 1) ? LONG_MS : SHORT_MS;
  }
}

// Print the card UID and the exact sequence for repeatability testing.
void printScan(const uint8_t* uid, uint8_t length) {
  Serial.print(F("Card UID: "));
  for (uint8_t i = 0; i < length; i++) {
    if (uid[i] < 16) Serial.print('0');
    Serial.print(uid[i], HEX);
    if (i + 1 < length) Serial.print(':');
  }
  Serial.println();
  Serial.print(F("UID hash: 0x"));
  Serial.println(hashUid(uid, length), HEX);
  Serial.println(F("Five-step pattern:"));
  for (uint8_t i = 0; i < PATTERN_STEPS; i++) {
    Serial.print(F("  Step "));
    Serial.print(i + 1);
    Serial.print(F(": "));
    Serial.print(PALETTE[pattern[i].colour].name);
    Serial.print(F(" | "));
    Serial.print(pattern[i].duration);
    Serial.println(F(" ms"));
  }
  Serial.println(F("--------------------------"));
}

void showStep(uint8_t index) {
  const Colour& colour = PALETTE[pattern[index].colour];
  setPixel(colour.r, colour.g, colour.b);
  publishColour(colour.r, colour.g, colour.b);
  phaseStarted = millis();
}

void updatePlayback() {
  if (phase == 0) return;
  unsigned long elapsed = millis() - phaseStarted;

  if (phase == 1 && elapsed >= ACK_MS) {
    setPixel(0, 0, 0);
    phase = 2;
    phaseStarted = millis();
  } else if (phase == 2 && elapsed >= GAP_MS) {
    phase = 3;
    stepIndex = 0;
    showStep(0);
  } else if (phase == 3 && elapsed >= pattern[stepIndex].duration) {
    if (++stepIndex < PATTERN_STEPS) {
      showStep(stepIndex);
    } else {
      setPixel(0, 0, 0);
      publishColour(0, 0, 0);
      phase = 0;
      lastSeen = millis();
      Serial.println(F("Pattern finished"));
    }
  }
}

void connectWiFi() {
  statusLED(0, 0, 155);
  Serial.print(F("Connecting to Wi-Fi: "));
  Serial.println(SSID);
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println(F("Wi-Fi module not found"));
    while (true) {}
  }
  for (uint8_t attempt = 0; attempt < 5 && WiFi.status() != WL_CONNECTED; attempt++) {
    WiFi.begin(SSID, PASSWORD);
    delay(3000); // Like V1, reconnection can temporarily pause card reading.
  }
  if (WiFi.status() == WL_CONNECTED) {
    statusLED(0, 155, 0);
    Serial.println(F("Wi-Fi connected"));
  } else {
    statusLED(155, 0, 0);
    Serial.println(F("Wi-Fi unavailable; local mode continues"));
  }
}

void connectMQTT() {
  statusLED(0, 0, 155);
  wifiClient.stop();
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    statusLED(0, 155, 0);
    Serial.println(F("MQTT connected"));
  } else {
    statusLED(155, 0, 0);
    Serial.print(F("MQTT failed, code "));
    Serial.println(mqtt.state());
    delay(5000); // Preserved from V1.
  }
}

void maintainConnection() {
  if (!WIFI_ENABLED || (WiFi.status() == WL_CONNECTED && mqtt.connected())) return;
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  unsigned long retryInterval = wifiUp ? MQTT_RETRY_MS : WIFI_RETRY_MS;
  if (lastConnectAttempt && millis() - lastConnectAttempt < retryInterval) return;
  lastConnectAttempt = millis();
  if (!wifiUp) connectWiFi();
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) connectMQTT();
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pixel.begin();
  pixel.setBrightness(PIXEL_BRIGHTNESS);
  setPixel(0, 0, 0);

  WiFiDrv::pinMode(25, OUTPUT);
  WiFiDrv::pinMode(26, OUTPUT);
  WiFiDrv::pinMode(27, OUTPUT);
  statusLED(155, 0, 0);

  nfc.begin();
  uint32_t firmware = 0;
  for (uint8_t attempt = 0; attempt < 5 && !firmware; attempt++) {
    firmware = nfc.getFirmwareVersion();
    if (!firmware) delay(200);
  }
  if (!firmware) {
    Serial.println(F("PN532 not found: check I2C wiring and DIP switches"));
    while (true) {}
  }
  nfc.SAMConfig();

  byte mac[6];
  WiFi.macAddress(mac);
  char macText[18];
  snprintf(macText, sizeof(macText), "%02X%02X%02X%02X%02X%02X",
           mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
  clientId = "MKR1010_RFID_" + String(macText);
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  maintainConnection();
  Serial.println(F("Ready: tap a card"));
}

void loop() {
  maintainConnection();
  if (mqtt.connected()) mqtt.loop();
  updatePlayback();
  if (phase != 0) return;

  uint8_t uid[10];
  uint8_t length = 0;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &length, 100)) return;

  if (length == 0 || length > sizeof(uid)) {
    Serial.println(F("Invalid UID length; scan ignored"));
    return;
  }

  bool sameCard = length == lastLen && memcmp(uid, lastUid, length) == 0;
  bool recent = millis() - lastSeen < SAME_TAP_MS;
  lastSeen = millis();
  if (sameCard && recent) return;

  memcpy(lastUid, uid, length);
  lastLen = length;
  makePattern(uid, length);
  printScan(uid, length);
  setPixel(0, 255, 0);
  tone(BUZZER_PIN, 2700, 150); // Passive buzzer, as in Version 1
  phase = 1;
  phaseStarted = millis();
  Serial.println(F("Card detected; playing five-step pattern"));
}
