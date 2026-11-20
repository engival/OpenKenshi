#include "dev/animlab.hpp"
#include "render/drawstats.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "characters/characters.hpp"
#include "render/drawlist_gpu.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"	// the lazy init's merged data load
#include "assets/resources.hpp"
#include "app/sappcmd.hpp"
#include "sokol_app.h"
#include "render/readback.hpp"

namespace
{
	constexpr double kFrameRate = 30.0;	// the original's clip frames (docs/formats/OGRE_SKELETON.md: `anim hesitate point` is in 30 fps frames)
	constexpr int kMeasureSamples = 96;	// the measure's samples per cycle: deriveContacts' (characters.cpp kContactSamples), so windows and travel compare to `dump contacts`
	constexpr float kContactHeight = 0.4f;	// the sole contact height (characters.cpp kContactHeight, task L4)
	constexpr int kGrid = 20;	// the floor grid's half extent in units
	constexpr float kPi = 3.14159265f;
	// The clip list's groups (task lab slice 3), in list order: a clip in several is listed in each. Every rule below reads the merged data's keys
	// (docs/reference/combat/DATA.md 2.8 / 2.10 / 2.11); the vocabulary is the UI's dropdown order.
	const char* const kGroupNames[] = { "Locomotion", "Idle + guard stances", "Attacks", "Defence", "Hit reactions", "Down/recovery",
		"Carry", "Ranged", "Swimming", "Work/actions", "Animals", "Unreferenced" };
	enum Group { kLoco, kIdle, kAttack, kDefence, kHit, kDown, kCarry, kRanged, kSwim, kWork, kAnimals, kUnreferenced, kGroupCount };
	static_assert(std::size(kGroupNames) == kGroupCount);
	constexpr float kRootFollowTravel = 5.0f;	// a raw clip whose Bip01 path passes this per loop follows its root by default (the coordinator's amendment)

	std::string folded(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}
	bool startsWith(const std::string& s, const char* prefix)
	{
		const size_t n = std::strlen(prefix);
		return s.size() >= n && std::equal(prefix, prefix + n, s.begin(), [](char a, char b) { return a == b; });
	}
	std::string joined(const std::vector<std::string>& v, const char* sep = ", ")
	{
		std::string s;
		for (const std::string& x : v) { s += (s.empty() ? "" : sep) + x; }
		return s;
	}

	// The portrait scene's fixed light (portraits.cpp init): deferred.material as rtt.hlsl reads it; the lab reuses it (coordinator 2026-11-07).
	LightParams portraitLight()
	{
		LightParams l = {};
		const float n = std::sqrt(6.0f);
		l.v[0][0] = 1 / n;
		l.v[0][1] = 1 / n;
		l.v[0][2] = 2 / n;
		for (int k = 0; k < 3; k++) { l.v[1][k] = 0.7f; l.v[7][k] = 1; }
		l.v[1][3] = 1;
		l.v[2][3] = 1;
		l.v[3][3] = 1;
		l.v[4][3] = 0.5f;
		l.v[5][0] = 1e9f;
		l.v[5][1] = 2e9f;
		return l;
	}

	void project(const float mvp[16], const double cam[3], const double p[3], int w, int h, float& sx, float& sy, float& depth)
	{
		const float q[3] = { (float)(p[0] - cam[0]), (float)(p[1] - cam[1]), (float)(p[2] - cam[2]) };
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * q[0] + mvp[4 + i] * q[1] + mvp[8 + i] * q[2] + mvp[12 + i]; }
		if (c[3] <= 1e-6f) { return; }
		sx = (c[0] / c[3] * 0.5f + 0.5f) * w;
		sy = (0.5f - c[1] / c[3] * 0.5f) * h;
		depth = c[3];
	}
}

animlab::Lab::Lab() : chars_(std::make_unique<Characters>()) {}

animlab::Lab::~Lab() { shutdown(); }

void animlab::Lab::setup(const ResourceIndex* res, const std::string& dataDir)
{
	res_ = res;
	dataDir_ = dataDir;
}

// Lazy init (coordinator 2026-11-07): a game start used to pay for the lab's own Characters set, render target and character; they are built on
// the first open or the first `lab` op. The merged data load is the lab's own: Characters keeps only what it copies out of the call.
bool animlab::Lab::ensure()
{
	if (ready_) { return true; }
	if (!res_ || dataDir_.empty()) { st_.reply = "the anim lab is not set up"; return false; }
	const kdata::GameData gd = kworld::loadMergedGameData(dataDir_);
	std::string err;
	if (!init(gd, err)) { fprintf(stderr, "kclient: anim lab: %s\n", err.c_str()); st_.reply = err; return false; }
	return true;
}

bool animlab::Lab::init(const kdata::GameData& gd, std::string& err)
{
	if (ready_) { return true; }
	chars_->init(res_, gd, false);	// its GPU half is a frame-lane job: this runs on the logic thread (a `lab` op, the browser's open)
	index(gd);	// the groups' and the root lock's figures: the lab keeps what it copies out, not the load
	st_.groups.assign(std::begin(kGroupNames), std::end(kGroupNames));
	st_.races = chars_->races(false);	// every RACE a body can be built for, animals included (coordinator 2026-11-07)
	if (st_.races.empty()) { err = "no RACE with a body mesh"; return false; }
	st_.race = std::find(st_.races.begin(), st_.races.end(), std::string("Greenlander")) != st_.races.end() ? "Greenlander" : st_.races.front();
	light_ = portraitLight();
	ready_ = true;
	std::string why;
	if (!target(480, 480, why)) { err = why; ready_ = false; return false; }
	build();
	return true;
}

void animlab::Lab::touch()
{
	touched_ = true;
	ensure();
}

void animlab::Lab::shutdown()
{
	if (!ready_ && !tgtQueued_) { return; }	// cleanup() calls this; the App's destructor calls it again after sg_shutdown
	dropTarget();
	if (chars_) { chars_->shutdown(); chars_->clear(); }
	ready_ = false;
}

animlab::Actions animlab::Lab::actions()
{
	Actions a;
	a.select = [this](const std::string& n) { select(n); };
	a.play = [this](bool on) { play(on); };
	a.step = [this](int d) { step(d); };
	a.seek = [this](double t) { seek(t); };
	a.speed = [this](double x) { speed(x); };
	a.camera = [this](const std::string& n) { camera(n); };
	a.orbit = [this](double dx, double dy) { orbit(dx, dy); };
	a.zoom = [this](double w) { zoom(w); };
	a.plant = [this](const std::string& p) { plant(p); };
	a.overlay = [this](const std::string& w, bool on) { overlay(w, on); };
	a.measure = [this]() { measure(); };
	a.mark = [this]() { mark(); };
	a.copy = [](const std::string& text) { sappcmd::setClipboard(text); };
	a.viewSize = [this](int w, int h) { viewSize(w, h); };
	a.race = [this](const std::string& n) { race(n); };
	a.female = [this](bool on) { female(on); };
	a.close = [this]() { close(); };
	a.group = [this](const std::string& n) { group(n); };
	a.follow = [this](bool on) { follow(on); };
	return a;
}

void animlab::Lab::open(bool on)
{
	touched_ = true;
	if (on && !ensure()) { st_.open = false; return; }
	st_.open = on;
}

