// Smart green-area irrigation node - NodeMCU ESP8266
// Reads DHT22 + soil + tank level, shows them on the LCD, publishes them over MQTT
// and waters automatically: only when the soil is dry, not in hot and dry air
// (water evaporates), and adjusts itself from the AI leaf check (green / yellow).
// Irrigation keeps working without Wi-Fi.

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"
// Wi-Fi and MQTT settings: copy secrets.example.h to secrets.h and fill it in
#include "secrets.h"

// ---------- Wiring ----------
// 0 = no extra parts: soil on NodeMCU A0, tank probe (2 bare wires) on D6
// 1 = later upgrade: soil on ADS1115 A0, tank sensor on ADS1115 A1
#define USE_ADS1115 0
// 0 = no tank sensor (Sara's schematic): the pump is never blocked for an empty tank
// 1 = tank probe (2 bare wires) on D6
#define USE_TANK_PROBE 0
#if USE_ADS1115
#include <Adafruit_ADS1X15.h>
#endif

#define DHTPIN     D2   // GPIO4
#define DHTTYPE    DHT22
#define RELAY_PIN  D5   // GPIO14
#define TANK_PROBE D6   // GPIO12, bare-wire tank probe
#define I2C_SDA    D3   // GPIO0
#define I2C_SCL    D1   // GPIO5

// Relay from the report switches ON with HIGH. If your pump runs when it should
// be off, your relay is "low level trigger": swap HIGH and LOW here.
#define RELAY_ON   HIGH
#define RELAY_OFF  LOW

// ---------- Calibration (Stage 4 of the guide) ----------
#if USE_ADS1115
int SOIL_DRY_RAW    = 20000;  // sensor in dry soil or in the air
int SOIL_WET_RAW    = 9000;   // sensor in a glass of water
int TANK_EMPTY_RAW  = 300;    // level sensor out of the water
int TANK_FULL_RAW   = 15000;  // level sensor in water up to its line
#else
int SOIL_DRY_RAW    = 1024;
int SOIL_WET_RAW    = 360;
#endif

// ---------- Irrigation rules ----------
int  dryThreshold = 35;                    // % : start watering below this
int  wetTarget    = 55;                    // % : stop watering at this
const int TANK_MIN = 15;                   // % : never run the pump below this
const unsigned long PUMP_MAX_MS   = 5000;  // one automatic shot lasts 5 s max
const unsigned long MANUAL_MAX_MS = 30000; // manual ON stops by itself after 30 s
const unsigned long SOAK_MS       = 30000; // wait for the water to spread
const float HOT_C        = 30.0;           // hot and dry air = most water evaporates:
const float DRY_AIR_RH   = 30.0;           //   above 30 C and below 30 % humidity, wait
const int   SOIL_CRITICAL = 15;            // % : below this, water even in the heat
float PUMP_ML_PER_S = 20.0;                // measure it: Stage 5

// ---------- MQTT topics ----------
const char* T_TEMP      = "iotfrontier/temperature";
const char* T_HUM       = "iotfrontier/humidity";
const char* T_SOIL      = "iotfrontier/soil";
const char* T_TANK      = "iotfrontier/waterlevel";
const char* T_PUMP_CMD  = "iotfrontier/pump";            // ON / OFF (manual)
const char* T_PUMP_ST   = "iotfrontier/pump/state";
const char* T_MODE_CMD  = "iotfrontier/mode";            // AUTO / MANUAL
const char* T_MODE_ST   = "iotfrontier/mode/state";
const char* T_THR_CMD   = "iotfrontier/threshold";       // number 10..80
const char* T_THR_ST    = "iotfrontier/threshold/state";
const char* T_WATER     = "iotfrontier/water_used_ml";
const char* T_EVENT     = "iotfrontier/event";
const char* T_STATUS    = "iotfrontier/status";          // online / offline
const char* T_LEAF      = "iotfrontier/leaf";            // green / yellow (AI result)
const char* T_DIAG      = "iotfrontier/diagnosis";

LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient mqtt(espClient);
#if USE_ADS1115
Adafruit_ADS1115 ads;
#endif

float temperature = NAN, humidity = NAN;
int soilRaw = 0, soilPercent = 0, tankRaw = 0, tankPercent = 100;
bool autoMode = true, pumpOn = false;
unsigned long pumpStartMs = 0, lastPumpStopMs = 0;
float waterUsedMl = 0;
unsigned long lastRead = 0, lastPublish = 0, lastMqttTry = 0, lastWifiLog = 0;
bool wifiWasUp = false;
bool heatHold = false;                     // true while waiting for cooler air
String leafState = "unknown";

int toPercent(long raw, long raw0, long raw100) {
  return constrain(map(raw, raw0, raw100, 0, 100), 0, 100);
}

void publish(const char* topic, const String& value, bool retained = false) {
  if (mqtt.connected()) mqtt.publish(topic, value.c_str(), retained);
}

void publishSettings() {
  publish(T_MODE_ST, autoMode ? "AUTO" : "MANUAL", true);
  publish(T_THR_ST, String(dryThreshold), true);
  publish(T_PUMP_ST, pumpOn ? "ON" : "OFF", true);
}

void setPump(bool on, const char* reason) {
  if (on == pumpOn) return;
  if (on && tankPercent < TANK_MIN) {
    Serial.println("Pump blocked: tank empty");
    publish(T_EVENT, "blocked: tank empty");
    return;
  }
  pumpOn = on;
  digitalWrite(RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
  if (on) {
    pumpStartMs = millis();
  } else {
    waterUsedMl += (millis() - pumpStartMs) / 1000.0 * PUMP_ML_PER_S;
    lastPumpStopMs = millis();
    publish(T_WATER, String((int)waterUsedMl), true);
  }
  Serial.printf("Pump %s (%s)\n", on ? "ON" : "OFF", reason);
  publish(T_PUMP_ST, on ? "ON" : "OFF", true);
  publish(T_EVENT, String(on ? "pump on: " : "pump off: ") + reason);
}

void setMode(bool autoOn) {
  autoMode = autoOn;
  Serial.printf("Mode %s\n", autoMode ? "AUTO" : "MANUAL");
  publish(T_MODE_ST, autoMode ? "AUTO" : "MANUAL", true);
}

void onLeaf(const String& leaf);

void onMessage(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  String t(topic);
  Serial.printf("MQTT [%s] %s\n", topic, msg.c_str());

  if (t == T_PUMP_CMD) {
    if (autoMode) setMode(false);          // a manual command takes over
    if (msg == "ON")  setPump(true, "manual");
    if (msg == "OFF") setPump(false, "manual");
  } else if (t == T_MODE_CMD) {
    setMode(msg == "AUTO");
    if (!autoMode) setPump(false, "manual mode");
  } else if (t == T_LEAF) {
    msg.toLowerCase();
    onLeaf(msg);
  } else if (t == T_THR_CMD) {
    int v = msg.toInt();
    if (v >= 10 && v <= 80) {
      dryThreshold = v;
      wetTarget = min(v + 20, 95);
      publish(T_THR_ST, String(dryThreshold), true);
    }
  }
}

void readSensors() {
  float h = dht.readHumidity();
  float tc = dht.readTemperature();
  if (!isnan(h) && !isnan(tc)) { humidity = h; temperature = tc; }
  else Serial.println("DHT22 read failed (check DATA wire on D2)");

#if USE_ADS1115
  long s = 0, w = 0;
  for (int i = 0; i < 4; i++) {
    s += ads.readADC_SingleEnded(0);
    w += ads.readADC_SingleEnded(1);
  }
  soilRaw = s / 4;
  tankRaw = w / 4;
  tankPercent = toPercent(tankRaw, TANK_EMPTY_RAW, TANK_FULL_RAW);
#else
  long s = 0;
  for (int i = 0; i < 4; i++) s += analogRead(A0);
  soilRaw = s / 4;
#if USE_TANK_PROBE
  tankRaw = digitalRead(TANK_PROBE);          // LOW = wires touch water
  tankPercent = (tankRaw == LOW) ? 100 : 0;
#else
  tankRaw = 0;
  tankPercent = 100;                          // no tank sensor: always "OK"
#endif
#endif
  soilPercent = toPercent(soilRaw, SOIL_DRY_RAW, SOIL_WET_RAW);

  // Calibration line: write down these raw numbers in Stage 4
  Serial.printf("T=%.1fC H=%.0f%% soilRaw=%d soil=%d%% tankRaw=%d tank=%d%% pump=%s mode=%s\n",
                temperature, humidity, soilRaw, soilPercent, tankRaw, tankPercent,
                pumpOn ? "ON" : "OFF", autoMode ? "AUTO" : "MANUAL");
}

void controlPump() {
  unsigned long now = millis();
  // Safety rules, in every mode
  if (pumpOn && tankPercent < TANK_MIN) { setPump(false, "tank empty"); return; }
  if (pumpOn && now - pumpStartMs > (autoMode ? PUMP_MAX_MS : MANUAL_MAX_MS)) {
    setPump(false, "max run time");
    return;
  }
  if (!autoMode) return;
  bool hotDry = !isnan(temperature) && temperature > HOT_C && humidity < DRY_AIR_RH;
  if (!hotDry) heatHold = false;
  if (pumpOn && soilPercent >= wetTarget) { setPump(false, "soil wet enough"); return; }
  bool soaked = (lastPumpStopMs == 0) || (now - lastPumpStopMs > SOAK_MS);
  if (pumpOn || soilPercent >= dryThreshold || !soaked) return;

  // Smart timing: in hot and dry air most of the water evaporates, so wait for
  // cooler air. Exception: the soil is critically dry and the plant must survive.
  if (hotDry && soilPercent >= SOIL_CRITICAL) {
    if (!heatHold) {
      heatHold = true;
      Serial.println("Watering delayed: hot and dry air");
      publish(T_EVENT, "delayed: hot and dry air, water would evaporate");
    }
    return;
  }
  setPump(true, hotDry ? "soil critically dry" : "soil dry");
}

// AI leaf check. Yellow leaves alone do not say "too much water": they also come
// from too little water, nutrients or disease. The soil reading tells which one.
void onLeaf(const String& leaf) {
  if (leaf != "green" && leaf != "yellow") return;
  leafState = leaf;
  String diag;
  if (leaf == "green") {
    diag = "healthy: keep settings";
  } else if (soilPercent >= wetTarget) {
    dryThreshold = max(dryThreshold - 5, 20);     // water less often
    diag = "yellow + wet soil: overwatering, threshold lowered";
  } else if (soilPercent < dryThreshold) {
    dryThreshold = min(dryThreshold + 5, 60);     // water sooner
    diag = "yellow + dry soil: underwatering, threshold raised";
  } else {
    diag = "yellow + normal soil: check nutrients or disease";
  }
  wetTarget = min(dryThreshold + 20, 95);
  Serial.printf("Leaf %s -> %s (threshold %d%%)\n", leaf.c_str(), diag.c_str(), dryThreshold);
  publish(T_DIAG, diag, true);
  publish(T_THR_ST, String(dryThreshold), true);
  publish(T_EVENT, "leaf " + leaf);
}

void showLcd() {
  char line[17];
  snprintf(line, sizeof(line), "T%5.1fC H%3.0f%%   ", temperature, humidity);
  lcd.setCursor(0, 0); lcd.print(line);
  const char* state = pumpOn ? "ON " : (heatHold ? "HOT" : (autoMode ? "AUT" : "MAN"));
#if USE_ADS1115 || USE_TANK_PROBE
  snprintf(line, sizeof(line), "S%3d%% W%3d%% %s ", soilPercent, tankPercent, state);
#else
  snprintf(line, sizeof(line), "Soil%3d%%  %s   ", soilPercent, state);
#endif
  lcd.setCursor(0, 1); lcd.print(line);
}

// Prints the Wi-Fi state so problems are visible in the Serial Monitor
void watchWifi() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiWasUp) {
    Serial.print("Wi-Fi connected. Board IP: ");
    Serial.print(WiFi.localIP());
    Serial.print("  gateway: ");
    Serial.println(WiFi.gatewayIP());
  }
  if (!up && millis() - lastWifiLog > 5000) {
    lastWifiLog = millis();
    Serial.printf("Wi-Fi NOT connected (status %d). Looking for \"%s\"\n", WiFi.status(), WiFi.SSID().c_str());
  }
  wifiWasUp = up;
}

