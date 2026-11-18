#include "ksim/scenario_file.hpp"

#include "ksim/clock.hpp"
#include "ksim/inventory_grid.hpp"
#include "ksim/navigator.hpp"
#include "ksim/rng.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ksim::scenario {

namespace {

[[noreturn]] void fail(const std::string& path, int line, const std::string& reason)
{
	throw std::runtime_error(path + ":" + std::to_string(line) + ": " + reason);
}

std::string decode(const std::string& in)
{
	std::string out;
	for (std::size_t i = 0; i < in.size(); ++i)
	{
		if (in[i] == '%' && i + 2 < in.size() + 0 && std::isxdigit(static_cast<unsigned char>(in[i + 1])) && std::isxdigit(static_cast<unsigned char>(in[i + 2])))
		{
			out.push_back(static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16)));
			i += 2;
		} else {
			out.push_back(in[i]);
		}
	}
	return out;
}

std::vector<std::string> splitOn(const std::string& in, char sep)
{
	std::vector<std::string> parts;
	std::size_t from = 0;
	for (;;)
	{
		const std::size_t at = in.find(sep, from);
		parts.push_back(in.substr(from, at == std::string::npos ? std::string::npos : at - from));
		if (at == std::string::npos)
			return parts;
		from = at + 1;
	}
}

using Fields = std::map<std::string, std::string>; // still encoded

struct Reader
{
	const std::string& path;
	int line;
	const Fields& f;

	bool has(const char* key) const { return f.count(key) != 0; }
	std::string str(const char* key, const char* fallback = "") const
	{
		const auto it = f.find(key);
		return it == f.end() ? fallback : decode(it->second);
	}
	std::string need(const char* key) const
	{
		if (!has(key))
			fail(path, line, std::string("missing ") + key);
		return str(key);
	}
	double num(const char* key, double fallback) const
	{
		const auto it = f.find(key);
		if (it == f.end() || it->second.empty())
			return fallback;
		try
		{
			std::size_t used = 0;
			const double v = std::stod(decode(it->second), &used);
			if (used != decode(it->second).size())
				throw std::invalid_argument("trailing");
			return v;
		}
		catch (const std::exception&)
		{
			fail(path, line, std::string("bad number for ") + key + ": " + it->second);
		}
	}
	double needNum(const char* key) const
	{
		if (!has(key))
			fail(path, line, std::string("missing ") + key);
		return num(key, 0.0);
	}
	bool flag(const char* key, bool fallback = false) const { return num(key, fallback ? 1.0 : 0.0) != 0.0; }
	// A list of `name:value` items.
	std::vector<std::pair<std::string, std::string>> pairs(const char* key) const
	{
		std::vector<std::pair<std::string, std::string>> out;
		const auto it = f.find(key);
		if (it == f.end() || it->second.empty())
			return out;
		for (const std::string& item : splitOn(it->second, ','))
		{
			const std::vector<std::string> kv = splitOn(item, ':');
			if (kv.size() != 2)
				fail(path, line, std::string("bad list item in ") + key + ": " + item);
			out.emplace_back(decode(kv[0]), decode(kv[1]));
		}
		return out;
	}
};

double toNumber(const std::string& path, int line, const std::string& key, const std::string& text)
{
	try
	{
		return std::stod(text);
	}
	catch (const std::exception&)
	{
		fail(path, line, "bad number for " + key + ": " + text);
	}
}

Char* charById(File& file, int id)
{
	for (Char& c : file.chars)
	{
		if (c.id == id)
			return &c;
	}
	return nullptr;
}

} // namespace