// The figures the clip groups and the root lock read: ANIMATION (docs/reference/combat/DATA.md 2.10), COMBAT_TECHNIQUE (2.8), LOCATIONAL_DAMAGE's
// pain anims and the CROSSBOW reload anims, and ANIMAL_CHARACTER's clips through ANIMAL_ANIMATION (2.11). Copied out of the load, as Characters does.
void animlab::Lab::index(const kdata::GameData& gd)
{
	recs_.clear();
	techs_.clear();
	pains_.clear();
	reloads_.clear();
	animals_.clear();
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ANIMATION")))
	{
		Rec a;
		a.name = r->name;
		a.clip = r->stringOr("anim name", r->name);
		const std::string layer = folded(r->stringOr("layer", "all"));
		a.layer = layer == "lower" ? 0 : layer == "overlay" ? 2 : layer == "tail" ? 3 : layer == "ears" ? 4 : 1;
		a.category = r->intOr("category", 0);
		a.combat = r->intOr("is combat mode", 2);
		a.handL = r->intOr("has weapon L", 2);
		a.handR = r->intOr("has weapon R", 2);
		a.idle = r->boolOr("idle", false);
		a.action = r->boolOr("is action", false);
		a.prone = r->boolOr("prone", false);
		a.relocates = r->boolOr("relocates", false);
		a.big = r->boolOr("big stumble", false);
		a.stumbleFrom = r->intOr("stumble from", 0);
		a.carry = (r->boolOr("being carried", false) ? 1u : 0u) | (r->boolOr("carrying left", false) ? 2u : 0u) | (r->boolOr("carrying right", false) ? 4u : 0u);
		a.move = r->floatOr("move speed", 0.0f);
		a.min = r->floatOr("min speed", 0.0f);
		a.max = r->floatOr("max speed", 0.0f);
		a.strafe = r->floatOr("strafe speed max", 0.0f);
		if (const kdata::RefCategory* refs = r->findRefs("stumbles"))
		{
			for (const kdata::RefEntry& e : refs->entries) { if (const kdata::Record* t = gd.find(e.targetID)) { a.stumbles.push_back(t->name); } }
		}
		recs_[folded(r->name)] = std::move(a);
	}
	// COMBAT_TECHNIQUE: the attacks (by weapon set, or by animal type) and the blocks / dodges. `animal` 0 is the human weapon set and 5 the martial
	// arts; `is block` with `is dodge` is a dodge, a block alone is a pure block (its clip's root is deleted).
	static const char* const kWeapons[] = { "katanas", "sabre", "hackers", "heavy weapons", "blunt", "polearm", "unarmed" };
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("COMBAT_TECHNIQUE")))
	{
		Tech t;
		t.clip = r->stringOr("anim name", r->name);
		std::string w;
		for (const char* k : kWeapons) { if (r->boolOr(k, false)) { w += (w.empty() ? "" : ", ") + std::string(k); } }
		const int animal = r->intOr("animal", 0);
		t.by = "technique " + r->name;
		if (animal > 0 && animal != 5) { t.by += " (animal " + std::to_string(animal) + ")"; }
		else { t.by += w.empty() ? " (no weapon flags)" : " (" + w + ")"; }
		if (r->boolOr("disabled", false)) { t.by += " [disabled]"; }
		t.defence = r->boolOr("is block", false) || r->boolOr("is dodge", false);
		t.pureBlock = r->boolOr("is block", false) && !r->boolOr("is dodge", false);
		techs_.push_back(std::move(t));
	}
	// LOCATIONAL_DAMAGE `pain anim`: the overlay clip a wounded part plays (the ref's v0 is the health threshold, 0..1)
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("LOCATIONAL_DAMAGE")))
	{
		const kdata::RefCategory* refs = r->findRefs("pain anim");
		const kdata::Record* a = refs && !refs->entries.empty() ? gd.find(refs->entries[0].targetID) : nullptr;
		if (!a) { continue; }
		std::string& s = pains_[folded(a->name)];
		if (!s.empty()) { s += "; "; }
		s += "pain overlay: " + r->name + " (" + std::to_string(refs->entries[0].v0) + "%)";
	}
	// CROSSBOW `reload anim`: the ANIMATION records a crossbow's reload plays, by weapon
	std::unordered_map<std::string, std::vector<std::string>> reloadWeapons;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CROSSBOW")))
	{
		const kdata::RefCategory* refs = r->findRefs("reload anim");
		if (!refs) { continue; }
		for (const kdata::RefEntry& e : refs->entries)
		{
			const kdata::Record* a = gd.find(e.targetID);
			if (!a) { continue; }
			std::vector<std::string>& w = reloadWeapons[folded(a->name)];
			if (std::find(w.begin(), w.end(), r->name) == w.end()) { w.push_back(r->name); }
		}
	}
	for (const auto& [name, weapons] : reloadWeapons) { reloads_[name] = "CROSSBOW reload anim (" + joined(weapons) + ")"; }
	// ANIMAL_CHARACTER's clips, by species: its `animations` / `strafe anim` / `turning anim` point at ANIMAL_ANIMATION records, whose `anim name`
	// is the clip in that animal's skeleton
	std::map<std::string, std::map<std::string, std::vector<std::string>>> animalUse;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ANIMAL_CHARACTER")))
	{
		auto add = [&](const char* key, const char* kind)
		{
			const kdata::RefCategory* refs = r->findRefs(key);
			if (!refs) { return; }
			for (const kdata::RefEntry& e : refs->entries)
			{
				const kdata::Record* a = gd.find(e.targetID);
				if (!a) { continue; }
				std::vector<std::string>& kinds = animalUse[folded(a->stringOr("anim name", a->name))][r->name];
				if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) { kinds.push_back(kind); }
			}
		};
		add("animations", "animations");
		add("strafe anim", "strafe");
		add("turning anim", "turning");
	}
	for (const auto& [clip, species] : animalUse)
	{
		std::string s;
		for (const auto& [name, kinds] : species) { s += (s.empty() ? "" : "; ") + std::string("ANIMAL_CHARACTER ") + name + " (" + joined(kinds) + ")"; }
		animals_[clip] = s;
	}
}

void animlab::Lab::build()
{
	if (!ready_ || !chars_) { return; }
	chars_->clear();
	Characters::Spec s;
	s.race = st_.race;
	s.female = st_.female;
	s.move = -1;
	s.speed = 0;	// the lab sets the layer times itself (setClipTime); update() must not advance them
	std::string err;
	if (!chars_->add(s, err))
	{
		// an animal's skeleton has no `idle_stand_normal`: build it on the bind pose, then the clip list's own default (the first clip) is selected
		s.clip.clear();
		err.clear();
		if (!chars_->add(s, err))
		{
			st_.reply = "cannot build " + st_.race + ": " + err;
			printf("lab: %s\n", st_.reply.c_str());
			return;
		}
	}
	chars_->update(0);
	const kworld::Skeleton* sk = chars_->skeleton(0);
	st_.clips.clear();
	st_.bones.clear();
	if (!sk) { st_.reply = "no skeleton for " + st_.race; return; }
	for (const kworld::SkeletonAnimation& a : sk->animations)
	{
		Clip c;
		c.name = a.name;
		c.kind = "raw";
		c.clip = a.name;
		c.length = a.length;
		c.frames = std::max(1, (int)std::lround(a.length * kFrameRate));
		st_.clips.push_back(std::move(c));
	}
	for (const auto& [name, clip] : chars_->animationRecords())
	{
		const kworld::SkeletonAnimation* a = sk->animation(clip);
		if (!a) { continue; }
		Clip c;
		c.name = name;
		c.kind = "record";
		c.clip = clip;
		c.length = a->length;
		c.frames = std::max(1, (int)std::lround(a->length * kFrameRate));
		// a raw clip of the same name is the same animation: the record entry wins (the UI shows the record's kind)
		auto same = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& x) { return x.name == name && x.kind == "raw"; });
		if (same != st_.clips.end()) { same->kind = "record"; same->clip = clip; same->length = c.length; same->frames = c.frames; }
		else { st_.clips.push_back(std::move(c)); }
	}
	for (const kworld::SkeletonBone& b : sk->bones) { st_.bones.push_back(b.name); }
	classify();
	const auto found = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == st_.clip; });
	if (found == st_.clips.end())
	{
		const auto idle = std::find_if(st_.clips.begin(), st_.clips.end(), [](const Clip& c) { return c.name == "idle_stand_normal" && c.kind == "raw"; });
		st_.clip = idle != st_.clips.end() ? idle->name : st_.clips.empty() ? "" : st_.clips.front().name;
	}
	keep_.clear();
	plantClip_ = std::string::npos;
	printf("lab: %s %s: %zu clips, %zu raw, %zu records; default \"%s\"\n", st_.race.c_str(), st_.female ? "female" : "male", st_.clips.size(),
		std::count_if(st_.clips.begin(), st_.clips.end(), [](const Clip& c) { return c.kind == "raw"; }),
		std::count_if(st_.clips.begin(), st_.clips.end(), [](const Clip& c) { return c.kind == "record"; }), st_.clip.c_str());
	if (st_.clip.empty())
	{
		st_.reply = "no clips";
		printf("lab: %s\n", st_.reply.c_str());
		return;
	}
	select(st_.clip);
}

