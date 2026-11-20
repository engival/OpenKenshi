#pragma once
#include <cstdio>
#include <string>
#include <vector>

// The local server child (platform/localserver.cpp, kclient only; OpenKenshi runs its server on a thread): spawn with stdout piped back, poll, stop.
// POSIX fork / execv on Linux and macOS (platform/posix/process.cpp), CreateProcess on Windows (platform/windows/process.cpp, kenshi-379.31.1).
namespace plat {

struct Child
{
	long long pid = -1;	// -1: none
	void* handle = nullptr;	// Windows: the process handle and its kill-on-close job (POSIX: unused)
	void* job = nullptr;
};

// Starts argv[0] with argv; out reads the child's stdout (stderr stays ours). The child dies with us where the OS can arrange it (Linux
// PR_SET_PDEATHSIG, a Windows kill-on-close job; macOS cannot).
bool spawnPiped(const std::vector<std::string>& argv, Child& c, FILE*& out, std::string& err);
// True while it runs. False once it exited: c is reset and how says how ("exited with status N", "killed by signal ...").
bool childRunning(Child& c, std::string& how);
// Ends it. graceful: ask it to stop (SIGTERM; Windows has no such request: terminated at once), up to 2 s, then kill. Returns true when it stopped
// on the request (false: killed). c is reset.
bool stopChild(Child& c, bool graceful);

// This process's environment, as getenv reads it: setenv / unsetenv on POSIX, _putenv_s on Windows (the CRT's copy, which getenv reads).
void setEnv(const char* name, const char* value);
void unsetEnv(const char* name);

}