File parseFile(const std::string& path)
{
	std::ifstream in(path);
	if (!in)
		throw std::runtime_error("cannot read scenario file " + path);
	File file;
	file.path = path;
	std::string text;
	int line = 0;
	bool header = false;
	while (std::getline(in, text))
	{
		++line;
		if (!text.empty() && text.back() == '\r')
			text.pop_back();
		if (text.empty() || text[0] == '#')
			continue;
		std::istringstream words(text);
		std::string tag;
		words >> tag;
		if (!header)
		{
			std::string version;
			words >> version;
			if (tag != "KSCEN" || (version != "1" && version != "2"))
				fail(path, line, "not a KSCEN 1 or 2 file (first line is `KSCEN 2`)");
			file.version = version == "2" ? 2 : 1;
			header = true;
			continue;
		}
		Fields fields;
		std::string word;
		while (words >> word)
		{
			const std::size_t eq = word.find('=');
			if (eq == std::string::npos)
				fail(path, line, "expected key=value, got " + word);
			fields[word.substr(0, eq)] = word.substr(eq + 1);
		}
		const Reader r{path, line, fields};
		if (tag == "source")
		{
			file.save = r.str("save");
			file.gameVersion = r.str("game");
			file.radius = r.num("radius", 0.0);
			const std::vector<std::string> centre = splitOn(r.has("centre") ? fields["centre"] : "0,0", ',');
			if (centre.size() != 2)
				fail(path, line, "centre wants x,z");
			file.centreX = toNumber(path, line, "centre", decode(centre[0]));
			file.centreZ = toNumber(path, line, "centre", decode(centre[1]));
			file.leader = r.str("leader");
		} else if (tag == "clock") {
			file.day = static_cast<std::uint64_t>(r.needNum("day"));
			file.hour = static_cast<int>(r.needNum("hour"));
			file.minute = static_cast<int>(r.needNum("minute"));
		} else if (tag == "weather") {
			file.weatherGroup = r.need("group");
			file.weather = r.need("weather");
		} else if (tag == "camera") {
			file.cameraLine = text;
		} else if (tag == "scenario") {
			// The playtest header (task CE): what the client's Scenarios window shows and the metrics the run reports. Optional: a fixture file
			// (a test's setup) has none, and a playtest scenario is the same file plus this line.
			file.title = r.need("title");
			file.setup = r.str("setup");
			file.lookFor = r.str("look_for");
			file.bead = r.str("bead");
			for (const std::string& metric : splitOn(r.str("metrics"), ','))
			{
				const std::string name = decode(metric);
				if (name.empty())
					continue;
				// Fail loudly: a name the server has no counter for would be listed and answered as never valid (SIMLINK.md "SCENARIO").
				if (!simlink::isScenarioMetric(name))
					fail(path, line, "scenario: unknown metric '" + name + "' (SIMLINK.md SCENARIO lists the vocabulary)");
				file.metrics.push_back(name);
			}
			if (file.metrics.empty())
				fail(path, line, "scenario: no metrics (a comma list of metric names)");
			if (r.has("retarget_hold"))
			{
				file.retargetHold = r.num("retarget_hold", 0.0);
				if (*file.retargetHold < 0.0 || *file.retargetHold > 10.0)
					fail(path, line, "scenario: retarget_hold takes 0..10 seconds");
			}
			if (r.has("turret_manning"))
			{
				const double on = r.num("turret_manning", 0.0);
				if (on != 0.0 && on != 1.0)
					fail(path, line, "scenario: turret_manning takes 0 or 1");
				file.turretManning = on == 1.0;
			}
			file.hasHeader = true;
		} else if (tag == "squad") {
			Squad s;
			s.id = static_cast<int>(r.needNum("id"));
			s.name = r.need("name");
			s.faction = r.str("faction");
			s.templateSid = r.str("template");
			s.package = r.str("package");
			s.player = r.flag("player");
			s.money = static_cast<int>(r.num("money", 0.0));
			s.hostile = r.flag("hostile");
			s.war = r.flag("war");
			s.sheathed = r.flag("sheathed");
			file.squads.push_back(std::move(s));
		} else if (tag == "char") {
			Char c;
			c.id = static_cast<int>(r.needNum("id"));
			c.squad = static_cast<int>(r.needNum("squad"));
			c.name = r.need("name");
			c.templateSid = r.need("template");
			c.race = r.str("race");
			c.faction = r.str("faction");
			c.x = r.needNum("x");
			c.y = r.needNum("y");
			c.z = r.needNum("z");
			c.yaw = r.num("yaw", 0.0);
			c.leader = r.flag("leader");
			c.player = r.flag("player");
			c.passive = r.flag("passive");
			c.hold = r.flag("hold");
			c.defensive = r.flag("defensive");
			c.taunt = r.flag("taunt");
			c.chase = r.flag("chase");
			c.ranged = r.flag("ranged");
			c.stealth = r.flag("stealth");
			c.cage = r.str("cage");
			c.bounty = static_cast<int>(r.num("bounty", 0.0));
			c.peeler = r.flag("peeler");
			c.noncombat = r.flag("noncombat");
			c.shackled = r.flag("shackled");
			c.task = r.str("task");
			c.target = static_cast<int>(r.num("target", 0.0));
			if (!c.task.empty() && !c.player)
				fail(path, line, "task: char must be a player character");
			c.speed = static_cast<int>(r.num("speed", 2.0));
			c.age = r.num("age", 1.0);
			c.blood = r.num("blood", -1.0); // absent: the race's maximum, filled in by apply
			c.hunger = r.num("hunger", 3.0);
			c.fed = r.num("fed", 0.0);
			c.bleeding = r.num("bleeding", 0.0);
			c.ko = r.num("ko", 0.0);
			c.unconscious = r.flag("unconscious");
			c.coma = r.flag("coma");
			c.dead = r.flag("dead");
			c.incapacitated = r.flag("incapacitated");
			for (const auto& [name, value] : r.pairs("stats"))
				c.stats[name] = toNumber(path, line, name, value);
			if (charById(file, c.id) != nullptr)
				fail(path, line, "duplicate char id " + std::to_string(c.id));
			file.chars.push_back(std::move(c));
		} else if (tag == "order") {
			if (file.version < 2)
				fail(path, line, "`order` records need `KSCEN 2`");
			Order o;
			o.who = static_cast<int>(r.needNum("char"));
			o.attack = static_cast<int>(r.needNum("attack"));
			const Char* who = charById(file, o.who);
			const Char* target = charById(file, o.attack);
			if (who == nullptr || target == nullptr)
				fail(path, line, "order names a char that is not defined above it");
			if (who->player || who->id == target->id)
				fail(path, line, "order: char must be a non-player and not its own target");
			file.orders.push_back(o);
		} else if (tag == "building") {
			if (file.version < 2)
				fail(path, line, "`building` records need `KSCEN 2`");
			Building b;
			b.sid = r.need("sid");
			b.x = r.needNum("x");
			b.z = r.needNum("z");
			b.yaw = r.num("yaw", 0.0);
			b.built = r.num("built", 1.0) != 0.0;
			b.faction = r.str("faction");
			b.id = static_cast<int>(r.num("id", static_cast<double>(file.buildings.size() + 1)));
			for (const Building& other : file.buildings)
				if (other.id == b.id)
					fail(path, line, "duplicate building id " + std::to_string(b.id));
			file.buildings.push_back(std::move(b));
		} else if (tag == "town") {
			if (file.version < 2)
				fail(path, line, "`town` records need `KSCEN 2`");
			Town t;
			t.name = r.need("name");
			if (r.need("owner") != "player")
				fail(path, line, "town: owner must be `player`");
			file.towns.push_back(std::move(t));
		} else if (tag == "container") {
			if (file.version < 2)
				fail(path, line, "`container` records need `KSCEN 2`");
			Container c;
			c.sid = r.need("sid");
			c.x = r.needNum("x");
			c.z = r.needNum("z");
			c.owner = r.str("owner");
			for (const auto& [sid, count] : r.pairs("stock"))
				c.stock.emplace_back(sid, static_cast<std::uint32_t>(toNumber(path, line, "stock", count)));
			file.containers.push_back(std::move(c));
		} else if (tag == "raid") {
			if (file.version < 2)
				fail(path, line, "`raid` records need `KSCEN 2`");
			Raid raid;
			raid.faction = r.need("faction");
			raid.campaign = r.str("campaign");
			raid.town = r.need("town");
			raid.stay = r.num("stay", raid.stay);
			raid.retreat = r.num("retreat", raid.retreat);
			for (const std::string& id : splitOn(r.need("squads"), ','))
				raid.squads.push_back(static_cast<int>(toNumber(path, line, "squads", id)));
			file.raids.push_back(std::move(raid));
		} else if (tag == "limb" || tag == "item" || tag == "look") {
			Char* owner = charById(file, static_cast<int>(r.needNum("char")));
			if (owner == nullptr)
				fail(path, line, tag + " names a char that is not defined above it");
			if (tag == "limb")
			{
				Limb l;
				l.sid = r.need("sid");
				l.flesh = r.num("flesh", 100.0);
				l.hit = r.num("hit", 0.0);
				l.hitMult = r.num("hitmult", 1.0);
				l.wear = r.num("wear", 0.0);
				l.bandage = r.num("bandage", 0.0);
				l.stun = r.num("stun", 0.0);
				l.rig = r.num("rig", 0.0);
				owner->limbs.push_back(std::move(l));
			} else if (tag == "item") {
				Item it;
				it.sid = r.need("sid");
				it.section = r.str("section", "main");
				it.count = static_cast<std::uint32_t>(std::max(1.0, r.num("qty", 1.0)));
				it.level = static_cast<int>(r.num("level", 0.0));
				it.quality = r.num("quality", 100.0);
				it.material = r.str("material");
				it.company = r.str("company");
				it.colour = r.str("color");
				owner->items.push_back(std::move(it));
			} else {
				owner->female = r.flag("female");
				for (const auto& [key, value] : fields)
				{
					if (key != "char")
						owner->look[key] = decode(value);
				}
			}
		} else {
			fail(path, line, "unknown record " + tag);
		}
	}
	if (!header)
		throw std::runtime_error(path + ": empty file (first line is `KSCEN 2`)");
	if (file.chars.empty())
		throw std::runtime_error(path + ": no characters");
	return file;
}

