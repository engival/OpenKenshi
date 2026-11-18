// DEV ONLY (include/ksim/dev/checkpoint_log.hpp): the per-NPC checkpoint log of a two-squad fight, in the shape of rwh.91's.
#include "ksim/dev/checkpoint_log.hpp"

#include "ksim/combat/fighter_state.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace ksim::dev {

namespace {

constexpr double kWallToSim = 5.0; // the original ran at game speed 5: one wall second is five sim seconds
constexpr double kRunShare = 0.6; // a gait above this share of the run speed is a run
constexpr double kMovingShare = 0.05; // below it the body stands

std::string num(double v)
{
	if (!std::isfinite(v))
		return "null";
	std::ostringstream out;
	out << std::setprecision(10) << v;
	return out.str();
}

std::string quoted(const std::string& s)
{
	std::string out = "\"";
	for (const char c : s)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out + "\"";
}

} // namespace

CheckpointLog::CheckpointLog(Simulation& sim, std::shared_ptr<const CombatData> combat, std::string start)
	: sim_(sim), combat_(std::move(combat)), start_(std::move(start))
{
	for (const double wall : {0.0, 0.5, 1.0, 2.0, 3.0, 5.0, 8.0, 12.0, 20.0, 30.0, 60.0})
		checkpoints_.push_back(wall * kWallToSim);
}

void CheckpointLog::addSide(const std::string& name, const std::string& tagBase, const std::string& faction, const std::vector<std::uint64_t>& members)
{
	const int side = static_cast<int>(sides_.size());
	sides_.push_back({name, tagBase, faction});
	std::vector<std::uint64_t> ids = members;
	std::sort(ids.begin(), ids.end());
	int n = 0;
	for (const std::uint64_t id : ids)
	{
		Member m;
		m.id = id;
		m.side = side;
		m.tag = tagBase + "#" + std::to_string(++n);
		members_.push_back(m);
	}
}

double CheckpointLog::simNow() const
{
	return static_cast<double>(sim_.clockTick() - startTick_) * kTickSeconds;
}

CheckpointLog::Member* CheckpointLog::memberOf(std::uint64_t id)
{
	for (Member& m : members_)
		if (m.id == id)
			return &m;
	return nullptr;
}

std::string CheckpointLog::tagOf(std::uint64_t id) const
{
	for (const Member& m : members_)
		if (m.id == id)
			return m.tag;
	return "0";
}

void CheckpointLog::mark(const std::string& key, const std::string& event, std::uint64_t who, std::uint64_t other, double d)
{
	if (marks_.count(key) == 0)
		marks_[key] = {simNow(), event, tagOf(who), other != 0 ? tagOf(other) : "0", d};
}

void CheckpointLog::begin()
{
	startTick_ = sim_.clockTick();
	sim_.setCombatObserver([this](const CombatEvent& e)
	{
		Member* who = memberOf(e.who);
		Member* other = memberOf(e.other);
		if (who == nullptr || other == nullptr || who->side == other->side)
			return;
		if (e.kind == CombatEvent::Kind::Resolve)
			mark("first_contact", "resolve", e.who, e.other);
		else if (e.kind == CombatEvent::Kind::Hit)
		{
			mark("first_hit", "hit", e.who, e.other);
			if (who->firstHurt < 0.0)
				who->firstHurt = simNow();
			if (other->firstHit < 0.0)
				other->firstHit = simNow();
		}
	});
	sample(0.0, true);
	++nextCheckpoint_;
}

