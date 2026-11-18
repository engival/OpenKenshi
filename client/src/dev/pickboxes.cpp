// dump pickboxes (kenshi-k16.2.260.1): see pickboxes.hpp.
#include "dev/pickboxes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "world/worldbuildings.hpp"

namespace dev
{
std::string pickBoxReport(WorldBuildings& wb, uint32_t index, bool (*screen)(const double p[3], float& x, float& y))
{
	char line[1400];
	const BuildingInstance* bp = wb.instanceAt(index);
	if (!bp) { return "box: no such instance\n"; }
	const BuildingInstance& b = *bp;
	const std::string owner = wb.nameOfInstance(b.parent >= 0 ? b.parent : (int)index);	// a layout piece draws its building's name (a furniture instance carries names[0])
	const int floor = wb.instanceFloor(index);
	const int shown = wb.instanceShown(index);
	snprintf(line, sizeof line, "box inst %u \"%s\" owner \"%s\" parent %d at %.1f %.1f %.1f scale %.3f quat %.6f %.6f %.6f %.6f stretch %.3f %.3f %.3f floor %d shown %d parts %u\n",
		index, wb.nameOfInstance((int)index).c_str(), owner.c_str(), (int)b.parent, b.x, b.y, b.z, (double)b.scale, (double)b.qw, (double)b.qx, (double)b.qy, (double)b.qz,
		(double)b.stretch[0], (double)b.stretch[1], (double)b.stretch[2], floor, shown, b.partCount);
	std::string out = line;
	double worstGap = 0, worstFrame = 0;
	int parts = 0;
	bool inside = true;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		WorldBuildings::PartGeometry g;
		if (!wb.partGeometry(index, k, true, g) || g.xyz.empty()) { continue; }
		// the pick volume: the uploaded bounds + the part offset, through the instance transform (instanceRay's frame, applied forward)
		double plo[3] = { 1e30, 1e30, 1e30 }, phi[3] = { -1e30, -1e30, -1e30 };
		for (int c = 0; c < 8; c++)
		{
			const double v[3] = { (c & 1) ? g.bmax[0] : g.bmin[0], (c & 2) ? g.bmax[1] : g.bmin[1], (c & 4) ? g.bmax[2] : g.bmin[2] };
			double p[3];
			wb.partToWorld(index, v, g.off, p);
			for (int a = 0; a < 3; a++) { plo[a] = std::min(plo[a], p[a]); phi[a] = std::max(phi[a], p[a]); }
		}
		// the drawn mesh: every triangle vertex through the same transform
		double dlo[3] = { 1e30, 1e30, 1e30 }, dhi[3] = { -1e30, -1e30, -1e30 }, frame = 0, sum[3] = {};
		const size_t verts = g.xyz.size() / 3;
		for (size_t i = 0; i < verts; i++)
		{
			const double vd[3] = { g.xyz[i * 3], g.xyz[i * 3 + 1], g.xyz[i * 3 + 2] };
			double p[3];
			wb.partToWorld(index, vd, g.off, p);
			for (int a = 0; a < 3; a++)
			{
				dlo[a] = std::min(dlo[a], p[a]); dhi[a] = std::max(dhi[a], p[a]); sum[a] += p[a];
				const double at = (double)(float)vd[a] + g.off[a];	// the mesh frame: the pick box is [bmin + off, bmax + off] there
				frame = std::max(frame, std::max(g.bmin[a] + g.off[a] - at, at - (g.bmax[a] + g.off[a])));
			}
		}
		double gap = 0;
		for (int a = 0; a < 3; a++) { gap = std::max(gap, std::max(std::fabs(plo[a] - dlo[a]), std::fabs(phi[a] - dhi[a]))); }
		bool centre = true;	// the drawn mesh's centre, in world: inside the pick box (a box about the same origin and rotation)
		for (int a = 0; a < 3; a++)
		{
			const double c = sum[a] / (double)verts;
			centre = centre && c >= plo[a] - 1e-3 && c <= phi[a] + 1e-3;
		}
		parts++;
		worstGap = std::max(worstGap, gap);
		worstFrame = std::max(worstFrame, frame);
		inside = inside && frame <= 1e-3 && centre;
		snprintf(line, sizeof line, "box part %u %s verts %zu pick %.1f %.1f %.1f - %.1f %.1f %.1f drawn %.1f %.1f %.1f - %.1f %.1f %.1f gap %.2f frame %.2f centre %d\n",
			k, g.mesh.c_str(), verts, plo[0], plo[1], plo[2], phi[0], phi[1], phi[2], dlo[0], dlo[1], dlo[2], dhi[0], dhi[1], dhi[2], gap, frame, centre ? 1 : 0);
		out += line;
		if (screen)
		{
			// the pick box's 8 corners and the drawn AABB's, projected: the findings shot's two outlines (bit0 = x, bit1 = y, bit2 = z)
			const auto project = [&](const char* tag, const double lo[3], const double hi[3])
			{
				std::string projected = tag;
				for (int c = 0; c < 8; c++)
				{
					const double p[3] = { (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2] };
					float sx = 0, sy = 0;
					if (!screen(p, sx, sy)) { projected += " 0 0"; continue; }
					char t[48];
					snprintf(t, sizeof t, " %.0f %.0f", sx, sy);
					projected += t;
				}
				out += projected + "\n";
			};
			project("box screen", plo, phi);
			project("box drawn", dlo, dhi);
		}
	}
	snprintf(line, sizeof line, "box summary inst %u owner \"%s\" floor %d shown %d parts %d gap %.2f frame %.2f inside %d\n",
		index, owner.c_str(), floor, shown, parts, worstGap, worstFrame, inside ? 1 : 0);
	return out + line;
}
}
