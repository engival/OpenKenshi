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

namespace host
{


// The first collision shape along the ray within far that the view draws (see_through: cut-away floors and roofs pass); building: it is a building's.
// kenshi-k16.2.259 (user 2026-11-15, "pick only what is drawn"): eye (the camera's position, null off the pick) - a shape the ray starts inside is passed
// when it is not drawn at the camera either, MINE: the original reports a shape the ray starts inside at distance 0 (kenshi-rwh.142). skipped: each shape
// passed over and why, for the report.
bool ray_shapes(const double origin[3], const double dir[3], double far, double hit[3], bool* building,
	const double* eye, std::vector<PickSkip>* skipped, float* hitBox, bool foliageOnly)
{
	bool isBuilding = false;
	if (!building) { building = &isBuilding; }
	double from[3] = { origin[0], origin[1], origin[2] }, t;
	float box[6], skippedBox[6] = {};	// a shape passed through is passed whole (its exit face too), told apart by its bounds
	bool solid = false;
	std::string name;
	for (int n = 0; n < 16 && app.collide.ray(from, dir, far, t, box, {}, nullptr, &solid, building, skipped ? &name : nullptr); n++)
	{
		double at[3];
		for (int k = 0; k < 3; k++) { at[k] = from[k] + dir[k] * t; }
		const bool passed = std::equal(box, box + 6, skippedBox) || (foliageOnly && *building);	// foliageOnly (the click pick, kenshi-k16.2.396.5): a building's shape never picks, only foliage shells
		if (!passed)
		{
			std::string why;
			const bool inside = eye && eye[0] >= box[0] && eye[0] <= box[3] && eye[1] >= box[1] && eye[1] <= box[4] && eye[2] >= box[2] && eye[2] <= box[5];
			bool hidden = false;
			if (inside && see_through(eye, 1e30, &why))	// MINE (kenshi-k16.2.259): the camera is inside it and it is not drawn there either
			{
				hidden = true;
				why = "the camera is inside it, " + why;
			}
			else { hidden = see_through(at, solid ? box[1] : 1e30, &why); }	// a staircase counts by the floor it starts on (drawn whole); a wall by its height
			if (!hidden)
			{
				std::copy(at, at + 3, hit);
				if (hitBox) { std::copy(box, box + 6, hitBox); }
				return true;
			}
			if (skipped) { skipped->push_back({ name.empty() ? "an unnamed shape" : name, { at[0], at[1], at[2] }, why }); }
		}
		std::copy(box, box + 6, skippedBox);
		constexpr double kStep = 0.5;	// past the face just hit
		for (int k = 0; k < 3; k++) { from[k] = at[k] + dir[k] * kStep; }
		far -= t + kStep;
	}
	return false;
}

// A scripted run waits for the collision gather before every pick so its clicks are deterministic; the player never waits (the pick uses the shapes
// it has, the worker swaps the new cell's in). Off, a script measures the pick as the player meets it (kenshi-k16.2.205.10: the wait was the whole 92-353 ms
// of sim.peek+hover on a camera jump to a fresh zone cell, Hub 22k shapes ~170 ms).
const cfg::Bool kPickWait{ cfg::Group{ "Scripted runs" }, "debug.pick_wait", true, "Scripted runs: wait for the collision gather before each pick (deterministic clicks). Off: pick with the shapes at hand, as the player does" };


// The floating camera's ground at (x, z): the highest surface drawn there, a building floor or roof (cut-away ones pass) or a rock, else the
// terrain (kinspect camera_rts.md 3: a roof or floor under the pivot, ray from +3000, holds its y). on: "floor" or "terrain".
double ground_under(double x, double z, const char** on)
{
	constexpr double kTop = 3000;
	const double terrain = app.hm->height(x, z);
	const double from[3] = { x, terrain + kTop, z }, down[3] = { 0, -1, 0 };
	double hit[3];
	app.buildings.peekPass = false;	// the camera rests on what stands there, not on what the cursor peeks through
	const bool shape = ray_shapes(from, down, kTop, hit) && hit[1] > terrain;
	app.buildings.peekPass = true;
	*on = shape ? "floor" : "terrain";
	return shape ? hit[1] : terrain;
}

// The click ray's world hit (SIMLINK 13: the picker's bottom layer). kenshi-k16.2.396.5, the rule (user 2026-11-21, ask 396.4: "Item in the world has a 3d mesh,
// that 3d mesh is what we ray cast against. Same system, all objects."): the nearest DRAWN surface along the ray wins. WorldBuildings::drawnPickRay tests the drawn
// triangles of every instance (building parts, layout furniture, props, doors at their drawn pose); a hit the cut, the ghost or the occluder cone hides restarts
// the ray just past it. Collision picks only foliage shells (trees, rocks, deposit rocks); the terrain is the ground under the carve rule (a hit inside a carved
// interior is the dune the interior mask clips, not drawn: SimView::setTerrainFn's masked). False: nothing.
// building: the hit is a building's (a drawn instance that is not a hand-placed feature). origin is the near-plane start of the pick ray, eye the camera's position,
// skipped the report's hidden surfaces. instance: the drawn instance hit (-1 none); hitBox: the foliage shell's box when the hit is one.
bool ray_hit(const double origin[3], const double dir[3], double hit[3], bool* building, const double* eye, std::vector<PickSkip>* skipped, float* hitBox, int* instance)
{
	*building = false;
	if (instance) { *instance = -1; }
	if (app.scripted() && kPickWait.get()) { app.collide.wait(); }
#ifdef KCLIENT_DEV_TOOLS
	if (pick_collision_on()) { return ray_hit_collision(origin, dir, hit, building, eye, skipped, hitBox); }
#endif
	const bool prevCuts = app.buildings.levelCuts;
	app.buildings.levelCuts = true;	// the click pick: buildings nobody is in are cut at the floor selector's level (WorldBuildings::levelCuts)
	double terrain[3], groundT = 1e30;
	const bool onTerrain = ray_ground(origin, dir, terrain);
	const bool carved = onTerrain && app.buildings.insideInterior(terrain, nullptr, nullptr, nullptr, true);
	if (onTerrain && !carved) { groundT = std::sqrt(std::pow(terrain[0] - origin[0], 2) + std::pow(terrain[1] - origin[1], 2) + std::pow(terrain[2] - origin[2], 2)); }
	const double reach = std::min(groundT, 20000.0);
	double drawnT = reach;
	uint32_t drawnInst = 0;
	bool drawn = false, foliage = false;
	float foliageBox[6] = {};
	for (double from = 0; from < reach;)
	{
		// the nearest drawn surface of either set: a building / prop / layout piece, or a foliage instance (tree, rock, deposit rock; kenshi-l5dx). One test, no per-kind rule.
		double t = 1e30, tf = 1e30;
		uint32_t i = 0;
		float fbox[6] = {};
		const bool b = app.buildings.drawnPickRay(origin, dir, from, reach, t, i);
#ifdef KCLIENT_DEV_TOOLS
		const auto fc0 = std::chrono::steady_clock::now();
#endif
		const bool f = app.foliageOn && app.foliage.pickRay(origin, dir, from, b ? std::min(reach, t) : reach, tf, fbox);
#ifdef KCLIENT_DEV_TOOLS
		if (app.opt.debug)	// the foliage half's cost (kenshi-l5dx): median / p99 over 300 queries, one line each
		{
			static std::vector<double> us;
			us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - fc0).count());
			if (us.size() >= 300)
			{
				std::sort(us.begin(), us.end());
				fprintf(stderr, "pickfoliage cost: %zu queries, median %.1f us, p99 %.1f us, max %.1f us\n", us.size(), us[us.size() / 2], us[us.size() * 99 / 100], us.back());
				us.clear();
			}
		}
#endif
		if (!b && !f) { break; }
		const bool isFoliage = f && (!b || tf < t);
		const double hitT = isFoliage ? tf : t;
		const double at[3] = { origin[0] + dir[0] * hitT, origin[1] + dir[1] * hitT, origin[2] + dir[2] * hitT };
		std::string why;
		if (!see_through(at, 1e30, &why))
		{
			drawnT = hitT;
			drawnInst = i;
			drawn = true;
			foliage = isFoliage;
			if (isFoliage) { std::copy(fbox, fbox + 6, foliageBox); }
			break;
		}
		if (skipped)
		{
			const std::string n = isFoliage ? "foliage / rock" : app.buildings.nameOfInstance((int)i);
			skipped->push_back({ n.empty() ? "an unnamed drawn instance" : "drawn " + n, { at[0], at[1], at[2] }, why });
		}
		from = hitT + 0.5;
	}
	app.buildings.levelCuts = prevCuts;
	*building = false;
	if (drawn)
	{
		for (int k = 0; k < 3; k++) { hit[k] = origin[k] + dir[k] * drawnT; }
		if (foliage)
		{
			if (hitBox) { std::copy(foliageBox, foliageBox + 6, hitBox); }
			return true;
		}
		const BuildingInstance* bi = app.buildings.instanceAt(drawnInst);
		*building = !(bi && bi->feature);
		if (instance) { *instance = (int)drawnInst; }
		return true;
	}
	if (onTerrain) { std::copy(terrain, terrain + 3, hit); }	// a carved hit nothing else answers: the terrain as before
	return onTerrain;
}

