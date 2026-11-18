#include "localserver.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

#ifdef KENSHI_EMBED_SERVER
#include "ksim/server_main.hpp"
#include <functional>
#include <ostream>
#include <streambuf>
#endif

LocalServer::~LocalServer() { stop(); }

#ifdef KENSHI_EMBED_SERVER
namespace {
// The server's out stream as lines: what the child's stdout pipe is in the fork path.
class LineBuf : public std::streambuf
{
public:
	explicit LineBuf(std::function<void(const std::string&)> f) : f_(std::move(f)) {}
protected:
	int_type overflow(int_type c) override
	{
		if (c == traits_type::eof()) { return traits_type::not_eof(c); }
		if (c == '\n') { f_(line_); line_.clear(); }
		else { line_ += static_cast<char>(c); }
		return c;
	}
private:
	std::function<void(const std::string&)> f_;
	std::string line_;
};
}
#endif

bool LocalServer::start(const std::string& exe, const std::vector<std::string>& args, std::string& err)
{
	stop();
#ifdef KENSHI_EMBED_SERVER
	(void)exe;
	{
		std::lock_guard<std::mutex> lk(m_);
		address_.clear();
		exited_.clear();
	}
	std::vector<std::string> all = { "OpenKenshi-server", "--listen", "127.0.0.1:0" };
	all.insert(all.end(), args.begin(), args.end());
	std::vector<char*> argv;
	for (std::string& a : all) { argv.push_back(a.data()); }
	argv.push_back(nullptr);
	ksim::ServerConfig config;	// log: the client's stderr; out: below; beforeRun stays empty (it installs the tool's SIGINT/SIGTERM handlers)
	if (const int rc = ksim::parseServerArgs((int)all.size(), argv.data(), config); rc >= 0) { err = "the server options were refused (exit code " + std::to_string(rc) + ")"; return false; }
	std::string line;
	for (const std::string& a : all) { line += (line.empty() ? "" : " ") + a; }
	printf("local server: in process (thread): %s\n", line.c_str());
	stopFlag_ = false;
	done_ = false;
	embedded_ = true;
	thread_ = std::thread([this, config]() mutable
	{
		LineBuf buf([this](const std::string& l)
		{
			printf("local server: %s\n", l.c_str());
			if (l.rfind("listening ", 0) == 0)
			{
				std::lock_guard<std::mutex> lk(m_);
				address_ = l.substr(10);
			}
		});
		std::ostream out(&buf);
		config.out = &out;
		code_ = ksim::runServer(config, stopFlag_);
		out.flush();
		done_ = true;
	});
	return true;
#else
	int fds[2];
	if (pipe(fds) != 0) { err = std::string("pipe: ") + strerror(errno); return false; }
	std::vector<std::string> all = { exe, "--listen", "127.0.0.1:0" };
	all.insert(all.end(), args.begin(), args.end());
	std::vector<char*> argv;
	for (std::string& a : all) { argv.push_back(a.data()); }
	argv.push_back(nullptr);
	if (access(exe.c_str(), X_OK) != 0) { close(fds[0]); close(fds[1]); err = exe + ": " + strerror(errno); return false; }
	const pid_t parent = getpid();
	pid_ = fork();
	if (pid_ == 0)	// the child: only async-signal-safe calls until exec
	{
#if defined(__linux__)	// macOS has no PDEATHSIG: a crashed client can leave the server running (CI round 1; runtime untested)
		prctl(PR_SET_PDEATHSIG, SIGTERM);	// the client gone (a crash too): the server goes
#endif
		if (getppid() != parent) { _exit(1); }
		dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		close(fds[1]);
		execv(exe.c_str(), argv.data());
		_exit(127);
	}
	close(fds[1]);
	if (pid_ < 0)
	{
		close(fds[0]);
		err = std::string("fork: ") + strerror(errno);
		return false;
	}
	{
		std::lock_guard<std::mutex> lk(m_);
		address_.clear();
		exited_.clear();
	}
	std::string line;
	for (const std::string& a : all) { line += (line.empty() ? "" : " ") + a; }
	printf("local server: pid %d: %s\n", (int)pid_, line.c_str());
	thread_ = std::thread(&LocalServer::reader, this, fds[0]);
	return true;
#endif
}

// The server's stdout, echoed with a prefix; the listen line gives the address.
void LocalServer::reader(int fd)
{
#ifdef KENSHI_EMBED_SERVER
	close(fd);	// no child pipe in the full build
	return;
#else
	FILE* f = fdopen(fd, "r");
	if (!f) { close(fd); return; }
	char buf[512];
	while (fgets(buf, sizeof buf, f))
	{
		buf[strcspn(buf, "\n")] = 0;
		printf("local server: %s\n", buf);
		if (strncmp(buf, "listening ", 10) == 0)
		{
			std::lock_guard<std::mutex> lk(m_);
			address_ = buf + 10;
		}
	}
	fclose(f);
#endif
}

bool LocalServer::running()
{
#ifdef KENSHI_EMBED_SERVER
	if (!embedded_) { return false; }
	if (!done_) { return true; }
	if (thread_.joinable()) { thread_.join(); }
	embedded_ = false;
	std::lock_guard<std::mutex> lk(m_);
	address_.clear();
	exited_ = "exited with status " + std::to_string(code_.load());
	printf("local server: %s\n", exited_.c_str());
	return false;
#else
	if (pid_ < 0) { return false; }
	int status = 0;
	if (waitpid(pid_, &status, WNOHANG) != pid_) { return true; }
	pid_ = -1;
	if (thread_.joinable()) { thread_.join(); }
	std::lock_guard<std::mutex> lk(m_);
	address_.clear();
	exited_ = WIFEXITED(status) ? "exited with status " + std::to_string(WEXITSTATUS(status)) : WIFSIGNALED(status) ? std::string("killed by signal ") + strsignal(WTERMSIG(status)) : "stopped";
	printf("local server: %s\n", exited_.c_str());
	return false;
#endif
}

std::string LocalServer::address() const
{
	std::lock_guard<std::mutex> lk(m_);
	return address_;
}

void LocalServer::stop(bool graceful)
{
#ifdef KENSHI_EMBED_SERVER
	if (embedded_)
	{
		stopFlag_ = true;
		if (thread_.joinable()) { thread_.join(); }
		embedded_ = false;
		printf("local server: in-process server stopped (exit code %d)\n", code_.load());
	}
#endif
	if (pid_ >= 0)
	{
		bool reaped = false;
		if (graceful)
		{
			kill(pid_, SIGTERM);
			for (int i = 0; i < 200 && !reaped; ++i)	// 2 s in 10 ms steps
			{
				const pid_t r = waitpid(pid_, nullptr, WNOHANG);
				reaped = r == pid_ || (r < 0 && errno != EINTR);	// reaped, or gone already
				if (!reaped) { usleep(10000); }
			}
		}
		if (!reaped)
		{
			kill(pid_, SIGKILL);
			while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
		}
		printf("local server: pid %d stopped%s\n", (int)pid_, reaped ? "" : " (killed)");
		pid_ = -1;
	}
	if (thread_.joinable()) { thread_.join(); }
	std::lock_guard<std::mutex> lk(m_);
	address_.clear();
}
