// Original save -> our world (docs/reference/SAVE_IMPORT.md, kenshi-cym.2).
#include "ksim/save/save_import.hpp"

#include "ksim/ai/jobs.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/save/scenario_file.hpp"

#include "kdata/format.hpp"
#include "kworld/coords.hpp"
#include "kworld/zones.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace ksim::saveimport {

namespace {

namespace fs = std::filesystem;
using Handle = std::pair<std::int32_t, std::int32_t>; // the original's (pool index C, serial I)

// A record's fields by name (the container keeps one list per type; a save's names are unique per record).
struct View
{
	const kdata::Record* record = nullptr;

	bool has(std::string_view key) const
	{
		return record->findInt(key) != nullptr || record->findBool(key) != nullptr || record->findFloat(key) != nullptr || record->findString(key) != nullptr
			|| record->findFile(key) != nullptr;
	}
	double num(std::string_view key, double fallback = 0.0) const
	{
		if (const kdata::FieldInt* field = record->findInt(key))
			return field->value;
		if (const kdata::FieldBool* field = record->findBool(key))
			return field->value ? 1.0 : 0.0;
		if (const kdata::FieldFloat* field = record->findFloat(key))
			return kdata::decodeFloat(*field);
		return fallback;
	}
	bool flag(std::string_view key) const { return num(key) != 0.0; }
	std::string str(std::string_view key) const
	{
		if (const kdata::FieldString* field = record->findString(key))
			return field->value;
		return record->fileOr(key);
	}
	std::optional<std::array<double, 3>> vec3(std::string_view key) const
	{
		for (const kdata::FieldVec3& field : record->vec3s)
		{
			if (field.key != key)
				continue;
			float values[3];
			std::memcpy(values, field.value.data(), sizeof values);
			return std::array<double, 3>{values[0], values[1], values[2]};
		}
		return std::nullopt;
	}
	std::optional<std::array<double, 4>> vec4(std::string_view key) const
	{
		for (const kdata::FieldVec4& field : record->vec4s)
		{
			if (field.key != key)
				continue;
			float values[4];
			std::memcpy(values, field.value.data(), sizeof values);
			return std::array<double, 4>{values[0], values[1], values[2], values[3]};
		}
		return std::nullopt;
	}
	// A five-int object pointer `<prefix>C/I/CS/S/TYPE`; null when absent or of type 11 (the null type: a freed pointer keeps its old C / I, 681 such
	// `slaver` handles in SkinHouse against 294 real ones).
	std::optional<Handle> handle(const std::string& prefix) const
	{
		if (!has(prefix + "C") && !has(prefix + "TYPE"))
			return std::nullopt;
		const auto c = static_cast<std::int32_t>(num(prefix + "C"));
		const auto i = static_cast<std::int32_t>(num(prefix + "I"));
		const double type = has(prefix + "TYPE") ? num(prefix + "TYPE") : 11.0;
		if (type == 11.0)
			return std::nullopt;
		return Handle{c, i};
	}
};

kdata::File readFile(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw std::runtime_error("save import: cannot read " + path.string());
	std::vector<char> text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::vector<std::byte> bytes(text.size());
	std::memcpy(bytes.data(), text.data(), text.size());
	return kdata::parse(bytes, path.filename().string());
}

// `.TECH.N` suffixes are research level references, not sids (SAVE_IMPORT.md).
std::string stripTech(const std::string& sid)
{
	const std::size_t at = sid.find(".TECH.");
	return at == std::string::npos ? sid : sid.substr(0, at);
}

// A finished-research entry's sid: the `.TECH.N` and the trailing `.N` level suffix folded off.
std::string researchSid(const kdata::GameData& gd, const std::string& raw)
{
	std::string sid = stripTech(raw);
	const std::size_t dot = sid.rfind('.');
	if (gd.find(sid) == nullptr && dot != std::string::npos && dot + 1 < sid.size()
		&& std::all_of(sid.begin() + static_cast<std::ptrdiff_t>(dot) + 1, sid.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
		sid.resize(dot);
	return sid;
}

bool looksLikeSid(const std::string& text)
{
	std::size_t digits = 0;
	while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits])))
		++digits;
	return digits > 0 && digits < text.size() && text[digits] == '-' && digits + 1 < text.size();
}

bool hairMod(const std::string& sid)
{
	const auto endsWith = [&](const char* tail) { return sid.size() >= std::strlen(tail) && sid.compare(sid.size() - std::strlen(tail), std::string::npos, tail) == 0; };
	return endsWith("-AF Hair.mod") || endsWith("-roro hair.mod");
}

struct Member
{
	scenario::Char c;
	Handle handle{0, 0};
	std::optional<Handle> slaver;
	std::optional<Handle> carrying;
	std::uint8_t slaveState = 0;
	double slaveStamp = -1.0; // `ssct`: the world-clock game hour of the last change into slave state 2 or 3 (rwh.106; the sim's Fighter::slaveStamp); -1 = none
	std::vector<state::Bounty> bounties; // `amount#` / `bountyfac#` / `bountyexp#` / `crimes#` / `claim#`, by faction sid
	// `sentence` (the game hour the sentence began) and `psts` (its length in hours), read as CrimeRecord::sentenceStart / sentenceHours (UNSURE, 3b); 0 / 0 = none.
	double sentenceStart = 0.0;
	double sentenceHours = 0.0;
	// GAMESTATE_AI of a player member: `pjob<i>` (+ `pjobT<i>*`) the persistent jobs, `tjob<i>` (+ `tjobT<i>*`, `tjobP<i>`) the one-shot orders (rwh92).
	struct SavedJob
	{
		std::uint16_t type = 0;
		std::optional<Handle> target;
		std::optional<std::array<double, 3>> point;
	};
	std::vector<SavedJob> pjobs;
	std::vector<SavedJob> tjobs;
};

