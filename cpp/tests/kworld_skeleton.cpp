// Ogre .skeleton reader (docs/formats/OGRE_SKELETON.md): every skeleton in
// the install parses, the clip census matches docs/reference/combat/data/
// anim_clips.tsv, and sampling / root motion agree with the keyframes.

#include "kworld/error.hpp"
#include "kworld/ogreskeleton.hpp"
#include "kworld_test_util.hpp"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool near(float a, float b, float eps = 1e-4f)
{
	return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b));
}

bool nearT(const kworld::SkelTransform& a, const kworld::SkelTransform& b)
{
	// q and -q are the same rotation
	const float dot = a.rot.w * b.rot.w + a.rot.x * b.rot.x + a.rot.y * b.rot.y + a.rot.z * b.rot.z;
	return near(a.pos.x, b.pos.x) && near(a.pos.y, b.pos.y) && near(a.pos.z, b.pos.z)
		&& std::fabs(std::fabs(dot) - 1.0f) < 1e-4f
		&& near(a.scale.x, b.scale.x) && near(a.scale.y, b.scale.y) && near(a.scale.z, b.scale.z);
}

struct Clip
{
	float length;
	std::size_t tracks;
	std::size_t keys;
};

// skeleton (relative to data/) -> clip name -> census row
std::map<std::string, std::map<std::string, Clip>> readCensus()
{
	const std::string path = std::string(KWORLD_SOURCE_DIR) + "/../docs/reference/combat/data/anim_clips.tsv";
	std::ifstream in(path);
	test::require(static_cast<bool>(in), "cannot open " + path);
	std::map<std::string, std::map<std::string, Clip>> out;
	std::string line;
	std::getline(in, line); // header
	while (std::getline(in, line))
	{
		std::istringstream row(line);
		std::string file, clip, length, tracks, keys;
		std::getline(row, file, '\t');
		std::getline(row, clip, '\t');
		std::getline(row, length, '\t');
		std::getline(row, tracks, '\t');
		std::getline(row, keys, '\t');
		out[file][clip] = Clip { std::stof(length), std::stoul(tracks), std::stoul(keys) };
	}
	return out;
}

void checkCensus(const std::map<std::string, kworld::Skeleton>& all)
{
	const auto census = readCensus();
	std::size_t clips = 0;
	for (const auto& [file, rows] : census)
	{
		const auto it = all.find(file);
		test::require(it != all.end(), "census skeleton not parsed: " + file);
		const kworld::Skeleton& s = it->second;
		test::require(s.animations.size() == rows.size(), file + ": clip count");
		for (const auto& [name, clip] : rows)
		{
			const kworld::SkeletonAnimation* a = s.animation(name);
			test::require(a != nullptr, file + ": no clip " + name);
			std::size_t keys = 0;
			for (const auto& t : a->tracks)
				keys += t.keys.size();
			test::require(std::fabs(a->length - clip.length) < 1e-3f, file + " " + name + ": length");
			test::require(a->tracks.size() == clip.tracks, file + " " + name + ": tracks");
			test::require(keys == clip.keys, file + " " + name + ": keyframes");
			++clips;
		}
	}
	test::require(census.size() == 29 && clips == 561, "census is 561 clips in 29 skeletons");
	test::require(all.at("character/meshes/male_skeleton/male_skeleton.skeleton").animations.size() == 174,
		"male clips");
	test::require(all.at("character/meshes/female_skeleton/female_skeleton.skeleton").animations.size() == 166,
		"female clips");
}

void checkSampling(const kworld::Skeleton& s)
{
	std::vector<kworld::SkelTransform> local(s.bones.size()), model(s.bones.size());
	for (const auto& a : s.animations)
	{
		for (const auto& track : a.tracks)
		{
			// at a key time the sample is that key's offset on the bind pose
			const auto& key = track.keys[track.keys.size() / 2];
			kworld::SkelTransform expect = s.bones[track.bone].bind;
			expect.pos = { expect.pos.x + key.offset.pos.x, expect.pos.y + key.offset.pos.y,
				expect.pos.z + key.offset.pos.z };
			const float len = std::sqrt(key.offset.rot.w * key.offset.rot.w + key.offset.rot.x * key.offset.rot.x
				+ key.offset.rot.y * key.offset.rot.y + key.offset.rot.z * key.offset.rot.z);
			const kworld::SkelQuat unit { key.offset.rot.w / len, key.offset.rot.x / len,
				key.offset.rot.y / len, key.offset.rot.z / len };
			expect.rot = kworld::multiply(expect.rot, unit);
			expect.scale = { expect.scale.x * key.offset.scale.x, expect.scale.y * key.offset.scale.y,
				expect.scale.z * key.offset.scale.z };
			// equal key times: Ogre lands on the first of them
			if (track.keys.size() / 2 > 0 && track.keys[track.keys.size() / 2 - 1].time == key.time)
				continue;
			test::require(nearT(kworld::sampleLocal(s, &a, track.bone, key.time), expect),
				a.name + ": sample at a key time");
		}
		const float t = a.length * 0.37f;
		kworld::samplePose(s, &a, t, local, model);
		for (std::size_t b = 0; b < s.bones.size(); ++b)
		{
			test::require(nearT(model[b], kworld::sampleModel(s, &a, static_cast<int>(b), t)),
				a.name + ": pose and single-bone model agree");
		}
		const auto still = kworld::rootMotion(s, a, 0, t, t);
		test::require(still.delta.x == 0.0f && still.delta.y == 0.0f && still.delta.z == 0.0f
			&& std::fabs(still.yaw) < 1e-5f, a.name + ": zero span, zero motion");
	}
}