void set_scenery(Pick& p, const float box[6])
{
	uint64_t h = 1469598103934665603ull;
	for (int k = 0; k < 6; k++) { uint32_t b; std::memcpy(&b, &box[k], 4); h = (h ^ b) * 1099511628211ull; }
	p.scenery = h | 1;
	std::copy(box, box + 6, p.sceneryBox);
}

// What the mouse ray hits first: Pick, and its doc, are in pick.hpp (kenshi-k16.2.294.2, step 0 of the control-hints design); pick_at below fills it.

// Label text: the UI style's tag font (ui::drawTag's default) when the UI is up, else the debug font
const ui::Font* label_font() { return app.uiOn ? ui::fonts::get("Kenshi_StandardFont_Medium") : nullptr; }
float label_text_width(const std::string& text)
{
	const ui::Font* f = label_font();
	return f ? f->width(text) : 8.0f * text_scale() * text.size();
}
float label_height()
{
	const ui::Font* f = label_font();
	return std::max(kLabelHeight * view::heightf(), (f ? f->lineHeight() : 8.0f * text_scale()) + 2.0f);
}

// The original's 140163b70 on whole-pixel boxes: sort by top then left, then up to 10 passes over every overlapping pair (edges count). Each
// overlap is measured on both axes as |overlap| * 0.5 + 1. Both boxes move that far apart, away from each other, on the axis that needs less;
// the sixth pass always moves sideways. A pass with no overlap ends it. Not re-clamped on screen, as in the original.
void spread_labels(std::vector<ItemLabel>& labels)
{
	struct Box { int l, t, r, b; };
	std::vector<Box> box;
	for (const ItemLabel& l : labels) { box.push_back({ (int)l.x, (int)l.y, (int)(l.x + l.w), (int)(l.y + l.h) }); }
	std::vector<size_t> order(labels.size());
	for (size_t i = 0; i < order.size(); i++) { order[i] = i; }
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return box[a].t != box[b].t ? box[a].t < box[b].t : box[a].l < box[b].l; });
	for (int pass = 10; pass > 0; pass--)
	{
		bool moved = false;
		for (const size_t i : order)
		{
			for (const size_t j : order)
			{
				Box& a = box[i];
				Box& b = box[j];
				if (i == j || b.l > a.r || a.l > b.r || b.t > a.b || a.t > b.b) { continue; }
				const int dx = (int)(std::abs(std::min(a.r, b.r) - std::max(a.l, b.l)) * 0.5f + 1.0f);
				const int dy = (int)(std::abs(std::min(a.b, b.b) - std::max(a.t, b.t)) * 0.5f + 1.0f);
				if (dy < dx && pass != 5)
				{
					const int d = b.t < a.t ? -dy : dy;
					a.t -= d; a.b -= d; b.t += d; b.b += d;
				} else {
					const int d = b.l < a.l ? -dx : dx;
					a.l -= d; a.r -= d; b.l += d; b.r += d;
				}
				moved = true;
			}
		}
		if (!moved) { break; }
	}
	for (size_t i = 0; i < labels.size(); i++) { labels[i].x = (float)box[i].l; labels[i].y = (float)box[i].t; }
}

