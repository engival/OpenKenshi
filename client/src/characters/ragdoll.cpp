#include "characters/ragdoll.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "kworld/ogreskeleton.hpp"

// See ragdoll.hpp for where the layout comes from. All little-endian and packed (byte fields sit between the floats).
namespace
{
constexpr float kVersionSlack = 1e-6f;	// the game compares the header float against 1.1, 1.2, 1.3, 1.4
constexpr uint32_t kMaxString = 4096;	// a longer length means we lost the stream
constexpr uint32_t kMaxCount = 4096;	// bodies, joints and their tail lists (the shipped ragdolls have <= 14)
constexpr uint32_t kMaxHullVerts = 100000, kMaxHullTriangles = 200000;

// A bounds-checked cursor over the file. Every read either succeeds or sets the error and leaves the cursor put.
class Reader
{
public:
	Reader(const std::vector<uint8_t>& data, const std::string& path) : data_(data), path_(path) {}

	bool ok() const { return ok_; }
	const std::string& error() const { return error_; }
	size_t at() const { return at_; }
	size_t size() const { return data_.size(); }

	bool skip(size_t n, const char* what) { return bytes(n, what) != nullptr; }
	void failAt(const char* what) { fail(what, "impossible count"); }

	uint32_t u32(const char* what) { uint32_t v = 0; read(v, what); return v; }
	int32_t i32(const char* what) { int32_t v = 0; read(v, what); return v; }
	float f32(const char* what) { float v = 0; read(v, what); return v; }
	uint8_t u8(const char* what) { uint8_t v = 0; read(v, what); return v; }

	bool vec3(float out[3], const char* what) { return floats(out, 3, what); }

	bool floats(float* out, size_t n, const char* what)
	{
		const uint8_t* p = bytes(n * sizeof(float), what);
		if (!p) { return false; }
		memcpy(out, p, n * sizeof(float));
		return true;
	}

	bool string(std::string& out, const char* what)
	{
		const uint32_t n = u32(what);
		if (n > kMaxString) { return fail(what, "implausible string length"); }
		const uint8_t* p = bytes(n, what);
		if (!p) { return false; }
		out.assign((const char*)p, n);
		return true;
	}

	const uint8_t* bytes(size_t n, const char* what)
	{
		if (!ok_) { return nullptr; }
		if (at_ + n > data_.size()) { fail(what, "past the end of the file"); return nullptr; }
		const uint8_t* p = data_.data() + at_;
		at_ += n;
		return p;
	}

private:
	template <typename T> bool read(T& v, const char* what)
	{
		const uint8_t* p = bytes(sizeof(T), what);
		if (!p) { return false; }
		memcpy(&v, p, sizeof(T));
		return true;
	}

	bool fail(const char* what, const char* why)
	{
		if (ok_)
		{
			ok_ = false;
			error_ = path_ + ": " + why + " at " + std::to_string(at_) + " (" + what + ", file " + std::to_string(data_.size()) + " bytes)";
		}
		return false;
	}

