#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <PubSubClient.h> // MQTT-Bibliothek
#include <esp_task_wdt.h> // Watchdog


// ==== WLAN Heimnetz ====
const char* ssid     = "xx";
const char* password = "xx";

// ==== MQTT-Einstellungen ====
const char* mqtt_server = "192.168.178.xx"; // IP vom Home Assistant MQTT-Broker
const int   mqtt_port   = 1883;
const char* mqtt_user   = "xx";
const char* mqtt_pass   = "xx";
const char* mqtt_client_id = "KellerlueftungESP32";

// Home Assistant MQTT-Discovery: legt Sensoren und Schalter automatisch an.
// Auf false lassen, wenn die Entitäten bereits per YAML angelegt sind (sonst doppelt).
const bool HA_DISCOVERY = false;

// ==== MQTT-Topics ====
#define TOPIC_BASE           "kellerlueftung"
#define TOPIC_AVAILABILITY   TOPIC_BASE "/status"       // online / offline (Last Will)
#define TOPIC_RELAY_SET      TOPIC_BASE "/relais/set"   // ON / OFF -> schaltet in Handbetrieb
#define TOPIC_RELAY_STATUS   TOPIC_BASE "/relais/status"
#define TOPIC_AUTO_SET       TOPIC_BASE "/auto/set"     // ON = Automatik, OFF = Handbetrieb
#define TOPIC_AUTO_STATUS    TOPIC_BASE "/auto/status"

// ==== I²C-Bus 1 (Standard-Pins ESP32) ====
#define SDA_1 21
#define SCL_1 22

// ==== I²C-Bus 2 ====
#define SDA_2 17
#define SCL_2 16

// ==== Relais ====
#define RELAY_PIN 5 // GPIO für Relaismodul (LOW = an, HIGH = aus)

// ==== TwoWire Instanzen ====
TwoWire I2CBus1 = TwoWire(0);
TwoWire I2CBus2 = TwoWire(1);

// ==== BME280-Objekte ====
Adafruit_BME280 bme1; // Innen
Adafruit_BME280 bme2; // Außen

// ==== Steuerparameter ====
const float AH_ON_MARGIN  = 0.5; // g/m³ – draußen so viel trockener, um einzuschalten
const float AH_OFF_MARGIN = 0.1; // g/m³ – unter diesen Vorteil wieder aus
const float MIN_OUT_TEMP   = 5.0;         // °C
const unsigned long MIN_ON_TIME  = 120000; // ms
const unsigned long MIN_OFF_TIME = 10000; // ms

// ==== Zeitsteuerung (nicht blockierend) ====
const unsigned long MEASURE_INTERVAL    = 10000; // ms – Messen, Regeln, Publizieren
const unsigned long MQTT_RETRY_INTERVAL = 5000;  // ms – Abstand zwischen MQTT-Verbindungsversuchen
const unsigned long WIFI_CONNECT_TIMEOUT = 20000; // ms – max. Wartezeit auf WLAN in setup()
const uint32_t      WDT_TIMEOUT_S = 30;          // s  – Watchdog-Timeout

unsigned long lastSwitchTime = 0;
unsigned long lastMeasureTime = 0;
unsigned long lastMqttAttempt = 0;
bool firstMeasure = true;
bool relayState = false;
bool autoMode = true; // false = Handbetrieb über MQTT

// ==== Webserver ====
WebServer server(80);

// ==== Globale Messwerte ====
float tempIn = NAN, humIn = NAN, ahIn = NAN;
float tempOut = NAN, humOut = NAN, ahOut = NAN;
float ahDiff = NAN;
bool sensorsOk = false;

// ==== WiFi / MQTT Objekte ====
WiFiClient espClient;
PubSubClient client(espClient);

// ===== Hilfsfunktionen =====
float absoluteHumidity(float tempC, float rhPercent) {
  float e_s = 6.112 * exp((17.62 * tempC) / (243.12 + tempC));
  float e   = (rhPercent / 100.0) * e_s;
  float ah  = 216.7 * e / (tempC + 273.15);
  return ah;
}
static inline bool validT(float v){ return (v > -30.0f && v < 70.0f) && !isnan(v); }
static inline bool validRH(float v){ return (v >= 0.0f && v <= 100.0f) && !isnan(v); }
static inline bool validAH(float v){ return (v >= 0.0f && v <= 50.0f) && !isnan(v); }
bool sensorsValid() {
  return validT(tempIn) && validRH(humIn) && validAH(ahIn)
      && validT(tempOut)&& validRH(humOut)&& validAH(ahOut);
}