// Every clip's groups and "used by" line, and whether its root lock is the game's own rule. A clip answers to its own name (a record entry) and to the
// clips of the records that name it (a raw entry: the animation of that record); a record several rules match is listed in each group.
void animlab::Lab::classify()
{
	for (Clip& c : st_.clips)
	{
		c.groups.clear();
		c.usedBy.clear();
		c.relocates = false;
		c.autoFollow = false;
		const std::string key = folded(c.name);
		std::vector<const Rec*> recs;
		if (const auto it = recs_.find(key); it != recs_.end()) { recs.push_back(&it->second); }
		for (const auto& [name, r] : recs_)
		{
			if (&r != (recs.empty() ? nullptr : recs.front()) && folded(r.clip) == key) { recs.push_back(&r); }
		}
		std::vector<std::string> clips{ key };
		for (const Rec* r : recs) { const std::string k = folded(r->clip); if (std::find(clips.begin(), clips.end(), k) == clips.end()) { clips.push_back(k); } }
		struct Tag { int group; std::string why; };
		std::vector<Tag> tags;
		auto tag = [&](int g, const std::string& why) { tags.push_back({ g, why }); };
		for (const Rec* r : recs)
		{
			c.relocates = c.relocates || r->relocates;
			const std::string rec = "ANIMATION record " + r->name;
			if (r->move != 0 || (r->layer == 0 && r->strafe > 0))	// the moving records' speed bands, `strafe lower` alone with no band
			{
				char b[192];
				if (r->move != 0) { snprintf(b, sizeof b, "%s (move %.0f u/s, min %.0f, max %.0f)", rec.c_str(), r->move, r->min, r->max); }
				else { snprintf(b, sizeof b, "%s (strafe %.0f u/s)", rec.c_str(), r->strafe); }
				tag(kLoco, b);
			}
			if (r->idle)
			{
				std::string s = "idle";
				if (r->combat == 1) { s += ", combat"; }
				if (r->handL == 1) { s += ", weapon L"; }
				if (r->handR == 1) { s += ", weapon R"; }
				if (r->carry & 1) { s += ", being carried"; }
				if (r->carry & 2) { s += ", carrying left"; }
				if (r->carry & 4) { s += ", carrying right"; }
				tag(kIdle, rec + " (" + s + ")");
			}
			if (!r->stumbles.empty())
			{
				static const char* const kFrom[] = { "front", "rear", "left", "right" };	// CutOrigination
				tag(kHit, "stumble: " + joined(r->stumbles) + " (" + kFrom[std::clamp(r->stumbleFrom, 0, 3)] + (r->big ? ", big" : "") + ")");
			}
			if (const auto p = pains_.find(folded(r->name)); p != pains_.end()) { tag(kHit, p->second); }
			if (r->prone || r->category == 5 || startsWith(folded(r->name), "knockout") || startsWith(folded(r->name), "standing up"))
			{
				std::string s;
				if (r->prone) { s = "prone"; }
				if (r->category == 5) { s += (s.empty() ? "" : ", ") + std::string("category GROUNDED"); }
				if (startsWith(folded(r->name), "knockout")) { s += (s.empty() ? "" : ", ") + std::string("knockout"); }
				if (startsWith(folded(r->name), "standing up")) { s += (s.empty() ? "" : ", ") + std::string("get-up"); }
				tag(kDown, rec + " (" + s + ")");
			}
			if (r->carry || r->category == 3)
			{
				std::string s;
				if (r->carry & 1) { s = "being carried"; }
				if (r->carry & 2) { s += (s.empty() ? "" : ", ") + std::string("carrying left"); }
				if (r->carry & 4) { s += (s.empty() ? "" : ", ") + std::string("carrying right"); }
				if (r->category == 3) { s += (s.empty() ? "" : ", ") + std::string("category CARRIED"); }
				tag(kCarry, rec + " (" + s + ")");
			}
			if (r->category == 8) { tag(kRanged, rec + " (category RANGED)"); }
			if (r->category == 4) { tag(kSwim, rec + " (category SWIMMING)"); }
			if (r->action) { tag(kWork, rec + " (is action)"); }
			if (const auto l = reloads_.find(folded(r->name)); l != reloads_.end()) { tag(kRanged, l->second); }
		}
		bool tech = false, pureOnly = true;
		for (const Tech& t : techs_)
		{
			if (std::find(clips.begin(), clips.end(), folded(t.clip)) == clips.end()) { continue; }
			tech = true;
			pureOnly = pureOnly && t.pureBlock;
			tag(t.defence ? kDefence : kAttack, t.by);
		}
		// the animal clips are the clip the entry plays (a record's own animation is not the animal clip of the same record name)
		if (const auto a = animals_.find(folded(c.clip)); a != animals_.end()) { tag(kAnimals, a->second); }
		// the root lock the game itself makes: a relocates record moves the entity, and an action's (a technique's) root is the sim's position; a
		// pure block's clip has no root to carry
		c.autoFollow = c.relocates || (tech && !pureOnly);
		std::stable_sort(tags.begin(), tags.end(), [](const Tag& a, const Tag& b) { return a.group < b.group; });
		for (const Tag& t : tags)
		{
			const std::string& g = kGroupNames[t.group];
			if (c.groups.empty() || c.groups.back() != g) { c.groups.push_back(g); }
			c.usedBy += (c.usedBy.empty() ? "" : "; ") + t.why;
		}
		if (c.groups.empty() && c.kind == "raw")
		{
			c.groups.push_back(kGroupNames[kUnreferenced]);
			c.usedBy = "no ANIMATION record or technique names it";
		}
	}
}

void animlab::Lab::select(const std::string& name)
{
	if (!ready_ || !chars_) { return; }
	auto it = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == name; });
	if (it == st_.clips.end())
	{
		// the locomotion records split the body: `walk` is `walk lower` + `walk upper` (the same for jog / run); the legs are the lower one
		it = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == name + " lower"; });
		if (it != st_.clips.end()) { printf("lab: \"%s\" is \"%s lower\" (the locomotion records split the body; the legs are the lower one)\n", name.c_str(), name.c_str()); }
	}
	if (it == st_.clips.end()) { st_.reply = "no clip named " + name; printf("lab: %s\n", st_.reply.c_str()); return; }
	Characters::Spec s;
	s.race = st_.race;
	s.female = st_.female;
	s.move = -1;
	s.speed = 0;
	if (it->kind == "record") { s.anims = { it->name }; }
	else { s.clip = it->name; }
	chars_->clear();
	std::string err;
	if (!chars_->add(s, err)) { st_.reply = "cannot play " + name + ": " + err; printf("lab: %s\n", st_.reply.c_str()); return; }
	chars_->update(0);
	st_.clip = it->name;
	st_.frames = it->frames;
	st_.time = 0;
	st_.frame = 0;
	time_ = 0;
	std::fill(std::begin(shift_), std::end(shift_), 0.0);
	for (auto& foot : trail_) { for (auto& sole : foot) { sole.clear(); } }
	rootPath_.clear();
	keep_.clear();
	plantClip_ = std::string::npos;
	plantWindow_ = -1;
	// The root: the loop's Bip01 path, then the game's own handling where it has one (a relocates record, or a technique's action root), else the
	// default of the path rule; the node and the camera/grid follow it while it is on.
	st_.travel = rootTravel();
	applyFollow(it->autoFollow || st_.travel > kRootFollowTravel);
}

const animlab::Clip* animlab::Lab::current() const
{
	const auto it = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == st_.clip; });
	return it == st_.clips.end() ? nullptr : &*it;
}

// The current clip's Bip01 path over one loop, u: its horizontal travel. Sampled on the pose, so a record whose root track is deleted has none.
// The measure sees the real root (the root lock off); the playback position comes back after.
float animlab::Lab::rootTravel()
{
	const kworld::Skeleton* sk = chars_ ? chars_->skeleton(0) : nullptr;
	const Clip* c = current();
	if (!sk || !c || c->length <= 0) { return 0; }
	int root = sk->bone("Bip01");
	if (root < 0 && !sk->parentFirst.empty()) { root = sk->parentFirst.front(); }
	if (root < 0) { return 0; }
	const bool rootWas = chars_->live(0).rootInPlace;
	chars_->live(0).rootInPlace = false;
	double prev[3] = {}, path = 0;
	for (int k = 0; k < kMeasureSamples; k++)
	{
		chars_->setClipTime(0, c->length * (float)k / kMeasureSamples);
		double p[3];
		if (!chars_->boneWorld(0, sk->bones[(size_t)root].name.c_str(), p)) { path = 0; break; }
		if (k) { path += std::hypot(p[0] - prev[0], p[2] - prev[2]); }
		std::copy(p, p + 3, prev);
	}
	chars_->live(0).rootInPlace = rootWas;
	chars_->setClipTime(0, time_);
	return (float)path;
}

