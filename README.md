# kellerentfeuchter
Lüftungssteuerung um abs. trockenere Luft auszutauschen

ESP32 mit zwei BME280 (innen/außen, je eigener I²C-Bus) schaltet über ein Relais einen Lüfter,
wenn die Außenluft absolut trockener ist als die Kellerluft. Werte auf OLED-Display (SSD1306)
und als CSV auf SD-Karte.

## Dateien
| Pfad | Inhalt |
|---|---|
| `esp32_taupunkt_lokal_v5/` | **Aktueller Sketch** (lokal, ohne WLAN) – in der Arduino-IDE diesen Ordner öffnen |
| `esp32_taupunkt_lokal_v5/regelung.h` | Regellogik (vom Sketch und von den Tests genutzt) |
| `tests/` | Tests der Regelung auf dem PC |
| `esp32_taupunkt_mqtt_v2.ini` | Alte Version mit WLAN/MQTT/Webserver (Archiv) |

Bibliotheken: Adafruit BME280, Adafruit Unified Sensor, Adafruit SSD1306, Adafruit GFX.
Unterstützt ESP32-Arduino-Core 2.0.x und 3.x (CI kompiliert gegen beide).

## Verdrahtung
| Funktion | GPIO |
|---|---|
| BME280 innen + OLED (I²C-Bus 1) | SDA 21, SCL 22 |
| BME280 außen (I²C-Bus 2) | SDA 17, SCL 16 |
| SD-Karte (VSPI) | CS 5, SCK 18, MISO 19, MOSI 23 |
| Relais (LOW = an) | 14 |

## Regelung
- **EIN**: absolute Feuchte innen > außen + 0,5 g/m³ **und** außen > 5 °C (mind. 10 s aus gewesen)
- **AUS**: Vorteil ≤ 0,1 g/m³ **oder** außen ≤ 4,5 °C (mind. 2 min an gewesen)
- Ungültige Sensorwerte → Relais sofort AUS (Failsafe), keine Regelung
- Parameter in `regelung.h` (`struct Parameter`)

## SD-Log (`/keller_log_3.csv`)
Spalten: `Boot;Millis;TempIn;HumIn;AH_in;TempOut;HumOut;AH_out;ahDiff;Relais;Ereignis`
- `Boot` zählt jeden Neustart hoch (NVS), `Millis` beginnt pro Boot bei 0
- `Ereignis`: `BOOT (<Grund>)` z. B. Watchdog/Unterspannung, `EIN`, `AUS`, `FAILSAFE`
- Ungültige Werte als leeres Feld; Dezimalkomma für Excel
- Schreibcache wird jede Minute und bei jedem Schaltvorgang geleert
- SD-Ausfall wird erkannt, Neuversuch jede Minute

## Tests
```sh
tests/run_tests.sh                          # Unit-Tests + 1-Jahres-Simulation (~3 s)
tests/run_tests.sh --replay keller_log_3.csv  # echtes SD-Log durch die Regelung schicken
```
Die Tests prüfen Schwellen, Hysterese, Mindestlaufzeiten, Failsafe, `millis()`-Überlauf und in einer
simulierten Jahreslaufzeit (mit Sensorrauschen und -ausfällen), dass der Lüfter nie unter falschen
Bedingungen einschaltet und nicht taktet. Der Replay-Modus wertet ein SD-Log aus (Schaltvorgänge,
Laufzeit, Neustarts) und vergleicht den geloggten Relaisstatus mit der aktuellen Regelung – nützlich,
um Parameteränderungen an echten Daten zu prüfen. Die Tests laufen bei jedem Push per GitHub Actions,
zusätzlich wird der Sketch für ESP32-Core 2.x und 3.x kompiliert.
