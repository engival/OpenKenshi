#pragma once
// The Detour query filter every knav search uses: the per-area costs of a TravelFilter, plus the original's flat +5 on the edge
// that enters a door polygon (PATHFINDING_STATIC.md 2.3 / 6.3), which a per-length area cost cannot express.
#include "knav/navmesh.hpp"
#include "knav/travel.hpp"
#include <DetourNavMeshQuery.h>

namespace knav
{

inline bool isDoorArea(unsigned char area) noexcept
{
	return area == static_cast<unsigned char>(Area::DoorOpen) || area == static_cast<unsigned char>(Area::DoorClosed);
}

class TravelQueryFilter : public dtQueryFilter
{
public:
	explicit TravelQueryFilter(const TravelFilter& travel) : doorCrossing_(travel.doorCrossing) { configureFilter(*this, travel); }

	float getCost(const float* pa, const float* pb, const dtPolyRef prevRef, const dtMeshTile* prevTile, const dtPoly* prevPoly,
		const dtPolyRef curRef, const dtMeshTile* curTile, const dtPoly* curPoly, const dtPolyRef nextRef, const dtMeshTile* nextTile,
		const dtPoly* nextPoly) const override
	{
		float cost = dtQueryFilter::getCost(pa, pb, prevRef, prevTile, prevPoly, curRef, curTile, curPoly, nextRef, nextTile, nextPoly);
		if (nextPoly && isDoorArea(nextPoly->getArea()) && !isDoorArea(curPoly->getArea()))
			cost += doorCrossing_;
		return cost;
	}

private:
	float doorCrossing_;
};

} // namespace knav