// Watchdog einrichten – API unterscheidet sich zwischen ESP32-Arduino-Core 2.x und 3.x
void setupWatchdog() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  // Core 3.x initialisiert den Task-Watchdog u. U. bereits selbst -> dann nur umkonfigurieren
  if (esp_task_wdt_init(&wdtConfig) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdtConfig);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true); // Reset bei Timeout
#endif
  esp_task_wdt_add(NULL); // aktuelle Task (loop) überwachen
}

// ==== MQTT Publizieren ====
void publishState() {
  if (!client.connected()) return;
  client.publish(TOPIC_RELAY_STATUS, relayState ? "ON" : "OFF", true);
  client.publish(TOPIC_AUTO_STATUS,  autoMode   ? "ON" : "OFF", true);
}

// Ungültige Werte werden nicht gesendet, damit in Home Assistant kein "nan" landet
void publishValue(const char* topic, float value, bool valid) {
  if (valid) client.publish(topic, String(value, 2).c_str(), true);
}

void publishMeasurements() {
  if (!client.connected()) return;
  publishValue(TOPIC_BASE "/temp_in",  tempIn,  validT(tempIn));
  publishValue(TOPIC_BASE "/hum_in",   humIn,   validRH(humIn));
  publishValue(TOPIC_BASE "/ah_in",    ahIn,    validAH(ahIn));
  publishValue(TOPIC_BASE "/temp_out", tempOut, validT(tempOut));
  publishValue(TOPIC_BASE "/hum_out",  humOut,  validRH(humOut));
  publishValue(TOPIC_BASE "/ah_out",   ahOut,   validAH(ahOut));
}

// ==== Home Assistant Discovery ====
const char* HA_DEVICE =
  "\"device\":{\"identifiers\":[\"kellerlueftung\"],\"name\":\"Kellerlüftung\",\"model\":\"ESP32 + 2x BME280\"},"
  "\"availability_topic\":\"" TOPIC_AVAILABILITY "\"";

void publishDiscoverySensor(const char* id, const char* name, const char* unit, const char* deviceClass) {
  String payload = "{\"name\":\"" + String(name) + "\","
                   "\"unique_id\":\"kellerlueftung_" + id + "\","
                   "\"state_topic\":\"" TOPIC_BASE "/" + id + "\","
                   "\"unit_of_measurement\":\"" + unit + "\","
                   "\"state_class\":\"measurement\",";
  if (deviceClass) payload += "\"device_class\":\"" + String(deviceClass) + "\",";
  payload += HA_DEVICE;
  payload += "}";
  String topic = "homeassistant/sensor/kellerlueftung_" + String(id) + "/config";
  client.publish(topic.c_str(), payload.c_str(), true);
}

void publishDiscoverySwitch(const char* id, const char* name, const char* cmdTopic, const char* stateTopic) {
  String payload = "{\"name\":\"" + String(name) + "\","
                   "\"unique_id\":\"kellerlueftung_" + id + "\","
                   "\"command_topic\":\"" + cmdTopic + "\","
                   "\"state_topic\":\"" + stateTopic + "\","
                   "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",";
  payload += HA_DEVICE;
  payload += "}";
  String topic = "homeassistant/switch/kellerlueftung_" + String(id) + "/config";
  client.publish(topic.c_str(), payload.c_str(), true);
}

