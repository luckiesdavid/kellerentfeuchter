#!/usr/bin/env bash
# Baut und startet die Tests der Lüftersteuerung auf dem PC (g++ oder clang++ nötig).
#   tests/run_tests.sh                    alle Tests
#   tests/run_tests.sh --replay LOG.csv   SD-Logdatei durch die Regelung schicken
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
mkdir -p build
# -std=c++11 wie ESP32-Arduino-Core 2.x (gnu++11) – was hier kompiliert, kompiliert auch dort
"$CXX" -std=c++11 -O2 -Wall -Wextra -Werror -o build/test_regelung test_regelung.cpp
exec ./build/test_regelung "$@"
