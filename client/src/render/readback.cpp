#include "app/logicthread.hpp"
#include "render/readback.hpp"

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "sokol_gfx.h"
#include "stb_image_write.h"
#include "platform/windowgate.hpp"

#if defined(SOKOL_GLCORE)
#include "platform/gl.hpp"
#endif

namespace readback
{

bool write_png(const std::string& path, int w, int h, const unsigned char* rgba)
{
	static std::mutex m;	// stbi_flip_vertically_on_write is a process global: the screenshot writer, the anim lab and lastframe set it from different threads
	std::lock_guard<std::mutex> lock(m);
	stbi_flip_vertically_on_write(1);
	if (!stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4)) { return false; }
	return true;
}

namespace
{
	// The screenshot writer (kenshi-k16.2.205.12.70): the PNG encode of a 1280x720 frame is 150-250 ms of stb's zlib, so the draw half only
	// reads the pixels and one writer thread encodes and writes them, in request order (two shots to one path end with the later one).
	struct WriteJob
	{
		std::string path;
		int w = 0, h = 0;
		std::vector<unsigned char> px;	// RGBA8 as glReadPixels returned it (bottom row first, alpha as read)
		WriteDone wrote;		// the write's result, run on the logic thread (runWrites); empty when the caller wants none
	};
	struct WriteResult
	{
		WriteDone wrote;
		bool ok = false;
		std::string why;
	};
	struct Writer
	{
		std::mutex m;
		std::condition_variable work, idle;
		std::deque<WriteJob> q;
		std::deque<WriteResult> results;	// written, not yet run on the logic thread (runWrites)
		bool busy = false, started = false;
	};
	Writer& writer()	// never destroyed: the detached thread waits on it until the process ends (no condition variable destroyed under a waiter)
	{
		static Writer* w = new Writer;
		return *w;
	}

	void writerLoop()
	{
		Writer& wr = writer();
		std::unique_lock<std::mutex> lock(wr.m);
		for (;;)
		{
			wr.work.wait(lock, [&] { return !wr.q.empty(); });
			WriteJob j = std::move(wr.q.front());
			wr.q.pop_front();
			wr.busy = true;
			lock.unlock();
			for (size_t i = 3; i < j.px.size(); i += 4) { j.px[i] = 255; }
			errno = 0;	// the reason a failed write gives is the file system's (kenshi-k16.2.312)
			const bool ok = write_png(j.path, j.w, j.h, j.px.data());
			std::string why;
			if (!ok) { why = errno ? std::strerror(errno) : "stb_image_write failed"; fprintf(stderr, "failed to write %s: %s\n", j.path.c_str(), why.c_str()); }
			else { printf("wrote %s (%dx%d)\n", j.path.c_str(), j.w, j.h); }
			lock.lock();
			if (j.wrote) { wr.results.push_back({ std::move(j.wrote), ok, std::move(why) }); }
			wr.busy = false;
			if (wr.q.empty()) { wr.idle.notify_all(); }
		}
	}

	void queueWrite(WriteJob&& j)
	{
		Writer& wr = writer();
		std::lock_guard<std::mutex> lock(wr.m);
		if (!wr.started)
		{
			std::thread(writerLoop).detach();
			wr.started = true;
		}
		wr.q.push_back(std::move(j));
		wr.work.notify_one();
	}

	struct Request
	{
		enum Kind { kScreenshot, kProbe, kSample, kAfter } kind = kAfter;
		std::string path;			// kScreenshot
		int w = 0, h = 0;			// kScreenshot: the window size of the frame the request was made for
		std::function<sg_image(bool&)> image;	// kProbe
		std::vector<std::pair<int, int>> px;	// kProbe
		std::function<std::vector<float>()> read;	// kSample
		Done done;
		WriteDone wrote;			// kScreenshot: handed to the write job (runs on the logic thread, runWrites)
		ProbeDone probeDone;
		SampleDone sampleDone;
		uint64_t serial = 0;			// the packet logic was filling when it made the request (beginPacket)
	};
	// A served request: its done and the read's result, by value, until runDone calls it on the logic thread.
	struct Finished
	{
		Done done;
		ProbeDone probeDone;
		SampleDone sampleDone;
		bool ok = false;
		std::vector<float> values;	// kProbe, kSample
		ProbeInfo info;			// kProbe
	};
	std::mutex gMtx;		// guards gQueue and gFinished (logic pushes and runs dones; the draw half serves)
	std::vector<Request> gQueue;
	std::vector<Finished> gFinished;
	uint64_t gLogicSerial = 0;	// logic side only
	uint64_t gDrawSerial = 0;	// render side only

	void push(Request&& r)
	{
		r.serial = gLogicSerial;
		std::lock_guard<std::mutex> lock(gMtx);
		gQueue.push_back(std::move(r));
	}

	bool readScreenshot(Request& r)
	{
		bool ok = false;
#if defined(SOKOL_GLCORE)
		const int w = r.w, h = r.h;
		std::vector<unsigned char> px((size_t)w * h * 4);
		glFinish();
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadBuffer(GL_BACK);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
		queueWrite(WriteJob{ r.path, w, h, std::move(px), std::move(r.wrote) });	// the encode and the file on the writer thread; flushWrites waits for it
		ok = true;
#else
		fprintf(stderr, "--screenshot is only implemented for the glcore backend (%s)\n", r.path.c_str());
#endif
		return ok;
	}

