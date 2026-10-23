#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .test-build
compiler="${CXX:-c++}"
include="src/rslidar_sdk/src/rs_driver/src"
"$compiler" -std=c++17 -O2 -pthread -I "$include" tests/em4_decoder_test.cpp -o .test-build/em4_decoder_test
.test-build/em4_decoder_test
if [ "$#" -gt 0 ]; then
  "$compiler" -std=c++17 -O2 -pthread -DENABLE_DIFOP_PARSE -I "$include" tests/em4_pcap_driver_test.cpp -lpcap -o .test-build/em4_pcap_driver_test
  .test-build/em4_pcap_driver_test "$1" .test-build/real-pcap-report.json
fi
