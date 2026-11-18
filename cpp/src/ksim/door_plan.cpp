#include "ksim/door_plan.hpp"

#include <algorithm>
#include <cmath>

namespace ksim {

bool kindBashesDoors(std::int32_t kind) noexcept
{
	switch (static_cast<FactionKind>(kind))
	{
	case FactionKind::LawEnforcement:
	case FactionKind::Military:
	case FactionKind::Slave:
	case FactionKind::Slaver:
	case FactionKind::Bandit:
		return true;
	default:
		return false;
	}
}

namespace
{

// splitmix64 (the lock's dice have no state of their own).
std::uint64_t mix(std::uint64_t z) noexcept
{
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

double unit(std::uint64_t z) noexcept
{
	return static_cast<double>(mix(z) >> 11) * (1.0 / 9007199254740992.0);
}

// Closes the gap of summing 0.05 s steps: 100 of them reach 5 s.
constexpr double kTimerSlack = 1e-9;

} // namespace

PickTick tickLock(LockTimer& lock, double dt, double chance, std::uint64_t lockKey, std::uint64_t picker) noexcept
{
	lock.elapsed += dt;
	if (lock.elapsed + kTimerSlack < lock.interval)
		return PickTick::Waiting;
	lock.elapsed = 0.0;
	const std::uint64_t seed = lockKey ^ (static_cast<std::uint64_t>(lock.attempts++) << 40) ^ (picker * 0x9e3779b97f4a7c15ULL);
	if (unit(seed ^ 0x7069636bULL) < chance)
		return PickTick::Unlocked;
	lock.interval = kPickIntervalMin + (kPickIntervalMax - kPickIntervalMin) * unit(seed ^ 0x696e7476ULL);
	return PickTick::Failed;
}

double doorMaxHealth(const kdata::Record& door) noexcept
{
	double total = 0.0;
	const kdata::RefCategory* construction = door.findRefs("construction");
	if (construction != nullptr)
	{
		for (const kdata::RefEntry& entry : construction->entries)
		{
			if (!entry.opaqueEntry && entry.v0 > 0)
				total += static_cast<double>(entry.v0);
		}
	}
	if (total <= 0.0)
		total = static_cast<double>(std::max(door.intOr("build materials", 1), 0));
	return total > 0.0 ? total : 1.0;
}

double doorMoveSpeed(const kdata::Record& door) noexcept
{
	return static_cast<double>(door.floatOr("door move speed", 0.5f));
}

double doorMoveSeconds(double speed) noexcept
{
	return 1.0 / std::max(speed, 0.1);
}

std::int32_t rollLockLevel(std::int32_t base, std::int32_t random, std::int32_t bonus, std::uint64_t key) noexcept
{
	random = std::max(random, 0);
	const std::int32_t spread = static_cast<std::int32_t>(mix(key ^ 0x6c6f636bULL) % static_cast<std::uint64_t>(2 * random + 1)) - random;
	return std::clamp(spread + base + bonus, 1, 100);
}

double pickChance(double lockLevel, double skill) noexcept
{
	const double difficulty = lockLevel - skill;
	if (difficulty <= 0.0)
		return kPickBestChance;
	return kPickBestChance / std::pow(2.0, difficulty / kPickLevelsPerHalving);
}

double cutRate(double strength) noexcept
{
	const double t = std::clamp(strength * 0.01, 0.0, 1.0);
	return kCutRate * (kCutDrainMin + (kCutDrainMax - kCutDrainMin) * t);
}

bool bruteBreaks(double strength, double hardness) noexcept
{
	return strength > hardness * kBruteFactor;
}

bool sufficientLockSkill(double level, double skill, bool caged) noexcept
{
	return pickChance(level, caged ? skill - kCagePickSkillPenalty : skill) > kMinPickChance;
}

DoorPoint doorStandPoint(const DoorSlab& slab, DoorSide side, double fromX, double fromZ, double faceOut) noexcept
{
	const double nx = -slab.axisZ;
	const double nz = slab.axisX;
	const double out = slab.halfDeep + faceOut;
	const bool known = slab.inX != 0.0 || slab.inZ != 0.0;
	double sign = (fromX - slab.x) * nx + (fromZ - slab.z) * nz >= 0.0 ? 1.0 : -1.0; // Near: the walker's face
	if (known && side != DoorSide::Near)
	{
		const double inward = nx * slab.inX + nz * slab.inZ >= 0.0 ? 1.0 : -1.0; // which of the two normals points into the building
		sign = side == DoorSide::Inside ? inward : -inward;
	}
	return {slab.x + nx * sign * out, slab.z + nz * sign * out};
}

} // namespace ksim
