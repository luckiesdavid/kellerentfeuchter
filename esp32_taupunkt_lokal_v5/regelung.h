// Regelung der Kellerlüftung – ohne Arduino-Abhängigkeiten, damit dieselbe Logik
// im Sketch und in den Tests (tests/test_regelung.cpp) läuft.
#pragma once

#include <math.h>
#include <stdint.h>

namespace regelung {

// ==== Steuerparameter ====
struct Parameter {
  float    ahOnMargin  = 0.5f;    // g/m³ – draußen so viel trockener, um einzuschalten
  float    ahOffMargin = 0.1f;    // g/m³ – unter diesen Vorteil wieder aus
  float    minOutTemp  = 5.0f;    // °C   – außen wärmer als das, um einzuschalten (evtl. hochsetzen)
  float    tempHyst    = 0.5f;    // °C   – bleibt an bis minOutTemp - tempHyst (verhindert Takten um 5 °C)
  uint32_t minOnTime   = 120000;  // ms
  uint32_t minOffTime  = 10000;   // ms
};

struct Messung {
  float tempIn, humIn;   // Innen
  float tempOut, humOut; // Außen
};

struct Auswertung {
  float tempIn, humIn, ahIn;
  float tempOut, humOut, ahOut;
  float ahDiff;  // > 0: drinnen feuchter
  bool  gueltig; // alle Werte plausibel
};

enum class Ereignis { KEINS, EIN, AUS, FAILSAFE };

inline const char* ereignisText(Ereignis e) {
  switch (e) {
    case Ereignis::EIN:      return "EIN";
    case Ereignis::AUS:      return "AUS";
    case Ereignis::FAILSAFE: return "FAILSAFE";
    default:                 return "";
  }
}

// ===== Hilfsfunktionen =====
inline float absoluteHumidity(float tempC, float rhPercent) {
  float e_s = 6.112f * expf((17.62f * tempC) / (243.12f + tempC));
  float e   = (rhPercent / 100.0f) * e_s;
  float ah  = 216.7f * e / (tempC + 273.15f);
  return ah;
}
inline bool validT(float v)  { return !isnan(v) && v > -30.0f && v < 70.0f; }
inline bool validRH(float v) { return !isnan(v) && v >= 0.0f && v <= 100.0f; }
inline bool validAH(float v) { return !isnan(v) && v >= 0.0f && v <= 50.0f; }

inline Auswertung auswerten(const Messung& m) {
  Auswertung a;
  a.tempIn  = m.tempIn;  a.humIn  = m.humIn;  a.ahIn  = absoluteHumidity(m.tempIn,  m.humIn);
  a.tempOut = m.tempOut; a.humOut = m.humOut; a.ahOut = absoluteHumidity(m.tempOut, m.humOut);
  a.ahDiff  = a.ahIn - a.ahOut;
  a.gueltig = validT(a.tempIn)  && validRH(a.humIn)  && validAH(a.ahIn)
           && validT(a.tempOut) && validRH(a.humOut) && validAH(a.ahOut);
  return a;
}

// ==== Regler mit Hysterese und Mindestlaufzeiten ====
// Zeiten als uint32_t wie millis() auf dem ESP32 – Überlauf nach ~49,7 Tagen
// wird durch die vorzeichenlose Differenz korrekt behandelt.
class Regler {
public:
  explicit Regler(const Parameter& p = Parameter()) : p_(p) {}

  Ereignis update(const Auswertung& a, uint32_t now) {
    // Failsafe / ungültige Werte -> Relais sofort AUS, keine Regelung
    if (!a.gueltig) {
      if (relayOn_) { schalte(false, now); return Ereignis::FAILSAFE; }
      return Ereignis::KEINS;
    }

    bool condOn   = (a.ahDiff > p_.ahOnMargin)  && (a.tempOut > p_.minOutTemp);
    bool condKeep = (a.ahDiff > p_.ahOffMargin) && (a.tempOut > p_.minOutTemp - p_.tempHyst);
    uint32_t seit = now - lastSwitch_;

    if (!relayOn_) {
      if (condOn && seit >= p_.minOffTime) { schalte(true, now); return Ereignis::EIN; }
    } else {
      if (!condKeep && seit >= p_.minOnTime) { schalte(false, now); return Ereignis::AUS; }
    }
    return Ereignis::KEINS;
  }

  bool     relaisAn() const { return relayOn_; }
  uint32_t seitLetztemSchalten(uint32_t now) const { return now - lastSwitch_; }
  const Parameter& parameter() const { return p_; }

private:
  void schalte(bool an, uint32_t now) { relayOn_ = an; lastSwitch_ = now; }

  Parameter p_;
  bool      relayOn_    = false;
  uint32_t  lastSwitch_ = 0;
};

} // namespace regelung
