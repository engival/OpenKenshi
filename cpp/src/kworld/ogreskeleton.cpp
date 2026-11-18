#include "kworld/ogreskeleton.hpp"

#include "kworld/error.hpp"
#include "io.hpp"
#include "le.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kworld {

namespace {

enum : std::uint16_t {
	kHeader = 0x1000,
	kBlendMode = 0x1010,
	kBone = 0x2000,
	kBoneParent = 0x3000,
	kAnimation = 0x4000,
	kAnimationBaseInfo = 0x4010,
	kAnimationTrack = 0x4100,
	kKeyFrame = 0x4110,
	kAnimationLink = 0x5000,
};

constexpr std::size_t kChunkHeader = 6; // u16 id + u32 length

class Reader
{
public:
	Reader(std::span<const std::byte> bytes, const std::string& name)
		: bytes_(bytes), name_(name)
	{
	}

	[[noreturn]] void fail(const std::string& what) const
	{
		throw Error("skeleton " + name_ + ": " + what + " at offset " + std::to_string(pos_));
	}

	void need(std::size_t count) const
	{
		if (bytes_.size() - pos_ < count)
			fail("truncated");
	}

	bool atEnd() const noexcept { return pos_ >= bytes_.size(); }
	std::size_t pos() const noexcept { return pos_; }
	void seek(std::size_t pos) { pos_ = pos; }

	std::uint16_t u16()
	{
		need(2);
		const auto v = detail::readU16(bytes_.data() + pos_);
		pos_ += 2;
		return v;
	}

	std::uint32_t u32()
	{
		need(4);
		const auto v = detail::readU32(bytes_.data() + pos_);
		pos_ += 4;
		return v;
	}

	float f32()
	{
		need(4);
		const float v = detail::readF32(bytes_.data() + pos_);
		pos_ += 4;
		return v;
	}

	SkelVec3 vec3()
	{
		SkelVec3 v;
		v.x = f32();
		v.y = f32();
		v.z = f32();
		return v;
	}

	SkelQuat quat() // stored x, y, z, w
	{
		SkelQuat q;
		q.x = f32();
		q.y = f32();
		q.z = f32();
		q.w = f32();
		return q;
	}

	std::string line() // Ogre strings end with '\n'
	{
		const auto* begin = bytes_.data() + pos_;
		const auto* end = bytes_.data() + bytes_.size();
		const auto* nl = std::find(begin, end, std::byte { '\n' });
		if (nl == end)
			fail("unterminated string");
		std::string out(reinterpret_cast<const char*>(begin), static_cast<std::size_t>(nl - begin));
		pos_ += out.size() + 1;
		return out;
	}

