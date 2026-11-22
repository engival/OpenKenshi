#pragma once
// The bd writer behind a playtest run's --bead (kenshi-k16.2.127): each note, the verdict and each F12 shot of the run becomes one
// comment on that beads item. The posts go out on a worker thread, one `bd -C <repo> --actor user comments add <item> <text>` child
// process each, so a frame never waits on bd (a desktop dolt call, a missing bd or a hung one). The text is one argv element, never a
// shell. The answers (posted, or why not) come back on the frame thread through pump().
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class BeadPost
{
public:
	// ok: the comment landed on the item. why: "" or what went wrong ("bd not found", "bd exited 2: <its last error line>", "no answer in 15 s").
	using Done = std::function<void(bool ok, const std::string& why)>;

	~BeadPost();
	// repo: the -C directory, the kenshi repo the client's playtests/ live in. Starts the thread; a second call is a no-op.
	void start(const std::string& repo);
	// The run is over: the queued posts go first, all of them within one 20 s cap (a post's own timeout is cut to what is left); what is
	// still queued past the cap is dropped and counted on stdout (their texts are in the run file). Answers never delivered are dropped
	// too: pump() after this does nothing.
	void stop();
	void post(const std::string& item, const std::string& text, Done done);	// queues one comment (the text is copied)
	void pump();	// the frame thread: run the answers that came in since the last pump
	bool started() const { return running_; }

private:
	struct Job
	{
		std::string item, text;
		Done done;
	};
	struct Answer
	{
		std::string item;
		Done done;
		bool ok = false;
		std::string why;
	};

	void run();	// the worker thread: the queue, one bd child per post
	bool runBd(const std::string& repo, const std::string& item, const std::string& text, double limitSeconds, std::string& why) const;

	std::string repo_;
	std::thread thread_;
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::deque<Job> queue_;
	std::deque<Answer> answers_;
	bool quit_ = false;
	std::chrono::steady_clock::time_point quitBy_;	// stop(): the drain's end, 20 s after the call (guarded by mu_)
	size_t dropped_ = 0;	// posts left unposted at that end (guarded by mu_)
	bool running_ = false;
};