void check(const kdata::GameData& gd, const File& file)
{
	std::map<std::string, std::vector<std::string>> missing;
	const auto need = [&](const std::string& sid, const std::string& why)
	{
		if (!sid.empty() && gd.find(sid) == nullptr)
			missing[sid].push_back(why);
	};
	need(file.weatherGroup, "weather group");
	need(file.weather, "weather");
	for (const Squad& s : file.squads)
	{
		need(s.faction, "squad " + s.name + " faction");
		need(s.templateSid, "squad " + s.name + " template");
		need(s.package, "squad " + s.name + " package");
	}
	for (const Char& c : file.chars)
	{
		const std::string who = c.name + " (char " + std::to_string(c.id) + ")";
		need(c.templateSid, who + " template");
		need(c.race, who + " race");
		need(c.faction, who + " faction");
		for (const Limb& l : c.limbs)
			need(l.sid, who + " limb");
		for (const Item& it : c.items)
		{
			need(it.sid, who + " item");
			need(it.material, who + " item material");
			need(it.company, who + " item company");
			need(it.colour, who + " item colour");
		}
	}
	for (const Building& b : file.buildings)
	{
		need(b.sid, "building " + std::to_string(b.id));
		need(b.faction, "building " + std::to_string(b.id) + " faction");
	}
	for (const Container& c : file.containers)
	{
		need(c.sid, "container");
		need(c.owner == "player" ? std::string() : c.owner, "container owner");
		for (const auto& [sid, count] : c.stock)
			need(sid, "container stock");
	}
	for (const Raid& raid : file.raids)
	{
		need(raid.faction, "raid faction");
		need(raid.campaign, "raid campaign");
	}
	if (missing.empty())
		return;
	std::string text = "scenario " + file.path + ": " + std::to_string(missing.size()) + " referenced record(s) are not in the loaded game data (nothing was substituted):";
	for (const auto& [sid, why] : missing)
		text += "\n  " + sid + "  <- " + why.front() + (why.size() > 1 ? " (+" + std::to_string(why.size() - 1) + " more)" : "");
	throw std::runtime_error(text);
}

