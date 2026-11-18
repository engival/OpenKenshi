#pragma once

// The last composed frame, kept render side (docs/RENDER_THREAD.md §7.1 rule 9 and its open point "lockstep's render-side timeout";
// kenshi-k16.2.205.12.25). In lockstep the render thread may not redraw a packet while logic runs (render_frame still reads app.), so a frame
// callback whose wait for a packet ran out draws no pass. sokol_app swaps anyway, and a back buffer nobody drew holds an older frame or
// undefined contents (the driver's): the window flickers. So every drawn frame is copied into a render-owned target after its commit (keep),
// and a frame without a packet blits it back (show) before the swap. Render thread only; GL only (Vulkan: no-ops, kenshi-k16.2.246).
// Only the lockstep path calls these: with render.thread off nothing here runs.
namespace lastframe
{
void keep();	// after the drawn frame's sg_commit: the default framebuffer's back buffer into the kept target (resized to the window)
bool show();	// a frame without a pass: the kept target onto the back buffer (scaled to the window); false before the first keep
void shutdown();	// cleanup(), before sg_shutdown
// Dev check of the open point (KCLIENT_LOCKSTEP_STALL_SHOTS=DIR, KCLIENT_LOCKSTEP_NOKEEP=1 skips show): the first frames without a pass
// write the back buffer that is about to be swapped, DIR/nopass-NN.png, so a run shows what the window would show.
void stallShot();
}