void publishDiscovery() {
  publishDiscoverySensor("temp_in",  "Temperatur innen",        "°C",   "temperature");
  publishDiscoverySensor("hum_in",   "Luftfeuchte innen",       "%",    "humidity");
  publishDiscoverySensor("ah_in",    "Absolute Feuchte innen",  "g/m³", nullptr);
  publishDiscoverySensor("temp_out", "Temperatur außen",        "°C",   "temperature");
  publishDiscoverySensor("hum_out",  "Luftfeuchte außen",       "%",    "humidity");
  publishDiscoverySensor("ah_out",   "Absolute Feuchte außen",  "g/m³", nullptr);
  publishDiscoverySwitch("relais", "Lüfter",    TOPIC_RELAY_SET, TOPIC_RELAY_STATUS);
  publishDiscoverySwitch("auto",   "Automatik", TOPIC_AUTO_SET,  TOPIC_AUTO_STATUS);
}

// ==== Relais ====
void setRelay(bool on, const char* reason) {
  if (relayState == on) return;
  relayState = on;
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  lastSwitchTime = millis();
  publishState();
  Serial.printf("Relais %s (%s)\n", on ? "EIN" : "AUS", reason);
}

// ==== Webserver ====
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta http-equiv='refresh' content='10'>";
  html += "<title>Kellerlüftung</title></head><body>";
  html += "<h1>Kellerlüftung - Sensorwerte</h1>";
  html += "<table border='1'><tr><th>Messwert</th><th>Innen</th><th>Außen</th></tr>";
  html += "<tr><td>Temperatur (°C)</td><td>" + String(tempIn,2) + "</td><td>" + String(tempOut,2) + "</td></tr>";
  html += "<tr><td>Relative Luftfeuchte (%)</td><td>" + String(humIn,2) + "</td><td>" + String(humOut,2) + "</td></tr>";
  html += "<tr><td>Absolute Feuchte (g/m³)</td><td>" + String(ahIn,2) + "</td><td>" + String(ahOut,2) + "</td></tr>";
  html += "</table>";
  html += "<p>Relaisstatus: <b>" + String(relayState ? "AN" : "AUS") + "</b></p>";
  html += "<p>Betriebsart: <b>" + String(autoMode ? "Automatik" : "Hand (MQTT)") + "</b></p>";
  if (!sensorsOk) html += "<p><b>Achtung: ungültige Sensorwerte!</b></p>";
  html += "<p>MQTT: " + String(client.connected() ? "verbunden" : "getrennt") + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ==== MQTT Steuerung ====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  String t(topic);

  if (t == TOPIC_RELAY_SET) {
    // Manuelles Schalten -> Handbetrieb, damit die Automatik den Befehl nicht sofort überschreibt
    if (message == "ON" || message == "OFF") {
      autoMode = false;
      setRelay(message == "ON", "MQTT, Handbetrieb");
      publishState();
    }
  } else if (t == TOPIC_AUTO_SET) {
    if (message == "ON") {
      autoMode = true;
      Serial.println("Automatik über MQTT aktiviert");
    } else if (message == "OFF") {
      autoMode = false;
      Serial.println("Handbetrieb über MQTT aktiviert");
    }
    publishState();
  }
}

// Nicht blockierend: höchstens ein Verbindungsversuch alle MQTT_RETRY_INTERVAL ms
void maintainMQTT(unsigned long now) {
  if (client.connected()) {
    client.loop();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL) return;
  lastMqttAttempt = now;

  Serial.print("Verbinde mit MQTT...");
  if (client.connect(mqtt_client_id, mqtt_user, mqtt_pass, TOPIC_AVAILABILITY, 0, true, "offline")) {
    Serial.println("verbunden!");
    client.publish(TOPIC_AVAILABILITY, "online", true);
    client.subscribe(TOPIC_RELAY_SET);
    client.subscribe(TOPIC_AUTO_SET);
    if (HA_DISCOVERY) publishDiscovery();
    publishState();
    publishMeasurements();
  } else {
    Serial.print("fehlgeschlagen, rc=");
    Serial.print(client.state());
    Serial.println(" – neuer Versuch später");
  }
}

// ==== Sensoren ====
void reinitSensor(Adafruit_BME280& bme, TwoWire& bus, const char* name) {
  Serial.printf("BME280 %s nicht erreichbar – versuche Re-Init...\n", name);
  if (bme.begin(0x76, &bus)) {
    Serial.printf("BME280 %s wieder verbunden!\n", name);
  } else {
    Serial.printf("BME280 %s immer noch nicht gefunden.\n", name);
  }
}

