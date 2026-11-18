// Prints an Ogre .skeleton: bones (handle, parent, bind pose), clips (length,
// tracks, keyframes) and links. --motion BONE adds each clip's root motion of
// that bone over the whole clip. docs/formats/OGRE_SKELETON.md.

#include "kworld/ogreskeleton.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

int main(int argc, char** argv)
{
	std::string path;
	std::string motionBone;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--motion") == 0 && i + 1 < argc)
			motionBone = argv[++i];
		else
			path = argv[i];
	}
	if (path.empty())
	{
		std::fprintf(stderr, "usage: kskeleton_dump FILE.skeleton [--motion BONE]\n");
		return 2;
	}
	try
	{
		const kworld::Skeleton s = kworld::loadSkeleton(path);
		std::printf("version %s  blend mode %d  bones %zu  clips %zu  links %zu\n", s.version.c_str(),
			s.blendMode, s.bones.size(), s.animations.size(), s.links.size());
		for (std::size_t i = 0; i < s.bones.size(); ++i)
		{
			const auto& b = s.bones[i];
			std::printf("bone %2zu parent %2d  pos %9.4f %9.4f %9.4f  rot %7.4f %7.4f %7.4f %7.4f  scale %.3f %.3f %.3f  %s\n",
				i, b.parent, b.bind.pos.x, b.bind.pos.y, b.bind.pos.z, b.bind.rot.w, b.bind.rot.x,
				b.bind.rot.y, b.bind.rot.z, b.bind.scale.x, b.bind.scale.y, b.bind.scale.z, b.name.c_str());
		}
		const int mb = motionBone.empty() ? -1 : s.bone(motionBone);
		if (!motionBone.empty() && mb < 0)
			std::fprintf(stderr, "no bone %s\n", motionBone.c_str());
		for (const auto& a : s.animations)
		{
			std::size_t keys = 0;
			for (const auto& t : a.tracks)
				keys += t.keys.size();
			std::printf("clip %-32s %8.4f s  tracks %3zu  keys %6zu", a.name.c_str(), a.length, a.tracks.size(), keys);
			if (!a.baseName.empty())
				std::printf("  base %s @ %.3f", a.baseName.c_str(), a.baseTime);
			if (mb >= 0)
			{
				const auto m = kworld::rootMotion(s, a, mb, 0.0f, a.length);
				std::printf("  motion %9.3f %9.3f %9.3f  yaw %7.2f deg", m.delta.x, m.delta.y, m.delta.z,
					m.yaw * 57.29578f);
			}
			std::printf("\n");
		}
		for (const auto& l : s.links)
			std::printf("link %s scale %.3f\n", l.file.c_str(), l.scale);
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
	return 0;
}
