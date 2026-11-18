#pragma once

// Ogre binary .skeleton reader (SkeletonSerializer v1.10 and v1.80) and the
// keyframe sampling the original applies (Ogre 1.x NodeAnimationTrack, linear
// interpolation, shortest-path nlerp). Shared by the sim (clip lengths, root
// motion) and the client (skinning). Format: docs/formats/OGRE_SKELETON.md.
//
// Queries never allocate: sampling and root motion walk the keyframes and
// the parent chain in place.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kworld {

struct SkelVec3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// Unit quaternion. The file stores x, y, z, w.
struct SkelQuat
{
	float w = 1.0f;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// A node transform as Ogre keeps it: position, orientation, scale.
struct SkelTransform
{
	SkelVec3 pos;
	SkelQuat rot;
	SkelVec3 scale { 1.0f, 1.0f, 1.0f };
};

struct SkeletonBone
{
	std::string name;
	int parent = -1; // bone index, -1 for a root
	SkelTransform bind; // bind pose, relative to the parent
};

// One keyframe of a bone track: an offset from the bind pose
// (pos added, rot post-multiplied, scale multiplied).
struct SkeletonKey
{
	float time = 0.0f; // seconds
	SkelTransform offset;
};

struct SkeletonTrack
{
	int bone = -1;
	std::vector<SkeletonKey> keys; // time order as stored (spacing not uniform)
};

struct SkeletonAnimation
{
	std::string name;
	float length = 0.0f; // seconds
	std::string baseName; // v1.80 additive base animation, usually empty
	float baseTime = 0.0f;
	std::vector<SkeletonTrack> tracks;
	std::vector<int> trackOfBone; // bone index -> track index, -1 when the bone has none

	const SkeletonTrack* track(int bone) const noexcept;
};

// Animation links: further skeleton files whose clips this one borrows.
struct SkeletonLink
{
	std::string file;
	float scale = 1.0f;
};

struct Skeleton
{
	std::string version; // "1.10", "1.80"
	int blendMode = 0; // v1.80 SKELETON_BLENDMODE (0 average, 1 cumulative); 0 when absent
	std::vector<SkeletonBone> bones; // index = bone handle
	std::vector<SkeletonAnimation> animations; // file order
	std::vector<SkeletonLink> links;
	std::vector<int> parentFirst; // bone indices, every parent before its children

	int bone(std::string_view name) const noexcept; // -1 when absent
	const SkeletonAnimation* animation(std::string_view name) const noexcept;
};

// Throws kworld::Error on a truncated, empty or unknown file.
Skeleton parseSkeleton(std::span<const std::byte> bytes, const std::string& name);
Skeleton loadSkeleton(const std::string& path);

// The bone's local transform at time t (bind combined with the keyframe
// offset; the bind pose when the clip has no track for it). t is clamped to
// the first key below it; past the last key it interpolates towards the first
// key at length + its time, as Ogre does for a looping clip. The caller wraps
// or clamps t to [0, length].
SkelTransform sampleLocal(const Skeleton& skeleton, const SkeletonAnimation* animation,
	int bone, float t) noexcept;

// The bone's transform in skeleton (model) space at time t, through its
// parents. animation nullptr = bind pose.
SkelTransform sampleModel(const Skeleton& skeleton, const SkeletonAnimation* animation,
	int bone, float t) noexcept;

// Whole pose: local and model transforms of every bone at time t. Both spans
// must hold bones.size() entries (model may be empty to skip it).
void samplePose(const Skeleton& skeleton, const SkeletonAnimation* animation, float t,
	std::span<SkelTransform> local, std::span<SkelTransform> model) noexcept;

// Model-space motion of a bone from t0 to t1 (no wrap; split a looping span
// at the clip end): translation delta, and the yaw of the rotation delta (the
// twist about model +Y, radians, counter-clockwise seen from above).
struct SkelMotion
{
	SkelVec3 delta;
	float yaw = 0.0f;
};
SkelMotion rootMotion(const Skeleton& skeleton, const SkeletonAnimation& animation,
	int bone, float t0, float t1) noexcept;

// Transform algebra as Ogre's Node applies it (inherit orientation and scale).
SkelVec3 rotate(const SkelQuat& q, const SkelVec3& v) noexcept;
SkelQuat multiply(const SkelQuat& a, const SkelQuat& b) noexcept;
SkelTransform combine(const SkelTransform& parent, const SkelTransform& child) noexcept;
SkelTransform inverse(const SkelTransform& t) noexcept; // exact for uniform scale
// Column-major 4x4 of a transform (rotation x scale, then translation).
void toMatrix(const SkelTransform& t, float out[16]) noexcept;

} // namespace kworld
