// ksim_server: the process around the library (src/ksim/server_main.cpp, which documents the options): the SIGINT / SIGTERM
// stop flag, the crash recorder and the exit code.

#include "ksim/server_main.hpp"

#include "kutil/crashlog.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <string>

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int)
{
	g_stop.store(true);
}

} // namespace

int main(int argc, char** argv)
{
	ksim::ServerConfig options;
	if (const int code = ksim::parseServerArgs(argc, argv, options); code >= 0)
		return code;
	// A crash leaves a report with the signal, the sim tick (the number a replay names), a backtrace and the maps (kutil/crashlog.hpp,
	// shared with kclient). No HOME, no default directory: --crash-dir still installs it.
	{
		std::string crashDir = options.crashDir;
		const char* home = crashDir.empty() ? std::getenv("HOME") : nullptr;
		if (crashDir.empty() && home != nullptr)
			crashDir = std::string(home) + "/.local/share/ksim/crashes";
		if (!crashDir.empty())
			kutil::crashlog_install(crashDir, "ksim_server");
	}
	options.onTick = kutil::crashlog_tick;
	options.beforeRun = []()
	{
		std::signal(SIGINT, onSignal);
		std::signal(SIGTERM, onSignal);
	};
	return ksim::runServer(options, g_stop);
}