namespace {

constexpr std::int32_t kHostileTeam = 1; // residents.cpp kWildTeam: hostile to everyone, a team of its own

// STATS record name -> FighterSkills field.
struct StatField
{
	const char* name;
	double state::FighterSkills::* into;
};
constexpr StatField kStatFields[] = {
	{"strength", &state::FighterSkills::strength}, {"dexterity", &state::FighterSkills::dexterity}, {"toughness2", &state::FighterSkills::toughness},
	{"athletics", &state::FighterSkills::athletics}, {"dodge", &state::FighterSkills::dodge}, {"attack", &state::FighterSkills::attack},
	{"defence", &state::FighterSkills::defence}, {"katana", &state::FighterSkills::katana}, {"sabres", &state::FighterSkills::sabres},
	{"hackers", &state::FighterSkills::hackers}, {"blunt", &state::FighterSkills::blunt}, {"heavy weapons", &state::FighterSkills::heavy},
	{"unarmed", &state::FighterSkills::unarmed}, {"poles", &state::FighterSkills::poles}, {"medic", &state::FighterSkills::medic},
	{"lockpicking", &state::FighterSkills::lockpicking}, {"stealth", &state::FighterSkills::stealth}, {"science", &state::FighterSkills::science},
	{"labouring", &state::FighterSkills::labouring},
};

void applyStats(state::FighterSkills& skills, const std::map<std::string, double>& stats)
{
	for (const StatField& field : kStatFields)
	{
		const auto it = stats.find(field.name);
		if (it != stats.end())
			skills.*field.into = it->second;
	}
}

bool isWeapon(const kdata::Record& r) { return r.type == kworld::itemTypeByName("WEAPON"); }
bool isArmour(const kdata::Record& r) { return r.type == kworld::itemTypeByName("ARMOUR"); }

// Sections that hold the item on the body; everything else is carried in the pack.
bool onBody(const std::string& section) { return section != "main" && section != "backpack_content" && section != "backpack_attach" && !section.empty(); }

state::ItemQuality qualityOf(const Item& item, bool weapon, bool coverage)
{
	state::ItemQuality q;
	if (weapon)
	{
		q.maker = item.company;
		q.model = item.material;
		q.level = item.level / 100.0;
		q.grade = state::ItemQuality::gradeOf(q.level);
	} else if (coverage) {
		q.level = item.level / 100.0;
		q.grade = state::ItemQuality::gradeOf(q.level);
	} else {
		q.level = 1.0; // clothing without part coverage: level 1, no grade (simlink_scenario.cpp makeEngival)
	}
	return q;
}

} // namespace

void addCarried(state::Inventory& pack, const kdata::GameData& gd, const std::vector<Item>& carried)
{
	for (const Item& it : carried)
	{
		const kdata::Record& record = *gd.find(it.sid);
		pack.add(it.sid, it.count, state::Inventory::kCarried, qualityOf(it, isWeapon(record), isArmour(record) && record.findRefs("part coverage") != nullptr));
	}
}

std::vector<std::string> unmappedStats(const std::map<std::string, double>& stats)
{
	std::vector<std::string> names;
	for (const auto& [name, value] : stats)
	{
		const bool mapped = std::any_of(std::begin(kStatFields), std::end(kStatFields), [&](const StatField& field) { return name == field.name; });
		if (!mapped)
			names.push_back(name);
	}
	return names;
}

