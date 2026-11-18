// The sokol thread-owner assert (dev builds; docs/RENDER_THREAD.md review 2.1, kenshi-k16.2.205.12.19).
//
// sokol_gfx is single-threaded: its resource pools and its GL state are unsynchronised. Every sg_* call must come from the one thread that
// owns it (today the sapp thread; with render.thread, the render thread). sokol_impl.cpp builds sokol with SOKOL_TRACE_HOOKS in a dev
// build, and sgOwnerInstall() puts a hook on every traced sg_* entry point (every member of sg_trace_hooks) that aborts, with the call's
// name, both thread ids and a stack, when the calling thread is not the owner. On one thread it never fires.
//
// What it does not see: sokol runs a hook after the call's own work, so the abort follows the offending call (the stack still names its
// caller); sg_query_*, sg_isvalid, sg_setup / sg_shutdown and the commit listeners have no hook. The previous hooks are chained.
//
// --sg-owner-probe (dev flag): right after the install, a second thread calls sg_alloc_buffer() (a pool operation, no GL), which must abort.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sokol_gfx.h"

#include "dev/devhooks.hpp"

namespace dev
{
namespace
{
	std::atomic<std::thread::id> gOwner{};
	sg_trace_hooks gPrev = {};
	bool gProbe = false;
	bool gLogic = false;	// --sg-owner-logic
	bool gLogicLog = false;	// --sg-owner-logic=log: each distinct stack printed once, no abort (a survey of the logic-side callers)
	std::thread::id gInstaller{};	// the thread that ran sg_setup (the sapp thread). The scopes act only there: on the real logic thread
					// (render.thread=lockstep) the owner already is the render thread, and a scope there would hand sokol to no one under it

	std::string idText(std::thread::id id)
	{
		std::ostringstream s;
		s << id;
		return s.str();
	}

	void offThread(const char* fn)
	{
		const std::thread::id self = std::this_thread::get_id();
		if (gOwner.load(std::memory_order_relaxed) == std::thread::id() && gLogicLog)
		{
			static std::mutex mu;
			static std::set<std::vector<void*>> seen;
			void* frames[24];
			const int n = backtrace(frames, 24);
			std::lock_guard<std::mutex> lk(mu);
			if (!seen.insert(std::vector<void*>(frames + 1, frames + n)).second) { return; }
			fprintf(stderr, "kclient: sg-owner-logic: sg_%s from the logic half (stack %zu; addr2line -f -C -i -e kclient ADDR):\n", fn, seen.size());
			fflush(stderr);
			backtrace_symbols_fd(frames, n, 2);
			return;
		}
		if (gOwner.load(std::memory_order_relaxed) == std::thread::id())
		{
			fprintf(stderr, "kclient: sokol thread-owner assert: sg_%s called from the logic half (--sg-owner-logic: logic_frame owns no sokol; queue it, renderqueue.hpp)\n", fn);
			fflush(stderr);
			std::abort();
		}
		fprintf(stderr, "kclient: sokol thread-owner assert: sg_%s called on thread %s, sokol is owned by thread %s (dev/sgowner.cpp)\n", fn,
			idText(self).c_str(), idText(gOwner.load(std::memory_order_relaxed)).c_str());
		fflush(stderr);
		std::abort();	// kutil's crash handler prints the stack and writes the crash report
	}