std::vector<ItemLabel> item_labels(bool all, const float* cam)	// cam: the mvp to project with (record_ui: the packet's); null: build_mvp
{
	std::vector<ItemLabel> out;
	if (!app.sim.active()) { return out; }
	float mvp[16];
	if (cam) { std::copy(cam, cam + 16, mvp); }
	else { build_mvp(mvp, 1.0f, 1000.0f); }
	for (const auto& [id, th] : app.sim.things())
	{
		const bool producer = th.kind == simlink::Interactable::kProducer;	// labelled only while hovered (MINE)
		if ((th.kind != simlink::Interactable::kItem && th.kind != simlink::Interactable::kContainer && !producer) || ((!all || producer) && id != app.hoverThing)) { continue; }
		const double thY = producer ? th.pos[1] : app.sim.thingY(id, th);	// items and containers: the drawn ground (k16.2.412.4)
		if (const double at[3] = { th.pos[0], thY, th.pos[2] }; cut_hidden(at)) { continue; }	// on a floor the cut hides
		const double lift = producer ? kProducerLabelLift : th.kind == simlink::Interactable::kContainer ? kContainerLabelLift : kLabelLift;
		const float p[3] = { (float)(th.pos[0] - app.pos[0]), (float)(thY + lift - app.pos[1]), (float)(th.pos[2] - app.pos[2]) };
		if (all && id != app.hoverThing && std::hypot(p[0], p[1], p[2]) > kLabelRange) { continue; }
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
		if (c[3] < 1.0f) { continue; }
		std::string text = app.sim.nameOf(producer ? th.output : th.record, app.chars);	// a producer: its output and fill, "Iron Ore 3/5" (MINE)
		if (producer) { text += " " + std::to_string(th.count) + "/" + std::to_string(th.cap); }
		else if (th.count > 1) { text += " (" + std::to_string(th.count) + ")"; }
		const float w = label_text_width(text) + kLabelPad * view::heightf() / 720.0f, h = label_height();
		const float sx = (c[0] / c[3] * 0.5f + 0.5f) * view::widthf(), sy = (0.5f - c[1] / c[3] * 0.5f) * view::heightf();
		if (sx < 0 || sx > view::widthf() || sy < 0 || sy > view::heightf()) { continue; }	// the item is off screen: no label (the box alone is clamped)
		const float x = std::clamp(sx - w * 0.5f, 0.0f, std::max(0.0f, view::widthf() - w)), y = std::clamp(sy - h, 0.0f, std::max(0.0f, view::heightf() - h));
		out.push_back({ id, x, y, w, h, std::move(text), std::hypot(p[0], p[1], p[2]) });
	}
	if (all) { spread_labels(out); }
	std::sort(out.begin(), out.end(), [](const ItemLabel& a, const ItemLabel& b) { return a.dist > b.dist; });
	return out;
}

// The item whose shown label is under (mx, my); 0: none
uint64_t label_at(double mx, double my)
{
	const std::vector<ItemLabel> labels = item_labels(labels_shown());
	for (auto l = labels.rbegin(); l != labels.rend(); ++l)
	{
		if (mx >= l->x && mx <= l->x + l->w && my >= l->y && my <= l->y + l->h) { return l->id; }
	}
	return 0;
}

// The click's position test (kenshi-k16.2.259), cheap enough for every thing along the ray: pos is not drawn — in a ghosted occluder, on a sliced
// building's hidden floor or, with levelCuts set around a pick, on any building's floor above the level. WorldBuildings::cutAway is the one test the
// renderer's own cut and fade use, so a click on a thing there passes to what shows behind it.
bool pick_hidden(const double pos[3], std::string* why) { return app.buildings.cutAway(pos, 1e30, why); }

// A thing's own pick box the cut hides goes in the report too (kenshi-k16.2.259: the water tank's prop box on floor 1 over a floor-0
// click): the name from the wire's record, at the box's hit point, with the cut's reason.
void note_thing_skip(std::vector<PickSkip>* skipped, const char* kind, const simlink::Interactable& th, const double at[3], const std::string& why)
{
	if (!skipped) { return; }
	const std::string n = app.sim.nameOf(th.record, app.chars);
	skipped->push_back({ std::string(kind) + " \"" + (n.empty() ? "no record name" : n) + "\"", { at[0], at[1], at[2] }, why });
}


// debug.pick_collision (dev): the old collision-shape pick, for parity comparisons
#ifdef KCLIENT_DEV_TOOLS
const cfg::Bool kPickCollision{ cfg::Group{ "Pick mask" }, "debug.pick_collision", false, "Debug: the click / hover pick as before the drawn-surface rule (collision shapes, per-layer patches), for parity comparisons (dev builds)" };
bool pick_collision_on() { return kPickCollision.get(); }
#endif

// A thing with no drawn mesh has nothing to ray against (kenshi-k16.2.396.5: no sphere fallback): said once per thing in the event log, a bug to file
static void log_meshless_things()
{
	// A thing's prop mesh binds when its instance streams in, seconds after the thing is announced: a thing is named only when it still has none kMeshWait frames
	// after it was first seen (a meshless-at-first-sight check named every layout counter and barrel at the world load).
	constexpr uint64_t kMeshWait = 900;
	static std::map<uint64_t, uint64_t> firstSeen;
	static std::set<uint64_t> told;
	static uint64_t lastCheck = 0;
	if (app.frames - lastCheck < 60) { return; }
	lastCheck = app.frames;
	for (const auto& [id, th] : app.sim.things())
	{
		const bool pickable = th.kind == simlink::Interactable::kItem || th.kind == simlink::Interactable::kContainer || th.kind == simlink::Interactable::kProducer;
		// A foliage deposit (a copper / iron rock: its id is kIdBase + kFoliageIdBase + ..., cpp/include/ksim/economy/producers.hpp) is not a prop: the rock is a foliage
		// instance the foliage layer draws, and the pick matches it to the producer through the rock's shell (ray_hit's foliage case). It never has a prop mesh (kenshi-sagp).
		constexpr uint64_t kFoliageDepositBase = (1ull << 62) + (1ull << 40);
		if (!pickable || told.count(id) || (th.kind == simlink::Interactable::kProducer && id >= kFoliageDepositBase)) { continue; }
		if (app.buildings.hasMesh(id)) { firstSeen.erase(id); continue; }
		const uint64_t first = firstSeen.emplace(id, app.frames).first->second;
		if (app.frames - first < kMeshWait) { continue; }
		told.insert(id);
		evlog::add("pickmesh", 0, 0, "thing %llu \"%s\" has no drawn mesh: it cannot be picked (no sphere fallback)", (unsigned long long)id, app.sim.nameOf(th.record, app.chars).c_str());
	}
}