// The node: the plant shift, plus the followed root while the root lock is on (the pose zeroes it, so the body stands where the clip's root put it).
void animlab::Lab::place()
{
	double r[2] = {};
	if (follow_) { chars_->rootOffset(0, r); }
	root_[0] = r[0];
	root_[1] = r[1];
	chars_->live(0).pos[0] = shift_[0] + r[0];
	chars_->live(0).pos[1] = 0;
	chars_->live(0).pos[2] = shift_[2] + r[1];
}

// The root lock, as the game plays it: the pose zeroes Bip01's x/z (Spec::rootInPlace) and the node carries them, so the camera and the grid follow
// the character through its travel while the root path overlay stays in world space.
void animlab::Lab::applyFollow(bool on)
{
	follow_ = on;
	root_[0] = root_[1] = 0;
	if (chars_ && chars_->skeleton(0))
	{
		chars_->live(0).rootInPlace = on;
		chars_->setClipTime(0, time_);	// the pose under the mode: the root offset comes from it
		place();
		chars_->update(0);
	}
	st_.follow = follow_;
	st_.root = rootLine();
}

// The root readout (the coordinator's wording): a relocates record shows "relocates (root-locked)"; a clip whose Bip01 stays is "in place"; else the
// path it walks, with the mode it is played in. A record shows its own `relocates` flag too.
std::string animlab::Lab::rootLine() const
{
	const Clip* c = current();
	if (!c) { return "in place"; }
	if (c->relocates) { return "relocates (root-locked)"; }
	std::string s = c->kind == "record" ? "record relocates 0, " : "";
	if (st_.travel <= kRootFollowTravel) { return s + "in place"; }
	char b[96];
	snprintf(b, sizeof b, "travels %.1f u per loop (%s)", st_.travel, follow_ ? "root-locked" : "unlocked");
	return s + b;
}

void animlab::Lab::follow(bool on)
{
	if (!chars_ || !chars_->skeleton(0)) { return; }
	const Clip* c = current();
	if (!c) { st_.reply = "no clip"; return; }
	if (c->autoFollow)
	{
		st_.reply = std::string("root-locked: the game plays ") + (c->relocates ? "a relocates record" : "a technique") + "'s root as the entity's position";
		printf("lab: %s\n", st_.reply.c_str());
		return;
	}
	applyFollow(on);
	st_.reply = std::string("follow ") + (on ? "1" : "0") + " (" + rootLine() + ")";
	printf("lab: %s\n", st_.reply.c_str());
}

void animlab::Lab::play(bool on) { st_.playing = on; st_.reply = on ? "playing" : "paused"; }

void animlab::Lab::step(int dir)
{
	if (st_.frames <= 0) { return; }
	st_.playing = false;
	st_.frame = (st_.frame + dir + st_.frames) % st_.frames;
	seek((double)st_.frame / kFrameRate);
}

void animlab::Lab::seek(double t)
{
	if (!chars_) { return; }
	const auto it = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == st_.clip; });
	const float len = it != st_.clips.end() ? it->length : 0;
	if (len > 0) { t = t - std::floor(t / len) * len; }
	time_ = (float)t;
	st_.time = (float)t;
	st_.frame = st_.frames > 0 ? std::min(st_.frames - 1, (int)std::lround(t * kFrameRate)) : 0;
	chars_->setClipTime(0, time_);
	if (chars_->skeleton(0)) { place(); }	// the root lock's node follows the new time (the pause reads the pose as placed)
}

void animlab::Lab::speed(double x) { st_.speed = std::clamp(x, 0.1, 2.0); }

void animlab::Lab::camera(const std::string& name)
{
	if (name != "side" && name != "front" && name != "top" && name != "orbit") { st_.reply = "camera: side, front, top or orbit"; return; }
	st_.camera = name;
}

void animlab::Lab::orbit(double dx, double dy)
{
	if (st_.camera != "orbit") { st_.camera = "orbit"; }
	orbitYaw_ -= dx * 0.01;
	orbitPitch_ = std::clamp(orbitPitch_ + dy * 0.01, -1.4, 1.45);
}

void animlab::Lab::zoom(double wheel) { dist_ = std::clamp(dist_ * std::pow(0.9, wheel), 8.0, 300.0); }

void animlab::Lab::plant(const std::string& point)
{
	auto low = point;
	std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	std::fill(std::begin(shift_), std::end(shift_), 0.0);
	std::fill(std::begin(plantBase_), std::end(plantBase_), 0.0);
	plantWindow_ = -1;
	if (low.empty() || low == "lheel" || low == "lball" || low == "rheel" || low == "rball") { st_.plant = low; return; }
	const kworld::Skeleton* sk = chars_ ? chars_->skeleton(0) : nullptr;
	if (sk && sk->bone(point) >= 0) { st_.plant = point; return; }
	st_.reply = "plant: lheel, lball, rheel, rball or a bone name";
}

void animlab::Lab::overlay(const std::string& which, bool on)
{
	if (which == "trails") { st_.trails = on; }
	else if (which == "root") { st_.rootPath = on; }
	else if (which == "bones") { st_.boneNames = on; }
	else { st_.reply = "overlay: trails, root or bones"; }
}

void animlab::Lab::viewSize(int w, int h)
{
	std::string err;
	if (!target(std::clamp(w, 64, 2048), std::clamp(h, 64, 2048), err)) { st_.reply = err; }
}

void animlab::Lab::race(const std::string& name)
{
	if (std::find(st_.races.begin(), st_.races.end(), name) == st_.races.end()) { st_.reply = "no race named " + name; return; }
	st_.race = name;
	build();
}

void animlab::Lab::female(bool on)
{
	st_.female = on;
	build();
}

void animlab::Lab::close() { st_.open = false; }

void animlab::Lab::group(const std::string& name)
{
	auto count = [&](const char* g)
	{
		return (size_t)std::count_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return std::find(c.groups.begin(), c.groups.end(), g) != c.groups.end(); });
	};
	if (name.empty() || folded(name) == "all")
	{
		st_.group.clear();
		std::string all;
		for (const char* g : kGroupNames) { all += (all.empty() ? "" : ", ") + std::string(g) + " " + std::to_string(count(g)); }
		st_.reply = std::to_string(st_.clips.size()) + " clips";
		printf("lab: groups: %s\nlab: %s\n", all.c_str(), st_.reply.c_str());
		return;
	}
	for (const char* g : kGroupNames)
	{
		if (folded(g) != folded(name)) { continue; }
		st_.group = g;
		st_.reply = std::to_string(count(g)) + " clips in " + g;
		printf("lab: %s\n", st_.reply.c_str());
		return;
	}
	st_.reply = "no group named \"" + name + "\"";
	printf("lab: %s\n", st_.reply.c_str());
}

void animlab::Lab::list()
{
	size_t n = 0;
	for (const Clip& c : st_.clips)
	{
		if (!st_.group.empty() && std::find(c.groups.begin(), c.groups.end(), st_.group) == c.groups.end()) { continue; }
		printf("lab: clip \"%s\" %s %.3f s %d frames [%s]\n", c.name.c_str(), c.kind.c_str(), c.length, c.frames, joined(c.groups).c_str());
		n++;
	}
	st_.reply = std::to_string(n) + (st_.group.empty() ? " clips" : " clips in " + st_.group);
	printf("lab: %s\n", st_.reply.c_str());
}

void animlab::Lab::mark()
{
	printf("lab: mark clip \"%s\" t %.3f frame %d of %d speed %.2f camera %s plant \"%s\"\n", st_.clip.c_str(), st_.time, st_.frame, st_.frames,
		st_.speed, st_.camera.c_str(), st_.plant.c_str());
	if (const Clip* c = current())
	{
		printf("lab: mark clip groups [%s] root %s\n", joined(c->groups).c_str(), rootLine().c_str());
		if (!c->usedBy.empty()) { printf("lab: mark clip used by %s\n", c->usedBy.c_str()); }
	}
	st_.reply = "marked frame " + std::to_string(st_.frame);
}

