#!/bin/sh
# AFL++ run over the frame parsers.
#   fuzz/run_afl.sh [seconds]    (default 600)
# Results land in fuzz/out/default/ (fuzzer_stats, crashes/, hangs/).
set -eu
cd "$(dirname "$0")/.."
SECS=${1:-600}

command -v afl-fuzz >/dev/null || { echo "install afl++ first" >&2; exit 1; }
python3 fuzz/make_corpus.py

# Instrumented build in its own tree (ASan catches silent corruption).
AFL_USE_ASAN=1 make BUILD=build-afl CC=afl-clang-fast OPT=-O1 \
    build-afl/fuzz/parse_harness

AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
afl-fuzz -i fuzz/corpus -o fuzz/out -V "$SECS" -m none \
    -- build-afl/fuzz/parse_harness

echo "---- fuzzer_stats ----"
grep -E 'execs_done|execs_per_sec|saved_crashes|saved_hangs|corpus_count' \
    fuzz/out/default/fuzzer_stats