// What a saved character is, applied onto `f` (built from the template by makeFighter, or by the importer from the member's own template):
// its name, sex, stats, the equipment on the body (what is carried goes to `carried`), limbs, blood and modes. Position, team and squad are the caller's.
void applyChar(const kdata::GameData& gd, const CombatData& data, const Char& c, state::Fighter& f, std::vector<Item>& carried, std::set<std::string>& unmapped)
{
	f.name = c.name;
	f.look.female = c.female ? 1 : 0; // the file's own sex, not makeFighter's roll
	applyStats(f.skills, c.stats);

	// Equipment: what is on the body replaces the template's roll. The first weapon is wielded (the sim wields one), armour pieces are worn.
	// A character with no `item` lines keeps makeFighter's roll, as a world-spawned or new-game one does (k16.5.3); one with item
	// lines gets exactly those lines.
	if (!c.items.empty())
	{
		f.weapon = data.makeFists(gd);
		f.armour.clear();
	}
	bool wielded = false;
	for (const Item& it : c.items)
	{
		const kdata::Record& record = *gd.find(it.sid);
		if (onBody(it.section) && isWeapon(record) && !wielded)
		{
			f.weapon = data.buildWeapon(gd, record, qualityOf(it, true, false));
			wielded = true;
		} else if (onBody(it.section) && isArmour(record) && record.intOr("slot", -1) != 12) {
			f.armour.push_back(data.buildArmour(record, qualityOf(it, false, record.findRefs("part coverage") != nullptr)));
		} else {
			carried.push_back(it);
			if (onBody(it.section))
				unmapped.insert("a second weapon or a backpack on the body (" + it.section + ") is carried in the pack instead");
		}
	}

	// Medical state. flesh is the part's health (a fresh Greenlander: 100 on every part, as the live med sample); UNSURE: hit / hitmult / rig.
	for (const Limb& l : c.limbs)
	{
		const kdata::Record& limb = *gd.find(l.sid);
		bool found = false;
		for (state::FighterPart& part : f.parts)
		{
			if (part.name != limb.name)
				continue;
			part.health = l.flesh;
			part.stun = l.stun;
			part.wear = l.wear;
			part.pool = l.bandage;
			found = true;
		}
		if (!found)
			throw std::runtime_error("scenario: " + c.name + " (char " + std::to_string(c.id) + ") has no body part '" + limb.name + "' in its race (" + f.raceSid + ")");
	}
	// A file that names no `blood` starts the character at the race's maximum, as a freshly built one does
	// (CombatData::makeFighter): the old 0.0 default left the tank empty and the first wound killed it. Recomputed here because
	// the file's `stats=` can change the strength the maximum scales with.
	f.blood = c.blood >= 0.0 ? c.blood : f.minBlood + (f.maxBloodRace - f.minBlood) * f.skills.strength * 0.01;
	f.hunger = c.hunger;
	f.bleeding = c.bleeding;
	if (c.bleeding > 0.0)
		f.wounds = {c.bleeding}; // UNSURE: the save's `bleeding` is the total leak, here one wound at that rate
	f.dead = c.dead;
	f.unconscious = c.unconscious || c.coma || c.incapacitated;
	f.koTimer = c.ko;
	f.blockMode = c.defensive;
	f.sneak = c.stealth;
	f.moveMode = static_cast<std::uint8_t>(std::clamp(c.speed, 0, 2)); // UNSURE: the original's speed 3 (Group) loads as Run; the group key is not in the scenario
	f.passive = c.passive;
	f.holdPosition = c.hold;
	if (c.taunt) unmapped.insert("taunt mode (no sim counterpart)");
	if (c.noncombat) unmapped.insert("non combat mode (no sim counterpart)");
	if (!c.chase) unmapped.insert("chase mode off (the sim always chases)");
	if (!c.ranged) unmapped.insert("ranged mode off (no sim counterpart)");
}