void animlab::Lab::update(double dt)
{
	if (!ready_ || !chars_) { return; }
	const auto it = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == st_.clip; });
	const float len = it != st_.clips.end() ? it->length : 0;
	if (st_.playing && len > 0)
	{
		time_ += (float)(dt * st_.speed);
		if (time_ >= len) { time_ -= std::floor(time_ / len) * len; }
		st_.time = time_;
		st_.frame = std::min(st_.frames - 1, (int)std::lround(time_ * kFrameRate));
	}
	else if (!st_.playing) { time_ = st_.time; }
	if (!chars_->skeleton(0)) { return; }
	chars_->setClipTime(0, time_);
	// plant: the chosen point stays fixed on the ground through its contact window; the body walks by its own feet. The windows come from the
	// measurement, so a plant is only live once `measure` (or the first update after picking the point) has run. shift_ persists: between
	// windows the body keeps the ground it walked on, and a window compares against the shift it had when it began (plantBase_).
	if (!st_.plant.empty() && len > 0)
	{
		const auto ci = (size_t)(it - st_.clips.begin());
		if (keep_.empty() || plantClip_ != ci)
		{
			std::vector<Sample> samples;
			const bool rootWas = chars_->live(0).rootInPlace;
			chars_->live(0).rootInPlace = false;	// the windows are the clip's own foot travel
			measureLoop(samples, kMeasureSamples);
			chars_->live(0).rootInPlace = rootWas;
			std::string sum;
			analyse(samples, keep_, sum);
			plantClip_ = ci;
			chars_->setClipTime(0, time_);	// analyse walked the layers' times
		}
		plantWindow_ = -1;
		int foot = -1, sole = -1;
		if (st_.plant == "lheel") { foot = 0; sole = 0; }
		else if (st_.plant == "lball") { foot = 0; sole = 1; }
		else if (st_.plant == "rheel") { foot = 1; sole = 0; }
		else if (st_.plant == "rball") { foot = 1; sole = 1; }
		if (foot >= 0)
		{
			const int n = kMeasureSamples;	// the windows are the measure's samples
			const int phase = std::min(n - 1, (int)(time_ / len * n));
			for (size_t wi = 0; wi < keep_.size(); wi++)
			{
				const Window& w = keep_[wi];
				if (w.foot != foot || w.sole != sole) { continue; }
				const bool inside = phase >= w.from ? phase < w.from + w.count : phase + n < w.from + w.count;
				if (!inside) { continue; }
				// the point at the window's start and now: the shift holds the point where the window began, and the body keeps the shift it
				// reached by then, so it walks on at each next contact instead of jumping back
				if (plantWindow_ != (int)wi)
				{
					std::copy(std::begin(shift_), std::end(shift_), std::begin(plantBase_));
					plantWindow_ = (int)wi;
				}
				chars_->setClipTime(0, (float)w.from / n * len);
				Sample s0;
				s0.ok = chars_->solePoints(0, s0.p);
				chars_->setClipTime(0, time_);
				Sample s1;
				s1.ok = chars_->solePoints(0, s1.p);
				shift_[0] = plantBase_[0] + s0.p[foot][sole][0] - s1.p[foot][sole][0];
				shift_[2] = plantBase_[2] + s0.p[foot][sole][2] - s1.p[foot][sole][2];
				break;
			}
		}
	}
	place();
	chars_->update(0);
}

void animlab::Lab::measureLoop(std::vector<Sample>& out, int n) const
{
	out.clear();
	const kworld::Skeleton* sk = chars_->skeleton(0);
	if (!sk) { return; }
	const auto it = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == st_.clip; });
	const float len = it != st_.clips.end() ? it->length : 0;
	if (len <= 0 || n < 2) { return; }
	out.resize((size_t)n);
	for (int k = 0; k < n; k++)
	{
		chars_->setClipTime(0, len * (float)k / n);
		Sample& s = out[(size_t)k];
		s.ok = chars_->solePoints(0, s.p);
		s.ok = chars_->boneWorld(0, "Bip01", s.root) && s.ok;
	}
}

void animlab::Lab::analyse(const std::vector<Sample>& samples, std::vector<Window>& windows, std::string& summary) const
{
	windows.clear();
	summary.clear();
	const int n = (int)samples.size();
	if (n < 2) { summary = "lab: nothing to measure (no clip)\n"; return; }
	const auto cit = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == st_.clip; });
	const double clipLen = cit != st_.clips.end() ? cit->length : 0.0;
	for (const Sample& s : samples) { if (!s.ok) { summary = "lab: no biped sole fit on this skeleton (plant by bone instead)\n"; return; } }
	float lowest[2] = { 1e30f, 1e30f };
	for (int k = 0; k < n; k++) { for (int f = 0; f < 2; f++) { for (int w = 0; w < 2; w++) { lowest[f] = std::min(lowest[f], (float)samples[(size_t)k].p[f][w][1]); } } }
	auto backward = [&](int k, int f, int w)
	{
		const double* p0 = samples[(size_t)((k + n - 1) % n)].p[f][w];
		const double* p1 = samples[(size_t)((k + 1) % n)].p[f][w];
		const double fx = samples[(size_t)k].p[f][1][0] - samples[(size_t)k].p[f][0][0], fz = samples[(size_t)k].p[f][1][2] - samples[(size_t)k].p[f][0][2];
		const double len = std::hypot(fx, fz);
		return len > 1e-6 && ((p1[0] - p0[0]) * fx + (p1[2] - p0[2]) * fz) / len < 0;
	};
	char line[256];
	snprintf(line, sizeof line, "lab: %s %.3f s %d frames, %d samples\n", st_.clip.c_str(), clipLen, st_.frames, n);
	summary += line;
	std::vector<unsigned char> planted((size_t)n * 4, 0);	// sample x foot x sole: the point is planted (the plant integration below reads it too)
	auto isPlanted = [&](int k, int f, int w) { return planted[(size_t)k * 4 + f * 2 + w] != 0; };
	for (int f = 0; f < 2; f++)
	{
		for (int w = 0; w < 2; w++)
		{
			for (int k = 0; k < n; k++) { planted[(size_t)k * 4 + f * 2 + w] = (float)samples[(size_t)k].p[f][w][1] - lowest[f] <= kContactHeight && backward(k, f, w); }
			int start = 0;
			while (start < n && isPlanted(start, f, w)) { start++; }
			for (int i = 0; i < n; )
			{
				if (!isPlanted((start + i) % n, f, w)) { i++; continue; }
				int len = 0;
				while (len < n && isPlanted((start + i + len) % n, f, w)) { len++; }
				const int a = (start + i) % n, b = (start + i + len - 1) % n;
				const double dx = samples[(size_t)b].p[f][w][0] - samples[(size_t)a].p[f][w][0];
				const double dz = samples[(size_t)b].p[f][w][2] - samples[(size_t)a].p[f][w][2];
				const double travel = std::hypot(dx, dz);
				const double seconds = (double)len / n * clipLen;	// the window's length
				if (travel > 1e-6 && seconds > 0)
				{
					Window win;
					win.foot = f;
					win.sole = w;
					win.from = a;
					win.count = len;
					win.travel = travel;
					win.speed = travel / seconds;	// the point moves back this fast while it is planted: the body's ground speed
					win.fwd = -dz / seconds;	// the body (the point held still) moves opposite to the point
					win.lat = -dx / seconds;
					// residual slip: the point's horizontal distance from the window's chord
					double rms = 0, maxd = 0;
					for (int j = 0; j < len; j++)
					{
						const int k = (a + j) % n;
						const double px = samples[(size_t)k].p[f][w][0] - samples[(size_t)a].p[f][w][0];
						const double pz = samples[(size_t)k].p[f][w][2] - samples[(size_t)a].p[f][w][2];
						const double t = travel > 1e-9 ? (px * dx + pz * dz) / (travel * travel) : 0.0;
						const double ex = px - dx * t, ez = pz - dz * t;
						const double d = std::hypot(ex, ez);
						rms += d * d;
						maxd = std::max(maxd, d);
					}
					win.slipRms = std::sqrt(rms / len);
					win.slipMax = maxd;
					windows.push_back(win);
					snprintf(line, sizeof line, "lab: window %c %s %.3f-%.3f s (%d samples) travel %.2f u speed %.2f u/s body fwd %.2f lat %.2f slip rms %.3f max %.3f\n",
						f ? 'R' : 'L', w ? "ball" : "heel", (double)a / n * clipLen, (double)(a + len) / n * clipLen,
						len, travel, win.speed, win.fwd, win.lat, win.slipRms, win.slipMax);
					summary += line;
				}
				i += len;
			}
		}
	}
	// per foot: that foot's own contacts (the two feet can move opposite ways: left +lat, right -lat cancel in a naive sum)
	for (int f = 0; f < 2; f++)
	{
		double u = 0, secs = 0, fwd = 0, lat = 0; int count = 0, wins = 0;
		for (const Window& w : windows)
		{
			if (w.foot != f) { continue; }
			wins++;
			u += w.travel;
			secs += (double)w.count / n * clipLen;
			fwd += w.fwd * w.count;
			lat += w.lat * w.count;
			count += w.count;
		}
		if (wins == 0) { continue; }
		snprintf(line, sizeof line, "lab: %c foot: %d window%s, %.2f u over %.2f s, body fwd %+.2f lat %+.2f u/s (signed)\n",
			f ? 'R' : 'L', wins, wins == 1 ? "" : "s", u, secs, count ? fwd / count : 0.0, count ? lat / count : 0.0);
		summary += line;
	}
	double travel = 0, span = 0, gone = 0, gspan = 0;
	for (const Window& w : windows)
	{
		travel += w.travel;
		span += (double)w.count / n * clipLen;
		// deriveContacts' own travel: the window chord projected on the foot's heel -> ball axis at the window's start
		const int a = w.from, b = (w.from + w.count - 1) % n;
		const double ax = samples[(size_t)a].p[w.foot][1][0] - samples[(size_t)a].p[w.foot][0][0];
		const double az = samples[(size_t)a].p[w.foot][1][2] - samples[(size_t)a].p[w.foot][0][2];
		const double al = std::hypot(ax, az);
		const double gx = samples[(size_t)b].p[w.foot][w.sole][0] - samples[(size_t)a].p[w.foot][w.sole][0];
		const double gz = samples[(size_t)b].p[w.foot][w.sole][2] - samples[(size_t)a].p[w.foot][w.sole][2];
		const double g = al > 1e-6 ? -(gx * ax + gz * az) / al : 0;	// positive: the point went backward along the foot
		if (g > 0) { gone += g; gspan += (double)w.count / n * clipLen; }
	}
	if (span > 0)
	{
		const double speed = travel / span;
		snprintf(line, sizeof line, "lab: windows: %.2f u/cycle (chord), %.2f u/cycle (dump contacts' heel-ball projection), %.2f u/s (chord sum)\n",
			speed * clipLen, gspan > 0 ? gone / gspan * clipLen : 0.0, speed);
		summary += line;
	}
	// The root's displacement per cycle: while a point is planted the body must move opposite to it. Average the planted points at each sample
	// (a coherent clip pins both feet to one motion; their disagreement is the residual slip) and carry the mean speed over the whole cycle. For a
	// clip in place (Bip01 only sways) this is how the body walks by its own feet; a root-motion clip's own travel is the root lock's figure.
	const double dt = clipLen / n;
	double body[2] = {}, held = 0;	// the body's model-space displacement (x = right, z = forward) while the feet are planted, and its time
	for (int k = 0; k < n; k++)
	{
		double vx = 0, vz = 0; int m = 0;
		for (int f = 0; f < 2; f++)
		{
			for (int w = 0; w < 2; w++)
			{
				if (!isPlanted(k, f, w)) { continue; }
				const double* p0 = samples[(size_t)((k + n - 1) % n)].p[f][w];
				const double* p1 = samples[(size_t)((k + 1) % n)].p[f][w];
				vx += (p1[0] - p0[0]) / (2 * dt);
				vz += (p1[2] - p0[2]) / (2 * dt);
				m++;
			}
		}
		if (m) { body[0] -= vx / m * dt; body[1] -= vz / m * dt; held += dt; }
	}
	if (held > 0)
	{
		const double vx = body[0] / held, vz = body[1] / held, speed = std::hypot(vx, vz);
		const double mag = std::fabs(vx) + std::fabs(vz);
		double xmin = 1e30, xmax = -1e30, zmin = 1e30, zmax = -1e30;
		for (const Sample& s : samples) { xmin = std::min(xmin, s.root[0]); xmax = std::max(xmax, s.root[0]); zmin = std::min(zmin, s.root[2]); zmax = std::max(zmax, s.root[2]); }
		snprintf(line, sizeof line, "lab: root (Bip01) sway %.2f u x %.2f u z; feet planted -> %.2f u/s, %.2f u/cycle: fwd %+.2f lat %+.2f u/cycle, direction fwd %.0f%% lat %.0f%%\n",
			xmax - xmin, zmax - zmin, speed, speed * clipLen, vz * clipLen, vx * clipLen,
			mag > 1e-9 ? 100 * std::fabs(vz) / mag : 0.0, mag > 1e-9 ? 100 * std::fabs(vx) / mag : 0.0);
		summary += line;
	}
	else { summary += "lab: no sole contact window in this clip\n"; }
}

