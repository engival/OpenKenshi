#include "assets/ogremesh.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ogre
{

namespace
{

enum : uint16_t
{
	M_HEADER = 0x1000, M_MESH = 0x3000, M_SUBMESH = 0x4000, M_SUBMESH_OPERATION = 0x4010,
	M_SUBMESH_BONE_ASSIGNMENT = 0x4100, M_SUBMESH_TEXTURE_ALIAS = 0x4200,
	M_GEOMETRY = 0x5000, M_VERTEX_DECLARATION = 0x5100, M_VERTEX_ELEMENT = 0x5110, M_VERTEX_BUFFER = 0x5200, M_VERTEX_BUFFER_DATA = 0x5210,
	M_MESH_SKELETON_LINK = 0x6000, M_MESH_BONE_ASSIGNMENT = 0x7000, M_MESH_BOUNDS = 0x9000,
	M_POSES = 0xC000, M_POSE = 0xC100, M_POSE_VERTEX = 0xC111,
};

enum { VES_POSITION = 1, VES_NORMAL = 4, VES_DIFFUSE = 5, VES_TEXTURE_COORDINATES = 7, VES_TANGENT = 9 };
enum { VET_FLOAT1 = 0, VET_FLOAT4 = 3, VET_COLOUR = 4, VET_COLOUR_ARGB = 10, VET_COLOUR_ABGR = 11 };

struct Reader
{
	const uint8_t* d;
	size_t n;

	void need(size_t off, size_t len) const
	{
		if (off > n || len > n - off) { throw std::runtime_error("truncated data at offset " + std::to_string(off)); }
	}
	uint16_t u16(size_t o) const { need(o, 2); uint16_t v; memcpy(&v, d + o, 2); return v; }
	uint32_t u32(size_t o) const { need(o, 4); uint32_t v; memcpy(&v, d + o, 4); return v; }
	float f32(size_t o) const { need(o, 4); float v; memcpy(&v, d + o, 4); return v; }
	std::string line(size_t& o) const	// '\n'-terminated string
	{
		const uint8_t* e = (const uint8_t*)memchr(d + o, '\n', n - o);
		if (!e) { throw std::runtime_error("unterminated string"); }
		std::string s((const char*)d + o, e - (d + o));
		o = (size_t)(e - d) + 1;
		return s;
	}
	// Chunk size at o (includes the 6-byte header); throws if it does not fit.
	uint32_t chunkSize(size_t o) const
	{
		const uint32_t sz = u32(o + 2);
		if (sz < 6 || sz > n - o) { throw std::runtime_error("bad chunk " + std::to_string(u16(o)) + " size " + std::to_string(sz)); }
		return sz;
	}
	bool more(size_t o) const { return o + 6 <= n; }
};

struct Element { uint16_t source, type, semantic, offset, index; };

struct Assignment { uint32_t vertex; uint16_t bone; float weight; };

// M_*_BONE_ASSIGNMENT: u32 vertex, u16 bone handle, f32 weight.
Assignment readAssignment(const Reader& r, size_t p)
{
	return { r.u32(p + 6), r.u16(p + 10), r.f32(p + 12) };
}

// Per vertex at most four assignments, as Ogre's _rationaliseBoneAssignments leaves them: the lightest dropped beyond four, the
// rest normalised, kept in file order (the order compileBoneAssignments writes them; the game's skin.hlsl reads only the first 3).
void applyAssignments(Geometry& g, std::vector<Assignment>& as)
{
	if (as.empty()) { return; }
	std::stable_sort(as.begin(), as.end(), [](const Assignment& a, const Assignment& b) { return a.vertex < b.vertex; });
	g.boneIndex.assign((size_t)g.count * 4, 0);
	g.boneWeight.assign((size_t)g.count * 4, 0.0f);
	for (size_t i = 0; i < as.size();)
	{
		size_t j = i;
		while (j < as.size() && as[j].vertex == as[i].vertex) { j++; }
		const uint32_t v = as[i].vertex;
		if (v >= g.count) { throw std::runtime_error("bone assignment for vertex out of range"); }
		std::vector<Assignment> keep(as.begin() + (long)i, as.begin() + (long)j);
		while (keep.size() > 4) { keep.erase(std::min_element(keep.begin(), keep.end(), [](const Assignment& a, const Assignment& b) { return a.weight < b.weight; })); }
		float total = 0.0f;
		for (const Assignment& a : keep) { total += a.weight; }
		for (size_t k = 0; k < keep.size(); k++)
		{
			if (keep[k].bone > 255) { throw std::runtime_error("bone handle above 255"); }
			g.boneIndex[(size_t)v * 4 + k] = (uint8_t)keep[k].bone;
			g.boneWeight[(size_t)v * 4 + k] = total > 0.0f ? keep[k].weight / total : 0.0f;
		}
		i = j;
	}
}

// Components of a vertex element of float or colour type: floats in out[], returns the count (0 = skipped).
int decode(const Reader& r, size_t at, uint16_t type, float out[4])
{
	if (type <= VET_FLOAT4)
	{
		const int c = type + 1;
		for (int i = 0; i < c; i++) { out[i] = r.f32(at + i * 4); }
		return c;
	}
	return 0;
}

Geometry readGeometry(const Reader& r, size_t o, size_t& end)
{
	Geometry g;
	g.count = r.u32(o + 6);
	std::vector<Element> decl;
	struct Buf { uint16_t vsize; size_t data, len; };
	std::vector<std::pair<uint16_t, Buf>> bufs;
	size_t p = o + 10;
	while (r.more(p) && (r.u16(p) == M_VERTEX_DECLARATION || r.u16(p) == M_VERTEX_BUFFER))
	{
		const uint32_t size = r.chunkSize(p);
		if (r.u16(p) == M_VERTEX_DECLARATION)
		{
			size_t q = p + 6;
			while (q + 6 <= p + size && r.u16(q) == M_VERTEX_ELEMENT)
			{
				decl.push_back({ r.u16(q + 6), r.u16(q + 8), r.u16(q + 10), r.u16(q + 12), r.u16(q + 14) });
				q += r.chunkSize(q);
			}
		} else {
			const uint16_t bind = r.u16(p + 6), vsize = r.u16(p + 8);
			const size_t q = p + 10;
			if (q + 6 <= p + size && r.u16(q) == M_VERTEX_BUFFER_DATA)
			{
				const uint32_t s2 = r.chunkSize(q);
				bufs.push_back({ bind, { vsize, q + 6, s2 - 6 } });
			}
		}
		p += size;
	}
	end = p;
	for (const Element& e : decl)
	{
		const auto b = std::find_if(bufs.begin(), bufs.end(), [&](const auto& x) { return x.first == e.source; });
		if (b == bufs.end() || b->second.vsize == 0) { continue; }
		const Buf& buf = b->second;
		if ((size_t)buf.vsize * g.count > buf.len) { throw std::runtime_error("vertex buffer smaller than count * stride"); }
		const bool wantPos = e.semantic == VES_POSITION && e.index == 0 && e.type >= VET_FLOAT1 + 2 && e.type <= VET_FLOAT4;
		const bool wantNrm = e.semantic == VES_NORMAL && e.index == 0 && e.type >= VET_FLOAT1 + 2 && e.type <= VET_FLOAT4;
		const bool wantUv = e.semantic == VES_TEXTURE_COORDINATES && e.index == 0 && e.type >= VET_FLOAT1 + 1 && e.type <= VET_FLOAT4;
		const bool wantTan = e.semantic == VES_TANGENT && e.index == 0 && e.type >= VET_FLOAT1 + 2 && e.type <= VET_FLOAT4;
		const bool wantCol = e.semantic == VES_DIFFUSE && e.index == 0 && (e.type == VET_COLOUR || e.type == VET_COLOUR_ARGB || e.type == VET_COLOUR_ABGR);
		if (!(wantPos || wantNrm || wantUv || wantTan || wantCol)) { continue; }	// unknown elements are skipped by offset/stride
		if (wantTan) { g.tangentComps = e.type + 1; }
		for (uint32_t i = 0; i < g.count; i++)
		{
			const size_t at = buf.data + (size_t)i * buf.vsize + e.offset;
			float v[4];
			if (wantCol)
			{
				r.need(at, 4);
				const uint8_t* c = r.d + at;
				// VET_COLOUR is read as the ARGB dword (bytes B G R A); unverified, no Kenshi mesh with colours has been checked yet.
				if (e.type == VET_COLOUR_ABGR) { g.colour.insert(g.colour.end(), { c[0], c[1], c[2], c[3] }); }
				else { g.colour.insert(g.colour.end(), { c[2], c[1], c[0], c[3] }); }
				continue;
			}
			const int comps = decode(r, at, e.type, v);
			if (wantPos) { g.pos.insert(g.pos.end(), v, v + 3); }
			else if (wantNrm) { g.normal.insert(g.normal.end(), v, v + 3); }
			else if (wantUv) { g.uv.push_back(v[0]); g.uv.push_back(comps > 1 ? v[1] : 0.0f); }
			else { g.tangent.insert(g.tangent.end(), v, v + comps); }
		}
	}
	return g;
}

size_t readSubmesh(const Reader& r, size_t o, Submesh& sm)
{
	size_t p = o + 6;
	sm.material = r.line(p);
	r.need(p, 6);
	sm.shared = r.d[p] != 0;
	const uint32_t count = r.u32(p + 1);
	const bool wide = r.d[p + 5] != 0;
	p += 6;
	r.need(p, (size_t)count * (wide ? 4 : 2));
	sm.indices.resize(count);
	for (uint32_t i = 0; i < count; i++) { sm.indices[i] = wide ? r.u32(p + (size_t)i * 4) : r.u16(p + (size_t)i * 2); }
	p += (size_t)count * (wide ? 4 : 2);
	std::vector<Assignment> as;
	while (r.more(p))
	{
		const uint16_t id = r.u16(p);
		if (id != M_GEOMETRY && id != M_SUBMESH_OPERATION && id != M_SUBMESH_BONE_ASSIGNMENT && id != M_SUBMESH_TEXTURE_ALIAS) { break; }
		if (id == M_GEOMETRY) { sm.geometry = readGeometry(r, p, p); continue; }
		if (id == M_SUBMESH_OPERATION) { sm.operation = r.u16(p + 6); }
		if (id == M_SUBMESH_BONE_ASSIGNMENT) { as.push_back(readAssignment(r, p)); }
		p += r.chunkSize(p);
	}
	if (!sm.shared) { applyAssignments(sm.geometry, as); }
	return p;
}

// M_POSES: M_POSE children (name, u16 target: 0 shared, n = submesh n-1, bool includesNormals in v1.8+), each with M_POSE_VERTEX children
// (u32 vertex, f32 x3 offset, f32 x3 normal when included). Read off human_male.mesh (v1.100).
void readPoses(const Reader& r, size_t at, Mesh& m)
{
	const size_t end = at + r.chunkSize(at);
	for (size_t p = at + 6; p + 6 <= end && r.u16(p) == M_POSE; p += r.chunkSize(p))
	{
		const size_t poseEnd = p + r.chunkSize(p);
		Pose pose;
		size_t q = p + 6;
		pose.name = r.line(q);
		pose.submesh = (int)r.u16(q) - 1;
		q += 2;
		bool normals = false;
		if (m.version != "1.41") { r.need(q, 1); normals = r.d[q] != 0; q++; }
		for (; q + 6 <= poseEnd && r.u16(q) == M_POSE_VERTEX; q += r.chunkSize(q))
		{
			pose.index.push_back(r.u32(q + 6));
			for (int k = 0; k < 3; k++) { pose.offset.push_back(r.f32(q + 10 + k * 4)); }
			for (int k = 0; k < 3 && normals; k++) { pose.normal.push_back(r.f32(q + 22 + k * 4)); }
		}
		m.poses.push_back(std::move(pose));
	}
}

void parse(const Reader& r, Mesh& m)
{
	if (r.n < 8 || r.u16(0) != M_HEADER) { throw std::runtime_error("not an Ogre mesh"); }
	size_t p = 2;
	const std::string ver = r.line(p);
	const size_t a = ver.rfind("_v"), b = ver.rfind(']');
	m.version = a != std::string::npos && b != std::string::npos && b > a + 2 ? ver.substr(a + 2, b - a - 2) : ver;
	while (r.more(p) && r.u16(p) != M_MESH) { p += r.chunkSize(p); }
	if (!r.more(p)) { throw std::runtime_error("no M_MESH chunk"); }
	p += 7;		// M_MESH header + bool skeletallyAnimated
	std::vector<Assignment> shared;
	while (r.more(p))
	{
		const uint16_t id = r.u16(p);
		if (id == M_SUBMESH)
		{
			m.submeshes.emplace_back();
			p = readSubmesh(r, p, m.submeshes.back());
		}
		else if (id == M_GEOMETRY)
		{
			m.shared = readGeometry(r, p, p);
			m.hasShared = true;
		} else {
			const uint32_t sz = r.u32(p + 2);
			if (sz < 6 || sz > r.n - p) { break; }
			if (id == M_MESH_BOUNDS)
			{
				m.hasBounds = true;
				for (int i = 0; i < 3; i++) { m.boundsMin[i] = r.f32(p + 6 + i * 4); m.boundsMax[i] = r.f32(p + 18 + i * 4); }
				m.boundsRadius = r.f32(p + 30);
			}
			else if (id == M_MESH_SKELETON_LINK) { size_t q = p + 6; m.skeleton = r.line(q); }
			else if (id == M_MESH_BONE_ASSIGNMENT) { shared.push_back(readAssignment(r, p)); }
			else if (id == M_POSES) { readPoses(r, p, m); }
			p += sz;
		}
	}
	applyAssignments(m.shared, shared);
	for (const Submesh& s : m.submeshes)
	{
		const Geometry& g = m.geometry(s);
		if (g.pos.size() != (size_t)g.count * 3) { throw std::runtime_error("submesh without float positions"); }
		for (uint32_t i : s.indices) { if (i >= g.count) { throw std::runtime_error("index out of range"); } }
	}
}

}

std::vector<uint32_t> Submesh::triangleList() const
{
	if (operation == kTriangleList) { return indices; }
	std::vector<uint32_t> out;
	if (operation != kTriangleStrip && operation != kTriangleFan) { return out; }
	for (size_t i = 2; i < indices.size(); i++)
	{
		if (operation == kTriangleFan) { out.insert(out.end(), { indices[0], indices[i - 1], indices[i] }); }
		else if (i & 1) { out.insert(out.end(), { indices[i - 1], indices[i - 2], indices[i] }); }
		else { out.insert(out.end(), { indices[i - 2], indices[i - 1], indices[i] }); }
	}
	return out;
}

bool Mesh::vertexBounds(float mn[3], float mx[3]) const
{
	bool any = false;
	for (const Submesh& s : submeshes)
	{
		const Geometry& g = geometry(s);
		for (uint32_t i = 0; i < g.count; i++)
		{
			for (int k = 0; k < 3; k++)
			{
				const float v = g.pos[(size_t)i * 3 + k];
				if (!any || v < mn[k]) { mn[k] = v; }
				if (!any || v > mx[k]) { mx[k] = v; }
			}
			any = true;
		}
	}
	return any;
}

bool load(const std::string& path, Mesh& out, std::string& err)
{
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in) { err = "cannot open " + path; return false; }
	std::vector<uint8_t> file((size_t)in.tellg());
	in.seekg(0);
	in.read((char*)file.data(), (std::streamsize)file.size());
	out = Mesh();
	try { parse({ file.data(), file.size() }, out); }
	catch (const std::exception& e) { err = path + ": " + e.what(); return false; }
	return true;
}

}
