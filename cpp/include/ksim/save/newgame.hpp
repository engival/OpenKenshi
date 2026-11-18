#pragma once

// New game (docs/MULTIPLAYER.md section 4, docs/formats/SIMLINK.md STARTS / NEW_GAME / order 84): the NEW_GAME_STARTOFF records of the
// merged data as the sim reads them, and the outcome of the logged NewGame command. The sim half is Simulation::runNewGame and
// friends (newgame.cpp); the frames are the session's (simlink.cpp).

#include "kdata/gamedata.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ksim {

// One NEW_GAME_STARTOFF record (STARTS_CAGES_STATIC.md a; the fields were checked against the merged data of the vanilla load order).
struct StartOff
{
	// A place the squad may start: the forced position, or a `town` ref's primary 0-townlist placement.
	struct Place
	{
		std::string town; // TOWN sid; "" = a forced or fallback position
		double x = 0.0;
		double z = 0.0;
	};
	// A `squad` ref: a SQUAD_TEMPLATE or a bare CHARACTER (count = v0, 0 counts as 1).
	struct SquadRef
	{
		std::string sid;
		std::int32_t count = 0;
	};
	std::string sid;
	std::string name;
	std::int32_t money = 0;
	bool forcePos = false;
	std::vector<Place> places;
	std::vector<SquadRef> squads;
	std::vector<std::pair<std::string, std::int32_t>> relations; // `faction relations`: FACTION sid, v0 (v1 and v2 are ignored by the exe)
	std::vector<std::string> research; // RESEARCH sids the new game completes (empty: `_Default Start`, see startResearch)
	std::vector<std::string> forceRace; // RACE sids a character creation may pick from (CreatorCatalog::allowedRaces); n = 0 keeps the template's own races
};

// Every NEW_GAME_STARTOFF of the merged data, in record order (a record a later file REMOVED is gone from the merge).
class StartCatalog
{
public:
	static StartCatalog build(const kdata::GameData& gameData, std::string_view dataDir);
	const std::vector<StartOff>& all() const noexcept { return starts_; }
	const StartOff* find(std::string_view sid) const noexcept;

private:
	std::vector<StartOff> starts_;
};

// What a NewGame command did when it applied (the observers hear it inside Simulation::tick). reason empty = ok.
struct NewGameOutcome
{
	std::uint64_t ticket = 0; // Command::object
	std::uint64_t tick = 0;
	std::string reason;
	std::uint64_t faction = 0; // state::Id of the FactionState that became the player faction
	std::string factionSid; // its FACTION record
	std::vector<std::uint64_t> members; // state::Id of the characters, leader first
	double x = 0.0; // the leader's world position after placement (NEW_GAME_RESULT x y z)
	double y = 0.0;
	double z = 0.0;
};

// The squad a start-off gives without a character creation (n = 0): the members of its `squad` refs as they are. Each is a CHARACTER
// sid and the SQUAD_TEMPLATE it came from ("" for a bare CHARACTER ref). Counts are deterministic from `rng` (ranges drawn, `choosefrom
// list` drawn as the default-world spawner does); animals and a template's other lists (`housemates`, `slaves`, `prisoners`) are not
// spawned. `rng` is a RngStream-style draw: pass nullptr to take every range at its minimum (the count a Spec list must match).
struct StartMember
{
	std::string character;
	std::string squadTemplate;
	// Filled by Simulation::startSquad (the seeded expansion STARTS shows and NEW_GAME builds):
	std::uint64_t rng = 0; // the makeFighter rng state of this member (its race pick, weapon and armour rolls)
	std::string race; // RACE sid it will be ("" if its record cannot be built)
	std::uint8_t female = 0; // from the CHARACTER's `female chance`, then made a sex the race has in the creator
	std::string name; // the CHARACTER's name
	bool editable = false; // a creator race and not an animal (STARTS flags bit 0)
	// What the same makeFighter roll equips (STARTS outfit, kenshi-k16.5.4): the ARMOUR sids in the fighter's equip order, the WEAPON sid ("" = fists).
	std::vector<std::string> armour;
	std::string weapon;
};
class RngStream;
std::vector<StartMember> startMembers(const kdata::GameData& gameData, const StartOff& start, RngStream* rng);

// The RESEARCH `_Default Start` of the base game (5359-gamedata.base): what a new game with no `research` refs, and a save with no
// completed research, begins with (build_menu.md section 1).
inline constexpr std::string_view kDefaultStartResearch = "5359-gamedata.base";

// The research a start completes: the `wanted` sids that are live RESEARCH records (unknown, wrong-type and REMOVED ones dropped, each
// once, order kept); when none is left, `_Default Start` (empty if the data has no such live record).
std::vector<std::string> startResearch(const kdata::GameData& gameData, const std::vector<std::string>& wanted);

// The reason a start-off name is refused ("bad_name"), "" if it is fine: 1..32 code points of valid UTF-8, no control characters, not
// all whitespace.
std::string checkFactionName(const std::string& name);

} // namespace ksim
