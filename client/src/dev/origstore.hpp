#pragma once
// The original's character positions, as the follow stream's `orig` records bring them (docs/FOLLOW_STREAM.md section 4; kenshi-cym.40.2.5).
// The follow stream (dev/followstream.cpp) is the only writer; the drift view (kenshi-cym.40.2.6) reads it and sends `drift`. Header-only, no game.
//
//   follow::origStore()                the one store of the process
//   put(id, sample)                    one record: the latest, plus a short history (kHistory samples, oldest dropped) kept in time order
//   latest(id)                         the newest sample, or nullptr
//   at(id, t, out)                     the position at game time t: interpolated between the two samples round t (yaw by the short way),
//                                      the nearer end held for up to kHold hours outside the history; false with no sample near t
//   ids()                              every id that has a sample
//   clear()                            a reconnect or a new session: the old positions are not the new run's
//   alignT() / hasAlign()              the `align` record: the original's clock (game hours) at the quick-save instant; the sim's imported clock is
//                                      the save's whole minutes, so it can sit up to a minute behind it: compare at matched time = the sim's t
//                                      minus (the sim's clock at the import minus alignT)
// Times are the original's game hours (`orig t=`, the same unit as `clock t=`). Not thread safe: the logic thread only.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace follow
{
	struct OrigSample
	{
		double t = 0;	// game hours, the original's clock
		double pos[3] = {};	// the logical point (body node), world units
		double yaw = 0;	// radians, CHAR_DYNAMIC's convention: 0 = +Z, toward (sin yaw, cos yaw)
		uint8_t state = 0;	// follow::State: 0 up (or unknown), 1 down, 2 ko, 3 dead, 4 carried
	};

	class OrigStore
	{
	public:
		static constexpr size_t kHistory = 8;	// about eight seconds at the stream's one record a second
		static constexpr double kHold = 5.0 / 3600.0;	// hours: how far past an end `at` still answers (5 s)

		void put(uint64_t id, const OrigSample& s)
		{
			std::deque<OrigSample>& h = hist_[id];
			if (!h.empty() && s.t < h.back().t)	// out of order: sorted in, a duplicate time replaces
			{
				const auto it = std::lower_bound(h.begin(), h.end(), s.t, [](const OrigSample& a, double t) { return a.t < t; });
				if (it != h.end() && it->t == s.t) { *it = s; } else { h.insert(it, s); }
			}
			else if (!h.empty() && s.t == h.back().t) { h.back() = s; }
			else { h.push_back(s); }
			while (h.size() > kHistory) { h.pop_front(); }
			puts_++;
		}
		const OrigSample* latest(uint64_t id) const
		{
			const auto it = hist_.find(id);
			return it == hist_.end() || it->second.empty() ? nullptr : &it->second.back();
		}
		bool at(uint64_t id, double t, OrigSample& out) const
		{
			const auto it = hist_.find(id);
			if (it == hist_.end() || it->second.empty()) { return false; }
			const std::deque<OrigSample>& h = it->second;
			if (t <= h.front().t) { if (h.front().t - t > kHold) { return false; } out = h.front(); out.t = t; return true; }
			if (t >= h.back().t) { if (t - h.back().t > kHold) { return false; } out = h.back(); out.t = t; return true; }
			const auto hi = std::lower_bound(h.begin(), h.end(), t, [](const OrigSample& a, double v) { return a.t < v; });
			const OrigSample& b = *hi;
			const OrigSample& a = *(hi - 1);
			const double f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0.0;
			out.t = t;
			for (int k = 0; k < 3; k++) { out.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * f; }
			double dy = std::fmod(b.yaw - a.yaw, 2 * M_PI);
			if (dy > M_PI) { dy -= 2 * M_PI; } else if (dy < -M_PI) { dy += 2 * M_PI; }
			out.yaw = a.yaw + dy * f;
			out.state = f < 0.5 ? a.state : b.state;
			return true;
		}
		std::vector<uint64_t> ids() const
		{
			std::vector<uint64_t> r;
			for (const auto& [id, h] : hist_) { if (!h.empty()) { r.push_back(id); } }
			std::sort(r.begin(), r.end());
			return r;
		}
		size_t size() const { return hist_.size(); }
		uint64_t puts() const { return puts_; }	// records stored since the start (a change counter for the drift view)
		void clear() { hist_.clear(); }
		void setAlign(double t) { align_ = t; hasAlign_ = true; }
		bool hasAlign() const { return hasAlign_; }
		double alignT() const { return align_; }
		void clearAlign() { hasAlign_ = false; }

	private:
		std::unordered_map<uint64_t, std::deque<OrigSample>> hist_;
		uint64_t puts_ = 0;
		double align_ = 0;
		bool hasAlign_ = false;
	};

	inline OrigStore& origStore() { static OrigStore s; return s; }
}