void CheckpointLog::tick()
{
	const double now = simNow();
	const CombatArena* arena = sim_.combat();
	// the marks that come from the state
	for (Member& m : members_)
	{
		const state::Fighter* f = arena->find(m.id);
		if (f == nullptr)
			continue;
		if (m.firstTarget < 0.0 && f->target != 0)
		{
			const Member* t = memberOf(f->target);
			if (t != nullptr && t->side != m.side)
			{
				m.firstTarget = now;
				mark("first_target", "target", m.id, f->target);
			}
		}
		if (m.firstRun < 0.0 && std::hypot(f->velX, f->velZ) >= kRunShare * arena->runSpeed(*f))
		{
			m.firstRun = now;
			mark("first_run", "sample", m.id, 0);
		}
	}
	if (marks_.count("first_close_1u") == 0)
		for (const Member& a : members_)
			for (const Member& b : members_)
			{
				const state::Fighter* fa = arena->find(a.id);
				const state::Fighter* fb = arena->find(b.id);
				if (a.side < b.side && fa != nullptr && fb != nullptr && std::hypot(fa->x - fb->x, fa->z - fb->z) < 1.0)
					mark("first_close_1u", "proximity", a.id, b.id, std::hypot(fa->x - fb->x, fa->z - fb->z));
			}
	if (nextCheckpoint_ < checkpoints_.size() && now + 1e-9 >= checkpoints_[nextCheckpoint_])
	{
		sample(now, true);
		++nextCheckpoint_;
	}
	if (now + 1e-9 >= static_cast<double>(nextShare_))
	{
		sample(now, false);
		++nextShare_;
	}
}

// The centroid of the side's members still in the arena, the side share and the checkpoint rows.
void CheckpointLog::sample(double simT, bool checkpoint)
{
	const CombatArena* arena = sim_.combat();
	struct Centre
	{
		double x = 0.0;
		double z = 0.0;
		int n = 0;
	};
	std::vector<Centre> centre(sides_.size());
	for (const Member& m : members_)
		if (const state::Fighter* f = arena->find(m.id))
		{
			centre[static_cast<std::size_t>(m.side)].x += f->x;
			centre[static_cast<std::size_t>(m.side)].z += f->z;
			++centre[static_cast<std::size_t>(m.side)].n;
		}
	for (Centre& c : centre)
		if (c.n != 0)
		{
			c.x /= c.n;
			c.z /= c.n;
		}
	if (sides_.size() != 2 || centre[0].n == 0 || centre[1].n == 0)
		return;
	const double gap = std::hypot(centre[0].x - centre[1].x, centre[0].z - centre[1].z);
	const double midX = (centre[0].x + centre[1].x) / 2.0;
	const double midZ = (centre[0].z + centre[1].z) / 2.0;
	if (!checkpoint)
	{
		SideShare s;
		s.simSeconds = simT;
		s.gap = gap;
		for (const Member& m : members_)
			if (const state::Fighter* f = arena->find(m.id))
			{
				const Centre& own = centre[static_cast<std::size_t>(m.side)];
				const Centre& far = centre[static_cast<std::size_t>(1 - m.side)];
				const bool onOwn = (f->x - midX) * (own.x - far.x) + (f->z - midZ) * (own.z - far.z) > 0.0;
				(m.side == 0 ? s.nA : s.nB) += 1;
				(m.side == 0 ? s.ownA : s.ownB) += onOwn ? 1 : 0;
			}
		shares_.push_back(s);
		return;
	}
	int mixed = 0;
	for (const Member& a : members_)
	{
		const state::Fighter* fa = arena->find(a.id);
		for (const Member& b : members_)
		{
			const state::Fighter* fb = arena->find(b.id);
			if (fa != nullptr && fb != nullptr && a.side != b.side && std::hypot(fa->x - fb->x, fa->z - fb->z) < 5.0)
			{
				++mixed;
				break;
			}
		}
	}
	for (const Member& m : members_)
	{
		const state::Fighter* f = arena->find(m.id);
		if (f == nullptr)
			continue;
		const Side& side = sides_[static_cast<std::size_t>(m.side)];
		const Centre& own = centre[static_cast<std::size_t>(m.side)];
		const double speed = std::hypot(f->velX, f->velZ);
		const double run = arena->runSpeed(*f);
		const char* gait = speed < kMovingShare * run ? "-" : speed >= kRunShare * run ? "run" : "walk";
		const state::Fighter* target = f->target != 0 ? arena->find(f->target) : nullptr;
		const int task = f->rangedTask != 0 ? f->rangedTask : f->target != 0 ? 4 : f->followTarget != 0 ? 141 : -1;
		const kdata::Record* race = sim_.gameData()->find(f->raceSid);
		const Technique* tech = f->technique >= 0 ? &combat_->techniques[static_cast<std::size_t>(f->technique)] : nullptr;
		std::ostringstream row;
		row << "{\"t\": " << num(simT / kWallToSim) << ", \"sample_t\": " << num(simT) << ", \"game_s\": " << num(simT) << ", \"front_gap\": " << num(gap)
			<< ", \"mixed_within_5u\": " << mixed << ", \"side\": " << quoted(side.name) << ", \"addr\": " << quoted(std::to_string(m.id)) << ", \"tag\": " << quoted(m.tag)
			<< ", \"name\": " << quoted(side.tagBase) << ", \"faction\": " << quoted(side.faction) << ", \"race\": " << quoted(race != nullptr ? race->name : f->raceSid)
			<< ", \"weapon\": \"\", \"weapon_type\": " << f->weapon.weaponType << ", \"pos\": [" << num(f->x) << ", " << num(f->y) << ", " << num(f->z) << "]"
			<< ", \"d_own_centroid\": " << num(std::hypot(f->x - own.x, f->z - own.z)) << ", \"yaw\": " << num(f->yaw * 180.0 / 3.14159265358979323846)
			<< ", \"speed\": " << num(speed) << ", \"gait\": " << quoted(gait) << ", \"clip\": " << quoted(tech != nullptr ? tech->animName : "")
			<< ", \"st\": " << static_cast<int>(f->state) << ", \"task\": " << task << ", \"goal\": " << f->combatGoal << ", \"tech\": " << quoted(tech != nullptr ? tech->name : "")
			<< ", \"tgt\": " << quoted(f->target != 0 ? std::to_string(f->target) : "0") << ", \"tgt_name\": " << quoted(tagOf(f->target))
			<< ", \"tgt_dist\": " << (target != nullptr ? num(std::hypot(target->x - f->x, target->z - f->z)) : std::string("null")) << ", \"blood\": " << num(f->blood) << "}";
		rows_.push_back(row.str());
	}
}