void animlab::Lab::measure()
{
	if (!ready_ || !chars_ || st_.clip.empty()) { return; }
	std::fill(std::begin(shift_), std::end(shift_), 0.0);
	chars_->live(0).pos[0] = chars_->live(0).pos[1] = chars_->live(0).pos[2] = 0;
	const bool rootWas = chars_->live(0).rootInPlace;
	chars_->live(0).rootInPlace = false;	// the measure sees the clip's own root (the root lock zeroes it in the playback pose)
	std::vector<Sample> samples;
	measureLoop(samples, kMeasureSamples);
	chars_->live(0).rootInPlace = rootWas;
	std::string sum;
	analyse(samples, keep_, sum);
	plantClip_ = (size_t)std::distance(st_.clips.begin(), std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == st_.clip; }));
	st_.readout = "lab: root: " + rootLine() + "\n" + sum;
	printf("lab: root: %s\n", rootLine().c_str());
	printf("%s", sum.c_str());
	chars_->setClipTime(0, time_);	// the measurement walked the layers' times: back to the playback position
	place();
	chars_->update(0);
}

bool animlab::Lab::csv(const std::string& path)
{
	if (!ready_ || !chars_ || st_.clip.empty()) { st_.reply = "no clip"; return false; }
	std::fill(std::begin(shift_), std::end(shift_), 0.0);
	chars_->live(0).pos[0] = chars_->live(0).pos[1] = chars_->live(0).pos[2] = 0;
	std::vector<Sample> samples;
	const auto it = std::find_if(st_.clips.begin(), st_.clips.end(), [&](const Clip& c) { return c.name == st_.clip; });
	const float len = it != st_.clips.end() ? it->length : 0;
	const int n = std::max(2, (int)std::lround(len * kFrameRate));	// per clip frame, for the user's eye (the measure samples 96)
	const bool rootWas = chars_->live(0).rootInPlace;
	chars_->live(0).rootInPlace = false;	// the rows are the clip's own root travel
	measureLoop(samples, n);
	chars_->live(0).rootInPlace = rootWas;
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) { st_.reply = "cannot write " + path; return false; }
	fprintf(f, "clip,t,frame,lheel_x,lheel_y,lheel_z,lball_x,lball_y,lball_z,rheel_x,rheel_y,rheel_z,rball_x,rball_y,rball_z,root_x,root_y,root_z,planted\n");
	int pfoot = -1, psole = -1;
	if (st_.plant == "lheel") { pfoot = 0; psole = 0; }
	else if (st_.plant == "lball") { pfoot = 0; psole = 1; }
	else if (st_.plant == "rheel") { pfoot = 1; psole = 0; }
	else if (st_.plant == "rball") { pfoot = 1; psole = 1; }
	for (int k = 0; k < n && k < (int)samples.size() && samples[(size_t)k].ok; k++)
	{
		const Sample& s = samples[(size_t)k];
		const double t = (double)len * k / n;
		bool on = false;	// the chosen plant point is in one of its contact windows at this frame (the windows are the measure's samples)
		for (const Window& w : keep_)
		{
			if (w.foot != pfoot || w.sole != psole) { continue; }
			const double a = (double)w.from / kMeasureSamples * len, b = a + (double)w.count / kMeasureSamples * len;
			if ((t >= a && t < b) || (b > len && t < b - len)) { on = true; break; }
		}
		fprintf(f, "%s,%.4f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", st_.clip.c_str(), t, k,
			s.p[0][0][0], s.p[0][0][1], s.p[0][0][2], s.p[0][1][0], s.p[0][1][1], s.p[0][1][2],
			s.p[1][0][0], s.p[1][0][1], s.p[1][0][2], s.p[1][1][0], s.p[1][1][1], s.p[1][1][2],
			s.root[0], s.root[1], s.root[2], on ? 1 : 0);
	}
	fclose(f);
	st_.reply = "wrote " + path;
	printf("lab: csv %s (%d rows)\n", path.c_str(), n);
	chars_->setClipTime(0, time_);
	place();
	chars_->update(0);
	return true;
}

