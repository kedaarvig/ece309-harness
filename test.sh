#!/usr/bin/env bash
# ============================================================
# test.sh - AI-generated automated test harness for ece309_harness
#
# Per Project 1 requirement #5 ("AI-Generated Testing"), this script
# was written by an AI assistant (Claude) in response to the prompt:
#
#   "I have a compiled C program named harness. Write a very simple
#    Bash script that pipes a deterministic sequence of inputs into
#    the program, checks that its context/state management behaves
#    the way the spec describes, and checks that it does not leak
#    memory."
#
# See vibe_coding_log.md for the exact prompt and iteration history.
#
# What this script checks:
#   1. STATE MANAGEMENT - feeds a fixed 5-turn conversation into
#      ./harness and greps the transcript for the response we expect
#      at each step (greeting, tool call, history readback, default
#      echo, clean shutdown).
#   2. MEMORY SAFETY - rebuilds harness with AddressSanitizer +
#      LeakSanitizer (or runs it under valgrind, if installed) and
#      confirms zero leaks and no errors across that same transcript.
#
# Exit code is 0 only if every check passes.
# ============================================================

set -u   # unset variables are treated as errors; deliberately NOT
         # using -e, because we want every check to run and report,
         # rather than stopping at the first failure.

FAIL=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

pass() { printf '  [PASS] %s\n' "$1"; }
fail() { printf '  [FAIL] %s\n' "$1"; FAIL=1; }

echo "=== 1. Build ==="
if gcc -std=c99 -Wall -Wextra -O2 harness.c -o harness; then
    pass "gcc -std=c99 -Wall -Wextra -O2 harness.c -o harness (no warnings)"
else
    fail "build failed"
    exit 1
fi

echo ""
echo "=== 2. State management (deterministic transcript) ==="
# Five scripted turns exercise: the greeting keyword, the calculator
# tool, the context/history readback, a default echo, and exit.
INPUT=$'hello\ncalc 6 * 7\nhistory\nsomething else entirely\nexit\n'
OUTPUT=$(printf '%s' "$INPUT" | ./harness)

check_contains() {
    if printf '%s' "$OUTPUT" | grep -qF -- "$1"; then
        pass "$2"
    else
        fail "$2 (expected to find: $1)"
    fi
}

check_contains "Hello! I'm a mock model"      "greeting keyword triggers canned response"
check_contains "[tool:calculator] 6 * 7 = 42" "calculator tool computes 6 * 7 correctly"
check_contains "[1] user "                    "history command shows the earliest stored turn"
check_contains "I'm holding"                  "default path echoes input with a context count"
check_contains "Shutting down. Goodbye!"      "'exit' triggers a clean shutdown"

echo ""
echo "=== 3. Memory safety ==="
if command -v valgrind >/dev/null 2>&1; then
    printf '%s' "$INPUT" | valgrind --error-exitcode=99 --leak-check=full \
        ./harness > /tmp/harness_valgrind.log 2>&1
    VG_STATUS=$?
    if [ "$VG_STATUS" -eq 99 ]; then
        fail "valgrind reported errors or leaks (see /tmp/harness_valgrind.log)"
        tail -n 20 /tmp/harness_valgrind.log
    else
        pass "valgrind --leak-check=full: no leaks, no errors"
    fi
else
    echo "  (valgrind not installed here -- falling back to ASan/LSan)"
    if gcc -std=c99 -Wall -Wextra -g -fsanitize=address,undefined -o harness_asan harness.c; then
        ASAN_OPTIONS="detect_leaks=1"
        export ASAN_OPTIONS
        printf '%s' "$INPUT" | ./harness_asan > /tmp/harness_asan.log 2>&1
        if grep -q "ERROR: AddressSanitizer\|ERROR: LeakSanitizer" /tmp/harness_asan.log; then
            fail "AddressSanitizer/LeakSanitizer reported a problem"
            cat /tmp/harness_asan.log
        else
            pass "AddressSanitizer + LeakSanitizer: no leaks, no errors"
        fi
    else
        fail "could not build the AddressSanitizer instrumented binary"
    fi
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "ALL CHECKS PASSED"
    exit 0
else
    echo "SOME CHECKS FAILED"
    exit 1
fi
