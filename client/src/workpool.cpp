#include "workpool.hpp"

#include <cstdio>
#if defined(__linux__)
#include <pthread.h>
#endif

WorkPool::WorkPool(int workers, const char* name)
{
	for (int t = 0; t < workers; t++)
	{
		threads_.emplace_back([this] { loop(); });
#if defined(__linux__)
		char nm[16];
		snprintf(nm, sizeof nm, "%.11s%d", name, t);
		pthread_setname_np(threads_.back().native_handle(), nm);
#else
		(void)name;
#endif
	}
}

WorkPool::~WorkPool()
{
	{
		std::lock_guard<std::mutex> lock(m_);
		stop_ = true;
	}
	wake_.notify_all();
	for (std::thread& t : threads_) { t.join(); }
}

void WorkPool::work()
{
	for (;;)
	{
		const size_t i = next_.fetch_add(1, std::memory_order_relaxed);
		if (i >= n_) { return; }
		try { (*f_)(i); }
		catch (...)
		{
			std::lock_guard<std::mutex> lock(errM_);
			if (!err_ || i < errIndex_) { err_ = std::current_exception(); errIndex_ = i; }
		}
	}
}

void WorkPool::loop()
{
	unsigned long long seen = 0;
	std::unique_lock<std::mutex> lock(m_);
	for (;;)
	{
		wake_.wait(lock, [&] { return stop_ || gen_ != seen; });
		if (stop_) { return; }
		seen = gen_;
		if (!open_) { continue; }	// a late wake: the caller already finished this run's items
		inflight_++;
		lock.unlock();
		work();
		if (epilogue_ && *epilogue_) { (*epilogue_)(); }
		lock.lock();
		if (--inflight_ == 0) { done_.notify_one(); }
	}
}

void WorkPool::run(size_t n, const std::function<void(size_t)>& f, const std::function<void()>& epilogue)
{
	if (n == 0) { return; }
	if (threads_.empty() || n == 1)
	{
		for (size_t i = 0; i < n; i++) { f(i); }
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_);
		f_ = &f;
		epilogue_ = &epilogue;
		n_ = n;
		next_.store(0, std::memory_order_relaxed);
		err_ = nullptr;
		open_ = true;
		gen_++;
	}
	wake_.notify_all();
	work();
	std::exception_ptr err;
	{
		std::unique_lock<std::mutex> lock(m_);
		open_ = false;
		done_.wait(lock, [&] { return inflight_ == 0; });
		f_ = nullptr;
		epilogue_ = nullptr;
		err = err_;
		err_ = nullptr;
	}
	if (err) { std::rethrow_exception(err); }
}
