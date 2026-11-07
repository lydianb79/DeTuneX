#!/bin/bash
# Builds and runs the offline DSP test suite (macOS or Linux).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
c++ -std=c++17 -O2 -ISources/DSPCore/include Sources/DSPCore/Processor.cpp Tests/dsp_offline_test.cpp -o build/dsp_test
./build/dsp_test
