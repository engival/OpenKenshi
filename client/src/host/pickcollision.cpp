// debug.pick_collision (kenshi-k16.2.396.5, dev): the click pick as it was before the drawn-surface rule: the collision shapes' nearest hit, then the hand-ordered
// layers (doors, producers, spheres, mesh boxes, bed boxes, character segments) with their patches. Kept only for parity comparisons against the new pick
// (pick.cpp pick_at); never the default. Moved here unchanged except for the names.
#ifdef KCLIENT_DEV_TOOLS
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "app/cpustage.hpp"
#include "input/collisionpick.hpp"
#include "input/pick.hpp"
#include "world/worldbuildings.hpp"
#include "host/app.hpp"
#include "host/pick.hpp"

namespace host
{
constexpr double kPropBoxSlack = 0.5;	// how far past a container's drawn box a click still counted as the container (the sim's collision proxy sits a thousandth nearer than its mesh box, k16.2.239.1); the old pick only

constexpr double kBedReach = 10, kBedLift = 3;
// The click ray's world hit (SIMLINK 13: the picker's bottom layer). Ground layer (kenshi-k16.2.412.3, CLIENT.md "One ground solver, both directions"): the nearer
// of the drawn walkable parts (BuildingPartDef::walkable, the stand solver's set; the cut and the ghost pass what they hide) and the terrain under the carve rule
// (a hit inside a carved interior is the dune the interior mask clips, not drawn: SimView::setTerrainFn's masked). The collision shapes come first only where they are
// nearer than that ground: a wall or an object, passed where they are see_through (a click there is on what shows behind; user 2026-11-03). False: nothing.
// building: the hit is a building's (a shape, or a walkable part). kenshi-k16.2.259: origin is the near-plane start of the pick ray, eye the camera's position
// (ray_shapes), skipped the report's shapes.
bool ray_hit_collision(const double origin[3], const double dir[3], double hit[3], bool* building, const double* eye, std::vector<PickSkip>* skipped, float* hitBox)
{
	*building = false;
	if (app.scripted()) { app.collide.wait(); }
	const bool prevCuts = app.buildings.levelCuts;
	app.buildings.levelCuts = true;	// the click pick: buildings nobody is in are cut at the floor selector's level (WorldBuildings::levelCuts)
	double terrain[3], groundT = 1e30;
	const bool onTerrain = ray_ground(origin, dir, terrain);
	const bool carved = onTerrain && app.buildings.insideInterior(terrain, nullptr, nullptr, nullptr, true);
	if (onTerrain && !carved) { groundT = std::sqrt(std::pow(terrain[0] - origin[0], 2) + std::pow(terrain[1] - origin[1], 2) + std::pow(terrain[2] - origin[2], 2)); }
	double walkT = 1e30;
	bool walk = false;
	for (double from = 0; from < std::min(groundT, 20000.0);)	// the nearest walkable part the view draws: a hit the cut / ghost hides is passed
	{
		double t;
		if (!app.buildings.walkableRay(origin, dir, from, std::min(groundT, 20000.0), t)) { break; }
		const double at[3] = { origin[0] + dir[0] * t, origin[1] + dir[1] * t, origin[2] + dir[2] * t };
		if (!see_through(at)) { walkT = t; walk = true; break; }
		from = t + 0.5;
	}
	const double far = std::min(walk ? walkT : groundT, 20000.0);
	const bool shape = ray_shapes(origin, dir, far, hit, building, eye, skipped, hitBox);
	app.buildings.levelCuts = prevCuts;
	if (shape) { return true; }
	*building = false;
	if (walk)
	{
		for (int k = 0; k < 3; k++) { hit[k] = origin[k] + dir[k] * walkT; }
		*building = true;
		return true;
	}
	if (onTerrain) { std::copy(terrain, terrain + 3, hit); }	// a carved hit nothing else answers: the terrain as before
	return onTerrain;
}

Pick pick_at_collision(const double dir[3], std::vector<PickSkip>* skipped)
{
	Pick p;
	double from[3];	// the ray starts at the near clip plane, as the original's does (kenshi-k16.2.259, camera_near)
	for (int k = 0; k < 3; k++) { from[k] = app.pos[k] + dir[k] * camera_near(); }
	const bool prevCuts = app.buildings.levelCuts;
	app.buildings.levelCuts = true;	// every layer below skips what the cut hides, not only the world's shapes
	double best = 1e30;
	float worldBox[6] = {};	// the box of the collision shape the world hit is, when it is one (not the terrain)
	if (ray_hit_collision(from, dir, p.hit, &p.building, app.pos, skipped, worldBox))
	{
		p.kind = Pick::Ground;
		best = std::sqrt(std::pow(p.hit[0] - from[0], 2) + std::pow(p.hit[1] - from[1], 2) + std::pow(p.hit[2] - from[2], 2));
	}
	const double world = best;
	double t;
	// doors: the pose update_doors keeps per door (the drawn leaf's own): the leaf shapes sit where the leaf is drawn, closed in the frame, open
	// (or broken) slid aside, between them mid-travel; a door with no interactable has no pick
	const auto leaf = [&](const kworld::WorldShape& s, double off[3])
	{
		const auto it = app.doorPose.find(s.door);
		if (it == app.doorPose.end() || !it->second.thing) { return false; }
		if (!it->second.exact && it->second.open > 0) { return false; }	// not a box: no leaf normal, left out while open
		for (int k = 0; k < 3; k++) { off[k] = it->second.off[k]; }
		return true;
	};
	if (kworld::DoorKey key; app.collide.ray(from, dir, world, t, nullptr, leaf, &key) && app.doorPose[key].thing)
	{
		const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
		if (!see_through(at))	// a hidden building's doors do not pick
		{
			p.kind = Pick::Door;
			p.id = app.doorPose[key].thing;
			best = t;
			std::copy(at, at + 3, p.hit);
		}
	}
	for (const auto& [id, th] : app.sim.things())	// producers: by their placement's mesh boxes (the original's hover covers the whole mine)
	{
		if (th.kind != simlink::Interactable::kProducer || !app.buildings.propRay(id, from, dir, t) || t > std::min(best, world)) { continue; }
		const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
		std::string why;
		if (pick_hidden(at, &why))	// a producer on a floor the cut hides is not drawn (k16.2.259)
		{
			note_thing_skip(skipped, "producer", th, at, why);
			continue;
		}
		p.kind = Pick::Producer;
		p.id = id;
		best = t;
		std::copy(at, at + 3, p.hit);
	}
	for (const auto& [id, b] : app.sim.decoder().buildings)	// the player's drawn buildings, like a world building's shape: Ground with `building` (k16.2.36.1)
	{
		if (app.buildings.propInstance(id) < 0 || !app.sim.playerBuilding(b)) { continue; }
		if (app.buildings.propRay(id, from, dir, t) && t <= std::min(best, world))
		{
			const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
			if (pick_hidden(at)) { continue; }
			p.kind = Pick::Ground;
			p.building = true;
			p.id = id;
			std::copy(at, at + 3, p.hit);
			best = t;
		}
	}
	// no placement (a deposit is a foliage rock): the rock's own collision, a hit shape whose footprint holds the producer's origin, or else a hit within
	// kProducerMatch of it
	if (float box[6]; p.kind != Pick::Producer && app.collide.ray(from, dir, std::min(best, world) + 1, t, box))
	{
		const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
		double near = kProducerMatch * kProducerMatch;
		if (!pick_hidden(at))
		{
			for (const auto& [id, th] : app.sim.things())
			{
				if (th.kind != simlink::Interactable::kProducer) { continue; }
				const bool own = th.pos[0] >= box[0] && th.pos[0] <= box[3] && th.pos[2] >= box[2] && th.pos[2] <= box[5];
				const double d2 = own ? 0.0 : std::pow(th.pos[0] - at[0], 2) + std::pow(th.pos[2] - at[2], 2);
				if (d2 < near || (own && near > 0))
				{
					near = d2;
					p.kind = Pick::Producer;
					p.id = id;
					best = t;
					std::copy(at, at + 3, p.hit);
				}
			}
		}
	}
	// the item sphere, or a drawn container's mesh box; a thing on a floor the cut hides is not drawn, so the ray goes through it (k16.2.259)
	const auto thingHidden = [&](uint64_t id)
	{
		const simlink::Interactable* th = app.sim.thing(id);
		if (!th) { return true; }
		if (th->kind == simlink::Interactable::kContainer && app.buildings.hasMesh(id)) { return true; }	// a drawn container: its mesh box picks, below
		const double at[3] = { th->pos[0], app.sim.thingY(id, *th), th->pos[2] };
		std::string why;
		if (!pick_hidden(at, &why)) { return false; }
		const bool box = th->kind == simlink::Interactable::kContainer;
		const double reach = box ? SimView::kContainerReach : SimView::kItemReach;	// where the ray would have met it, its own hit point
		const double c[3] = { at[0] - from[0], at[1] + (box ? SimView::kContainerLift : SimView::kItemLift) - from[1], at[2] - from[2] };
		const double along = c[0] * dir[0] + c[1] * dir[1] + c[2] * dir[2];
		const double d2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - along * along;
		double t2 = std::max(0.0, along - std::sqrt(std::max(0.0, reach * reach - d2)));
		const double hit[3] = { from[0] + dir[0] * t2, from[1] + dir[1] * t2, from[2] + dir[2] * t2 };
		note_thing_skip(skipped, box ? "container" : "item", *th, hit, why);
		return true;
	};
	if (const uint64_t id = app.sim.pickItem(from, dir, &t, thingHidden); id && t <= (best < world ? best : world + SimView::kContainerReach))	// a door or producer in front wins; the world only past the pick sphere (an item on a table sits in its collision box)
	{
		const simlink::Interactable* th = app.sim.thing(id);
		p.kind = th && th->kind == simlink::Interactable::kContainer ? Pick::Container : Pick::Item;
		p.id = id;
		best = t;
		for (int k = 0; k < 3; k++) { p.hit[k] = from[k] + dir[k] * t; }
	}
	int backed = 0;	// the world shape's drawn backing, tested on the first container behind it: 0 not yet, 1 none (undrawn proxy), 2 drawn
	for (const auto& [id, th] : app.sim.things())	// drawn containers: by their mesh boxes (a container a layout piece draws: that piece's, any visible part of it)
	{
		if (th.kind != simlink::Interactable::kContainer) { continue; }
		if (!app.buildings.propRay(id, from, dir, t, true))
		{
			// the ray misses the drawn triangles; a hidden container whose box it still crosses is named in the report, as before the refinement
			double tb = 0;
			if (app.buildings.propRay(id, from, dir, tb))
			{
				const double at[3] = { from[0] + dir[0] * tb, from[1] + dir[1] * tb, from[2] + dir[2] * tb };
				std::string why;
				if (pick_hidden(at, &why)) { note_thing_skip(skipped, "container", th, at, why); }
			}
			continue;
		}
		// the click is on the container's own drawn box while the collision proxy the sim bakes for the piece sits just outside it (a counter's
		// face, k16.2.239.1): the box entry is a hair behind the world hit, so the container still picks; any other shape really in front (a wall,
		// a floor edge) puts the hit point outside the box and keeps the click
		double lo[3], hi[3];
		const bool onProp = best == world && app.buildings.propBox(id, lo, hi, kPropBoxSlack) && p.hit[0] >= lo[0] && p.hit[0] <= hi[0] &&
			p.hit[1] >= lo[1] && p.hit[1] <= hi[1] && p.hit[2] >= lo[2] && p.hit[2] <= hi[2];
		// a building's collision shape in front of the container is an undrawn proxy volume unless something is drawn inside it along the ray (Storm House's
		// staircase body: the barrel on the floor behind it lost its whole drawn body, k16.2.239.4.1): the shape holds the click only when a drawn surface backs it
		if (t > best && !onProp && best == world && p.kind == Pick::Ground && p.building && !p.id)
		{
			if (!backed)
			{
				// the span of the ray inside the shape's box, up to the container's hit
				double t0 = 0, t1 = 1e30;
				const double blo[3] = { worldBox[0], worldBox[1], worldBox[2] }, bhi[3] = { worldBox[3], worldBox[4], worldBox[5] };
				for (int a = 0; a < 3; a++)
				{
					const double lo2 = blo[a] - from[a], hi2 = bhi[a] - from[a];
					if (std::abs(dir[a]) < 1e-12) { if (lo2 > 0 || hi2 < 0) { t1 = -1; } continue; }
					t0 = std::max(t0, std::min(lo2 / dir[a], hi2 / dir[a]));
					t1 = std::min(t1, std::max(lo2 / dir[a], hi2 / dir[a]));
				}
				double tb = 0;
				bool drawn = t0 <= t1 && app.buildings.drawnHitIn(from, dir, blo, bhi, std::max(0.0, t0 - 1.0), std::min(t1 + 1.0, t), tb);
				if (drawn)
				{
					const double bat[3] = { from[0] + dir[0] * tb, from[1] + dir[1] * tb, from[2] + dir[2] * tb };
					drawn = !pick_hidden(bat);	// a surface the cut hides is not drawn
				}
				backed = drawn ? 2 : 1;
			}
			if (backed == 1) { best = 1e30; }	// no drawn surface inside the shape: it does not hide the container (nothing nearer held the click: best == world)
		}
		if (t > best && !onProp) { continue; }
		const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
		std::string why;
		if (pick_hidden(at, &why))	// the water tank's box on floor 1 over a floor-0 click (k16.2.259): not drawn, the ray goes through it
		{
			note_thing_skip(skipped, "container", th, at, why);
			continue;
		}
		p.kind = Pick::Container;
		p.id = id;
		best = std::min(best, t);
		std::copy(at, at + 3, p.hit);
	}
	// beds (INTERACTABLE kind 5): the bed's own placed shape, the layout furniture piece holding the sleeping spot (its mesh box, oriented: a click
	// anywhere on the drawn bed picks it, not a sphere over the spot); a sphere only for a bed no drawn piece holds (MINE, k16.2.220)
	double bedAt[3] = {};
	{	// scope of the bed loop
	CPUSTAGE_ACC("pa.beds");	// the bed loop's time (+pa.beds; kenshi-k16.2.205.12.109 found 0.55-0.65 ms at Heft, .111 memoizes bedRay's search)
	for (const auto& [id, th] : app.sim.things())
	{
		if (th.kind != simlink::Interactable::kBed) { continue; }
		const double at[3] = { th.pos[0], th.pos[1], th.pos[2] };
		double entry = 0;
		const int piece = app.buildings.bedRay(at, from, dir, entry);
		bool hit = piece > 0;
		if (piece < 0)	// no drawn piece holds the spot
		{
			const double c[3] = { th.pos[0] - from[0], th.pos[1] + kBedLift - from[1], th.pos[2] - from[2] };
			const double along = c[0] * dir[0] + c[1] * dir[1] + c[2] * dir[2];
			const double perp2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - along * along;
			hit = along > 0 && perp2 < kBedReach * kBedReach;
			entry = along - std::sqrt(std::max(0.0, kBedReach * kBedReach - perp2));
		}
		if (!hit || entry > std::min(best, world + 1)) { continue; }
		// a bed on a floor the cut hides is not drawn: the ray goes through it (Storm House floor 0: the floor-1 beds sat over the stairs, k16.2.233.1).
		// Tested only for a bed that would take the pick: cut_hidden casts a collide ray per bed, most of the loop's time at Heft once bedRay's search
		// was memoized (kenshi-k16.2.205.12.111); a filter, so the pick is the same as testing it first.
		if (cut_hidden(at) || pick_hidden(at)) { continue; }
		p.kind = Pick::Bed;
		p.id = id;
		best = std::max(entry, 0.0);
		for (int k = 0; k < 3; k++) { bedAt[k] = at[k]; p.hit[k] = from[k] + dir[k] * best; }
	}
	}
	// a sleeper lies inside the bed's box, so it is behind the box's near face: the character wins over the bed it lies in. A character the floor cut
	// hides is not drawn (Characters::culled): the ray goes through it (k16.2.259: the draw's own test, so a character that shows still picks).
	const auto charHidden = [](uint64_t id)
	{
		double feet[3];
		float hull[3];
		return app.sim.placed(id, feet, hull) && cut_hidden(feet);
	};
	// a lying / asleep character is hit along its drawn skeleton (head - pelvis - both feet), not the sim's segment: the placed, animated body in the bed (k16.2.223)
	const auto drawnBody = [](uint64_t id, double pts[][3])
	{
		const int index = app.sim.charIndex(id);
		double head[3], pelvis[3], lf[3], rf[3];
		if (index < 0 || !app.chars.boneWorld((size_t)index, "Bip01 Head", head) || !app.chars.boneWorld((size_t)index, "Bip01 Pelvis", pelvis) ||
			!app.chars.boneWorld((size_t)index, "Bip01 L Foot", lf) || !app.chars.boneWorld((size_t)index, "Bip01 R Foot", rf)) { return 0; }
		const double* seg[3][2] = { { head, pelvis }, { pelvis, lf }, { pelvis, rf } };
		for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { pts[2 * j][k] = seg[j][0][k]; pts[2 * j + 1][k] = seg[j][1][k]; } }
		return 3;
	};
	if (const uint64_t id = app.sim.pick(from, dir, &t, charHidden, drawnBody); id)
	{
		bool inBed = false;	// the character lies in the picked bed: its sim position is on the sleeping spot
		if (double cp[3]; p.kind == Pick::Bed && app.sim.simPos(id, cp)) { inBed = std::hypot(cp[0] - bedAt[0], cp[2] - bedAt[2]) < kBedReach && std::fabs(cp[1] - bedAt[1]) < 4; }
		// a character drawn beside the bed (a KO'd body on the floor next to it) is not the bed's: the bed's box reaches 8-11 u above the floor and takes its
		// pixels. Its own drawn triangles decide: the bed holds the click only when the ray meets them before the body (k16.2.224.3)
		bool beside = false;
		if (p.kind == Pick::Bed && !inBed && t > best && t <= world + 3)
		{
			double td = 0;
			beside = app.buildings.bedRay(bedAt, from, dir, td, true) != 1 || td > t;
		}
		if (beside || t <= (inBed ? std::max(best, world) + 2 * kBedReach : std::min(best, world + 3)))
		{
			p.kind = Pick::Character;
			p.id = id;
			for (int k = 0; k < 3; k++) { p.hit[k] = from[k] + dir[k] * t; }
		}
	}
	app.buildings.levelCuts = prevCuts;
	return p;
}

}
#endif