// What the drawn instance the ray hit is: a door leaf (its door's interactable), a prop (item, container, producer, player's building: the prop the instance draws,
// the nearest to the hit when a layout piece draws several) or the furniture piece a bed's sleeping spot lies in; else a plain building / world part (Ground).
static void classify_drawn(Pick& p, uint32_t inst)
{
	const BuildingInstance* bi = app.buildings.instanceAt(inst);
	if (!bi) { return; }
	if (bi->door)
	{
		const auto it = app.doorPose.find(bi->door);
		if (it != app.doorPose.end() && it->second.thing)
		{
			p.kind = Pick::Door;
			p.id = it->second.thing;
			return;
		}
	}
	std::vector<uint64_t> ids;
	app.buildings.propsOfInstance(inst, ids);
	double bestD = 1e30;
	for (const uint64_t id : ids)
	{
		Pick::Kind kind;
		double c[3];
		if (const simlink::Interactable* th = app.sim.thing(id))
		{
			kind = th->kind == simlink::Interactable::kContainer ? Pick::Container : th->kind == simlink::Interactable::kProducer ? Pick::Producer : th->kind == simlink::Interactable::kItem ? Pick::Item : Pick::Ground;
			if (kind == Pick::Ground) { continue; }
			for (int k = 0; k < 3; k++) { c[k] = th->pos[k]; }
		}
		else if (const auto b = app.sim.decoder().buildings.find(id); b != app.sim.decoder().buildings.end() && app.sim.playerBuilding(b->second))
		{
			p.kind = Pick::Ground;	// the player's building, like a world building: Ground with `building` (k16.2.36.1)
			p.building = true;
			p.id = id;
			return;
		}
		else { continue; }
		const double d = std::pow(c[0] - p.hit[0], 2) + std::pow(c[1] - p.hit[1], 2) + std::pow(c[2] - p.hit[2], 2);
		if (d < bestD) { bestD = d; p.kind = kind; p.id = id; }
	}
	if (p.id) { return; }
	if (bi->parent < 0) { return; }	// only a layout furniture piece is a bed
	// beds (INTERACTABLE kind 5) are layout furniture: the piece holding the sleeping spot is the bed. The piece depends on the spot only, kept while the
	// instance set and the Ready meshes stand.
	static std::unordered_map<uint64_t, int> piece;
	static uint64_t gen = ~0ull;
	if (gen != app.buildings.pickGen()) { piece.clear(); gen = app.buildings.pickGen(); }
	for (const auto& [id, th] : app.sim.things())
	{
		if (th.kind != simlink::Interactable::kBed) { continue; }
		auto it = piece.find(id);
		if (it == piece.end())
		{
			const double at[3] = { th.pos[0], th.pos[1], th.pos[2] };
			it = piece.emplace(id, app.buildings.bedPiece(at, 6)).first;	// 6 u: a bed's sleeping spot can lie at the end of its drawn frame, off the mesh box
		}
		if (it->second == (int)inst) { p.kind = Pick::Bed; p.id = id; return; }
	}
}

// What the mouse ray hits first (kenshi-k16.2.396.5): ONE nearest-wins loop. The world half is ray_hit (the nearest drawn surface, a foliage shell, the terrain);
// the instance it hit says what it is (classify_drawn). Characters are the other half: SimView::pickBody, a capsule per bone of the drawn skeleton (kenshi-1rnr), competing
// with the world by distance, no slop.
Pick pick_at(const double dir[3], std::vector<PickSkip>* skipped)
{
#ifdef KCLIENT_DEV_TOOLS
	if (pick_collision_on()) { return pick_at_collision(dir, skipped); }
#endif
#ifdef KCLIENT_DEV_TOOLS
	const auto whole0 = std::chrono::steady_clock::now();
#endif
	Pick p;
	double from[3];	// the ray starts at the near clip plane, as the original's does (kenshi-k16.2.259, camera_near)
	for (int k = 0; k < 3; k++) { from[k] = app.pos[k] + dir[k] * camera_near(); }
	const bool prevCuts = app.buildings.levelCuts;
	app.buildings.levelCuts = true;	// the characters' cut test below reads it too
	log_meshless_things();
	double best = 1e30;
	float worldBox[6] = {};
	int inst = -1;
	if (ray_hit(from, dir, p.hit, &p.building, app.pos, skipped, worldBox, &inst))
	{
		p.kind = Pick::Ground;
		best = std::sqrt(std::pow(p.hit[0] - from[0], 2) + std::pow(p.hit[1] - from[1], 2) + std::pow(p.hit[2] - from[2], 2));
		if (inst >= 0) { classify_drawn(p, (uint32_t)inst); }
		else if (worldBox[3] > worldBox[0])
		{
			// a deposit is a foliage rock with no placement: the rock's own collision, a hit shape whose footprint holds the producer's origin, or else a hit within
			// kProducerMatch of it
			double near = kProducerMatch * kProducerMatch;
			for (const auto& [id, th] : app.sim.things())
			{
				if (th.kind != simlink::Interactable::kProducer) { continue; }
				const bool own = th.pos[0] >= worldBox[0] && th.pos[0] <= worldBox[3] && th.pos[2] >= worldBox[2] && th.pos[2] <= worldBox[5];
				const double d2 = own ? 0.0 : std::pow(th.pos[0] - p.hit[0], 2) + std::pow(th.pos[2] - p.hit[2], 2);
				if (d2 < near || (own && near > 0))
				{
					near = d2;
					p.kind = Pick::Producer;
					p.id = id;
				}
			}
		}
	}
	// A character the floor cut hides is not drawn (Characters::culled): the ray goes through it (k16.2.259: the draw's own test, so a character that shows
	// still picks). A character is hit by its drawn skeleton's bone capsules.
	const auto charHidden = [](uint64_t id)
	{
		double feet[3];
		float hull[3];
		return app.sim.placed(id, feet, hull) && cut_hidden(feet);
	};
	// the character half of the one nearest-wins test (kenshi-1rnr): a capsule per bone of the drawn skeleton, posed this frame (Characters::pickCapsules)
	const auto capsules = [](uint64_t id, std::vector<Characters::PickCapsule>& out)
	{
		const int index = app.sim.charIndex(id);
		return index < 0 ? 0 : app.chars.pickCapsules((size_t)index, out);
	};
	double t;
#ifdef KCLIENT_DEV_TOOLS
	const auto cost0 = std::chrono::steady_clock::now();
#endif
	const uint64_t bodyId = app.sim.pickBody(from, dir, &t, charHidden, capsules);
#ifdef KCLIENT_DEV_TOOLS
	if (app.opt.debug)	// the character half's cost (kenshi-1rnr): median / p99 over 300 picks, one line each
	{
		static std::vector<double> us;
		us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - cost0).count());
		if (us.size() >= 300)
		{
			std::sort(us.begin(), us.end());
			fprintf(stderr, "pickbody cost: %zu picks, %zu characters drawn, median %.1f us, p99 %.1f us, max %.1f us\n", us.size(), app.sim.characterIds().size(), us[us.size() / 2], us[us.size() * 99 / 100], us.back());
			us.clear();
		}
	}
#endif
	if (bodyId && t <= best)
	{
		p.kind = Pick::Character;
		p.id = bodyId;
		p.building = false;
		for (int k = 0; k < 3; k++) { p.hit[k] = from[k] + dir[k] * t; }
	}
	app.buildings.levelCuts = prevCuts;
	if (p.kind == Pick::Ground && inst < 0 && worldBox[3] > worldBox[0]) { set_scenery(p, worldBox); }
#ifdef KCLIENT_DEV_TOOLS
	if (app.opt.debug)	// the whole pick's cost (kenshi-l5dx, the hover cost): median / p99 over 300 picks, one line each (pickmem is the evlog line at the BVH loads)
	{
		static std::vector<double> us;
		us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - whole0).count());
		if (us.size() >= 300)
		{
			std::sort(us.begin(), us.end());
			fprintf(stderr, "pickat cost: %zu picks, median %.1f us, p99 %.1f us, max %.1f us\n", us.size(), us[us.size() / 2], us[us.size() * 99 / 100], us.back());
			us.clear();
		}
	}
