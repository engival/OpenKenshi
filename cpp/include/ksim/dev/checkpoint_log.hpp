#pragma once

// DEV ONLY (ARCHITECTURE "Dev-only code is fenced"; CMake option KSIM_DEV): the per-NPC checkpoint log of a fight between two squads, in the shape of the
// original's (kenshi-rwh.91 `fight_checkpoints.py`, docs/reference/PLAYTEST.md spectate_fight) so the two files diff row by row (kenshi-k16.6.109).
//
// One JSONL file: a `{"meta": ...}` line, then one row per NPC per checkpoint with the original's fields: t, sample_t, front_gap, mixed_within_5u, side,
// addr, tag, name, faction, race, weapon, weapon_type, pos, d_own_centroid, yaw, speed, gait, clip, st, task, tech, tgt, tgt_name, tgt_dist, blood.
//   - `t` is the original's WALL seconds at game speed 5 (it paused the game at t = 0, 0.5, 1, 2, 3, 5, 8, 12, 20, 30, 60 wall s): the sim's seconds / 5 (one
//     frame at 30 fps carries 0.166 sim s). `sample_t` is the sim seconds since the start, so the two diff on either.
//   - Fields the sim cannot fill: `addr` is the entity id; `weapon` is "" (the fighter holds no item name, `weapon_type` is the wielded category as the
//     original's); `clip` is the technique's animation name or ""; `task` is derived (4 MELEE_ATTACK with a target, 262 / 263 / 266 the ranged task, 141
//     FOLLOW_SQUADLEADER while following, -1 otherwise: the sim has no per-character task id); `yaw` is degrees as the original's (0 = +z here, UNSURE it is
//     the same reference there). Added: `goal` (the AI goal that started the fight, Fighter::combatGoal), `game_s`.
// The meta line holds the `marks` (first_target, first_contact, first_hit, first_run, first_close_1u: t, event, who, other) and each NPC's marks.
// Also the squads' side shares (`sideShares`, one per sim second): per squad the members on its own side of the perpendicular bisector between the two squad
// centroids ("held the line", the user 2026-11-12).

#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ksim::dev {

struct SideShare
{
	double simSeconds = 0.0; // since the start
	int ownA = 0; // side 0's members on its own side of the bisector
	int nA = 0;
	int ownB = 0;
	int nB = 0;
	double gap = 0.0; // the distance between the two centroids
};

class CheckpointLog
{
public:
	// `start` names the staging ("kscen" / "import"), kept in the meta line.
	CheckpointLog(Simulation& sim, std::shared_ptr<const CombatData> combat, std::string start);

	// A side: its name (`Berserkers_16`), the members' tag base (`Berserker`) and faction label, and its members (tagged `<base>#<n>` in id order).
	void addSide(const std::string& name, const std::string& tagBase, const std::string& faction, const std::vector<std::uint64_t>& members);

	// Takes t = 0 and installs the combat observer; then call tick() after every Simulation::tick().
	void begin();
	void tick();
	// Writes the meta line and the rows.
	void write(std::ostream& out) const;

	const std::vector<SideShare>& sideShares() const noexcept { return shares_; }
	// The sim second of the first cross-side contact (a Resolve event), -1 = none yet.
	double firstContact() const noexcept { return marks_.count("first_contact") != 0 ? marks_.at("first_contact").t : -1.0; }

private:
	struct Member
	{
		std::uint64_t id = 0;
		int side = 0;
		std::string tag;
		double firstRun = -1.0;
		double firstTarget = -1.0;
		double firstHit = -1.0; // it hit someone
		double firstHurt = -1.0; // it was hit
	};
	struct Mark
	{
		double t = 0.0;
		std::string event;
		std::string who;
		std::string other;
		double d = -1.0;
	};
	struct Side
	{
		std::string name;
		std::string tagBase;
		std::string faction;
	};

	void sample(double simT, bool checkpoint);
	void mark(const std::string& key, const std::string& event, std::uint64_t who, std::uint64_t other, double d = -1.0);
	Member* memberOf(std::uint64_t id);
	std::string tagOf(std::uint64_t id) const;
	double simNow() const;

	Simulation& sim_;
	std::shared_ptr<const CombatData> combat_;
	std::string start_;
	std::vector<Side> sides_;
	std::vector<Member> members_;
	std::uint64_t startTick_ = 0;
	std::vector<double> checkpoints_; // sim seconds
	std::size_t nextCheckpoint_ = 0;
	std::size_t nextShare_ = 0;
	std::map<std::string, Mark> marks_;
	std::vector<std::string> rows_; // the JSON of each row
	std::vector<SideShare> shares_;
};

} // namespace ksim::dev