// iPhones write "It's" with a curly apostrophe. Treat both apostrophes as the same.
String plainName(String name) {
  name.replace("\xE2\x80\x99", "'");
  return name;
}

// Lists the networks the board can see and returns the real name of our hotspot
String scanNetworks() {
  String found = WIFI_SSID;
  Serial.println("Networks the board can see:");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    Serial.printf("  \"%s\"  (%d dBm)\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    if (plainName(WiFi.SSID(i)) == plainName(WIFI_SSID)) found = WiFi.SSID(i);
  }
  if (n <= 0) Serial.println("  none: is the hotspot on, in 2.4 GHz (Maximize Compatibility)?");
  return found;
}

void keepMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttTry < 5000) return;   // retry every 5 s, never block
  lastMqttTry = millis();
  Serial.printf("Connecting to MQTT %s ... ", MQTT_HOST);
  if (mqtt.connect("smartfarm-esp8266", MQTT_USER, MQTT_PASS, T_STATUS, 1, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(T_STATUS, "online", true);
    mqtt.subscribe(T_PUMP_CMD);
    mqtt.subscribe(T_MODE_CMD);
    mqtt.subscribe(T_THR_CMD);
    mqtt.subscribe(T_LEAF);
    publishSettings();
  } else {
    Serial.printf("failed, rc=%d", mqtt.state());
    Serial.println(mqtt.state() == -2 ? "  -> laptop not reachable: check MQTT_HOST, Mosquitto running, firewall port 1883" : "");
  }
}

