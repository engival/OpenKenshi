#include "ksim/combat_data.hpp"

#include "kdata/format.hpp"
#include "ksim/rng.hpp"
#include "kworld/ogreskeleton.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace ksim {

namespace {

std::int32_t typeIndex(std::string_view name)
{
	for (std::int32_t i = 0;; ++i)
	{
		const char* candidate = kdata::itemTypeName(i);
		if (candidate == nullptr)
			break;
		if (name == candidate)
			return i;
	}
	throw std::runtime_error("combat data: unknown record type " + std::string(name));
}

double lerp(double t, double a, double b) noexcept
{
	return a + (b - a) * t;
}

// A numeric key stored as float or int. Throws when absent: constants have no defaults here.
double number(const kdata::Record& record, std::string_view key)
{
	if (const kdata::FieldFloat* field = record.findFloat(key))
		return kdata::decodeFloat(*field);
	if (const kdata::FieldInt* field = record.findInt(key))
		return field->value;
	throw std::runtime_error("combat data: record '" + record.name + "' has no key '"
		+ std::string(key) + "'");
}

double numberOr(const kdata::Record& record, std::string_view key, double fallback)
{
	if (const kdata::FieldFloat* field = record.findFloat(key))
		return kdata::decodeFloat(*field);
	if (const kdata::FieldInt* field = record.findInt(key))
		return field->value;
	return fallback;
}

const kdata::Record& requireRecord(const kdata::GameData& gameData, std::string_view name,
	std::int32_t type)
{
	for (const kdata::Record* record : gameData.byType(type))
	{
		if (record->name == name)
			return *record;
	}
	throw std::runtime_error("combat data: record '" + std::string(name) + "' not found");
}

std::string normalisePath(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	if (path.rfind("./", 0) == 0)
		path.erase(0, 2);
	if (path.rfind("data/", 0) == 0)
		path.erase(0, 5);
	return path;
}

} // namespace

Clip::Sample Clip::at(double t) const noexcept
{
	if (root.empty() || step <= 0.0)
		return {};
	const double u = std::clamp(t, 0.0, length) / step;
	const std::size_t i = std::min(static_cast<std::size_t>(u), root.size() - 1);
	const std::size_t j = std::min(i + 1, root.size() - 1);
	const float f = static_cast<float>(u - static_cast<double>(i));
	return Sample{root[i].x + (root[j].x - root[i].x) * f, root[i].z + (root[j].z - root[i].z) * f,
		root[i].yaw + (root[j].yaw - root[i].yaw) * f};
}

const Technique* CombatData::technique(std::string_view name) const noexcept
{
	const std::int32_t index = techniqueIndex(name);
	return index < 0 ? nullptr : &techniques[static_cast<std::size_t>(index)];
}

std::int32_t CombatData::techniqueIndex(std::string_view name) const noexcept
{
	for (std::size_t i = 0; i < techniques.size(); ++i)
	{
		if (techniques[i].name == name)
			return static_cast<std::int32_t>(i);
	}
	return -1;
}

const Clip* CombatData::clip(std::string_view name) const noexcept
{
	for (const Clip& clip : clips)
	{
		if (clip.name == name)
			return &clip;
	}
	return nullptr;
}

const RaceCombat* CombatData::race(std::string_view sidOrName) const noexcept
{
	if (!raceIndex.empty())
	{
		const auto found = raceIndex.find(sidOrName);
		return found != raceIndex.end() ? &races[found->second] : nullptr;
	}
	for (const RaceCombat& race : races)
	{
		if (race.sid == sidOrName || race.name == sidOrName)
			return &race;
	}
	return nullptr;
}

std::int32_t CombatData::anatomyIndex(std::string_view name) const noexcept
{
	for (std::size_t i = 0; i < anatomy.size(); ++i)
	{
		if (anatomy[i].name == name)
			return static_cast<std::int32_t>(i);
	}
	return -1;
}

