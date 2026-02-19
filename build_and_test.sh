#!/bin/bash
# =============================================================================
# KairosSNNI Build & Test Script
# =============================================================================
# Builds the production server/client, builds the test suite, runs all tests,
# and produces a summary report.
#
# Usage:
#   ./build_and_test.sh          # Full build + test
#   ./build_and_test.sh --test   # Test only (skip production build)
#   ./build_and_test.sh --clean  # Clean, build, and test
# =============================================================================

set -e

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

NPROC=$(nproc 2>/dev/null || echo 4)
TEST_BIN="test_schedulers"
LOG_DIR="logs"
REPORT_FILE="${LOG_DIR}/build_test_report.txt"

# Parse arguments
SKIP_BUILD=false
DO_CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --test)  SKIP_BUILD=true ;;
        --clean) DO_CLEAN=true ;;
        --help|-h)
            echo "Usage: $0 [--test] [--clean] [--help]"
            echo "  --test   Skip production build, run tests only"
            echo "  --clean  Run make clean before building"
            echo "  --help   Show this help message"
            exit 0
            ;;
    esac
done

mkdir -p "$LOG_DIR"

echo -e "${CYAN}=============================================${NC}"
echo -e "${CYAN}  KairosSNNI Build & Test Suite${NC}"
echo -e "${CYAN}=============================================${NC}"
echo ""

OVERALL_OK=true
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

# ----- Section 1: Production Build -----
if [ "$SKIP_BUILD" = false ]; then
    echo -e "${YELLOW}[1/4] Production Build${NC}"

    if [ "$DO_CLEAN" = true ]; then
        echo "  Cleaning..."
        make clean 2>&1 | sed 's/^/  /'
    fi

    echo "  Building Kairos_server and Kairos_client (make -j${NPROC})..."
    BUILD_START=$(date +%s%N)
    if make -j"$NPROC" 2>&1 | sed 's/^/  /'; then
        BUILD_END=$(date +%s%N)
        BUILD_MS=$(( (BUILD_END - BUILD_START) / 1000000 ))
        echo -e "  ${GREEN}Production build: OK${NC} (${BUILD_MS}ms)"
        PROD_BUILD_STATUS="PASS (${BUILD_MS}ms)"
    else
        echo -e "  ${RED}Production build: FAILED${NC}"
        PROD_BUILD_STATUS="FAIL"
        OVERALL_OK=false
    fi
    echo ""
else
    echo -e "${YELLOW}[1/4] Production Build -- SKIPPED (--test mode)${NC}"
    PROD_BUILD_STATUS="SKIPPED"
    echo ""
fi

# ----- Section 2: Test Build -----
echo -e "${YELLOW}[2/4] Test Suite Build${NC}"

CXX="g++"
CXXFLAGS="-std=c++17 -I./include -Wall -Wextra -O3"
TEST_SRCS="src/test_schedulers.cpp src/ConfigManager.cpp src/StringUtils.cpp src/SystemMonitor.cpp src/FileManager.cpp src/ResourceManager.cpp src/Logger.cpp"
TEST_LDFLAGS="-lpthread -lstdc++fs"

echo "  Compiling ${TEST_BIN}..."
TEST_BUILD_START=$(date +%s%N)
if $CXX $CXXFLAGS $TEST_SRCS -o "$TEST_BIN" $TEST_LDFLAGS 2>&1 | sed 's/^/  /'; then
    TEST_BUILD_END=$(date +%s%N)
    TEST_BUILD_MS=$(( (TEST_BUILD_END - TEST_BUILD_START) / 1000000 ))
    echo -e "  ${GREEN}Test build: OK${NC} (${TEST_BUILD_MS}ms)"
    TEST_BUILD_STATUS="PASS (${TEST_BUILD_MS}ms)"
else
    echo -e "  ${RED}Test build: FAILED${NC}"
    TEST_BUILD_STATUS="FAIL"
    OVERALL_OK=false
fi
echo ""

# ----- Section 3: Run Tests -----
echo -e "${YELLOW}[3/4] Running Tests${NC}"

