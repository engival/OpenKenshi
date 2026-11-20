#pragma once

// One gate for window state changes (kenshi-k16.2.222): size, maximize, fullscreen and monitor changes all arrive as a new
// frame size, and a high-rate mouse dragging a border (or a window manager animating one) can deliver a storm of them.
// The gate applies the first change at once (render targets rebuilt, UI relaid out, the loading screen's text re-baked), holds
// further changes for 1 s, and then applies only the latest held state once (never lost), which starts a new cooldown. Between
// the changes the world renders at the last applied size and the composite scales it to the window: no black frame, no crash.
// The applied size is what everything sized to the window (HDR scene/mask targets, post targets, the id buffer, the camera) uses.
namespace windowgate
{
	// The first frame: the window's own size, applied. Later calls are ignored.
	void init(int w, int h);
	// Once per frame on the sapp thread, after windowgeom::restore(): polls the window, applies or holds its change (see above) and publishes
	// the result. Logic copies it at its frame start (take); every accessor below reads that copy (kenshi-k16.2.205.12.23).
	void frame();
	void take();
	// Applies a held change now (the exit path, so windowgeom's save records the state the client applied, not a mid-cooldown event).
	void flush();
	// The applied size: what the frame renders at while a change is held (the swapchain is always the window's own size).
	// The render thread's frame (render_frame) sets the size it draws at for its own thread: the packet's applied size (RENDER_THREAD.md rule 10), so width()/height()
	// there never read the logic-side copy take() writes (kenshi-k16.2.205.12.108). A scope on one thread only; other threads are untouched.
	struct RenderSize
	{
		RenderSize(int w, int h);
		~RenderSize();
		RenderSize(const RenderSize&) = delete;
		RenderSize& operator=(const RenderSize&) = delete;
	};
	int width();
	int height();
	float widthf();
	float heightf();
	float aspect();
	// An apply happened in the frame just polled: the loading screen re-lays out its text (LoadScreen::relayout).
	bool appliedThisFrame();
	// A change is held for the cooldown (the dev burst waits for this to clear before its shot).
	bool holding();
	// Applies so far since init (a scripted burst of N >= 20 changes in under 1 s shows 2).
	long applies();
}

// The window's own metrics as one published view (render hook 2, kenshi-k16.2.205.12.7): the swapchain size and the frame duration,
// read from sapp_* once at the top of frame() on the sapp thread (view::snapshot, which publishes it) and copied by logic at its frame
// start (view::take); every read after that returns the copy. Logic code never calls sapp_width/height/frame_duration itself
// (kenshi-k16.2.205.12.23). Within a frame sokol's size does not change, so the copy equals a live read. Before the first take the
// accessors read sapp_* directly.
// (windowgate::width() above is the APPLIED size, which differs from this while a change is held.)
namespace view
{
	void snapshot();
	void take();
	// The render thread's frame (render_frame) sets the size it draws at for its own thread: the packet's applied size (RENDER_THREAD.md rule 10), so width()/height()
	// there never read the logic-side copy take() writes (kenshi-k16.2.205.12.108). A scope on one thread only; other threads are untouched.
	struct RenderSize
	{
		RenderSize(int w, int h);
		~RenderSize();
		RenderSize(const RenderSize&) = delete;
		RenderSize& operator=(const RenderSize&) = delete;
	};
	int width();
	int height();
	float widthf();
	float heightf();
	double frameDuration();
	double frameDurationLive();	// the sapp side's own: the last snapshot's frame duration (render thread; logic reads frameDuration)
	// The display's refresh period in s from RandR (windowgeom::refreshHz), published with the snapshot; 0 when unknown. Render thread: it
	// paces lockstep's no-packet bound (rule 4; kenshi-k16.2.205.12.87), never a measured interval.
	double refreshPeriodLive();
}
