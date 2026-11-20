#include <algorithm>
#include "kutil/executable.hpp"
#include "render/lightbake.hpp"
#include "render/drawstats.hpp"
#include "render/framestats.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <future>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <unistd.h>
#include <string>
#include <vector>

#include "render/interiorclip.hpp"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_debugtext.h"
#include "sokol_gl.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#if defined(SOKOL_GLCORE)
#include "platform/gl.hpp"	// the frame-buffer calls the dev depth readback needs (animlab / idbuffer do the same)
#endif

#include "world/buildings.hpp"
#include "world/foliage.hpp"
#include "world/grass.hpp"
#include "render/gputimer.hpp"
#include "app/cpustage.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "app/threadsplit.hpp"	// the logic / render split on the stages line (kenshi-k16.2.205.12.84)
#endif
#include "dev/packetdigest.hpp"	// KCLIENT_PACKET_DIGEST / KCLIENT_PACKET_POISON (kenshi-k16.2.205.12.93)
#include "knobs/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
#include "assets/sidorder.hpp"
#include "render/fogvolume.hpp"
#include "render/sunshafts.hpp"
#include "render/dust.hpp"
#include "render/blooddecals.hpp"
#include "render/arrows.hpp"
#include "render/particles.hpp"
#include "world/particleworld.hpp"
#include "frame/beamfield.hpp"
#include "characters/physics.hpp"
#include "bridge/ragdolldrive.hpp"
#include "characters/ragdoll.hpp"
#include "render/hdr.hpp"
#include "render/idbuffer.hpp"
#include "render/post.hpp"
#include "render/camera.hpp"
#include "characters/portraits.hpp"
#include "world/doorpose.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "dev/pickmask.hpp"
#endif
#include "knobs/lightterms.hpp"
#include "net/aitask.hpp"
#include "app/githash.hpp"
#include "frame/loadscreen.hpp"
#include "platform/localserver.hpp"
#include "assets/dds.hpp"
#include "assets/ogremesh.hpp"
#include "world/clientnav.hpp"
#include "kworld/towns.hpp"
#include "render/navoverlay.hpp"
#include "app/config.hpp"
#include "platform/windowgeom.hpp"
#include "platform/process.hpp"	// plat::unsetEnv
#include "platform/windowgate.hpp"
#include "render/readback.hpp"
#include "render/renderqueue.hpp"
#include "input/inputqueue.hpp"
#include "app/sappcmd.hpp"
#include "app/logicthread.hpp"
#include "frame/lastframe.hpp"
#include "frame/renderframe.hpp"
#include "app/opencont.hpp"
#include "render/weff.hpp"
#include "net/progressfeed.hpp"
#include "ui/ui.hpp"
#include "ui/gfx.hpp"
#include "ui/hud.hpp"
#include "ui/options.hpp"
#include "ui/gear.hpp"
#include "ui/itemwindows.hpp"
#include "ui/stats.hpp"
#include "ui/keys.hpp"
#include "ui/overview.hpp"
#include "ui/squads.hpp"
#include "ui/research.hpp"
#include "ui/crafting.hpp"
#include "ui/faction.hpp"
#include "ui/progressbars.hpp"
#include "ui/namelabels.hpp"
#include "ui/prompt.hpp"
#include "ui/icons.hpp"
#include "ui/creator.hpp"
#include "ui/menus.hpp"
#include "ui/savepanel.hpp"
#include "ui/newgame.hpp"
#include "ui/advanced.hpp"
#include "ui/tutorials.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "ui/dev/lightspanel.hpp"
#endif
#include "ui/layout.hpp"
#include "ui/prospecting.hpp"
#include "ui/build.hpp"
#include "ui/style.hpp"
#include "app/serverrows.hpp"
#include "ui/trade.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/bubbles.hpp"
#include "ui/dialogue.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/coords.hpp"
#include "assets/materials.hpp"
#include "assets/mods.hpp"
#include "assets/resources.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "kutil/crashlog.hpp"
#include "app/eventlog.hpp"
#include "world/squads.hpp"
#ifdef KCLIENT_DEBUG_VIEW
#include "debug/debugview.hpp"
#include "ui/dev/debugtree.hpp"
#include "ksim/debugview/enums.hpp"
#endif
#include "characters/characters.hpp"
#include "characters/creator.hpp"
#include "characters/creatorscene.hpp"
#include "kworld/gamedata.hpp"
#include "render/terrain.hpp"
#include "render/water.hpp"
#include "render/reflection.hpp"
#include "render/speccube.hpp"
#include "render/weathermap.hpp"
#include "world/weatherinstances.hpp"
#include "audio/audiohook.hpp"
#include "audio/events.hpp"
#include "bridge/simview.hpp"
#include "frame/trails.hpp"
#include "input/collisionpick.hpp"
#include "input/controlhints.hpp"	// the control-hints types and the resolver stub (kenshi-k16.2.294.2)
#include "input/pick.hpp"	// Pick and the target predicates the hover / click / menu read (kenshi-k16.2.294.2)
#include "input/cursors.hpp"
#include "render/drawlist_gpu.hpp"
#include "render/overlaylist.hpp"
#include "world/worldbuildings.hpp"
#include "world/worldmap.hpp"
#include "host/app.hpp"
#include "host/pick.hpp"
#include "host/orders.hpp"
#include "host/hud.hpp"
#include "host/script.hpp"
#include "host/logicframe.hpp"
#include "host/session.hpp"
#include "host/uihost.hpp"
#include "host/panels.hpp"

