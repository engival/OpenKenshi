#pragma once
// File-local helpers that the WorldBuildings member-function files share (worldbuildings.cpp, wbload.cpp, wbfloors.cpp, wbdraw.cpp).

#include "world/worldbuildings.hpp"

#include <algorithm>
#include <cmath>

#include "app/config.hpp"

namespace wbi
{

inline constexpr size_t kInstanceFloats = 12;	// position + scale, rotation quaternion, per-axis stretch
inline constexpr size_t kInstanceBytes = kInstanceFloats * 4;
inline constexpr int kVertexFloats = 13;	// MeshCpu::verts
inline constexpr int kVertexBytes = kVertexFloats * 4;
inline constexpr size_t kMaxInstances = 1 << 19;	// 16 MiB transient buffer
inline const cfg::Bool kOpenWindows{ cfg::Group{ "Indoor lighting (experimental)" }, "render.open_windows", false,
	"Experimental: leave out the small detached quads a building mesh closes its windows and openings with (textured from a dark patch of the atlas: the Storm House's four window barriers, the Watchtower's and Shack's alike), so the slits are see-through and the light bake sees the sky through them; default off" };
inline constexpr double kInteriorSlack = 600.0;	// world units added to the interior radius when testing a building's origin: its parts reach out this far

// Authoring-frame vector -> mesh (Ogre, y-up) frame. Frame 1 is Z-up: (x, y, z) -> (x, z, -y).
inline void toMesh(int frame, const float v[3], double out[3])
{
	if (frame == 0) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; }
	else { out[0] = v[0]; out[1] = v[2]; out[2] = -v[1]; }
}

// Spinner parts (rotation function 1..3: fans, spinners, rotors) turn continuously; 4 aims at the wind with its nested parts
// (BuildingPartDef::aimed; mesh.glsl inst_vs, per instance), 5 (turrets) stays still.
inline bool spinning(const BuildingPartDef& d) { return d.rotFunction >= 1 && d.rotFunction <= 3 && d.rotSpeed != 0; }

inline void rotate(const float q[4], const double v[3], double out[3])	// q = w x y z; v' = v + 2 u x (u x v + w v)
{
	const double ux = q[1], uy = q[2], uz = q[3], w = q[0];
	const double cx = uy * v[2] - uz * v[1] + w * v[0], cy = uz * v[0] - ux * v[2] + w * v[1], cz = ux * v[1] - uy * v[0] + w * v[2];
	out[0] = v[0] + 2 * (uy * cz - uz * cy);
	out[1] = v[1] + 2 * (uz * cx - ux * cz);
	out[2] = v[2] + 2 * (ux * cy - uy * cx);
}

// Camera (or any world point) in a building's local frame, in mesh units before the building scale.
inline void toLocal(const BuildingInstance& b, const double p[3], double out[3])
{
	const double rel[3] = { p[0] - b.x, p[1] - b.y, p[2] - b.z };
	const float conj[4] = { b.qw, -b.qx, -b.qy, -b.qz };
	rotate(conj, rel, out);
	for (int a = 0; a < 3; a++) { out[a] /= b.scale; }
}

// Where the segment s..e (a building's local frame) enters and leaves the box mn..mx, as fractions of it; false: it misses.
inline bool segmentBox(const double s[3], const double e[3], const float mn[3], const float mx[3], double& t0, double& t1)
{
	t0 = 0;
	t1 = 1;
	for (int a = 0; a < 3 && t0 <= t1; a++)
	{
		const double d = e[a] - s[a];
		if (std::abs(d) < 1e-9)
		{
			if (s[a] < mn[a] || s[a] > mx[a]) { return false; }
			continue;
		}
		double u0 = (mn[a] - s[a]) / d, u1 = (mx[a] - s[a]) / d;
		if (u0 > u1) { std::swap(u0, u1); }
		t0 = std::max(t0, u0);
		t1 = std::min(t1, u1);
	}
	return t0 <= t1;
}

}
