#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <esp_task_wdt.h> // Watchdog
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SD.h>
#include <stdarg.h>   // für printf-Style

// TODO: Logfile size > 4gb fehlerhaft (500KB/24h = 185MB/Jahr)


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

// ==== TwoWire Instanzen ====
TwoWire I2CBus1 = TwoWire(0);
TwoWire I2CBus2 = TwoWire(1);

// ==== BME280-Objekte ====
Adafruit_BME280 bme1; // Innen
Adafruit_BME280 bme2; // Außen

// Display-Parameter // 3,3V SDA21, SDL22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1  // kein Reset-Pin
bool displayOK = false;
// WICHTIG: nutzen den bestehenden Bus I2CBus1
extern TwoWire I2CBus1;  
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2CBus1, OLED_RESET);

// --- Log-Datei & Status ---
File logFile;
bool sdOK = false;
String log_name = "/keller_log_2.csv";  // Name der Logdatei


// ==== Steuerparameter ====
const float AH_ON_MARGIN  = 0.5; // g/m³ – draußen so viel trockener, um einzuschalten
const float AH_OFF_MARGIN =  0.1; // g/m³ – unter diesen Vorteil wieder aus
//const float AH_MARGIN      = 0.5;         // g/m³ old without hysterese
const float MIN_OUT_TEMP   = 5.0;         // °C -----evtl. hochsetzen
const unsigned long MIN_ON_TIME  = 120000; // ms 
const unsigned long MIN_OFF_TIME = 10000; // ms

unsigned long lastSwitchTime = 0;
bool relayState = false;

// ==== Globale Messwerte ====
float tempIn = 0, humIn = 0, ahIn = 0;
float tempOut = 0, humOut = 0, ahOut = 0;

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

String numToCSV(float val) {
  String s = String(val, 2);
  s.replace(".", ",");
  return "\"" + s + "\"";
}

void initSD() {
  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed");
    sdOK = false;
    return;
  }
  sdOK = true;


  // Prüfen, ob Datei schon existiert
  bool newFile = !SD.exists(log_name);
  // Datei öffnen / anlegen (CSV-kompatibel)
  logFile = SD.open(log_name, FILE_APPEND);
  if (!logFile) {
    Serial.println("SD open failed");
    sdOK = false;
    return;
  }

  // Optional: Header einmalig (nur wenn neue Datei)
  if (newFile) {
    Serial.println("Neue Logdatei angelegt:");
    logFile.println("\"Millis\";\"TempIn\";\"HumIn\";\"AH_in\";\"TempOut\";\"HumOut\";\"AH_out\";\"ahDiff\";\"Relais\"");
    logFile.flush();
  }

}

// printf-ähnliche Logging-Funktion -> Serial + SD (wenn vorhanden)
void logf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // Konsole
  // Serial.print(buf);

  // SD
  if (sdOK) {
    logFile.print(millis());
    logFile.print(';');
    logFile.println(buf);
    // Schreibcache regelmäßig leeren -> geringeres Korruptionsrisiko
    logFile.flush();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  // WATCHDOG
  Serial.println("Watchdog aktiv");
  esp_task_wdt_init(60, true); // initialisieren mit Reset bei Timeout (30s)
  esp_task_wdt_add(NULL);               // aktuelle Task (loop) überwachen

  initSD();
  //logf("Boot OK, Version %s\n", "1.0.0");


  Serial.println("Kellerlueftung mit 2x BME280 (ESP32, absolute Feuchte)");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  I2CBus1.begin(SDA_1, SCL_1, 100000);
  I2CBus2.begin(SDA_2, SCL_2, 100000);

  if (!bme1.begin(0x76, &I2CBus1)) {
    Serial.println("BME280 #1 (Innen) nicht gefunden!");
  }
  if (!bme2.begin(0x76, &I2CBus2)) {
    Serial.println("BME280 #2 (Aussen) nicht gefunden!");
  }

  Serial.println("Zeit\tT_in\tRH_in\tAH_in\tT_out\tRH_out\tAH_out\tRelais");

  // Display starten (nicht blockieren, bei Fehler einfach deaktivieren)
  displayOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (displayOK) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.println("Kellerlueftung ESP32");
    display.display(); // kurzer, einmaliger Frame
  } else {
    Serial.println("SSD1306 nicht gefunden – Display deaktiviert.");
  }

}


