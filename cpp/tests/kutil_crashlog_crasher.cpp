// The crash the kutil_crashlog test wants: install the recorder, set a tick, and die by SIGSEGV. argv[1] is the report directory.
// stderr is redirected by the test to a file it checks (the handler writes the same line there).
#include "kutil/crashlog.hpp"

#include <csignal>

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	kutil::crashlog_install(argv[1], "kutil_crashlog_crasher");
	kutil::crashlog_tick(42);
	std::raise(SIGSEGV);
	return 0;
}
