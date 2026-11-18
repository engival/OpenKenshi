// The shared crash recorder (kutil/crashlog.hpp) for every ksim / knav test binary. A test that dies of a signal under ctest leaves a
// report in ~/.local/share/ksim/crashes whose lines carry the test's name, a backtrace and /proc/self/maps, instead of dying with no
// trace at all: ksim_simlink died of SIGSEGV once in a full ctest -j8 run (2026-11-08) and there was nothing to send (no report, no core).
//
// One place: cpp/CMakeLists.txt adds this source, with TEST_CRASHLOG_NAME set to the target's name, to every test target whose sources
// are under tests/ and whose name starts ksim_ or knav_ (the block at the end of the file), so a new test takes it with no per-target
// line. The tools in tools/ and the *_asan binaries are left out (ksim_server installs its own; ASan owns the signals in the asan runs).
// The handler re-raises the signal (kutil::crashlog), so the process still dies by SIGSEGV and ctest still reports the test as failed;
// a test that installs its own handler from main (kutil_crashlog, ksim_server) overrides this one.
#include "kutil/crashlog.hpp"

#include <cstdlib>
#include <string>

namespace {

struct CrashRecorder
{
	CrashRecorder()
	{
		const char* home = std::getenv("HOME");
		if (home == nullptr)
			return; // no HOME, no default directory (as ksim_server: --crash-dir is the tool's own switch)
		kutil::crashlog_install(std::string(home) + "/.local/share/ksim/crashes", TEST_CRASHLOG_NAME);
	}
};

const CrashRecorder gCrashRecorder; // a static initializer: the handler is in place before main, so a crash in a test's setup is reported too

} // namespace
