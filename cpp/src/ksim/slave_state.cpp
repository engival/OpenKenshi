// The slave state machine (kenshi-k16.6.117, oracle rwh.99 section 6): the one setter, the escape tests and the escaped / ex-slave timers.
#include "ksim/simulation.hpp"

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/jobs.hpp"

#include <cmath>
#include <sstream>

namespace ksim
{

namespace
{

constexpr std::uint16_t kObedientSlaveJob = 187;

} // namespace

void Simulation::pointObedientRow(std::uint64_t slave, std::uint64_t captor)
{
	flecs::entity entity = fighterEntity(slave);
	const state::Jobs* held = entity.get<state::Jobs>();
	if (held == nullptr)
		return;
	state::Jobs jobs = *held;
	bool changed = false;
	for (state::JobRow& row : jobs.rows)
		if (row.type == kObedientSlaveJob && row.target != captor)
		{
			row.target = captor;
			changed = true;
		}
	if (changed)
		entity.set<state::Jobs>(std::move(jobs));
}

std::uint64_t Simulation::savedCaptor(std::uint64_t slave) const
{
	if (const state::Jobs* held = fighterEntity(slave).get<state::Jobs>())
		for (const state::JobRow& row : held->rows)
			if (row.type == kObedientSlaveJob && row.target != 0)
				return row.target;
	const state::Fighter* f = combat_->find(slave);
	return f != nullptr ? f->followTarget : 0;
}

void Simulation::setSlaveState(std::uint64_t id, std::uint8_t state, std::uint64_t captor)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr)
		return;
	const std::uint8_t old = f->slaveState;
	if (captor != 0)
	{
		SlaveTrack& track = slaveTrack_[id];
		track.captor = captor;
		const state::Fighter* hand = combat_->find(captor);
		track.captorSquad = hand != nullptr ? hand->squad : 0;
		track.goneSince = 0;
	}
	if (old == state)
	{
		if (captor != 0 && state == 1)
			pointObedientRow(id, captor); // a new captor (a re-capture, a hand-over): the row follows it
		return;
	}
	f->slaveState = state;
	{
		std::ostringstream line;
		line << "slave state " << id << ": " << int(old) << " -> " << int(state);
		logTask(line.str());
	}
	if (state == 0)
	{
		slaveTrack_.erase(id);
	} else if (state == 1) {
		// The row is added with the orders wiped (14032e0d0, new == 1). A slave the sim loaded keeps its follow target as the captor.
		SlaveTrack& track = slaveTrack_[id];
		track.goneSince = 0;
		if (track.captor == 0)
			track.captor = f->followTarget;
		cancelOrders(id);
		enslaveJobs(id);
		pointObedientRow(id, track.captor);
	} else {
		// Every change into 2 or 3 stamps the world clock (the setter, 1405a4940: `if (1 < new)`), the 2 -> 3 edge included (1405a4f50).
		slaveTrack_[id];
		f->slaveStamp = gameHoursIn(clockTick());
	}
	if (old == 1 && state != 1)
	{
		// Leaving state 1 removes only the Obedient Slave row; every other order survives.
		flecs::entity entity = fighterEntity(id);
		if (const state::Jobs* held = entity.get<state::Jobs>())
		{
			state::Jobs jobs = *held;
			bool removed = false;
			for (std::size_t i = jobs.rows.size(); i-- > 0;)
				if (jobs.rows[i].type == kObedientSlaveJob)
				{
					jobs.rows.erase(jobs.rows.begin() + static_cast<std::ptrdiff_t>(i));
					removed = true;
				}
			if (removed)
				entity.set<state::Jobs>(std::move(jobs));
		}
	}
	mirrorFighter(id);
}

double Simulation::slaveHeatSeconds(std::uint64_t id) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || f->slaveState != 2 || f->slaveStamp < 0.0)
		return 0.0;
	// A display value of the HUD (rwh.99 section 6, rwh.106 section 6): 250 s from the stamp of the escape, ticking down with the sim clock. It
	// gates nothing; the state machine is the 12 h / 96 h pair in stepSlaveState.
	return std::max(0.0, kEscapeHeatSeconds - (gameHoursIn(clockTick()) - f->slaveStamp) * kGameHourSeconds);
}

std::uint64_t Simulation::slaveCaptor(std::uint64_t id) const
{
	const auto track = slaveTrack_.find(id);
	return track != slaveTrack_.end() ? track->second.captor : 0;
}