namespace host
{

// Per frame: the creator screen's view of app.creator. Idle stance and skin tone first among the Body sliders (stance 1..N over
// Characters::idleStances, skin 0..100 for spec.skin 0..1).
ui::CreatorState creator_state()
{
	ui::CreatorState st;
	if (!app.creator.active()) { return st; }
	const Characters::Spec& s = app.creator.spec();
	const Characters::RaceChoices& ch = app.creator.choices();
	st.valid = true;
	st.name = app.creator.name;
	if (!app.ngMembers.empty())
	{
		st.character = app.ngIndex;
		st.characters = (int)app.ngMembers.size();
		st.editable = app.ngMembers[app.ngIndex].editable;
	}
	for (auto [group, races] : app.chars.raceGroups())
	{
		if (!app.ngForce.empty()) { std::erase_if(races, [](const std::string& r) { return !app.ngForce.count(r); }); }	// the start's force race
		if (races.empty()) { continue; }
		const auto at = std::find(races.begin(), races.end(), s.race);
		if (at != races.end()) { st.group = (int)st.groups.size(); st.race = (int)(at - races.begin()); }
		st.groups.push_back({ group, races });
	}
	st.sex = s.female ? 1 : 0;
	st.bothSexes = !ch.ranges[0].empty() && !ch.ranges[1].empty();
	if (!st.editable)	// an animal in the squad: its RACE record's name, no subrace, no sex; its description and stats are the record's own (choices())
	{
		st.raceText = s.race;
		st.subraceText = st.genderText = "-";
	}
	st.clothes = app.creator.clothes;
	st.description = ch.description;
	st.stats.push_back({ "Hit Points:", std::to_string((int)(ch.hp + 0.01f)) });
	for (const auto& [name, m] : ch.skills)
	{
		char value[16];
		snprintf(value, sizeof value, "%+ld%%", std::lround((m - 1) * 100));
		st.stats.push_back({ name, value });
	}
	if (ch.noBleeding) { st.stats.push_back({ "No bleeding", "" }); }	// UNSURE: its place in the panel
	st.heads = ch.heads[st.sex];
	st.head = s.head;
	auto withNone = [](const std::vector<std::string>& list, const std::string& cur, int& index)
	{
		std::vector<std::string> out{ "" };
		out.insert(out.end(), list.begin(), list.end());
		const auto at = std::find(list.begin(), list.end(), cur);
		index = at == list.end() ? 0 : 1 + (int)(at - list.begin());
		return out;
	};
	st.hairs = withNone(ch.hairs, s.hair, st.hair);
	if (!s.female) { st.beards = withNone(ch.beards, s.beard, st.beard); }
	st.hairColours = ch.hairColours;
	const auto col = std::find(ch.hairColours.begin(), ch.hairColours.end(), s.hairColour);
	st.hairColour = col == ch.hairColours.end() ? 0 : (int)(col - ch.hairColours.begin());
	const auto& stances = app.chars.idleStances();
	if (!stances.empty())
	{
		const auto at = std::find(stances.begin(), stances.end(), s.stance);
		st.sliders.push_back({ "Body", "Idle stance", 1, (float)stances.size(), at == stances.end() ? 1.0f : 1.0f + (float)(at - stances.begin()), 1 });
	}
	// Skin Tone: the original's slider is 0..10*(palette swatches - 1), integers, so the human 6 swatches give 0..50 (Creator::skinMax); the value
	// is the 0..1 palette position x the top, so a body saved above it (skin 0.69) keeps its colour and only the range is capped (kenshi-k16.2.432.3).
	const float skinTop = (float)app.creator.skinMax();
	st.sliders.push_back({ "Body", "Skin Tone", 0, skinTop, s.skin * skinTop, 1 });
	for (const Characters::EditorRange& r : app.creator.ranges())
	{
		st.sliders.push_back({ r.category, r.name, std::min(r.min, r.max), std::max(r.min, r.max), app.creator.value(r.name), 0 });
	}
	return st;
}

// Per frame, before ui::frame: the HUD's view of the sim. Names from APPEARANCE (protocol 4; none: "#id"); health from CHAR_MEDICAL
// (SimView::health: the mean part health / race max), 0 when dead.
// An INVENTORY entry for the UI. Quality (ITEM_QUALITY): grade 0..5 colours the name (ui: ItemGrade0..4, 5 ItemArtifact, UNSURE). A weapon's tooltip
// ends with its model (MATERIAL_SPECS_WEAPON name, e.g. "Catun No.1") and maker lines (Sword::vf80, kinspect item21a_static); armour shows
// "{word} Grade" (Armour::vf79), both in the grade colour.
// Grade from ITEM_QUALITY `level` (docs/reference/GRADES_GATESQUADS_KO_LIVE.md section a): g = round(level x 100). The colour and the name use different bands;
// the name shows only for 0 < g < 100 and only on items without a model name (armour, crossbows, robot limbs; weapons show their model).
struct ItemName { std::string name, model = {}, maker = {}, makerText = {}, grade_word = {}; int grade = -1; };	// grade: the ItemGrade palette index, 5 = artifact; -1 none

static ItemName item_name(const simlink::Inventory::Entry& e)
{
	ItemName out{ app.sim.nameOf(e.item, app.chars) };
	if (!e.hasQuality) { return out; }
	const int g = (int)std::lround(e.level * 100.0f);
	out.grade = g < 16 ? 0 : g < 36 ? 1 : g < 51 ? 2 : g < 66 ? 3 : g <= 80 ? 4 : 5;
	if (e.model) { out.model = app.sim.nameOf(e.model, app.chars); }
	else if (g > 0 && g < 100) { out.grade_word = g < 11 ? "Prototype" : g <= 20 ? "Shoddy" : g < 41 ? "Standard" : g < 61 ? "High" : g < 81 ? "Specialist" : "Masterwork"; }
	if (e.maker)
	{
		out.maker = app.sim.nameOf(e.maker, app.chars);
		if (const Characters::ModInfo* m = app.chars.modInfo(app.sim.decoder().str(e.maker))) { out.makerText = m->description; }
	}
	return out;
}

// The status panel's rows from CHAR_STATS (kinspect ui_captures/hud_panel16.md 3-5, inspect_focus.md): State / Goal / Encumbrance / the skill
// row, the stats rows by weapon class, the extended rows, the tag row. A stat is "N (+M)" in StatUp, "N (-M)" in StatDown (TOUGHNESS never
// shows one); skills the sim lacks have no row. UNSURE: the KO point's second figure.
void stats_info(const simlink::CharStats& cs, ui::HudInfo& info, bool playerFaction)
{
	using CS = simlink::CharStats;
	const uint16_t tagFlags = CS::shownTagFlags(cs.tagFlags, playerFaction);
	static const char* const kState[] = { "Normal", "Defensive", "Sleeping", "Unconscious", "Dead", "Locked", "Unlocked" };
	std::string state = cs.state < 7 ? kState[cs.state] : "Normal";
	if (cs.state == CS::kLocked) { state += " (lvl: " + std::to_string(cs.lockLevel) + ")"; }
	const bool bad = cs.state == CS::kUnconscious || cs.state == CS::kDead || cs.state == CS::kLocked;
	// row tips (findings/hud_tooltips.md 2; our wording): State for the states the original explains, Goal fixed
	const std::string stateTip = cs.state == CS::kDead ? "Dead, and not coming back." : cs.state == CS::kDefensive ? "Fighting defensively: +20 defence." : "";
	info.state.push_back({ "State:", state, bad ? "HudBad" : cs.state == CS::kUnlocked ? "HudGood" : "", stateTip });
	const std::string goal = cs.goal ? app.sim.decoder().str(cs.goal) : "";
	info.state.push_back({ "Goal:", goal.empty() ? "Aimless" : goal, "", "What this character is currently trying to do." });
	info.state.push_back({ "Encumbrance:", std::to_string((int)std::lround(cs.encumbrance)) + "%" });
	if (cs.extraKind == 1 || cs.extraKind == 2) { info.state.push_back({ cs.extraKind == 1 ? "Stealth:" : "Athletics:", std::to_string((int)std::lround(cs.extra)) }); }
	const auto row = [&](uint8_t skill, const char* label)
	{
		for (const CS::SkillRow& k : cs.skills)
		{
			if (k.skill != skill) { continue; }
			const int n = (int)std::lround(k.value), m = skill == CS::kToughness ? 0 : (int)std::lround(k.bonus);
			const std::string mod = m > 0 ? " (+" + std::to_string(m) + ")" : m < 0 ? " (" + std::to_string(m) + ")" : "";
			info.stats.push_back({ label, std::to_string(n) + mod, m > 0 ? "StatUp" : m < 0 ? "StatDown" : "" });
		}
	};
	if (cs.weaponClass == 1) { row(CS::kAttack, "ATTACK:"); row(CS::kDefence, "DEFENSE:"); }
	else if (cs.weaponClass == 0) { row(CS::kMartialArts, "MARTIAL ARTS:"); }
	row(CS::kStrength, "STRENGTH:");
	row(CS::kDexterity, "DEXTERITY:");
	row(CS::kToughness, "TOUGHNESS:");
	if (cs.weaponClass != 2) { row(CS::kDodge, "DODGE:"); }
	const auto pct = [](float f) { return std::to_string((int)std::lround(f * 100)) + "%"; };
	const float unit = speedScale();	// any modifier, + or -, is red
	const int mod = (int)std::lround(-cs.runPenalty * unit);
	info.extended.push_back({ "ATHLETICS XP:", pct(cs.athleticsXp) });
	info.extended.push_back({ "STRENGTH XP:", pct(cs.strengthXp) });
	info.extended.push_back({ "TOUGHNESS XP:", std::to_string((int)std::lround(cs.toughnessXp)) + "x" });
	info.extended.push_back({ "KO POINT:", std::to_string((int)std::lround(cs.koPoint)) });
	info.extended.push_back({ "RUN SPEED:", std::to_string((int)std::lround(cs.runSpeed * unit)) + (mod > 0 ? " (+" + std::to_string(mod) + ")" : mod < 0 ? " (" + std::to_string(mod) + ")" : "") + " " + kSpeedUnit.get(), mod ? "StatDown" : "" });
	info.extended.push_back({ "HUNGER RATE:", pct(cs.hungerRate) });
	// The tag row (HudInfo tag / tagValue / tagTip): the original's if/else chain over the selected character, first match wins, in its order
	// (hud16_static.md 5, hud_panel16.md (5)). The deciding fields are CHAR_STATS rows 24-32 (k16.6.215); an older server stops before them
	// (cs.hasTag false) and only the bounty branch (row 14 bounties) can apply, as before. Branch by branch the wire can still be short: b1
	// army commander, b3 diplomat and b5 escaped prisoner arrive 0 until the sim models them (k16.6.211 / .212 / .214) and slaveLook stays 255
	// (k16.6.213), so those branches stay off and the slave tooltip leaves its Situation / Looks-like lines out rather than show 0 / 255.
	//   1 war leader / leader  b1 / b0    2 kidnapped  b2    3 diplomatic status  b3    4 slave state 1/2/3    5 crime in progress (N)
	//   6 escaped prisoner  b5    7 bounty (row 14 + 32)    8 kidnapped (as 2: the same bit, so the exe's last test is unreachable)
	const simlink::SimLinkDecoder& dec = app.sim.decoder();
	const auto factionName = [&](uint32_t sid) { return app.chars.nameOf(dec.str(sid)); };
	const auto amountText = [](int32_t a) { return "c." + std::to_string(a); };
	// The crime labels of the exe's FUN_140856530 switch (kenshi-inspect item25_static.md (a)); the wire sends the CrimeEnum id only. 7
	// (SLAVE_FREEING) shares 9's "Terrorism" label there.
	const auto crimeName = [](uint8_t c) -> const char*
	{
		static const char* const kNames[] = { "", "Enslaving", "Burglary", "Theft", "Murder", "Assault", "Treason", "Terrorism", "Smuggling", "Terrorism",
			"Looting", "Trespassing", "Escaping Prison", "Fencing", "Crop Theft", "Kidnapping", "Uniform Theft" };
		return c < sizeof kNames / sizeof kNames[0] ? kNames[c] : "a crime";
	};
	// The tooltip's per-faction bounty lines (FUN_1405d2da0): faction left, "c.N" right; the local faction's HudNeutral, the others HudFaint.
	// FUN_1405d1400 adds the same lines to the slave and crime tooltips, so those branches call it too.
	const auto bountyLines = [&](std::vector<ui::HudTipLine>& tip)
	{
		for (const CS::Bounty& b : cs.bounties)
		{
			if (b.amount <= 0) { continue; }
			tip.push_back({ factionName(b.faction), amountText(b.amount), "", b.faction != 0 && b.faction == cs.localFaction ? "HudNeutral" : "HudFaint" });
		}
	};
	if (tagFlags & CS::kSquadLeader)
	{
		const bool army = (tagFlags & CS::kArmyCommander) != 0;
		info.tag = army ? "War Leader" : "Leader";
		info.tagColour = "HudTag";
		info.tagTip.push_back({ army ? "This is the Commander in charge of an entire army." : "This is the leader of this group." });
	}
	else if (cs.tagFlags & CS::kCaptive)
	{
		info.tag = "Kidnapped";
		info.tagColour = "HudTag";
		info.tagTip.push_back({ "This character has been kidnapped, law enforcers will try to help." });
	}
	else if (cs.tagFlags & CS::kDiplomat)
	{
		info.tag = "Diplomatic Status";
		info.tagColour = "HudTag";
		info.tagTip.push_back({ "This is a special person with diplomatic importance. Anyone who attacks this guy will be in serious trouble with his faction." });
	}
	else if (cs.slaveState >= 1 && cs.slaveState <= 3)
	{
		static const char* const kTag[] = { "", "[SLAVE]", "[ESCAPED SLAVE]", "[EX-SLAVE]" };
		const std::string owner = cs.captor ? factionName(cs.captor) : "someone";
		info.tag = kTag[cs.slaveState];
		info.tagColour = "HudBad";	// the brackets too (live, the user's kenshi-8x6 shot 23)
		if (cs.slaveState == 1) { info.tagTip.push_back({ "A slave belonging to " + owner + ". Not considered a person with rights, a slave works for his master until death and nothing more." }); }
		else if (cs.slaveState == 2) { info.tagTip.push_back({ "A slave of " + owner + " that has been seen escaping. Needs to lay low for a day and avoid getting spotted." }); }
		else { info.tagTip.push_back({ "Was once a slave of " + owner + " but has now escaped. The heat has died down but he still needs to avoid getting found out." }); }
		// b4 slaveSeen and slaveLook: a slaveLook of 255 is "not modelled" (SIMLINK row 24 / 28), and 0 cannot tell unseen from unknown
		if (cs.slaveLook != 255)
		{
			const bool seen = (cs.tagFlags & CS::kSlaveSeen) != 0;
			info.tagTip.push_back({ "Situation:", seen ? "Seen!" : "Undiscovered", "", seen ? "HudBad" : "HudGood" });
			info.tagTip.push_back({ "Looks like a slave:", std::to_string(cs.slaveLook) + "%" });
		}
		if (cs.slaveState != 1 && cs.slaveHoursLeft >= 0) { info.tagTip.push_back({ "Time remaining:", std::to_string((int)std::lround(cs.slaveHoursLeft)) + " hour(s)" }); }
		if ((cs.tagFlags & CS::kObedient) != 0) { info.tagTip.push_back({ "Your character will have a permanent job assigned called [Obedient slave]." }); }
		bountyLines(info.tagTip);
	}
	else if (cs.crime != 0)
	{
		const char* name = crimeName(cs.crime);
		info.tag = "Committing Crime!";
		info.tagColour = "HudTag";
		info.tagValue = "(" + std::to_string((int)cs.crimeTimer) + ")";
		// witnessed vs caught: a fresh crime's timer tops out at crime::kMinTimer (3 s); the witness refresh lifts it above that
		if (cs.crimeTimer > 3.0) { info.tagTip.push_back({ std::string("This character has been caught committing ") + name + "!" }); }
		else { info.tagTip.push_back({ std::string("This character is currently committing ") + name + ", make sure nobody witnesses it." }); }
		bountyLines(info.tagTip);
	}
	else if (cs.tagFlags & CS::kEscapedPrisoner)
	{
		info.tag = "[ESCAPED PRISONER]";
		info.tagColour = "HudTag";
		info.tagTip.push_back({ "This character broke out of prison, if he gets spotted by any guards he is in trouble. Leave town to clear this status." });
		if (cs.escapeHoursLeft >= 0) { info.tagTip.push_back({ "Expires in:", std::to_string((int)std::lround(cs.escapeHoursLeft)) + " hr(s)" }); }
	}
	else
	{
		// 7 bounty: row 14's per-faction amounts with row 32's claim / crimes / hoursLeft (same order). A claimed reward (claim 1) is not
		// offered again, so it is out of the tag value, but it stays in the tooltip with its claim line.
		const CS::BountyTag none;
		const auto tagOf = [&](size_t i) -> const CS::BountyTag& { return i < cs.bountyTags.size() ? cs.bountyTags[i] : none; };
		const CS::Bounty* local = nullptr;
		const CS::Bounty* other = nullptr;
		size_t localI = 0, otherI = 0;
		for (size_t i = 0; i < cs.bounties.size(); i++)
		{
			const CS::Bounty& b = cs.bounties[i];
			if (b.amount <= 0 || tagOf(i).claim) { continue; }
			if (b.faction != 0 && b.faction == cs.localFaction) { local = &b; localI = i; }
			else if (!other || b.amount > other->amount) { other = &b; otherI = i; }
		}
		const CS::Bounty* top = local ? local : other;	// the local faction's is the one shown (HudNeutral); else the largest other (HudFaint)
		const size_t topI = local ? localI : otherI;
		if (top)
		{
			info.tag = "Bounty:";
			info.tagColour = "HudTag";
			info.tagValue = amountText(top->amount);
			info.tagValueColour = local ? "HudNeutral" : "HudFaint";
			bountyLines(info.tagTip);
			// "-Wanted For:" the crimes of every bounty listed above, named from their CrimeEnum bits (the union: the exe's list is one line)
			uint32_t bits = 0;
			for (size_t i = 0; i < cs.bounties.size(); i++) { if (cs.bounties[i].amount > 0) { bits |= tagOf(i).crimes; } }
			std::string crimes;
			for (int c = 1; c < 32; c++) { if (bits & (1u << c)) { crimes += (crimes.empty() ? "" : ", ") + std::string(crimeName((uint8_t)c)); } }
			if (!crimes.empty()) { info.tagTip.push_back({ "-Wanted For:", crimes, "HudTag" }); }
			// FUN_1405d1110's wanted paragraph, separator and expiry when the character is not caged; the local bounty's expiry is the one
			// shown, or the "not wanted here" line, or the shown bounty's own hoursLeft when the character stands outside any town.
			const bool caged = cs.state == CS::kLocked || cs.state == CS::kUnlocked;
			const auto expiry = [](float left) -> ui::HudTipLine
			{
				return left < 0 ? ui::HudTipLine{ "Notorious:", "The bounty will never expire.", "HudTag" }
					: ui::HudTipLine{ "The bounty will expire in", std::to_string((int)std::lround(left)) + " hr(s)", "HudTag" };
			};
			if (!caged)
			{
				info.tagTip.push_back({ "This character is wanted by the authorities. Turn them in for a reward." });
				info.tagTip.push_back({ "-------------------------" });
				if (local) { info.tagTip.push_back(expiry(tagOf(localI).hoursLeft)); }
				else if (cs.localFaction != 0) { info.tagTip.push_back({ "This character is not wanted by the local faction.", "", "HudTag" }); }
				else { info.tagTip.push_back(expiry(tagOf(topI).hoursLeft)); }
			}
			for (size_t i = 0; i < cs.bounties.size(); i++)	// the turn-in state, one line per collected reward
			{
				if (cs.bounties[i].amount <= 0 || !tagOf(i).claim) { continue; }
				info.tagTip.push_back({ "Claimed:", factionName(cs.bounties[i].faction) + " " + amountText(cs.bounties[i].amount) });
			}
			// The imprisonment row (kenshi-rwh.133) is the tooltip's last, and only while the character is in a CAGE (state 5 / 6, never a bed):
			// the wire's sentenceHours is the time REMAINING, so the 2000 h FOREVER test is on it (an imported length can differ; item rwh133).
			if (caged)
			{
				if (cs.sentenceHours >= 2000) { info.tagTip.push_back({ "Imprisoned, sentence time:", "FOREVER", "", "HudTag" }); }
				else if ((int)std::ceil(cs.sentenceHours) > 0)
				{
					const int hours = (int)std::ceil(cs.sentenceHours);
					info.tagTip.push_back({ "Imprisoned, sentence time remaining:", std::to_string(hours) + (hours == 1 ? " hr" : " hrs"), "", "HudTag" });
				}
				else { info.tagTip.push_back({ "Imprisoned", "Waiting to be released" }); }
			}
		}
	}
}

// The character stats window's state (kenshi-k16.2.96) from CHAR_STATS, in the original's grouping and order (kui_demo demoStats): the four
// attributes beside the skills in four columns of "-Group-" rows (the wire's ids 19-32 fill the rows the guide used to list as missing, k16.2.98;
// PERCEPTION is an attribute row, as the original shows it). A row needs a wire value: the window paints a whole level on every row, so a skill
// the wire leaves out is left out rather than shown as 0. The Next Level bar is row 21's progress (UNSURE: ksim's note that its formula may not
// match the exe's, deferred to playtesting); the per-stat affects/training/description texts are not on the wire. The derived column is ours (the
// original clips it off): wire values only, in the HUD's own wording.
void stats_window(uint64_t id, ui::StatsState& s)
{
	using CS = simlink::CharStats;
	s = {};
	const simlink::SimLinkDecoder& d = app.sim.decoder();
	const auto ch = id ? d.chars.find(id) : d.chars.end();
	const CS* cs = id ? app.sim.charStats(id) : nullptr;
	if (ch == d.chars.end() || !cs) { return; }	// nothing to show yet: the window's "No stats received yet"
	s.valid = true;
	s.name = d.str(ch->second.look.name);
	// a skill row as the status panel shows it: N, and "N (+M)" in StatUp / "N (-M)" in StatDown when the wire's bonus is not 0 (TOUGHNESS never shows one)
	const auto row = [cs](const char* name, uint8_t skill, ui::StatRow& out)
	{
		for (const CS::SkillRow& k : cs->skills)
		{
			if (k.skill != skill) { continue; }
			out.name = name;
			out.level = k.value;
			if (skill != CS::kToughness && (int)std::lround(k.bonus) != 0) { out.base = k.value - k.bonus; }
			for (const CS::SkillProgress& p : cs->progress) { if (p.skill == skill) { out.xp = p.progress; break; } }	// row 21: the Next Level bar
			return true;
		}
		return false;
	};
	static const struct { uint8_t skill; const char* name; } kAttributes[] = {
		{ CS::kStrength, "STRENGTH" }, { CS::kToughness, "TOUGHNESS" }, { CS::kDexterity, "DEXTERITY" }, { CS::kPerception, "PERCEPTION" } };
	for (const auto& a : kAttributes)
	{
		ui::StatRow r;
		if (row(a.name, a.skill, r)) { s.attributes.push_back(std::move(r)); }
	}
	// The original's order within each group (kui_demo demoStats); ids 19-32 are the wire's 2026-11-11 table (SIMLINK "CHAR_STATS" row 8)
	static const struct { uint8_t skill; const char* name, *group; int column; } kSkills[] = {
		{ 7, "Katanas", "Weapons", 0 }, { 8, "Sabres", "Weapons", 0 }, { 9, "Hackers", "Weapons", 0 },
		{ 11, "Heavy weapons", "Weapons", 0 }, { 10, "Blunt", "Weapons", 0 }, { 13, "Polearms", "Weapons", 0 },
		{ CS::kAttack, "Melee attack", "Combat", 1 }, { CS::kDefence, "Melee defence", "Combat", 1 },
		{ CS::kDodge, "Dodge", "Combat", 1 }, { CS::kMartialArts, "Martial arts", "Combat", 1 },
		{ 21, "Turrets", "Ranged", 1 }, { 20, "Crossbows", "Ranged", 1 }, { 32, "Precision shooting", "Ranged", 1 },
		{ CS::kStealth, "Stealth", "Thievery", 2 }, { 15, "Lockpicking", "Thievery", 2 },	// 15: lockpicking
		{ 22, "Thievery", "Thievery", 2 }, { 23, "Assassination", "Thievery", 2 },
		{ CS::kAthletics, "Athletics", "Athletics", 2 }, { 25, "Swimming", "Athletics", 2 },
		{ 14, "Field medic", "Sciences", 3 }, { 19, "Engineer", "Sciences", 3 }, { 26, "Robotics", "Sciences", 3 }, { 17, "Science", "Sciences", 3 },
		{ 27, "Weapon smith", "Trades", 3 }, { 28, "Armour smith", "Trades", 3 }, { 29, "Crossbow smith", "Trades", 3 },
		{ 18, "Labouring", "Trades", 3 }, { 30, "Farming", "Trades", 3 }, { 31, "Cooking", "Trades", 3 } };
	ui::StatGroup* group = nullptr;
	for (const auto& k : kSkills)
	{
		ui::StatRow r;
		if (!row(k.name, k.skill, r)) { continue; }
		if (!group || group->title != k.group)
		{
			s.skills.push_back({ k.group, k.column, {} });
			group = &s.skills.back();
		}
		group->rows.push_back(std::move(r));
	}
	std::string derived;
	const auto add = [&derived](const std::string& label, const std::string& value) { derived += label + "\t" + value + "\n"; };
	if (const auto m = d.medical.find(id); m != d.medical.end())
	{
		const simlink::Medical& med = m->second;
		const auto whole = [](float v) { return std::to_string((int)std::floor(v + 0.1f)); };	// the HUD's figure (ui/hud.cpp)
		add("Blood", med.bloodMax >= 0 ? whole(med.blood) + " / " + whole(med.bloodMax) : whole(med.blood));
		if (med.hunger >= 0) { add("Hunger", std::to_string((int)(med.hunger * 100)) + " / 300"); }	// the HUD's scale (0..3 x 100)
	}
	if (cs->hasCarry) { add("Carry weight", std::to_string((int)std::lround(cs->carryKg)) + " / " + std::to_string((int)std::lround(cs->carryMaxKg)) + " kg"); }	// row 22: kg, without the 30 kg carry-person weight
	else { add("Carry weight", std::to_string((int)std::lround(cs->encumbrance)) + "%"); }	// an older server: only the percentage
	if (cs->hasResist) { add("Damage resistance", std::to_string((int)std::lround(cs->damageResist * 100)) + "%"); }	// row 23: the fraction as a percent
	const float unit = speedScale();
	const int mod = (int)std::lround(-cs->runPenalty * unit);
	add("Run speed", std::to_string((int)std::lround(cs->runSpeed * unit)) + (mod > 0 ? " (+" + std::to_string(mod) + ")" : mod < 0 ? " (" + std::to_string(mod) + ")" : "")
		+ " " + kSpeedUnit.get());
	add("KO point", std::to_string((int)std::lround(cs->koPoint)));
	add("Hunger rate", std::to_string((int)std::lround(cs->hungerRate * 100)) + "%");
	s.derived = derived;
}

// Auto-split (kenshi-k16.20.13.3): a held stack dropped on a cell that holds fewer than it carries puts one unit at the cell and autoplaces the rest,
// one unit at a time through the order queue. Off while ui.place_one (the original's one-unit-per-click) is on: that is the UI's.
const cfg::Bool kAutoSplit{ cfg::Group{ "Inventory windows" }, "ui.auto_split", true, "Item windows: a held stack bigger than the cell it is dropped on allows places one unit there and autoplaces the rest (off: the drop is refused)" };

// The grid inventory (ui::Gear) over SIMLINK 9. A GridItem's id is its INVENTORY entry's index + 1, so an action finds the entry (and the version it
// was built from: the current one; the server answers `stale` when it moved since) without a table.
namespace
{
	// Equipment grids by the entry's INVENTORY slot (1 wielded, 2 + AttachSlot worn): size and accepted kind (Characters::ItemInfo::kind), after
	// INVENTORY_GRID_STATIC (a). UNSURE: the wielded weapon goes to WEAPON I (the 10 x 2 `back` section), the 7 x 1 `hip` (WEAPON II) stays empty;
	// the server has one wielded weapon. AttachSlot 1 (back) has no grid and is not shown.
	struct GearSlot { int grid, cols, rows, kind; };
	const GearSlot* gear_slot(int slot)
	{
		static const GearSlot weapon1 = { ui::kGearWeapon1, 10, 2, 1 }, belt = { ui::kGearBelt, 2, 2, 14 }, shirt = { ui::kGearShirt, 4, 2, 8 }, head = { ui::kGearHead, 4, 3, 3 },
			pants = { ui::kGearPants, 4, 5, 6 }, armour = { ui::kGearArmour, 4, 6, 5 }, boots = { ui::kGearBoots, 4, 2, 9 }, pack = { ui::kGearBackpack, 5, 4, 12 };
		switch (slot)
		{
			case 1: return &weapon1;
			case 2 + 3: return &head;
			case 2 + 5: return &armour;
			case 2 + 6: return &pants;
			case 2 + 8: return &shirt;
			case 2 + 9: return &boots;
			case 2 + 12: return &pack;
			case 2 + 14: return &belt;
		}
		return nullptr;
	}

