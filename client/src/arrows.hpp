#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sokol_gfx.h"

// The stealth detection arrows (kenshi-h2v.27.2; kenshi-inspect findings/stealth_detection/rwh63_arrow_live.md and red_arrow_rules.md section 1):
// one arrow.mesh instance per DETECTED_BY (SIMLINK 60) entry, unlit in the entry's colour, at clamp(dist - 7, 0, 15) from the sneaker toward the
// observer plus 12 u world Y, the mesh's local -X turned onto the full 3D direction, scale x 0.5, y/z 0.35. The game's own mesh, through the
// client's Ogre reader; a mesh that cannot be loaded draws nothing and says why (never a substitute shape).
class Arrows
{
public:
	// The resource path of arrow.mesh, then its triangles into buffers; false with err filled in (the caller logs it once).
	bool init(const std::string& meshPath, std::string& err);
	void shutdown();
	bool loaded() const { return pip_.id != 0; }
	const std::string& path() const { return path_; }

	// One arrow: its anchor (world) and the unit direction to the observer; colour 0..1.
	struct Mark { double pos[3]; double dir[3]; float colour[3]; };

	// The frame's arrows into the instance buffer (a transient write: call it outside any pass, before render). cam = the camera position.
	void update(const std::vector<Mark>& marks, const double cam[3]);
	// Inside the scene pass after the opaque world (the depth buffer is bound).
	void render(const float mvp[16]);

private:
	struct Part
	{
		sg_buffer vb = {};	// its own geometry (when the mesh's submesh is not shared)
		sg_buffer ib = {};
		int indexCount = 0;
	};
	bool buildParts(const std::string& meshPath, std::string& err);	// reads arrow.mesh (client/src/ogremesh.hpp) into the buffers

	std::string path_;
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_buffer sharedVb_ = {};	// the mesh's shared geometry, when the submeshes use it
	std::vector<Part> parts_;
	static constexpr int kMaxMarks = 256;	// the instance buffer's capacity (one arrow per detection record; the sim sends a handful)
	sg_buffer inst_ = {};
	int count_ = 0;	// the arrows written this frame
};