bool animlab::Lab::target(int w, int h, std::string& err)
{
	(void)err;	// a failed make shows in the job (the lab draws nothing, `lab shot` answers no render)
	// The UI's id for the view (kenshi-k16.2.205.12.38): one id for the lab's life, allocated here; the job binds the new view to it
	static const dl::TexId viewTex = dl::newTex();
	const rq::Handle<Target> old = tgt_;
	const bool hadOld = tgtQueued_;
	tgt_ = rq::Handle<Target>();
	tgtQueued_ = true;
	rq::push("lab-target", [old, hadOld, out = tgt_, w, h]
	{
		Target t;
		if (hadOld)	// the same lane, queued earlier: its job has run
		{
			destroyTarget(old.get(), false);
			t.sgl = old.get().sgl;
			t.sdtx = old.get().sdtx;
		}
		sg_image_desc id = {};
		id.usage.color_attachment = true;
		id.width = w;
		id.height = h;
		id.pixel_format = SG_PIXELFORMAT_RGBA16F;	// the character pipelines' colour format
		id.sample_count = 1;
		id.label = "anim-lab";
		t.color = sg_make_image(&id);
		t.ok = t.color.id != SG_INVALID_ID;
		if (!t.ok) { fprintf(stderr, "kclient: anim lab: cannot create the lab render target\n"); }
		sg_view_desc vd = {};
		vd.texture.image = t.color;
		t.colorTex = sg_make_view(&vd);
		vd = {};
		vd.color_attachment.image = t.color;
		t.colorAtt = sg_make_view(&vd);
		id = {};
		id.usage.depth_stencil_attachment = true;
		id.width = w;
		id.height = h;
		id.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
		id.sample_count = 1;
		id.label = "anim-lab-depth";
		t.depth = sg_make_image(&id);
		vd = {};
		vd.depth_stencil_attachment.image = t.depth;
		t.depthAtt = sg_make_view(&vd);
		sg_sampler_desc sd = {};
		sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
		sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
		t.sampler = sg_make_sampler(&sd);
		if (!t.sgl.id)
		{
			sgl_context_desc_t d = {};
			d.color_format = SG_PIXELFORMAT_RGBA16F;
			d.depth_format = SG_PIXELFORMAT_NONE;	// the overlay pass has no depth buffer: the lines are never hidden by the body
			d.sample_count = 1;
			t.sgl = sgl_make_context(&d);
		}
		if (!t.sdtx.id)
		{
			sdtx_context_desc_t d = {};
			d.color_format = SG_PIXELFORMAT_RGBA16F;
			d.depth_format = SG_PIXELFORMAT_NONE;
			d.sample_count = 1;
			d.char_buf_size = 4096;
			t.sdtx = sdtx_make_context(&d);
		}
		dl::gpu::bind(viewTex, t.colorTex, t.sampler);
		out.publish(t);
	});
	texW_ = w;
	texH_ = h;
	st_.view.tex = viewTex;
	// GL: the render target's row 0 is the image's bottom (texture v 0), so the top edge is v 1 (as Portraits::get; user 2026-11-07: upside down)
	st_.view.uv[0] = st_.view.uv[3] = 0;
	st_.view.uv[1] = st_.view.uv[2] = 1;
	return true;
}

void animlab::Lab::destroyTarget(const Target& t, bool contexts)
{
	if (t.sampler.id) { sg_destroy_sampler(t.sampler); }
	for (sg_view v : { t.colorTex, t.colorAtt, t.depthAtt }) { if (v.id) { sg_destroy_view(v); } }
	if (t.color.id) { sg_destroy_image(t.color); }
	if (t.depth.id) { sg_destroy_image(t.depth); }
	if (contexts && t.sgl.id) { sgl_destroy_context(t.sgl); }
	if (contexts && t.sdtx.id) { sdtx_destroy_context(t.sdtx); }
}

// shutdown only (cleanup(): the logic thread is joined and rq::flushAll has run every job, so the target job's result is in)
void animlab::Lab::dropTarget()
{
	if (tgtQueued_ && tgt_.ready()) { destroyTarget(tgt_.get(), true); }
	tgt_ = rq::Handle<Target>();
	tgtQueued_ = false;
	if (st_.view.tex) { dl::gpu::unbind(st_.view.tex); }
	st_.view = {};
	texW_ = texH_ = 0;
}

