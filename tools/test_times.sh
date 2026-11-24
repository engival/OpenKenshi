#!/bin/bash
# Measures every registered ctest alone (kenshi-k16.15.54): one test per ctest call, wall + CPU (user+sys of the whole process tree) from
# /usr/bin/time, the dev load average before the run, and the phase lines of KSIM_TEST_PHASES (include/ksim/dev/test_phases.hpp).
# Usage: tools/test_times.sh <outdir> [lock-file [test-list-file]]   (build dir: cpp/build of this checkout; rebuild first; run it in the background)
# With a lock file, each test runs under `flock <lock> nice -n 10` (the dev box's worker lock); resumes: a test with a result file is skipped.
# Writes <outdir>/raw/<test>.{time,phases,log}; tools/test_times_collect.py turns them into times.tsv.
out="${1:?usage: tools/test_times.sh <outdir> [lock-file]}"
lock="$2"
list="$3"
build="$(cd "$(dirname "$0")/.." && pwd)/cpp/build"
mkdir -p -- "$out/raw"
out="$(cd "$out" && pwd)"   # absolute: the tests run in the build dir and KSIM_TEST_PHASES is a path
if [ -n "$list" ]; then cp -- "$list" "$out/tests.txt"; else ctest --test-dir "$build" -N | sed -n 's/^ *Test *#[0-9]*: //p' > "$out/tests.txt"; fi
while read -r name
do
	[ -s "$out/raw/$name.time" ] && continue
	rm -f -- "$out/raw/$name.phases"
	runner="nice -n 10"
	[ -n "$lock" ] && runner="flock $lock nice -n 10"
	cat /proc/loadavg > "$out/raw/$name.load.pre"
	KSIM_WORKER_TESTS=1 KSIM_TEST_PHASES="$out/raw/$name.phases" $runner /usr/bin/time -f "%e %U %S" -o "$out/raw/$name.time" \
		ctest --test-dir "$build" -R "^$name\$" -FS . -FC . --timeout 1500 --output-on-failure > "$out/raw/$name.log" 2>&1
	echo "rc=$?" >> "$out/raw/$name.time"
	cat /proc/loadavg > "$out/raw/$name.load.post"
done < "$out/tests.txt"
echo finished > "$out/DONE"