std::shared_ptr<const CombatData> CombatData::build(const kdata::GameData& gameData,
	std::string_view dataDir)
{
	auto data = std::make_shared<CombatData>();
	CombatData& out = *data;

	// ---- GLOBAL CONSTANTS, after the loader's scaling (RESOLUTION.md 0.1) ----
	const kdata::Record& c = requireRecord(gameData, "GLOBAL CONSTANTS", typeIndex("CONSTANTS"));
	CombatConstants& k = out.k;
	const double damageMult = number(c, "damage multiplier");
	k.maxNumAttackSlots = static_cast<std::int32_t>(number(c, "max num attack slots"));
	k.attackChanceFactor = number(c, "attack chance factor");
	k.baseBlockChance = number(c, "base block chance");
	k.blockReductionPerLevel = number(c, "block chance reduction per 10levels") * 0.1;
	k.blockIncreasePerLevel = number(c, "block chance increase per 10levels") * 0.1;
	k.cutDamage1 = number(c, "cut damage 1") * damageMult;
	k.cutDamage99 = number(c, "cut damage 99") * damageMult;
	k.bluntDamage1 = number(c, "blunt damage 1") * damageMult;
	k.bluntDamage99 = number(c, "blunt damage 99") * damageMult;
	k.pierceDamageMultiplier = number(c, "pierce damage multiplier") * damageMult;
	k.unarmedDamageMult = number(c, "unarmed damage mult");
	k.resistanceMin = number(c, "damage resistance min");
	k.resistanceMax = number(c, "damage resistance max");
	// The exe reads it as an int, then scales it.
	k.stumbleDamageMax = std::trunc(number(c, "stumble damage max")) * damageMult;
	k.immediateBloodLoss = number(c, "immediate blood loss");
	k.bleedRate = number(c, "bleed rate") * 0.1;
	k.bleedClotRate = number(c, "bleeding clot rate") * 0.1;
	k.bloodRecoveryRate = number(c, "blood recovery rate") * 0.1;
	k.extraBloodLoss = number(c, "extra blood loss from bodyparts");
	k.bluntPermanentOrganDamage = number(c, "blunt permanent organ damage");
	k.minToughnessKoPoint = number(c, "min toughness ko point");
	k.maxToughnessKoPoint = number(c, "max toughness ko point");
	k.knockoutTimeBase = number(c, "knockout time base");
	k.knockoutMult1 = number(c, "knockout mult 1");
	k.knockoutMult99 = number(c, "knockout mult 99");
	k.stunRecoveryRate = number(c, "stun recovery rate");
	k.healRateMult = number(c, "heal rate mult");
	k.restingHealRateMult = number(c, "resting heal rate mult");
	k.xpRateStrength = number(c, "xp rate strength");
	k.xpRateToughness = number(c, "xp rate toughness");
	k.expGainMultiplier = number(c, "exp gain multiplier");
	k.skillDiffXp0xPenalty = number(c, "skill diff xp 0x penalty");
	k.skillDiffXp2xBonus = number(c, "skill diff xp 2x bonus");
	k.minStrengthXpMult = number(c, "min strength xp mult");
	k.weightStrengthDiff1x = number(c, "weight strength diff 1x");
	k.weightStrengthDiffMax = number(c, "weight strength diff max");
	k.robotWearRate = number(c, "robot wear rate");
	k.medicSpeed = number(c, "medic speed mult");
	k.robotMedicSpeed = number(c, "robot medic speed mult");
	k.medkitDrain1 = number(c, "medkit drain 1") * 0.1;
	k.medkitDrain99 = number(c, "medkit drain 99") * 0.1;
	k.degenerationMult1 = number(c, "degeneration mult 1");
	k.degenerationMult99 = number(c, "degeneration mult 99");
	k.bodypartDegenerationRate = number(c, "bodypart degeneration rate");
	k.starvationHours = number(c, "starvation time");
	k.bedHungerRate = number(c, "bed hunger rate");
	k.encumbranceHungerRate = number(c, "encumbrance hunger rate");
	k.carryWeightMult = numberOr(c, "carry weight mult", k.carryWeightMult);
	k.encumbranceBase = numberOr(c, "encumbrance base", k.encumbranceBase);
	k.carryPersonWeight = numberOr(c, "carry person weight", k.carryPersonWeight);
	k.xpMedic1 = number(c, "XP rate medic 1");
	k.xpMedic99 = number(c, "XP rate medic 99");

	// ---- Body parts ----
	std::unordered_map<std::string, std::int32_t> anatomyBySid;
	for (const kdata::Record* record : gameData.byType(typeIndex("LOCATIONAL_DAMAGE")))
	{
		AnatomyPart part;
		part.sid = record->stringID;
		part.name = record->name;
		part.type = record->intOr("body part type", 0);
		part.koMult = numberOr(*record, "KO mult", 1.0);
		part.collapseBits = record->intOr("collapse part", 0);
		part.collapses = record->boolOr("collapses", false);
		part.death = record->boolOr("death", false);
		part.severance = record->boolOr("severance", false);
		part.affectsSkills = numberOr(*record, "affects skills", 0.0);
		part.affectsMoveSpeed = numberOr(*record, "affects move speed", 0.0);
		anatomyBySid.emplace(record->stringID, static_cast<std::int32_t>(out.anatomy.size()));
		out.anatomy.push_back(std::move(part));
	}

	// ---- Items (food, first aid kits) ----
	for (const kdata::Record* record : gameData.byType(typeIndex("ITEM")))
	{
		ItemInfo info;
		info.function = record->intOr("item function", 0);
		info.charges = numberOr(*record, "charges", 1.0);
		info.quality = numberOr(*record, "quality", 1.0);
		out.items.emplace(record->stringID, info);
	}

	// ---- Races ----
	for (const kdata::Record* record : gameData.byType(typeIndex("RACE")))
	{
		RaceCombat race;
		race.sid = record->stringID;
		race.name = record->name;
		race.strength = numberOr(*record, "strength", 1.0);
		race.dexterity = numberOr(*record, "dexterity", 1.0);
		race.combatMoveSpeedMult = numberOr(*record, "combat move speed mult", 1.0);
		race.walkSpeed = numberOr(*record, "walk speed", 15.0);
		race.speedMinSkill = numberOr(*record, "speed min skill", 70.0);
		race.speedMaxSkill = numberOr(*record, "speed max skill", 120.0);
		race.bloodColour = record->intOr("blood colour", 0x800000);
		race.minBlood = numberOr(*record, "min blood", 75.0);
		race.maxBlood = numberOr(*record, "max blood", 150.0);
		race.bleedRate = numberOr(*record, "bleed rate", 1.0);
		race.healRate = numberOr(*record, "heal rate", 1.0);
		race.hungerRate = numberOr(*record, "hunger rate", 1.0);
		race.visionRangeMult = numberOr(*record, "vision range mult", 1.0);
		race.pathfindAcceleration = numberOr(*record, "pathfind acceleration", 20.0);
		// RACE `water avoidance` -> the cost scale. rwh144 documents human 7.0 (data +6) and Fishman 0.25 (data -3): 1 + a for a >= 0 and
		// 1 / (1 - a) below fits both points (a bare 1 + a would be negative for the Fishman). UNSURE for other negative values.
		const double avoidance = numberOr(*record, "water avoidance", 0.0);
		race.waterCost = avoidance >= 0.0 ? 1.0 + avoidance : 1.0 / (1.0 - avoidance);
		race.footprintRadius = numberOr(*record, "pathfind footprint radius", 2.6);
		// Task BQ: RaceCombat::bodyWidth. Every humanoid race has `hull size X` 5 (4.0, the box's old constant); the beak has 18.
		race.bodyWidth = numberOr(*record, "hull size X", 5.0) / 2.0 + 1.5;
		const kdata::RefCategory* femaleHeads = record->findRefs("heads female");
		race.hasFemale = femaleHeads != nullptr && !femaleHeads->entries.empty();
		race.carriable = record->boolOr("carriable", true);
		race.isRobot = record->boolOr("is robot", false);
		race.vampiric = record->boolOr("vampiric", false); // RACE +0x7d (dt39 Q8): the blood-drain eater (spiders, skimmer, land bat)
		race.extraAttackSlots = record->intOr("extra attack slots", 0);
		// `weather immunity0..N`: the VALUE is the affect type, a 0 is skipped (kinspect RaceData::load), so a race that lists 2
		// is immune to acid rain.
		for (const kdata::FieldInt& field : record->ints)
			if (field.key.starts_with("weather immunity") && field.value > 0 && field.value < 32)
				race.weatherImmunity |= 1u << field.value;
		if (const kdata::RefCategory* refs = record->findRefs("combat anatomy"))
		{
			for (const kdata::RefEntry& entry : refs->entries)
			{
				const auto found = anatomyBySid.find(entry.targetID);
				if (entry.opaqueEntry || found == anatomyBySid.end())
					continue;
				race.anatomy.push_back(RaceAnatomyEntry{found->second,
					static_cast<double>(entry.v0), static_cast<double>(entry.v1)});
			}
		}
		out.races.push_back(std::move(race));
	}
	for (std::size_t i = 0; i < out.races.size(); ++i)
	{
		out.raceIndex.emplace(out.races[i].sid, i); // emplace keeps the first: the linear scan's first match
		out.raceIndex.emplace(out.races[i].name, i);
	}

	// ---- Techniques ----
	for (const kdata::Record* record : gameData.byType(typeIndex("COMBAT_TECHNIQUE")))
	{
		Technique t;
		t.name = record->name;
		t.animName = record->stringOr("anim name");
		t.speedMult = numberOr(*record, "anim speed mult", 1.0);
		t.isBlock = record->boolOr("is block", false);
		t.isDodge = record->boolOr("is dodge", false);
		t.isStumbleDodge = record->boolOr("is stumble dodge", false);
		t.gainsGround = record->boolOr("gains ground", false);
		t.isProne = record->boolOr("is prone", false);
		t.lowStrike = record->boolOr("low strike", false);
		t.hesitate = numberOr(*record, "anim hesitate point", 0.2);
		t.attackDistance = numberOr(*record, "attack distance", 0.0);
		t.attackDistanceMinVsStatic = numberOr(*record, "attack distance min vs static", 0.0);
		t.acceptableEndTime = numberOr(*record, "acceptable end time", 0.95);
		t.minSkill = numberOr(*record, "min skill", -100.0);
		t.maxSkill = numberOr(*record, "max skill", 999.0);
		t.maxEncumbrance = numberOr(*record, "max encumbrance", 999.0);
		t.chance = numberOr(*record, "chance", 1.0);
		t.maxHits = record->intOr("max simultaneous hits", 99);
		t.animal = record->intOr("animal", 0);
		t.disabled = record->boolOr("disabled", false);
		// Weapon-type flags, by the exe's weapon type index (RESOLUTION.md 1.3).
		t.weapon[0] = record->boolOr("katanas", true);
		t.weapon[1] = record->boolOr("sabre", true);
		t.weapon[2] = record->boolOr("blunt", true);
		t.weapon[3] = record->boolOr("heavy weapons", true);
		t.weapon[4] = record->boolOr("hackers", true);
		t.weapon[5] = record->boolOr("unarmed", true);
		t.weapon[8] = record->boolOr("polearm", true);
		const double numFrames = std::max(1.0, numberOr(*record, "num frames", 1.0));
		const std::int32_t count = std::max(1, record->intOr("num techniques", 1));
		for (std::int32_t i = 1; i <= count; ++i)
		{
			const std::string suffix = " " + std::to_string(i);
			ImpactPoint impact;
			impact.dir = record->intOr("attack direction" + suffix, 1);
			impact.power = numberOr(*record, "power" + suffix, 100.0) / 100.0;
			// Values <= 1 are fractions; larger ones are frames (RESOLUTION.md 1.1).
			const double hit = numberOr(*record, "anim blocked frame" + suffix, 0.8);
			const double stop = numberOr(*record, "anim stop frame" + suffix, 1.0);
			impact.hitFrac = hit <= 1.0 ? hit : hit / numFrames;
			impact.stopFrac = stop <= 1.0 ? stop : stop / numFrames;
			impact.limb = record->intOr("limb" + suffix, 4);
			t.impacts.push_back(impact);
		}
		out.techniques.push_back(std::move(t));
	}

	// ---- Clips: lengths and root motion of the human clips (shared Ogre reader) ----
	std::string skeletonFile = ".\\data\\character\\meshes\\male_skeleton\\male_skeleton.skeleton";
	for (const kdata::Record* record : gameData.byType(typeIndex("ANIMATION_FILE")))
	{
		const std::string& value = record->fileOr("male animation");
		if (!value.empty())
			skeletonFile = value;
	}
	const kworld::Skeleton skeleton =
		kworld::loadSkeleton(std::string(dataDir) + "/" + normalisePath(skeletonFile));
	const int rootBone = skeleton.bone("Bip01");
	if (rootBone < 0)
		throw std::runtime_error("combat data: the human skeleton has no Bip01 bone");
	auto addClip = [&](const std::string& name) -> std::int32_t
	{
		for (std::size_t i = 0; i < out.clips.size(); ++i)
		{
			if (out.clips[i].name == name)
				return static_cast<std::int32_t>(i);
		}
		const kworld::SkeletonAnimation* animation = skeleton.animation(name);
		if (animation == nullptr)
			return -1;
		Clip clip;
		clip.name = name;
		clip.length = animation->length;
		clip.step = 1.0 / 60.0;
		const std::size_t samples = static_cast<std::size_t>(std::ceil(clip.length / clip.step)) + 1;
		clip.root.reserve(samples);
		clip.root.push_back({});
		float x = 0.0f;
		float z = 0.0f;
		float yaw = 0.0f;
		for (std::size_t i = 1; i < samples; ++i)
		{
			const float t0 = static_cast<float>(static_cast<double>(i - 1) * clip.step);
			const float t1 = std::min(static_cast<float>(static_cast<double>(i) * clip.step),
				animation->length);
			const kworld::SkelMotion motion = kworld::rootMotion(skeleton, *animation, rootBone, t0, t1);
			x += motion.delta.x;
			z += motion.delta.z;
			yaw += motion.yaw;
			clip.root.push_back({x, z, yaw});
		}
		out.clips.push_back(std::move(clip));
		return static_cast<std::int32_t>(out.clips.size() - 1);
	};
	for (Technique& t : out.techniques)
	{
		if (t.animal == 0 || t.animal == 5) // the human set; animal clips live in other skeletons
			t.clip = addClip(t.animName);
		else if (!t.impacts.empty() && t.impacts.front().hitFrac > 0.0)
			t.length = kAnimalAttackSeconds / t.impacts.front().hitFrac; // task BQ: no animal clip; the measured impact time decides
	}
	out.stumbleClip = addClip("mid blow light");
	out.stumbleHeavyClip = addClip("mid blow");
	out.unarmed = out.makeFists(gameData);
	return data;
}