if [ -x "$TEST_BIN" ]; then
    TEST_RUN_START=$(date +%s%N)
    TEST_OUTPUT=$(./"$TEST_BIN" 2>&1)
    TEST_EXIT=$?
    TEST_RUN_END=$(date +%s%N)
    TEST_RUN_MS=$(( (TEST_RUN_END - TEST_RUN_START) / 1000000 ))

    echo "$TEST_OUTPUT" | sed 's/^/  /'
    echo ""

    # Extract pass/total from output
    PASS_LINE=$(echo "$TEST_OUTPUT" | grep "TOTAL:")
    if [ -n "$PASS_LINE" ]; then
        CHECKS_PASSED=$(echo "$PASS_LINE" | grep -oP '\d+(?=/)')
        CHECKS_TOTAL=$(echo "$PASS_LINE" | grep -oP '(?<=/)\d+')
    else
        CHECKS_PASSED="?"
        CHECKS_TOTAL="?"
    fi

    if [ $TEST_EXIT -eq 0 ]; then
        echo -e "  ${GREEN}Tests: ALL PASSED${NC} (${CHECKS_PASSED}/${CHECKS_TOTAL} checks, ${TEST_RUN_MS}ms)"
        TEST_RUN_STATUS="PASS (${CHECKS_PASSED}/${CHECKS_TOTAL}, ${TEST_RUN_MS}ms)"
    else
        echo -e "  ${RED}Tests: FAILED${NC} (${CHECKS_PASSED}/${CHECKS_TOTAL} checks)"
        TEST_RUN_STATUS="FAIL (${CHECKS_PASSED}/${CHECKS_TOTAL})"
        OVERALL_OK=false
    fi
else
    echo -e "  ${RED}Test binary not found -- skipping test run${NC}"
    TEST_RUN_STATUS="SKIP (binary not found)"
    OVERALL_OK=false
fi
echo ""

# ----- Section 4: Config Validation -----
echo -e "${YELLOW}[4/4] Configuration Validation${NC}"

CONFIG_STATUS="PASS"
for cfg_file in config.cfg toy_config.cfg; do
    if [ -f "$cfg_file" ]; then
        echo -e "  Checking ${cfg_file}..."
        MISSING_KEYS=""
        for key in SERVER_IP SCHEDULER_PORT TOTAL_CORES SCHEDULER_MODE \
                   VFT_SAFETY_MARGIN DEFAULT_SLO_K_FACTOR AGING_FACTOR \
                   SA_INITIAL_TEMP SA_COOLING_RATE SA_MAX_ITERATIONS SA_WINDOW_SIZE \
                   SERVER_CMD_TEMPLATE PREPROC_CMD_TEMPLATE SNNI_DIR LOG_FILE; do
            if ! grep -q "^${key}=" "$cfg_file"; then
                MISSING_KEYS="${MISSING_KEYS} ${key}"
            fi
        done
        if [ -z "$MISSING_KEYS" ]; then
            echo -e "    ${GREEN}All required keys present${NC}"
        else
            echo -e "    ${RED}Missing keys:${MISSING_KEYS}${NC}"
            CONFIG_STATUS="WARN: missing keys in ${cfg_file}"
        fi
    else
        echo -e "  ${YELLOW}${cfg_file} not found (optional for test env)${NC}"
    fi
done
echo ""

# ----- Summary Report -----
echo -e "${CYAN}=============================================${NC}"
echo -e "${CYAN}  BUILD & TEST REPORT${NC}"
echo -e "${CYAN}=============================================${NC}"
echo -e "  Date:              ${TIMESTAMP}"
echo -e "  Production Build:  ${PROD_BUILD_STATUS}"
echo -e "  Test Build:        ${TEST_BUILD_STATUS}"
echo -e "  Test Execution:    ${TEST_RUN_STATUS}"
echo -e "  Config Validation: ${CONFIG_STATUS}"
echo ""

if [ "$OVERALL_OK" = true ]; then
    echo -e "  ${GREEN}OVERALL: ALL CHECKS PASSED${NC}"
    OVERALL_RESULT="PASS"
else
    echo -e "  ${RED}OVERALL: SOME CHECKS FAILED${NC}"
    OVERALL_RESULT="FAIL"
fi
echo -e "${CYAN}=============================================${NC}"

# Save report to file
cat > "$REPORT_FILE" <<EOF
KairosSNNI Build & Test Report
Date: ${TIMESTAMP}
==============================================
Production Build:  ${PROD_BUILD_STATUS}
Test Build:        ${TEST_BUILD_STATUS}
Test Execution:    ${TEST_RUN_STATUS}
Config Validation: ${CONFIG_STATUS}
----------------------------------------------
OVERALL: ${OVERALL_RESULT}
==============================================
EOF

echo ""
echo "  Report saved to: ${REPORT_FILE}"

# Exit with appropriate code
if [ "$OVERALL_OK" = true ]; then
    exit 0
else
    exit 1
fi