	// Next chunk id without consuming it; 0 at the end.
	std::uint16_t peekId() const
	{
		if (bytes_.size() - pos_ < kChunkHeader)
			return 0;
		return detail::readU16(bytes_.data() + pos_);
	}

private:
	std::span<const std::byte> bytes_;
	std::string name_;
	std::size_t pos_ = 0;
};

// The bone chunk's length field is not trusted: Kenshi's exporter writes it
// without the name (36, or 48 with a scale); Ogre's own serializer includes
// the name. Either form is accepted, anything else is rejected.
void readBone(Reader& r, std::uint32_t length, Skeleton& out)
{
	SkeletonBone bone;
	bone.name = r.line();
	const std::uint32_t named = static_cast<std::uint32_t>(bone.name.size() + 1);
	constexpr std::uint32_t plain = kChunkHeader + 2 + 12 + 16;
	constexpr std::uint32_t scaled = plain + 12;
	bool hasScale = false;
	if (length == scaled || length == scaled + named)
		hasScale = true;
	else if (length != plain && length != plain + named)
		r.fail("bone chunk length " + std::to_string(length));
	const std::uint16_t handle = r.u16();
	bone.bind.pos = r.vec3();
	bone.bind.rot = r.quat();
	if (hasScale)
		bone.bind.scale = r.vec3();
	if (handle >= out.bones.size())
		out.bones.resize(handle + 1u);
	else if (!out.bones[handle].name.empty())
		r.fail("duplicate bone handle " + std::to_string(handle));
	out.bones[handle] = std::move(bone);
}

void readKeyFrame(Reader& r, std::uint32_t length, SkeletonTrack& track)
{
	constexpr std::uint32_t plain = kChunkHeader + 4 + 16 + 12;
	if (length != plain && length != plain + 12)
		r.fail("keyframe chunk length " + std::to_string(length));
	SkeletonKey key;
	key.time = r.f32();
	key.offset.rot = r.quat();
	key.offset.pos = r.vec3();
	if (length == plain + 12)
		key.offset.scale = r.vec3();
	track.keys.push_back(key);
}

void readAnimation(Reader& r, Skeleton& out)
{
	SkeletonAnimation anim;
	anim.name = r.line();
	anim.length = r.f32();
	anim.trackOfBone.assign(out.bones.size(), -1);
	for (;;)
	{
		const std::uint16_t id = r.peekId();
		if (id != kAnimationBaseInfo && id != kAnimationTrack)
			break;
		const std::size_t start = r.pos();
		r.u16();
		const std::uint32_t length = r.u32();
		if (id == kAnimationBaseInfo)
		{
			anim.baseName = r.line();
			anim.baseTime = r.f32();
			continue;
		}
		SkeletonTrack track;
		track.bone = r.u16();
		if (track.bone >= static_cast<int>(out.bones.size()) || out.bones[track.bone].name.empty())
			r.fail("track for unknown bone " + std::to_string(track.bone));
		if (anim.trackOfBone[track.bone] >= 0)
			r.fail("second track for bone " + std::to_string(track.bone));
		while (r.peekId() == kKeyFrame)
		{
			r.u16();
			readKeyFrame(r, r.u32(), track);
		}
		if (r.pos() != start + length)
			r.fail("track length " + std::to_string(length) + " does not cover its keyframes");
		// Ogre inserts keyframes in time order (after equal times).
		std::stable_sort(track.keys.begin(), track.keys.end(),
			[](const SkeletonKey& a, const SkeletonKey& b) { return a.time < b.time; });
		anim.trackOfBone[track.bone] = static_cast<int>(anim.tracks.size());
		anim.tracks.push_back(std::move(track));
	}
	out.animations.push_back(std::move(anim));
}

void orderBones(Reader& r, Skeleton& out)
{
	const std::size_t n = out.bones.size();
	for (std::size_t i = 0; i < n; ++i)
	{
		if (out.bones[i].name.empty())
			r.fail("bone handle " + std::to_string(i) + " missing");
	}
	// Depth-first from the roots; a bone not reached sits on a parent cycle.
	std::vector<std::vector<int>> children(n);
	for (std::size_t i = 0; i < n; ++i)
	{
		if (out.bones[i].parent >= 0)
			children[out.bones[i].parent].push_back(static_cast<int>(i));
	}
	std::vector<int> stack;
	for (std::size_t i = n; i-- > 0;)
	{
		if (out.bones[i].parent < 0)
			stack.push_back(static_cast<int>(i));
	}
	while (!stack.empty())
	{
		const int b = stack.back();
		stack.pop_back();
		out.parentFirst.push_back(b);
		for (auto it = children[b].rbegin(); it != children[b].rend(); ++it)
			stack.push_back(*it);
	}
	if (out.parentFirst.size() != n)
		r.fail("bone parent cycle");
}

SkelQuat normalised(SkelQuat q) noexcept
{
	const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	if (len > 0.0f)
	{
		q.w /= len;
		q.x /= len;
		q.y /= len;
		q.z /= len;
	}
	return q;
}

// Ogre Quaternion::nlerp with shortestPath = true.
SkelQuat nlerp(float t, const SkelQuat& p, SkelQuat q) noexcept
{
	if (p.w * q.w + p.x * q.x + p.y * q.y + p.z * q.z < 0.0f)
		q = SkelQuat { -q.w, -q.x, -q.y, -q.z };
	return normalised(SkelQuat { p.w + t * (q.w - p.w), p.x + t * (q.x - p.x),
		p.y + t * (q.y - p.y), p.z + t * (q.z - p.z) });
}

SkelVec3 lerp(float t, const SkelVec3& a, const SkelVec3& b) noexcept
{
	return SkelVec3 { a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z) };
}

// Ogre AnimationTrack::getKeyFramesAtTime + TransformKeyFrame linear
// interpolation.
SkelTransform interpolate(const SkeletonTrack& track, float length, float t) noexcept
{
	const auto& keys = track.keys;
	auto i = std::lower_bound(keys.begin(), keys.end(), t,
		[](const SkeletonKey& k, float time) { return k.time < time; });
	const SkeletonKey* k2;
	float t2;
	if (i == keys.end())
	{
		k2 = &keys.front(); // wrap towards the first key
		t2 = length + k2->time;
		--i;
	} else {
		k2 = &*i;
		t2 = k2->time;
		if (i != keys.begin() && t < i->time)
			--i;
	}
	const SkeletonKey& k1 = *i;
	const float f = k1.time == t2 ? 0.0f : (t - k1.time) / (t2 - k1.time);
	SkelTransform out;
	out.pos = lerp(f, k1.offset.pos, k2->offset.pos);
	out.rot = nlerp(f, k1.offset.rot, k2->offset.rot);
	out.scale = lerp(f, k1.offset.scale, k2->offset.scale);
	return out;
}

} // namespace