state::FighterWeapon CombatData::makeWeapon(const kdata::Record& weapon, double level,
	double manufacturerCut, double manufacturerBlunt, double manufacturerWeight) const
{
	state::FighterWeapon w;
	w.name = weapon.name;
	w.sid = weapon.stringID;
	w.weaponType = weapon.intOr("skill category", 0);
	w.cutMult = lerp(level, 0.3, 2.0) * numberOr(weapon, "cut damage multiplier", 1.0)
		* manufacturerCut;
	w.bluntMult = lerp(level, 0.0, 2.0) * numberOr(weapon, "blunt damage multiplier", 0.0)
		* manufacturerBlunt;
	w.bleedMult = numberOr(weapon, "bleed mult", 1.0);
	w.pierceMult = numberOr(weapon, "pierce damage multiplier", 0.0);
	if (w.bluntMult > w.cutMult)
	{
		w.blunt1 = k.bluntDamage1;
		w.cut1 = w.blunt1 * 0.4; // a club cuts for 40% of its blunt base
	} else {
		w.cut1 = k.cutDamage1;
		w.blunt1 = 0.0;
	}
	w.humanMult = numberOr(weapon, "human damage mult", 1.0);
	w.animalMult = numberOr(weapon, "animal damage mult", 1.0);
	w.robotMult = numberOr(weapon, "robot damage mult", 1.0);
	w.armourPenetration = numberOr(weapon, "armour penetration", 0.0);
	w.length = numberOr(weapon, "length", 14.0);
	w.weight = std::max(w.bluntMult * 40.0, numberOr(weapon, "weight kg", 1.0) * manufacturerWeight);
	w.attackMod = numberOr(weapon, "attack mod", 0.0);
	w.defenceMod = numberOr(weapon, "defence mod", 0.0);
	w.indoorsMod = numberOr(weapon, "indoors mod", 0.0); // an int in the data (indoors_state.md section 2)
	return w;
}

