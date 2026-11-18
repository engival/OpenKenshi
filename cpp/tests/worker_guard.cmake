# ctest's worker guard (fixture `worker_guard`, required by every test): a worker started by beads_launcher.lua (env BEADS_LAUNCHER)
# does not run tests; its close goes to the test queue, which builds and tests the branch (user 2026-11-17). A worker on an item
# labelled no-test-queue (a debugging loop) sets KSIM_WORKER_TESTS=1 and names its tests with -R. Leads, master CI and the queue's
# runner are not under the launcher and never see this.
if(DEFINED ENV{BEADS_LAUNCHER} AND NOT "$ENV{KSIM_WORKER_TESTS}" STREQUAL "1")
	message(FATAL_ERROR
		"\n\n*** WORKER: DO NOT RUN TESTS. Commit, push ds/<task> and close the item: the test queue tests it. ***\n"
		"*** Only an item labelled no-test-queue: KSIM_WORKER_TESTS=1 ctest -R <its tests> (your CLAUDE.md \"Tests\"). ***\n\n")
endif()
