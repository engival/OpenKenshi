#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Ogre binary .mesh reader (MeshSerializer v1.41, v1.8, v1.100): geometry, submeshes, material names, bounds,
// skeleton link, bone assignments, poses. No LOD, edge lists or animation (skipped by chunk id). Format notes: docs/formats/COLLISION_FORMAT.md
// section 2. Chunk lengths of M_MESH/M_SUBMESH are not trusted (they do not cover their children in v1.8/v1.41).
namespace ogre
{

enum { kTriangleList = 4, kTriangleStrip = 5, kTriangleFan = 6 };

struct Geometry
{
	uint32_t count = 0;
	std::vector<float> pos;		// 3 per vertex
	std::vector<float> normal;	// 3 per vertex, empty when absent
	std::vector<float> uv;		// 2 per vertex (texcoord set 0), empty when absent
	std::vector<float> tangent;	// 3 or 4 per vertex (tangentComps), empty when absent
	int tangentComps = 0;
	std::vector<uint8_t> colour;	// RGBA, 4 per vertex (diffuse), empty when absent
	// Skinning, 4 per vertex, empty when the geometry has no bone assignments: skeleton bone handles and weights, the heaviest
	// four kept and normalised (Ogre Mesh::_rationaliseBoneAssignments); unused slots are bone 0 weight 0.
	std::vector<uint8_t> boneIndex;
	std::vector<float> boneWeight;
};

struct Submesh
{
	std::string material;
	bool shared = false;		// uses the mesh's shared geometry
	uint16_t operation = kTriangleList;
	std::vector<uint32_t> indices;
	Geometry geometry;		// dedicated vertices when !shared
	std::vector<uint32_t> triangleList() const;	// indices as a plain triangle list (strips and fans expanded)
};

// A morph target (M_POSE): vertex offsets on one geometry (Ogre applies them as weight x offset).
struct Pose
{
	std::string name;
	int submesh = -1;	// -1 = the shared geometry
	std::vector<uint32_t> index;
	std::vector<float> offset;	// 3 per index
	std::vector<float> normal;	// 3 per index, empty when the pose holds no normals
};

struct Mesh
{
	std::string version;		// "1.100", "1.8", "1.41"
	std::vector<Submesh> submeshes;
	Geometry shared;
	bool hasShared = false;
	bool hasBounds = false;		// the M_MESH_BOUNDS chunk (exporter AABB, may be padded)
	float boundsMin[3] = {}, boundsMax[3] = {}, boundsRadius = 0;
	std::string skeleton;		// skeleton link, empty if none
	std::vector<Pose> poses;

	const Geometry& geometry(const Submesh& s) const { return s.shared ? shared : s.geometry; }
	// AABB of the vertices the submeshes reference. False when there are none.
	bool vertexBounds(float mn[3], float mx[3]) const;
};

// False with err filled in on a malformed or unsupported file.
bool load(const std::string& path, Mesh& out, std::string& err);

}