	// The icon: the game's cache <item>.<material>[.<colour>].png from the entry's material, else the record's own `icon` file; none: the grid writes the name.
	std::string gear_icon(const simlink::Inventory::Entry& e, const std::string& sid, const Characters::ItemInfo* info)
	{
		const simlink::SimLinkDecoder& d = app.sim.decoder();
		if (e.material)
		{
			const std::string cached = ui::icons::cached(sid, d.str(e.material), e.colour ? d.str(e.colour) : std::string());
			if (std::filesystem::exists(ui::res::dataFile(cached)) && ui::icons::get(cached)) { return cached; }	// the game caches only items it has shown: a miss is normal
		}
		return info && !info->icon.empty() ? ui::icons::recordPath(info->icon) : std::string();
	}

	// A trade price per unit (TRADE 43): what the trader asks (value) and pays (sell; -1: it won't buy). valid false outside a trade.
	struct TradePrice { bool valid = false; int32_t value = -1, sell = -1; uint32_t buybackCount = 0; int32_t buybackPrice = 0; };

	// e: entry index of the INVENTORY of id at version (the ITEM_STATS key)
	ui::GridItem gear_item(const simlink::Inventory::Entry& e, size_t index, uint64_t id, uint32_t version, TradePrice trade = {})
	{
		const std::string& sid = app.sim.decoder().str(e.item);
		const Characters::ItemInfo* info = app.chars.itemInfo(sid);
		// ITEM_STATS (order 69), asked once per key for weapons and armour: until it answers the rows below come from the record, then the sim's
		// numbers replace them (a crossbow answers kind 0: its record lines stay)
		if (info && (info->kind == 1 || info->armourClass >= 0)) { app.sim.askItemStats(id, version, (uint16_t)index); }
		const simlink::SimLinkDecoder::ItemStats* stats = info ? app.sim.itemStats(id, version, (uint16_t)index) : nullptr;
		const simlink::SimLinkDecoder::ItemStats* weapon = stats && stats->kind == 2 ? stats : nullptr, * armour = stats && stats->kind == 1 ? stats : nullptr;
		const ItemName q = item_name(e);
		ui::GridItem it;
		it.id = (uint32_t)index + 1;
		it.x = e.x;
		it.y = e.y;
		it.name = q.name;
		it.count = e.count;
		it.icon = gear_icon(e, sid, info);
		if (info) { it.w = info->w; it.h = info->h; it.kind = info->kind; it.soundSwitch = info->soundSwitch; }
		// The generic tooltip (kinspect inv_click10_static.md 5, FUN_1407ae130 order): title (n), tags, ingredients and nutrition (food), the weapon
		// lines (items14_static.md c), weight, value (the record price without the town factor: Avg. Price), description; backpacks add their CONTAINER
		// stats (0x1407aa090's labels; the value formats are MINE); armour its class / material, resistances, coverage (kinspect tooltips.md). Not done:
		// weapon and armour prices, blueprint, map, charges and quality.
		it.tip.title = q.name;
		it.tip.count = e.count;
		const std::string gradeInk = q.grade >= 5 ? "ItemArtifact" : "ItemGrade" + std::to_string(std::max(q.grade, 0));
		if (!q.grade_word.empty()) { it.tip.tags.push_back({ "[" + q.grade_word + " Grade]", gradeInk }); }
		if (e.steal != 255) { it.tip.tags.push_back({ "Steal chance " + std::to_string((unsigned)e.steal) + "%", e.steal >= 50 ? "TipGood" : "TipBad" }); }	// ITEM_GRID steal tail: a foreign container's item for the take session's actor
		if (trade.valid && trade.buybackCount > 0) { it.tip.tags.push_back({ trade.buybackCount > 1 ? "Buyback (" + std::to_string(trade.buybackCount) + ")" : std::string("Buyback"), "TipBad" }); }
		if (info && info->armourClass >= 0 && !info->coverage.empty())
		{
			// tooltips.md (armour; cloth / light): the class left, the material right, both TipMaker; the words follow fcs_enums.def ArmourClass /
			// MaterialType. Cloth (class 0) has no class tag, only the material; armour without part coverage has no line at all
			static const char* const kClass[] = { "", "[Light Armour Class]", "[Medium Armour Class]", "[Heavy Armour Class]" }, * const kMaterial[] = { "Cloth", "Leather", "Chain", "Metal Plate" };
			it.tip.tags.push_back({ kClass[info->armourClass], "TipMaker", std::string("[") + kMaterial[info->material] + "]" });
		}
		if (info)
		{
			const char* tag = info->function == 3 ? "[Food]" : info->function == 0xd ? "[Book]" : info->function == 0xf ? "[Possibly Edible]" : info->function == 0x10 ? "Ammunition" : nullptr;
			if (tag) { it.tip.tags.push_back({ tag, info->function == 3 ? "TipGood" : "" }); }
			if ((info->function == 3 || info->function == 0xf) && !info->ingredients.empty())
			{
				std::string list;
				for (const std::string& n : info->ingredients) { list += (list.empty() ? "" : ", ") + n; }
				it.tip.lines.push_back({ "-Ingredients", list });
			}
			char buf[64];
			if (info->function == 3 || info->function == 0xf)
			{
				snprintf(buf, sizeof buf, "%d nu", (int)info->charges);
				it.tip.lines.push_back({ "-Nutrition", buf });
			}
			if (info->kind == 1 && e.hasQuality)
			{
				// Sword::vf79 (kinspect items14_static.md c): t = level; the maker scales the damage, the model adds attack / defence
				const Characters::ModInfo* maker = e.maker ? app.chars.modInfo(app.sim.decoder().str(e.maker)) : nullptr;
				const Characters::ModInfo* model = e.model ? app.chars.modInfo(app.sim.decoder().str(e.model)) : nullptr;
				// ITEM_STATS: the sim's damage, bleed, penetration and attack / defence replace the derivation once it answers
				// (the rows are the multipliers cutMult / bluntMult, not the absolute cutDamage / bluntDamage)
				const float t = e.level, cut = weapon ? weapon->cutMult : (0.3f + 1.7f * t) * info->cut * (maker ? maker->cut : 1),
					blunt = weapon ? weapon->bluntMult : 2 * t * info->blunt * (maker ? maker->blunt : 1);
				auto line = [&](const char* label, const char* fmt, auto v, const char* colour = "")
				{
					snprintf(buf, sizeof buf, fmt, v);
					it.tip.lines.push_back({ label, buf, false, colour });
				};
				// Sword::vf79: the class from `skill category` (kinspect prices15_static.md); 5 unarmed / 7 turret have none. UNSURE: its place in the list
				static const char* const kClass[] = { "Katana class", "Sabre class", "Blunt weapon class", "Heavy weapons class", "Hacker class", nullptr, "Crossbow class", nullptr, "Polearm class" };
				if (info->skillCategory >= 0 && info->skillCategory < 9 && kClass[info->skillCategory]) { it.tip.lines.push_back({ kClass[info->skillCategory], "" }); }
				if (cut > 0) { line("-Cutting damage", "%.2f", cut); }	// MINE: a zero damage line is hidden
				if (blunt > 0) { line("-Blunt damage", "%.2f", blunt); }
				line("-Blood loss", "%.2fx", weapon ? weapon->bleed : info->bleed);
				if (const int p = (int)((weapon ? weapon->armourPenetration : info->penetration) * 100)) { line("-Armour penetration", "%+d%%", p, p > 0 ? "TipGood" : "TipBad"); }
				if (const int a = weapon ? (int)std::lround(weapon->attackMod) : info->attack + (model ? model->attack : 0)) { line("-Attack bonus", "%+d", a, a > 0 ? "TipGood" : "TipBad"); }
				if (const int d = weapon ? (int)std::lround(weapon->defenceMod) : info->defence + (model ? model->defence : 0)) { line("-Defence bonus", "%+d", d, d > 0 ? "TipGood" : "TipBad"); }
				if (info->indoors) { line(info->indoors > 0 ? "-Indoors bonus" : "-Indoors penalty", "%+d", info->indoors, info->indoors > 0 ? "TipGood" : "TipBad"); }
				const std::pair<const char*, float> vs[] = { { "-Damage vs robots", info->vsRobot }, { "-Damage vs humans", info->vsHuman }, { "-Damage vs animals", info->vsAnimal } };
				for (const auto& [label, m] : vs)
				{
					if (const int p = (int)std::lround((m - 1) * 100)) { line(label, "%+d%%", p, p > 0 ? "TipGood" : "TipBad"); }
				}
				it.tip.lines.push_back({ "----------------", "" });	// tooltips.md: the weapon's dashed rule before the weight (armour has none)
			}
			if (armour)
			{
				// tooltips.md (armour): the resistances before the weight, values in #afa68b; Harpoon in points (the sim's 0..90), none at 0 (Monk Robe).
				// UNSURE (SIMLINK ITEM_STATS): cutEfficiency is the row "Cut resistance efficiency", harpoon the pierce resist
				const std::pair<const char*, float> res[] = { { "-Blunt resistance", armour->blunt }, { "-Cut resistance", armour->cut }, { "-Cut resistance efficiency", armour->cutEfficiency } };
				for (const auto& [label, v] : res)
				{
					snprintf(buf, sizeof buf, "%ld%%", std::lround(v * 100));
					it.tip.lines.push_back({ label, buf, false, "TipMaker" });
				}
				if (const long h = std::lround(armour->harpoon))
				{
					snprintf(buf, sizeof buf, "%ld pts", h);
					it.tip.lines.push_back({ "-Harpoon resistance", buf, false, "TipMaker" });
				}
			}
			snprintf(buf, sizeof buf, e.count > 1 ? "%g kg (x %u)" : "%g kg", info->weight, e.count);
			it.tip.lines.push_back({ "-Weight", buf });
			if (info->kind == 15)
			{
				// a multiplier as a change in percent, green when it helps
				auto mult = [&](const char* label, float m)
				{
					if (std::fabs(m - 1) < 1e-3f) { return; }
					snprintf(buf, sizeof buf, "%+d%%", (int)std::lround((m - 1) * 100));
					it.tip.lines.push_back({ label, buf, false, m > 1 ? "TipGood" : "TipBad" });
				};
				if (std::fabs(info->encumbrance - 1) >= 1e-3f)
				{
					snprintf(buf, sizeof buf, "%d%%", (int)std::lround((1 - info->encumbrance) * 100));	// `encumbrance effect` 0.5: the contents weigh half
					it.tip.lines.push_back({ "-Encumbrance reduction", buf, false, info->encumbrance < 1 ? "TipGood" : "TipBad" });
				}
				if (info->combatSkill != 0)
				{
					snprintf(buf, sizeof buf, "%+d", info->combatSkill);
					it.tip.lines.push_back({ "-Combat skills bonus", buf, false, info->combatSkill > 0 ? "TipGood" : "TipBad" });
				}
				mult("-Athletics", info->athletics);
				mult("-Combat speed", info->combatSpeed);
				mult("-Stealth", info->stealth);
			}
			// Not done: weapon and armour prices (their formula is unknown to us yet), so they show no value at all.
			// Sell value: a quarter of the value, floored (UNSURE: both tooltips.md captures, c.37,979 / c.9,494 and c.2,085 / c.521)
			// While trading (trade_window.md 2): the trader's per-unit prices, both labels muted, then Avg. Price (the record price) and the
			// mark-up (Value over it; TipBad above 100 %)
			if (trade.valid)
			{
				if (trade.buybackCount > 0) { it.tip.lines.push_back({ "-Value", ui::trade::cats(trade.buybackPrice), true }); }	// the first buyback units cost this, the rest `value`
				else if (trade.value >= 0) { it.tip.lines.push_back({ "-Value", ui::trade::cats(trade.value), true }); }
				if (trade.sell >= 0) { it.tip.lines.push_back({ "-Sell value", ui::trade::cats(trade.sell), true }); }
				if (info->plain && info->price > 0 && trade.value >= 0)
				{
					it.tip.lines.push_back({ "-Avg. Price", ui::trade::cats(info->price) });
					const int pct = (int)std::lround(100.0 * trade.value / info->price);
					it.tip.lines.push_back({ "-Price mark-up", std::to_string(pct) + "%", false, pct > 100 ? "TipBad" : pct < 100 ? "TipGood" : "" });
				}
			}
			else if (info->plain && info->price > 0)
			{
				it.tip.lines.push_back({ "-Value", ui::trade::cats(info->price) });
				it.tip.lines.push_back({ "-Sell value", ui::trade::cats(info->price / 4) });
			}
			// Armour, after the value (tooltips.md): [Coverage] and a row per covered part (none: [No Armour Coverage], muted), then [Skills], then a
			// blank line before the description when there is one. Not done: the resistance rows (the numbers are the sim's: they wait on b0's
			// ITEM_STATS reply)
			if (info->armourClass >= 0)
			{
				if (info->coverage.empty()) { it.tip.lines.push_back({ "[No Armour Coverage]", "", true }); }
				else { it.tip.lines.push_back({ "[Coverage]", "" }); }
				for (const auto& [part, pct] : info->coverage) { it.tip.lines.push_back({ "-" + part, std::to_string(pct) + "%" }); }
				// [Skills], in kinspect's fixed order (tooltips.md "The [Skills] block"); the header only when a row follows. Mult rows "1.10x", hidden
				// at exactly 1 (Athletics: unless > 1 or < 0.97); Fist protection (1 - mult) * 100 "%", only below 1; int rows "+6" / "-3", hidden at 0.
				// Stealth, Athletics, Combat speed, Dexterity and Melee attack/defence only from ITEM_STATS (the loader's values; UNSURE: the sim sends
				// stealth / athletics / dexterity raw for now); Dodge, Attack damage and Martial arts from it once it answers; the rest from the record
				bool header = false;
				const auto skill = [&](const char* label, std::string value, bool good)
				{
					if (!header) { it.tip.lines.push_back({ "[Skills]", "" }); header = true; }
					it.tip.lines.push_back({ label, std::move(value), false, good ? "TipGood" : "TipBad" });
				};
				const auto mult = [&](const char* label, float m, bool shown)
				{
					if (!shown) { return; }
					char buf[32];
					snprintf(buf, sizeof buf, "%.2fx", m);
					skill(label, buf, m > 1.0f);
				};
				const auto bonus = [&](const char* label, int v)
				{
					if (v) { skill(label, (v > 0 ? "+" : "") + std::to_string(v), v > 0); }
				};
				if (armour) { mult("-Stealth effect", armour->stealthMult, armour->stealthMult != 1.0f); }
				mult("-Crossbows effect", info->rangedMult, info->rangedMult != 1.0f);
				const float dodge = armour ? armour->dodgeMult : info->dodgeMult, damage = armour ? armour->damageOutputMult : info->damageMult;
				mult("-Dodge skill effect", dodge, dodge != 1.0f);
				if (armour)
				{
					mult("-Athletics effect", armour->athleticsMult, armour->athleticsMult > 1.0f || armour->athleticsMult < 0.97f);
					mult("-Combat speed effect", armour->loadedCombatSpeedMult, armour->loadedCombatSpeedMult != 1.0f);
					mult("-Dexterity effect", armour->dexterityMult, armour->dexterityMult != 1.0f);
				}
				mult("-Attack damage effect", damage, damage != 1.0f);
				mult("-Assassination skill", info->assassinMult, info->assassinMult != 1.0f);
				if (info->fistMult < 1.0f)
				{
					char buf[32];
					snprintf(buf, sizeof buf, "%g%%", (1.0f - info->fistMult) * 100.0f);
					skill("-Fist protection", buf, true);
				}
				if (armour)
				{
					bonus(armour->meleeAttack == armour->meleeDefence ? "-Melee attack/defence bonus" : "-Melee attack bonus", armour->meleeAttack);
					if (armour->meleeDefence != armour->meleeAttack) { bonus("-Melee defence bonus", armour->meleeDefence); }
				}
				bonus("-Martial arts bonus", armour ? (int)std::lround(armour->unarmedBonus) : info->unarmedBonus);
				bonus("-Perception bonus", info->perceptionBonus);
				if (!info->description.empty()) { it.tip.lines.push_back({ "", "" }); }
			}
			// Sword::vf80, as kenshi-inspect's tooltips.md capture shows it: after the description and a blank line, "[Model # <model>]" in the
			// grade colour, "[Manufactured by <maker>]" in #afa68b, the maker's company description. Not done: "Crafted by {1}" (no crafter in SIMLINK).
			it.tip.description = info->description;
			if (!q.model.empty()) { it.tip.footer.push_back({ "[Model # " + q.model + "]", gradeInk }); }
			if (!q.maker.empty()) { it.tip.footer.push_back({ "[Manufactured by " + q.maker + "]", "TipMaker" }); }
			if (!q.makerText.empty()) { it.tip.footer.push_back({ q.makerText, "" }); }
		}
		return it;
	}