state::FighterWeapon CombatData::makeFists(const kdata::GameData& gameData) const
{
	const kdata::Record& fists = requireRecord(gameData, "FISTS", typeIndex("WEAPON"));
	state::FighterWeapon w = makeWeapon(fists, 0.5);
	w.weaponType = 5;
	// Bare hands weigh nothing for the heaviness test (the FISTS record's blunt mult would give 40 kg and halve
	// the servant's swing speed; the logged unarmed anim_speed 0.789778 needs weight <= strength). UNSURE source.
	w.weight = 0.0;
	return w;
}

// The crossbow at item level L (dt41_A): the paired keys (`range` / `range 1`, `pierce damage min 0` / `... 1`) are the worst and best
// quality, interpolated by the level (DATA.md). UNSURE: the level of a crossbow instance (no manufacturer path is decoded), 0.5 stands in.
state::FighterRanged CombatData::makeRanged(const kdata::Record& crossbow, double level) const
{
	const auto pair = [&](const char* worst, const char* best, double fallback)
	{
		const double a = numberOr(crossbow, worst, fallback);
		return lerp(level, a, numberOr(crossbow, best, a));
	};
	state::FighterRanged r;
	r.sid = crossbow.stringID;
	r.range = pair("range", "range 1", 0.0);
	r.deviation = pair("accuracy deviation at 0 skill", "accuracy deviation at 0 skill 1", 0.0);
	r.perfectSkill = numberOr(crossbow, "accuracy perfect skill", 100.0);
	r.aimSpeed = numberOr(crossbow, "aim speed", 1.0);
	r.shotSpeed = pair("shot speed", "shot speed 1", 1.0);
	r.numShots = std::max(1, crossbow.intOr("num shots", 1));
	r.reloadMin = pair("reload time min", "reload time min 1", 1.0);
	r.reloadMax = pair("reload time max", "reload time max 1", 5.0);
	r.pierceMin = pair("pierce damage min 0", "pierce damage min 1", 0.0);
	r.pierceMax = pair("pierce damage max 0", "pierce damage max 1", 0.0);
	r.bleedMult = pair("bleed mult", "bleed mult 1", 1.0);
	r.humanMult = numberOr(crossbow, "human damage mult", 1.0);
	r.robotMult = numberOr(crossbow, "robot damage mult", 1.0);
	r.ammo = kStartAmmo;
	return r;
}

// A turret's gun (dt41_B section 3: `1404345c0` hands the shot the BUILDING's data, a GUN_DATA row): range, accuracy, aim, shot speed, the
// shots per cycle and the reload from the row (single values, no worst/best pairs). The row has no damage keys and the decode names no other
// source, so the bolt is a CROSSBOW record's (the first in the data, at the level of a carried one; UNSURE) and the turret's own numbers
// ride on it (no CROSSBOW record: no damage).
state::FighterRanged CombatData::makeTurretGun(const kdata::GameData& gameData, const kdata::Record& gunData) const
{
	state::FighterRanged r;
	for (const kdata::Record& bolt : gameData.records())
		if (bolt.type == typeIndex("CROSSBOW"))
		{
			r = makeRanged(bolt, 0.5);
			break;
		}
	r.sid = gunData.stringID;
	r.range = numberOr(gunData, "range", 0.0);
	r.deviation = numberOr(gunData, "accuracy deviation at 0 skill", 0.0);
	r.perfectSkill = numberOr(gunData, "accuracy perfect skill", 100.0);
	r.aimSpeed = numberOr(gunData, "aim speed", 1.0);
	r.shotSpeed = numberOr(gunData, "shot speed", 1.0);
	r.numShots = std::max(1, gunData.intOr("num shots", 1));
	r.reloadMin = numberOr(gunData, "reload time min", 1.0);
	r.reloadMax = numberOr(gunData, "reload time max", 5.0);
	return r;
}

