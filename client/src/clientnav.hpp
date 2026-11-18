#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>

#include "knav/navmesh.hpp"
#include "knav/walk.hpp"
#include "simlink.hpp"

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
	// Is there walkable mesh on the vertical line through the point (any floor above or below it, +-5000)? kUnknown when its tile is not
	// streamed (outside the full-detail area). The original's invalidmove test (1403a2750: a two-way Havok ray, centre +-500 Havok units;
	// kinspect findings/cursors.md "Wall hover").
	Reach walkable(const double at[3]) const;
	Reach surface(const double at[3]) const;	// walkable mesh right at the point: over it in xz, within 2 in y (a ray meeting the floor)
	// Can a character at `from` walk to `to`? kUnknown when `from` is outside the streamed tiles or the search ran out of nodes.
	Reach reach(const double from[3], const double to[3]) const;

private:
	Reach over(const double at[3], float halfY) const;
	struct Held { uint32_t baked, version; };	// the blob's version, and the version after the patches applied on top
	std::optional<knav::NavMesh> mesh_;
	std::unique_ptr<knav::NavQuery> query_;
	std::unique_ptr<knav::Walker> walker_;
	std::map<std::array<int32_t, 3>, Held> held_;
	uint32_t meshId_ = 0, generation_ = 0;
	float origin_[2] = {}, tileSize_ = 0;

	void rebuild(const simlink::SimLinkDecoder& dec);
	void patch(const simlink::NavPatch& p);
};