void checkAlgebra()
{
	const float h = std::sqrt(0.5f);
	const kworld::SkelTransform parent { { 10.0f, 0.0f, 0.0f }, { h, 0.0f, h, 0.0f }, { 2.0f, 2.0f, 2.0f } };
	const kworld::SkelTransform child { { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f } };
	const auto c = kworld::combine(parent, child);
	// +90 deg about +Y maps +X to -Z; scaled by 2, then moved by the parent
	test::require(near(c.pos.x, 10.0f) && near(c.pos.y, 0.0f) && near(c.pos.z, -2.0f), "combine position");
	const auto id = kworld::combine(kworld::inverse(parent), parent);
	test::require(nearT(id, kworld::SkelTransform {}), "inverse");

	// yaw of a pure +Y turn
	kworld::Skeleton s;
	s.bones.push_back({ "root", -1, {} });
	s.parentFirst = { 0 };
	kworld::SkeletonAnimation a;
	a.name = "turn";
	a.length = 1.0f;
	kworld::SkeletonTrack track;
	track.bone = 0;
	const float q = std::sin(0.25f);
	track.keys.push_back({ 0.0f, {} });
	track.keys.push_back({ 1.0f, { { 3.0f, 0.0f, 4.0f }, { std::cos(0.25f), 0.0f, q, 0.0f }, { 1.0f, 1.0f, 1.0f } } });
	a.tracks.push_back(track);
	a.trackOfBone = { 0 };
	const auto m = kworld::rootMotion(s, a, 0, 0.0f, 1.0f);
	test::require(near(m.yaw, 0.5f) && near(m.delta.x, 3.0f) && near(m.delta.z, 4.0f), "root motion of a turn");
	const auto half = kworld::rootMotion(s, a, 0, 0.0f, 0.5f);
	test::require(near(half.delta.x, 1.5f) && near(half.yaw, 0.25f, 1e-3f), "root motion halfway");
}

} // namespace

int main()
{
	try
	{
		const std::filesystem::path census = std::filesystem::path(KWORLD_SOURCE_DIR) / "../docs/reference/combat/data/anim_clips.tsv";
		if (!std::filesystem::exists(census))
		{
			std::cout << "kworld_skeleton: SKIPPED: the clip census is not in this tree (game-derived, not shipped)\n";
			return 77;
		}
		checkAlgebra();
		const std::filesystem::path data(KENSHI_DATA);
		std::map<std::string, kworld::Skeleton> all;
		std::size_t files = 0, empty = 0, clips = 0;
		std::map<std::string, std::size_t> versions;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(data))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".skeleton")
				continue;
			++files;
			const std::string rel = entry.path().lexically_relative(data).generic_string();
			if (entry.file_size() == 0)
			{
				bool threw = false;
				try
				{
					kworld::loadSkeleton(entry.path().string());
				}
				catch (const kworld::Error&)
				{
					threw = true;
				}
				test::require(threw, rel + ": empty file must be rejected");
				++empty;
				continue;
			}
			kworld::Skeleton s = kworld::loadSkeleton(entry.path().string());
			++versions[s.version];
			clips += s.animations.size();
			checkSampling(s);
			all.emplace(rel, std::move(s));
		}
		test::require(files == 365 && empty == 1, "365 skeleton files, one empty (found "
			+ std::to_string(files) + ", " + std::to_string(empty) + " empty)");
		test::require(versions["1.80"] == 264 && versions["1.10"] == 100, "serializer versions");
		checkCensus(all);
		std::cout << "kworld skeleton: " << all.size() << " skeletons, " << clips
			<< " clips, census and sampling ok\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld skeleton: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