const SkeletonTrack* SkeletonAnimation::track(int bone) const noexcept
{
	if (bone < 0 || bone >= static_cast<int>(trackOfBone.size()) || trackOfBone[bone] < 0)
		return nullptr;
	return &tracks[trackOfBone[bone]];
}

int Skeleton::bone(std::string_view name) const noexcept
{
	for (std::size_t i = 0; i < bones.size(); ++i)
	{
		if (bones[i].name == name)
			return static_cast<int>(i);
	}
	return -1;
}

const SkeletonAnimation* Skeleton::animation(std::string_view name) const noexcept
{
	for (const SkeletonAnimation& a : animations)
	{
		if (a.name == name)
			return &a;
	}
	return nullptr;
}

Skeleton parseSkeleton(std::span<const std::byte> bytes, const std::string& name)
{
	Reader r(bytes, name);
	Skeleton out;
	if (r.u16() != kHeader)
		r.fail("not a skeleton");
	const std::string header = r.line();
	if (header == "[Serializer_v1.10]")
		out.version = "1.10";
	else if (header == "[Serializer_v1.80]")
		out.version = "1.80";
	else
		r.fail("unsupported version " + header);

	while (!r.atEnd())
	{
		const std::size_t start = r.pos();
		const std::uint16_t id = r.u16();
		const std::uint32_t length = r.u32();
		switch (id)
		{
		case kBlendMode:
			out.blendMode = r.u16();
			r.seek(start + length);
			break;
		case kBone:
			if (!out.animations.empty())
				r.fail("bone after animations");
			readBone(r, length, out);
			break;
		case kBoneParent:
		{
			const std::uint16_t child = r.u16();
			const std::uint16_t parent = r.u16();
			if (child >= out.bones.size() || parent >= out.bones.size() || child == parent)
				r.fail("bad bone parent " + std::to_string(child) + " -> " + std::to_string(parent));
			out.bones[child].parent = parent;
			break;
		}
		case kAnimation:
			if (out.parentFirst.empty())
				orderBones(r, out);
			readAnimation(r, out);
			if (r.pos() != start + length)
				r.fail("animation length " + std::to_string(length) + " does not cover its tracks");
			break;
		case kAnimationLink:
		{
			SkeletonLink link;
			link.file = r.line();
			link.scale = r.f32();
			out.links.push_back(std::move(link));
			break;
		}
		default:
			r.seek(start);
			r.fail("unknown chunk " + std::to_string(id));
		}
	}
	if (out.bones.empty())
		r.fail("no bones");
	if (out.parentFirst.empty())
		orderBones(r, out);
	return out;
}

Skeleton loadSkeleton(const std::string& path)
{
	const auto bytes = detail::readFile(path);
	return parseSkeleton(bytes, path);
}

