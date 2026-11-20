#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>

#include "knav/navmesh.hpp"
#include "knav/route.hpp"
#include "knav/walk.hpp"
#include "net/simlink.hpp"

// The client's copy of the server's navmesh (SIMLINK 12 tile stream, docs/CLIENT.md "Live sim link"): a knav::NavMesh built from the
// NAV_PARAMS / NAV_TILE frames the decoder holds plus the NAV_POLY_FLAGS patches (door state), for previews and prediction only (the
// server alone paths). update() each frame with the patches drained from the decoder (SimView::takeNavPatches).
class ClientNav
{
public:
	enum class Reach { kUnknown, kYes, kNo };

	void update(const simlink::SimLinkDecoder& dec, std::deque<simlink::NavPatch> patches);
	bool ready() const { return mesh_.has_value(); }
	const knav::NavMesh* mesh() const { return mesh_ ? &*mesh_ : nullptr; }
	size_t tiles() const { return held_.size(); }
	const knav::Walker* walker() const { return walker_.get(); }	// for drive prediction; null until the first tiles; replaced on a rebuild
	// The move_to planner's inputs on this mesh (the debug route preview, kenshi-k16.2.331.2): the query, and the connected sets kept per mesh. Null until the first tiles.
	const knav::NavQuery* query() const { return query_.get(); }
	knav::RouteSets& routeSets() const { return routeSets_; }
	// Is there walkable mesh on the vertical line through the point (any floor above or below it, +-5000)? kUnknown when its tile is not
	// streamed (outside the full-detail area). The original's invalidmove test (1403a2750: a two-way Havok ray, centre +-500 Havok units;
	// kinspect findings/cursors.md "Wall hover").
	Reach walkable(const double at[3]) const;
	Reach surface(const double at[3]) const;	// walkable mesh right at the point: over it in xz, within 2 in y (a ray meeting the floor)
	// kenshi-k16.2.412.3: the navmesh point nearest a drawn click hit, within halfXZ / halfY of it (the floor the hit implies: a small halfY keeps it on its own level).
	// kYes: out is that point; kNo: no polygon in reach; kUnknown: the tile is not streamed (the caller sends the hit as it is).
	// The window is the client's own and is NOT the server's goal snap: the server snaps a goal with a height in a box of knav::kClickReach = 340 u in 3D
	// (route.hpp:22; route.cpp:59,62-65 makes the extents {reach, toY ? reach : 1e5, reach}), so a point already on the mesh snaps to itself whatever the window
	// is, and the window's only job is to land the drawn hit exactly on the surface it hit. Keeping y tight (kSnapY, 4 u) stops the client from picking
	// another floor up to 340 u away (an awning top onto the ground under it) and sending that as the goal: a hit off the mesh goes to the server as drawn,
	// and the server's own snap decides (it also prefers the walker's own set first, route.cpp:72-81).
	static constexpr float kSnapXZ = 6, kSnapY = 4;	// MINE: a floor's drawn top sits well within this of its navmesh polygon; the terrain's can be coarser, and then nothing snaps
	Reach snap(const double at[3], double out[3], float halfXZ = kSnapXZ, float halfY = kSnapY) const;
	// Can a character at `from` walk to `to`? kUnknown when `from` is outside the streamed tiles or the search ran out of nodes.
	Reach reach(const double from[3], const double to[3]) const;

private:
	Reach over(const double at[3], float halfY) const;
	struct Held { uint32_t baked, version; };	// the blob's version, and the version after the patches applied on top
	std::optional<knav::NavMesh> mesh_;
	std::unique_ptr<knav::NavQuery> query_;
	std::unique_ptr<knav::Walker> walker_;
	mutable knav::RouteSets routeSets_;
	std::map<std::array<int32_t, 3>, Held> held_;
	uint32_t meshId_ = 0, generation_ = 0;
	float origin_[2] = {}, tileSize_ = 0;

	void rebuild(const simlink::SimLinkDecoder& dec);
	void patch(const simlink::NavPatch& p);
};
