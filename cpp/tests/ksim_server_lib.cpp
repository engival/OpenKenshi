// Task 379.5: ksim_server's command line and server as a library (ksim/server_main.hpp), without the process around them: --help and
// a usage error give their exit codes through parseServerArgs, and runServer dumps a --no-world snapshot into the log sink.

#include "ksim/net/simlink.hpp"
#include "ksim/server_main.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ksim;

void require(bool ok, const std::string& what)
{
	if (!ok)
		throw std::runtime_error(what);
}

int parse(std::vector<std::string> words, ServerConfig& config)
{
	std::vector<char*> argv;
	for (std::string& word : words)
		argv.push_back(word.data());
	return parseServerArgs(static_cast<int>(argv.size()), argv.data(), config);
}

} // namespace

int main()
{
	try
	{
		std::ostringstream log;
		ServerConfig help;
		help.log = &log;
		require(parse({"ksim_server", "--help"}, help) == 0, "--help is not exit code 0");
		require(log.str().find("usage: OpenKenshi-server --listen") != std::string::npos, "--help printed no usage text");
		{
			std::ostringstream versionLog;
			ServerConfig version;
			version.log = &versionLog;
			require(parse({"ksim_server", "--version"}, version) == 0, "--version is not exit code 0");
			require(versionLog.str().rfind("OpenKenshi server ", 0) == 0 && versionLog.str().size() > 19 && versionLog.str().back() == '\n', "--version text");
		}
		{
			std::ostringstream wireLog;
			ServerConfig wire;
			wire.log = &wireLog;
			require(parse({"ksim_server", "--wire-version"}, wire) == 0, "--wire-version is not exit code 0");
			require(wireLog.str() == std::to_string(simlink::kProtocol) + "\n", "--wire-version prints kProtocol and a newline");
		}

		ServerConfig bad;
		bad.log = &log;
		require(parse({"ksim_server", "--bogus"}, bad) == 2, "an unknown option is not exit code 2");
		require(parse({"ksim_server"}, bad) == 2, "neither --listen nor --dump-snapshot is not exit code 2");

		const std::string dump = "ksim_server_lib.txt";
		ServerConfig config;
		std::ostringstream out;
		std::ostringstream err;
		config.log = &err;
		config.out = &out;
		require(parse({"ksim_server", "--scenario", "duel", "--no-world", "--dump-snapshot", dump, "--dump-ticks", "2"}, config) == -1,
			"a good command line did not say run");
		std::uint64_t lastTick = 0;
		config.onTick = [&](std::uint64_t tick) { lastTick = tick; };
		std::atomic<bool> stop{false};
		require(runServer(config, stop) == 0, "runServer failed: " + err.str());
		require(err.str().find("dumped 2 ticks") != std::string::npos, "the log sink missed the dump line: " + err.str());
		require(lastTick != 0, "onTick was never called");
		std::ifstream in(dump);
		std::string first;
		require(static_cast<bool>(std::getline(in, first)) && first.rfind("# ksim_server --dump-snapshot", 0) == 0, "the snapshot file is empty");
		std::remove(dump.c_str());
		std::cout << "ksim_server_lib: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_server_lib: FAILED: " << e.what() << "\n";
		return 1;
	}
}
