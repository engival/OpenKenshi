#pragma once
// A small persistent parallel-for (kenshi-k16.2.205.12.102, overlap review .85 B2 / T22): workers parked on a condition variable, woken per
// run(), the calling thread working too. For per-frame loops whose items are independent (Characters::update's pose sampling): no thread is
// made per frame. Deterministic by construction when f(i) writes only item i's state: which thread runs an item changes nothing in its output.
// run() returns when every item is done; an exception from f is caught per item and the one of the lowest index is rethrown on the caller.
// A worker that wakes after the caller has finished the items does not join the run (the caller never waits on a late wake).
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class WorkPool
{
public:
	explicit WorkPool(int workers, const char* name = "kpool");	// workers: threads beside the caller (0: run() is a plain loop)
	~WorkPool();
	WorkPool(const WorkPool&) = delete;
	WorkPool& operator=(const WorkPool&) = delete;
	int workers() const { return (int)threads_.size(); }
	// f(i) for every i in [0, n), on the caller and the workers. epilogue: run once on each worker that took part, after its last item (the
	// caller's own thread does not run it): for handing a worker's thread-local measurements to the caller.
	void run(size_t n, const std::function<void(size_t)>& f, const std::function<void()>& epilogue = {});

private:
	void loop();
	void work();
	std::vector<std::thread> threads_;
	std::mutex m_;
	std::condition_variable wake_, done_;
	unsigned long long gen_ = 0;
	bool open_ = false;	// the current run still takes workers (closed by the caller once it ran out of items)
	bool stop_ = false;
	int inflight_ = 0;	// workers inside the current run
	const std::function<void(size_t)>* f_ = nullptr;
	const std::function<void()>* epilogue_ = nullptr;
	size_t n_ = 0;
	std::atomic<size_t> next_{ 0 };
	std::mutex errM_;
	size_t errIndex_ = 0;
	std::exception_ptr err_;
};
