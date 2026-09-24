// Tests für die Lüftersteuerung (esp32_taupunkt_lokal_v5/regelung.h) – laufen auf dem PC.
//
//   tests/run_tests.sh                     alle Tests inkl. 1-Jahres-Simulation
//   tests/run_tests.sh --replay LOG.csv    SD-Logdatei durch die Regelung schicken und auswerten
//
// Die Regelung wird unverändert aus dem Sketch eingebunden – was hier grün ist,
// verhält sich auf dem ESP32 genauso.

#include "../esp32_taupunkt_lokal_v5/regelung.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace regelung;

static const double PI = 3.14159265358979323846;

static int fehler = 0;
static int pruefungen = 0;

#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++pruefungen;                                                     \
    if (!(cond)) {                                                    \
      ++fehler;                                                       \
      std::printf("  FEHLER %s:%d: %s – ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

static bool nahe(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Messung mit gewünschter Differenz der absoluten Feuchte erzeugen (innen 20 °C, außen tOut)
static Auswertung mitDiff(float diff, float tOut = 15.0f) {
  const float tIn = 20.0f, rhIn = 70.0f;
  float ahOutZiel = absoluteHumidity(tIn, rhIn) - diff;
  float rhOut = ahOutZiel / absoluteHumidity(tOut, 100.0f) * 100.0f;
  Auswertung a = auswerten(Messung{tIn, rhIn, tOut, rhOut});
  CHECK(a.gueltig, "Testdaten ungültig (diff %.1f, tOut %.1f -> rF außen %.0f %%)", diff, tOut, rhOut);
  return a;
}

// ---------------------------------------------------------------------------
static void testAbsoluteFeuchte() {
  std::printf("[Absolute Feuchte]\n");
  // Referenzwerte (Magnus-Formel / Tabellenwerte)
  CHECK(nahe(absoluteHumidity(20.0f, 50.0f), 8.63f, 0.05f), "%.3f", absoluteHumidity(20.0f, 50.0f));
  CHECK(nahe(absoluteHumidity(10.0f, 100.0f), 9.39f, 0.05f), "%.3f", absoluteHumidity(10.0f, 100.0f));
  CHECK(nahe(absoluteHumidity(0.0f, 100.0f), 4.85f, 0.05f), "%.3f", absoluteHumidity(0.0f, 100.0f));
  CHECK(nahe(absoluteHumidity(30.0f, 60.0f), 18.2f, 0.1f), "%.3f", absoluteHumidity(30.0f, 60.0f));
  CHECK(absoluteHumidity(15.0f, 0.0f) == 0.0f, "0 %% rF muss 0 g/m³ sein");
  // Hilfsfunktion für Tests liefert die gewünschte Differenz
  CHECK(nahe(mitDiff(0.7f).ahDiff, 0.7f, 0.001f), "%.4f", mitDiff(0.7f).ahDiff);
}

static void testPlausibilitaet() {
  std::printf("[Plausibilitätsprüfung]\n");
  CHECK(auswerten(Messung{20, 60, 10, 80}).gueltig, "normale Werte");
  CHECK(!auswerten(Messung{NAN, 60, 10, 80}).gueltig, "NaN innen");
  CHECK(!auswerten(Messung{20, 60, 10, NAN}).gueltig, "NaN außen");
  CHECK(!auswerten(Messung{-40, 60, 10, 80}).gueltig, "Temperatur zu niedrig");
  CHECK(!auswerten(Messung{20, 60, 75, 80}).gueltig, "Temperatur zu hoch");
  CHECK(!auswerten(Messung{20, 101, 10, 80}).gueltig, "rF > 100 %%");
  CHECK(!auswerten(Messung{20, 60, 10, -1}).gueltig, "rF < 0 %%");
}

static void testEinschalten() {
  std::printf("[Einschalten]\n");
  {
    Regler r;
    CHECK(r.update(mitDiff(1.0f), 5000) == Ereignis::KEINS, "vor MIN_OFF_TIME nach Start nicht einschalten");
    CHECK(r.update(mitDiff(1.0f), 10000) == Ereignis::EIN, "nach MIN_OFF_TIME einschalten");
    CHECK(r.relaisAn(), "Relais muss an sein");
  }
  {
    Regler r;
    CHECK(r.update(mitDiff(0.5f), 20000) == Ereignis::KEINS, "Diff genau 0,5 reicht nicht (> nicht >=)");
    CHECK(r.update(mitDiff(0.4f), 30000) == Ereignis::KEINS, "Diff 0,4 reicht nicht");
    CHECK(r.update(mitDiff(-2.0f, 20.0f), 40000) == Ereignis::KEINS, "draußen feuchter -> aus");
    CHECK(r.update(mitDiff(0.52f), 50000) == Ereignis::EIN, "Diff 0,52 reicht");
  }
  {
    Regler r;
    CHECK(r.update(mitDiff(7.0f, 5.0f), 20000) == Ereignis::KEINS, "außen genau 5 °C -> nicht einschalten");
    CHECK(r.update(mitDiff(7.0f, 2.0f), 30000) == Ereignis::KEINS, "außen 2 °C -> nicht einschalten");
    CHECK(r.update(mitDiff(7.0f, 5.1f), 40000) == Ereignis::EIN, "außen 5,1 °C -> einschalten");
  }
}

static void testAusschaltenUndHysterese() {
  std::printf("[Ausschalten / Hysterese / Mindestlaufzeit]\n");
  Regler r;
  CHECK(r.update(mitDiff(1.0f), 100000) == Ereignis::EIN, "einschalten");
  // Hysterese: zwischen 0,1 und 0,5 bleibt der Lüfter an
  CHECK(r.update(mitDiff(0.3f), 400000) == Ereignis::KEINS && r.relaisAn(), "Diff 0,3 -> bleibt an");
  CHECK(r.update(mitDiff(0.11f), 500000) == Ereignis::KEINS && r.relaisAn(), "Diff 0,11 -> bleibt an");
  // Mindestlaufzeit 120 s
  Regler r2;
  r2.update(mitDiff(1.0f), 100000);
  CHECK(r2.update(mitDiff(0.0f), 100000 + 119999) == Ereignis::KEINS && r2.relaisAn(), "vor MIN_ON_TIME nicht aus");
  CHECK(r2.update(mitDiff(0.0f), 100000 + 120000) == Ereignis::AUS && !r2.relaisAn(), "nach MIN_ON_TIME aus");
  // Mindestpause 10 s
  CHECK(r2.update(mitDiff(1.0f), 220000 + 9999) == Ereignis::KEINS, "vor MIN_OFF_TIME nicht wieder an");
  CHECK(r2.update(mitDiff(1.0f), 220000 + 10000) == Ereignis::EIN, "nach MIN_OFF_TIME wieder an");
  // Temperatur-Hysterese: zwischen 4,5 und 5 °C bleibt er an
  CHECK(r2.update(mitDiff(7.0f, 4.6f), 230000 + 130000) == Ereignis::KEINS && r2.relaisAn(), "4,6 °C -> bleibt an");
  // Kälte schaltet aus (nach Mindestlaufzeit)
  Regler r3;
  r3.update(mitDiff(1.0f), 100000);
  CHECK(r3.update(mitDiff(7.0f, 4.0f), 100000 + 60000) == Ereignis::KEINS, "Kälte, aber Mindestlaufzeit läuft");
  CHECK(r3.update(mitDiff(7.0f, 4.5f), 100000 + 120000) == Ereignis::AUS, "genau 4,5 °C -> aus");
}

static void testFailsafe() {
  std::printf("[Failsafe]\n");
  Regler r;
  r.update(mitDiff(1.0f), 50000);
  CHECK(r.relaisAn(), "an");
  Auswertung defekt = auswerten(Messung{NAN, NAN, 10, 80});
  CHECK(r.update(defekt, 51000) == Ereignis::FAILSAFE && !r.relaisAn(), "sofort aus, auch vor MIN_ON_TIME");
  CHECK(r.update(defekt, 52000) == Ereignis::KEINS, "kein zweites Failsafe-Ereignis");
  // Ungültige, aber nicht-NaN Werte dürfen nie einschalten (Fehler aus v5-Original)
  Auswertung unplausibel = auswerten(Messung{20, 100.5f, 10, 20});
  CHECK(unplausibel.ahDiff > 0.5f, "Testvoraussetzung: große Differenz");
  CHECK(r.update(unplausibel, 200000) == Ereignis::KEINS && !r.relaisAn(), "unplausible Werte -> bleibt aus");
  // Nach Failsafe wieder normal
  CHECK(r.update(mitDiff(1.0f), 210000) == Ereignis::EIN, "nach gültigen Werten wieder an");
}

static void testMillisUeberlauf() {
  std::printf("[millis()-Überlauf nach 49,7 Tagen]\n");
  Regler r;
  const uint32_t kurzVorUeberlauf = 0xFFFFFFFFu - 30000u;
  CHECK(r.update(mitDiff(1.0f), kurzVorUeberlauf) == Ereignis::EIN, "einschalten kurz vor Überlauf");
  CHECK(r.update(mitDiff(0.0f), kurzVorUeberlauf + 60000u) == Ereignis::KEINS, "nach Überlauf: erst 60 s an");
  CHECK(r.update(mitDiff(0.0f), kurzVorUeberlauf + 120000u) == Ereignis::AUS, "nach Überlauf: nach 120 s aus");
  CHECK(r.seitLetztemSchalten(kurzVorUeberlauf + 125000u) == 5000u, "Zeitdifferenz über Überlauf korrekt");
}

// ---------------------------------------------------------------------------
// Langzeitsimulation: ein Jahr synthetisches Wetter im 5-s-Takt mit Rauschen,
// Sensorausfällen und Ausreißern. Geprüft werden Regeln, die immer gelten müssen.
struct Statistik {
  long schaltungen = 0, failsafe = 0, maxProStunde = 0;  // maxProStunde: nur reguläre EIN/AUS
  long ausRegulaer = 0, kurzeLaeufe = 0;                 // kurz: Laufzeit < 3 min
  double laufzeitH = 0;
};

static Statistik simuliereJahr(bool pruefen, float rauschT, float rauschRH) {
  const uint32_t dt = 5000;                 // ms, wie MESS_INTERVALL
  const long schritte = 365L * 24 * 3600 / 5;
  std::mt19937 rng(42);                     // fester Seed -> reproduzierbar
  std::normal_distribution<float> rauschen(0.0f, 1.0f);  // skaliert mit rauschT / rauschRH
  std::uniform_real_distribution<float> zufall(0.0f, 1.0f);

  Regler r;
  Parameter p = r.parameter();
  Statistik s;
  uint32_t now = 1000, letzteSchaltung = 0, letzterFailsafe = 0;
  bool letzterZustand = false, ersterWechsel = true;
  long schaltungenStunde = 0;
  float ahInKeller = 11.0f;                 // Feuchteeintrag vs. Abtransport durch Lüften

  for (long i = 0; i < schritte; ++i, now += dt) {  // now läuft nach ~49,7 Tagen über – gewollt
    double tage = i * 5.0 / 86400.0;
    double saison = std::sin((tage - 110.0) / 365.0 * 2 * PI);   // Maximum im Sommer
    double tag    = std::sin((tage - std::floor(tage) - 0.375) * 2 * PI);
    float tOut  = float(9.0 + 10.0 * saison + 5.0 * tag) + rauschT * rauschen(rng);
    float rhOut = float(78.0 - 18.0 * tag) + rauschRH * rauschen(rng);
    if (rhOut > 100) rhOut = 100;
    float tIn = float(14.0 + 4.0 * saison) + rauschT * rauschen(rng);
    // Keller wird feuchter, Lüften trocknet ihn Richtung Außenluft
    float ahOut = absoluteHumidity(tOut, rhOut);
    ahInKeller += 0.0004f;
    if (r.relaisAn()) ahInKeller += (ahOut - ahInKeller) * 0.002f;
    float ahSatt = absoluteHumidity(tIn, 100.0f);
    if (ahInKeller > 0.95f * ahSatt) ahInKeller = 0.95f * ahSatt;  // kondensiert
    // BME280 (Adafruit-Lib) begrenzt die rF auf 0–100 %
    float rhIn = std::min(100.0f, std::max(0.0f, ahInKeller / ahSatt * 100.0f + rauschRH * rauschen(rng)));

    Messung m{tIn, rhIn, tOut, rhOut};
    float z = zufall(rng);
    if (z < 0.0005f)      m.tempIn = NAN;     // Sensorausfall innen
    else if (z < 0.001f)  m.humOut = NAN;     // Sensorausfall außen
    else if (z < 0.0012f) m.humIn = 120.0f;   // Ausreißer

    Auswertung a = auswerten(m);
    bool vorher = r.relaisAn();
    Ereignis e = r.update(a, now);
    bool nachher = r.relaisAn();

    if (e == Ereignis::FAILSAFE) { ++s.failsafe; letzterFailsafe = now; }
    if (e == Ereignis::AUS) {
      ++s.ausRegulaer;
      if (now - letzteSchaltung < 180000u) ++s.kurzeLaeufe;
    }
    // Wiedereinschalten kurz nach einem Failsafe zählt nicht als Takten der Regelung
    bool nachFailsafe = s.failsafe > 0 && now - letzterFailsafe <= 60000u;
    if (e == Ereignis::AUS || (e == Ereignis::EIN && !nachFailsafe)) ++schaltungenStunde;
    if (vorher != nachher) {
      ++s.schaltungen;
      uint32_t dauer = now - letzteSchaltung;
      if (pruefen && !ersterWechsel) {
        if (nachher)  CHECK(dauer >= p.minOffTime, "Pause %u ms < MIN_OFF_TIME (Schritt %ld)", dauer, i);
        else if (e != Ereignis::FAILSAFE)
                      CHECK(dauer >= p.minOnTime, "Laufzeit %u ms < MIN_ON_TIME (Schritt %ld)", dauer, i);
      }
      if (pruefen && nachher) {
        CHECK(a.gueltig, "Einschalten mit ungültigen Werten (Schritt %ld)", i);
        CHECK(a.ahDiff > p.ahOnMargin, "Einschalten bei Diff %.2f (Schritt %ld)", a.ahDiff, i);
        CHECK(a.tempOut > p.minOutTemp, "Einschalten bei %.1f °C außen (Schritt %ld)", a.tempOut, i);
      }
      letzteSchaltung = now; letzterZustand = nachher; ersterWechsel = false;
    }
    if (pruefen) {
      if (!a.gueltig) CHECK(!nachher, "Relais an trotz ungültiger Werte (Schritt %ld)", i);
      if (e == Ereignis::KEINS) CHECK(vorher == nachher, "Zustandswechsel ohne Ereignis (Schritt %ld)", i);
    }
    if (nachher) s.laufzeitH += dt / 3600000.0;
    if ((i + 1) % 720 == 0) {               // volle Stunde
      if (schaltungenStunde > s.maxProStunde) s.maxProStunde = schaltungenStunde;
      schaltungenStunde = 0;
    }
  }
  (void)letzterZustand;
  return s;
}

static double anteilKurz(const Statistik& s) {
  return s.ausRegulaer ? 100.0 * s.kurzeLaeufe / s.ausRegulaer : 0.0;
}

static void druckeStatistik(const Statistik& s) {
  std::printf("  Schaltvorgänge: %ld (%.1f/Tag), davon Failsafe: %ld, max. %ld reguläre Schaltungen/h\n",
              s.schaltungen, s.schaltungen / 365.0, s.failsafe, s.maxProStunde);
  std::printf("  Kurzläufe (< 3 min): %ld von %ld (%.1f %%), Lüfterlaufzeit: %.0f h (%.1f %%)\n",
              s.kurzeLaeufe, s.ausRegulaer, anteilKurz(s), s.laufzeitH, s.laufzeitH / 87.6);
}

static void testLangzeit() {
  std::printf("[Langzeitsimulation 1 Jahr, 5-s-Takt, realistisches Sensorrauschen, inkl. millis()-Überläufe]\n");
  // BME280: Temperatur-Rauschen ~0,01 °C, Feuchte ~0,1 %% rF – hier großzügiger angesetzt
  Statistik s = simuliereJahr(true, 0.05f, 0.3f);
  druckeStatistik(s);
  CHECK(s.schaltungen > 100, "Simulation sollte den Lüfter regelmäßig schalten (%ld)", s.schaltungen);
  CHECK(s.failsafe > 0, "Simulation sollte Failsafe auslösen");
  // Die Hysteresen (Feuchte und Temperatur) sollen Takten verhindern:
  // kaum Läufe, die nur die Mindestlaufzeit dauern. (Bei Grenzwetterlagen sind Läufe von
  // wenigen Minuten normal – maxProStunde wird daher nur ausgegeben, nicht streng geprüft.)
  CHECK(anteilKurz(s) <= 10.0, "Lüfter taktet: %.1f %% Kurzläufe", anteilKurz(s));
  CHECK(s.maxProStunde <= 56, "Mindestzeiten verletzt: %ld Schaltungen/h", s.maxProStunde);

  std::printf("[Stresstest 1 Jahr mit starkem Rauschen (0,15 °C / 0,45 %% rF pro Messung)]\n");
  s = simuliereJahr(true, 0.15f, 0.45f);
  druckeStatistik(s);
  // Harte Grenze aus den Mindestzeiten: 2 Schaltungen je (120 s + 10 s) -> max. ~55/h (+ Failsafe)
  CHECK(s.maxProStunde <= 56, "Mindestzeiten verletzt: %ld Schaltungen/h", s.maxProStunde);
}

// ---------------------------------------------------------------------------
// Replay einer SD-Logdatei (keller_log_*.csv): Werte durch die Regelung schicken
static std::vector<std::string> splitCSV(const std::string& zeile) {
  std::vector<std::string> felder;
  std::string f;
  std::stringstream ss(zeile);
  while (std::getline(ss, f, ';')) {
    std::string t;
    for (char c : f) if (c != '"' && c != '\r') t += (c == ',' ? '.' : c);
    felder.push_back(t);
  }
  return felder;
}

static int replay(const char* pfad) {
  std::ifstream in(pfad);
  if (!in) { std::printf("Datei nicht lesbar: %s\n", pfad); return 2; }
  std::string zeile;
  if (!std::getline(in, zeile)) { std::printf("Datei leer\n"); return 2; }
  std::vector<std::string> kopf = splitCSV(zeile);
  auto spalte = [&](const char* name) {
    for (size_t i = 0; i < kopf.size(); ++i) if (kopf[i] == name) return int(i);
    return -1;
  };
  int cMs = spalte("Millis"), cTi = spalte("TempIn"), cHi = spalte("HumIn");
  int cTo = spalte("TempOut"), cHo = spalte("HumOut"), cRel = spalte("Relais"), cBoot = spalte("Boot");
  if (cMs < 0 || cTi < 0 || cHi < 0 || cTo < 0 || cHo < 0) {
    std::printf("Spalten Millis/TempIn/HumIn/TempOut/HumOut nicht gefunden\n");
    return 2;
  }
  int maxSpalte = std::max(std::max(cMs, cTi), std::max(cHi, std::max(cTo, cHo)));
  auto zahl = [](const std::vector<std::string>& f, int c) {
    return (c >= 0 && c < int(f.size()) && !f[c].empty()) ? std::strtof(f[c].c_str(), nullptr) : NAN;
  };

  Regler r;
  long datensaetze = 0, schaltungen = 0, failsafe = 0, abweichungen = 0, neustarts = 0;
  double laufzeitH = 0;
  uint32_t letzteZeit = 0;
  std::string letzterBoot;
  bool erste = true;

  while (std::getline(in, zeile)) {
    std::vector<std::string> f = splitCSV(zeile);
    if (int(f.size()) <= maxSpalte || (f[cTi].empty() && f[cTo].empty())) continue;  // BOOT-/Ereigniszeilen
    uint32_t ms = uint32_t(std::strtoul(f[cMs].c_str(), nullptr, 10));
    std::string boot = cBoot >= 0 ? f[cBoot] : "";
    // Neustart erkennen (neue Boot-Nr. oder Millis springt zurück) -> Regler zurücksetzen wie auf dem ESP32
    if (!erste && (boot != letzterBoot || ms < letzteZeit)) { r = Regler(); ++neustarts; }
    if (!erste && r.relaisAn() && ms > letzteZeit) laufzeitH += (ms - letzteZeit) / 3600000.0;

    Auswertung a = auswerten(Messung{zahl(f, cTi), zahl(f, cHi), zahl(f, cTo), zahl(f, cHo)});
    Ereignis e = r.update(a, ms);
    if (e == Ereignis::EIN || e == Ereignis::AUS) ++schaltungen;
    if (e == Ereignis::FAILSAFE) ++failsafe;
    if (cRel >= 0 && cRel < int(f.size()) && !f[cRel].empty() && (f[cRel] == "1") != r.relaisAn()) ++abweichungen;

    ++datensaetze; letzteZeit = ms; letzterBoot = boot; erste = false;
  }

  std::printf("Replay %s\n", pfad);
  std::printf("  Datensätze:        %ld\n", datensaetze);
  std::printf("  Neustarts im Log:  %ld\n", neustarts);
  std::printf("  Schaltvorgänge:    %ld\n", schaltungen);
  std::printf("  Failsafe:          %ld\n", failsafe);
  std::printf("  Lüfterlaufzeit:    %.1f h\n", laufzeitH);
  if (cRel >= 0)
    std::printf("  Abweichungen zum geloggten Relaisstatus: %ld (%.2f %%)\n", abweichungen,
                datensaetze ? 100.0 * abweichungen / datensaetze : 0.0);
  return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--replay") == 0) return replay(argv[2]);

  testAbsoluteFeuchte();
  testPlausibilitaet();
  testEinschalten();
  testAusschaltenUndHysterese();
  testFailsafe();
  testMillisUeberlauf();
  testLangzeit();

  std::printf("\n%d Prüfungen, %d Fehler -> %s\n", pruefungen, fehler, fehler ? "FEHLGESCHLAGEN" : "OK");
  return fehler ? 1 : 0;
}
