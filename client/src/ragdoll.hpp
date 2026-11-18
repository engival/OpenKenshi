#pragma once

// Kenshi's ragdoll physics files (data/ragdoll/*.phs): one Scythe/PhysX 2.8 physics entity as the game dumped it, a list of bodies
// (collision parts) and a list of joints. The layout was decoded by the kinspect session from the game's loader (FUN_1401a4ff0): the
// field-for-field notes are in findings/ragdoll_phs_format.md and its byte-exact reference parser tools/phs_parse.py. This is our own
// C++ reader of that format (no game code), with the same version gates 1.1-1.4.
//
// Frames: body positions are in the rig's model space (they match the Ogre skeleton's bind pose); a body's 3x3 rotation maps model to
// the body's own frame (the inverse of the bind orientation the .skeleton stores). A part's position is an absolute model point (the calf's
// shin part sits at y 3.258, between its body's knee 5.562 and the ankle), its rotation is in model axes: the part in the body's frame is
// (bodyRot * (partPos - bodyPos), bodyRot * partRot). A part's dims are full extents (box), the diameter (sphere) or radius, length,
// radius (capsule; length includes the caps, along the shape's y axis).

#include <cstdint>
#include <string>
#include <vector>

namespace kworld { struct Skeleton; }

namespace ragdoll
{
	struct Part
	{
		int type = 0;	// 3 box, 4 sphere, 5 capsule, 9 convex hull
		float pos[3] = {};
		float rot[9] = {};	// 3x3 rows, model space
		float dims[3] = {};
		std::vector<float> hull;	// type 9 only: vertex triples, shape space (untested: no human ragdoll has one)
	};

	struct Body
	{
		std::string bone;	// the skeleton bone this body is rigged to ("Bip01 L Thigh", ...; older files: empty, use entity)
		std::string entity;
		float pos[3] = {};	// model space
		float rot[9] = {};	// model -> body, 3x3 rows
		float density = 0;	// f220: 0 = the game's default density
		float angularDamping = 0;	// f224, times 10 = the body's angular damping
		float linearDamping = 0;	// f228
		float mass = 0;	// u22c: > 0 overrides the density-derived mass
		uint32_t solverIterations = 4;	// u270; the game clamps it to >= 16
		std::vector<Part> parts;
	};

	struct Joint
	{
		float pos[3] = {};	// model space
		float frame[9] = {};	// 3x3 rows: frame[0..2] = row 0, ... The hinge/twist axis is row 1, the normal/swing axis row 0.
		uint32_t type = 0;	// 1 revolute (hinge), 2 spherical (swing-twist), 5 fixed
		float limits[3] = {};	// degrees: type 2 = twist max, twist min, swing; type 1 = max, min, 0
		bool limitsEnabled = false;
		int parent = -1, child = -1;	// body indices; a root marker has -1
	};

	struct Def
	{
		float version = 0;
		size_t leftover = 0;	// bytes after the last record (0 for every shipped file)
		std::vector<Body> bodies;
		std::vector<Joint> joints;
	};

	// Reads one .phs into def (replacing it). False with err set on a bad file: truncated, an unknown version or a leftover tail.
	bool load(const std::string& path, Def& def, std::string& err);
	// One line for the logs: version, bodies, joints, parts, and the joints a ragdoll will not build.
	std::string summary(const Def& def);

	// body i -> its bone in our Ogre skeleton (-1 when the name is not in it). Bodies are matched by bone name, as the game rigs them.
	std::vector<int> boneMap(const Def& def, const kworld::Skeleton& skeleton);
}
