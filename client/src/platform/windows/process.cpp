#include "platform/process.hpp"

#include <fcntl.h>
#include <io.h>

#include <windows.h>

// CreateProcessW with the child's stdout on an inherited pipe (kenshi-379.31.1). The child goes in a job that kills it when our handle closes,
// so a crashed client takes the server with it (Linux: PR_SET_PDEATHSIG). Windows has no SIGTERM: a graceful stop terminates it too.
namespace plat {

namespace {
std::wstring widen(const std::string& s)
{
	if (s.empty()) { return {}; }
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

// One argument quoted the way CommandLineToArgvW / the CRT split it back.
void appendQuoted(std::wstring& cmd, const std::wstring& arg)
{
	if (!cmd.empty()) { cmd += L' '; }
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) { cmd += arg; return; }
	cmd += L'"';
	for (size_t i = 0;; ++i)
	{
		size_t slashes = 0;
		while (i < arg.size() && arg[i] == L'\\') { ++i; ++slashes; }
		if (i == arg.size()) { cmd.append(slashes * 2, L'\\'); break; }
		if (arg[i] == L'"') { cmd.append(slashes * 2 + 1, L'\\'); }
		else { cmd.append(slashes, L'\\'); }
		cmd += arg[i];
	}
	cmd += L'"';
}

std::string lastError(const char* what)
{
	const DWORD code = GetLastError();
	char msg[256] = {};
	if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, msg, sizeof msg, nullptr) == 0) { return std::string(what) + ": error " + std::to_string(code); }
	std::string s = msg;
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) { s.pop_back(); }
	return std::string(what) + ": " + s;
}

void release(Child& c)
{
	if (c.handle) { CloseHandle((HANDLE)c.handle); }
	if (c.job) { CloseHandle((HANDLE)c.job); }
	c = Child();
}
}

bool spawnPiped(const std::vector<std::string>& args, Child& c, FILE*& out, std::string& err)
{
	if (args.empty()) { err = "no executable"; return false; }
	std::wstring cmd;
	for (const std::string& a : args) { appendQuoted(cmd, widen(a)); }
	SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
	HANDLE rd = nullptr, wr = nullptr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) { err = lastError("CreatePipe"); return false; }
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);	// only the write end goes to the child
	STARTUPINFOW si = {};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = wr;
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	PROCESS_INFORMATION pi = {};
	const std::wstring exe = widen(args[0]);
	if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
	{
		err = args[0] + ": " + lastError("CreateProcess");
		CloseHandle(rd);
		CloseHandle(wr);
		return false;
	}
	CloseHandle(wr);
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
		lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
		AssignProcessToJobObject(job, pi.hProcess);
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	c.pid = (long long)pi.dwProcessId;
	c.handle = pi.hProcess;
	c.job = job;
	const int fd = _open_osfhandle((intptr_t)rd, _O_RDONLY | _O_TEXT);
	out = fd >= 0 ? _fdopen(fd, "r") : nullptr;
	if (!out) { if (fd >= 0) { _close(fd); } else { CloseHandle(rd); } }
	return true;
}

bool childRunning(Child& c, std::string& how)
{
	if (!c.handle) { return false; }
	if (WaitForSingleObject((HANDLE)c.handle, 0) != WAIT_OBJECT_0) { return true; }
	DWORD code = 0;
	GetExitCodeProcess((HANDLE)c.handle, &code);
	how = "exited with status " + std::to_string((unsigned long)code);
	release(c);
	return false;
}

bool stopChild(Child& c, bool graceful)
{
	(void)graceful;	// no SIGTERM on Windows
	if (!c.handle) { return true; }
	bool gone = WaitForSingleObject((HANDLE)c.handle, 0) == WAIT_OBJECT_0;
	if (!gone)
	{
		TerminateProcess((HANDLE)c.handle, 1);
		WaitForSingleObject((HANDLE)c.handle, 2000);
	}
	release(c);
	return gone;
}

void setEnv(const char* name, const char* value) { _putenv_s(name, value); }
void unsetEnv(const char* name) { _putenv_s(name, ""); }	// an empty value removes it

}