void CheckpointLog::write(std::ostream& out) const
{
	out << "{\"meta\": {\"log\": \"ksim\", \"start\": " << quoted(start_) << ", \"clock\": \"t = sim seconds / " << kWallToSim << " (the original's wall seconds at speed 5)\", \"sides\": [";
	for (std::size_t i = 0; i < sides_.size(); ++i)
		out << (i != 0 ? ", " : "") << quoted(sides_[i].name);
	out << "], \"samples\": " << rows_.size() << ", \"t_end\": " << num(simNow() / kWallToSim) << ", \"marks\": {";
	bool first = true;
	for (const auto& [key, m] : marks_)
	{
		out << (first ? "" : ", ") << quoted(key) << ": {\"t\": " << num(m.t / kWallToSim) << ", \"sim_s\": " << num(m.t) << ", \"event\": " << quoted(m.event) << ", \"who\": " << quoted(m.who)
			<< ", \"other\": " << quoted(m.other);
		if (m.d >= 0.0)
			out << ", \"d\": " << num(m.d);
		out << "}";
		first = false;
	}
	out << "}, \"side_totals\": {";
	for (std::size_t i = 0; i < sides_.size(); ++i)
	{
		std::size_t n = 0;
		for (const Member& m : members_)
			n += m.side == static_cast<int>(i) ? 1 : 0;
		out << (i != 0 ? ", " : "") << quoted(sides_[i].name) << ": " << n;
	}
	out << "}, \"npc_marks\": {";
	first = true;
	for (const Member& m : members_)
	{
		out << (first ? "" : ", ") << quoted(std::to_string(m.id)) << ": {\"side\": " << quoted(sides_[static_cast<std::size_t>(m.side)].name) << ", \"npc\": " << quoted(m.tag)
			<< ", \"first_run_t\": " << (m.firstRun >= 0.0 ? num(m.firstRun / kWallToSim) : "null") << ", \"first_target_t\": " << (m.firstTarget >= 0.0 ? num(m.firstTarget / kWallToSim) : "null")
			<< ", \"first_hurt_t\": " << (m.firstHurt >= 0.0 ? num(m.firstHurt / kWallToSim) : "null") << ", \"first_hit_t\": " << (m.firstHit >= 0.0 ? num(m.firstHit / kWallToSim) : "null") << "}";
		first = false;
	}
	out << "}}}\n";
	for (const std::string& row : rows_)
		out << row << "\n";
}

} // namespace ksim::dev