state::FighterArmour CombatData::makeArmour(const kdata::Record& armour, double level) const
{
	state::FighterArmour a;
	a.name = armour.name;
	a.sid = armour.stringID;
	a.slot = armour.intOr("slot", 7);
	const std::int32_t material = std::clamp(armour.intOr("material type", 0), 0, 3);
	const std::int32_t cls = std::clamp(armour.intOr("class", 0), 0, 3);
	const double l = std::clamp(level + numberOr(armour, "level bonus", 0.0) / 100.0, 0.0, 1.0);
	a.cut = std::clamp(lerp(l, ArmourTables::kCut[material][0], ArmourTables::kCut[material][1])
		* ArmourTables::kClassCut[cls] + lerp(l, 0.0, numberOr(armour, "cut def bonus", 0.0)),
		0.0, 0.9);
	a.blunt = std::clamp(lerp(l, ArmourTables::kBlunt[material][0], ArmourTables::kBlunt[material][1])
		* ArmourTables::kClassBlunt[cls] + lerp(l, 0.0, numberOr(armour, "blunt def bonus", 0.0)),
		0.0, 0.8);
	a.pierce = lerp(l, ArmourTables::kPierce[material][0], ArmourTables::kPierce[material][1])
		* numberOr(armour, "pierce def mult", 1.0);
	// UNSURE source: the shirt layer (slot 8) with part coverage has cut and blunt x0.8 and pierce x0.5 in
	// the logged item dumps (Chain Shirt, Blackened Chain Shirt, Leather Vest: 5 pieces exact); a shirt
	// without coverage (Cloth Shirt, level forced to 1) is unscaled. Not found in ArmourData::load.
	const kdata::RefCategory* coverage = armour.findRefs("part coverage");
	if (armour.intOr("slot", 0) == 8 && coverage != nullptr && !coverage->entries.empty())
	{
		a.cut *= 0.8;
		a.blunt *= 0.8;
		a.pierce *= 0.5;
	}
	a.cutIntoStun = std::clamp(numberOr(armour, "cut into stun", 0.5), 0.0, 1.0);
	a.combatSpeedMult = numberOr(armour, "combat speed mult", 1.0);
	a.dodgeMult = numberOr(armour, "dodge mult", 1.0);
	a.damageOutputMult = numberOr(armour, "damage output mult", 1.0);
	a.attackBonus = numberOr(armour, "combat attk bonus", 0.0);
	a.defenceBonus = numberOr(armour, "combat def bonus", 0.0);
	a.unarmedBonus = numberOr(armour, "unarmed bonus", 0.0);
	// Weather protection (weather_acid_rain.md section 3): the `weather protectionN` fields are a list of affect types the piece
	// covers (their VALUE is the type, 0 = none and dropped by the loader), and `weather protection amount` its share. The
	// shipped acid gear (Dustcoat 0.8, Ashland Hat 0.5) lists type 2.
	a.weatherProtection = numberOr(armour, "weather protection amount", 0.0);
	for (const kdata::FieldInt& field : armour.ints)
	{
		if (field.key.starts_with("weather protection") && field.value >= 0 && field.value < 32)
			a.weatherTypes |= 1u << field.value;
	}
	if (const kdata::RefCategory* refs = armour.findRefs("part coverage"))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			if (entry.opaqueEntry)
				continue;
			std::string partName;
			for (const AnatomyPart& part : anatomy)
			{
				if (part.sid == entry.targetID)
					partName = part.name;
			}
			if (partName.empty())
				continue;
			a.coverage.push_back(state::FighterCoverage{partName,
				static_cast<double>(entry.v0) / 100.0});
		}
	}
	return a;
}

void CombatData::initParts(state::Fighter& fighter, const RaceCombat& race) const
{
	fighter.parts.clear();
	for (const RaceAnatomyEntry& entry : race.anatomy)
	{
		const AnatomyPart& part = anatomy[static_cast<std::size_t>(entry.part)];
		state::FighterPart p;
		p.name = part.name;
		p.type = part.type;
		p.chance = entry.chance;
		p.maxHealth = entry.health;
		p.health = entry.health;
		p.koMult = part.koMult;
		p.collapseBits = part.collapseBits;
		p.vital = part.death;
		p.collapses = part.collapses;
		p.severable = part.severance;
		p.robotic = race.isRobot;
		fighter.parts.push_back(std::move(p));
	}
	fighter.minBlood = race.minBlood;
	fighter.maxBloodRace = race.maxBlood;
}

namespace {

// One weighted list entry: a record and its relative chance.
struct Choice
{
	const kdata::Record* record = nullptr;
	double weight = 0.0;
};

// Roulette over the weights. One entry is returned without a draw (so single-entry lists do not
// shift the stream); all-zero weights pick the first.
const Choice& roulette(const std::vector<Choice>& choices, RngStream& rng)
{
	if (choices.size() == 1)
		return choices.front();
	double total = 0.0;
	for (const Choice& c : choices)
		total += c.weight;
	if (!(total > 0.0))
		return choices.front();
	const double r = rng.uniform01() * total;
	double cumulative = 0.0;
	for (const Choice& c : choices)
	{
		cumulative += c.weight;
		if (cumulative > r)
			return c;
	}
	return choices.back();
}

// Entries of a reference list that resolve to a record of `type`, weighted by v0.
std::vector<Choice> listed(const kdata::GameData& gd, const kdata::Record& owner, std::string_view category,
	std::int32_t type)
{
	std::vector<Choice> out;
	if (const kdata::RefCategory* refs = owner.findRefs(category))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			const kdata::Record* target = entry.opaqueEntry ? nullptr : gd.find(entry.targetID);
			if (target != nullptr && target->type == type)
				out.push_back({target, static_cast<double>(entry.v0)});
		}
	}
	return out;
}

const kdata::Record* firstListed(const kdata::GameData& gd, const kdata::Record& owner, std::string_view category,
	std::int32_t type)
{
	const std::vector<Choice> all = listed(gd, owner, category, type);
	return all.empty() ? nullptr : all.front().record;
}

} // namespace

std::uint8_t CombatData::rollFemale(const kdata::Record& character, const RaceCombat& race, std::uint64_t diceState)
{
	const std::int64_t chance = character.intOr("female chance", 0);
	if (chance <= 0 || !race.hasFemale || character.type == typeIndex("ANIMAL_CHARACTER"))
		return 0;
	std::uint64_t z = diceState + 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	z ^= z >> 31;
	return static_cast<std::uint8_t>(static_cast<std::int64_t>(z % 100) < chance ? 1 : 0);
}