#endif
	return p;
}

// What is under the screen point: an item label (while shown), else the world pick along the ray. skipped (the F12 report): the shapes the ray passed.
Pick pick_screen(double mx, double my, std::vector<PickSkip>* skipped)
{
	if (const int bar = Post::letterboxBar(view::width(), view::height()); my < bar || my >= view::height() - bar) { return {}; }	// the letterbox bars cover the world: nothing there to pick
	if (const uint64_t id = label_at(mx, my))
	{
		Pick p;
		const simlink::Interactable* th = app.sim.thing(id);
		p.kind = !th ? Pick::Item : th->kind == simlink::Interactable::kContainer ? Pick::Container : th->kind == simlink::Interactable::kProducer ? Pick::Producer : Pick::Item;
		p.id = id;
		return p;
	}
	double dir[3];
	mouse_ray(mx, my, dir);
	return pick_at(dir, skipped);
}
CageHover cageHover;

// entries: the MENU_ENTRIES bitset the server sent for this target (bit 0 Talk To; kenshi-k16.2.121), -1 when there is none (a non-character
// pick, or the query's answer did not come): then the rows are offered as before, Talk included.
std::vector<ui::Menu::Item> menu_items(const Pick& p, int entries)
{
	std::vector<ui::Menu::Item> it;
	const auto order = [](const char* what, uint64_t id, const std::function<bool()>& send)
	{
		const bool sent = send();
		printf("sim menu: %s %llu: %s\n", what, (unsigned long long)id, sent ? "sent" : "not sent");
		order_sent(sent);
	};
	const auto title = [](const std::string& text) { return ui::Menu::Item{ text, nullptr, false, true }; };
	const uint64_t id = p.id;
	if (p.kind == Pick::Character)
	{
		const auto ch = app.sim.decoder().chars.find(id);
		const bool down = ch != app.sim.decoder().chars.end() && (ch->second.cur.flags & (simlink::kKO | simlink::kDead));
		const std::string name = ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "";
		it.push_back(title(name.empty() ? "#" + std::to_string(id) : name));
		// The original's rows (kinspect ctxmenu.md, ctx18_static.md): ours Trade / Follow / Bodyguard / Pick Up, others Follow / Bodyguard /
		// Attack (ascending order id). A row whose order cannot be done is left out, never greyed; Trade waits for the server. Bodyguard is
		// a job (76 type 45, plus its Stay close row: SIMLINK JOBS) for the selection but the target.
		const ui::Menu::Item bodyguard{ "Bodyguard", [=] { order("bodyguard", id, [=] { return app.sim.jobAdd(45, id); }); } };
		if (app.sim.isPlayer(id))
		{
			it.push_back({ "Follow", [=] { order("follow", id, [=] { return app.sim.follow(id); }); } });
			if (!down) { it.push_back(bodyguard); }
		}
		else
		{
			if (!down)
			{
				// Talk To only when the server's gate passes (MENU_ENTRIES bit 0, the answer to order 99; SIMLINK MENU_ENTRIES)
				if (entries < 0 || (entries & 1)) { it.push_back({ "Talk", [=] { order("talk", id, [=] { return app.sim.interact(id, 6); }); } }); }
				it.push_back(bodyguard);
			}
			it.push_back({ "Attack", [=] { order("attack", id, [=] { return app.sim.attack(id); }); } });
			// Stealth KO stays offered for every living, standing target while a selected character sneaks (user 2026-11-11: the 2_ shot lists
			// ATTACK TARGET, ATTACK ALL, STEALTH KO 100%; we carry no chance, so no %). The row sends the KO whether or not the target has seen
			// the actor (the server answers: an aware victim aborts, event 50 result 2), so the menu also reaches a target the red fist would not.
			if (!down && sneak_actor(app.sim)) { it.push_back({ "Stealth KO", [=] { order("stealth ko", id, [=] { return app.sim.interact(id, 14); }); } }); }
		}
		if (down) { it.push_back({ "Loot", [=] { order("loot", id, [=] { return open_body(id, false); }); } }); }
		// first aid (needs a kit in the medic's inventory; the server answers otherwise): anyone down, and our own when hurt
		if (down || (app.sim.isPlayer(id) && app.sim.health(id, app.chars) < 1.0f)) { it.push_back({ "First aid", [=] { order("first aid", id, [=] { return app.sim.interact(id, 10); }); } }); }
		// carrying (order 60 action 8 lifts a KO'd / dead character, 13 puts the carried one down; the server picks the carrier for 13)
		const bool carried = ch != app.sim.decoder().chars.end() && ch->second.cur.carriedBy != 0;
		if (carried) { it.push_back({ "Put down", [=] { order("put down", id, [=] { return app.sim.interact(id, 13); }); } }); }
		else if (down) { it.push_back({ "Pick up", [=] { order("pick up", id, [=] { return app.sim.interact(id, 8); }); } }); }
		// the carrier's menu puts its load down too (the carried body rides on the carrier, so a click on it picks the carrier)
		for (const auto& [cid, cc] : app.sim.decoder().chars)
		{
			if (cc.cur.carriedBy != id || !app.sim.isPlayer(id)) { continue; }
			it.push_back({ "Put down", [=] { order("put down", cid, [=] { return app.sim.interact(cid, 13); }); } });
		}
	}
	else if (const simlink::Interactable* t = app.sim.thing(id))
	{
		std::string name = app.sim.nameOf(t->record, app.chars);
		if (t->kind == simlink::Interactable::kItem && t->count > 1) { name += " (" + std::to_string(t->count) + ")"; }
		if (t->kind == simlink::Interactable::kDoor && t->state == simlink::Interactable::kBroken) { name += " (broken)"; }
		it.push_back(title(name));
		if (t->kind == simlink::Interactable::kItem) { it.push_back({ "Pick up", [=] { order("pick up", id, [=] { return app.sim.interact(id, 7); }); } }); }
		else if (t->kind == simlink::Interactable::kProducer)
		{
			it.push_back({ "Mine", [=] { order("use", id, [=] { return use_producer(id, false); }); } });	// MINE wording ("Operate" for a powered one later)
			it.push_back({ "Open", [=] { order("open", id, [=] { return open_container(id, false); }); } });
		}
		else if (t->kind == simlink::Interactable::kContainer)
		{
			if (t->state == simlink::Interactable::kLocked)
			{
				// a locked box: the pick (order 60 action 3, SIMLINK's kind 3 lock); "Take all" stays so the sim answers the loot itself
				it.push_back({ "Pick Lock", [=] { order("lockpick", id, [=] { return app.sim.interact(id, 3); }); } });
				it.push_back({ "Take all", [=] { order("take all", id, [=] { return app.sim.interact(id, 5); }); } });
			}
			else
			{
				it.push_back({ "Open", [=] { order("open", id, [=] { return open_container(id, false); }); } });
				it.push_back({ "Take all", [=] { order("take all", id, [=] { return app.sim.interact(id, 5); }); } });
			}
		}
		else
		{
			ui::Menu::Item open{ "Open", [=] { order("open", id, [=] { return app.sim.interact(id, 1); }); } };
			ui::Menu::Item close{ "Close", [=] { order("close", id, [=] { return app.sim.interact(id, 2); }); } };
			// unlock: the server allows the door's owner faction only; bash: any shut door, until it breaks (state 3)
			ui::Menu::Item unlock{ "Unlock", [=] { order("unlock", id, [=] { return app.sim.interact(id, 11); }); } };
			ui::Menu::Item bash{ "Bash", [=] { order("bash", id, [=] { return app.sim.interact(id, 12); }); } };
			ui::Menu::Item pick{ "Pick Lock", [=] { order("lockpick", id, [=] { return app.sim.interact(id, 3); }); } };
			if (t->state == simlink::Interactable::kClosed) { it.push_back(open); }
			if (t->state == simlink::Interactable::kOpen) { it.push_back(close); }
			if (t->state == simlink::Interactable::kLocked) { it.push_back(unlock); it.push_back(pick); }
			if (t->state == simlink::Interactable::kLocked || t->state == simlink::Interactable::kClosed) { it.push_back(bash); }
		}
	}
	return it;
}

