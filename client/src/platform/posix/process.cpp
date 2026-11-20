#include "platform/process.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>

#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

namespace plat {

bool spawnPiped(const std::vector<std::string>& args, Child& c, FILE*& out, std::string& err)
{
	if (args.empty()) { err = "no executable"; return false; }
	const std::string& exe = args[0];
	std::vector<std::string> all = args;
	std::vector<char*> argv;
	for (std::string& a : all) { argv.push_back(a.data()); }
	argv.push_back(nullptr);
	if (access(exe.c_str(), X_OK) != 0) { err = exe + ": " + strerror(errno); return false; }
	int fds[2];
	if (pipe(fds) != 0) { err = std::string("pipe: ") + strerror(errno); return false; }
	const pid_t parent = getpid();
	const pid_t pid = fork();
	if (pid == 0)	// the child: only async-signal-safe calls until exec
	{
#if defined(__linux__)	// macOS has no PDEATHSIG: a crashed client can leave the server running (CI round 1; runtime untested)
		prctl(PR_SET_PDEATHSIG, SIGTERM);	// the client gone (a crash too): the server goes
#endif
		if (getppid() != parent) { _exit(1); }
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);	// one pipe for both: the host reads the startup stages from stderr and echoes it
		close(fds[0]);
		close(fds[1]);
		execv(exe.c_str(), argv.data());
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0)
	{
		close(fds[0]);
		err = std::string("fork: ") + strerror(errno);
		return false;
	}
	out = fdopen(fds[0], "r");
	if (!out) { close(fds[0]); }	// the reader sees no output; the child still runs and is stopped as usual
	c.pid = pid;
	return true;
}

bool childRunning(Child& c, std::string& how)
{
	if (c.pid < 0) { return false; }
	int status = 0;
	if (waitpid((pid_t)c.pid, &status, WNOHANG) != (pid_t)c.pid) { return true; }
	c = Child();
	how = WIFEXITED(status) ? "exited with status " + std::to_string(WEXITSTATUS(status)) : WIFSIGNALED(status) ? std::string("killed by signal ") + strsignal(WTERMSIG(status)) : "stopped";
	return false;
}

bool stopChild(Child& c, bool graceful)
{
	if (c.pid < 0) { return true; }
	const pid_t pid = (pid_t)c.pid;
	bool reaped = false;
	if (graceful)
	{
		kill(pid, SIGTERM);
		for (int i = 0; i < 200 && !reaped; ++i)	// 2 s in 10 ms steps
		{
			const pid_t r = waitpid(pid, nullptr, WNOHANG);
			reaped = r == pid || (r < 0 && errno != EINTR);	// reaped, or gone already
			if (!reaped) { usleep(10000); }
		}
	}
	if (!reaped)
	{
		kill(pid, SIGKILL);
		while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
	}
	c = Child();
	return reaped;
}

void setEnv(const char* name, const char* value) { ::setenv(name, value, 1); }
void unsetEnv(const char* name) { ::unsetenv(name); }

}