SkelVec3 rotate(const SkelQuat& q, const SkelVec3& v) noexcept
{
	// v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
	const SkelVec3 u { q.x, q.y, q.z };
	const SkelVec3 c { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
	const SkelVec3 cc { u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x };
	return SkelVec3 { v.x + 2.0f * (q.w * c.x + cc.x), v.y + 2.0f * (q.w * c.y + cc.y),
		v.z + 2.0f * (q.w * c.z + cc.z) };
}

SkelQuat multiply(const SkelQuat& a, const SkelQuat& b) noexcept
{
	return SkelQuat {
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
		a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x,
	};
}

// Ogre Node::_updateFromParent with inherited orientation and scale.
SkelTransform combine(const SkelTransform& parent, const SkelTransform& child) noexcept
{
	SkelTransform out;
	out.rot = multiply(parent.rot, child.rot);
	out.scale = SkelVec3 { parent.scale.x * child.scale.x, parent.scale.y * child.scale.y,
		parent.scale.z * child.scale.z };
	const SkelVec3 scaled { parent.scale.x * child.pos.x, parent.scale.y * child.pos.y,
		parent.scale.z * child.pos.z };
	const SkelVec3 moved = rotate(parent.rot, scaled);
	out.pos = SkelVec3 { moved.x + parent.pos.x, moved.y + parent.pos.y, moved.z + parent.pos.z };
	return out;
}

SkelTransform inverse(const SkelTransform& t) noexcept
{
	SkelTransform out;
	out.rot = SkelQuat { t.rot.w, -t.rot.x, -t.rot.y, -t.rot.z };
	out.scale = SkelVec3 { 1.0f / t.scale.x, 1.0f / t.scale.y, 1.0f / t.scale.z };
	const SkelVec3 back = rotate(out.rot, t.pos);
	out.pos = SkelVec3 { -back.x * out.scale.x, -back.y * out.scale.y, -back.z * out.scale.z };
	return out;
}

void toMatrix(const SkelTransform& t, float out[16]) noexcept
{
	const SkelQuat& q = t.rot;
	const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	out[0] = (1.0f - 2.0f * (yy + zz)) * t.scale.x;
	out[1] = 2.0f * (xy + wz) * t.scale.x;
	out[2] = 2.0f * (xz - wy) * t.scale.x;
	out[3] = 0.0f;
	out[4] = 2.0f * (xy - wz) * t.scale.y;
	out[5] = (1.0f - 2.0f * (xx + zz)) * t.scale.y;
	out[6] = 2.0f * (yz + wx) * t.scale.y;
	out[7] = 0.0f;
	out[8] = 2.0f * (xz + wy) * t.scale.z;
	out[9] = 2.0f * (yz - wx) * t.scale.z;
	out[10] = (1.0f - 2.0f * (xx + yy)) * t.scale.z;
	out[11] = 0.0f;
	out[12] = t.pos.x;
	out[13] = t.pos.y;
	out[14] = t.pos.z;
	out[15] = 1.0f;
}

// Ogre NodeAnimationTrack::applyToNode at weight 1 after Bone::reset:
// translate in parent space, rotate in local space (normalised), scale.
SkelTransform sampleLocal(const Skeleton& skeleton, const SkeletonAnimation* animation,
	int bone, float t) noexcept
{
	SkelTransform out = skeleton.bones[bone].bind;
	const SkeletonTrack* track = animation != nullptr ? animation->track(bone) : nullptr;
	if (track == nullptr || track->keys.empty())
		return out;
	const SkelTransform key = interpolate(*track, animation->length, t);
	out.pos = SkelVec3 { out.pos.x + key.pos.x, out.pos.y + key.pos.y, out.pos.z + key.pos.z };
	out.rot = multiply(out.rot, normalised(key.rot));
	out.scale = SkelVec3 { out.scale.x * key.scale.x, out.scale.y * key.scale.y,
		out.scale.z * key.scale.z };
	return out;
}

SkelTransform sampleModel(const Skeleton& skeleton, const SkeletonAnimation* animation,
	int bone, float t) noexcept
{
	// Top-down like Ogre: combine() is not associative under non-uniform scale.
	const SkelTransform local = sampleLocal(skeleton, animation, bone, t);
	const int parent = skeleton.bones[bone].parent;
	return parent < 0 ? local : combine(sampleModel(skeleton, animation, parent, t), local);
}

void samplePose(const Skeleton& skeleton, const SkeletonAnimation* animation, float t,
	std::span<SkelTransform> local, std::span<SkelTransform> model) noexcept
{
	for (const int b : skeleton.parentFirst)
	{
		local[b] = sampleLocal(skeleton, animation, b, t);
		if (model.empty())
			continue;
		const int p = skeleton.bones[b].parent;
		model[b] = p < 0 ? local[b] : combine(model[p], local[b]);
	}
}

SkelMotion rootMotion(const Skeleton& skeleton, const SkeletonAnimation& animation,
	int bone, float t0, float t1) noexcept
{
	const SkelTransform a = sampleModel(skeleton, &animation, bone, t0);
	const SkelTransform b = sampleModel(skeleton, &animation, bone, t1);
	SkelMotion out;
	out.delta = SkelVec3 { b.pos.x - a.pos.x, b.pos.y - a.pos.y, b.pos.z - a.pos.z };
	// Twist about +Y of b * a^-1 (swing-twist decomposition).
	const SkelQuat rel = multiply(b.rot, SkelQuat { a.rot.w, -a.rot.x, -a.rot.y, -a.rot.z });
	if (rel.y != 0.0f || rel.w != 0.0f)
	{
		float yaw = 2.0f * std::atan2(rel.y, rel.w);
		if (yaw > std::numbers::pi_v<float>)
			yaw -= 2.0f * std::numbers::pi_v<float>;
		else if (yaw <= -std::numbers::pi_v<float>)
			yaw += 2.0f * std::numbers::pi_v<float>;
		out.yaw = yaw;
	}
	return out;
}

} // namespace kworld