void loop() {
  // WATCHDOG
  esp_task_wdt_reset(); // Watchdog regelmäßig füttern

  // lastswitchtime (debugging)
  unsigned long now = millis();
  unsigned long timeSinceLastSwitch = now - lastSwitchTime;


  tempIn = bme1.readTemperature();
  humIn  = bme1.readHumidity();
  ahIn   = absoluteHumidity(tempIn, humIn);

  tempOut = bme2.readTemperature();
  humOut  = bme2.readHumidity();
  ahOut   = absoluteHumidity(tempOut, humOut);


  // Sensor check Nan and re-init
  if (isnan(bme1.readTemperature())) {
  Serial.println("BME280 #1 nicht erreichbar – versuche Re-Init...");
  if (bme1.begin(0x76, &I2CBus1)) {
    Serial.println("BME280 #1 wieder verbunden!");
  } else {
    Serial.println("BME280 #1 immer noch nicht gefunden.");
  }
  }
  if (isnan(bme2.readTemperature())) {
  Serial.println("BME280 #2 nicht erreichbar – versuche Re-Init...");
  if (bme2.begin(0x76, &I2CBus2)) {
    Serial.println("BME280 #2 wieder verbunden!");
  } else {
    Serial.println("BME280 #2 immer noch nicht gefunden.");
  }
  }

  // Failsafe / Ungültige Werte ->  Relais AUS
  bool ok = sensorsValid();
  if (!ok && relayState){
    relayState=false; 
    digitalWrite(RELAY_PIN, HIGH);
    lastSwitchTime = now;
    Serial.println("Failsafe: ungültige Sensorwerte -> Relais AUS");
  }

  // Steuerung (canSwitchon = kann switch an sein)
  //bool canSwitchOn = (ahOut + AH_MARGIN < ahIn) && (tempOut > MIN_OUT_TEMP); //old without hysterese
  float ahDiff = ahIn - ahOut; // > 0: drinnen feuchter
  bool condOn   = (ahDiff > AH_ON_MARGIN)  && (tempOut > MIN_OUT_TEMP);
  bool condKeep = (ahDiff > AH_OFF_MARGIN) && (tempOut > MIN_OUT_TEMP);

  if (!relayState) {
    if (condOn && timeSinceLastSwitch >= MIN_OFF_TIME) {
      relayState = true;
      digitalWrite(RELAY_PIN, LOW);
      lastSwitchTime = now;
      Serial.println("Relais EIN");
    }
  } else {
    if (!condKeep && timeSinceLastSwitch >= MIN_ON_TIME) {
      relayState = false;
      digitalWrite(RELAY_PIN, HIGH);
      lastSwitchTime = now;
      Serial.println("Relais AUS");
    }
  }

  // Debug-Ausgabe
  Serial.printf("TempIn:  %.2f °C, HumIn:  %.2f %%, AH_in:  %.2f g/m³\n", tempIn, humIn, ahIn);
  Serial.printf("TempOut: %.2f °C, HumOut: %.2f %%, AH_out: %.2f g/m³\n", tempOut, humOut, ahOut);
  Serial.printf("Relais: %s (seit %lu s) mit Diff: %.2f\n", relayState ? "AN" : "AUS", timeSinceLastSwitch / 1000, ahDiff);
  //Serial.printf("ValidinT: %d, ValidinRH: %d, ValidinAH: %d\n", validT(tempIn), validRH(humIn), validAH(ahIn)); // only debugging
  //Serial.printf("ValidoutT: %d, ValidoutRH: %d, ValidoutAH: %d\n", validT(tempOut), validRH(humOut), validAH(ahOut)); // only debugging


  displayOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (displayOK) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.printf("TempIn:   %.1f C\n", tempIn);
    display.printf("HumIn:    %.1f %%\n", humIn);
    //display.printf("AH_in:    %.1f g/m3\n", ahIn);
    display.printf("TempOut:  %.1f C\n", tempOut);
    display.printf("HumOut:   %.1f %%\n", humOut);
    //display.printf("AH_out:   %.1f g/m3\n", ahOut);
    display.printf("AH_diff:  %.2f g/m3\n", ahDiff);
    display.printf("Relais:   %s\n", relayState ? "AN" : "AUS");
    display.display();
  } else {
    Serial.println("SSD1306 nicht gefunden – Display deaktiviert.");
  }

  // Loggging to SD Card
  //logf("TempIn:  %.2f °C, HumIn:  %.2f %%, AH_in:  %.2f g/m³, TempOut: %.2f °C, HumOut: %.2f %%, AH_out: %.2f g/m³, Relais: %s\n", tempIn, humIn, ahIn, tempOut, humOut, ahOut, relayState ? "AN" : "AUS");

// Log schreiben
if (logFile) {
  logFile.print(millis()); logFile.print(';');
  logFile.print(numToCSV(tempIn));logFile.print(';');
  logFile.print(numToCSV(humIn));logFile.print(';');
  logFile.print(numToCSV(ahIn));logFile.print(';');
  logFile.print(numToCSV(tempOut));logFile.print(';');
  logFile.print(numToCSV(humOut));logFile.print(';');
  logFile.print(numToCSV(ahOut));logFile.print(';');
  logFile.print(numToCSV(ahDiff));logFile.print(';');
  logFile.print(relayState ? "1" : "0");logFile.print(';');
  logFile.println(); // <--- neue Zeile für nächsten Datensatz
  logFile.flush();
}

  

  delay(5000); // TODO: Change for productive
}