void readSensors() {
  tempIn = bme1.readTemperature();
  humIn  = bme1.readHumidity();
  if (isnan(tempIn)) reinitSensor(bme1, I2CBus1, "#1 (Innen)");
  ahIn   = absoluteHumidity(tempIn, humIn);

  tempOut = bme2.readTemperature();
  humOut  = bme2.readHumidity();
  if (isnan(tempOut)) reinitSensor(bme2, I2CBus2, "#2 (Aussen)");
  ahOut   = absoluteHumidity(tempOut, humOut);

  sensorsOk = sensorsValid();
  ahDiff = ahIn - ahOut; // > 0: drinnen feuchter
}

// ==== Regelung ====
void regulate(unsigned long now) {
  if (!autoMode) return; // Handbetrieb: Relais bleibt wie per MQTT gesetzt

  // Failsafe / Ungültige Werte -> Relais AUS, keine Regelung
  if (!sensorsOk) {
    setRelay(false, "Failsafe: ungültige Sensorwerte");
    return;
  }

  bool condOn   = (ahDiff > AH_ON_MARGIN)  && (tempOut > MIN_OUT_TEMP);
  bool condKeep = (ahDiff > AH_OFF_MARGIN) && (tempOut > MIN_OUT_TEMP);
  unsigned long timeSinceLastSwitch = now - lastSwitchTime;

  if (!relayState) {
    if (condOn && timeSinceLastSwitch >= MIN_OFF_TIME) setRelay(true, "Automatik");
  } else {
    if (!condKeep && timeSinceLastSwitch >= MIN_ON_TIME) setRelay(false, "Automatik");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  // WATCHDOG
  setupWatchdog();
  Serial.println("Watchdog aktiv");

  Serial.println("Kellerlueftung mit 2x BME280 (ESP32, absolute Feuchte)");

  I2CBus1.begin(SDA_1, SCL_1, 100000);
  I2CBus2.begin(SDA_2, SCL_2, 100000);

  if (!bme1.begin(0x76, &I2CBus1)) {
    Serial.println("BME280 #1 (Innen) nicht gefunden!");
  }
  if (!bme2.begin(0x76, &I2CBus2)) {
    Serial.println("BME280 #2 (Aussen) nicht gefunden!");
  }

  // WLAN mit Heimnetz verbinden – mit Timeout, die Regelung läuft auch ohne WLAN
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  Serial.print("Verbinde mit WLAN ");
  Serial.println(ssid);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_CONNECT_TIMEOUT) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WLAN verbunden!");
    Serial.print("IP-Adresse: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WLAN noch nicht verbunden – Regelung startet trotzdem, Verbindung wird im Hintergrund aufgebaut");
  }

  // MQTT konfigurieren
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(512);   // für Home-Assistant-Discovery-Nachrichten
  client.setSocketTimeout(5);  // s – blockiert bei nicht erreichbarem Broker nicht zu lange

  // Webserver starten
  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  // WATCHDOG
  esp_task_wdt_reset(); // Watchdog regelmäßig füttern

  unsigned long now = millis();

  server.handleClient();
  maintainMQTT(now);

  if (firstMeasure || now - lastMeasureTime >= MEASURE_INTERVAL) {
    firstMeasure = false;
    lastMeasureTime = now;

    readSensors();
    regulate(now);

    // Debug-Ausgabe
    Serial.printf("TempIn:  %.2f °C, HumIn:  %.2f %%, AH_in:  %.2f g/m³\n", tempIn, humIn, ahIn);
    Serial.printf("TempOut: %.2f °C, HumOut: %.2f %%, AH_out: %.2f g/m³\n", tempOut, humOut, ahOut);
    Serial.printf("Relais: %s (seit %lu s), %s, Diff: %.2f%s\n",
                  relayState ? "AN" : "AUS", (now - lastSwitchTime) / 1000,
                  autoMode ? "Automatik" : "Handbetrieb", ahDiff,
                  sensorsOk ? "" : " – SENSORWERTE UNGÜLTIG");

    // MQTT Messwerte senden
    publishMeasurements();
  }

  delay(10); // kurz abgeben, Webserver bleibt reaktionsfähig
}
