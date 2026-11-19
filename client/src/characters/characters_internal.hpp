#pragma once
// Private to characters/: the file-local helpers that more than one of characters.cpp / charload.cpp / charanim.cpp / chardraw.cpp use.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "characters/characters.hpp"

namespace characters_internal
{

inline constexpr int kMaxBones = 60;	// skin.hlsl BONES

// anim.action_root (fix 2): the seconds the drawn action path takes to ease onto the wire where the two differ (the sim's navmesh clamp, separation
// pushes, stumble recoil): short enough that a clamped dash stays at the wire, long enough that a push is not a one-frame jump (MINE), and the step
// the drawn body may lead the wire by in the meantime (a walk's step; past it the pose would be drawn through the crowd the sim stopped it in)
inline constexpr float kActEase = 0.15f;

inline std::string folded(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// column-major 4x4: out = a * b
inline void mul(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for (int c = 0; c < 4; c++)
	{
		for (int i = 0; i < 4; i++) { r[c * 4 + i] = a[i] * b[c * 4] + a[4 + i] * b[c * 4 + 1] + a[8 + i] * b[c * 4 + 2] + a[12 + i] * b[c * 4 + 3]; }
	}
	memcpy(out, r, sizeof r);
}

// ANIMATION `delete ...` flags and the bones whose tracks each one destroys (traced: FUN_140869860, by bone name).
struct DeleteGroup { const char* field; std::vector<const char*> bones; };
inline const DeleteGroup kDeleteGroups[] = {
	{ "delete root", { "Bip01" } },
	{ "delete L arm", { "Bip01 L Clavicle", "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand" } },
	{ "delete R arm", { "Bip01 R Clavicle", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand" } },
	{ "delete below waist", { "Bip01", "Bip01 Pelvis", "Bip01 R Thigh", "Bip01 L Thigh", "Bip01 R Calf", "Bip01 L Calf", "Bip01 R Foot", "Bip01 L Foot", "Bip01 R Toe0", "Bip01 L Toe0" } },
	{ "delete spine0", { "Bip01 Spine" } },
	{ "delete body head", { "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2", "Bip01 Neck", "Bip01 Head" } },
	{ "delete weapons", { "Bip01 Prop2", "Bip01 Prop1" } },
	{ "delete tail", { "Bip01 Tail", "Bip01 Tail1", "Bip01 Tail2", "Bip01 Tail3", "Bip01 Tail4" } },
};

inline kworld::SkelVec3 vadd(const kworld::SkelVec3& a, const kworld::SkelVec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline kworld::SkelVec3 vsub(const kworld::SkelVec3& a, const kworld::SkelVec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline kworld::SkelVec3 vmul(const kworld::SkelVec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float vdot(const kworld::SkelVec3& a, const kworld::SkelVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline kworld::SkelVec3 vcross(const kworld::SkelVec3& a, const kworld::SkelVec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float vlen(const kworld::SkelVec3& a) { return std::sqrt(vdot(a, a)); }
inline kworld::SkelQuat qconj(const kworld::SkelQuat& q) { return { q.w, -q.x, -q.y, -q.z }; }
// the shortest rotation taking the unit vector a to the unit vector b
inline kworld::SkelQuat qfromTo(const kworld::SkelVec3& a, const kworld::SkelVec3& b)
{
	const float d = vdot(a, b);
	kworld::SkelQuat q;
	if (d < -0.99999f)
	{
		const kworld::SkelVec3 ax = std::fabs(a.y) < 0.9f ? kworld::SkelVec3{ 0, 1, 0 } : kworld::SkelVec3{ 1, 0, 0 };
		const kworld::SkelVec3 c = vcross(ax, a);
		q = { 0.0f, c.x, c.y, c.z };
	}
	else { q = { 1.0f + d, a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	const float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	return l > 1e-6f ? kworld::SkelQuat{ q.w / l, q.x / l, q.y / l, q.z / l } : kworld::SkelQuat{};
}
// the shortest rotation taking the local orientation a to b as an axis-angle vector (radians), and back: the inertialization offset, its
// velocity and its decay (Characters::inertialize)
inline kworld::SkelVec3 qaxis(const kworld::SkelQuat& a, const kworld::SkelQuat& b)
{
	kworld::SkelQuat d = kworld::multiply(qconj(a), b);
	if (d.w < 0) { d = { -d.w, -d.x, -d.y, -d.z }; }
	const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (s < 1e-7f) { return {}; }
	const float k = 2.0f * std::atan2(s, d.w) / s;
	return { d.x * k, d.y * k, d.z * k };
}
inline kworld::SkelQuat qexp(const kworld::SkelVec3& v)
{
	const float a = vlen(v);
	if (a < 1e-7f) { return {}; }
	const float s = std::sin(0.5f * a) / a;
	return { std::cos(0.5f * a), v.x * s, v.y * s, v.z * s };
}

// The editor's sliders for one gender (defined in charload.cpp)
std::vector<Characters::EditorRange> readRanges(const std::string& path, bool female);

}

using namespace characters_internal;