state::Fighter CombatData::makeFighter(const kdata::GameData& gameData,
	std::string_view characterSid, std::string_view raceSid, std::uint64_t& rngState,
	std::string_view squadTemplateSid) const
{
	const kdata::Record* character = gameData.find(characterSid);
	if (character == nullptr)
		throw std::runtime_error("combat data: no CHARACTER " + std::string(characterSid));
	const kdata::Record* squad = squadTemplateSid.empty() ? nullptr : gameData.find(squadTemplateSid);
	if (!squadTemplateSid.empty() && squad == nullptr)
		throw std::runtime_error("combat data: no SQUAD_TEMPLATE " + std::string(squadTemplateSid));
	RngStream rng(rngState == 0 ? 1 : rngState);
	const std::uint64_t entryState = rngState == 0 ? 1 : rngState;
	state::Fighter f;
	f.name = character->name;
	f.animal = character->type == typeIndex("ANIMAL_CHARACTER");
	// Task BQ: an animal's `attack type` (a WeaponCategory) picks its COMBAT_TECHNIQUE family; the beak's is 20 under
	// rebirth.mod (8 in gamedata.base), matching its two `giraffe type attack` records. 0 for a human.
	f.attackType = f.animal ? character->intOr("attack type", 0) : 0;
	f.npcClass = static_cast<std::uint8_t>(std::clamp(character->intOr("NPC class", 0), 0, 9));

	// Race. The caller's, else: the squad template's `race override`, the character's own `race` list,
	// the `races` list of its faction (the CHARACTER's `faction`, else the squad template's); each is a
	// weighted list (v0). Hungry bandits list no race and take their faction's; the Shinobi Thieves'
	// list gives Scorchlander / Greenlander / Hive drones, as the Bad Teeth log shows. A faction with no
	// list (Starving Bandits) gets Greenlander: UNSURE, the log's 6 bandits are all Greenlanders.
	std::string race(raceSid);
	if (race.empty())
	{
		const std::int32_t raceType = typeIndex("RACE");
		std::vector<Choice> candidates;
		if (squad != nullptr)
			candidates = listed(gameData, *squad, "race override", raceType);
		if (candidates.empty())
			candidates = listed(gameData, *character, "race", raceType);
		if (candidates.empty())
		{
			const kdata::Record* faction = firstListed(gameData, *character, "faction", typeIndex("FACTION"));
			if (faction == nullptr && squad != nullptr)
				faction = firstListed(gameData, *squad, "faction", typeIndex("FACTION"));
			if (faction != nullptr)
				candidates = listed(gameData, *faction, "races", raceType);
		}
		race = candidates.empty() ? std::string("17-gamedata.quack") : roulette(candidates, rng).record->stringID;
	}
	const RaceCombat* raceData = this->race(race);
	if (raceData == nullptr)
		throw std::runtime_error("combat data: unknown race " + race);
	f.raceSid = raceData->sid;
	f.look.female = rollFemale(*character, *raceData, entryState);
	f.hungerRate = raceData->hungerRate;
	f.raceStrength = raceData->strength;
	f.raceDexterity = raceData->dexterity;
	initParts(f, *raceData);

	// Skills: the STATS record when linked, else the character's own numbers.
	state::FighterSkills& s = f.skills;
	const kdata::RefCategory* statsRef = character->findRefs("stats");
	const kdata::Record* stats =
		statsRef != nullptr && !statsRef->entries.empty() ? gameData.find(statsRef->entries.front().targetID)
			: nullptr;
	if (stats != nullptr)
	{
		s.strength = numberOr(*stats, "strength", 1.0);
		s.dexterity = numberOr(*stats, "dexterity", 1.0);
		s.toughness = numberOr(*stats, "toughness2", 1.0);
		s.athletics = numberOr(*stats, "athletics", 1.0);
		s.dodge = numberOr(*stats, "dodge", 1.0);
		s.attack = numberOr(*stats, "attack", 1.0);
		s.defence = numberOr(*stats, "defence", 1.0);
		s.katana = numberOr(*stats, "katana", 1.0);
		s.sabres = numberOr(*stats, "sabres", 1.0);
		s.hackers = numberOr(*stats, "hackers", 1.0);
		s.blunt = numberOr(*stats, "blunt", 1.0);
		s.heavy = numberOr(*stats, "heavy weapons", 1.0);
		s.unarmed = numberOr(*stats, "unarmed", 1.0);
		s.poles = numberOr(*stats, "poles", 1.0);
		s.medic = numberOr(*stats, "medic", 1.0);
		s.lockpicking = numberOr(*stats, "lockpicking", 1.0);
		s.stealth = numberOr(*stats, "stealth", 1.0);
		s.science = numberOr(*stats, "science", 1.0);
		s.labouring = numberOr(*stats, "labouring", 1.0);
		s.engineer = numberOr(*stats, "engineer", 1.0);
		s.crossbows = numberOr(*stats, "bow", 1.0);
		s.turrets = numberOr(*stats, "turrets", 1.0);
		s.thievery = numberOr(*stats, "thievery", 1.0);
	} else {
		const double spread = character->intOr("stats randomise", 3);
		auto jitter = [&](double base) { return std::max(1.0, base + (rng.uniform01() * 2.0 - 1.0) * spread); };
		const double combat = numberOr(*character, "combat stats", 1.0);
		const double unarmed = numberOr(*character, "unarmed stats", 1.0);
		s.attack = jitter(combat);
		s.defence = jitter(combat);
		s.katana = s.sabres = s.hackers = s.blunt = s.heavy = s.poles = jitter(combat);
		s.unarmed = jitter(unarmed);
		s.dodge = jitter(unarmed);
		// `strength` 0 means unset, not strength 0: the exe sets these ints only `if > 0` (FIRST_KO_STATIC.md (c) continued) and the
		// `combat stats` group it falls back to carries strength. CHARACTER 510 (Dust bandit) lists `strength` 0 with `combat stats` 10
		// and `stats randomise` 3, and the exe's bandits carry 50-92% of their load (kinspect crowd_melee: strength ~7-13). Reading the 0
		// literally made every spawned bandit a strength-1 weakling at 150-170% load.
		const double strength = numberOr(*character, "strength", combat);
		s.strength = jitter(strength > 0.0 ? strength : combat);
		s.dexterity = jitter(combat);
		s.toughness = jitter(combat);
		s.medic = 1.0; // UNSURE: the data gives a record without STATS no First Aid number
		s.stealth = std::max(1.0, numberOr(*character, "stealth stats", 1.0)); // `stealth stats`: the sneak skill without a STATS record (no jitter: it must not draw from the rng)
	}

	// Weapon (Bad Teeth and e1 logs): the first `weapons` entry with quantity (v0) >= 1 is carried; the
	// quantity-0 entries are not (every Hungry bandit has just the Iron Club, the Paladins just the
	// Paladin's Cross). Its quality: a WEAPON_MANUFACTURER from the `weapon level` list (weight v0), then one
	// of the manufacturer's `weapon models` (weight v1) whose v0 is the item level in percent and whose
	// MATERIAL_SPECS_WEAPON gives the attack / defence mod. Matches the logged items: Unknown (Rusted Junk
	// level 5, mods -1 / -1; Rusting Blade level 10) clubs at 0.05 / 0.1, Ancient (cut x0.9, weight x0.7;
	// levels 15 / 20 / 25) crosses and cleavers. UNSURE: the quantity-0 weapon entries (their v1 100 may be
	// a chance) and the manufacturer's `weapon types` filter are not applied.
	f.weapon = makeFists(gameData);
	const kdata::Record* carried = nullptr;
	// A carried crossbow (a CROSSBOW record) is the ranged weapon; the melee one is picked below as before. The vanilla templates list
	// it under the `crossbows` reference category (40 of them; none under `weapons`, kenshi-k16.6.24) with `v1` a percent chance: Sniperbot
	// and the Dust Bandit Bowman 100, an Escaped Servant 20. `weapons` is read too, first, as before (no chance there).
	// UNSURE: v1 as a per-spawn percent (a 0 never arms: Sir Shootalot's three entries are all 0); the first entry that passes wins.
	if (const kdata::RefCategory* refs = f.animal ? nullptr : character->findRefs("weapons"))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			const kdata::Record* bow = entry.opaqueEntry || entry.v0 < 1 ? nullptr : gameData.find(entry.targetID);
			if (bow != nullptr && bow->type == typeIndex("CROSSBOW"))
			{
				f.ranged = makeRanged(*bow, 0.5);
				break;
			}
		}
	}
	if (const kdata::RefCategory* refs = f.animal || f.ranged.ammo > 0 ? nullptr : character->findRefs("crossbows"))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			const kdata::Record* bow = entry.opaqueEntry || entry.v0 < 1 ? nullptr : gameData.find(entry.targetID);
			if (bow == nullptr || bow->type != typeIndex("CROSSBOW") || entry.v1 <= 0)
				continue;
			if (entry.v1 < 100 && rng.uniform01() * 100.0 >= entry.v1)
				continue;
			f.ranged = makeRanged(*bow, 0.5);
			break;
		}
	}
	// Task BQ: an ANIMAL_CHARACTER lists its natural weapon under `weapon` (singular; the beak's `giraffe type`, length 65,
	// skill category 20), while a CHARACTER lists carried items under `weapons` a quantity (v0) at a time.
	if (const kdata::RefCategory* refs = character->findRefs(f.animal ? "weapon" : "weapons"))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			const kdata::Record* weapon = entry.opaqueEntry ? nullptr : gameData.find(entry.targetID);
			if (weapon != nullptr && weapon->type == typeIndex("WEAPON") && (f.animal || entry.v0 >= 1))
			{
				carried = weapon;
				break;
			}
		}
	}
	if (carried != nullptr)
	{
		std::vector<Choice> makers = listed(gameData, *character, "weapon level", typeIndex("WEAPON_MANUFACTURER"));
		if (makers.empty())
		{
			// UNSURE: a record with a weapon and no `weapon level` (e.g. Holy Armour Trader): the "Unknown" manufacturer.
			for (const kdata::Record* m : gameData.byType(typeIndex("WEAPON_MANUFACTURER")))
			{
				if (m->name == "Unknown")
					makers.push_back({m, 1.0});
			}
			if (makers.empty())
				throw std::runtime_error("combat data: " + character->name + " carries " + carried->name
					+ " but lists no `weapon level` and there is no Unknown manufacturer");
		}
		const kdata::Record& maker = *roulette(makers, rng).record;
		std::vector<Choice> models;
		if (const kdata::RefCategory* refs = maker.findRefs("weapon models"))
		{
			for (const kdata::RefEntry& entry : refs->entries)
			{
				const kdata::Record* spec = entry.opaqueEntry ? nullptr : gameData.find(entry.targetID);
				if (spec != nullptr)
					models.push_back({spec, static_cast<double>(entry.v1)});
			}
		}
		if (models.empty())
			throw std::runtime_error("combat data: manufacturer " + maker.name + " lists no weapon models");
		const Choice& model = roulette(models, rng);
		std::int32_t levelPercent = 0;
		for (const kdata::RefEntry& entry : maker.findRefs("weapon models")->entries)
		{
			if (entry.targetID == model.record->stringID)
				levelPercent = entry.v0;
		}
		state::ItemQuality quality;
		quality.maker = maker.stringID;
		quality.model = model.record->stringID;
		quality.level = levelPercent / 100.0;
		quality.grade = state::ItemQuality::gradeOf(quality.level);
		f.weapon = buildWeapon(gameData, *carried, quality);
	}

	// Armour (Bad Teeth log): per slot one piece among the `clothing` entries with quantity (v0) >= 1,
	// weighted by v1; a piece with part coverage has level 0.2 x (`armour grade` + 1 with probability
	// `armour upgrade chance` percent), one without has level 1 (every logged Paladin: grade 1 gives 0.2
	// or 0.4; the High Paladin, grade 2, 0.4 and one 0.6). Pieces are listed by ascending slot.
	std::map<std::int32_t, std::vector<Choice>> bySlot;
	if (const kdata::RefCategory* refs = character->findRefs("clothing"))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			const kdata::Record* armour = entry.opaqueEntry ? nullptr : gameData.find(entry.targetID);
			if (armour != nullptr && armour->type == typeIndex("ARMOUR") && entry.v0 >= 1)
				bySlot[armour->intOr("slot", 0)].push_back({armour, static_cast<double>(entry.v1)});
		}
	}
	const double grade = character->intOr("armour grade", 1);
	const double upgrade = character->intOr("armour upgrade chance", 0);
	// `wears uniform`: the clothes are branded as the faction's (the CHARACTER's `faction`, else the squad template's). UNSURE: the exe's
	// spawner that sets the uniform pointer was not traced (s72 section 7); a character with no faction gets no mark.
	std::string uniform;
	if (character->boolOr("wears uniform", false))
	{
		const kdata::Record* faction = firstListed(gameData, *character, "faction", typeIndex("FACTION"));
		if (faction == nullptr && squad != nullptr)
			faction = firstListed(gameData, *squad, "faction", typeIndex("FACTION"));
		if (faction != nullptr)
			uniform = faction->stringID;
	}
	for (const auto& [slot, choices] : bySlot)
	{
		(void) slot;
		const kdata::Record& armour = *roulette(choices, rng).record;
		const kdata::RefCategory* coverage = armour.findRefs("part coverage");
		double level = 1.0;
		if (coverage != nullptr && !coverage->entries.empty())
		{
			const double upgraded = rng.uniform01() * 100.0 < upgrade ? 1.0 : 0.0;
			level = std::clamp(0.2 * (grade + upgraded), 0.0, 1.0);
		}
		state::ItemQuality quality;
		quality.level = level;
		quality.grade = coverage != nullptr && !coverage->entries.empty() ? state::ItemQuality::gradeOf(level) : state::ItemQuality::kNoGrade;
		quality.uniform = uniform;
		f.armour.push_back(buildArmour(armour, quality));
	}
	f.blood = f.minBlood + (f.maxBloodRace - f.minBlood) * s.strength * 0.01;
	rngState = rng.state();
	return f;
}

