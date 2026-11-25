#include "ksim/ai/perception.hpp"

#include "ksim/core/rng.hpp"

#include <algorithm>
#include <cmath>

namespace ksim::perception {

using state::Fighter;
using state::SightRecord;

namespace {

double clamp01(double v) noexcept
{
	return std::clamp(v, 0.0, 1.0);
}

double unit01(std::uint64_t z) noexcept
{
	return static_cast<double>(splitMix64(z) >> 11) * (1.0 / 9007199254740992.0);
}

// The target-side weather factor of the noise range: dust and rain make a character harder to hear.
double noiseWeather(const Fighter& target, const Environment& env) noexcept
{
	if (target.animal || env.weatherType == 0)
		return 1.0;
	const double k = env.weatherType == 1 ? 0.25 : (env.weatherType == 5 || env.weatherType == 2) ? kRainFactor : 1.0;
	return 1.0 + (k - 1.0) * env.weatherStrength;
}

SightRecord* findMutable(Fighter& f, std::uint64_t target) noexcept
{
	const auto it = std::lower_bound(f.sight.begin(), f.sight.end(), target,
		[](const SightRecord& r, std::uint64_t id) { return r.target < id; });
	return it != f.sight.end() && it->target == target ? &*it : nullptr;
}

SightRecord& obtain(Fighter& f, std::uint64_t target)
{
	const auto it = std::lower_bound(f.sight.begin(), f.sight.end(), target,
		[](const SightRecord& r, std::uint64_t id) { return r.target < id; });
	if (it != f.sight.end() && it->target == target)
		return *it;
	SightRecord record;
	record.target = target;
	return *f.sight.insert(it, record);
}

} // namespace

double daylight(double dayFraction) noexcept
{
	const double t = dayFraction - std::floor(dayFraction);
	if (t < kSunrise || t > kSunset)
		return 0.0;
	if (t < kSunrise + kRamp)
		return (t - kSunrise) / kRamp;
	if (t > kSunset - kRamp)
		return (kSunset - t) / kRamp;
	return 1.0;
}

double visionRange(double raceMult, bool indoor, const Environment& env, bool weatherAffected, double protection) noexcept
{
	double base = indoor ? kIndoorRange : kBaseRange * raceMult;
	base *= 0.5 + 0.5 * daylight(env.dayFraction);
	if (weatherAffected && env.weatherType != 0)
	{
		const double k = env.weatherType == 1 ? kDustFactor : kRainFactor;
		base += (base * k - base) * env.weatherStrength * (1.0 - protection);
	}
	return base;
}

double stealthFraction(double skill, bool crouched, bool carrying, double mult) noexcept
{
	double f = skill;
	if (carrying)
		f *= 0.75;
	if (crouched)
		f *= 2.0;
	return clamp01(f * mult * 0.01);
}

double coneFactor(double dot) noexcept
{
	const double c = (dot - kConeEdge) / (1.0 - kConeEdge);
	return c > 0.0 ? c : 0.0;
}

double visibility(double dot, bool sneakish, bool crouched, bool moving, double s) noexcept
{
	const double c = coneFactor(dot);
	if (c <= 0.0)
		return 0.0;
	double v = crouched ? c * 0.5 : c;
	if (sneakish)
	{
		double base = v;
		if (!moving)
			base *= c < 0.5 ? 0.5 : 0.75;
		v = base * (1.0 - 0.4 * s);
	}
	return v;
}

double characterLight(double ambient, bool indoor, double lamps) noexcept
{
	if (!indoor && ambient > 0.75)
		return ambient;
	return ambient > 0.5 ? 1.0 : 2.0 * ambient + lamps;
}

double lightTerm(double targetLight, double observerLight) noexcept
{
	const double a = std::clamp(targetLight - observerLight + 1.0, 0.0, 2.0);
	const double b = kDarkLightK + (1.0 - kDarkLightK) * clamp01((targetLight + observerLight) * 0.5);
	return a * b;
}

double reach(double range, double v, double lightTerms, bool observerInCombat) noexcept
{
	double r = range * v;
	if (observerInCombat && r > kCombatReachCap * range)
		r = kCombatReachCap * range;
	return r * lightTerms;
}

bool isMoving(const Fighter& f) noexcept
{
	return std::hypot(f.velX, f.velZ) > 0.5 || std::abs(f.forwardSpeed) > 0.5 || std::abs(f.lateralSpeed) > 0.5;
}

bool inCombat(const Fighter& f) noexcept
{
	return f.target != 0 || !f.engaged.empty();
}

bool isSneakish(const Fighter& f) noexcept
{
	return (f.sneak && !inCombat(f)) || f.crouched;
}

double noiseRange(const Fighter& target, const Environment& env, bool moving, bool combat) noexcept
{
	if (!moving && !combat)
		return 0.0;
	const double wf = noiseWeather(target, env);
	if ((target.sneak && !combat) || target.crouched)
	{
		const double s = stealthFraction(target.skills.stealth, target.crouched);
		return wf * kNoiseSneakLow + (kNoiseSneakFloor - wf * kNoiseSneakLow) * s;
	}
	const double ratio = moving ? (target.moveMode != 0 ? 1.0 : kWalkRatio) : 0.0;
	return wf * kNoiseWalk + (wf * kNoiseRun - wf * kNoiseWalk) * ratio;
}

double fillTime(double s, double v, double attention) noexcept
{
	return (kFillBase + kFillStealth * s * (1.0 - v)) * (4.0 - 3.0 * attention);
}

double scanInterval(std::size_t characters) noexcept
{
	return std::clamp(static_cast<double>(characters) / kScanBudgetPerSecond, kScanMin, kScanMax);
}

double stagger(std::uint64_t diceKey) noexcept
{
	return unit01(diceKey ^ 0x73656e7365ULL);
}

bool notice(Fighter& observer, std::uint64_t target, bool known)
{
	SightRecord& r = obtain(observer, target);
	r.sinceSeen = 0.0;
	r.sinceNoticed = 0.0;
	const bool was = r.seen && r.state == 1;
	r.seen = true;
	r.knownNow = r.knownNow || known;
	r.state = 1; // 140858f40 writes +0x38 = 1 on every call: a yellow (state 2) record turns red
	return !was;
}

const SightRecord* find(const Fighter& observer, std::uint64_t target) noexcept
{
	const auto it = std::lower_bound(observer.sight.begin(), observer.sight.end(), target,
		[](const SightRecord& r, std::uint64_t id) { return r.target < id; });
	return it != observer.sight.end() && it->target == target ? &*it : nullptr;
}

bool aware(const Fighter& observer, std::uint64_t target) noexcept
{
	const SightRecord* r = find(observer, target);
	return r != nullptr && r->seen;
}

bool knownNow(const Fighter& observer, std::uint64_t target) noexcept
{
	const SightRecord* r = find(observer, target);
	return r != nullptr && r->knownNow;
}

bool sensedNow(const Fighter& observer, std::uint64_t target) noexcept
{
	const Sense s = senseOf(observer, target);
	return s == Sense::Sight || s == Sense::Hearing;
}

Sense senseOf(const Fighter& observer, std::uint64_t target) noexcept
{
	const SightRecord* r = find(observer, target);
	if (r == nullptr || !r->seen)
		return Sense::None;
	if (r->knownNow)
		return Sense::Sight;
	return r->sinceSensed <= 0.0 ? Sense::Hearing : Sense::Remembered;
}

bool seenWithin(const Fighter& observer, std::uint64_t target, double maxAge) noexcept
{
	const SightRecord* r = find(observer, target);
	return r != nullptr && r->seen && r->sinceSeen <= maxAge;
}

bool sensedWithin(const Fighter& observer, std::uint64_t target, double maxAge) noexcept
{
	const SightRecord* r = find(observer, target);
	return r != nullptr && r->sinceSensed <= maxAge;
}

void forget(Fighter& observer, std::uint64_t target) noexcept
{
	const auto it = std::lower_bound(observer.sight.begin(), observer.sight.end(), target,
		[](const SightRecord& r, std::uint64_t id) { return r.target < id; });
	if (it == observer.sight.end() || it->target != target)
		return;
	it->seen = false;
	it->state = 0;
	it->meter = 0.0;
	it->active = false;
}

Detection detection(const SightRecord& record) noexcept
{
	Detection d;
	d.state = static_cast<std::uint8_t>(record.state);
	d.meter = record.state == 1 ? 1.0f : static_cast<float>(clamp01(record.meter));
	return d;
}

void scan(std::uint64_t observerId, Fighter& obs, double visionMult, double dt, const std::vector<Other>& others,
	const Environment& env, std::vector<Noticed>& noticed)
{
	const bool obsCombat = inCombat(obs);
	const double memory = obsCombat ? kMemoryCombat : kMemory;
	// The integrator (per character update): the meters of the last scan's active spots fill, the others drain. Timers run.
	for (SightRecord& r : obs.sight)
	{
		r.sinceSeen += dt;
		r.sinceSensed += dt;
		r.sinceNoticed += dt;
		r.sinceRoll += dt;
		if (r.active && r.fill > 0.0)
			r.meter += dt / r.fill;
		else
			r.meter = std::max(0.0, r.meter - dt / kMeterDrain);
		r.active = false;
		if (r.seen && r.sinceSeen > memory)
			r.seen = false;
	}

	// Under a ceiling, not merely inside a building's footprint: a character on a roof is outdoors and reads the daylight (the
	// exe's interior + vf+0x1e0, indoors_state.md section 1). A roof and the floor below stay the same building for the sight
	// test below, which is membership.
	const bool indoor = obs.indoors;
	const double range = visionRange(visionMult, env.inWalls(obs.x, obs.z), env, !obs.animal);
	const double rangeSquaredCull = range * range * (1.0 + 1.0e-9) + 1.0e-9;
	const double ambient = daylight(env.dayFraction);
	const double obsLamps = env.lamps ? env.lamps(obs.x, obs.z, obs.building) : 0.0;
	const double obsLight = characterLight(ambient, indoor, obsLamps);
	const double fx = std::sin(obs.yaw);
	const double fz = std::cos(obs.yaw);

	for (const Other& other : others)
	{
		if (other.id == observerId || other.fighter == nullptr)
			continue;
		const Fighter& tgt = *other.fighter;
		const double dx = tgt.x - obs.x;
		const double dz = tgt.z - obs.z;
		if (dx * dx + dz * dz > rangeSquaredCull)
			continue; // far outside the range: skips the hypot (the margin keeps the exact test below the only judge)
		const double dist = std::hypot(dx, dz);
		if (dist > range)
			continue; // not even a scan candidate
		SightRecord* rec = findMutable(obs, other.id);
		if (tgt.dead)
		{
			// A corpse gets a record too, and only the dead bit (s59 bit 4: the exe builds it with that bit alone). It is never
			// seen or heard: the sight and noise tests below stay for the living.
			if (rec == nullptr)
				obtain(obs, other.id);
			continue;
		}
		const bool sameBuilding = obs.building == tgt.building;
		const bool tgtCombat = inCombat(tgt);
		const bool tgtMoving = isMoving(tgt);

		bool known = false;
		if (sameBuilding)
		{
			const double dot = dist > 0.0 ? (dx * fx + dz * fz) / dist : 1.0;
			const bool sneakish = isSneakish(tgt);
			const double s = stealthFraction(tgt.skills.stealth, tgt.crouched);
			const double v = obs.asleep ? 0.0 : visibility(dot, sneakish, tgt.crouched, tgtMoving, s);
			if (v > kConeMin)
			{
				const double tgtLamps = env.lamps ? env.lamps(tgt.x, tgt.z, tgt.building) : 0.0;
				const double tgtLight = characterLight(ambient, tgt.indoors, tgtLamps);
				if (dist <= reach(range, v, lightTerm(tgtLight, obsLight), obsCombat))
				{
					const bool los = !env.lineOfSight || env.lineOfSight(observerId, obs, other.id, tgt);
					const bool engaged = obs.target == other.id;
					if (sneakish && !engaged)
					{
						// The awareness accumulator (item 18): attention 0 when the observer fights or sleeps.
						const double attn = (obsCombat || obs.asleep) ? 0.0 : obs.attention;
						if (attn > 0.0)
						{
							if (!los)
							{
								if (rec != nullptr)
								{
									rec->meter = 0.0;
									rec->active = false;
								}
							} else {
								if (rec == nullptr)
									rec = &obtain(obs, other.id);
								rec->fill = fillTime(s, v, attn);
								rec->active = true;
								known = rec->meter > kNoticeMeter; // the meter's value before this tick
							}
						}
					} else {
						known = los;
					}
				}
			}
		}
		if (rec == nullptr && known)
			rec = &obtain(obs, other.id);
		// "Sighted" (the noise roll) runs only when "known" failed.
		bool sighted = false;
		if (!known)
		{
			if (obs.playerControlled && tgt.playerControlled)
			{
				sighted = dist < kPlayerPairSight;
			} else if (rec != nullptr && rec->sinceRoll < kSightedThrottle) {
				sighted = rec->sighted;
			} else if (tgtMoving || tgtCombat) {
				const bool building = sameBuilding || (tgtCombat && (obs.building == 0 || tgt.building == 0));
				// rwh.191: the noise of a combatant standing still (an aimed shot held from behind a wall, hours, the gorillo never heard it) does not
				// cross a wall: outside one building (id != 0 on both sides) the roll needs a clear line. UNSURE: the live check is one stationary shooter
				// at 154 u; a walking or running target keeps the roll through walls as before (the Hub bar lure).
				const bool wallStops = tgtCombat && !tgtMoving && !(sameBuilding && obs.building != 0);
				if (building && (!wallStops || !env.lineOfSight || env.lineOfSight(observerId, obs, other.id, tgt)))
				{
					const double noise = noiseRange(tgt, env, tgtMoving, tgtCombat) * (obs.asleep ? 1.0 : kSightedAwake);
					// UNSURE: the visibility curve of the roll was dropped by the decompiler (static 2.7); an inverse lerp
					// of the squared distance over the squared noise range, doubled while the record's memory timer runs.
					double p = noise > 0.0 ? clamp01(1.0 - dist * dist / (noise * noise)) : 0.0;
					if (rec != nullptr && rec->seen && rec->sinceSeen < kRecordFresh)
						p = std::min(1.0, p * 2.0);
					if (p > 0.0)
					{
						if (rec == nullptr)
							rec = &obtain(obs, other.id);
						// Task CC: the roll hangs on the two characters' dice keys, not their Ids (an unrelated spawn moved every Id
						// and re-rolled every hearing); a fighter without one keeps its Id.
						const std::uint64_t observerKey = obs.diceSeed != 0 ? obs.diceSeed : observerId;
						const std::uint64_t otherKey = tgt.diceSeed != 0 ? tgt.diceSeed : other.id;
						sighted = (tgtCombat && sameBuilding)
							|| unit01(observerKey * 0x9e3779b97f4a7c15ULL ^ (otherKey << 20) ^ rec->rolls) < p;
						++rec->rolls;
						rec->sinceRoll = 0.0;
					}
				}
			}
			if (rec != nullptr)
				rec->sighted = sighted;
		} else if (rec != nullptr) {
			rec->sighted = false;
		}
		if (known || sighted)
		{
			if (rec == nullptr)
				rec = &obtain(obs, other.id);
			rec->knownNow = known;
			const bool firstSight = !rec->seen;
			if (firstSight)
				noticed.push_back({observerId, other.id});
			rec->seen = true;
			rec->sinceSeen = 0.0;
			rec->sinceSensed = 0.0;
			// The dialogue memory (DC_TARGET_LAST_SEEN_X_HOURS_AGO / DC_MET_TARGET_BEFORE): the sighting is stamped, and it outlives
			// the record (bounded per character, the oldest evicted). A target noticed for the first time is stamped on its next
			// sighting instead (one scan interval later): the sight events this scan raises are evaluated after it in the same tick,
			// and dialogue 3186's MET_TARGET_BEFORE gate asks "not met before" about the meeting it is deciding (task BV; the
			// original's order UNCERTAIN).
			if (!firstSight)
				obs.rememberMet(other.id, env.tick);
		} else if (rec != nullptr) {
			rec->knownNow = false; // 140859010: a scan with neither sight nor hearing clears seen_now
		} else {
			// The exe's sensor list is the radius query's result: a record exists for every character inside the scan range, seen or
			// not (s59 bit 8 "remembered"; the capture resolver's masks do not exclude it). sinceSensed keeps its old value, so the
			// record reads "not currently seen".
			obtain(obs, other.id);
		}
	}

	// Records: the state, and the erase of the spent (a corpse's record stays while it is in range: s59 bit 4).
	std::vector<SightRecord> kept;
	kept.reserve(obs.sight.size());
	for (SightRecord& r : obs.sight)
	{
		bool gone = false;
		const auto it = std::lower_bound(others.begin(), others.end(), r.target,
			[](const Other& o, std::uint64_t id) { return o.id < id; });
		if (it == others.end() || it->id != r.target || it->fighter == nullptr)
			gone = true;
		if (!gone)
		{
			// The radius query returns the character again every pass: a record is kept while its target is inside the scan range, and a
			// record the scan did not sense, with no meter and no roll pending, is spent only once the target is out of range.
			const Fighter& tgt = *it->fighter;
			const bool inRange = std::hypot(tgt.x - obs.x, tgt.z - obs.z) <= range;
			if (!inRange && !r.seen && !r.active && r.meter <= 0.0 && r.sinceRoll > kSightedThrottle)
				gone = true;
		}
		if (gone)
			continue;
		r.state = r.seen ? 1 : (r.active ? 2 : 0);
		kept.push_back(r);
	}
	obs.sight = std::move(kept);
}

} // namespace ksim::perception
