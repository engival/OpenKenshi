// The surface comparison of knav_compare (knav/compare.hpp): moved out of the tool so the shipped-tile gate test can score every rebaked zone.

#include "knav/compare.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace knav::compare {

// Every triangle sampled at the centres of the cells it covers (xz), then one sample per surface layer: a cell's samples within
// 1 unit of the one below are the same surface (shared edges, fans).
std::vector<Sample> raster(const std::vector<Tri>& tris, const Grid& g)
{
	std::vector<Sample> out;
	for (std::size_t t = 0; t < tris.size(); ++t)
	{
		const Vec& a = tris[t].a;
		const Vec& b = tris[t].b;
		const Vec& c = tris[t].c;
		const int ix0 = std::max(static_cast<int>(std::floor((std::min({a[0], b[0], c[0]}) - g.x0) / g.cell - 0.5)) + 1, 0);
		const int ix1 = std::min(static_cast<int>(std::floor((std::max({a[0], b[0], c[0]}) - g.x0) / g.cell - 0.5)), g.nx - 1);
		const int iz0 = std::max(static_cast<int>(std::floor((std::min({a[2], b[2], c[2]}) - g.z0) / g.cell - 0.5)) + 1, 0);
		const int iz1 = std::min(static_cast<int>(std::floor((std::max({a[2], b[2], c[2]}) - g.z0) / g.cell - 0.5)), g.nz - 1);
		const double det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
		if (ix1 < ix0 || iz1 < iz0 || std::abs(det) < 1e-9)
			continue;
		for (int iz = iz0; iz <= iz1; ++iz)
		{
			const double pz = g.z0 + (iz + 0.5) * g.cell;
			for (int ix = ix0; ix <= ix1; ++ix)
			{
				const double px = g.x0 + (ix + 0.5) * g.cell;
				const double l1 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / det;
				const double l2 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / det;
				const double l3 = 1.0 - l1 - l2;
				if (l1 >= -1e-6 && l2 >= -1e-6 && l3 >= -1e-6)
					out.push_back({static_cast<std::int64_t>(iz) * g.nx + ix, l1 * a[1] + l2 * b[1] + l3 * c[1], static_cast<int>(t)});
			}
		}
	}
	std::sort(out.begin(), out.end(), [](const Sample& l, const Sample& r) { return l.cell != r.cell ? l.cell < r.cell : l.h < r.h; });
	std::vector<Sample> kept;
	for (std::size_t k = 0; k < out.size(); ++k)
	{
		if (k == 0 || out[k].cell != out[k - 1].cell || out[k].h - out[k - 1].h > 1.0)
			kept.push_back(out[k]);
	}
	return kept;
}


// The samples of `cell` in a (cell, h)-sorted list.
std::pair<std::size_t, std::size_t> cellRange(const std::vector<Sample>& s, std::int64_t cell)
{
	const auto lo = std::lower_bound(s.begin(), s.end(), cell, [](const Sample& x, std::int64_t c) { return x.cell < c; });
	const auto hi = std::upper_bound(lo, s.end(), cell, [](std::int64_t c, const Sample& x) { return c < x.cell; });
	return {static_cast<std::size_t>(lo - s.begin()), static_cast<std::size_t>(hi - s.begin())};
}


// For each sample of `a`, the height difference to the nearest-height sample of `b` in its cell (infinity if none).
std::vector<double> match(const std::vector<Sample>& a, const std::vector<Sample>& b)
{
	std::vector<double> out(a.size(), std::numeric_limits<double>::infinity());
	for (std::size_t k = 0; k < a.size(); ++k)
	{
		const auto [lo, hi] = cellRange(b, a[k].cell);
		for (std::size_t m = lo; m < hi; ++m)
		{
			const double dh = a[k].h - b[m].h;
			if (std::abs(dh) < std::abs(out[k]))
				out[k] = dh;
		}
	}
	return out;
}


// The native meshes' triangles (each face fanned) and wall segments.
void nativeGeometry(const std::vector<native::Mesh>& meshes, std::vector<Tri>& tris, std::vector<Wall>& walls)
{
	for (std::size_t m = 0; m < meshes.size(); ++m)
	{
		const native::Mesh& mesh = meshes[m];
		for (const native::Face& face : mesh.faces)
		{
			for (int k = 1; k + 1 < face.numEdges; ++k)
			{
				tris.push_back({mesh.vertices[mesh.edges[face.startEdge].a], mesh.vertices[mesh.edges[face.startEdge + k].a],
					mesh.vertices[mesh.edges[face.startEdge + k + 1].a], static_cast<int>(m)});
			}
		}
		for (std::size_t e = 0; e < mesh.edges.size(); ++e)
		{
			if (mesh.wall(e))
				walls.push_back({mesh.vertices[mesh.edges[e].a], mesh.vertices[mesh.edges[e].b]});
		}
	}
}