	// A grid number is a kGear* one (kGearContainer in a box's window); the window's owner is the GridRef's. "Far": an open box, body or trader's
	// stock (app.conts); the rest are the player's own character windows.
	bool gear_far(uint64_t owner) { return cont_find(owner) != nullptr; }

	// The ITEM_GRID section a grid shows (255 equipment).
	uint8_t gear_section(int grid) { return grid == ui::kGearBag ? 1 : grid == ui::kGearMain || grid == ui::kGearContainer ? 0 : 255; }

	bool gear_equipment(int grid) { return grid != ui::kGearMain && grid != ui::kGearBag && grid != ui::kGearContainer; }

	// The per-inventory order queue (kenshi-k16.20.14.2). Orders 63-68 carry the INVENTORY version the client last saw, so a second order sent before
	// the answering INVENTORY is refused `stale`; the queue keeps one in flight per inventory and re-resolves the rest at send time. A lost answer must
	// not wedge it (a reply comes within a tick; 2 s is noise), and a load / new world clears it.
	constexpr double kGearOrderTimeout = 2.0;

	// Steady seconds, for the queue's watchdog.
	double gear_now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

	// The entry a queued order names, by its record + section + cell: -1 when it is gone or something else is there now.
	int gear_order_index(const GearOrder& o, const simlink::Inventory& inv)
	{
		for (size_t i = 0; i < inv.items.size(); i++)
		{
			const simlink::Inventory::Entry& e = inv.items[i];
			if (o.byRecord)
			{
				if (e.item == o.record && e.section != 255 && (!o.hasQuality || (e.hasQuality && e.grade == o.grade && e.level == o.level))) { return (int)i; }
				continue;
			}
			if (e.item == o.record && e.section == o.section && e.x == o.x && e.y == o.y) { return (int)i; }
		}
		return -1;
	}