// Order 99 menu_query (SIMLINK MENU_ENTRIES, kenshi-k16.2.121): the context menu on a non-player, standing character asks the server which rows
// its right-click gate allows (bit 0 Talk To) and opens when MENU_ENTRIES (61) answers, at most kMenuQueryWait later. A server too old to know
// order 99 (the send fails), a refused query and a silence all open the menu as before, Talk offered, and say why on the log.
constexpr double kMenuQueryWait = 0.5;	// MINE: about 10 ticks; the reply is one order round trip
PendingMenu menuPending;	// the right-click whose menu is held for its MENU_ENTRIES (one at a time: a new one supersedes it)

// The prisoner's menus side by side (kenshi-0qr shot 10): the cage's first, at the cursor, the character's beside it on the left (the second menu's
// right edge is at x). No default order: every row is a choice. A cage with no row to offer opens the character's menu alone.
void open_pick_menu(const PendingMenu& pm)
{
	std::vector<ui::Menu::Item> items = menu_items(pm.p, pm.entries);
	std::vector<ui::ContextRow> cageRows;
	std::vector<uint8_t> cageActions;
	if (pm.cage)
	{
		const int e = pm.cageEntries;
		if (e < 0 || (e & (1 << 16))) { cageRows.push_back({ "Pick Lock", pm.chance >= 0 ? (int)(std::floor(pm.chance * 1000.0f) * 0.1f) : -1 }); cageActions.push_back(18); }	// the original's int(chance x 1000) x 0.1
		if (e < 0 || (e & (1 << 17))) { cageRows.push_back({ "Use Tools" }); cageActions.push_back(19); }
		if (e < 0 || (e & (1 << 18))) { cageRows.push_back({ "Use Strength" }); cageActions.push_back(20); }
	}
	if (items.size() < 2 && cageRows.empty()) { return; }	// a title and no rows: no menu (the original opens none without an action)
	app.look = false;
	sappcmd::lockMouse(false);
	app.mouseX = pm.x;
	app.mouseY = pm.y;
	pointer::set((float)pm.x, (float)pm.y);
	// items[0] is the title row (menu_items); the rest become ContextRows, picked by index after the menu closes
	std::vector<ui::ContextRow> rows;
	std::vector<std::function<void()>> acts;
	for (size_t i = 1; i < items.size(); i++) { rows.push_back({ items[i].text }); acts.push_back(items[i].action); }
	if (!cageRows.empty())
	{
		const uint64_t key = pm.cage;
		ui::openContextMenu((float)pm.x, (float)pm.y, pm.cageTitle, cageRows, [key, cageActions](int i)
		{
			if (i < 0 || i >= (int)cageActions.size()) { return; }
			const bool sent = app.sim.interact(key, cageActions[i]);
			printf("sim menu: cage %s %llu: %s\n", cageActions[i] == 18 ? "pick lock" : cageActions[i] == 19 ? "use tools" : "use strength", (unsigned long long)key, sent ? "sent" : "not sent");
			order_sent(sent);
		});
	}
	if (!rows.empty()) { ui::openContextMenu((float)pm.x, (float)pm.y, items[0].text, rows, [acts](int i) { if (i >= 0 && i < (int)acts.size() && acts[i]) { acts[i](); } }, !cageRows.empty()); }	// at the cursor (ctx18_static.md)
	printf("sim menu: open at %.0f %.0f%s%s\n", pm.x, pm.y, pm.entries >= 0 ? (pm.entries & 1 ? " with Talk" : " without Talk") : "", cageRows.empty() ? "" : " with the cage's menu");
}

// The pending menu opens once everything it asked has answered
void menu_open_if_ready()
{
	if (!menuPending.on || !menuPending.charDone || !menuPending.cageDone) { return; }
	const PendingMenu pm = menuPending;
	menuPending = {};
	open_pick_menu(pm);
}

