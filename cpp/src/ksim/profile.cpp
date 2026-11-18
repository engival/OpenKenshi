#include "ksim/profile.hpp"

#if KSIM_PROFILE

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace ksim::profile {

namespace {

std::uint64_t nsBetween(Profiler::Clock::time_point from, Profiler::Clock::time_point to) noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
}

double percentile(std::vector<std::uint64_t>& sorted, double fraction)
{
	return static_cast<double>(sorted[static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1))]) / 1000.0;
}

} // namespace

Profiler::Profiler() : ring_(kWindow)
{
	if (const char* seconds = std::getenv("KSIM_PROFILE_JSON"); seconds != nullptr)
		jsonSeconds_ = std::max(0.0, std::atof(seconds));
	if (const char* path = std::getenv("KSIM_PROFILE_TICKS"); path != nullptr && *path != 0 && (ticksFile_.reset(std::fopen(path, "w"), [](std::FILE* file) { if (file != nullptr) std::fclose(file); }), ticksFile_ != nullptr))
	{
		std::fprintf(ticksFile_.get(), "tick,tick_us");
		for (const char* name : kSectionNames)
			std::fprintf(ticksFile_.get(), ",%s", name);
		for (const char* name : kSubNames)
			std::fprintf(ticksFile_.get(), ",sub_%s", name);
		std::fprintf(ticksFile_.get(), ",characters,squads,path_queries,path_us\n");
	}
}


void Profiler::begin() noexcept
{
	tickStart_ = last_ = subLast_ = Clock::now();
}

void Profiler::mark(Section section) noexcept
{
	const Clock::time_point now = Clock::now();
	pending_[static_cast<std::size_t>(section)] += nsBetween(last_, now);
	last_ = subLast_ = now;
}

void Profiler::end(std::uint64_t tick, std::uint32_t characters, std::uint32_t squads, std::uint64_t navQueries, std::uint64_t navNs)
{
	const Clock::time_point now = Clock::now();
	TickSample& sample = ring_[next_];
	sample.tick = tick;
	sample.tickNs = nsBetween(tickStart_, now);
	sample.ns = pending_;
	sample.characters = characters;
	sample.squads = squads;
	sample.pathQueries = static_cast<std::uint32_t>(navQueries - navQueries_);
	sample.pathNs = navNs - navNs_;
	navQueries_ = navQueries;
	navNs_ = navNs;
	if (ticksFile_)
	{
		std::fprintf(ticksFile_.get(), "%llu,%.1f", static_cast<unsigned long long>(tick), static_cast<double>(sample.tickNs) / 1000.0);
		for (const std::uint64_t ns : sample.ns)
			std::fprintf(ticksFile_.get(), ",%.1f", static_cast<double>(ns) / 1000.0);
		for (const std::uint64_t ns : subPending_)
			std::fprintf(ticksFile_.get(), ",%.1f", static_cast<double>(ns) / 1000.0);
		std::fprintf(ticksFile_.get(), ",%u,%u,%u,%.1f\n", characters, squads, sample.pathQueries, static_cast<double>(sample.pathNs) / 1000.0);
		if (tick % 200 == 0)
			std::fflush(ticksFile_.get());
		subPending_.fill(0);
	}
	pending_.fill(0);
	next_ = (next_ + 1) % kWindow;
	filled_ = std::min(filled_ + 1, kWindow);
	++totals_.ticks;
	totals_.tickNs += sample.tickNs;
	for (std::size_t i = 0; i < kSections; ++i)
		totals_.ns[i] += sample.ns[i];
	totals_.pathQueries += sample.pathQueries;
	totals_.pathNs += sample.pathNs;
	if (jsonSeconds_ > 0.0)
	{
		if (jsonLast_ == Clock::time_point{})
			jsonLast_ = now;
		if (std::chrono::duration<double>(now - jsonLast_).count() >= jsonSeconds_)
		{
			jsonLast_ = now;
			std::fprintf(stderr, "%s\n", json().c_str());
		}
	}
}

Summary Profiler::summary() const
{
	Summary out;
	out.ticks = filled_;
	if (filled_ == 0)
		return out;
	std::vector<std::uint64_t> totalsNs;
	totalsNs.reserve(filled_);
	std::array<double, kSections> sum{};
	double characters = 0.0;
	double squads = 0.0;
	double paths = 0.0;
	double pathNs = 0.0;
	double tickNs = 0.0;
	for (std::size_t n = 0; n < filled_; ++n)
	{
		const TickSample& sample = ring_[n];
		totalsNs.push_back(sample.tickNs);
		tickNs += static_cast<double>(sample.tickNs);
		for (std::size_t i = 0; i < kSections; ++i)
		{
			sum[i] += static_cast<double>(sample.ns[i]);
			out.maxUs[i] = std::max(out.maxUs[i], static_cast<double>(sample.ns[i]) / 1000.0);
		}
		characters += sample.characters;
		squads += sample.squads;
		paths += sample.pathQueries;
		pathNs += static_cast<double>(sample.pathNs);
	}
	const double count = static_cast<double>(filled_);
	for (std::size_t i = 0; i < kSections; ++i)
		out.meanUs[i] = sum[i] / count / 1000.0;
	out.tickMeanUs = tickNs / count / 1000.0;
	std::sort(totalsNs.begin(), totalsNs.end());
	out.tickP50Us = percentile(totalsNs, 0.5);
	out.tickP99Us = percentile(totalsNs, 0.99);
	out.tickMaxUs = static_cast<double>(totalsNs.back()) / 1000.0;
	out.characters = characters / count;
	out.squads = squads / count;
	out.pathQueries = paths / count;
	out.pathUs = pathNs / count / 1000.0;
	return out;
}

std::string Profiler::json() const
{
	const Summary s = summary();
	std::ostringstream out;
	out.setf(std::ios::fixed);
	out.precision(1);
	out << "{\"profile\":1,\"tick\":" << (filled_ != 0 ? ring_[(next_ + kWindow - 1) % kWindow].tick : 0) << ",\"window\":" << s.ticks << ",\"tick_us\":{\"mean\":" << s.tickMeanUs
		<< ",\"p50\":" << s.tickP50Us << ",\"p99\":" << s.tickP99Us << ",\"max\":" << s.tickMaxUs << "},\"section_mean_us\":{";
	for (std::size_t i = 0; i < kSections; ++i)
		out << (i != 0 ? "," : "") << '"' << kSectionNames[i] << "\":" << s.meanUs[i];
	out << "},\"counts\":{\"characters\":" << s.characters << ",\"squads\":" << s.squads << ",\"path_queries\":" << s.pathQueries << ",\"path_us\":" << s.pathUs << "}}";
	return out.str();
}

} // namespace ksim::profile

#endif
