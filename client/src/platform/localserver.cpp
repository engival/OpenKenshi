#include "platform/localserver.hpp"

#include <cstdio>
#include <cstring>

#ifdef KENSHI_EMBED_SERVER
#include "ksim/server_main.hpp"
#include <functional>
#include <ostream>
#include <streambuf>
#endif

LocalServer::~LocalServer() { stop(); }

// The startup stages, from the server's own lines (cpp/src/ksim/server_main.cpp): game data, world ready, dialogue book ready, listening.
void LocalServer::line(const char* l)
{
	if (strncmp(l, "listening ", 10) == 0) { printf("local server: %s\n", l); }
	else { fprintf(stderr, "%s\n", l); }	// stderr lines unchanged: the logs stay as they were
	std::lock_guard<std::mutex> lk(m_);
	if (*l && strncmp(l, "listening ", 10) != 0)
	{
		last_ = strncmp(l, "ksim_server: ", 12) == 0 ? l + 12 : l;
		last_.erase(0, last_.find_first_not_of(' '));	// the server's error text can carry a leading space
	}
	// Only ever raise the step: every build prints "sim data", only the release server (runtime --data) also prints "game data"; the first of them is step 2.
	const auto at = [this](int step, const char* text) { if (step > step_) { step_ = step; stageText_ = text; } };
	if (strncmp(l, "listening ", 10) == 0) { address_ = l + 10; at(4, "Opening the connection"); }
	else if (strncmp(l, "ksim_server: game data ", 23) == 0 || strncmp(l, "ksim_server: sim data ", 22) == 0) { at(2, "Reading the game data"); }
	else if (strncmp(l, "ksim_server: world ready", 24) == 0) { at(3, "Loading the dialogue"); }
	else if (strncmp(l, "ksim_server: dialogue book ready", 32) == 0) { at(4, "Opening the connection"); }
}

std::string LocalServer::lastLine() const
{
	std::lock_guard<std::mutex> lk(m_);
	return last_;
}

ServerStage LocalServer::stage() const
{
	std::lock_guard<std::mutex> lk(m_);
	ServerStage s;
	s.text = stageText_;
	s.step = step_;
	if (step_ > 0) { s.seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0_).count(); }
	return s;
}

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
		t0_ = std::chrono::steady_clock::now();
		step_ = 1;
		stageText_ = "Starting the server";
		last_.clear();
	}
	std::vector<std::string> all = { "OpenKenshi-server", "--listen", "127.0.0.1:0" };
	all.insert(all.end(), args.begin(), args.end());
	std::vector<char*> argv;
	for (std::string& a : all) { argv.push_back(a.data()); }
	argv.push_back(nullptr);
	ksim::ServerConfig config;	// log: the client's stderr; out: below; beforeRun stays empty (it installs the tool's SIGINT/SIGTERM handlers)
	if (const int rc = ksim::parseServerArgs((int)all.size(), argv.data(), config); rc >= 0) { err = "the server options were refused (exit code " + std::to_string(rc) + ")"; return false; }
	std::string cmd;
	for (const std::string& a : all) { cmd += (cmd.empty() ? "" : " ") + a; }
	printf("local server: in process (thread): %s\n", cmd.c_str());
	stopFlag_ = false;
	done_ = false;
	embedded_ = true;
	thread_ = std::thread([this, config]() mutable
	{
		LineBuf buf([this](const std::string& l) { line(l.c_str()); });	// its out and log streams: the same lines the child's merged pipe carries
		std::ostream out(&buf);
		config.out = &out;
		config.log = &out;
		code_ = ksim::runServer(config, stopFlag_);
		out.flush();
		done_ = true;
	});
	return true;
#else
	std::vector<std::string> all = { exe, "--listen", "127.0.0.1:0" };
	all.insert(all.end(), args.begin(), args.end());
	FILE* out = nullptr;
	if (!plat::spawnPiped(all, child_, out, err)) { return false; }
	{
		std::lock_guard<std::mutex> lk(m_);
		address_.clear();
		exited_.clear();
		t0_ = std::chrono::steady_clock::now();
		step_ = 1;
		stageText_ = "Starting the server";
		last_.clear();
	}
	std::string cmd;
	for (const std::string& a : all) { cmd += (cmd.empty() ? "" : " ") + a; }
	printf("local server: pid %lld: %s\n", child_.pid, cmd.c_str());
	thread_ = std::thread(&LocalServer::reader, this, out);
	return true;
#endif
}

// The server's stdout and stderr (one pipe): every line echoed, the stage lines and the listen line recorded.
void LocalServer::reader(FILE* f)
{
#ifdef KENSHI_EMBED_SERVER
	(void)f;	// no child pipe in the full build
	return;
#else
	if (!f) { return; }
	char buf[512];
	while (fgets(buf, sizeof buf, f))
	{
		buf[strcspn(buf, "\n")] = 0;
		line(buf);
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
	if (child_.pid < 0) { return false; }
	std::string how;
	if (plat::childRunning(child_, how)) { return true; }
	if (thread_.joinable()) { thread_.join(); }
	std::lock_guard<std::mutex> lk(m_);
	address_.clear();
	exited_ = how;
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
	if (child_.pid >= 0)
	{
		const long long pid = child_.pid;
		const bool reaped = plat::stopChild(child_, graceful);
		printf("local server: pid %lld stopped%s\n", pid, reaped ? "" : " (killed)");
	}
	if (thread_.joinable()) { thread_.join(); }
	std::lock_guard<std::mutex> lk(m_);
	address_.clear();
}
