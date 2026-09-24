# kellerentfeuchter
Lüftungssteuerung um abs. trockenere Luft auszutauschen

ESP32 mit zwei BME280 (innen/außen, je eigener I²C-Bus) schaltet über ein Relais einen Lüfter,
wenn die Außenluft absolut trockener ist als die Kellerluft.

Sketch: `esp32_taupunkt_mqtt_v2/esp32_taupunkt_mqtt_v2.ino` (Arduino-IDE, ESP32-Core 2.x oder 3.x).
Benötigte Bibliotheken: Adafruit BME280, Adafruit Unified Sensor, PubSubClient.

## Regelung
- EIN: absolute Feuchte innen > außen + 0,5 g/m³ und außen > 5 °C (mind. 10 s aus gewesen)
- AUS: Vorteil ≤ 0,1 g/m³ oder außen ≤ 5 °C (mind. 2 min an gewesen)
- Ungültige Sensorwerte → Relais AUS (Failsafe, nur im Automatikbetrieb)
- Regelung läuft auch ohne WLAN/MQTT weiter

## MQTT-Topics
| Topic | Richtung | Inhalt |
|---|---|---|
| `kellerlueftung/{temp,hum,ah}_{in,out}` | ESP → Broker | Messwerte (retained, nur gültige Werte) |
| `kellerlueftung/relais/status` | ESP → Broker | `ON` / `OFF` |
| `kellerlueftung/relais/set` | Broker → ESP | `ON` / `OFF` – schaltet in den Handbetrieb |
| `kellerlueftung/auto/status` | ESP → Broker | `ON` = Automatik, `OFF` = Handbetrieb |
| `kellerlueftung/auto/set` | Broker → ESP | `ON` = zurück in Automatik |
| `kellerlueftung/status` | ESP → Broker | `online` / `offline` (Last Will) |

Home-Assistant-Autodiscovery lässt sich im Sketch mit `HA_DISCOVERY = true` aktivieren
(aus lassen, wenn die Entitäten schon per YAML angelegt sind).
