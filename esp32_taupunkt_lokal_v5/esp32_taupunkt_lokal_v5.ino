#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <esp_task_wdt.h> // Watchdog
#include <esp_system.h>   // esp_reset_reason()
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>  // Boot-Zähler im NVS
#include "regelung.h"     // Regelung (auch von tests/ genutzt)

using namespace regelung;

// Logfile-Größe: ~70 Byte/Zeile, bei 5 s Takt ~1,2 MB/Tag -> 4-GB-Grenze (FAT32) erst nach ~9 Jahren


#define SD_CS   5    // Chip Select
// VSPI SCK=18, MISO=19, MOSI=23, VCC=5V

// ==== I²C-Bus 1 (Standard-Pins ESP32) ====
#define SDA_1 21
#define SCL_1 22

// ==== I²C-Bus 2 ====
#define SDA_2 17
#define SCL_2 16

// ==== Relais ====
#define RELAY_PIN 14 // GPIO für Relaismodul (LOW = an, HIGH = aus)

// ==== Zeitsteuerung ====
const unsigned long MESS_INTERVALL   = 5000;  // ms – TODO: für Produktivbetrieb ggf. erhöhen
const unsigned long SD_FLUSH_INTERVALL = 60000; // ms – seltener flushen schont die SD-Karte
const unsigned long SD_RETRY_INTERVALL = 60000; // ms – Abstand für SD-Neuinitialisierung
const uint32_t      WDT_TIMEOUT_S = 60;         // s  – Watchdog-Timeout

// ==== TwoWire Instanzen ====
TwoWire I2CBus1 = TwoWire(0);
TwoWire I2CBus2 = TwoWire(1);

// ==== BME280-Objekte ====
Adafruit_BME280 bme1; // Innen
Adafruit_BME280 bme2; // Außen

// Display-Parameter // 3,3V SDA21, SDL22 – teilt sich I2CBus1 mit BME280 #1
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1  // kein Reset-Pin
#define OLED_ADDR    0x3C
bool displayOK = false;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2CBus1, OLED_RESET);

// --- Log-Datei & Status ---
// Neuer Dateiname, weil sich das Spaltenformat gegenüber keller_log_2.csv geändert hat
const char* LOG_NAME = "/keller_log_3.csv";
File logFile;
bool sdOK = false;
unsigned long lastSdFlush = 0;
unsigned long lastSdRetry = 0;
uint32_t bootCount = 0;

// ==== Regelung ====
Regler regler;           // Parameter siehe regelung.h
Auswertung werte;

// ===== Hilfsfunktionen =====
bool i2cPresent(TwoWire& bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

void setRelayPin(bool an) {
  digitalWrite(RELAY_PIN, an ? LOW : HIGH);
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

const char* resetGrund() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Einschalten";
    case ESP_RST_SW:       return "Software";
    case ESP_RST_PANIC:    return "Absturz";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "Watchdog";
    case ESP_RST_BROWNOUT: return "Unterspannung";
    case ESP_RST_EXT:      return "Reset-Taste";
    default:               return "Sonstiges";
  }
}

uint32_t incrementBootCount() {
  Preferences prefs;
  prefs.begin("keller", false);
  uint32_t n = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", n);
  prefs.end();
  return n;
}

// ==== SD-Logging ====
// Ungültige Werte (NaN) als leeres Feld, damit Excel sauber rechnet
String numToCSV(float val) {
  if (isnan(val)) return "";
  String s = String(val, 2);
  s.replace(".", ",");
  return "\"" + s + "\"";
}

bool initSD() {
  if (logFile) logFile.close();
  SD.end();
  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed");
    return sdOK = false;
  }

  // Prüfen, ob Datei schon existiert
  bool newFile = !SD.exists(LOG_NAME);
  // Datei öffnen / anlegen (CSV-kompatibel)
  logFile = SD.open(LOG_NAME, FILE_APPEND);
  if (!logFile) {
    Serial.println("SD open failed");
    return sdOK = false;
  }

  // Header einmalig (nur wenn neue Datei)
  if (newFile) {
    Serial.println("Neue Logdatei angelegt");
    logFile.println("\"Boot\";\"Millis\";\"TempIn\";\"HumIn\";\"AH_in\";\"TempOut\";\"HumOut\";\"AH_out\";\"ahDiff\";\"Relais\";\"Ereignis\"");
    logFile.flush();
  }
  lastSdFlush = millis();
  return sdOK = true;
}

// Schreibt eine Zeile; Fehler -> SD als ausgefallen markieren, wird später neu initialisiert
void logZeile(const Auswertung* a, bool relais, const String& ereignis, bool sofortFlush) {
  if (!sdOK) return;
  String z = String(bootCount) + ";" + String(millis()) + ";";
  if (a) {
    z += numToCSV(a->tempIn)  + ";" + numToCSV(a->humIn)  + ";" + numToCSV(a->ahIn)  + ";";
    z += numToCSV(a->tempOut) + ";" + numToCSV(a->humOut) + ";" + numToCSV(a->ahOut) + ";";
    z += numToCSV(a->ahDiff)  + ";";
  } else {
    z += ";;;;;;;";
  }
  z += String(relais ? "1" : "0") + ";";
  if (ereignis.length()) z += "\"" + ereignis + "\"";

  if (logFile.println(z) == 0) {
    Serial.println("SD-Schreibfehler – Logging pausiert, neuer Versuch später");
    logFile.close();
    sdOK = false;
    lastSdRetry = millis();
    return;
  }
  if (sofortFlush || millis() - lastSdFlush >= SD_FLUSH_INTERVALL) {
    logFile.flush();
    lastSdFlush = millis();
  }
}

