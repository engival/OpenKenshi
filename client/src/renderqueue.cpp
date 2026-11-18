#include "renderqueue.hpp"
#include "cpustage.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <utility>
#include <cstdio>
#include <ctime>
#include <vector>

// docs/RENDER_THREAD.md §7.2. One mutex guards both lanes; jobs run outside it, so a job may queue another.
namespace rq
{
namespace
{
	struct Job
	{
		uint64_t serial;
		const char* tag;
		std::function<void()> fn;
		size_t bytes;
	};
	std::mutex gMu;
	std::deque<Job> gFrame, gStream;
	uint64_t gSerial = 0;	// the packet logic is filling
	size_t gBudget = 8u << 20;
	double gTimeBudgetMs = 4.0;	// the stream lane's time per drain (setStreamTimeBudget; render.stream_ms)
	// the predicted cost of a stream job: its bytes at this rate (ms per byte), a running mean of the creates' own (jobs of kRateMinBytes and up:
	// the small ones are fixed overhead). Start: 0.4 ms per MiB, about a 4096^2 BC3 create's on the Intel test card
	double gMsPerByte = 0.4 / (1 << 20);
	constexpr size_t kRateMinBytes = 256u << 10;
	constexpr double kRateBlend = 0.25;
	Stats gStats;
}

void push(const char* tag, std::function<void()> job, Lane lane, size_t bytes)
{
	std::lock_guard<std::mutex> lk(gMu);
	(lane == Lane::kFrame ? gFrame : gStream).push_back({ gSerial, tag, std::move(job), bytes });
}

uint64_t beginPacket()
{
	std::lock_guard<std::mutex> lk(gMu);
	return ++gSerial;
}

void drain(uint64_t serial)
{
	std::vector<Job> run;
	size_t eligible = 0;	// the stream jobs this drain may take: those queued before it, up to the drawn packet's serial (a job's own pushes wait)
	double budgetMs = 0, rate = 0;
	size_t budget = 0;
	{
		std::lock_guard<std::mutex> lk(gMu);
		while (!gFrame.empty() && gFrame.front().serial <= serial)
		{
			run.push_back(std::move(gFrame.front()));
			gFrame.pop_front();
		}
		gStats.frameJobs += run.size();
		// the stream lane is gated by serial too: a job queued while packet N+1 is filled never runs on a redraw of N (§7.1 rule 4), so a destroy
		// that logic queued once N+1 stopped naming a resource cannot pull it from under N (on one thread every job is the drawn packet's or older)
		while (eligible < gStream.size() && gStream[eligible].serial <= serial) { eligible++; }
		budgetMs = gTimeBudgetMs;
		rate = gMsPerByte;
		budget = gBudget;
	}
	for (Job& j : run)	// the frame lane first, all of it, in submit order
	{
		CPUSTAGE_ACC(j.tag);	// the job's time under its tag (a static string): +buildings.texture, +character-texture (kenshi-k16.2.205.12.74)
		j.fn();
	}
	// Then the stream lane, in submit order, one job at a time under two budgets (kenshi-k16.2.205.12.74): the bytes (gBudget) and the time. A job runs
	// only when the lane's time so far plus its predicted cost (its bytes at the measured ms per byte) fits in gTimeBudgetMs; the first job always runs,
	// so the lane never stalls and one job over the budget runs alone. Measured (findings/kenshi-k16.2.205.12.74): a create costs about its bytes
	// (2048^2 BC3, 5.6 MB: 1-1.7 ms; 4096^2, 22 MB: 3-10 ms), and an 8 MB byte cap alone let a frame take 16-24 ms of mesh and texture creates.
	using Clock = std::chrono::steady_clock;
	const Clock::time_point t0 = Clock::now();
	size_t streamRun = 0, bytes = 0;
	double measuredMs = 0;
	size_t measuredBytes = 0;
	for (; streamRun < eligible; streamRun++)
	{
		Job j;
		{
			std::lock_guard<std::mutex> lk(gMu);
			Job& f = gStream.front();
			if (streamRun > 0)
			{
				if (bytes + f.bytes > budget) { break; }
				const double spent = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
				if (budgetMs > 0 && spent + (double)f.bytes * rate > budgetMs) { break; }
			}
			j = std::move(f);
			gStream.pop_front();
		}
		bytes += j.bytes;
		CPUSTAGE_ACC(j.tag);
		const Clock::time_point a = Clock::now();
#ifdef KCLIENT_DEV_TOOLS
		// wall vs this thread's CPU time per job (kenshi-k16.2.205.12.75): wall far above cpu = preempted or waiting; jobs of 2 ms wall and up go on the frame's stage line
		timespec c0{}, c1{};
		const bool timed = cpustage::on();
		if (timed) { clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0); }
		j.fn();
		if (timed)
		{
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
			const double wall = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
			const double cpu = (double)(c1.tv_sec - c0.tv_sec) * 1e3 + (double)(c1.tv_nsec - c0.tv_nsec) * 1e-6;
			if (wall >= 2.0)
			{
				char b[128];
				snprintf(b, sizeof b, " | rqjob %s %.1fMB wall %.1f cpu %.1f", j.tag, (double)j.bytes / (1 << 20), wall, cpu);
				cpustage::extra(b);
			}
		}
#else
		j.fn();
#endif
		if (j.bytes >= kRateMinBytes)
		{
			measuredMs += std::chrono::duration<double, std::milli>(Clock::now() - a).count();
			measuredBytes += j.bytes;
		}
	}
	std::lock_guard<std::mutex> lk(gMu);
	gStats.streamJobs += streamRun;
	gStats.streamBytes += bytes;
	if (streamRun < eligible) { gStats.deferredFrames++; }
	if (measuredBytes > 0) { gMsPerByte += kRateBlend * (measuredMs / (double)measuredBytes - gMsPerByte); }	// a running mean of the create rate
}

void flushAll()
{
	for (;;)
	{
		std::deque<Job> f, s;
		{
			std::lock_guard<std::mutex> lk(gMu);
			if (gFrame.empty() && gStream.empty()) { return; }
			f.swap(gFrame);
			s.swap(gStream);
			gStats.frameJobs += f.size();
			gStats.streamJobs += s.size();
		}
		for (Job& j : f) { j.fn(); }
		for (Job& j : s) { j.fn(); }
	}
}

void setStreamBudget(size_t bytesPerFrame)
{
	std::lock_guard<std::mutex> lk(gMu);
	gBudget = bytesPerFrame;
}

void setStreamTimeBudget(double ms)
{
	std::lock_guard<std::mutex> lk(gMu);
	gTimeBudgetMs = ms;
}

Stats stats()
{
	std::lock_guard<std::mutex> lk(gMu);
	Stats s = gStats;
	s.pendingFrame = gFrame.size();
	s.pendingStream = gStream.size();
	return s;
}
}