Placed apply(Simulation& sim, const kdata::GameData& gd, const CombatData& data, simlink::ServerContext& context, const File& file,
	std::uint64_t seed)
{
	check(gd, file);
	const MeshNavigator* nav = sim.navigation();
	if (nav == nullptr || sim.combat() == nullptr)
		throw std::runtime_error("scenario: needs combat and a navmesh enabled (Simulation::enableCombat / enableNavigation)");
	Placed placed;

	// The server flag over the header over the exe's 0 (a run never inherits the last scenario's pin).
	sim.setRetargetHold(context.retargetHold.value_or(file.retargetHold.value_or(CombatSettings{}.retargetHold)));
	if (context.log)
		context.log("combat.retarget_hold_s " + std::to_string(sim.retargetHold()));
	sim.setTurretManning(context.turretManning.value_or(file.turretManning.value_or(false)));
	// The pristine world's stamps are those of a new game's start: a clock before it puts them in the future (kenshi-k16.6.65, hazard 6).
	const std::uint64_t startAt = tickAt(file.day, file.hour, file.minute);
	if (startAt < tickAt(kNewGameDay, kNewGameHour, kNewGameMinute))
		throw std::runtime_error("scenario: the clock (day " + std::to_string(file.day) + " " + std::to_string(file.hour) + ":" + std::to_string(file.minute)
			+ ") is before a new game's start (day 1 10:00)");
	sim.setClockTick(startAt);
	if (!file.weatherGroup.empty())
	{
		if (!sim.pinWeather(file.weatherGroup, file.weather))
			throw std::runtime_error("scenario: the weather " + file.weather + " could not be set on " + file.weatherGroup + " (weather off or no such region)");
	} else {
		placed.notes.push_back("no weather in the file: the world's own weather runs");
	}

	// The streaming mesh holds the zones around every character (a mesh without streaming has what it has).
	if (nav->streaming())
	{
		std::vector<kworld::ZoneCell> wanted = nav->coveredZones();
		for (const Char& c : file.chars)
		{
			const kworld::ZoneCell at = kworld::zoneCellOf(c.x, c.z);
			for (int dz = -1; dz <= 1; ++dz)
			{
				for (int dx = -1; dx <= 1; ++dx)
					wanted.push_back({at.x + dx, at.z + dz});
			}
		}
		nav->setCoverage(wanted);
	}

	// Every character is built before anything is added: a record the combat data cannot build, or ground off the mesh, refuses the whole file.
	struct Built
	{
		const Char* source;
		state::Fighter fighter;
		std::vector<Item> carried;
	};
	std::vector<Built> built;
	std::vector<std::string> offMesh;
	std::set<std::string> unmapped;
	std::uint64_t rng = 12345 + 1000003 * seed;
	std::map<int, const Squad*> squadOf;
	for (const Squad& s : file.squads)
		squadOf[s.id] = &s;
	for (const Char& c : file.chars)
	{
		const auto squad = squadOf.find(c.squad);
		// Task CC: the character's stable dice identity, the file's own per-character spawn value (the build rng before makeFighter
		// advances it), not its entity Id: an unrelated entity spawned first moves every Id and used to re-roll the whole fight.
		const std::uint64_t dice = splitMix64(rng);
		state::Fighter f = data.makeFighter(gd, c.templateSid, c.race, rng, squad != squadOf.end() ? squad->second->templateSid : std::string());
		f.diceSeed = dice;
		f.playerControlled = c.player;
		f.yaw = c.yaw;
		std::vector<Item> carried;
		applyChar(gd, data, c, f, carried, unmapped);

		// On the navmesh's nearest walkable ground, from the saved height; open ground takes the terrain's height like a step does
		// (the runtime mesh floats up to 1.14 u above it, kenshi-k16.6.273).
		const auto snapped = nav->walker().snap({c.x, c.y, c.z}, 100.0);
		if (!snapped)
		{
			offMesh.push_back(c.name + " (char " + std::to_string(c.id) + ") at (" + std::to_string(c.x) + ", " + std::to_string(c.z) + ")");
			continue;
		}
		const knav::Position spot = nav->walker().settle(*snapped);
		f.x = spot.x;
		f.y = spot.y;
		f.z = spot.z;
		built.push_back({&c, std::move(f), std::move(carried)});
	}
	if (!offMesh.empty())
	{
		std::string text = "scenario: " + std::to_string(offMesh.size()) + " character(s) are not within reach of the navmesh (nothing was placed):";
		for (const std::string& line : offMesh)
			text += "\n  " + line;
		throw std::runtime_error(text);
	}
	for (const std::string& note : unmapped)
		placed.notes.push_back("not applied: " + note);

	// The player faction and its squads (the squads' order is the file's, the leader first).
	std::string playerFaction;
	for (const Squad& s : file.squads)
	{
		if (s.player && !s.faction.empty())
			playerFaction = s.faction;
	}
	std::map<int, std::uint64_t> playerSquadId;
	if (!playerFaction.empty())
	{
		sim.createPlayerFaction(playerFaction);
		for (const Squad& s : file.squads)
		{
			if (s.player)
				playerSquadId[s.id] = sim.addPlayerSquad(s.name);
		}
	}

	// Every non-player squad of the file is a world squad (SquadInstance), so its characters get the squad AI, the residents' routines and the
	// perception of the normal world: a default-world squad of the same template inside the area is claimed (it keeps its town, den and traders),
	// else a new one is made. The default-world squads left in the area are removed, so the area holds what the file says and nothing doubled.
	std::map<int, flecs::entity> squadEntity;
	std::set<flecs::entity_t> taken;
	for (const Squad& s : file.squads)
	{
		if (s.player)
			continue;
		double x = 0.0;
		double z = 0.0;
		int n = 0;
		for (const Built& b : built)
		{
			if (b.source->squad == s.id)
			{
				x += b.fighter.x;
				z += b.fighter.z;
				++n;
			}
		}
		if (n == 0)
			continue;
		bool claimed = false;
		const flecs::entity squad = sim.scenarioSquad(s.templateSid, s.faction, x / n, z / n, file.centreX, file.centreZ, file.radius, s.hostile, taken, &claimed);
		taken.insert(squad.id());
		squadEntity[s.id] = squad;
		placed.claimedSquads += claimed ? 1 : 0;
	}
	for (const Squad& s : file.squads)
	{
		const state::EntityRef enemy = s.war && !s.player && !playerFaction.empty() ? sim.importFaction(s.faction) : state::EntityRef{};
		if (enemy.id == 0)
			continue;
		flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());
		const state::Id them = *flecs::entity(ecs.c_ptr(), enemy.id).get<state::Id>();
		const state::Id us = *flecs::entity(ecs.c_ptr(), sim.importFaction(playerFaction).id).get<state::Id>();
		sim.setWar(them, us);
	}
	placed.removedSquads = sim.removeWorldSquadsNear(file.centreX, file.centreZ, file.radius, taken);
	// Task BM: the file's area stays its own for the rest of the session. A default-world squad that appears there later (the
	// resident pass materialising a camp whose den is at the file's spot, a town population on a later load) would push the file's
	// fighters apart, so it is removed at its first tick unless the file owns it. A file with no radius describes no area.
	if (file.radius > 0.0)
		sim.holdScenarioArea(file.centreX, file.centreZ, file.radius, taken);

	// Players first (leader first), then the rest in file order, so the Ids are stable.
	std::stable_sort(built.begin(), built.end(), [](const Built& a, const Built& b)
	{
		if (a.source->player != b.source->player)
			return a.source->player;
		return a.source->leader && !b.source->leader;
	});
	for (Built& b : built)
	{
		const Char& c = *b.source;
		const std::string faction = !c.faction.empty() ? c.faction : squadOf.count(c.squad) != 0 ? squadOf[c.squad]->faction : std::string();
		b.fighter.team = c.player ? 0 : sim.teamOfFaction(faction);
		if (!c.player && b.fighter.team == 0 && squadOf.count(c.squad) != 0 && squadOf[c.squad]->hostile)
			b.fighter.team = kHostileTeam; // the file says they fight the player; the sim's relation rule would not (see Squad::hostile)
		flecs::entity entity;
		if (c.player)
		{
			entity = sim.spawnFighter(std::move(b.fighter));
		} else {
			const auto squad = squadEntity.find(c.squad);
			if (squad == squadEntity.end())
				throw std::runtime_error("scenario: " + c.name + " (char " + std::to_string(c.id) + ") names squad " + std::to_string(c.squad) + " which the file does not define");
			entity = sim.scenarioMember(squad->second, c.templateSid, c.leader, std::move(b.fighter));
		}
		const std::uint64_t id = entity.get<state::Id>()->value;
		if (!faction.empty())
			entity.set<state::Allegiance>(state::Allegiance{faction});
		if (!b.carried.empty())
		{
			state::Inventory pack = *entity.get<state::Inventory>();
			addCarried(pack, gd, b.carried);
			grid::settle(pack, grid::Rules(gd), id);
			entity.set<state::Inventory>(std::move(pack));
		}
		simlink::CharLook look;
		look.factionSid = faction;
		look.sex = c.female ? 1 : 0;
		context.looks[id] = look;
		placed.idOf[c.id] = id;
		if (!c.cage.empty() && !sim.imprisonScenario(id, c.cage, c.bounty, c.peeler))
			throw std::runtime_error("scenario: " + c.name + " (char " + std::to_string(c.id) + ") names cage=" + c.cage + " and no free cage matches");
		if (c.player)
		{
			placed.players.push_back(id);
			sim.addPlayerMember(playerSquadId.at(c.squad), id);
		} else {
			placed.others.push_back(id);
			const auto squad = std::find_if(file.squads.begin(), file.squads.end(), [&](const Squad& s) { return s.id == c.squad; });
			if (squad != file.squads.end() && squad->sheathed)
				sim.sheatheWeapon(id);
		}
	}
	// The full-detail area: where the player characters stand, as a client's SetPlayerAreas gives it (a scenario faction has no start-off, so nothing
	// else sets it; without it every squad stays at the token level and no resident gets its routine). A player-less file (spectate_fight,
	// spectator mode) sets it at the file's centre instead: the area the spectator watches, with no player to stand anywhere.
	{
		std::vector<state::GroundPoint> centres;
		for (const std::uint64_t id : placed.players)
			centres.push_back({sim.combat()->find(id)->x, sim.combat()->find(id)->z});
		if (centres.empty() && !file.squads.empty())
			centres.push_back({file.centreX, file.centreZ});
		if (!centres.empty())
			sim.enqueue(Command::setPlayerAreas(std::move(centres)));
	}
	// The buildings, then the shackles and the player tasks that name them. Everything stands where the file puts it: no walk is planned.
	for (const Building& b : file.buildings)
	{
		std::string reason;
		const std::uint64_t id = sim.placeScenarioBuilding(b.sid, b.x, b.z, b.yaw, b.built, b.faction, reason);
		if (id == 0)
			throw std::runtime_error("scenario: building " + std::to_string(b.id) + " (" + b.sid + ") could not be placed at " + std::to_string(b.x) + ", " + std::to_string(b.z) + ": " + reason);
		placed.buildingOf[b.id] = id;
	}
	// Towns that change hands, the storage boxes (unlocked, stocked) and the forced raids: all of it where the file puts it.
	for (const Town& t : file.towns)
	{
		if (!sim.scenarioTownToPlayer(t.name, file.centreX, file.centreZ))
			throw std::runtime_error("scenario: town `" + t.name + "` is not in the world or the file has no player faction");
	}
	for (const Container& c : file.containers)
	{
		const std::optional<double> ground = sim.groundHeight(c.x, c.z);
		if (!ground)
			throw std::runtime_error("scenario: container " + c.sid + " has no ground at " + std::to_string(c.x) + ", " + std::to_string(c.z));
		state::Container box;
		box.recordSid = c.sid;
		box.x = box.useX = c.x;
		box.y = box.useY = *ground;
		box.z = box.useZ = c.z;
		box.ownerSid = c.owner == "player" ? sim.playerFactionSid() : c.owner;
		state::Inventory stock;
		stock.version = 1;
		for (const auto& [sid, count] : c.stock)
			stock.add(sid, count);
		sim.spawnContainer(std::move(box), std::move(stock));
	}
	for (const Raid& raid : file.raids)
	{
		std::vector<flecs::entity> force;
		for (const int id : raid.squads)
		{
			const auto squad = squadEntity.find(id);
			if (squad == squadEntity.end())
				throw std::runtime_error("scenario: raid names squad " + std::to_string(id) + " which the file does not define as a non-player squad with characters");
			force.push_back(squad->second);
		}
		if (!sim.scenarioRaid(raid.faction, raid.campaign, raid.town, file.centreX, file.centreZ, force, static_cast<std::uint64_t>(raid.stay * kTicksPerGameMinute),
				static_cast<std::uint64_t>(raid.retreat * kTicksPerGameMinute)))
			throw std::runtime_error("scenario: raid of " + raid.faction + " on town `" + raid.town + "` could not be set up (no such town)");
	}
	for (const Char& c : file.chars)
	{
		const std::uint64_t id = placed.idOf.at(c.id);
		const auto fail = [&](const std::string& why) { return std::runtime_error("scenario: " + c.name + " (char " + std::to_string(c.id) + ") " + why); };
		if (c.shackled && !sim.scenarioTask(id, "shackle", 0).empty())
			throw fail("could not be shackled");
	}
	for (const Char& c : file.chars)
	{
		const std::uint64_t id = placed.idOf.at(c.id);
		const auto fail = [&](const std::string& why) { return std::runtime_error("scenario: " + c.name + " (char " + std::to_string(c.id) + ") " + why); };
		if (c.task.empty())
			continue;
		const bool onChar = c.task.ends_with("_shackles");
		const auto& names = onChar ? placed.idOf : placed.buildingOf;
		const auto target = names.find(c.target);
		if (c.task != "pick_cage" && c.task != "cut_cage" && target == names.end())
			throw fail("task=" + c.task + " names target=" + std::to_string(c.target) + " which the file does not define");
		if (const std::string refused = sim.scenarioTask(id, c.task, target != names.end() ? target->second : 0); !refused.empty())
			throw fail("task=" + c.task + " was refused: " + refused);
	}
	// The initial attack orders (KSCEN 2), given at the first tick like the player's: the same order a player gives (Command::fighterAttack).
	for (const Order& o : file.orders)
	{
		const std::uint64_t who = placed.idOf.at(o.who);
		sim.enqueue(Command::fighterAttack(state::Id{who}, state::Id{placed.idOf.at(o.attack)}));
		placed.attackers.push_back(who);
		++placed.orders;
	}
	placed.notes.push_back("appearance: only the sex is applied (heads, hair and the morphs are kept in the file)");
	return placed;
}

