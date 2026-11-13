#!/usr/bin/env bash
# ==============================================================================
# Universal LiDAR Viewer - Master E2E Automated Test Suite Runner
# Covers Tiers 1-4: Contract, Boundary, Cross-Feature, and Real-World Scenarios
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Ensure build tools are in PATH if running in local environment
if [ -d "${ROOT_DIR}/.macos-build-env/bin" ]; then
    export PATH="${ROOT_DIR}/.macos-build-env/bin:${PATH}"
fi

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

echo -e "${CYAN}${BOLD}"
echo "======================================================================"
echo "    Universal LiDAR Viewer - End-to-End Test Suite Execution          "
echo "======================================================================"
echo -e "${NC}"

echo -e "${BOLD}[Step 1/4] Checking and building C++ main application binary...${NC}"
if [ ! -f "${ROOT_DIR}/cpp/build/bin/ulv_viewer" ]; then
    echo "Configuring main build..."
    cmake -B "${ROOT_DIR}/cpp/build" -S "${ROOT_DIR}/cpp" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${ROOT_DIR}/cpp/build" -j4
echo -e "${GREEN}Main executable built successfully: ${ROOT_DIR}/cpp/build/bin/ulv_viewer${NC}\n"

echo -e "${BOLD}[Step 2/4] Building C++ native test suite...${NC}"
cmake -B "${ROOT_DIR}/cpp/build-tests" -S "${ROOT_DIR}/cpp/tests" -DCMAKE_BUILD_TYPE=Release
cmake --build "${ROOT_DIR}/cpp/build-tests" -j4
echo -e "${GREEN}Test suite binary ready: ${ROOT_DIR}/cpp/build-tests/bin/ulv_test_suite${NC}\n"

echo -e "${BOLD}[Step 3/4] Running Tier 1-4 automated test cases...${NC}"
"${ROOT_DIR}/cpp/build-tests/bin/ulv_test_suite"
"${ROOT_DIR}/cpp/build-tests/bin/ulv_ui_tests"
echo -e "${GREEN}All C++ unit and integration tests passed!${NC}\n"

echo -e "${BOLD}[Step 4/4] Executing Headless CLI PCAP Regression Verification...${NC}"
TEST_PCAP="${TEST_PCAP:-}"
if [ -z "${TEST_PCAP}" ] && [ -f "${ROOT_DIR}/cpp/tests/fixtures/111.pcap" ]; then
    TEST_PCAP="${ROOT_DIR}/cpp/tests/fixtures/111.pcap"
fi

if [ -n "${TEST_PCAP}" ] && [ -f "${TEST_PCAP}" ]; then
    echo "Testing PCAP replay with: ${TEST_PCAP}"
    CLI_OUTPUT=$("${ROOT_DIR}/cpp/build/bin/ulv_viewer" --test "${TEST_PCAP}")
    echo "${CLI_OUTPUT}" | grep -E "(\[ULV C\+\+ Test Mode\]|Frame #|Opened PCAP)" || true
    echo -e "${GREEN}Headless PCAP replay test passed with code 0!${NC}\n"
else
    echo -e "${YELLOW}Warning: Test PCAP not found, skipping headless CLI PCAP replay.${NC}\n"
fi

echo -e "${CYAN}${BOLD}"
echo "======================================================================"
echo "    E2E Test Suite Status: ALL TESTS PASSED (100% GREEN)             "
echo "======================================================================"
echo -e "${NC}"