// Our mesh's detail triangles (the surface Detour walks), with each polygon's area.
std::vector<Tri> ourTriangles(const NavMesh& mesh)
{
	std::vector<Tri> tris;
	const dtNavMesh& nav = mesh.detour();
	for (int i = 0; i < nav.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = nav.getTile(i);
		if (tile == nullptr || tile->header == nullptr)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.flags == 0) // flags 0: cut by region pruning
				continue;
			const dtPolyDetail& detail = tile->detailMeshes[p];
			const auto vertex = [&](unsigned char index)
			{
				const float* v = index < poly.vertCount ? &tile->verts[poly.verts[index] * 3]
					: &tile->detailVerts[(detail.vertBase + index - poly.vertCount) * 3];
				return Vec{v[0], v[1], v[2]};
			};
			for (int t = 0; t < detail.triCount; ++t)
			{
				const unsigned char* tri = &tile->detailTris[(detail.triBase + t) * 4];
				tris.push_back({vertex(tri[0]), vertex(tri[1]), vertex(tri[2]), poly.getArea()});
			}
		}
	}
	return tris;
}

std::vector<double> wallDistances(const std::vector<Sample>& ns, const Grid& g, const std::vector<Wall>& walls, double tol, double kFar)
{
	std::vector<double> wallDistance(ns.size(), std::numeric_limits<double>::infinity());
	for (const auto& [p, q] : walls)
	{
		const int ix0 = std::max(static_cast<int>((std::min(p[0], q[0]) - kFar - g.x0) / g.cell), 0);
		const int ix1 = std::min(static_cast<int>((std::max(p[0], q[0]) + kFar - g.x0) / g.cell), g.nx - 1);
		const int iz0 = std::max(static_cast<int>((std::min(p[2], q[2]) - kFar - g.z0) / g.cell), 0);
		const int iz1 = std::min(static_cast<int>((std::max(p[2], q[2]) + kFar - g.z0) / g.cell), g.nz - 1);
		const double dx = q[0] - p[0], dz = q[2] - p[2], ll = dx * dx + dz * dz;
		for (int iz = iz0; iz <= iz1; ++iz)
		{
			for (int ix = ix0; ix <= ix1; ++ix)
			{
				const double px = g.x0 + (ix + 0.5) * g.cell, pz = g.z0 + (iz + 0.5) * g.cell;
				const double t = ll > 0.0 ? std::clamp(((px - p[0]) * dx + (pz - p[2]) * dz) / ll, 0.0, 1.0) : 0.0;
				const double d = std::hypot(px - p[0] - t * dx, pz - p[2] - t * dz);
				if (d >= kFar)
					continue;
				const double h = p[1] + t * (q[1] - p[1]);
				const auto [lo, hi] = cellRange(ns, static_cast<std::int64_t>(iz) * g.nx + ix);
				for (std::size_t k = lo; k < hi; ++k)
				{
					if (std::abs(ns[k].h - h) <= tol)
						wallDistance[k] = std::min(wallDistance[k], d);
				}
			}
		}
	}
	return wallDistance;
}

Score score(const std::vector<Tri>& nativeTris, const std::vector<Wall>& walls, const std::vector<bool>& nativeInterior,
	const std::vector<Tri>& ourTris, const Grid& g, double tol, double radius)
{
	const std::vector<Sample> ns = raster(nativeTris, g);
	const std::vector<Sample> os = raster(ourTris, g);
	const std::vector<double> n2o = match(ns, os);
	const std::vector<double> o2n = match(os, ns);
	const std::vector<double> wallDistance = wallDistances(ns, g, walls, tol, radius + 1.0);
	Score out;
	out.cell = g.cell;
	for (std::size_t k = 0; k < ns.size(); ++k)
	{
		if (wallDistance[k] < radius)
			continue;
		const bool hit = std::abs(n2o[k]) <= tol;
		++out.nativeKept;
		out.nativeHit += hit ? 1 : 0;
		if (!hit)
			++(nativeInterior[static_cast<std::size_t>(nativeTris[static_cast<std::size_t>(ns[k].tri)].source)] ? out.missInterior : out.missWorld);
	}
	out.oursTotal = os.size();
	for (std::size_t k = 0; k < os.size(); ++k)
		out.oursNoNative += std::abs(o2n[k]) > tol ? 1 : 0;
	return out;
}

} // namespace knav::compare