void Simulation::stepSlaveState()
{
	if (combat_ == nullptr)
		return;
	const std::uint64_t now = clockTick();
	// A slave the sim loaded (or any writer that bypassed the setter) with a follow target is tracked from it, once a second.
	if (now % kSquadIntervalTicks == 0)
		for (const CombatArena::Member& member : combat_->members())
			if (member.fighter.slaveState == 1 && slaveTrack_.count(member.id) == 0)
				if (const std::uint64_t captor = savedCaptor(member.id); captor != 0)
					slaveTrack_[member.id].captor = captor;
	// A state 2 or 3 the sim loaded (or any writer that bypassed the setter) is tracked too, so its timer runs.
	for (const CombatArena::Member& member : combat_->members())
		if (member.fighter.slaveState > 1 && slaveTrack_.count(member.id) == 0)
			slaveTrack_[member.id];
	const double hours = gameHoursIn(now);
	std::vector<std::uint64_t> ids;
	ids.reserve(slaveTrack_.size());
	for (const auto& [id, track] : slaveTrack_)
		ids.push_back(id);
	for (const std::uint64_t id : ids)
	{
		state::Fighter* f = combat_->find(id);
		if (f == nullptr || f->dead || f->slaveState == 0)
		{
			slaveTrack_.erase(id);
			continue;
		}
		SlaveTrack& track = slaveTrack_[id];
		if (f->slaveState > 1)
		{
			// rwh.106 (1405a4f50): elapsed = the world clock - the stamp. A stamp ahead of the clock (a load of an earlier clock) frees the slave
			// at once; 3 -> 0 past 4.0 days, 2 -> 3 past 12.0 hours, both strict. No stamp yet (a pre-v107 save, a writer around the setter): now.
			if (f->slaveStamp < 0.0)
				f->slaveStamp = hours;
			const double elapsed = hours - f->slaveStamp;
			if (elapsed < 0.0 || (f->slaveState == 3 && elapsed / 24.0 > kExSlaveGameHours / 24.0))
				setSlaveState(id, 0);
			else if (f->slaveState == 2 && elapsed > kEscapedSlaveGameHours)
				setSlaveState(id, 3);
			continue;
		}
		// State 1. An imprisoned slave is the cage's (the per-frame test skips it, and the delivery carries its slave a long way from the hand).
		if (f->cageBuilding != 0 || track.captor == 0)
			continue;
		// A hand-over (the squad's leader, a keeper, a buyer) re-points the follow: the new follow target is the captor hand.
		if (f->followTarget != 0 && f->followTarget != track.captor)
			if (const state::Fighter* next = combat_->find(f->followTarget); next != nullptr && !next->dead)
			{
				track.captor = f->followTarget;
				pointObedientRow(id, track.captor);
			}
		const state::Fighter* hand = combat_->find(track.captor);
		if (hand != nullptr && !hand->dead)
		{
			track.goneSince = 0;
			track.captorSquad = hand->squad;
			// Per AI update (FUN_1405a4f50): beyond 3535 u the slave escapes.
			if (now % kSquadIntervalTicks == 0 && std::hypot(f->x - hand->x, f->z - hand->z) > kEscapeDistance)
				setSlaveState(id, 2);
			continue;
		}
		if (f->unconscious || f->down)
			continue; // incapacitated: the per-frame test does not run
		// The captor is gone (AI::syncCharacterState): a still-valid character of its squad takes the slave over, else 45 s and it escapes.
		std::uint64_t heir = 0;
		if (track.captorSquad != 0)
			for (const CombatArena::Member& member : combat_->members())
				if (member.id != id && member.id != track.captor && member.fighter.squad == track.captorSquad && !member.fighter.dead
					&& (heir == 0 || member.id < heir))
					heir = member.id;
		if (heir != 0)
		{
			if (f->followTarget == track.captor)
				f->followTarget = heir;
			track.captor = heir;
			pointObedientRow(id, heir);
			track.goneSince = 0;
			mirrorFighter(id);
			continue;
		}
		if (track.goneSince == 0)
			track.goneSince = now;
		else if (static_cast<double>(now - track.goneSince) * kTickSeconds >= kCaptorGoneSeconds)
			setSlaveState(id, 2);
	}
}

} // namespace ksim
