// Readback requests (render hook 4, kenshi-k16.2.205.12.10; docs/RENDER_THREAD.md section 6 and 7.1): every GPU -> CPU pixel read of a frame
// the client makes (screenshots, the dev depth probe) is a request. Logic makes it while it builds the frame (logic_frame's tail, after the
// packet is filled); the draw half serves the queue after the frame's last pass and before sg_commit (serve), so the read sees the frame the
// request was made for. serve records each result by value (ok, the probe's values and info); the dones run on the logic thread at its next
// frame start (runDone, at the top of logic_frame, after framestats::take), in request order (kenshi-k16.2.205.12.24). On one thread that is
// right after the frame the request was made for, before logic touches anything, so a done sees what it saw inside serve before
// (kenshi-k16.2.205.12.23), the frame stats included (framestats::stats() is that frame's copy by then). No done may capture a caller's locals
// by reference; a done that needs a value of the request's frame that logic changes (app.frames) captures it by value.
// A request carries the serial of the packet logic was filling (beginPacket); serve only serves the packets drawn so far (drawing).
// The cursor id pass (idbuffer.cpp, a PBO ring read two frames late) is not here: it joins the frame stats slot (hook 5).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sokol_gfx.h"

namespace readback
{

using Done = std::function<void(bool ok)>;

// The swapchain's back buffer as a PNG at `path` (opaque, top row first), at the window size of the frame being built (view::, read at the
// call). The draw half reads the pixels at the serve point; the encode and the file are the writer thread's, in request order, so the file
// lands after done may have run (kenshi-k16.2.205.12.70). done(ok): ok when the pixels were read; a failed write prints "failed to write"
// with the reason (errno's text).
// Whatever reads the file in this process, or ends it, calls flushWrites first.
// wrote(ok, why), when given, is the writer thread's result (kenshi-k16.2.312): it runs on the logic thread at the next frame start after the
// file is written or the write failed (runWrites), so a caller's message about the file follows the file. ok false with why from errno: the
// PNG could not be written. It runs only when the pixels were read (a failed read is done's). Results still pending at a flushWrites are not run.
using WriteDone = std::function<void(bool ok, const std::string& why)>;
void screenshot(const std::string& path, Done done = {}, WriteDone wrote = {});

// Blocks until every screenshot queued so far is on disk (or failed). Before a read of a shot file (the --bead copy) and before the process
// ends (exit_now, cleanup), so a script's quit right after its screenshot still leaves the file.
void flushWrites();

// The dev depth probe: one float per (x, gl_y) pixel (GL origin bottom left) of the image `image` returns when the request is served (the
// target as the frame drew it; a resize may have rebuilt it since the request): the R32F colour copy when it sets `colour`, else the depth
// attachment. An image id 0 is a failed read (ok false, no values). info.tex / info.complete describe the read FBO for the probe's log line.
struct ProbeInfo { unsigned tex = 0; bool complete = false; bool colour = false; };
using ProbeDone = std::function<void(bool ok, const std::vector<float>& values, const ProbeInfo& info)>;
void probe(std::function<sg_image(bool& colour)> image, std::vector<std::pair<int, int>> px, ProbeDone done);

// A render-side read that is not a pixel read of the frame (e.g. the HDR luminance levels): read() runs at the serve point on the render side
// and returns its values; done(ok, values) gets them by value on the logic thread. read must touch only render-owned state and capture
// nothing by reference. ok is false when read returns no values.
using SampleDone = std::function<void(bool ok, const std::vector<float>& values)>;
void sample(std::function<std::vector<float>()> read, SampleDone done);

// A request with no pixels: done(true) runs with the reads' dones, in order. For a request that needs the frame's draw outputs
// (the frame stats slot, the draw counters) as they were when the frame's last pass ended.
void after(Done done);

// Logic side: logic_frame's start, the packet being filled from now on (rq::beginPacket's serial); the requests made until the next call carry it.
void beginPacket(uint64_t serial);

// Render side: render_frame's start, the serial of the packet about to be drawn.
void drawing(uint64_t serial);

// The draw half, after the frame's last pass and before sg_commit: reads every queued request of the packets drawn so far, in order, and
// keeps the results for runDone.
void serve();

// Logic side, at logic_frame's start (after framestats::take): the served requests' dones, in request order. A request a done makes belongs to
// the packet being filled.
void runDone();

// Logic side, at logic_frame's start after runDone: the screenshots the writer thread finished since the last call, in write order, their
// wrote results (kenshi-k16.2.312). The toast about a shot's file hangs off this, not off the read, and never waits on the draw.
void runWrites();

// A float RGBA image (an offscreen colour target, `w` x `h`, bottom row first) as raw floats, w * h * 4 (empty when it cannot read).
// Synchronous, on the thread that drew the target: a read of a target the caller just drew itself (the dev anim lab), not of the frame.
std::vector<float> image_rgba32f(sg_image img, int w, int h);

// Writes an RGBA8 image (top row first as given) as a PNG; the one PNG writer, for readers that process the pixels themselves. true on success.
bool write_png(const std::string& path, int w, int h, const unsigned char* rgba);

}
