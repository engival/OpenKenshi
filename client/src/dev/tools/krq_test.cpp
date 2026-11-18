// The render-side resource job queue's unit check (renderqueue.hpp, docs/RENDER_THREAD.md §7.2): no sokol, no window.
//   krq_test   exit 0 and "krq_test: ok" when every check holds
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "renderqueue.hpp"

namespace
{
int gFails = 0;

void check(bool ok, const char* what)
{
	if (!ok) { fprintf(stderr, "krq_test: FAIL %s\n", what); gFails++; }
}
}

int main()
{
	std::vector<std::string> log;
	const auto job = [&](const char* s) { return [&log, s] { log.push_back(s); }; };

	// Jobs queued before the first packet belong to serial 0 and run on the first drain.
	rq::push("t", job("setup"));
	const uint64_t s1 = rq::beginPacket();
	rq::push("t", job("a1"));
	rq::push("t", job("a2"));
	rq::drain(s1);
	check(log == std::vector<std::string>{ "setup", "a1", "a2" }, "frame lane: submit order, setup jobs first");

	// A redraw of the same serial runs nothing; a job queued for the next packet waits for that packet's draw (overlap).
	log.clear();
	const uint64_t s2 = rq::beginPacket();
	rq::push("t", job("b"));
	rq::drain(s1);
	check(log.empty(), "frame lane: a job for packet N+1 does not run at the (re)draw of packet N");
	rq::drain(s2);
	check(log == std::vector<std::string>{ "b" }, "frame lane: it runs at the draw of N+1");

	// The frame lane ignores the budget; the stream lane runs under it, at least one job per drain, in order.
	log.clear();
	rq::setStreamBudget(100);
	const uint64_t s3 = rq::beginPacket();
	rq::push("t", job("big-frame"), rq::Lane::kFrame, 1000);
	rq::push("t", job("s1"), rq::Lane::kStream, 60);
	rq::push("t", job("s2"), rq::Lane::kStream, 60);
	rq::push("t", job("s3"), rq::Lane::kStream, 30);
	rq::push("t", job("s4"), rq::Lane::kStream, 500);
	rq::drain(s3);
	check(log == std::vector<std::string>{ "big-frame", "s1" }, "stream lane: the budget stops at s2 (60 + 60 > 100)");
	log.clear();
	rq::drain(s3);
	check(log == std::vector<std::string>{ "s2", "s3" }, "stream lane: the next drain takes s2 + s3 (90)");
	log.clear();
	rq::drain(s3);
	check(log == std::vector<std::string>{ "s4" }, "stream lane: one job over the budget still runs alone");
	check(rq::stats().pendingStream == 0 && rq::stats().deferredFrames == 2, "stats: nothing pending, two deferring drains");

	// The stream lane's time budget (kenshi-k16.2.205.12.74): a job runs while the lane's time so far plus its predicted cost (4 MiB at the starting
	// 0.4 ms per MiB: 1.6 ms) fits in 10 ms. Each job takes 6 ms: the first always runs, the second starts at 6 + 1.6, the third would at 12 + ~2.
	log.clear();
	rq::setStreamBudget(64u << 20);
	rq::setStreamTimeBudget(10.0);
	const auto slow = [&](const char* s) { return [&log, s] { std::this_thread::sleep_for(std::chrono::milliseconds(6)); log.push_back(s); }; };
	for (const char* n : { "t1", "t2", "t3", "t4" }) { rq::push("t", slow(n), rq::Lane::kStream, 4u << 20); }
	rq::drain(s3);
	check(log == std::vector<std::string>{ "t1", "t2" }, "stream lane: the time budget stops at t3");
	log.clear();
	rq::setStreamTimeBudget(0);
	rq::drain(s3);
	check(log == std::vector<std::string>{ "t3", "t4" }, "stream lane: time budget 0 is bytes only");

	// A Handle is published by the job (render side) and seen ready by logic; pushes from another thread are ordered by the mutex.
	rq::Handle<int> h;
	check(!h.ready(), "handle: pending before its job ran");
	std::thread logic([&] { rq::push("t", [h] { h.publish(42); }, rq::Lane::kStream, 1); });
	logic.join();
	rq::drain(s3);
	check(h.ready() && h.get() == 42, "handle: ready with the published value after the drain");

	// flushAll runs both lanes whatever the serial and the budget, and jobs that a job queues.
	log.clear();
	rq::push("t", job("f1"), rq::Lane::kStream, 1000);
	rq::push("t", job("f2"), rq::Lane::kStream, 1000);
	rq::push("t", [&] { log.push_back("f3"); rq::push("t", job("f4")); });
	rq::flushAll();
	check(log == std::vector<std::string>{ "f3", "f1", "f2", "f4" }, "flushAll: frame lane, stream lane, then the jobs they queued");
	check(rq::stats().pendingFrame == 0 && rq::stats().pendingStream == 0, "flushAll: empty after");

	if (gFails) { fprintf(stderr, "krq_test: %d failed\n", gFails); return 1; }
	printf("krq_test: ok\n");
	return 0;
}