state::FighterWeapon CombatData::buildWeapon(const kdata::GameData& gameData, const kdata::Record& weapon,
	const state::ItemQuality& quality) const
{
	if (!quality.set())
		throw std::logic_error("combat data: weapon " + weapon.name + " has no quality to build from");
	double cut = 1.0;
	double blunt = 1.0;
	double weight = 1.0;
	if (!quality.maker.empty())
	{
		const kdata::Record* maker = gameData.find(quality.maker);
		if (maker == nullptr)
			throw std::runtime_error("combat data: unknown weapon manufacturer " + quality.maker);
		cut = numberOr(*maker, "cut damage mod", 1.0);
		blunt = numberOr(*maker, "blunt damage mod", 1.0);
		weight = numberOr(*maker, "weight mod", 1.0);
	}
	state::FighterWeapon w = makeWeapon(weapon, quality.level, cut, blunt, weight);
	if (!quality.model.empty())
	{
		const kdata::Record* model = gameData.find(quality.model);
		if (model == nullptr)
			throw std::runtime_error("combat data: unknown weapon model " + quality.model);
		w.attackMod += model->intOr("attack mod", 0);
		w.defenceMod += model->intOr("defence mod", 0);
	}
	w.quality = quality;
	return w;
}

state::FighterArmour CombatData::buildArmour(const kdata::Record& armour, const state::ItemQuality& quality) const
{
	if (!quality.set())
		throw std::logic_error("combat data: armour " + armour.name + " has no quality to build from");
	state::FighterArmour a = makeArmour(armour, quality.level);
	a.quality = quality;
	return a;
}