struct Platoon
{
	std::string name;
	std::string file; // "" = never activated
	std::string factionSid;
	std::string squadTemplate;
	std::string hometown; // the town record's name, "" = none
	std::int32_t pool = 0;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	std::size_t charCount = 0;
	bool resident = false;
	bool imprisoned = false;
	bool dead = false;
	int money = 0;
	bool player = false;
	std::vector<Member> members;
	std::size_t result = 0; // its place in Report::platoons
};

} // namespace

Report importSave(Simulation& sim, const kdata::GameData& gd, const CombatData& data, const std::string& dir, const Options& options)
{
	Report report;
	if (sim.combat() == nullptr)
		throw std::runtime_error("save import: needs combat enabled (Simulation::enableCombat)");
	const fs::path root = dir;
	const kdata::File quick = readFile(root / "quick.save");
	const auto recordsOf = [&](const kdata::File& file, const char* type)
	{
		std::vector<View> out;
		const std::int32_t id = kworld::itemTypeByName(type);
		for (const kdata::Record& record : file.records)
		{
			if (record.type == id)
				out.push_back(View{&record});
		}
		return out;
	};
	std::map<std::string, std::vector<std::string>> missing; // sid -> what refers to it
	const auto need = [&](const std::string& raw, const std::string& why)
	{
		const std::string sid = stripTech(raw);
		if (!sid.empty() && gd.find(sid) == nullptr)
			missing[sid].push_back(why);
	};

	// The camera and the player faction.
	const std::vector<View> cameras = recordsOf(quick, "CAMERA");
	if (cameras.empty())
		throw std::runtime_error("save import: quick.save has no CAMERA record");
	const View& camera = cameras.front();
	const std::string playerFactionName = camera.str("pfaction name");
	if (const auto pos = camera.vec3("pos"))
	{
		report.hasCamera = true;
		report.cameraPos = *pos;
		report.cameraZoom = camera.num("zoom");
		if (const auto rot = camera.vec4("rot"))
			report.cameraRot = *rot;
	}

	// CAMERA's `usedUniques#`: the unique NPCs already spawned (3b). The handle is resolved after the squads are built.
	struct UniqueRow
	{
		state::WorldEventState::Used used;
		std::optional<Handle> handle;
	};
	std::vector<UniqueRow> uniqueRows;
	for (int n = 0; n < static_cast<int>(camera.num("num usedUniques")); ++n)
	{
		const std::string at = std::to_string(n);
		UniqueRow row;
		row.used.sid = camera.str("usedUniques" + at);
		need(row.used.sid, "used unique " + at);
		row.used.state = static_cast<std::int32_t>(camera.num("usedUniquesState" + at));
		row.used.player = camera.flag("usedUniquesPlayer" + at);
		row.handle = camera.handle("usedUniques" + at + "handle");
		uniqueRows.push_back(std::move(row));
	}

	// The towns by pool index (a platoon's hometown handle points at one): the record's name is "Town state <town name>".
	std::map<std::int32_t, std::string> townName;
	struct TownRow
	{
		std::string instance;
		std::string owner;
		std::string replacement;
		std::string name; // `name`, set for a town the player named
		state::TownInfo info;
	};
	std::vector<TownRow> townRows;
	for (const View& town : recordsOf(quick, "GAMESTATE_TOWN"))
	{
		if (town.num("handTYPE") != 13.0)
			continue;
		townRows.push_back({town.str("instance"), town.str("faction"), town.str("replacementTown"), town.str("name"), {}});
		TownRow& row = townRows.back();
		need(row.owner, "town " + town.record->name + " owner");
		need(row.replacement, "town " + town.record->name + " replacementTown");
		state::TownInfo& info = row.info;
		info.known = true;
		info.isPublic = town.flag("public");
		info.level = static_cast<std::int32_t>(town.num("plevel"));
		info.material = town.str("building material");
		need(info.material, "town " + town.record->name + " building material");
		info.started = town.flag("started");
		info.explored = town.flag("explored");
		info.discovered = town.flag("discovered");
		info.recentlyDiscovered = town.flag("recently discovered");
		info.ded = town.flag("ded");
		info.pop = town.num("pop");
		info.popd = town.num("popd");
		info.pop2 = town.num("pop2");
		info.popd2 = town.num("popd2");
		for (int n = 0; town.has("factions here" + std::to_string(n)); ++n)
		{
			info.factionsHere.push_back(town.str("factions here" + std::to_string(n)));
			need(info.factionsHere.back(), "town " + town.record->name + " factions here");
		}
		std::string name = town.record->name;
		if (name.rfind("Town state ", 0) == 0)
			name = name.substr(11);
		townName[static_cast<std::int32_t>(town.num("handC"))] = name;
	}

	// Stage 3a: the rest of the world-wide records, parsed (and their sids checked) before anything is built.
	struct FactionRow
	{
		std::string sid;
		double prosperity = 0.0;
		bool hasProsperity = false;
		std::vector<std::pair<std::string, double>> relations;
		std::int32_t rank = 0;
		double globalTrust = 0.0;
		std::vector<state::FactionState::Trust> trust; // the non-zero `trust#` / `trustNeg#` rows, by the row's relationSID#
	};
	std::vector<FactionRow> factionRows;
	for (const View& record : recordsOf(quick, "GAMESTATE_FACTION"))
	{
		FactionRow row;
		row.sid = record.str("gamedata stringID");
		row.hasProsperity = record.has("prosperity");
		row.prosperity = record.num("prosperity");
		need(row.sid, "faction " + record.record->name);
		for (int n = 0; record.has("relationSID" + std::to_string(n)); ++n)
		{
			const std::string other = record.str("relationSID" + std::to_string(n));
			need(other, "faction " + record.record->name + " relation");
			row.relations.push_back({other, record.num("relation" + std::to_string(n))});
			const auto at = std::to_string(n);
			if (record.num("trust" + at) != 0.0 || record.num("trustNeg" + at) != 0.0)
				row.trust.push_back({other, record.num("trust" + at), record.num("trustNeg" + at)});
		}
		row.rank = static_cast<std::int32_t>(record.num("rank"));
		row.globalTrust = record.num("global trust");
		factionRows.push_back(std::move(row));
	}
	std::vector<std::string> researchSids;
	for (const View& record : recordsOf(quick, "RESEARCH"))
	{
		const auto keep = [&](const std::string& key, const std::string& raw)
		{
			if (key.rfind("finished", 0) != 0)
				return;
			const std::string sid = researchSid(gd, raw);
			need(sid, "finished research");
			const kdata::Record* found = gd.find(sid);
			if (found != nullptr && found->type == kworld::itemTypeByName("RESEARCH") && !found->boolOr("REMOVED", false))
			{
				if (std::find(researchSids.begin(), researchSids.end(), sid) == researchSids.end())
					researchSids.push_back(sid);
			} else if (found != nullptr) {
				++report.world.researchDropped;
			}
		};
		for (const kdata::FieldString& field : record.record->strings)
			keep(field.key, field.value);
		for (const kdata::FieldFile& field : record.record->files)
			keep(field.key, field.value);
	}
	std::vector<Simulation::ImportWeather> weatherSlots;
	for (const View& record : recordsOf(quick, "BIOMES"))
	{
		for (int n = 0; record.has(std::to_string(n) + "_w_w_id") || record.has(std::to_string(n) + "_w_s_id"); ++n)
		{
			const std::string key = std::to_string(n) + "_w_";
			Simulation::ImportWeather slot;
			slot.weather = record.str(key + "w_id");
			slot.season = record.str(key + "s_id");
			slot.strength = record.num(key + "wi_str");
			slot.effectStrength = record.num(key + "wi_ef_str");
			slot.startMinute = static_cast<std::uint64_t>(record.num(key + "wi_start"));
			slot.endMinute = static_cast<std::uint64_t>(record.num(key + "wi_end"));
			slot.seasonEndDay = static_cast<std::uint64_t>(record.num(key + "s_end"));
			weatherSlots.push_back(std::move(slot));
		}
	}

	// Phase 1: every PLATOON record and the file it names, parsed. Nothing is built before every sid is known to resolve.
	std::vector<Platoon> platoons;
	std::set<std::string> named;
	const kdata::GameData& data_ = gd;
	(void)data_;
	for (const View& record : recordsOf(quick, "PLATOON"))
	{
		Platoon p;
		p.name = record.record->name;
		p.file = record.str("content file");
		if (p.file == "platoon/" || p.file.empty())
			p.file.clear();
		p.factionSid = record.str("faction stringID");
		p.squadTemplate = record.str("squad template");
		p.pool = static_cast<std::int32_t>(record.num("handleC"));
		if (const auto at = record.vec3("position"))
		{
			p.x = (*at)[0];
			p.y = (*at)[1];
			p.z = (*at)[2];
		}
		p.charCount = static_cast<std::size_t>(record.num("char count"));
		p.resident = record.flag("is resident");
		p.imprisoned = record.flag("imprisoned");
		p.dead = record.flag("dead");
		p.money = static_cast<int>(record.num("money"));
		p.player = !playerFactionName.empty() && record.str("faction name") == playerFactionName;
		if (const auto home = record.handle("hometown"); home && record.num("hometownTYPE") == 13.0)
		{
			const auto at = townName.find(home->first);
			if (at != townName.end())
				p.hometown = at->second;
		}
		need(p.factionSid, "platoon " + p.name + " faction");
		need(p.squadTemplate, "platoon " + p.name + " squad template");
		PlatoonResult result;
		result.name = p.name;
		result.file = p.file;
		p.result = report.platoons.size();
		report.platoons.push_back(result);
		if (!p.file.empty())
			named.insert(fs::path(p.file).filename().string());
		platoons.push_back(std::move(p));
	}

	for (Platoon& p : platoons)
	{
		PlatoonResult& result = report.platoons[p.result];
		if (p.file.empty())
			continue;
		const fs::path path = root / p.file;
		if (!fs::exists(path))
		{
			result.outcome = "skipped";
			result.reason = "content file missing";
			continue;
		}
		const kdata::File file = readFile(path);
		std::unordered_map<std::string, const kdata::Record*> bySid;
		const kdata::Record* collection = nullptr;
		for (const kdata::Record& record : file.records)
		{
			bySid[record.stringID] = &record;
			if (collection == nullptr && record.type == kworld::itemTypeByName("INSTANCE_COLLECTION"))
				collection = &record;
		}
		if (collection == nullptr)
		{
			result.outcome = "skipped";
			result.reason = "content file holds no INSTANCE_COLLECTION";
			continue;
		}
		const std::int32_t typeOf[] = {kworld::itemTypeByName("GAMESTATE_CHARACTER"), kworld::itemTypeByName("MEDICAL_STATE"), kworld::itemTypeByName("STATS"),
			kworld::itemTypeByName("CHARACTER_APPEARANCE"), kworld::itemTypeByName("INVENTORY_STATE"), kworld::itemTypeByName("GAMESTATE_AI")};
		for (const kdata::Instance& instance : collection->instances)
		{
			if (instance.targetStringID.empty())
				continue;
			std::map<std::int32_t, View> state;
			for (const std::string& sid : instance.states)
			{
				const auto found = bySid.find(sid);
				if (found != bySid.end())
					state[found->second->type] = View{found->second};
			}
			if (state.count(typeOf[0]) == 0 || state.count(typeOf[1]) == 0 || state.count(typeOf[2]) == 0)
			{
				++report.gaps["instances without a character, medical or stats record (not imported)"];
				continue;
			}
			const View& ch = state.at(typeOf[0]);
			const View& med = state.at(typeOf[1]);
			const View& stats = state.at(typeOf[2]);
			Member m;
			scenario::Char& c = m.c;
			const std::string who = p.name + "/" + ch.str("name");
			c.name = ch.str("name");
			c.templateSid = instance.targetStringID;
			c.faction = ch.str("owner faction ID");
			need(c.templateSid, who + " template");
			need(c.faction, who + " faction");
			float xform[7];
			std::memcpy(xform, instance.transform.data(), sizeof xform);
			c.x = xform[0];
			c.y = xform[1];
			c.z = xform[2];
			c.yaw = std::atan2(2.0 * (xform[3] * xform[5] + xform[4] * xform[6]), 1.0 - 2.0 * (xform[5] * xform[5] + xform[4] * xform[4]));
			c.leader = ch.flag("is leader");
			c.player = p.player;
			c.passive = ch.flag("passive mode");
			c.defensive = ch.flag("defensive mode");
			c.taunt = ch.flag("taunt mode");
			c.chase = ch.flag("chase mode");
			c.ranged = ch.flag("ranged mode");
			c.stealth = ch.flag("stealth");
			c.noncombat = ch.flag("non combat mode");
			c.speed = static_cast<int>(ch.num("speed mode", 2.0));
			c.age = ch.num("age", 1.0);
			c.blood = med.num("blood", -1.0);
			c.hunger = med.num("hung", 3.0);
			c.fed = med.num("fed");
			c.bleeding = med.num("bleeding");
			c.ko = med.num("KO");
			c.unconscious = med.flag("unconcious");
			c.coma = med.flag("coma");
			c.dead = med.flag("dead");
			c.incapacitated = med.flag("incapacitated");
			for (const kdata::FieldFloat& field : stats.record->floats)
				c.stats[field.key] = kdata::decodeFloat(field);
			for (int i = 0; i < 7; ++i)
			{
				const std::string n = std::to_string(i);
				const std::string sid = med.str("sid" + n);
				if (sid.empty())
					continue;
				need(sid, who + " limb " + n);
				scenario::Limb limb;
				limb.sid = sid;
				limb.flesh = med.num("flesh" + n);
				limb.hit = med.num("hit" + n);
				limb.hitMult = med.num("hitmult" + n);
				limb.wear = med.num("wear" + n);
				limb.bandage = med.num("bandage" + n);
				limb.stun = med.num("stun" + n);
				limb.rig = med.num("rig" + n);
				c.limbs.push_back(limb);
			}
			if (const auto at = state.find(typeOf[5]); at != state.end() && p.player)
			{
				// Only a player-faction character saves its orders (the exe's saveJobs gate); the list ends at the first absent index.
				const View& ai = at->second;
				for (int i = 0; ai.has("pjob" + std::to_string(i)); ++i)
				{
					const std::string n = std::to_string(i);
					m.pjobs.push_back({static_cast<std::uint16_t>(ai.num("pjob" + n)), ai.handle("pjobT" + n), std::nullopt});
				}
				for (int i = 0; ai.has("tjob" + std::to_string(i)); ++i)
				{
					const std::string n = std::to_string(i);
					m.tjobs.push_back({static_cast<std::uint16_t>(ai.num("tjob" + n)), ai.handle("tjobT" + n), ai.vec3("tjobP" + n)});
				}
			}
			if (const auto at = state.find(typeOf[4]); at != state.end())
			{
				for (const kdata::Instance& entry : at->second.record->instances)
				{
					const auto found = bySid.find(entry.targetStringID);
					if (found == bySid.end())
						continue;
					const View item{found->second};
					scenario::Item it;
					it.sid = item.str("base data sid");
					it.section = item.str("section");
					it.count = static_cast<std::uint32_t>(std::max(1.0, item.num("quantity", 1.0)));
					it.level = static_cast<int>(item.num("level"));
					it.quality = item.num("quality", 100.0);
					it.material = item.str("material sid");
					it.company = item.str("company sid");
					it.colour = item.str("color sid");
					need(it.sid, who + " item");
					need(it.material, who + " item material");
					need(it.company, who + " item company");
					need(it.colour, who + " item colour");
					c.items.push_back(it);
				}
			}
			if (const auto at = state.find(typeOf[3]); at != state.end())
			{
				const View& look = at->second;
				c.female = look.flag("sex female");
				const auto appearance = [&](const std::string& sid)
				{
					if (!looksLikeSid(sid) || gd.find(stripTech(sid)) != nullptr)
						return;
					if (hairMod(sid))
					{
						report.appearanceSids.insert(sid);
						++report.appearanceOccurrences;
					} else {
						need(sid, who + " appearance");
					}
				};
				for (const kdata::FieldString& field : look.record->strings)
					appearance(field.value);
				for (const kdata::RefCategory& category : look.record->references)
				{
					for (const kdata::RefEntry& entry : category.entries)
						appearance(entry.targetID);
				}
			}
			m.handle = ch.handle("handle").value_or(Handle{p.pool, static_cast<std::int32_t>(p.members.size()) + 1});
			m.slaver = ch.handle("slaver");
			m.carrying = ch.handle("carrying");
			m.slaveState = static_cast<std::uint8_t>(std::clamp(ch.num("slavestate"), 0.0, 255.0));
			if (m.slaveState > 1 && ch.has("ssct"))
				m.slaveStamp = ch.num("ssct");
			for (int n = 0; ch.has("bountyfac" + std::to_string(n)) || ch.has("amount" + std::to_string(n)); ++n)
			{
				const std::string at = std::to_string(n);
				state::Bounty bounty;
				bounty.factionSid = ch.str("bountyfac" + at);
				bounty.amount = static_cast<std::int32_t>(ch.num("amount" + at));
				bounty.crimes = static_cast<std::uint32_t>(ch.num("crimes" + at));
				bounty.claim = ch.flag("claim" + at);
				bounty.stamp = ch.num("bountyexp" + at);
				need(bounty.factionSid, who + " bounty holder");
				if (!bounty.factionSid.empty())
					m.bounties.push_back(std::move(bounty));
			}
			std::sort(m.bounties.begin(), m.bounties.end(), [](const state::Bounty& l, const state::Bounty& r) { return l.factionSid < r.factionSid; });
			if (ch.num("psts") > 0.0 && ch.num("sentence") > 0.0)
			{
				m.sentenceStart = ch.num("sentence");
				m.sentenceHours = ch.num("psts");
			}
			p.members.push_back(std::move(m));
		}
	}
	if (!missing.empty())
	{
		std::string text = "save import: " + std::to_string(missing.size()) + " referenced record(s) are not in the loaded game data (nothing was substituted):";
		for (const auto& [sid, why] : missing)
			text += "\n  " + sid + "  <- " + why.front() + (why.size() > 1 ? " (+" + std::to_string(why.size() - 1) + " more)" : "");
		throw std::runtime_error(text);
	}

	// The platoon files nothing names: listed by name, never opened.
	if (fs::is_directory(root / "platoon"))
	{
		for (const fs::directory_entry& entry : fs::directory_iterator(root / "platoon"))
		{
			if (entry.path().extension() == ".platoon" && named.count(entry.path().filename().string()) == 0)
				report.staleFiles.push_back(entry.path().filename().string());
		}
		std::sort(report.staleFiles.begin(), report.staleFiles.end());
	}

	// The handle map's source: a member's own handle (C = its platoon's pool index, I = a serial that is not its list position).
	std::map<Handle, std::pair<std::size_t, std::size_t>> parsed;
	for (std::size_t i = 0; i < platoons.size(); ++i)
	{
		for (std::size_t k = 0; k < platoons[i].members.size(); ++k)
			parsed[platoons[i].members[k].handle] = {i, k};
	}

	// Phase 2: the world. The player's squads first: their positions decide which squads are Full detail.
	std::vector<std::array<double, 2>> playerAt;
	const auto streaming = [&](const std::vector<std::array<double, 2>>& spots)
	{
		const MeshNavigator* nav = sim.navigation();
		if (nav == nullptr || !nav->streaming())
			return;
		std::vector<kworld::ZoneCell> wanted = nav->coveredZones();
		for (const auto& at : spots)
		{
			const kworld::ZoneCell cell = kworld::zoneCellOf(at[0], at[1]);
			for (int dz = -1; dz <= 1; ++dz)
			{
				for (int dx = -1; dx <= 1; ++dx)
					wanted.push_back({cell.x + dx, cell.z + dz});
			}
		}
		nav->setCoverage(wanted);
	};
	const auto fighterOf = [&](const Platoon& p, std::size_t k, bool player) -> state::Fighter
	{
		const Member& m = p.members[k];
		std::uint64_t rng = splitMix64(static_cast<std::uint64_t>(p.pool) * 1000003ull + static_cast<std::uint64_t>(m.handle.second));
		const std::uint64_t dice = splitMix64(rng);
		state::Fighter f = data.makeFighter(gd, m.c.templateSid, "", rng, p.squadTemplate);
		f.diceSeed = dice;
		f.playerControlled = player;
		f.yaw = m.c.yaw;
		return f;
	};
	const auto captorOf = [&](const Member& m) -> std::string
	{
		if (!m.slaver)
			return {};
		const auto found = parsed.find(*m.slaver);
		if (found == parsed.end())
		{
			++report.unresolvedHandles;
			return {};
		}
		++report.captors;
		return platoons[found->second.first].factionSid;
	};
	const auto noteGaps = [&](const Platoon& p, const Member& m, const std::set<std::string>& unmapped)
	{
		for (const std::string& note : unmapped)
			++report.gaps["mode: " + note];
		for (const std::string& name : scenario::unmappedStats(m.c.stats))
			++report.statsUnmapped[name];
		++report.gaps["appearance beyond sex (head, hair, beard, morphs, skin)"];
		++report.gaps["hunger and the stance / move modes (not in MemberDelta: rebuilt from the template)"];
		if (m.carrying)
			++report.gaps["carrying handle (a carry is a task: not restored)"];
		report.members += 1;
		report.dead += m.c.dead ? 1 : 0;
		(void)p;
	};

	// The clock first: what the import stamps (decay clocks, spawn timers) reads the saved time.
	report.world.day = static_cast<std::uint64_t>(camera.num("time day"));
	report.world.hour = static_cast<int>(camera.num("time hour"));
	report.world.minute = static_cast<int>(camera.num("time minute"));
	sim.setClockTick(tickAt(report.world.day, report.world.hour, report.world.minute));

	// The save's own zone files hold the placements the original spawned at run time: a Peeler Machine of a village is one (kenshi-cym.28, rwh.97 7). A
	// placement of a cage building the loaded zones do not carry (same instance id) is added as a cage site; the rest of them stay derivable (SAVE_IMPORT.md).
	if (fs::is_directory(root / "zone"))
	{
		std::set<std::string> have;
		for (const kworld::PlacedInstance& placed : sim.world().placements())
			have.insert(placed.instanceID);
		std::vector<fs::path> zoneFiles;
		for (const fs::directory_entry& entry : fs::directory_iterator(root / "zone"))
			zoneFiles.push_back(entry.path());
		std::sort(zoneFiles.begin(), zoneFiles.end());
		for (const fs::path& path : zoneFiles)
		{
			std::ifstream in(path, std::ios::binary);
			std::vector<char> text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			std::vector<std::byte> bytes(text.size());
			std::memcpy(bytes.data(), text.data(), text.size());
			const kworld::ZoneFile zone = kworld::parseZoneFile(bytes, path.filename().string(), ".");
			for (const kworld::ZoneInstance& instance : zone.instances)
			{
				const kdata::Record* building = instance.kind == kworld::ZoneInstanceKind::Placement ? gd.find(instance.targetID) : nullptr;
				if (building == nullptr || have.count(instance.instanceID) != 0)
					continue;
				kworld::PlacedInstance placed;
				placed.instanceID = instance.instanceID;
				placed.buildingID = instance.targetID;
				placed.x = instance.x;
				placed.z = instance.z;
				placed.relativeY = instance.y;
				placed.y = instance.y + std::max(kworld::kSeaPlane, sim.groundHeight(instance.x, instance.z).value_or(kworld::kSeaPlane));
				placed.qw = instance.qw;
				placed.qx = instance.qx;
				placed.qy = instance.qy;
				placed.qz = instance.qz;
				placed.ownerFaction = instance.ownerFaction;
				placed.squadTemplate = instance.squadTemplate;
				placed.interiorLayout = instance.interiorLayout;
				placed.label = instance.label;
				placed.townHand = instance.townHand;
				if (sim.placeFurnitureCage(placed))
					++report.furnitureCages;
			}
		}
	}

	if (std::any_of(platoons.begin(), platoons.end(), [](const Platoon& p) { return p.player; }))
	{
		const std::string playerSid = std::find_if(platoons.begin(), platoons.end(), [](const Platoon& p) { return p.player; })->factionSid;
		sim.createPlayerFaction(playerSid);
	}
	for (Platoon& p : platoons)
	{
		if (!p.player)
			continue;
		PlatoonResult& result = report.platoons[p.result];
		const std::uint64_t squadId = sim.addPlayerSquad(p.name);
		report.squadOfPool[p.pool] = squadId;
		std::vector<std::array<double, 2>> spots;
		for (const Member& m : p.members)
			spots.push_back({m.c.x, m.c.z});
		if (options.snapFull)
			streaming(spots);
		for (std::size_t k = 0; k < p.members.size(); ++k)
		{
			Member& m = p.members[k];
			std::set<std::string> unmapped;
			std::vector<scenario::Item> carried;
			state::Fighter f = fighterOf(p, k, true);
			scenario::applyChar(gd, data, m.c, f, carried, unmapped);
			f.slaveState = m.slaveState;
			f.slaveStamp = m.slaveStamp;
			f.captorSid = captorOf(m);
			f.x = m.c.x;
			f.y = m.c.y;
			f.z = m.c.z;
			if (options.snapFull && sim.navigation() != nullptr)
			{
				if (const auto ground = sim.placeOnGround(m.c.x, m.c.y, m.c.z))
				{
					f.x = ground->x;
					f.y = ground->y;
					f.z = ground->z;
				} else {
					++report.unplaced;
				}
			}
			const flecs::entity entity = sim.spawnFighter(std::move(f));
			const std::uint64_t id = entity.get<state::Id>()->value;
			if (!carried.empty())
			{
				state::Inventory pack = *entity.get<state::Inventory>();
				scenario::addCarried(pack, gd, carried);
				grid::settle(pack, grid::Rules(gd), id);
				entity.set<state::Inventory>(std::move(pack));
			}
			sim.addPlayerMember(squadId, id);
			report.memberOfHandle[m.handle] = id;
			++report.membersPlayer;
			report.slaves += m.slaveState != 0 ? 1 : 0;
			noteGaps(p, m, unmapped);
			playerAt.push_back({m.c.x, m.c.z});
		}
		result.outcome = "player";
		result.members = p.members.size();
		report.gaps["squad purse (money; PLATOON `money`)"] += p.money != 0 ? 1 : 0;
	}

	report.generatedRemoved = sim.importClearSquads();
	for (Platoon& p : platoons)
	{
		PlatoonResult& result = report.platoons[p.result];
		if (p.player || !result.outcome.empty())
			continue; // the player's, or skipped in phase 1
		if (!p.file.empty() && p.members.size() != p.charCount)
			++report.gaps["platoons whose file holds a different number of characters than `char count`"];
		const state::EntityRef town = p.hometown.empty() ? state::EntityRef{} : sim.importTown(p.hometown);
		if (!p.hometown.empty() && town.id == 0)
			++report.gaps["hometown names a town our world does not have (home town left empty)"];
		state::SquadInstance instance;
		instance.templateSid = p.squadTemplate;
		instance.faction = sim.importFaction(p.factionSid);
		instance.homeTown = town;
		if (town.id != 0)
		{
			instance.source = p.resident ? state::SpawnSource::TownResident : state::SpawnSource::TownRoaming;
		} else {
			instance.source = state::SpawnSource::BiomeHomeless;
			instance.hasDen = true;
			instance.denX = p.x;
			instance.denZ = p.z;
		}
		if (p.money != 0)
			++report.gaps["squad purse (money; PLATOON `money`)"];
		++report.gaps["homebuilding / currenttown / target town handles"];
		report.gaps["imprisoned platoons imported as ordinary squads (cages not restored)"] += p.imprisoned ? 1 : 0;
		if (p.file.empty())
		{
			std::size_t members = 0;
			const flecs::entity squad = sim.importTemplateSquad(p.squadTemplate, instance.faction, town, instance.source, p.x, p.z, static_cast<std::uint64_t>(p.pool), &members);
			if (!squad.is_valid())
			{
				result.outcome = "skipped";
				result.reason = "never activated and its squad template cannot be built";
				continue;
			}
			report.squadOfPool[p.pool] = squad.get<state::Id>()->value;
			result.outcome = "token (never activated)";
			result.members = members;
			report.members += members;
			report.membersToken += members;
			if (members != p.charCount)
				++report.gaps["never-activated squads whose template draw differs from `char count`"];
			continue;
		}
		const flecs::entity squad = sim.importSquad(instance, state::Position{p.x, p.z});
		report.squadOfPool[p.pool] = squad.get<state::Id>()->value;
		bool near = false;
		for (const auto& at : playerAt)
			near = near || std::hypot(at[0] - p.x, at[1] - p.z) <= options.fullRadius;
		const bool full = near && instance.source != state::SpawnSource::TownResident;
		if (near && !full)
			++report.gaps["resident squads inside the Full radius left as tokens (the resident pass builds them)"];
		std::vector<Simulation::ImportSpot> spots;
		for (std::size_t k = 0; k < p.members.size(); ++k)
		{
			Member& m = p.members[k];
			std::set<std::string> unmapped;
			const std::string captor = captorOf(m);
			const std::optional<flecs::entity> member = sim.importMember(squad, m.c.templateSid, m.c.leader, k,
				[&](state::Fighter& f, state::Inventory& pack)
				{
					std::vector<scenario::Item> carried;
					scenario::applyChar(gd, data, m.c, f, carried, unmapped);
					scenario::addCarried(pack, gd, carried);
					f.slaveState = m.slaveState;
					f.slaveStamp = m.slaveStamp;
					f.captorSid = captor;
				}, m.c.x, m.c.z);
			if (!member)
			{
				++report.membersSkipped;
				continue;
			}
			report.memberOfHandle[m.handle] = member->get<state::Id>()->value;
			report.slaves += m.slaveState != 0 ? 1 : 0;
			noteGaps(p, m, unmapped);
			++(full ? report.membersFull : report.membersToken);
			spots.push_back({*member, m.c.x, m.c.y, m.c.z});
		}
		result.members = p.members.size();
		result.outcome = full ? "full" : "token";
		if (full)
		{
			if (options.snapFull)
			{
				std::vector<std::array<double, 2>> where;
				for (const Simulation::ImportSpot& spot : spots)
					where.push_back({spot.x, spot.z});
				streaming(where);
			}
			report.unplaced += sim.importPromote(squad, spots, options.snapFull);
		}
	}
	sim.importDone();

	// Stage 3a: the world-wide state.
	WorldReport& world = report.world;
	std::map<std::string, state::EntityRef> factionOf;
	const auto factionRef = [&](const std::string& sid) -> state::EntityRef
	{
		const auto found = factionOf.find(sid);
		if (found != factionOf.end())
			return found->second;
		return factionOf[sid] = sim.importFaction(sid);
	};
	for (const FactionRow& row : factionRows)
	{
		const state::EntityRef from = factionRef(row.sid);
		if (from.id == 0)
			continue;
		++world.factions;
		if (row.hasProsperity)
		{
			sim.importProsperity(from, row.prosperity);
			++world.prosperity;
		}
		sim.importFactionStanding(from, row.rank, row.globalTrust, row.trust);
		world.trustRows += row.trust.size();
		world.ranked += row.rank != 0 ? 1 : 0;
		for (const auto& [other, value] : row.relations)
		{
			const state::EntityRef to = factionRef(other);
			if (to.id == 0 || to.id == from.id)
				continue;
			sim.importRelation(from, to, value);
			++world.relations;
			world.relationsNonZero += value != 0.0 ? 1 : 0;
		}
	}
	const std::map<std::string, state::EntityRef> townAt = sim.importTownIndex();
	const std::map<std::string, Simulation::ImportPlacement> placements = sim.importPlacements();
	for (const TownRow& row : townRows)
	{
		++world.towns;
		++world.townsInfo;
		world.townsPublic += row.info.isPublic ? 1 : 0;
		world.townsDiscovered += row.info.discovered ? 1 : 0;
		world.townsExplored += row.info.explored ? 1 : 0;
		if (!row.replacement.empty())
			world.replacements[row.instance] = row.replacement;
		const state::EntityRef owner = row.owner.empty() ? state::EntityRef{} : factionRef(row.owner);
		const auto found = townAt.find(row.instance);
		if (found == townAt.end())
		{
			// No TownState of ours: a later placement of a TOWN record, or the player's town. Kept as an ExtraTown.
			state::ExtraTown extra;
			extra.instance = row.instance;
			extra.activeSid = row.replacement;
			extra.name = row.name;
			extra.owner = owner;
			extra.info = row.info;
			if (const auto placed = placements.find(row.instance); placed != placements.end())
			{
				extra.placed = true;
				extra.rootSid = placed->second.rootSid;
				extra.x = placed->second.x;
				extra.z = placed->second.z;
			}
			++world.townsWithoutPlacement;
			++world.extraTowns;
			world.extraTownsPlaced += extra.placed ? 1 : 0;
			world.extraTownsOwned += owner.id != 0 ? 1 : 0;
			if (!row.replacement.empty())
				++world.replacementsApplied;
			sim.importExtraTown(extra);
			continue;
		}
		const state::TownState* now = sim.world().ecs().entity(found->second.id).get<state::TownState>();
		world.ownersChanged += now != nullptr && now->owner.id != owner.id ? 1 : 0;
		sim.importTownOwner(found->second, owner);
		++world.owners;
		if (sim.importTownInfo(found->second, row.info, row.replacement))
			++world.replacementsApplied;
		else if (!row.replacement.empty())
			++world.replacementsRefused;
	}
	world.money = static_cast<std::int64_t>(camera.num("player money"));
	world.research = researchSids.size();
	world.playerFaction = sim.importPlayerFaction(world.money, researchSids);
	std::map<std::uint64_t, state::CrimeRecord> crimes;
	for (const Platoon& p : platoons)
	{
		for (const Member& m : p.members)
		{
			if (m.bounties.empty() && m.sentenceHours == 0.0)
				continue;
			const auto id = report.memberOfHandle.find(m.handle);
			if (id == report.memberOfHandle.end())
			{
				world.bountiesLost += m.bounties.size();
				continue;
			}
			state::CrimeRecord& record = crimes[id->second];
			record.bounties = m.bounties;
			record.sentenceStart = m.sentenceStart;
			record.sentenceHours = m.sentenceHours;
			world.bounties += m.bounties.size();
			world.sentences += m.sentenceHours > 0.0 ? 1 : 0;
		}
	}
	world.bountyCharacters = sim.importCrimes(crimes);
	for (const Platoon& p : platoons)
	{
		for (const Member& m : p.members)
		{
			const auto id = report.memberOfHandle.find(m.handle);
			if (id == report.memberOfHandle.end() || (m.pjobs.empty() && m.tjobs.empty()))
				continue;
			std::vector<std::pair<std::uint16_t, std::uint64_t>> jobs;
			std::vector<std::array<double, 3>> moves;
			for (const Member::SavedJob& job : m.pjobs)
			{
				std::uint64_t target = 0;
				const JobType* known = jobType(job.type);
				// A handle on a type with no target (Engineer) is read and ignored, as addJob does; a targeted type's must resolve to a member (the handle map holds
				// members only: a machine or building handle drops the row, counted).
				if (job.target && known != nullptr && known->targeted)
				{
					const auto at = report.memberOfHandle.find(*job.target);
					if (at == report.memberOfHandle.end())
					{
						++report.ordersSkipped[job.type];
						continue;
					}
					target = at->second;
				}
				if (known == nullptr)
					++report.ordersSkipped[job.type];
				else
					jobs.push_back({job.type, target});
			}
			for (const Member::SavedJob& job : m.tjobs)
			{
				// Task 29 MOVE_CUS_ORDERED with its point (a null hand) is the one order the sim issues from a save.
				if (job.type == 29 && job.point && !job.target)
					moves.push_back(*job.point);
				else
					++report.ordersSkipped[job.type];
			}
			report.jobsImported += sim.importOrders(id->second, jobs, moves);
			report.movesImported += moves.size();
		}
	}
	std::vector<state::WorldEventState::Used> used;
	for (UniqueRow& row : uniqueRows)
	{
		if (row.handle)
			if (const auto at = report.memberOfHandle.find(*row.handle); at != report.memberOfHandle.end())
			{
				row.used.member = at->second;
				++world.uniquesResolved;
			}
		used.push_back(std::move(row.used));
	}
	world.uniques = used.size();
	sim.importUniques(std::move(used));
	world.weatherSlots = weatherSlots.size();
	const std::vector<Simulation::ImportWeatherResult> weather = sim.importWeather(weatherSlots);
	for (std::size_t i = 0; i < weather.size(); ++i)
	{
		if (weather[i].group.empty())
			++world.weatherLeft[weather[i].why.rfind("the season is shared", 0) == 0 ? "the season is shared by several groups" : weather[i].why];
		else
			world.weatherSet[i] = weather[i].group;
	}

	// CAMERA's selection, through the handle map.
	const auto select = [&](const std::optional<Handle>& handle) -> std::uint64_t
	{
		if (!handle)
			return 0;
		const auto found = report.memberOfHandle.find(*handle);
		if (found == report.memberOfHandle.end())
		{
			++report.unresolvedHandles;
			return 0;
		}
		return found->second;
	};
	report.selected = select(camera.handle("selected_character"));
	for (int n = 0; camera.has("selected_characters" + std::to_string(n) + "I") || camera.has("selected_characters" + std::to_string(n) + "C"); ++n)
	{
		if (const std::uint64_t id = select(camera.handle("selected_characters" + std::to_string(n))))
			report.selection.push_back(id);
	}

	for (const PlatoonResult& result : report.platoons)
		++report.byOutcome[result.outcome == "skipped" ? "skipped: " + result.reason : result.outcome];
	return report;
}

} // namespace ksim::saveimport