	const std::vector<uint8_t>& data_;
	std::string path_;
	size_t at_ = 0;
	bool ok_ = true;
	std::string error_;
};

// A display-mesh entry (300 bytes in the game): 3x3 rotation, position, a flags word, 32 unused bytes, the mesh path, a scale.
bool skipEntry(Reader& r, const char* what)
{
	std::string mesh;
	return r.skip(48, what) && r.skip(4, what) && r.skip(32, what) && r.string(mesh, what) && r.skip(12, what);
}

// One collision part (FUN_1401a48c0 in the game's loader): a shape frame, the type and dims, and for a hull its cooked mesh.
bool readPart(Reader& r, ragdoll::Part& part, bool v12)
{
	std::string mesh;
	if (!r.floats(part.rot, 9, "part rotation") || !r.vec3(part.pos, "part position")) { return false; }
	part.type = r.i32("part type");
	if (!r.skip(32, "part unused") || !r.skip(4, "part 0x54") || !r.vec3(part.dims, "part dims")) { return false; }
	if (!r.skip(16, "part flags") || !r.string(mesh, "part mesh")) { return false; }
	if (v12 && !r.string(mesh, "part mesh2")) { return false; }
	r.u8("part 0x80");
	const uint8_t hull = r.u8("part 0x74");
	if (hull || (v12 && part.type == 9))
	{
		if (!r.skip(1, "hull 0x28")) { return false; }
		const uint32_t verts = r.u32("hull vertex count"), tris = r.u32("hull triangle count");
		if (verts > kMaxHullVerts || tris > kMaxHullTriangles) { r.failAt("hull counts"); return false; }
		part.hull.resize((size_t)verts * 3);
		if (!r.floats(part.hull.data(), part.hull.size(), "hull vertices") || !r.skip((size_t)verts * 12, "hull normals") || !r.skip((size_t)tris * 6, "hull indices")) { return false; }
		if (v12)
		{
			if (!r.skip(2, "hull flags")) { return false; }
			return r.skip(4, "part flags");
		}
		return true;	// 1.1 ends a part after its hull
	}
	if (!v12) { return true; }	// 1.1 has no trailing part flags
	return r.skip(4, "part flags");
}

// One body (the body loop of FUN_1401a4ff0). The 1.4 fields are the bind pose the ragdoll starts from; older files have none.
bool readBody(Reader& r, ragdoll::Body& body, float version)
{
	const bool v11 = version + kVersionSlack >= 1.1f, v12 = version + kVersionSlack >= 1.2f, v13 = version + kVersionSlack >= 1.3f, v14 = version + kVersionSlack >= 1.4f;
	const uint32_t nParts = r.u32("body part count"), nEntries = r.u32("body entry count"), nRecs = r.u32("body 0x7c count");
	if (nParts > kMaxCount || nEntries > kMaxCount || nRecs > kMaxCount) { r.failAt("body counts"); return false; }
	if (!r.skip(4, "body 0x214") || !r.vec3(body.pos, "body position") || !r.skip(4, "body 0x30") || !r.skip(4, "body 0x218")) { return false; }
	if (!r.skip(1, "body 0x21c")) { return false; }
	body.density = r.f32("body density");
	body.angularDamping = r.f32("body angular damping");
	body.linearDamping = r.f32("body linear damping");
	if (!r.skip(32, "body unused floats")) { return false; }
	body.mass = r.f32("body mass");
	if (!r.skip(8 + 12 + 36, "body unused matrices")) { return false; }	// q264, v234, m240
	if (v11 && !r.skip(1, "body 0x230")) { return false; }
	if (v12)
	{
		if (!r.string(body.entity, "body entity") || !r.skip(2, "body 0x26c")) { return false; }
	}
	body.solverIterations = v13 ? r.u32("body solver iterations") : 4;
	if (v14)
	{
		if (!r.skip(4, "body 0xb0") || !r.string(body.bone, "body bone")) { return false; }
		float bindPos[3];
		if (!r.vec3(bindPos, "body bind position") || !r.floats(body.rot, 9, "body bind rotation") || !r.skip(1, "body 0x210")) { return false; }
	}
	else
	{
		body.bone = body.entity;
	}
	body.parts.resize(nParts);
	for (ragdoll::Part& part : body.parts) { if (!readPart(r, part, v12)) { return false; } }
	for (uint32_t i = 0; i < nEntries; i++) { if (!skipEntry(r, "body entry")) { return false; } }
	return r.skip((size_t)nRecs * 0x7c, "body 0x7c records");
}

// The 612-byte (312 before 1.2) joint: frame at 12, type at 48, limits at 88/92/96 (degrees), enabled at 104, parent/child at 108/112.
void readJoint(const uint8_t* blob, ragdoll::Joint& joint)
{
	int32_t type = 0;
	memcpy(joint.pos, blob, 12);
	memcpy(joint.frame, blob + 12, 36);
	memcpy(&type, blob + 48, 4);
	memcpy(&joint.limits[0], blob + 88, 4);
	memcpy(&joint.limits[1], blob + 92, 4);
	memcpy(&joint.limits[2], blob + 96, 4);
	uint32_t enabled = 0;
	memcpy(&enabled, blob + 104, 4);
	memcpy(&joint.parent, blob + 108, 4);
	memcpy(&joint.child, blob + 112, 4);
	joint.type = (uint32_t)type;
	joint.limitsEnabled = enabled != 0;
}

bool readFile(const std::string& path, std::vector<uint8_t>& data, std::string& err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) { err = path + ": cannot open"; return false; }
	f.seekg(0, std::ios::end);
	const std::streamoff n = f.tellg();
	if (n <= 0) { err = path + ": empty"; return false; }
	f.seekg(0, std::ios::beg);
	data.resize((size_t)n);
	f.read((char*)data.data(), n);
	if (!f) { err = path + ": cannot read"; return false; }
	return true;
}

} // namespace

