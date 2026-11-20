// The dev-only launch guard (kenshi-k16.2.392, user 2026-11-19; see launchguard.hpp): a Claude session may not open a window on the user's
// desktop. The check is two facts about the process: the Claude Code environment it inherited, and whether a gamescope ancestor (what
// client/tools/headless.sh wraps every agent run in) is there to receive the window instead of the user's desktop.
//
// The environment: Claude Code sets CLAUDECODE=1 on every session it starts, and CLAUDE_CODE_* variables (the session id, the entrypoint,
// the model, ...); our worker sessions on ds have those plus CLAUDE_PID and CLAUDE_EFFORT. Every name starting with CLAUDE counts, so a
// session that sets only the newer names is caught too.
//
// The ancestor: /proc/<pid>/stat's ppid, one process at a time up to pid 1, each named by /proc/<pid>/comm (the exe's basename as a
// fallback). A gamescope's name is "gamescope", "gamescope-wl" (what a headless run comes up as on ds) or the gamescopereaper it spawns,
// so the test is the prefix. headless.sh runs the client below `gamescopereaper | gamescope-wl | timeout | sh -c`, never as its direct
// child, hence the walk.
#include "dev/launchguard.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <unistd.h>
extern char** environ;	// the process environment (the global one: a declaration inside a namespace would name another symbol)
#endif

namespace dev
{
namespace
{
#if defined(__linux__)
// Any variable whose name starts with CLAUDE: CLAUDECODE, CLAUDE_CODE_SESSION_ID, CLAUDE_PID, ...
bool claudeEnvironment()
{
	for (char** e = ::environ; e && *e; ++e)
	{
		const char* eq = std::strchr(*e, '=');
		const std::size_t n = eq ? (std::size_t)(eq - *e) : std::strlen(*e);
		if (n >= 6 && std::strncmp(*e, "CLAUDE", 6) == 0) { return true; }
	}
	return false;
}

bool allowedDesktop()
{
	const char* v = std::getenv("KCLIENT_ALLOW_DESKTOP");
	return v && *v && std::strcmp(v, "0") != 0;
}

// /proc/<pid>/stat: "pid (comm) state ppid ..."; the comm is in parentheses and may hold spaces or ')' itself, so the fields after it start
// at the last ')'. -1 when the process is gone (read failed, or its stat is not parseable).
long parentOf(long pid)
{
	char path[64];
	std::snprintf(path, sizeof path, "/proc/%ld/stat", pid);
	FILE* f = std::fopen(path, "r");
	if (!f) { return -1; }
	char buf[1024];
	const bool read = std::fgets(buf, sizeof buf, f) != nullptr;
	std::fclose(f);
	if (!read) { return -1; }
	const char* close = std::strrchr(buf, ')');
	if (!close) { return -1; }
	char state;
	long ppid;
	if (std::sscanf(close + 1, " %c %ld", &state, &ppid) != 2) { return -1; }
	(void)state;
	return ppid;
}

std::string commOf(long pid)
{
	char path[64];
	std::snprintf(path, sizeof path, "/proc/%ld/comm", pid);
	FILE* f = std::fopen(path, "r");
	if (!f) { return std::string(); }
	char buf[256];
	const bool read = std::fgets(buf, sizeof buf, f) != nullptr;
	std::fclose(f);
	if (!read) { return std::string(); }
	std::string name(buf);
	while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) { name.pop_back(); }
	return name;
}

// gamescope: the name is "gamescope" or, in a headless run on this machine, "gamescope-wl" (the wrapper its --backend headless comes up
// as); the reaper gamescope spawns is a gamescope process too.
bool isGamescope(const std::string& name) { return name.compare(0, 9, "gamescope") == 0; }

std::string exeOf(long pid)
{
	char path[64];
	std::snprintf(path, sizeof path, "/proc/%ld/exe", pid);
	char buf[4096];
	const ssize_t n = ::readlink(path, buf, sizeof buf - 1);
	if (n <= 0) { return std::string(); }
	buf[n] = '\0';
	std::string exe(buf);
	const std::size_t slash = exe.rfind('/');
	return slash == std::string::npos ? exe : exe.substr(slash + 1);
}

bool gamescopeAncestor()
{
	long pid = (long)::getpid();
	for (int hop = 0; hop < 256 && pid > 1; ++hop)
	{
		pid = parentOf(pid);
		if (pid <= 0) { return false; }
		if (isGamescope(commOf(pid)) || isGamescope(exeOf(pid))) { return true; }
	}
	return false;
}
#endif
}

void launchGuard(const char* program)
{
#if defined(__linux__)
	if (!claudeEnvironment() || gamescopeAncestor()) { return; }
	if (allowedDesktop())
	{
		std::fprintf(stderr, "%s: launch guard skipped: KCLIENT_ALLOW_DESKTOP is set, so a Claude session may open a window on the desktop\n", program);
		return;
	}
	std::fprintf(stderr, "%s: refusing to open a window: launched from a Claude session outside gamescope; run it through "
		"client/tools/headless.sh (a deliberate desktop run: KCLIENT_ALLOW_DESKTOP=1)\n", program);
	std::fflush(stderr);
	std::exit(1);
#else
	(void)program;	// no /proc: the guard is Linux only (client/CLAUDE.md platform split), and a no-op elsewhere
#endif
}
}