state::ItemQuality CombatData::rollQuality(const kdata::GameData& gameData, const kdata::Record& item, double rollGrade,
	double rollPick, const double* gradeWeights, const std::vector<std::string>* makers)
{
	// A grade 0..5 by the generic faction `armors` weights (50 / 100 / 50 / 2 / 0 / 0), its quality 5 / 20 / 40 / 60 / 80 / 95.
	static constexpr double kGeneric[6] = {50.0, 100.0, 50.0, 2.0, 0.0, 0.0};
	const double* kWeights = gradeWeights != nullptr ? gradeWeights : kGeneric;
	double weightTotal = 0.0;
	for (int g = 0; g < 6; ++g)
		weightTotal += kWeights[g];
	static constexpr double kQualityPercent[6] = {5.0, 20.0, 40.0, 60.0, 80.0, 95.0};
	double roll = rollGrade * weightTotal; // 202 for the generic weights
	std::size_t grade = 0;
	while (grade < 5 && roll >= kWeights[grade])
		roll -= kWeights[grade++];
	state::ItemQuality q;
	if (item.type == typeIndex("ARMOUR"))
	{
		const kdata::RefCategory* coverage = item.findRefs("part coverage");
		const bool covered = coverage != nullptr && !coverage->entries.empty();
		q.level = covered ? kQualityPercent[grade] / 100.0 : 1.0;
		q.grade = covered ? static_cast<std::uint8_t>(grade) : state::ItemQuality::kNoGrade;
	} else if (item.type == typeIndex("WEAPON")) {
		// The manufacturers that list this weapon in `weapon types`, and the model of each whose level is nearest the quality.
		struct Pick
		{
			const kdata::Record* maker;
			const kdata::Record* model;
			double level;
		};
		std::vector<Pick> best;
		double distance = 1e9;
		// `narrow`: only the makers of a merchant's list (CombatData::rollQuality's `makers`).
		const auto collect = [&](bool narrow)
		{
			for (const kdata::Record* maker : gameData.byType(typeIndex("WEAPON_MANUFACTURER")))
			{
				if (narrow && std::find(makers->begin(), makers->end(), maker->stringID) == makers->end())
					continue;
				const kdata::RefCategory* types = maker->findRefs("weapon types");
				const kdata::RefCategory* models = maker->findRefs("weapon models");
				if (types == nullptr || models == nullptr)
					continue;
				bool lists = false;
				for (const kdata::RefEntry& entry : types->entries)
					lists = lists || (!entry.opaqueEntry && entry.targetID == item.stringID);
				if (!lists)
					continue;
				for (const kdata::RefEntry& entry : models->entries)
				{
					const kdata::Record* spec = entry.opaqueEntry ? nullptr : gameData.find(entry.targetID);
					if (spec == nullptr)
						continue;
					const double d = std::fabs(static_cast<double>(entry.v0) - kQualityPercent[grade]);
					if (d < distance)
					{
						distance = d;
						best.clear();
					}
					if (d == distance)
						best.push_back({maker, spec, entry.v0 / 100.0});
				}
			}
		};
		if (makers != nullptr)
			collect(true);
		if (best.empty())
			collect(false);
		if (best.empty())
		{
			// Content, not a code bug (a mod's weapon no manufacturer lists): the item is stocked with the grade's plain level and no
			// maker or model (its multipliers are 1), reported once per record so the broken data stays visible.
			static std::mutex reportedLock;
			static std::set<std::string> reported;
			bool first;
			{
				std::lock_guard<std::mutex> guard(reportedLock);
				first = reported.insert(item.stringID).second;
			}
			if (first)
				std::cerr << "combat data: no weapon manufacturer lists " << item.name << " (" << item.stringID << "); stocked with a plain grade "
						  << grade << " quality, no maker\n";
			q.level = kQualityPercent[grade] / 100.0;
			q.grade = static_cast<std::uint8_t>(grade);
			return q;
		}
		const Pick& chosen = best[std::min(static_cast<std::size_t>(rollPick * static_cast<double>(best.size())), best.size() - 1)];
		q.maker = chosen.maker->stringID;
		q.model = chosen.model->stringID;
		q.level = chosen.level;
		q.grade = state::ItemQuality::gradeOf(q.level);
	}
	return q;
}

} // namespace ksim
