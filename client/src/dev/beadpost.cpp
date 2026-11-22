#include "beadpost.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
constexpr double kTimeoutSeconds = 15;	// a bd that has not answered in this is killed and its post reported failed
constexpr double kQuitSeconds = 20;	// stop(): the whole queue's drain at quit, in total (not per comment); the rest is dropped and counted

// The last non-empty line of bd's stderr (its error), trimmed, capped for one console line.
std::string errorLine(const std::string& err)
{
	std::string best;
	size_t at = 0;
	while (at < err.size())
	{
		size_t end = err.find('\n', at);
		if (end == std::string::npos) { end = err.size(); }
		const std::string line = err.substr(at, end - at);
		if (line.find_first_not_of(" \t\r") != std::string::npos) { best = line; }
		at = end + 1;
	}
	const size_t b = best.find_first_not_of(" \t\r");
	if (b == std::string::npos) { return {}; }
	best = best.substr(b, best.find_last_not_of(" \t\r") - b + 1);
	if (best.size() > 160) { best = best.substr(0, 157) + "..."; }
	return best;
}
}

BeadPost::~BeadPost()
{
	stop();
}

void BeadPost::start(const std::string& repo)
{
	if (running_) { return; }
	{
		std::lock_guard<std::mutex> lock(mu_);
		repo_ = repo.empty() ? "." : repo;
		quit_ = false;
	}
	running_ = true;
	thread_ = std::thread([this] { run(); });
}

void BeadPost::stop()
{
	if (!running_) { return; }
	{
		std::lock_guard<std::mutex> lock(mu_);
		quit_ = true;	// the queued comments go first (run() posts them, then returns); the whole drain is bounded by kQuitSeconds
		quitBy_ = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kQuitSeconds));
		dropped_ = 0;
	}
	cv_.notify_all();
	if (thread_.joinable()) { thread_.join(); }
	running_ = false;
	size_t dropped = 0;
	{
		std::lock_guard<std::mutex> lock(mu_);
		answers_.clear();	// nobody pumps any more
		dropped = dropped_;
	}
	if (dropped) { printf("bead: %zu queued comment(s) dropped at quit, past the %.0f s cap (the run file has the notes)\n", dropped, kQuitSeconds); }
}

void BeadPost::post(const std::string& item, const std::string& text, Done done)
{
	if (item.empty()) { if (done) { done(false, "no item id"); } return; }
	if (!running_)	// --bead without a started writer: say so where the caller can show it
	{
		if (done) { done(false, "the bd writer is not running"); }
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mu_);
		queue_.push_back({ item, text, std::move(done) });
	}
	cv_.notify_one();
}

void BeadPost::pump()
{
	std::deque<Answer> now;
	{
		std::lock_guard<std::mutex> lock(mu_);
		now.swap(answers_);
	}
	for (Answer& a : now)
	{
		printf("bead: comment on %s %s%s%s\n", a.item.c_str(), a.ok ? "posted" : "FAILED", a.why.empty() ? "" : ": ", a.why.c_str());
		if (a.done) { a.done(a.ok, a.why); }
	}
}

void BeadPost::run()
{
	std::string repo;
	{
		std::lock_guard<std::mutex> lock(mu_);
		repo = repo_;	// start() set it before the thread
	}
	for (;;)
	{
		Job job;
		double limit = kTimeoutSeconds;
		{
			std::unique_lock<std::mutex> lock(mu_);
			cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
			if (queue_.empty()) { return; }	// quit_ and nothing left to post
			if (quit_)	// the quit drain: one overall cap; what is left past it is dropped and counted
			{
				const double left = std::chrono::duration<double>(quitBy_ - std::chrono::steady_clock::now()).count();
				if (left <= 0) { dropped_ += queue_.size(); queue_.clear(); return; }
				limit = std::min(limit, left);
			}
			job = std::move(queue_.front());
			queue_.pop_front();
		}
		std::string why;
		const bool ok = runBd(repo, job.item, job.text, limit, why);
		std::lock_guard<std::mutex> lock(mu_);
		answers_.push_back({ job.item, std::move(job.done), ok, std::move(why) });
	}
}

// One child: bd -C <repo> --actor user comments add <item> <text>. Its stderr is kept for the failure's reason; stdin and stdout are
// /dev/null. False (and why) when bd is not on PATH, exits non-zero, or has not finished in kTimeoutSeconds.
bool BeadPost::runBd(const std::string& repo, const std::string& item, const std::string& text, double limitSeconds, std::string& why) const
{
	why.clear();
	int err[2];
	if (pipe(err) != 0) { why = std::string("cannot make a pipe: ") + std::strerror(errno); return false; }
	fcntl(err[0], F_SETFD, FD_CLOEXEC);	// the read end stays ours; the child gets only the dup2'd write end
	fcntl(err[1], F_SETFD, FD_CLOEXEC);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, err[1], 2);
	char* argv[] = { const_cast<char*>("bd"), const_cast<char*>("-C"), const_cast<char*>(repo.c_str()), const_cast<char*>("--actor"),
		const_cast<char*>("user"), const_cast<char*>("comments"), const_cast<char*>("add"), const_cast<char*>(item.c_str()),
		const_cast<char*>(text.c_str()), nullptr };
	pid_t pid = 0;
	const int spawned = posix_spawnp(&pid, "bd", &fa, nullptr, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(err[1]);
	if (spawned != 0)
	{
		close(err[0]);
		why = spawned == ENOENT ? std::string("bd not found on PATH") : std::string("cannot start bd: ") + std::strerror(spawned);
		return false;
	}
	int status = 0;
	const auto t0 = std::chrono::steady_clock::now();
	bool timedOut = false;
	for (;;)
	{
		const pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid) { break; }
		if (w < 0 && errno != EINTR) { why = std::string("waitpid: ") + std::strerror(errno); break; }
		if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= limitSeconds)
		{
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			timedOut = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	std::string out;
	{
		char buf[1024];
		for (;;)
		{
			const ssize_t n = read(err[0], buf, sizeof buf);
			if (n <= 0) { break; }
			if (out.size() < 8192) { out.append(buf, (size_t)std::min<ssize_t>(n, 8192 - (ssize_t)out.size())); }
		}
	}
	close(err[0]);
	if (timedOut) { why = "no answer in " + std::to_string((int)(limitSeconds + 0.5)) + " s"; return false; }
	if (!why.empty()) { return false; }
	const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	if (!ok)
	{
		if (WIFEXITED(status)) { why = "bd exited " + std::to_string(WEXITSTATUS(status)); }
		else { why = "bd was killed"; }
		const std::string line = errorLine(out);
		if (!line.empty()) { why += ": " + line; }
	}
	return ok;
}
