#pragma once
#include <functional>
#include <string>
#include <unordered_map>

#include "sokol_gfx.h"

// The original's hover cursors (docs/reference/CURSORS.md): MyGUI pointer resources (data/gui/pointer/kenshi_pointers.xml) cut from the
// Kenshi_UI.png atlas (image sets in data/gui/images/kenshi_images.xml). Drawn in software over the overlay as MyGUI does (its Pointer layer), the
// system cursor hidden while one shows: sokol_app's X11 custom cursor crashed in XFlush under the headless gamescope.
class Cursors
{
public:
	// Logic side: the CPU half (the pointer table, the atlas pixels); the GPU half is a frame-lane job (rq, docs/RENDER_THREAD.md §7.2).
	// shared: called by that job for the UI layer's copy of Kenshi_UI.png (w x h; not ours to destroy), used instead of loading our own when the pointers live there.
	// False: the files did not load (the system cursor stays).
	bool init(const std::string& dataDir, std::function<sg_view()> shared = {}, int sharedW = 0, int sharedH = 0);
	void shutdown();
	void set(const std::string& name);	// "" or an unknown name: the system cursor
	const std::string& current() const { return current_; }
	// Inside begin_overlay / end_overlay, last; render side. name: the shape logic chose (current(), carried in the packet: FramePacket::cursor,
	// render hook 8), so the draw reads nothing logic changes; (mx, my): the newest mouse position, so the cursor gains no frame of lag.
	void draw(const std::string& name, float mx, float my) const;

private:
	struct Pointer { int x = 0, y = 0, w = 0, h = 0, hotX = 0, hotY = 0; };
	std::unordered_map<std::string, Pointer> pointers_;	// logic: which names have a shape (set())
	std::unordered_map<std::string, Pointer> drawPointers_;	// render: the same table, published by the init job (draw())
	int atlasW_ = 0, atlasH_ = 0;
	sg_image img_ = {};
	sg_view view_ = {};
	sg_sampler smp_ = {};
	std::string current_;
};