// Per frame, after the sim's update (so a MENU_ENTRIES of this frame is in the decoder): the held right-click's answer, refusal or timeout opens
// its menu. A reply for another seq is a stale one (a second right-click superseded it): logged and dropped.
void menu_query_frame()
{
	if (app.opt.debug && app.sim.decoder().cagesSerial != app.cagesSeen)	// the lead's live check of the wire (kenshi-k16.2.105.1)
	{
		app.cagesSeen = app.sim.decoder().cagesSerial;
		std::string ids;
		for (const auto& [id, c] : app.sim.decoder().cages) { ids += " " + std::to_string(id) + "->" + std::to_string(c.cage) + (c.locked ? " locked lvl " : " open lvl ") + std::to_string(c.level); }
		printf("CAGES decoded: %zu%s\n", app.sim.decoder().cages.size(), ids.c_str());
	}
	for (const SimView::MenuEntries& m : app.sim.takeMenuEntries())
	{
		if (cageHover.seq && m.clientSeq == cageHover.seq)	// the hovered cage's pick chance (the cursor's tag)
		{
			cageHover.seq = 0;
			cageHover.chance = m.pickChance;
			printf("menu_entries: cage %llu entries %x chance %.3f (hover)\n", (unsigned long long)m.target, m.entries, m.pickChance);
			continue;
		}
		const bool forChar = menuPending.on && menuPending.seq && m.clientSeq == menuPending.seq;
		const bool forCage = menuPending.on && menuPending.cageSeq && m.clientSeq == menuPending.cageSeq;
		printf("menu_entries: target %llu entries %x%s%s\n", (unsigned long long)m.target, m.entries, forCage ? " chance " : "", forChar || forCage ? "" : " (stale, dropped)");
		if (forChar) { menuPending.charDone = true; menuPending.entries = (int)m.entries; }
		if (forCage) { menuPending.cageDone = true; menuPending.cageEntries = (int)m.entries; menuPending.chance = m.pickChance; }
		menu_open_if_ready();
	}
	for (const SimView::MenuAnswer& a : app.sim.takeMenuAnswers())
	{
		if (!menuPending.on) { continue; }
		const bool forChar = menuPending.seq && a.seq == menuPending.seq, forCage = menuPending.cageSeq && a.seq == menuPending.cageSeq;
		if (!forChar && !forCage) { continue; }
		printf("menu_entries: none (refused: %s), %s offered\n", a.applied ? "applied, no frame" : a.reason.c_str(), forCage ? "all cage rows" : "Talk");
		(forChar ? menuPending.charDone : menuPending.cageDone) = true;
		menu_open_if_ready();
	}
	if (menuPending.on && ui::now() >= menuPending.deadline)
	{
		printf("menu_entries: none (timeout), Talk offered\n");
		menuPending.charDone = menuPending.cageDone = true;
		menu_open_if_ready();
	}
}

// Per frame: RMB held still on a target long enough opens the menu where it went down; the release then picks (ui held mode).
void rmb_hold_menu()
{
	if (app.rmbAt < 0 || !app.look) { app.rmbAt = -1; return; }
	if (app.clickDrag >= 6) { app.rmbAt = -1; return; }	// a look drag
	if (ui::now() - app.rmbAt < kHoldMenu) { return; }
	app.rmbAt = -1;
	const Pick p = pick_screen(app.clickX, app.clickY);
	if (p.kind != Pick::Character && p.kind != Pick::Item && p.kind != Pick::Door && p.kind != Pick::Container && p.kind != Pick::Producer) { return; }
	PendingMenu pm;
	pm.p = p;
	pm.x = app.clickX;
	pm.y = app.clickY;
	pm.deadline = ui::now() + kMenuQueryWait;
	if (p.kind == Pick::Character && !app.sim.isPlayer(p.id))
	{
		const auto ch = app.sim.decoder().chars.find(p.id);
		const bool down = ch != app.sim.decoder().chars.end() && (ch->second.cur.flags & (simlink::kKO | simlink::kDead));
		if (!down)	// a standing non-player: the server's gate decides Talk To (the menu opens on its answer)
		{
			pm.seq = app.sim.menuQuery(p.id);
			if (!pm.seq) { printf("menu_entries: none (not sent), Talk offered\n"); }
		}
	}
	if (const simlink::SimLinkDecoder::Cage* cg = p.kind == Pick::Character ? locked_cage(app.sim, p.id) : nullptr)	// a prisoner: its cage's menu beside its own (kenshi-k16.2.105.1)
	{
		pm.cage = cg->cage;
		pm.cageTitle = "Prisoner Cage";	// the original's title (kenshi-0qr shot 10); CAGES' record names the building the cage stands in (Longhouse), not the cage
			pm.cageSeq = app.sim.menuQuery(cg->cage);
		if (!pm.cageSeq) { printf("menu_entries: none (not sent), all cage rows offered\n"); }
	}
	pm.charDone = !pm.seq;
	pm.cageDone = !pm.cageSeq;
	if (pm.seq || pm.cageSeq) { pm.on = true; menuPending = pm; return; }
	open_pick_menu(pm);
}

