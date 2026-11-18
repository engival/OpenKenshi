#pragma once
// The animation lab's frame (kenshi-k16.2.205.12.56): what the lab's offscreen draw needs, copied out of the lab on the logic thread at the
// packet's fill (dev::labFrame) and carried in FramePacket::lab, so the render thread's dev::labRender reads the packet and never the lab
// (animlab::Lab and its State are the logic thread's: the script ops, the browser window, update). Dev tools only (KCLIENT_DEV_TOOLS).
#include <string>
#include <vector>

#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"
#include "characters.hpp"	// Characters::FrameData
#include "sky.hpp"	// LightParams


namespace dev
{
	struct LabFrame
	{
		bool draw = false;	// the flag: the lab renders this frame (ready, open or scripted, a target, a skeleton)
		// the lab's character set: the render draws its portrait slot from `frame` (renderPortrait; the GPU sets are the object's). Its poses are set and
		// gathered on the logic thread (Lab::update, then portraitCopy in Lab::frame, kenshi-k16.2.205.12.67): render reads no character.
		Characters* chars = nullptr;
		LightParams light = {};
		float mvp[16] = {};
		double eye[3] = {};
		Characters::FrameData frame;	// the lab character's pose, camera-relative to eye (Characters::portraitCopy)
		int w = 0, h = 0;	// the target's size
		sg_view colorAtt = {}, depthAtt = {};	// the target (made and resized by the lab's frame-lane jobs; named here only once published, kenshi-k16.2.205.12.69)
		sgl_context sgl = {};
		sdtx_context sdtx = {};
		// the overlay, projected to the target's pixels (y down): the floor grid, the sole points and their trails, the root path
		struct Line { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; float c[4] = {}; };
		std::vector<Line> lines;
		bool boneNames = false;
		struct Label { float x = 0, y = 0; std::string text; };	// sdtx_pos units, as the lab placed them
		std::vector<Label> labels;
	};
}