void publishReadings() {
  if (!isnan(temperature)) {
    publish(T_TEMP, String(temperature, 1));
    publish(T_HUM, String(humidity, 0));
  }
  publish(T_SOIL, String(soilPercent));
#if USE_ADS1115 || USE_TANK_PROBE
  publish(T_TANK, String(tankPercent));
#endif
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);         // pump off before anything else
  Serial.begin(115200);
  Serial.println("\nSmart farm starting");

  Wire.begin(I2C_SDA, I2C_SCL);               // before the LCD and the ADS1115
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Smart Farm");
  lcd.setCursor(0, 1); lcd.print("starting...");

#if USE_ADS1115
  if (!ads.begin(0x48)) Serial.println("ADS1115 not found (check SDA/SCL and ADDR->GND)");
  ads.setGain(GAIN_ONE);                      // 0 to 4.096 V input range
#elif USE_TANK_PROBE
  pinMode(TANK_PROBE, INPUT_PULLUP);
#endif
  dht.begin();

  WiFi.mode(WIFI_STA);
  String ssid = scanNetworks();
  WiFi.begin(ssid.c_str(), WIFI_PASS);           // the password is never printed
  Serial.printf("Connecting to Wi-Fi \"%s\" in the background\n", WIFI_SSID);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  delay(1500);
  lcd.clear();
}

void loop() {
  watchWifi();
  keepMqtt();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastRead >= 2000) {               // DHT22 needs 2 s between reads
    lastRead = now;
    readSensors();
    controlPump();
    showLcd();
  }
  if (pumpOn) controlPump();                  // stop on time even between reads
  if (now - lastPublish >= 5000) {
    lastPublish = now;
    publishReadings();
  }
}
