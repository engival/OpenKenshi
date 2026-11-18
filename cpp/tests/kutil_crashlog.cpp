// The shared crash recorder (kutil/crashlog, kclient's moved to the cpp tree so ksim_server has it too). A program that installs it and
// raises SIGSEGV: dies by SIGSEGV as before, leaves exactly one crash-<time>-<pid>.txt, and that file carries the signal, the tick, the
// backtrace header naming the program (addr2line's target) and the maps dump (addr2line needs it after a rebuild). stderr gets the same
// report. argv[1] is the crasher binary.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "test_util.hpp"

namespace {

std::string readText(const std::filesystem::path& path)
{
	std::ifstream in(path);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool holds(const std::string& text, const std::string& what)
{
	return text.find(what) != std::string::npos;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "usage: kutil_crashlog CRASHER");
		const std::filesystem::path dir = std::filesystem::temp_directory_path()
			/ ("kutil-crashlog-" + std::to_string(static_cast<long long>(getpid())));
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		const std::filesystem::path errPath = dir.parent_path() / (dir.filename().string() + "-stderr.txt");

		const pid_t pid = fork();
		test::require(pid >= 0, "fork");
		if (pid == 0)
		{
			const int err = open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (err < 0 || dup2(err, 2) < 0)
				_exit(3);
			execl(argv[1], argv[1], dir.c_str(), static_cast<char*>(nullptr));
			_exit(4);
		}
		int status = 0;
		test::require(waitpid(pid, &status, 0) == pid, "waitpid");
		test::require(WIFSIGNALED(status), "the crasher did not die by a signal");
		test::require(WTERMSIG(status) == SIGSEGV, "the crasher's signal is not SIGSEGV");

		std::vector<std::filesystem::path> reports;
		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir))
		{
			if (entry.path().filename().string().rfind("crash-", 0) == 0)
				reports.push_back(entry.path());
		}
		test::require(reports.size() == 1, "expected exactly one crash report, found " + std::to_string(reports.size()));
		const std::string text = readText(reports.front());
		test::require(holds(text, "kutil_crashlog_crasher: crashed: signal 11 (SIGSEGV) at tick 42"),
			"the report lacks the signal line:\n" + text);
		test::require(holds(text, "backtrace (addr2line -f -C -e kutil_crashlog_crasher ADDR):"), "the report lacks the backtrace header");
		test::require(holds(text, "\nmaps:\n"), "the report lacks the maps dump");

		const std::string err = readText(errPath);
		test::require(holds(err, "kutil_crashlog_crasher: crashed: signal 11 (SIGSEGV) at tick 42"), "stderr lacks the signal line");
		test::require(holds(err, ": crash report: "), "stderr lacks the report path");

		std::filesystem::remove_all(dir, ec);
		std::filesystem::remove(errPath, ec);
		std::printf("kutil_crashlog: SIGSEGV -> %s (signal line, backtrace header, maps)\n", reports.front().filename().c_str());
		return 0;
	} catch (const std::exception& e) {
		std::printf("kutil_crashlog: %s\n", e.what());
		return 1;
	}
}
