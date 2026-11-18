// The Hub bar stair (zone 20,32) has no skirt (kenshi-k16.8.41). Recast fills a detail vertex of a polygon whose contour overhangs a
// drop from the nearest span of any layer under it: the slab's edge band (y 1645) over the stair well got a vertex at the stair's
// y 1622, so the band sloped 22 u down across (-51138, 2616) and a walker (ksim_fight_levels seed 6 on the furniture ring branch,
// (-51136.2, 2616.7, y 1628.1) -> (-51135.8, 2616.1, y 1626.0)) stood on it and stepped off its end. Checks:
//  - no surface between y 1626 and 1640 at (-51138, 2616.7) (the slab is at 1645.4, the stair 6 u or more below);
//  - within 60 u of the stair, no polygon whose corners are level (within the climb) has a detail vertex more than the climb off them.

#include "ksim/navigator.hpp"

#include "test_util.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_hub_stair_seam HUB.knav");
		const std::shared_ptr<ksim::MeshNavigator> nav = ksim::MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");
		const auto skirt = nav->walker().surfaceY(-51138.0, 1633.0, 2616.7, 7.0);
		test::require(!skirt, "a surface at y " + std::to_string(skirt.value_or(0.0)) + " under the slab edge at (-51138, 2616.7)");

		const dtNavMesh& dm = nav->mesh().detour();
		int outliers = 0;
		int polys = 0;
		for (int ti = 0; ti < dm.getMaxTiles(); ++ti)
		{
			const dtMeshTile* t = dm.getTile(ti);
			if (!t || !t->header)
				continue;
			for (int pi = 0; pi < t->header->polyCount; ++pi)
			{
				const dtPoly& poly = t->polys[pi];
				if (poly.getType() != DT_POLYTYPE_GROUND)
					continue;
				float lo = 1e30f;
				float hi = -1e30f;
				for (int v = 0; v < poly.vertCount; ++v)
				{
					lo = std::min(lo, t->verts[poly.verts[v] * 3 + 1]);
					hi = std::max(hi, t->verts[poly.verts[v] * 3 + 1]);
				}
				if (hi - lo > 5.0f || std::fabs(t->verts[poly.verts[0] * 3] + 51136.0f) > 60.0f || std::fabs(t->verts[poly.verts[0] * 3 + 2] - 2616.0f) > 60.0f)
					continue;
				const dtPolyDetail& pd = t->detailMeshes[pi];
				++polys;
				for (int k = 0; k < pd.vertCount; ++k)
				{
					const float y = t->detailVerts[(pd.vertBase + k) * 3 + 1];
					if (y < lo - 5.5f || y > hi + 5.5f)
					{
						if (++outliers <= 8)
							std::printf("poly area %d corners y %.1f..%.1f detail vertex y %.1f at (%.1f, %.1f) verts %d\n", poly.getArea(), lo, hi, y, t->detailVerts[(pd.vertBase + k) * 3], t->detailVerts[(pd.vertBase + k) * 3 + 2], poly.vertCount);
						break;
					}
				}
			}
		}
		std::printf("%d polygons, %d level polygons with a detail vertex more than the climb off their corners\n", polys, outliers);
		test::require(outliers == 0, std::to_string(outliers) + " level polygons have a detail vertex more than the climb off their corners");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_hub_stair_seam: FAILED: " << e.what() << "\n";
		return 1;
	}
}