// ==== Display ====
bool initDisplay() {
  // begin() prüft die Verbindung nicht selbst -> vorher per I²C nachsehen.
  // periphBegin=false: I2CBus1 ist bereits mit den richtigen Pins gestartet.
  if (!i2cPresent(I2CBus1, OLED_ADDR) || !display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    return false;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  return true;
}

void updateDisplay(const Auswertung& a, bool relais) {
  // Display erst (neu) initialisieren, wenn es (wieder) antwortet
  if (!i2cPresent(I2CBus1, OLED_ADDR)) {
    if (displayOK) Serial.println("SSD1306 nicht gefunden – Display deaktiviert.");
    displayOK = false;
    return;
  }
  if (!displayOK) {
    displayOK = initDisplay();
    if (!displayOK) return;
  }
  display.clearDisplay();
  display.setCursor(0,0);
  display.printf("TempIn:   %.1f C\n", a.tempIn);
  display.printf("HumIn:    %.1f %%\n", a.humIn);
  display.printf("TempOut:  %.1f C\n", a.tempOut);
  display.printf("HumOut:   %.1f %%\n", a.humOut);
  display.printf("AH_diff:  %.2f g/m3\n", a.ahDiff);
  display.printf("Relais:   %s\n", relais ? "AN" : "AUS");
  if (!a.gueltig)  display.println("!! SENSORFEHLER !!");
  else if (!sdOK)  display.println("SD: kein Logging");
  display.display();
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

Messung leseSensoren() {
  Messung m;
  m.tempIn = bme1.readTemperature();
  m.humIn  = bme1.readHumidity();
  if (isnan(m.tempIn)) reinitSensor(bme1, I2CBus1, "#1 (Innen)");

  m.tempOut = bme2.readTemperature();
  m.humOut  = bme2.readHumidity();
  if (isnan(m.tempOut)) reinitSensor(bme2, I2CBus2, "#2 (Aussen)");
  return m;
}

void setup() {
  // Relais als Erstes definiert AUS, damit es während des Bootens nicht anzieht
  pinMode(RELAY_PIN, OUTPUT);
  setRelayPin(false);

  Serial.begin(115200);
  delay(1000);

  // WATCHDOG
  setupWatchdog();
  Serial.println("Watchdog aktiv");

  bootCount = incrementBootCount();
  Serial.printf("Kellerlueftung mit 2x BME280 (ESP32, absolute Feuchte) – Boot #%lu, Grund: %s\n",
                (unsigned long)bootCount, resetGrund());

  I2CBus1.begin(SDA_1, SCL_1, 100000);
  I2CBus2.begin(SDA_2, SCL_2, 100000);

  if (!bme1.begin(0x76, &I2CBus1)) {
    Serial.println("BME280 #1 (Innen) nicht gefunden!");
  }
  if (!bme2.begin(0x76, &I2CBus2)) {
    Serial.println("BME280 #2 (Aussen) nicht gefunden!");
  }

  // Display starten (nicht blockieren, bei Fehler einfach deaktivieren)
  displayOK = initDisplay();
  if (displayOK) {
    display.setCursor(0,0);
    display.println("Kellerlueftung ESP32");
    display.printf("Boot #%lu\n", (unsigned long)bootCount);
    display.display(); // kurzer, einmaliger Frame
  } else {
    Serial.println("SSD1306 nicht gefunden – Display deaktiviert.");
  }

  initSD();
  lastSdRetry = millis();
  logZeile(nullptr, false, String("BOOT (") + resetGrund() + ")", true);
}


void loop() {
  // WATCHDOG
  esp_task_wdt_reset(); // Watchdog regelmäßig füttern

  uint32_t now = millis();

  werte = auswerten(leseSensoren());
  Ereignis ereignis = regler.update(werte, now);
  bool relais = regler.relaisAn();
  setRelayPin(relais);

  switch (ereignis) {
    case Ereignis::EIN:      Serial.println("Relais EIN"); break;
    case Ereignis::AUS:      Serial.println("Relais AUS"); break;
    case Ereignis::FAILSAFE: Serial.println("Failsafe: ungültige Sensorwerte -> Relais AUS"); break;
    default: break;
  }

  // Debug-Ausgabe
  Serial.printf("TempIn:  %.2f °C, HumIn:  %.2f %%, AH_in:  %.2f g/m³\n", werte.tempIn, werte.humIn, werte.ahIn);
  Serial.printf("TempOut: %.2f °C, HumOut: %.2f %%, AH_out: %.2f g/m³\n", werte.tempOut, werte.humOut, werte.ahOut);
  Serial.printf("Relais: %s (seit %lu s) mit Diff: %.2f%s\n", relais ? "AN" : "AUS",
                (unsigned long)(regler.seitLetztemSchalten(now) / 1000), werte.ahDiff,
                werte.gueltig ? "" : " – SENSORWERTE UNGÜLTIG");

  updateDisplay(werte, relais);

  // SD ausgefallen oder beim Start nicht da -> regelmäßig neu versuchen
  if (!sdOK && millis() - lastSdRetry >= SD_RETRY_INTERVALL) {
    lastSdRetry = millis();
    if (initSD()) logZeile(nullptr, relais, "SD wieder verfuegbar", true);
  }

  // Log schreiben – Schaltereignisse sofort auf die Karte
  logZeile(&werte, relais, ereignisText(ereignis), ereignis != Ereignis::KEINS);

  delay(MESS_INTERVALL);
}
