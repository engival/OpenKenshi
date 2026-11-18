#include "eventlog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <vector>
#include <unistd.h>

namespace evlog {

namespace {

struct Event
{
	double t;		// steady seconds since the first event or open()
	std::time_t wall;
	int ms;
	double tick;
	uint64_t id;
	std::string kind, text;
};

std::mutex mu;
std::vector<Event> ring;	// kCapacity slots once full; head = the next to overwrite
size_t head = 0;
std::string runPath;
FILE* run = nullptr;
const auto t0 = std::chrono::steady_clock::now();

void print(FILE* f, const Event& e)
{
	char when[16];
	std::strftime(when, sizeof when, "%H:%M:%S", std::localtime(&e.wall));
	char tick[32] = "-";
	if (e.tick >= 0) { snprintf(tick, sizeof tick, "%.1f", e.tick); }
	fprintf(f, "%s.%03d +%.3fs tick %s %s", when, e.ms, e.t, tick, e.kind.c_str());
	if (e.id) { fprintf(f, " id %llu", (unsigned long long)e.id); }
	fprintf(f, ": %s\n", e.text.c_str());
}

}

void open(const std::string& dir, const std::string& first)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::create_directories(dir, ec);
	std::vector<fs::path> old;
	for (const fs::directory_entry& d : fs::directory_iterator(dir, ec))
	{
		const std::string n = d.path().filename().string();
		if (n.rfind("events-", 0) == 0 && d.path().extension() == ".txt") { old.push_back(d.path()); }
	}
	std::sort(old.begin(), old.end());	// the names sort by date
	for (size_t i = 0; i + 19 < old.size(); i++) { fs::remove(old[i], ec); }
	const std::time_t now = std::time(nullptr);
	char name[64];
	std::strftime(name, sizeof name, "events-%Y%m%d-%H%M%S", std::localtime(&now));
	const std::string path = (fs::path(dir) / (std::string(name) + "-" + std::to_string(getpid()) + ".txt")).string();
	std::lock_guard<std::mutex> lock(mu);
	run = fopen(path.c_str(), "w");
	if (!run) { fprintf(stderr, "kclient: events: cannot write %s\n", path.c_str()); return; }
	runPath = path;
	if (!first.empty()) { fprintf(run, "%s\n", first.c_str()); }	// the build line, before any event (k16.2.127)
	const size_t n = ring.size();
	for (size_t i = 0; i < n; i++) { print(run, ring[(head + i) % n]); }	// the events before open()
	fflush(run);
}

const std::string& runFile() { return runPath; }

void add(const char* kind, double tick, uint64_t id, const char* fmt, ...)
{
	char text[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	const auto sys = std::chrono::system_clock::now();
	Event e{ std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), std::chrono::system_clock::to_time_t(sys),
		(int)(std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count() % 1000), tick, id, kind, text };
	std::lock_guard<std::mutex> lock(mu);
	if (run) { print(run, e); fflush(run); }
	if (ring.size() < kCapacity) { ring.push_back(std::move(e)); }
	else
	{
		ring[head] = std::move(e);
		head = (head + 1) % kCapacity;
	}
}

size_t write(FILE* f, double seconds)
{
	std::lock_guard<std::mutex> lock(mu);
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	const size_t n = ring.size();
	size_t count = 0;
	for (size_t i = 0; i < n; i++)
	{
		const Event& e = ring[(head + i) % n];
		if (seconds > 0 && e.t < now - seconds) { continue; }
		print(f, e);
		count++;
	}
	return count;
}

bool writeFile(const std::string& path, double seconds, size_t& count, const std::string& first)
{
	count = 0;
	FILE* f = fopen(path.c_str(), "w");
	if (!f) { return false; }
	if (!first.empty()) { fprintf(f, "%s\n", first.c_str()); }	// the build line, before any event (k16.2.127)
	count = write(f, seconds);
	if (count == 0) { fprintf(f, "(no events in the last %.0f s)\n", seconds); }
	fclose(f);
	return true;
}

}
