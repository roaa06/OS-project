#!/bin/bash

# ========================================================
# OS Scheduler Automated Test Runner
# ========================================================

# Colors for terminal output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Ensure clean build
echo -e "${BLUE}=== Building project binaries ===${NC}"
make build > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo -e "${RED}[ERROR] Build failed! Check compiler errors.${NC}"
    exit 1
fi

run_single_test() {
    local TEST_NAME=$1
    local TEST_DIR=$2
    local ALGO_INPUT=$3

    echo ""
    echo -e "${YELLOW}------------------------------------------------${NC}"
    echo -e "${BLUE}Running Test:${NC} ${TEST_NAME}"
    echo -e "${YELLOW}------------------------------------------------${NC}"

    # Clean up any leftover processes or IPC from aborted previous runs
    killall -9 clk.out scheduler.out process.out process_generator.out 2>/dev/null
    ipcrm -M 300 2>/dev/null
    ipcrm -Q 400 2>/dev/null
    sleep 0.2

    # Setup input files
    cp "${TEST_DIR}/processes.txt" ./processes.txt
    if [ -f "${TEST_DIR}/resources.txt" ]; then
        cp "${TEST_DIR}/resources.txt" ./resources.txt
    else
        > ./resources.txt
    fi

    # Run simulation
    echo -e "Executing process_generator with inputs: ${ALGO_INPUT}..."
    printf "%b" "${ALGO_INPUT}" | ./process_generator.out > /dev/null 2>&1

    # Verify scheduler.log
    echo -n "Checking scheduler.log: "
    if diff -u scheduler.log "${TEST_DIR}/expected_scheduler.log" > /dev/null; then
        echo -e "${GREEN}[PASS] Log matches expected output exactly!${NC}"
    else
        echo -e "${RED}[FAIL] Differences detected in scheduler.log:${NC}"
        diff -u scheduler.log "${TEST_DIR}/expected_scheduler.log"
        return 1
    fi

    # Verify scheduler.perf
    echo -n "Checking scheduler.perf: "
    if diff -u scheduler.perf "${TEST_DIR}/expected_scheduler.perf" > /dev/null; then
        echo -e "${GREEN}[PASS] Performance metrics match expected output!${NC}"
    else
        echo -e "${RED}[FAIL] Differences detected in scheduler.perf:${NC}"
        diff -u scheduler.perf "${TEST_DIR}/expected_scheduler.perf"
        return 1
    fi

    echo -e "${GREEN}>>> ${TEST_NAME}: ALL CHECKS PASSED! <<<${NC}"
    return 0
}

TARGET=${1:-"all"}

case "$TARGET" in
    sjf)
        run_single_test "Shortest Job First (SJF)" "test_cases/1_sjf" "1\n"
        ;;
    rr)
        run_single_test "Round Robin (RR)" "test_cases/2_rr" "2\n3\n1\n"
        ;;
    hpf)
        run_single_test "Preemptive HPF with Priority Inheritance" "test_cases/3_hpf" "3\n"
        ;;
    bonus)
        run_single_test "Bonus: HPF with Priority Demotion" "test_cases/4_bonus_demotion" "4\n"
        ;;
    all)
        echo -e "${BLUE}Running all 4 test suites sequentially...${NC}"
        run_single_test "1. Shortest Job First (SJF)" "test_cases/1_sjf" "1\n"
        run_single_test "2. Round Robin (RR)" "test_cases/2_rr" "2\n3\n1\n"
        run_single_test "3. Preemptive HPF with Priority Inheritance" "test_cases/3_hpf" "3\n"
        run_single_test "4. Bonus: HPF with Priority Demotion" "test_cases/4_bonus_demotion" "4\n"
        echo ""
        echo -e "${GREEN}================================================${NC}"
        echo -e "${GREEN}         ALL TEST SUITES COMPLETED!             ${NC}"
        echo -e "${GREEN}================================================${NC}"
        ;;
    *)
        echo "Usage: $0 [sjf | rr | hpf | bonus | all]"
        exit 1
        ;;
esac