std::uint64_t fileHash(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw std::runtime_error("cannot read scenario file " + path);
	std::uint64_t hash = 14695981039346656037ull;
	char byte = 0;
	while (in.get(byte))
		hash = (hash ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
	return hash;
}

std::vector<ScenarioInfo> listScenarios(const std::string& dir)
{
	std::vector<ScenarioInfo> found;
	std::error_code error;
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, error))
	{
		if (!entry.is_regular_file() || entry.path().extension() != ".kscen")
			continue;
		File file = parseFile(entry.path().string()); // a file that does not parse throws, header or not
		if (!file.hasHeader)
			continue; // a fixture, not a playtest scenario
		ScenarioInfo info;
		info.name = entry.path().stem().string();
		info.path = entry.path().string();
		info.title = file.title;
		info.setup = file.setup;
		info.lookFor = file.lookFor;
		info.bead = file.bead;
		info.metrics = file.metrics;
		info.fileHash = fileHash(info.path);
		found.push_back(std::move(info));
	}
	if (error)
		throw std::runtime_error("cannot list scenarios in " + dir + ": " + error.message());
	std::sort(found.begin(), found.end(), [](const ScenarioInfo& a, const ScenarioInfo& b) { return a.name < b.name; });
	return found;
}

} // namespace ksim::scenario