bool ragdoll::load(const std::string& path, Def& def, std::string& err)
{
	std::vector<uint8_t> data;
	if (!readFile(path, data, err)) { return false; }
	Reader r(data, path);
	def = Def();
	def.version = r.f32("version");
	const float version = def.version;
	if (version + kVersionSlack < 1.1f) { err = path + ": old format version " + std::to_string(version); return false; }
	const bool v12 = version + kVersionSlack >= 1.2f, v14 = version + kVersionSlack >= 1.4f;
	const uint32_t nBodies = r.u32("body count");
	if (nBodies > kMaxCount) { err = path + ": implausible body count " + std::to_string(nBodies); return false; }
	def.bodies.resize(nBodies);
	for (Body& body : def.bodies) { if (!readBody(r, body, version)) { err = r.error(); return false; } }
	if (v14)
	{
		const uint32_t n = r.u32("global entry count");
		if (n > kMaxCount) { err = path + ": implausible global entry count"; return false; }
		for (uint32_t i = 0; i < n; i++) { if (!skipEntry(r, "global entry")) { err = r.error(); return false; } }
	}
	const uint32_t nJoints = r.u32("joint count");
	if (nJoints > kMaxCount) { err = path + ": implausible joint count"; return false; }
	def.joints.resize(nJoints);
	for (Joint& joint : def.joints)
	{
		const size_t bytes = v12 ? 0x264 : 0x138;
		const uint8_t* blob = r.bytes(bytes, "joint");
		if (!blob) { err = r.error(); return false; }
		readJoint(blob, joint);	// every field we read sits in the 312-byte prefix of the 1.2+ record
	}
	// The tail lists: 0x38c records, more parts and entries, more 0x7c records. Empty in every shipped ragdoll.
	const uint32_t nRecs = r.u32("0x38c count");
	if (nRecs > kMaxCount) { err = path + ": implausible 0x38c count"; return false; }
	if (!r.skip((size_t)nRecs * 0x38c, "0x38c records")) { err = r.error(); return false; }
	const uint32_t nTailParts = r.u32("tail part count");
	if (nTailParts > kMaxCount) { err = path + ": implausible tail part count"; return false; }
	for (uint32_t i = 0; i < nTailParts; i++)
	{
		Part ignored;
		if (!readPart(r, ignored, v12)) { err = r.error(); return false; }
	}
	const uint32_t nTailEntries = r.u32("tail entry count");
	if (nTailEntries > kMaxCount) { err = path + ": implausible tail entry count"; return false; }
	for (uint32_t i = 0; i < nTailEntries; i++) { if (!skipEntry(r, "tail entry")) { err = r.error(); return false; } }
	const uint32_t nTailRecs = r.u32("tail 0x7c count");
	if (nTailRecs > kMaxCount) { err = path + ": implausible tail 0x7c count"; return false; }
	if (!r.skip((size_t)nTailRecs * 0x7c, "tail 0x7c records")) { err = r.error(); return false; }
	def.leftover = r.size() - r.at();
	// The reader must consume the file exactly (the reference parser does for all 86 shipped files); a tail means a field is missing.
	if (def.leftover != 0) { err = path + ": " + std::to_string(def.leftover) + " bytes left after the last record at " + std::to_string(r.at()); return false; }
	// The bind rotation and the bone name are only stored from 1.4: an older file's bone is its entity's name and its frame identity.
	for (Body& body : def.bodies)
	{
		if (body.bone.empty()) { body.bone = body.entity; }
		if (!v14) { for (int i = 0; i < 9; i++) { body.rot[i] = i % 4 == 0 ? 1.0f : 0.0f; } }
	}
	return true;
}

std::string ragdoll::summary(const Def& def)
{
	size_t parts = 0, hinge = 0, spherical = 0, fixed = 0, other = 0;
	for (const Body& body : def.bodies) { parts += body.parts.size(); }
	for (const Joint& joint : def.joints)
	{
		if (joint.type == 1) { hinge++; }
		else if (joint.type == 2) { spherical++; }
		else if (joint.type == 5) { fixed++; }
		else { other++; }
	}
	char line[256];
	snprintf(line, sizeof line, "version %.2f, %zu bodies, %zu parts, %zu joints (%zu hinge, %zu swing-twist, %zu fixed, %zu other)", def.version,
		def.bodies.size(), parts, def.joints.size(), hinge, spherical, fixed, other);
	return line;
}

std::vector<int> ragdoll::boneMap(const Def& def, const kworld::Skeleton& skeleton)
{
	std::vector<int> bones;
	bones.reserve(def.bodies.size());
	for (const Body& body : def.bodies) { bones.push_back(skeleton.bone(body.bone.empty() ? body.entity : body.bone)); }
	return bones;
}