	// One hook per sg_trace_hooks member: check the thread, then call the hook that was there before. The hooks' user_data is the
	// previous hooks' user_data, so the arguments forward unchanged.
	template <auto M> struct Hook;
	template <typename... A, void (*sg_trace_hooks::*M)(A...)> struct Hook<M>
	{
		static inline const char* name = "?";
		static void fn(A... a)
		{
			if (std::this_thread::get_id() != gOwner.load(std::memory_order_relaxed)) { offThread(name); }
			if (gPrev.*M) { (gPrev.*M)(a...); }
		}
	};

#define KCLIENT_SG_HOOKS(X) \
	X(reset_state_cache) X(make_buffer) X(make_image) X(make_sampler) X(make_shader) X(make_pipeline) X(make_view) \
	X(destroy_buffer) X(destroy_image) X(destroy_sampler) X(destroy_shader) X(destroy_pipeline) X(destroy_view) \
	X(write_buffer_transient) X(write_image_transient) X(write_buffer_unsealed) X(write_image_unsealed) X(seal_buffer) X(seal_image) \
	X(copy_buffer_to_buffer) X(copy_buffer_to_image) X(copy_image_to_image) \
	X(begin_pass) X(apply_viewport) X(apply_scissor_rect) X(apply_pipeline) X(apply_bindings) X(apply_uniforms) \
	X(draw) X(draw_ex) X(dispatch) X(end_pass) X(commit) \
	X(alloc_buffer) X(alloc_image) X(alloc_sampler) X(alloc_shader) X(alloc_pipeline) X(alloc_view) \
	X(dealloc_buffer) X(dealloc_image) X(dealloc_sampler) X(dealloc_shader) X(dealloc_pipeline) X(dealloc_view) \
	X(init_buffer) X(init_image) X(init_sampler) X(init_shader) X(init_pipeline) X(init_view) \
	X(uninit_buffer) X(uninit_image) X(uninit_sampler) X(uninit_shader) X(uninit_pipeline) X(uninit_view) \
	X(fail_buffer) X(fail_image) X(fail_sampler) X(fail_shader) X(fail_pipeline) X(fail_view) \
	X(push_debug_group) X(pop_debug_group)

#define KCLIENT_SG_COUNT(n) + 1
	// a sokol update that adds a hook must add it here too
	static_assert(sizeof(sg_trace_hooks) == (1 KCLIENT_SG_HOOKS(KCLIENT_SG_COUNT)) * sizeof(void*),
		"sg_trace_hooks has changed: update KCLIENT_SG_HOOKS in dev/sgowner.cpp");
#undef KCLIENT_SG_COUNT
}

bool sgOwnerArg(const std::string& a)
{
	if (a == "--sg-owner-probe") { gProbe = true; return true; }
	if (a == "--sg-owner-logic") { gLogic = true; return true; }
	if (a == "--sg-owner-logic=log") { gLogic = gLogicLog = true; return true; }
	return false;
}

// --sg-owner-logic: the owner is "no thread" (a default id, which no running thread has) inside the logic half, and given back for an allowed
// caller. Scopes nest: each restores the owner it found.
SgLogicScope::SgLogicScope() : prev(gOwner.load(std::memory_order_relaxed))
{
	if (gLogic && std::this_thread::get_id() == gInstaller) { gOwner.store(std::thread::id(), std::memory_order_relaxed); }
}

SgLogicScope::~SgLogicScope()
{
	if (gLogic && std::this_thread::get_id() == gInstaller) { gOwner.store(prev, std::memory_order_relaxed); }
}

SgAllowScope::SgAllowScope(const char* why) : prev(gOwner.load(std::memory_order_relaxed))
{
	(void)why;
	if (gLogic && std::this_thread::get_id() == gInstaller) { gOwner.store(std::this_thread::get_id(), std::memory_order_relaxed); }
}

SgAllowScope::~SgAllowScope()
{
	if (gLogic && std::this_thread::get_id() == gInstaller) { gOwner.store(prev, std::memory_order_relaxed); }
}

void sgOwnerInstall()
{
	gInstaller = std::this_thread::get_id();
	gOwner.store(std::this_thread::get_id(), std::memory_order_relaxed);
	sg_trace_hooks h = {};
#define KCLIENT_SG_SET(n) Hook<&sg_trace_hooks::n>::name = #n; h.n = Hook<&sg_trace_hooks::n>::fn;
	KCLIENT_SG_HOOKS(KCLIENT_SG_SET)
#undef KCLIENT_SG_SET
	gPrev = sg_install_trace_hooks(&h);	// sokol has no query for the installed hooks: install once to learn them
	h.user_data = gPrev.user_data;	// then again with the previous user_data, which the hooks forward unchanged (see Hook)
	sg_install_trace_hooks(&h);
	if (gProbe)
	{
		fprintf(stderr, "kclient: --sg-owner-probe: sg_alloc_buffer() from a second thread (must abort)\n");
		fflush(stderr);
		std::thread t([] { sg_alloc_buffer(); });
		t.join();
		fprintf(stderr, "kclient: --sg-owner-probe: the off-thread call did NOT abort\n");
	}
}

void sgOwnerTake()
{
	gOwner.store(std::this_thread::get_id(), std::memory_order_relaxed);
}
}