	bool readProbe(const Request& r, std::vector<float>& out, ProbeInfo& info)	// out, info: the Finished being filled
	{
		bool ok = false;
		const sg_image img = r.image ? r.image(info.colour) : sg_image{};
#if defined(SOKOL_GLCORE)
		if (img.id)
		{
			const bool colour = info.colour;
			GLuint fbo = 0;
			glGenFramebuffers(1, &fbo);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
			const GLuint tex = sg_gl_query_image_info(img).tex[0];
			if (colour)
			{
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
				glReadBuffer(GL_COLOR_ATTACHMENT0);
			}
			else
			{
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
				glReadBuffer(GL_NONE);
			}
			info.tex = tex;
			info.complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
			glPixelStorei(GL_PACK_ALIGNMENT, 4);
			out.assign(r.px.size(), 1.0f);
			for (size_t i = 0; i < r.px.size(); i++)
			{
				float d = 1.0f;
				if (colour) { glReadPixels(r.px[i].first, r.px[i].second, 1, 1, GL_RED, GL_FLOAT, &d); }
				else { glReadPixels(r.px[i].first, r.px[i].second, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d); }
				out[i] = d;
			}
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &fbo);
			sg_reset_state_cache();
			ok = true;
		}
#else
		(void)img;
#endif
		return ok;
	}
}

void flushWrites()
{
	Writer& wr = writer();
	std::unique_lock<std::mutex> lock(wr.m);
	wr.idle.wait(lock, [&] { return wr.q.empty() && !wr.busy; });
}

void beginPacket(uint64_t serial) { gLogicSerial = serial; }
void drawing(uint64_t serial) { gDrawSerial = serial; }

void screenshot(const std::string& path, Done done, WriteDone wrote)
{
	Request r;
	r.kind = Request::kScreenshot;
	r.path = path;
	r.w = view::width();
	r.h = view::height();
	r.done = std::move(done);
	r.wrote = std::move(wrote);
	push(std::move(r));
}

void probe(std::function<sg_image(bool& colour)> image, std::vector<std::pair<int, int>> px, ProbeDone done)
{
	Request r;
	r.kind = Request::kProbe;
	r.image = std::move(image);
	r.px = std::move(px);
	r.probeDone = std::move(done);
	push(std::move(r));
}

void sample(std::function<std::vector<float>()> read, SampleDone done)
{
	Request r;
	r.kind = Request::kSample;
	r.read = std::move(read);
	r.sampleDone = std::move(done);
	push(std::move(r));
}

void after(Done done)
{
	Request r;
	r.kind = Request::kAfter;
	r.done = std::move(done);
	push(std::move(r));
}

void serve()
{
	std::vector<Request> mine;	// the requests of the packets drawn so far, in request order; a later packet's stay queued
	{
		std::lock_guard<std::mutex> lock(gMtx);
		auto keep = gQueue.begin();
		for (auto it = gQueue.begin(); it != gQueue.end(); ++it)
		{
			if (it->serial <= gDrawSerial) { mine.push_back(std::move(*it)); }
			else { *keep++ = std::move(*it); }
		}
		gQueue.erase(keep, gQueue.end());
	}
	std::vector<Finished> done;
	done.reserve(mine.size());
	for (Request& r : mine)
	{
		Finished f;
		switch (r.kind)
		{
		case Request::kScreenshot: f.ok = readScreenshot(r); break;
		case Request::kProbe: f.ok = readProbe(r, f.values, f.info); break;
		case Request::kSample: if (r.read) { f.values = r.read(); } f.ok = !f.values.empty(); break;
		case Request::kAfter: f.ok = true; break;
		}
		f.done = std::move(r.done);
		f.probeDone = std::move(r.probeDone);
		f.sampleDone = std::move(r.sampleDone);
		done.push_back(std::move(f));
	}
	std::lock_guard<std::mutex> lock(gMtx);
	for (Finished& f : done) { gFinished.push_back(std::move(f)); }
}

void runDone()
{
	std::vector<Finished> list;
	{
		std::lock_guard<std::mutex> lock(gMtx);
		list.swap(gFinished);
	}
	// The dones read render-owned state (the draw's stats, the post's letterbox, the terrain's water chunks): with the draw idle, at the packet boundary (overlap
	// rule 12; free in off and lockstep). The shot frames only; the dones also write nothing the draw reads (kenshi-k16.2.205.12.108).
	if (!list.empty()) { logicthread::renderIdle(); }
	for (Finished& f : list)	// in request order; a request a done makes belongs to the packet being filled now
	{
		if (f.probeDone) { f.probeDone(f.ok, f.values, f.info); }
		else if (f.sampleDone) { f.sampleDone(f.ok, f.values); }
		else if (f.done) { f.done(f.ok); }
	}
}

void runWrites()
{
	Writer& wr = writer();
	std::deque<WriteResult> list;
	{
		std::lock_guard<std::mutex> lock(wr.m);
		list.swap(wr.results);
	}
	for (WriteResult& r : list) { r.wrote(r.ok, r.why); }	// in write order; never under the writer's lock (a done may take it, flushWrites)
}

std::vector<float> image_rgba32f(sg_image img, int w, int h)
{
	std::vector<float> out;
#if defined(SOKOL_GLCORE)
	out.assign((size_t)w * h * 4, 0.0f);
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sg_gl_query_image_info(img).tex[0], 0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, out.data());
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	sg_reset_state_cache();
#else
	(void)img; (void)w; (void)h;
#endif
	return out;
}

}