	// An entry with an order in flight or queued for it: GridItem.pending (dimmed, no press, nothing drops onto it). The entry is still the clicked one
	// until the answering INVENTORY moves it or the order is dropped.
	bool gear_pending(uint64_t inventory, const simlink::Inventory::Entry& e)
	{
		const auto it = app.gearQueues.find(inventory);
		if (it == app.gearQueues.end()) { return false; }
		const auto match = [&e](const GearOrder& o) { return o.record == e.item && o.section == e.section && o.x == e.x && o.y == e.y; };
		if (it->second.flight && match(it->second.current)) { return true; }
		for (const GearOrder& o : it->second.wait) { if (match(o)) { return true; } }
		return false;
	}

	// The first free top-left cell, scanning row by row, where a w x h item fits in `section` of the inventory; the entry `skip` (the one being moved) does
	// not block. False: no room.
	bool gear_free_cell(const simlink::Inventory& inv, uint8_t section, int w, int h, int skip, uint8_t& ox, uint8_t& oy)
	{
		int cols = 0, rows = 0;
		for (const simlink::Inventory::Grid& g : inv.grids) { if (g.section == section) { cols = g.w; rows = g.h; } }
		std::vector<bool> used((size_t)std::max(0, cols * rows), false);
		for (size_t i = 0; i < inv.items.size(); i++)
		{
			const simlink::Inventory::Entry& e = inv.items[i];
			if ((int)i == skip || e.section != section) { continue; }
			const Characters::ItemInfo* info = app.chars.itemInfo(app.sim.decoder().str(e.item));
			const int ew = info ? info->w : 1, eh = info ? info->h : 1;
			for (int y = e.y; y < e.y + eh && y < rows; y++) { for (int x = e.x; x < e.x + ew && x < cols; x++) { used[(size_t)(y * cols + x)] = true; } }
		}
		for (int y = 0; y + h <= rows; y++)
		{
			for (int x = 0; x + w <= cols; x++)
			{
				bool free = true;
				for (int yy = y; free && yy < y + h; yy++) { for (int xx = x; xx < x + w; xx++) { if (used[(size_t)(yy * cols + xx)]) { free = false; break; } } }
				if (free) { ox = (uint8_t)x; oy = (uint8_t)y; return true; }
			}
		}
		return false;
	}

	bool g_noRoomShown = false;	// gear_order_send said why it could not send (no free cell): the pump adds no "moved" message

	// Sends the order now: its inventory's current version and the entry's current index. False: it cannot go (the entry moved, the inventory is
	// gone); the caller drops it.
	bool gear_order_send(GearQueue& q, const GearOrder& o)
	{
		const simlink::Inventory* inv = app.sim.inventory(o.inv);
		if (o.kind == 63)	// by record, no version: the machine put
		{
			if (!inv || !app.sim.transfer(o.owner, o.container, o.record, o.count, o.take)) { return false; }
		}
		else if (o.kind == 65)	// arrange: no entry
		{
			if (!inv || !app.sim.arrange(o.owner, o.section)) { return false; }
		}
		else
		{
			const int found = inv ? gear_order_index(o, *inv) : -1;
			if (found < 0) { return false; }
			const uint16_t index = (uint16_t)found;
			GearOrder place = o;
			if (o.kind == 64 && o.autoCell)	// the auto-split's rest: the first free cell that holds the item
			{
				const Characters::ItemInfo* info = app.chars.itemInfo(app.sim.decoder().str(inv->items[index].item));
				if (!gear_free_cell(*inv, o.toSection, info ? info->w : 1, info ? info->h : 1, found, place.toX, place.toY))
				{
					ui::message("No room for that item.");
					g_noRoomShown = true;
					return false;
				}
			}
			const bool sent = o.kind == 64 ? app.sim.moveItem(o.owner, inv->version, index, place.toSection, place.toX, place.toY, o.count)
				: o.kind == 66 ? app.sim.dropEntry(o.owner, inv->version, index, o.count)
				: o.kind == 67 ? app.sim.equipEntry(o.owner, inv->version, index, o.on)
				: app.sim.transferEntry(o.owner, o.container, inv->version, index, o.count, o.take, o.toSection, o.toX, o.toY);
			if (!sent) { return false; }
		}
		q.flight = true;
		q.current = o;
		q.seq = app.sim.lastSentSeq();
		q.version = inv ? inv->version : 0;
		q.sentAt = gear_now();
		printf("gear order %u: %s x%u at %u:%u,%u in %llu (seq %u, version %u)\n", o.kind, app.sim.nameOf(o.record, app.chars).c_str(), o.count,
			o.section, o.x, o.y, (unsigned long long)o.inv, q.seq, q.version);
		return true;
	}

	// Drops every queued order of a moveAll group (its first refusal stops the rest, kenshi-k16.20.12).
	void gear_order_drop_group(uint64_t group)
	{
		if (!group) { return; }
		for (auto& [key, q] : app.gearQueues) { std::erase_if(q.wait, [group](const GearOrder& o) { return o.group == group; }); }
	}

	void gear_orders_pump()
	{
		for (auto& [key, q] : app.gearQueues)
		{
			while (!q.flight && !q.awaitVersion && !q.wait.empty())
			{
				const GearOrder o = std::move(q.wait.front());
				q.wait.pop_front();
				if (gear_order_send(q, o)) { break; }
				if (!app.sim.inventory(o.inv))
				{
					q.wait.clear();	// the window closed under the queue: nothing left to move
					break;
				}
				gear_order_drop_group(o.group);
				if (o.kind != 65 && !g_noRoomShown) { ui::message("That item moved; the order is dropped."); }
				g_noRoomShown = false;
			}
		}
		std::erase_if(app.gearQueues, [](const auto& kv) { return !kv.second.flight && !kv.second.awaitVersion && kv.second.wait.empty(); });
	}