// The F12 shot's pick report (kenshi-k16.2.234.1): who owns the cursor shape, which pick primitive was hit and which rule chose the shape. Three lines,
// "owner:", "primitive:" and "rule:", never an empty name (a thing with no record name says why). kenshi-k16.2.259 adds "skipped shape:" lines: each
// collision shape the click ray passed and why (above the floor cut, the camera inside it, faded). Report only: it re-reads the pick's inputs (the same
// rays and records pick_at uses) and never changes a decision.
std::string pick_owner_report(const Pick& p, double mx, double my, const std::vector<dev::PickSkip>& skipped)
{
	char b[512];
	std::string owner, prim;
	const auto fmt = [&](const char* f, auto... args) { snprintf(b, sizeof b, f, args...); return std::string(b); };
	double dir[3];
	mouse_ray(mx, my, dir);
	const auto thingName = [&](const simlink::Interactable* t)
	{
		const std::string n = app.sim.nameOf(t->record, app.chars);
		return n.empty() ? fmt("no record name (record strid %u)", t->record) : n;
	};
	const auto instanceOf = [&](uint64_t id) { return app.buildings.propInstance(id); };
	static const char* const kThing[] = { "?", "door", "item", "container", "producer", "bed" };
	if (p.kind == Pick::None)
	{
		owner = "nothing (the pointer is on a letterbox bar, or no ray hit anything)";
		prim = "none";
	}
	else if (p.kind == Pick::Character)
	{
		const auto ch = app.sim.decoder().chars.find(p.id);
		std::string name = ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "";
		if (name.empty()) { name = "no display name (unnamed character)"; }
		double sp[3] = {}, feet[3] = {};
		float hull[3] = {};
		const bool has = app.sim.simPos(p.id, sp);
		const bool placed = app.sim.placed(p.id, feet, hull);
		owner = fmt("character \"%s\" id %llu %s at %.1f %.1f %.1f flags 0x%x", name.c_str(), (unsigned long long)p.id, app.sim.isPlayer(p.id) ? "(ours)" : "(not ours)",
			has ? sp[0] : 0.0, has ? sp[1] : 0.0, has ? sp[2] : 0.0, ch != app.sim.decoder().chars.end() ? (unsigned)ch->second.cur.flags : 0u);
		prim = placed ? fmt("sim character pick (SimView::pick: the character's hull) feet %.1f %.1f %.1f hull %.1f %.1f %.1f", feet[0], feet[1], feet[2], hull[0], hull[1], hull[2])
			: "sim character pick (SimView::pick); the character has no placed pose";
	}
	else if (const simlink::Interactable* t = p.kind != Pick::Ground ? app.sim.thing(p.id) : nullptr)
	{
		const char* kind = t->kind < 6 ? kThing[t->kind] : "?";
		owner = fmt("%s \"%s\" id %llu at %.1f %.1f %.1f state %d faction %u%s", kind, thingName(t).c_str(), (unsigned long long)p.id, t->pos[0], t->pos[1], t->pos[2], (int)t->state, t->owner,
			p.kind == Pick::Container || p.kind == Pick::Item ? (app.sim.foreign(p.id) ? " (foreign)" : " (ours / nobody's)") : "");
		if (p.kind == Pick::Bed)
		{
			const double at[3] = { t->pos[0], t->pos[1], t->pos[2] };
			const int piece = app.buildings.bedPiece(at);
			owner += fmt(" occupant %llu furniture record %u", (unsigned long long)t->occupant, t->furniture);
			prim = piece >= 0 ? "drawn triangles of the layout furniture piece holding the sleeping spot: " + app.buildings.describeInstance(piece)
				: "no drawn layout piece holds the sleeping spot: nothing to ray against (no sphere fallback)";
		}
		else if (p.kind == Pick::Door)
		{
			const BuildingInstance* leaf = nullptr;
			for (uint32_t i = 0; (leaf = app.buildings.instanceAt(i)) != nullptr; i++)
			{
				if (leaf->door && app.doorPose.count(leaf->door) && app.doorPose[leaf->door].thing == p.id) { prim = "drawn triangles of the door leaf at its drawn pose: " + app.buildings.describeInstance((int)i); break; }
			}
			if (prim.empty()) { prim = "door leaf (no drawn instance found)"; }
		}
		else if (p.kind == Pick::Item)
		{
			const int inst = instanceOf(p.id);
			prim = inst >= 0 ? "drawn triangles of the item's mesh: " + app.buildings.describeInstance(inst) : "the item has no drawn mesh: nothing to ray against (no sphere fallback)";
		}
		else
		{
			const int inst = instanceOf(p.id);
			if (inst >= 0) { prim = "drawn triangles: " + app.buildings.describeInstance(inst); }
			else { prim = "no drawn prop mesh: the rock's own collision shape, or a pick label (item label under the pointer)"; }
		}
	}
	else if (p.kind == Pick::Ground && p.building && p.id)
	{
		const simlink::SimLinkDecoder::Building* pb = app.sim.building(p.id);
		const int inst = instanceOf(p.id);
		std::string name = inst >= 0 ? app.buildings.nameOfInstance(inst) : "";
		if (name.empty()) { name = pb ? app.sim.nameOf(pb->record, app.chars) : ""; }
		if (name.empty()) { name = "no record name: player building"; }
		owner = fmt("player building \"%s\" id %llu at %.1f %.1f %.1f%s", name.c_str(), (unsigned long long)p.id, p.hit[0], p.hit[1], p.hit[2], pb && (pb->flags & simlink::SimLinkDecoder::Building::kBuilt) ? " built" : " not built");
		prim = inst >= 0 ? "drawn triangles: " + app.buildings.describeInstance(inst) : "building shape";
	}
	else if (p.kind == Pick::Ground)
	{
		// the drawn instance whose triangles are at the hit point (the pick's own rule), else a foliage shell's collision, else the terrain
		std::string drawnPrim;
		for (double t0 = 0; t0 < 20000;)
		{
			double t3;
			uint32_t di;
			if (!app.buildings.drawnPickRay(app.pos, dir, t0, 20000, t3, di)) { break; }
			const double at[3] = { app.pos[0] + dir[0] * t3, app.pos[1] + dir[1] * t3, app.pos[2] + dir[2] * t3 };
			if (std::hypot(std::hypot(at[0] - p.hit[0], at[1] - p.hit[1]), at[2] - p.hit[2]) < 1.0) { drawnPrim = "drawn triangles: " + app.buildings.describeInstance((int)di); break; }
			t0 = t3 + 0.5;
		}
		if (drawnPrim.empty() && app.foliageOn)
		{
			double tf;
			float fb[6];
			if (app.foliage.pickRay(app.pos, dir, 0, 20000, tf, fb) && std::hypot(std::hypot(app.pos[0] + dir[0] * tf - p.hit[0], app.pos[1] + dir[1] * tf - p.hit[1]), app.pos[2] + dir[2] * tf - p.hit[2]) < 1.0)
			{
				drawnPrim = fmt("drawn triangles: foliage instance, box %.0f %.0f %.0f - %.0f %.0f %.0f", fb[0], fb[1], fb[2], fb[3], fb[4], fb[5]);
			}
		}
		double from[3] = { app.pos[0], app.pos[1], app.pos[2] }, t2;
		float box[6];
		bool solid = false, bld = false, found = false;
		std::string hn;
		for (int n = 0; n < 16 && !found && app.collide.ray(from, dir, 20000, t2, box, {}, nullptr, &solid, &bld, &hn); n++)
		{
			double at[3];
			for (int k = 0; k < 3; k++) { at[k] = from[k] + dir[k] * t2; from[k] = at[k] + dir[k] * 0.5; }
			if (std::hypot(std::hypot(at[0] - p.hit[0], at[1] - p.hit[1]), at[2] - p.hit[2]) < 1.0) { found = true; }
		}
		if (!drawnPrim.empty())
		{
			owner = fmt("world drawn surface at %.1f %.1f %.1f (no sim id: scenery%s)", p.hit[0], p.hit[1], p.hit[2], p.building ? ", a building or layout part" : "");
			prim = drawnPrim;
		}
		else if (found)
		{
			owner = fmt("world %s \"%s\" at %.1f %.1f %.1f (no sim id: static scenery%s)", bld ? "building shape" : "foliage / rock", hn.empty() ? "no record name: unnamed collision shape" : hn.c_str(), p.hit[0], p.hit[1], p.hit[2], bld ? ", a layout or building placement" : "");
			prim = fmt("collision shape box %.0f %.0f %.0f - %.0f %.0f %.0f%s", box[0], box[1], box[2], box[3], box[4], box[5], solid ? " (solid: stairs / gateway body)" : "");
		}
		else
		{
			owner = fmt("terrain at %.1f %.1f %.1f (no sim id: the heightmap)", p.hit[0], p.hit[1], p.hit[2]);
			prim = fmt("terrain heightmap ray hit, ground y %.1f", app.hm ? app.hm->height(p.hit[0], p.hit[2]) : 0.0);
		}
	}
	else { owner = "unknown pick (no thing with that id)"; prim = "unknown"; }
	std::string passed;	// kenshi-k16.2.259: what the ray passed, so a click that lands somewhere odd says which shape it went through and why
	for (const dev::PickSkip& s : skipped) { passed += "\nskipped shape: \"" + s.name + "\" at " + fmt("%.1f %.1f %.1f", s.at[0], s.at[1], s.at[2]) + ": " + s.why; }
	return "owner: " + owner + "\nprimitive: " + prim + passed + "\nrule: " + (hoverRule.empty() ? "no hover decision made yet this run" : hoverRule) + " (cursor " + app.cursors.current() + ")\n";
}

}