void animlab::Lab::frame(dev::LabFrame& f)
{
	f.lines.clear();
	f.labels.clear();
	f.draw = ready_ && chars_ && active() && tgtQueued_ && tgt_.ready() && tgt_.get().ok && texW_ > 0 && chars_->skeleton(0);
	if (!f.draw) { return; }
	const Target& tg = tgt_.get();	// published by its job: the packet may name it
	f.chars = chars_.get();
	f.light = light_;
	f.w = texW_;
	f.h = texH_;
	f.colorAtt = tg.colorAtt;
	f.depthAtt = tg.depthAtt;
	f.sgl = tg.sgl;
	f.sdtx = tg.sdtx;
	f.boneNames = st_.boneNames;
	// the character's node (the plant shift plus the followed root): the camera follows it, and with the root lock so does the grid, so the figure
	// stays in frame while the ground lines slide under it
	const double nx = shift_[0] + root_[0], nz = shift_[2] + root_[1];
	const double at[3] = { nx, 9.0 + shift_[1], nz };
	double yaw, pitch;
	if (st_.camera == "side") { yaw = kPi * 0.5; pitch = 0.12; }
	else if (st_.camera == "front") { yaw = 0; pitch = 0.12; }
	else if (st_.camera == "top") { yaw = 0; pitch = 1.45; }
	else { yaw = orbitYaw_; pitch = orbitPitch_; }
	double* eye = f.eye;
	eye[0] = at[0] + dist_ * std::sin(yaw) * std::cos(pitch);
	eye[1] = at[1] + dist_ * std::sin(pitch);
	eye[2] = at[2] + dist_ * std::cos(yaw) * std::cos(pitch);
	chars_->portraitCopy(0, eye, f.frame);	// the pose Lab::update set, gathered here on the logic thread (render draws only f.frame)
	double fw[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
	const double fl = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
	for (double& v : fw) { v /= fl; }
	double r[3] = { -fw[2], 0, fw[0] };
	const double rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
	r[0] /= rl;
	r[2] /= rl;
	const double u[3] = { r[1] * fw[2] - r[2] * fw[1], r[2] * fw[0] - r[0] * fw[2], r[0] * fw[1] - r[1] * fw[0] };
	float view[16] = {};
	for (int i = 0; i < 3; i++)
	{
		view[i * 4 + 0] = (float)r[i];
		view[i * 4 + 1] = (float)u[i];
		view[i * 4 + 2] = (float)-fw[i];
	}
	view[15] = 1;
	const float t = 1.0f / std::tan(25.0f * kPi / 360.0f);
	float proj[16] = {};
	proj[0] = t * (float)texH_ / (float)texW_;	// the vertical FOV is 25 deg; the target's aspect widens the horizontal one
	proj[5] = t;
	proj[11] = -1;
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
	proj[10] = 400 / (0.1f - 400);
	proj[14] = 400 * 0.1f / (0.1f - 400);
#else
	proj[10] = (400 + 0.1f) / (0.1f - 400);
	proj[14] = 2 * 400 * 0.1f / (0.1f - 400);
#endif
	float* mvp = f.mvp;
	for (int col = 0; col < 4; col++)
	{
		for (int row = 0; row < 4; row++)
		{
			float s = 0;
			for (int k = 0; k < 4; k++) { s += proj[k * 4 + row] * view[col * 4 + k]; }
			mvp[col * 4 + row] = s;
		}
	}

	// the floor grid, the sole points with trails, the root path and the bone names: drawn in a second, depth-less pass over the image (an
	// overlay that is never hidden by the body it measures); here as pixel lines in the colour each was given
	float cur[4] = { 1, 1, 1, 1 };
	auto colour = [&](float r0, float g0, float b0, float a0) { cur[0] = r0; cur[1] = g0; cur[2] = b0; cur[3] = a0; };
	auto seg = [&](const double a[3], const double b[3])
	{
		dev::LabFrame::Line l;
		float ad = 0, bd = 0;
		project(mvp, eye, a, texW_, texH_, l.x0, l.y0, ad);
		project(mvp, eye, b, texW_, texH_, l.x1, l.y1, bd);
		std::copy(cur, cur + 4, l.c);
		f.lines.push_back(l);
	};
	const double gx = follow_ ? nx : 0.0, gz = follow_ ? nz : 0.0;	// the grid under the followed character
	for (int i = -kGrid; i <= kGrid; i++)
	{
		const double a[3] = { gx + i, 0, gz - kGrid }, b[3] = { gx + i, 0, gz + kGrid };
		const double c[3] = { gx - kGrid, 0, gz + i }, d[3] = { gx + kGrid, 0, gz + i };
		if (i == 0) { colour(0.55f, 0.35f, 0.35f, 0.8f); } else { colour(0.45f, 0.47f, 0.5f, 0.35f); }
		seg(a, b);
		if (i == 0) { colour(0.35f, 0.4f, 0.6f, 0.8f); } else { colour(0.45f, 0.47f, 0.5f, 0.35f); }
		seg(c, d);
	}
	// the sole points: L heel cyan, L ball blue, R heel green, R ball orange; their trails over the loop
	static const float kCol[2][2][4] = { { { 0.2f, 0.9f, 1.0f, 1 }, { 0.2f, 0.4f, 1.0f, 1 } }, { { 0.3f, 1.0f, 0.4f, 1 }, { 1.0f, 0.6f, 0.2f, 1 } } };
	double soles[2][2][3];
	const bool haveSoles = chars_->solePoints(0, soles);
	if (haveSoles)
	{
		for (int ft = 0; ft < 2; ft++)
		{
			for (int w = 0; w < 2; w++)
			{
				const double* q = soles[ft][w];
				colour(kCol[ft][w][0], kCol[ft][w][1], kCol[ft][w][2], 1);
				const double x0[3] = { q[0] - 0.5, q[1], q[2] }, x1[3] = { q[0] + 0.5, q[1], q[2] };
				const double z0[3] = { q[0], q[1], q[2] - 0.5 }, z1[3] = { q[0], q[1], q[2] + 0.5 };
				seg(x0, x1);
				seg(z0, z1);
				std::vector<float>& tr = trail_[ft][w];
				if (st_.trails)
				{
					if (tr.empty() || std::fabs(tr[tr.size() - 3] - q[0]) + std::fabs(tr[tr.size() - 1] - q[2]) > 0.01f)
					{
						tr.push_back((float)q[0]); tr.push_back((float)q[1]); tr.push_back((float)q[2]);
						const size_t cap = 256 * 3;
						if (tr.size() > cap) { tr.erase(tr.begin(), tr.begin() + (long)(tr.size() - cap)); }
					}
					colour(kCol[ft][w][0], kCol[ft][w][1], kCol[ft][w][2], 0.5f);
					for (size_t i = 3; i + 2 < tr.size(); i += 3) { const double a[3] = { tr[i - 3], tr[i - 2], tr[i - 1] }, b[3] = { tr[i], tr[i + 1], tr[i + 2] }; seg(a, b); }
				} else { tr.clear(); }
			}
		}
	}
	if (st_.rootPath)
	{
		double root[3];
		if (chars_->boneWorld(0, "Bip01", root))
		{
			colour(1.0f, 1.0f, 0.3f, 0.9f);
			if (rootPath_.empty() || std::fabs(rootPath_[rootPath_.size() - 3] - root[0]) + std::fabs(rootPath_[rootPath_.size() - 1] - root[2]) > 0.005f)
			{
				rootPath_.push_back((float)root[0]); rootPath_.push_back((float)root[1]); rootPath_.push_back((float)root[2]);
				const size_t cap = 512 * 3;
				if (rootPath_.size() > cap) { rootPath_.erase(rootPath_.begin(), rootPath_.begin() + (long)(rootPath_.size() - cap)); }
			}
			for (size_t i = 3; i + 2 < rootPath_.size(); i += 3) { const double a[3] = { rootPath_[i - 3], rootPath_[i - 2], rootPath_[i - 1] }, b[3] = { rootPath_[i], rootPath_[i + 1], rootPath_[i + 2] }; seg(a, b); }
		}
	} else { rootPath_.clear(); }
	if (st_.boneNames)
	{
		const kworld::Skeleton* sk = chars_->skeleton(0);
		for (const kworld::SkeletonBone& b : sk->bones)
		{
			double q[3];
			if (!chars_->boneWorld(0, b.name.c_str(), q)) { continue; }
			float sx = 0, sy = 0, d = 0;
			project(mvp, eye, q, texW_, texH_, sx, sy, d);
			f.labels.push_back({ sx + 2, sy + 1, b.name });
		}
	}
}

void animlab::Lab::render(const dev::LabFrame& f, const Shadow::FrameData& shadow)
{
	if (!f.draw || !f.chars) { return; }
	f.chars->sfd = &shadow;	// the packet's shadow data, for this draw
	sg_pass p = {};
	p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	p.action.colors[0].clear_value = { 0.10f, 0.11f, 0.13f, 1.0f };
	p.action.depth.load_action = SG_LOADACTION_CLEAR;
	p.action.depth.clear_value = 1.0f;
	p.attachments.colors[0] = f.colorAtt;
	p.attachments.depth_stencil = f.depthAtt;
	sg_begin_pass(&p);
	f.chars->portraitsNewFrame();	// the lab's Characters never runs upload(), which hands the portrait buffer sets back: 64 renders later the body stopped drawing (kenshi-k16.2.309)
	f.chars->renderPortrait(f.frame, f.mvp, f.eye, f.light);	// the frame data Lab::frame built on the logic thread
	sg_end_pass();

	p = {};
	p.action.colors[0].load_action = SG_LOADACTION_LOAD;
	p.attachments.colors[0] = f.colorAtt;
	sg_begin_pass(&p);
	sgl_set_context(f.sgl);
	sgl_defaults();
	sgl_viewport(0, 0, f.w, f.h, true);
	sgl_matrix_mode_projection();
	sgl_ortho(0.0f, (float)f.w, (float)f.h, 0.0f, -1.0f, 1.0f);	// pixels, y down (the client's overlay setup)
	sgl_disable_texture();
	sgl_begin_lines();
	for (const dev::LabFrame::Line& l : f.lines)
	{
		sgl_c4f(l.c[0], l.c[1], l.c[2], l.c[3]);
		sgl_v2f(l.x0, l.y0);
		sgl_v2f(l.x1, l.y1);
	}
	DRAWSTATS_LINES(sgl_num_vertices());
	sgl_end();
	sgl_context_draw(f.sgl);	// sgl_end only records the commands: the context draws them into this pass
	if (f.boneNames)
	{
		sdtx_set_context(f.sdtx);
		sdtx_canvas((float)f.w, (float)f.h);
		sdtx_color3f(1.0f, 1.0f, 1.0f);
		for (const dev::LabFrame::Label& l : f.labels)
		{
			sdtx_pos(l.x, l.y);
			sdtx_printf("%s", l.text.c_str());
		}
		sdtx_draw();
	}
	sgl_set_context(SGL_DEFAULT_CONTEXT);
	sg_end_pass();
}

bool animlab::Lab::shot(const std::string& path)
{
	if (!ready_ || !tgtQueued_ || !tgt_.ready() || !tgt_.get().ok || texW_ <= 0) { st_.reply = "no render yet"; return false; }
#if defined(SOKOL_GLCORE)
	// A render-side read at this frame's serve point (readback.hpp sample: after the lab's pass, kenshi-k16.2.205.12.69); the PNG is written in the
	// done, on the logic thread at the next frame start. The read reads the target this packet's lab pass drew.
	const int w = texW_, h = texH_;
	st_.reply = "shot " + path + " queued";
	readback::sample([t = tgt_, w, h] { return readback::image_rgba32f(t.get().color, w, h); },
		[this, path, w, h](bool ok, const std::vector<float>& px)
	{
		if (!ok || px.size() < (size_t)w * h * 4) { st_.reply = "cannot read the lab target"; printf("lab: %s\n", st_.reply.c_str()); return; }
		std::vector<unsigned char> out((size_t)w * h * 4);
		for (size_t i = 0; i < (size_t)w * h; i++)
		{
			for (int k = 0; k < 3; k++)
			{
				float v = std::max(0.0f, px[i * 4 + (size_t)k]);
				v = v / (1.0f + v);	// a plain Reinhard for the HDR target; the tool is for shape and motion, not colour
				out[i * 4 + (size_t)k] = (unsigned char)std::clamp(std::pow(v, 1.0f / 2.2f) * 255.0f + 0.5f, 0.0f, 255.0f);
			}
			out[i * 4 + 3] = 255;
		}
		if (!readback::write_png(path, w, h, out.data())) { st_.reply = "cannot write " + path; printf("lab: %s\n", st_.reply.c_str()); return; }
		printf("lab: shot %s (%dx%d)\n", path.c_str(), w, h);
		st_.reply = "wrote " + path;
	});
	return true;
#else
	(void)path;
	st_.reply = "lab shot: GL only";
	return false;
#endif
}
