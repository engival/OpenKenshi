// kmeshdump [--resolve INSTALL_ROOT] FILE.mesh... : one line per mesh in tools/ogre_mesh.py's CLI format, then one line per submesh.
// Used for the parity test against ogre_mesh.py (client/README.md).
#include <cstdio>

#include <cstring>

#include "assets/materials.hpp"
#include "assets/ogremesh.hpp"
#include "assets/resources.hpp"

int main(int argc, char** argv)
{
	int bad = 0, first = 1;
	ResourceIndex res;
	MaterialLib mats;
	size_t subs = 0, known = 0, named = 0, real = 0, found = 0;
	const bool resolve = argc > 2 && strcmp(argv[1], "--resolve") == 0;
	if (resolve)
	{
		if (!res.init(argv[2])) { fprintf(stderr, "no resources.cfg under %s\n", argv[2]); return 2; }
		const size_t materials = mats.load(res);
		printf("resources: %zu dirs, %zu files; materials: %zu from %zu scripts\n", res.dirCount(), res.fileCount(), materials, mats.files);
		first = 3;
	}
	for (int i = first; i < argc; i++)
	{
		ogre::Mesh m;
		std::string err;
		if (!ogre::load(argv[i], m, err)) { fprintf(stderr, "%s\n", err.c_str()); bad++; continue; }
		float mn[3] = {}, mx[3] = {};
		const bool hasBounds = m.vertexBounds(mn, mx);
		size_t verts = m.hasShared ? m.shared.count : 0, indices = 0;
		for (const ogre::Submesh& s : m.submeshes) { verts += s.shared ? 0 : s.geometry.count; indices += s.indices.size(); }
		printf("%s: v%s submeshes=%zu verts=%zu tris=%zu", argv[i], m.version.c_str(), m.submeshes.size(), verts, indices / 3);
		if (hasBounds) { printf(" bounds [%.1f, %.1f, %.1f] .. [%.1f, %.1f, %.1f]", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]); }
		else { printf(" bounds None"); }
		printf("\n");
		for (size_t k = 0; k < m.submeshes.size(); k++)
		{
			const ogre::Submesh& s = m.submeshes[k];
			const ogre::Geometry& g = m.geometry(s);
			printf("  sub %zu material=%s shared=%d op=%d verts=%u indices=%zu normals=%d uv=%d tangents=%d colours=%d", k, s.material.c_str(),
				s.shared, s.operation, g.count, s.indices.size(), !g.normal.empty(), !g.uv.empty(), g.tangentComps, !g.colour.empty());
			if (resolve)
			{
				const std::string tex = mats.diffuse(s.material);
				subs++;
				known += mats.known(s.material);
				named += !tex.empty();
				real += !MaterialLib::placeholder(tex);
				found += !MaterialLib::placeholder(tex) && !res.find(tex).empty();
				printf(" tex=%s", tex.empty() ? "-" : tex.c_str());
			}
			if (!g.boneIndex.empty())
			{
				int maxBone = 0;
				for (uint8_t b : g.boneIndex) { maxBone = std::max(maxBone, (int)b); }
				printf(" skinned max-bone=%d", maxBone);
			}
			if (!g.uv.empty())
			{
				float lo[2] = { g.uv[0], g.uv[1] }, hi[2] = { g.uv[0], g.uv[1] };
				for (size_t j = 0; j < g.uv.size(); j++) { lo[j & 1] = std::min(lo[j & 1], g.uv[j]); hi[j & 1] = std::max(hi[j & 1], g.uv[j]); }
				printf(" uv [%.2f, %.2f] .. [%.2f, %.2f]", lo[0], lo[1], hi[0], hi[1]);
			}
			if (!g.colour.empty())	// vertex colour range, RGBA 0-255
			{
				uint8_t lo[4] = { 255, 255, 255, 255 }, hi[4] = {};
				for (size_t j = 0; j < g.colour.size(); j++) { lo[j & 3] = std::min(lo[j & 3], g.colour[j]); hi[j & 3] = std::max(hi[j & 3], g.colour[j]); }
				printf(" colour [%u %u %u %u] .. [%u %u %u %u]", lo[0], lo[1], lo[2], lo[3], hi[0], hi[1], hi[2], hi[3]);
			}
			printf("\n");
		}
		if (m.hasBounds) { printf("  chunk bounds [%.1f, %.1f, %.1f] .. [%.1f, %.1f, %.1f] radius %.1f\n", m.boundsMin[0], m.boundsMin[1], m.boundsMin[2], m.boundsMax[0], m.boundsMax[1], m.boundsMax[2], m.boundsRadius); }
		if (!m.skeleton.empty()) { printf("  skeleton %s\n", m.skeleton.c_str()); }
		for (const ogre::Pose& p : m.poses) { printf("  pose %s submesh %d verts %zu%s\n", p.name.c_str(), p.submesh, p.index.size(), p.normal.empty() ? "" : " normals"); }
	}
	if (resolve) { printf("submeshes %zu: material in a script %zu, diffuse texture named %zu, non-placeholder %zu, file found %zu\n", subs, known, named, real, found); }
	return bad ? 1 : 0;
}