	// Queues one order (its inventory's queue) and sends it if that queue is free.
	void gear_order_push(GearOrder o) { app.gearQueues[o.inv].wait.push_back(std::move(o)); gear_orders_pump(); }

}

void gear_orders_clear()
{
	app.gearQueues.clear();
	app.gearFollow.clear();
}

// Every frame, after the sim's update: the answers, the INVENTORY versions they brought, then the next orders. A refusal's own text stays the reply
// loop's (the original's wordings); only a stale is handled here (re-resolved once with the new version, then a message).
void gear_orders_frame()
{
	for (const SimView::OrderAnswer& a : app.sim.takeOrderAnswers())
	{
		for (auto& [key, q] : app.gearQueues)
		{
			if (!q.flight || q.seq != a.seq) { continue; }
			q.flight = false;
			if (a.applied)
			{
				// The answer comes with or before the INVENTORY that carries the new version: the next order must wait for that INVENTORY (sending
				// it with the old version is a guaranteed stale; the burst found this).
				q.awaitVersion = true;
				q.answeredAt = gear_now();
				printf("gear order %u applied: %s (inventory %llu)\n", q.current.kind, app.sim.nameOf(q.current.record, app.chars).c_str(), (unsigned long long)key);
				if (q.current.then) { app.gearFollow.push_back({ *q.current.then, gear_now() }); }
				break;
			}
			if (a.reason == "no_room" && q.current.splitOnFull && q.current.count > 1 && kAutoSplit.get() && !ui::ItemGrid::placeOne)
			{
				// Auto-split: one unit at the cell, then the rest unit by unit anywhere (autoplace); a refusal stops the rest (one group).
				const uint64_t group = ++app.gearGroup;
				GearOrder first = q.current;
				first.count = 1;
				first.splitOnFull = false;
				first.group = group;
				GearOrder rest = q.current;
				rest.count = 1;
				rest.splitOnFull = false;
				rest.group = group;
				if (rest.kind == 64) { rest.autoCell = true; }	// the wire's 64 has no autoplace: a free cell is looked up at send time
				else { rest.toSection = 255; }	// 68 autoplaces
				rest.toX = rest.toY = 0;
				for (uint16_t n = 1; n < q.current.count; n++) { q.wait.push_front(rest); }
				q.wait.push_front(std::move(first));
				app.splitQuiet++;
				printf("gear order %u: %u units do not fit the cell; placed one by one\n", q.current.kind, (unsigned)q.current.count);
				break;
			}
			if (a.reason == "stale" && !q.current.retried)
			{
				GearOrder again = q.current;
				again.retried = true;
				q.wait.push_front(std::move(again));
				printf("gear queue: %llu order re-sent once with the new version\n", (unsigned long long)key);
			}
			else
			{
				gear_order_drop_group(q.current.group);
				if (a.reason == "stale") { ui::message("The inventory moved again; the order is dropped."); }
			}
			break;
		}
	}
	const double now = gear_now();
	for (auto& [key, q] : app.gearQueues)
	{
		const simlink::Inventory* inv = app.sim.inventory(key);
		if (!inv) { q.flight = false; q.awaitVersion = false; q.wait.clear(); continue; }	// the inventory is gone (a load, the window closed): its orders with it
		// An applied order's INVENTORY: the commit the answer announced. Its arrival releases the queue; a lost one must not wedge it.
		if (q.awaitVersion)
		{
			if (inv->version != q.version) { q.awaitVersion = false; }
			else if (now - q.answeredAt >= kGearOrderTimeout)
			{
				fprintf(stderr, "kclient: gear queue: no INVENTORY version %u after an applied order in %.0f s; going on\n", q.version, now - q.answeredAt);
				q.awaitVersion = false;
			}
		}
		if (q.flight && now - q.sentAt >= kGearOrderTimeout)	// a lost answer must not wedge the queue
		{
			fprintf(stderr, "kclient: gear queue: no answer for %s in %.0f s; dropped\n", q.current.name.empty() ? "an order" : q.current.name.c_str(), now - q.sentAt);
			q.flight = false;
		}
	}
	// A chained move's second half (a box -> trader drag): sent when the taken item shows in the user's pack, dropped when it never does.
	for (size_t i = 0; i < app.gearFollow.size();)
	{
		const simlink::Inventory* inv = app.sim.inventory(app.gearFollow[i].first.inv);
		if (inv && gear_order_index(app.gearFollow[i].first, *inv) >= 0)
		{
			GearOrder o = std::move(app.gearFollow[i].first);
			app.gearFollow.erase(app.gearFollow.begin() + (long)i);
			gear_order_push(std::move(o));
		}
		else if (!inv || now - app.gearFollow[i].second >= kGearOrderTimeout)
		{
			app.gearFollow.erase(app.gearFollow.begin() + (long)i);
			ui::message("That item moved; the order is dropped.");
		}
		else { i++; }
	}
	gear_orders_pump();
}

// entry i's trade price: the stock's {buy, sell}, the actor's {sell, sell}; none while the inventory is newer than the TRADE that priced it
static TradePrice trade_price(bool stock, uint32_t version, size_t i)
{
	TradePrice p;
	const simlink::SimLinkDecoder::Trade& trade = app.sim.decoder().trade;
	if (!app.tradeStock || !trade.valid) { return p; }
	if (stock && version == trade.stockVersion)
	{
		for (const auto& x : trade.stockPrices) { if (x.index == i) { p = { true, x.buy, x.sell }; } }
		for (const auto& x : trade.buyback) { if (x.index == i) { p.buybackCount = x.count; p.buybackPrice = x.price; } }
	}
	else if (!stock && version == trade.actorVersion)
	{
		for (const auto& x : trade.actorPrices) { if (x.index == i) { p = { true, x.sell, x.sell }; } }
	}
	return p;
}

static void gear_fill(ui::GearGridState& g, uint64_t id, const simlink::Inventory& inv, uint8_t section, int cols, int rows, bool stock, bool priced)
{
	g.cols = cols;
	g.rows = rows;
	for (const simlink::Inventory::Grid& gr : inv.grids) { if (gr.section == section) { g.cols = gr.w; g.rows = gr.h; } }
	for (size_t i = 0; i < inv.items.size(); i++)
	{
		if (inv.items[i].section != section) { continue; }
		g.items.push_back(gear_item(inv.items[i], i, id, inv.version, priced ? trade_price(stock, inv.version, i) : TradePrice{}));
		g.items.back().pending = gear_pending(id, inv.items[i]);	// an order of ours is in flight or queued for it (k16.20.14.2)
	}
}

// A character window's state: the player's own, or a looted body's (its container fields stay empty; every box is a ContainerState).
ui::GearState gear_state(uint64_t owner)
{
	ui::GearState s;
	const simlink::Inventory* have = owner ? app.sim.inventory(owner) : nullptr;
	if (!have) { return s; }
	s.valid = true;
	s.owner = owner;
	s.name = app.sim.decoder().str(app.sim.decoder().chars.at(owner).look.name);
	const simlink::SimLinkDecoder::Trade& trade = app.sim.decoder().trade;
	const bool trading = app.tradeStock && trade.valid && owner == trade.actor;	// its items carry the trader's sell prices
	gear_fill(s.grids[ui::kGearMain], owner, *have, 0, 8, 10, false, trading);
	if (std::any_of(have->grids.begin(), have->grids.end(), [](const simlink::Inventory::Grid& g) { return g.section == 1; })) { gear_fill(s.grids[ui::kGearBag], owner, *have, 1, 0, 0, false, trading); }
	for (int slot : { 1, 2 + 3, 2 + 5, 2 + 6, 2 + 8, 2 + 9, 2 + 12, 2 + 14 })
	{
		const GearSlot* gs = gear_slot(slot);
		ui::GearGridState& g = s.grids[gs->grid];
		g.cols = gs->cols;
		g.rows = gs->rows;
		g.single = true;
		g.accepts = { gs->kind };
	}
	s.grids[ui::kGearWeapon2] = { 7, 1, true, false, { 1 }, {} };
	for (size_t i = 0; i < have->items.size(); i++)
	{
		const simlink::Inventory::Entry& e = have->items[i];
		const GearSlot* gs = e.section == 255 ? gear_slot(e.slot) : nullptr;
		if (!gs) { continue; }
		s.grids[gs->grid].items.push_back(gear_item(e, i, owner, have->version, trading ? trade_price(false, have->version, i) : TradePrice{}));
		s.grids[gs->grid].items.back().pending = gear_pending(owner, e);
		if (gs->grid == ui::kGearBackpack) { s.bagName = s.grids[gs->grid].items.back().name; }
	}
	return s;
}

// A box's, a trader's stock window; invalid (no inventory yet): the window waits.
bool container_state(const App::OpenCont& c, ui::ContainerState& s)
{
	const simlink::Inventory* inv = app.sim.inventory(c.id);
	if (!inv) { return false; }
	s.owner = c.id;
	s.pinned = c.pinned;
	if (c.trade)
	{
		const simlink::SimLinkDecoder::Trade& trade = app.sim.decoder().trade;
		const auto ch = app.sim.decoder().chars.find(trade.trader);
		s.name = ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "TRADER";
		std::transform(s.name.begin(), s.name.end(), s.name.begin(), [](unsigned char x) { return (char)std::toupper(x); });
		s.trade = { true, trade.traderMoney };
		gear_fill(s.grid, c.id, *inv, 0, 8, 8, true, true);
	}
	else
	{
		if (const simlink::Interactable* box = app.sim.thing(c.id)) { s.name = app.sim.nameOf(box->record, app.chars); }
		s.foreign = app.sim.foreign(c.id);
		gear_fill(s.grid, c.id, *inv, 0, 8, 8, false, false);	// UNSURE: 8 x 8 only before the first ITEM_GRID
	}
	return true;
}

// Gear's actions as orders. Items are addressed by the entry index in the INVENTORY of the grid they come from (version = the latest seen). A move inside
// one inventory is 64; between the character and the open container or looted body 68 (a body's worn piece too: it is unequipped into the looter's
// pack, SIMLINK 13; dropped on a slot it autoplaces, MINE); to / from an equipment slot 67 (the slot is the record's own: the server
// puts an unequipped piece where autoplace says, the drop cell is not used; UNSURE). A drag or drop out carries what the UI lifted (one unit, or the
// whole stack with Shift / Ctrl + left, kenshi-k16.20.12.1); a right-click moves one unit and Shift + right-click the stack. Ctrl + right-click is
// move all (ours). The orders go through the per-inventory queue (k16.20.14.2), which sends the version it re-resolves at send time.
static uint16_t gear_count(const ui::GridItem& item)
{
	// Shift + right-click moves the whole stack (the original; kinspect inv_click10). Ctrl + right-click is moveAll now (ours, k16.20.12.1), so Ctrl
	// alone no longer means the stack.
	const bool all = app.keys[SAPP_KEYCODE_LEFT_SHIFT] || app.keys[SAPP_KEYCODE_RIGHT_SHIFT];
	return all ? (uint16_t)item.count : 1;
}

// A producer's output taken by a player character (order 68 take; the server checks the reach: its `use range`). count: what the click carried.
static void machine_take(uint64_t owner, const ui::GridItem& item, uint16_t count)
{
	const simlink::Inventory* inv = app.sim.inventory(app.machine);
	if (!inv || !owner || !item.id) { return; }
	const size_t n = (size_t)item.id - 1;
	if (n >= inv->items.size()) { return; }
	GearOrder o;
	o.kind = 68;
	o.inv = app.machine;	// the order carries the machine's INVENTORY version (its source)
	o.owner = owner;
	o.container = app.machine;
	o.record = inv->items[n].item;
	o.section = inv->items[n].section;
	o.x = inv->items[n].x;
	o.y = inv->items[n].y;
	o.count = count ? count : 1;
	o.take = true;
	o.toSection = 255;
	gear_order_push(std::move(o));
}

// The generic machine window (ui::BuildingWindow) of a producer: its one output slot, the item's footprint (ITEM_GRID section 0). The bar waits for the
// producer's progress on the wire (asked of b0; TASK_PROGRESS 9 is not sent for mines).
ui::BuildingWindowState machine_state()
{
	ui::BuildingWindowState s;
	const simlink::Interactable* t = app.machine ? app.sim.thing(app.machine) : nullptr;
	if (!t || t->kind != simlink::Interactable::kProducer) { return s; }
	s.valid = true;
	s.id = app.machine;
	s.name = app.sim.nameOf(t->record, app.chars);
	ui::BuildingSlot out;
	out.name = app.sim.nameOf(t->output, app.chars);
	out.cols = out.rows = 1;
	if (const simlink::Inventory* inv = app.sim.inventory(app.machine))
	{
		for (const simlink::Inventory::Grid& g : inv->grids) { if (g.section == 0) { out.cols = g.w; out.rows = g.h; } }
		for (size_t i = 0; i < inv->items.size(); i++)
		{
			out.items.push_back(gear_item(inv->items[i], i, app.machine, inv->version));
			out.items.back().pending = gear_pending(app.machine, inv->items[i]);
		}
	}
	out.bar = (ProgressFeed{ t->progress, t->rate, t->progressTick }).at(app.sim.tick());	// SIMLINK kind 4 tail, through the one progress feed
	s.output = out;
	s.outputDropIn = true;	// a producer takes back the item it produces while it fits (user 2026-11-12, k16.19.7 / .8.2); the sim decides, not the slot
	return s;
}

// A right-click on the output moves it to the selection's first player character; the close X / Escape closes the window
ui::BuildingWindowActions machine_actions()
{
	ui::BuildingWindowActions a;
	a.quickMove = [](uint64_t, int, const ui::GridItem& item)
	{
		const auto& sel = app.sim.selected();
		const auto own = std::find_if(sel.begin(), sel.end(), [](uint64_t id) { return app.sim.isPlayer(id); });
		machine_take(own != sel.end() ? *own : 0, item, gear_count(item));	// a right-click: one unit, the stack with Shift
	};
	a.move = [](uint64_t, int from, const ui::GridItem& item, int to, int, int)
	{
		// A drag from the character's own inventory onto the output: order 63 put (take=0) into the producer, carrying what the UI lifted. The sim decides
		// whether it fits and logs a refusal's reason (event 10); the item stays where it was (nothing moves client-side until the wire says so).
		if (to != ui::kBuildingOut || !app.machine || !item.id) { return; }
		const ui::GridRef source = app.items ? app.items->ref(from) : ui::GridRef{};	// the character window the item was dragged from
		if (!source.owner || gear_far(source.owner)) { return; }	// the window's own slots, or an open container's / body's: not the character's
		const uint64_t owner = app.sim.isPlayer(source.owner) ? source.owner : 0;
		const simlink::Inventory* src = owner ? app.sim.inventory(owner) : nullptr;
		const size_t n = (size_t)item.id - 1;
		if (!src || n >= src->items.size()) { return; }
		GearOrder o;
		o.kind = 63;
		o.inv = owner;	// the character's inventory: its moves queue with this one
		o.owner = owner;
		o.container = app.machine;
		o.record = src->items[n].item;
		o.name = app.sim.nameOf(src->items[n].item, app.chars);
		o.section = src->items[n].section;
		o.x = src->items[n].x;
		o.y = src->items[n].y;
		o.count = (uint16_t)std::max<uint32_t>(1, item.count);	// the carried count (k16.20.12.1: the UI lifts one or the stack)
		printf("machine put %s x%u into %llu: queued\n", o.name.c_str(), (unsigned)o.count, (unsigned long long)app.machine);
		gear_order_push(std::move(o));
	};
	a.close = [](uint64_t) { app.machine = 0; };
	return a;
}

// The last clicked producer's focus panel, bottom left (kenshi-k16.19.7.1, k16.19.8.2; the original in kinspect findings/ui_captures/hud_building.md:
// status "Working" / "Output full" / "No workers", then the resource quality, Workers n/max, Efficiency, Max efficiency, and the Condition slat).
// Its values are the producer's BUILDING (47) row, which the sim sends for the session's order 87 focus (SIMLINK "A producer's row"): before the row
// arrives every value reads "-". Nothing is derived client-side (the item's rule): the fields come from the wire or are reported missing.
ui::HudBuildingInfo machine_panel()
{
	ui::HudBuildingInfo b;
	const simlink::Interactable* t = app.panelProducer ? app.sim.thing(app.panelProducer) : nullptr;
	if (!t || t->kind != simlink::Interactable::kProducer) { return b; }
	b.valid = true;
	b.id = app.panelProducer;
	b.name = app.sim.nameOf(t->record, app.chars);
	if (t->hasOwner && t->owner) { b.faction = app.chars.nameOf(app.sim.decoder().str(t->owner)); }
	// Everything else is the producer's BUILDING row (order 87's focus row, k16.19.8.2): a value the row has not brought yet is "-", never derived.
	const simlink::SimLinkDecoder::Building* rec = app.sim.focusBuilding();
	// HealthText (hud_building.md): section 7's status, the producer row's three being 0 Working, 5 Output full and 6 No workers
	if (rec)
	{
		switch (rec->status)
		{
		case 0: b.status = "Working"; break;
		case 1: b.status = "Broken"; b.statusColour = "HudBad"; break;
		case 2: b.status = "Under construction"; break;
		case 3: b.status = "Needs materials"; break;
		case 4: b.status = "Paused"; break;
		case 5: b.status = "Output full"; b.statusColour = "HudBad"; break;
		case 6: b.status = "No workers"; b.statusColour = "HudBad"; break;
		default: break;
		}
	}
	int mining = 0;
	if (const auto it = app.buildRecs.find(app.sim.decoder().str(t->record)); it != app.buildRecs.end()) { mining = it->second.mining; }
	// the resource quality row's label comes from the functionality's `world resource mining` (SIMLINK BUILDING 47's quality row); 4 (oil) UNSURE
	static const char* const kQuality[] = { "", "Iron Quality", "Stone Richness", "Copper Quality", "Carbon Quality", "Water Available", "Ground Fertility" };
	char v[32];
	if (mining > 0 && mining < (int)(sizeof kQuality / sizeof kQuality[0]))
	{
		if (rec)	// the location richness x 100, its colour computed as the original's RGB(clamp(0.8 - v), clamp(0.8 v), 0.1)
		{
			const float q = std::clamp(rec->quality, 0.0f, 1.0f);
			char ink[16];
			snprintf(ink, sizeof ink, "#%02x%02x%02x", (int)(std::clamp(0.8f - q, 0.0f, 1.0f) * 255.0f + 0.5f), (int)(std::clamp(0.8f * q, 0.0f, 1.0f) * 255.0f + 0.5f), 26);
			snprintf(v, sizeof v, "%d", (int)(q * 100.0f + 0.5f));
			b.stats.push_back({ std::string(kQuality[mining]) + ":", v, ink });
		}
		else { b.stats.push_back({ std::string(kQuality[mining]) + ":", "-" }); }
	}
	// Workers n/max from the row (maxWorkers 0 = fully automatic: "-", as before); before the row: "-"
	if (rec && rec->maxWorkers > 0) { snprintf(v, sizeof v, "%u/%u", (unsigned)rec->workers, (unsigned)rec->maxWorkers); b.stats.push_back({ "Workers:", v, rec->workers == 0 ? "HudBad" : "" }); }
	else { b.stats.push_back({ "Workers:", "-" }); }
	if (rec)
	{
		snprintf(v, sizeof v, "%d%%", (int)(rec->efficiency * 100.0f + 0.5f));
		b.stats.push_back({ "Efficiency:", v, rec->efficiency < 0.9f ? "HudBad" : "" });
		snprintf(v, sizeof v, "%d%%", (int)(rec->maxEfficiency * 100.0f + 0.5f));
		b.stats.push_back({ "Max efficiency:", v });
		// the Condition slat (hud_building.md): fill min(1, hp/maxHp), the number (int) hp
		if (rec->maxHp > 0)
		{
			ui::HudBuildingBar bar;
			bar.label = "Condition";
			bar.value = std::to_string((int)rec->hp);
			bar.fraction = std::clamp(rec->hp / rec->maxHp, 0.0f, 1.0f);
			b.bars.push_back(bar);
		}
	}
	else { b.stats.push_back({ "Efficiency:", "-" }); b.stats.push_back({ "Max efficiency:", "-" }); }
	return b;
}

// The clicked entry as the queue remembers it (its record + ITEM_GRID section + cell): the index it has now is found again at send time.
static bool gear_order_of(const simlink::Inventory& inv, const ui::GridItem& item, GearOrder& o)
{
	const size_t n = (size_t)item.id - 1;
	if (!item.id || n >= inv.items.size()) { return false; }
	const simlink::Inventory::Entry& e = inv.items[n];
	o.record = e.item;
	o.section = e.section;
	o.x = e.x;
	o.y = e.y;
	o.hasQuality = e.hasQuality;
	o.grade = e.grade;
	o.level = e.level;
	return true;
}

// The window a quick right click moves an item to (kenshi-k16.20.13.2): the marked one, else the only other window. With several others and none
// marked (the original picks by its window map, rwh.78 Q3; UNSURE here): an item of a box or body goes to the character's window, a character's own
// to the first box opened.
static uint64_t quick_partner(uint64_t from)
{
	if (app.itemTarget && app.itemTarget != from && app.items && app.items->window(app.itemTarget)) { return app.itemTarget; }
	if (gear_far(from) && app.itemsShown && app.gearChar && app.gearChar != from) { return app.gearChar; }
	for (const App::OpenCont& c : app.conts) { if (c.id != from && app.sim.inventory(c.id)) { return c.id; } }
	return 0;
}

// One item moved between two windows (to.grid < 0: autoplaced there), as orders: 64 within an inventory, 67 to / from a slot, 68 between a character
// and a box / body / stock. A box -> trader drag is a take into the user's pack and then a put by record (the wire has no container-to-container
// order), the put sent once the take shows in the pack (gear_orders_frame).
static bool item_route(ui::GridRef from, const ui::GridItem& item, ui::GridRef to, int x, int y, uint16_t count, uint64_t group, bool placed = false)
{
	if (!from.owner || !to.owner || !item.id) { return false; }
	const simlink::Inventory* src = app.sim.inventory(from.owner);
	if (!src) { return false; }
	GearOrder o;
	if (!gear_order_of(*src, item, o)) { return false; }
	o.count = count;
	o.group = group;
	o.toSection = to.grid < 0 ? 255 : gear_section(to.grid);
	o.toX = (uint8_t)x;
	o.toY = (uint8_t)y;
	o.splitOnFull = placed && count > 1 && to.grid >= 0 && !gear_equipment(to.grid);	// a stack dropped at a cell (auto-split)
	const bool ff = gear_far(from.owner), tf = gear_far(to.owner);
	if (from.owner == to.owner)
	{
		o.owner = from.owner;
		o.inv = from.owner;
		if (ff)
		{
			if (to.grid < 0 || gear_equipment(from.grid) || gear_equipment(to.grid)) { return false; }	// a body's worn gear goes only to the looter
			o.kind = 64;
			gear_order_push(std::move(o));
			return true;
		}
		const bool fromSlot = gear_equipment(from.grid), toSlot = to.grid >= 0 && gear_equipment(to.grid);
		if (fromSlot) { if (toSlot) { return false; } o.kind = 67; o.on = false; gear_order_push(std::move(o)); return true; }
		if (toSlot) { o.kind = 67; o.on = true; gear_order_push(std::move(o)); return true; }
		o.kind = 64;
		gear_order_push(std::move(o));
		return true;
	}
	if (!ff && tf)	// a put: ours into a box, a body or a stock (a sale)
	{
		if (gear_equipment(from.grid)) { return false; }	// ours worn: unequip it first
		o.kind = 68;
		o.owner = from.owner;
		o.inv = from.owner;
		o.container = to.owner;
		o.take = false;
		gear_order_push(std::move(o));
		return true;
	}
	if (ff && !tf)	// a take: into the character the item was dropped on
	{
		o.kind = 68;
		o.owner = to.owner;
		o.inv = from.owner;
		o.container = from.owner;
		o.take = true;
		gear_order_push(std::move(o));
		return true;
	}
	if (ff && tf)
	{
		const uint64_t actor = cont_actor(from.owner) ? cont_actor(from.owner) : cont_actor(to.owner);
		if (!actor || !app.sim.inventory(actor)) { return false; }
		GearOrder put = o;
		put.kind = 68;
		put.byRecord = true;
		put.owner = actor;
		put.inv = actor;
		put.container = to.owner;
		put.take = false;
		put.toSection = 255;
		put.section = 0;
		put.splitOnFull = false;
		o.kind = 68;
		o.owner = actor;
		o.inv = from.owner;
		o.container = from.owner;
		o.take = true;
		o.toSection = 255;
		o.splitOnFull = false;
		o.then = std::make_shared<GearOrder>(std::move(put));
		gear_order_push(std::move(o));
		return true;
	}
	return false;
}

// Every entry of the clicked item's grid with its record (and grade and level, when it has them), whole stacks, to `partner`: one order each in one
// group, so the first refusal stops the rest (gear_orders_frame drops the group). Ctrl + right-click and the menu's "all" rows (kenshi-k16.20.13.3).
static void move_all(ui::GridRef at, const ui::GridItem& item, uint64_t partner)
{
	const simlink::Inventory* inv = partner ? app.sim.inventory(at.owner) : nullptr;
	if (!inv || !item.id) { return; }	// no partner window open: nothing to move them to
	GearOrder pick;
	if (!gear_order_of(*inv, item, pick)) { return; }
	const uint8_t section = gear_section(at.grid);
	const uint64_t group = ++app.gearGroup;
	size_t queued = 0;
	for (size_t i = 0; i < inv->items.size(); i++)
	{
		const simlink::Inventory::Entry& e = inv->items[i];
		if (e.section != section || e.item != pick.record) { continue; }
		if (pick.hasQuality && (!e.hasQuality || e.grade != pick.grade || e.level != pick.level)) { continue; }
		ui::GridItem one = item;
		one.id = (decltype(one.id))(i + 1);
		if (item_route(at, one, { partner, -1 }, 0, 0, (uint16_t)std::max<uint32_t>(1, e.count), group)) { ++queued; }	// the whole stack
	}
	printf("gear moveAll: group %llu, %zu %s stacks into %llu\n", (unsigned long long)group, queued, app.sim.nameOf(pick.record, app.chars).c_str(), (unsigned long long)partner);
}

// How many entries of the clicked item's grid share its record (and grade and level): the menu's "all of this type" row shows from 2.
static size_t same_kind_entries(ui::GridRef at, const ui::GridItem& item)
{
	const simlink::Inventory* inv = at.owner ? app.sim.inventory(at.owner) : nullptr;
	GearOrder pick;
	if (!inv || !item.id || !gear_order_of(*inv, item, pick)) { return 0; }
	size_t n = 0;
	for (const simlink::Inventory::Entry& e : inv->items)
	{
		if (e.section != gear_section(at.grid) || e.item != pick.record) { continue; }
		if (pick.hasQuality && (!e.hasQuality || e.grade != pick.grade || e.level != pick.level)) { continue; }
		n++;
	}
	return n;
}

// A window's title for the menu rows: a box's or trader's name, a character's.
static std::string window_name(uint64_t id)
{
	if (const App::OpenCont* c = cont_find(id))
	{
		if (c->trade)
		{
			const auto ch = app.sim.decoder().chars.find(app.sim.decoder().trade.trader);
			return ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "the trader";
		}
		if (!c->body) { if (const simlink::Interactable* box = app.sim.thing(id)) { return app.sim.nameOf(box->record, app.chars); } }
	}
	const auto ch = app.sim.decoder().chars.find(id);
	return ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "the container";
}

// The other item windows an item of `from` could go to, the marked one first.
static std::vector<uint64_t> item_destinations(uint64_t from)
{
	std::vector<uint64_t> d;
	if (app.itemsShown && app.gearChar && app.gearChar != from && app.sim.inventory(app.gearChar)) { d.push_back(app.gearChar); }
	for (const App::OpenCont& c : app.conts) { if (c.id != from && app.sim.inventory(c.id)) { d.push_back(c.id); } }
	const auto marked = std::find(d.begin(), d.end(), app.itemTarget);
	if (app.itemTarget && marked != d.end()) { std::rotate(d.begin(), marked, marked + 1); }
	return d;
}

// What a right click on an item of window `from` would do (the badge, and whether the click opens the menu): the marked window, else the only other
// window; ambiguous (several, none marked): the menu; no other window: none (a worn piece unequips, a stored one equips).
// The worn shackles' lock when this is a worn item with a locked one (ITEM_LOCK 57); else nullptr. A locked worn item has no unequip: its right click is
// the menu with the three ways through it.
static const simlink::Inventory::Lock* worn_lock(ui::GridRef at, const ui::GridItem& item)
{
	if (!at.owner || !item.id || gear_far(at.owner) || !gear_equipment(at.grid)) { return nullptr; }
	const simlink::Inventory* inv = app.sim.inventory(at.owner);
	const simlink::Inventory::Lock* l = inv ? inv->lockOf((size_t)item.id - 1) : nullptr;
	return l && l->locked ? l : nullptr;
}

static ui::RightVerb item_verb(ui::GridRef at, const ui::GridItem& item)
{
	if (worn_lock(at, item)) { return ui::RightVerb::kMenu; }
	if (!at.owner || !item.id || (!gear_far(at.owner) && gear_equipment(at.grid))) { return ui::RightVerb::kNone; }
	const std::vector<uint64_t> d = item_destinations(at.owner);
	if (d.empty()) { return ui::RightVerb::kNone; }
	if (d.size() > 1 && d.front() != app.itemTarget) { return ui::RightVerb::kMenu; }
	const App::OpenCont* to = cont_find(d.front());
	const App::OpenCont* from = cont_find(at.owner);
	if (to && to->trade) { return ui::RightVerb::kSell; }
	if (from && from->trade) { return ui::RightVerb::kBuy; }
	return ui::RightVerb::kTransfer;
}

// The menu's actions: row = kind * 100 + destination slot (into g_menuDest, filled by the menu query that opened it).
enum { kRowOne = 1, kRowStack, kRowAll, kRowEquip, kRowUnequip, kRowDrop, kRowDropStack, kRowPickLock, kRowUseTools, kRowUseStrength };
static std::vector<uint64_t> g_menuDest;

// The item windows' actions. Items are addressed by the entry index in the INVENTORY of the grid they come from (version = the latest seen). A move
// inside one inventory is 64; between a character and a box / body / stock 68 (a body's worn piece too: it is unequipped into the looter's pack,
// SIMLINK 13; dropped on a slot it autoplaces, MINE); to / from an equipment slot 67 (the slot is the record's own: the server puts an unequipped
// piece where autoplace says, the drop cell is not used; UNSURE). A drag or drop out carries what the UI lifted (one unit, or the whole stack with
// Shift / Ctrl + left, kenshi-k16.20.12.1); a right-click moves one unit and Shift + right-click the stack, to the marked window else the only
// other one. Ctrl + right-click is move all (ours). The orders go through the per-inventory queue (k16.20.14.2).
ui::ItemWindowsActions item_actions()
{
	ui::ItemWindowsActions a;
	a.move = [](ui::GridRef from, const ui::GridItem& item, ui::GridRef to, int x, int y)
	{
		const uint16_t count = (uint16_t)std::max<uint32_t>(1, item.count);	// what the UI lifted (k16.20.12.1)
		if (!from.owner)	// the machine window's output (autoplaced into the window it was dropped on)
		{
			if (to.owner) { machine_take(cont_actor(to.owner), item, count); }
			return;
		}
		item_route(from, item, to, x, y, count, 0, true);
	};
	// Right-click on release (kinspect inv_click10; double-click is unbound in the original): the item moves to the partner window (autoplaced);
	// else a worn piece unequips and a stored equippable equips. MINE / not done: the one-window re-pack of other items, and the use actions
	// (blueprint learn, map reveal, money) need sim orders; the failure messages ("Can't wear that item") come from the server replies.
	a.quick = [](ui::GridRef at, const ui::GridItem& item, uint32_t)
	{
		const simlink::Inventory* inv = at.owner ? app.sim.inventory(at.owner) : nullptr;
		if (!inv || !item.id) { return; }
		const bool far = gear_far(at.owner);
		const uint16_t count = gear_count(item);	// one unit; Shift + right-click the stack
		GearOrder o;
		if (!far && gear_equipment(at.grid))
		{
			if (!gear_order_of(*inv, item, o)) { return; }
			o.kind = 67;
			o.owner = o.inv = at.owner;
			o.on = false;
			o.count = count;
			gear_order_push(std::move(o));
			return;
		}
		if (const uint64_t partner = quick_partner(at.owner)) { item_route(at, item, { partner, -1 }, 0, 0, count, 0); return; }
		if (!far && item.kind && gear_order_of(*inv, item, o))
		{
			o.kind = 67;
			o.owner = o.inv = at.owner;
			o.on = true;
			o.count = count;
			gear_order_push(std::move(o));
		}
	};
	// Ctrl + right-click (ours, kenshi-k16.20.12.1): every entry of that grid with the clicked item's record (and its grade and level, when it has
	// them), whole stacks, to the window a right-click moves to. The first refusal stops the rest (gear_orders_frame drops the group).
	a.moveAll = [](ui::GridRef at, const ui::GridItem& item) { move_all(at, item, at.owner ? quick_partner(at.owner) : 0); };
	a.dropOutside = [](ui::GridRef at, const ui::GridItem& item)
	{
		const simlink::Inventory* inv = at.owner ? app.sim.inventory(at.owner) : nullptr;
		if (!inv || gear_far(at.owner) || gear_equipment(at.grid) || !item.id) { return; }
		GearOrder o;
		if (!gear_order_of(*inv, item, o)) { return; }
		o.kind = 66;
		o.inv = at.owner;
		o.owner = at.owner;
		o.count = (uint16_t)std::max<uint32_t>(1, item.count);
		gear_order_push(std::move(o));
	};
	a.verb = [](ui::GridRef at, const ui::GridItem& item, uint32_t) { return item_verb(at, item); };
	// The item menu (hold right, or an ambiguous right click): per destination window the unit, the stack and all of the type (verb by the two
	// windows: sell to a trader, buy from one, move between ours, pick up from a box or body), then equip / unequip and drop. The binding shows on the
	// rows a click already does: only when the click is not ambiguous.
	a.menu = [](ui::GridRef at, const ui::GridItem& item)
	{
		std::vector<ui::ItemMenuRow> rows;
		if (!at.owner || !item.id) { return rows; }
		if (const simlink::Inventory::Lock* lock = worn_lock(at, item))	// locked shackles (order 60 actions 15 / 16 / 17, SIMLINK "Shackle orders"): the three ways through
		{
			char pick[40];
			snprintf(pick, sizeof pick, "Pick Lock %.1f%%", std::floor(lock->chance * 1000.0f) * 0.1f);	// the original's int(chance x 1000) x 0.1
			rows.push_back({ pick, "", true, kRowPickLock * 100 });
			rows.push_back({ "Use Tools", "", true, kRowUseTools * 100 });
			rows.push_back({ "Use Strength", "", true, kRowUseStrength * 100 });
			return rows;
		}
		const bool far = gear_far(at.owner), worn = !far && gear_equipment(at.grid);
		const App::OpenCont* from = cont_find(at.owner);
		g_menuDest.clear();
		if (!worn) { g_menuDest = item_destinations(at.owner); }
		const bool clicked = item_verb(at, item) != ui::RightVerb::kMenu && item_verb(at, item) != ui::RightVerb::kNone;
		const size_t kinds = same_kind_entries(at, item);
		const simlink::Inventory* inv = app.sim.inventory(at.owner);
		if (!inv || (size_t)item.id > inv->items.size()) { return rows; }
		const std::string kind = app.sim.nameOf(inv->items[(size_t)item.id - 1].item, app.chars);
		for (size_t i = 0; i < g_menuDest.size(); i++)
		{
			const App::OpenCont* to = cont_find(g_menuDest[i]);
			const char* verb = to && to->trade ? "Sell" : from && from->trade ? "Buy" : far ? "Pick up" : "Move";
			const std::string where = g_menuDest.size() > 1 ? " to " + window_name(g_menuDest[i]) : std::string();
			const bool bound = clicked && i == 0;
			rows.push_back({ std::string(verb) + where, bound ? "Right click" : "", true, kRowOne * 100 + (int)i });
			if (item.count > 1) { rows.push_back({ std::string(verb) + " stack" + where, bound ? "Shift+Right click" : "", true, kRowStack * 100 + (int)i }); }
			if (kinds > 1) { rows.push_back({ std::string(verb) + " all " + kind + where, bound ? "Ctrl+Right click" : "", true, kRowAll * 100 + (int)i }); }
		}
		const bool lone = g_menuDest.empty();	// nothing to move it to: the plain right click equips / unequips
		if (worn) { rows.push_back({ "Unequip", "Right click", true, kRowUnequip * 100 }); }
		else if (!far && item.kind) { rows.push_back({ "Equip", lone ? "Right click" : "", true, kRowEquip * 100 }); }
		if (!far && !worn)
		{
			rows.push_back({ "Drop", "Drag outside", true, kRowDrop * 100 });
			if (item.count > 1) { rows.push_back({ "Drop stack", "", true, kRowDropStack * 100 }); }
		}
		return rows;
	};
	a.pick = [](ui::GridRef at, const ui::GridItem& item, int action)
	{
		const int row = action / 100;
		const size_t slot = (size_t)(action % 100);
		const uint64_t dest = slot < g_menuDest.size() ? g_menuDest[slot] : 0;
		const simlink::Inventory* inv = at.owner ? app.sim.inventory(at.owner) : nullptr;
		if (!inv || !item.id) { return; }
		GearOrder o;
		switch (row)
		{
			case kRowOne: if (dest) { item_route(at, item, { dest, -1 }, 0, 0, 1, 0); } break;
			case kRowStack: if (dest) { item_route(at, item, { dest, -1 }, 0, 0, (uint16_t)std::max<uint32_t>(1, item.count), 0); } break;
			case kRowAll: move_all(at, item, dest); break;
			case kRowEquip:
			case kRowUnequip:
				if (!gear_order_of(*inv, item, o)) { return; }
				o.kind = 67;
				o.owner = o.inv = at.owner;
				o.on = row == kRowEquip;
				o.count = 1;
				gear_order_push(std::move(o));
				break;
			case kRowPickLock:
			case kRowUseTools:
			case kRowUseStrength:
			{
				const bool sent = app.sim.shackle(at.owner, at.owner, (uint8_t)(15 + row - kRowPickLock));
				printf("shackles: %s on %llu: %s\n", row == kRowPickLock ? "pick" : row == kRowUseTools ? "cut" : "break", (unsigned long long)at.owner, sent ? "sent" : "not sent");
				break;
			}
			case kRowDrop:
			case kRowDropStack:
				if (!gear_order_of(*inv, item, o)) { return; }
				o.kind = 66;
				o.owner = o.inv = at.owner;
				o.count = row == kRowDrop ? 1 : (uint16_t)std::max<uint32_t>(1, item.count);
				gear_order_push(std::move(o));
				break;
		}
	};
	a.arrange = [](ui::GridRef at)
	{
		if (!at.owner) { return; }
		GearOrder o;
		o.kind = 65;
		o.inv = at.owner;
		o.owner = at.owner;
		o.section = (uint8_t)(at.grid == ui::kGearBag ? 1 : 0);
		gear_order_push(std::move(o));
	};
	a.mark = [](uint64_t owner) { app.itemTarget = owner; };	// the single target; 0: unmarked
	a.close = [](uint64_t owner)
	{
		if (cont_find(owner))	// a body's (the loot ends) or a box's or a stock's window
		{
			printf("window %llu closed\n", (unsigned long long)owner);
			app.gearQueues.erase(owner);
			cont_close(owner);
			return;
		}
		app.itemsShown = false;	// the character's
		if (app.itemTarget == owner) { app.itemTarget = 0; }
	};
	a.pin = [](uint64_t owner, bool on)
	{
		if (App::OpenCont* c = cont_find(owner)) { c->pinned = on; }
		else { app.charPinned = on; }
	};
	return a;
}

}
