#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "kworld/heightmap.hpp"
#include "sokol_gfx.h"

// Squads of the sim's snapshot (cpp/build/default_world_squads.json, ksim_run --snapshot) drawn as pillars in the world and dots on the maps.
class Squads
{
public:
	struct Dot { float x, y, z; float r, g, b; uint64_t id; };	// x,z world (already fanned out), y ground height; id: the sim squad (live), else 0
	struct Source { std::string name; float r, g, b; size_t count; };

	// Reads the snapshot; a missing file is a one-line notice and leaves the set empty. Returns the number of squads shown.
	size_t load(const std::string& path, const kworld::Heightmap& hm);
	// The live set instead (SIMLINK SQUADS, coloured by the host: the debug view's goal kind, else one colour): replaces whatever was shown.
	struct Live { uint64_t id; float x, z; float r, g, b; std::string source; };
	void setLive(const std::vector<Live>& squads, const kworld::Heightmap& hm);
	void init();
	void shutdown();

	// Inside a pass, after the opaque world. cam = camera position. count: the pillars to draw, drawCount() taken by logic into the frame packet
	// (FramePacket::scene.squads, kenshi-k16.2.205.12.51): the draw reads neither `on` nor the dots, which setLive / the toggle change on logic's side.
	void render(const float mvp[16], const double cam[3], int count) const;
	int drawCount() const { return on ? (int)dots_.size() : 0; }

	bool on = false;
	const std::vector<Dot>& dots() const { return dots_; }
	const std::vector<Source>& sources() const { return sources_; }

private:
	void place(uint64_t id, float x, float z, const std::string& source, float r, float g, float b, const kworld::Heightmap& hm, std::map<std::pair<float, float>, int>& stacked);
	std::vector<Dot> dots_;
	std::vector<Source> sources_;
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_buffer box_ = {}, boxIdx_ = {}, inst_ = {};
};
