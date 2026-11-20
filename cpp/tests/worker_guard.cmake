# ctest's worker guard (fixture `worker_guard`, required by every test): a worker started by beads_launcher.lua (env BEADS_LAUNCHER)
# runs tests only through tools/wtest.sh (the worker test lane: up to three at once, user 2026-11-19, kenshi-b9o7; the queue and CI
# do not pause for it), which sets KSIM_WORKER_TESTS=1. Leads, CI and the queue's runner are not under the launcher.
if(DEFINED ENV{BEADS_LAUNCHER} AND NOT "$ENV{KSIM_WORKER_TESTS}" STREQUAL "1")
	message(FATAL_ERROR
		"\n\n*** WORKER: run tests only with tools/wtest.sh '<regex>' (in the background; it waits its turn). ***\n"
		"*** Or commit, push ds/<task> and close the item: ksim queues it (your CLAUDE.md \"Tests\"). ***\n\n")
endif()
