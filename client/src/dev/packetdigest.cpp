// The packet digest and poison (dev/packetdigest.hpp). Field by field, never raw struct bytes: padding is indeterminate.
#include "dev/packetdigest.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "frame/renderframe.hpp"

namespace dev
{
namespace
{
	bool envOn(const char* name)
	{
		const char* v = getenv(name);
		return v && *v && strcmp(v, "0") != 0;
	}

	// ---- digest ----
	struct H
	{
		uint64_t h = 1469598103934665603ull;
		void bytes(const void* p, size_t n)
		{
			const unsigned char* b = (const unsigned char*)p;
			for (size_t i = 0; i < n; i++) { h = (h ^ b[i]) * 1099511628211ull; }
		}
		template <class T> void v(const T& x)
		{
			static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
			bytes(&x, sizeof(T));
		}
		template <class T, size_t N> void a(const T (&x)[N]) { for (size_t i = 0; i < N; i++) { v(x[i]); } }
		template <class T> void arr(const T* x, size_t n) { for (size_t i = 0; i < n; i++) { v(x[i]); } }
		void s(const std::string& x) { v(x.size()); bytes(x.data(), x.size()); }
		template <class T> void flat(const std::vector<T>& x) { v(x.size()); arr(x.data(), x.size()); }	// scalars only
		template <class F, class T> void each(const std::vector<T>& x, F f) { v(x.size()); for (const T& e : x) { f(e); } }
	};

	void hCamera(H& h, const Camera& c)
	{
		h.a(c.pos); h.a(c.f); h.a(c.r); h.a(c.u);
		h.v(c.fovY); h.v(c.tanY); h.v(c.aspect); h.v(c.nearZ); h.v(c.farZ); h.v(c.width); h.v(c.height); h.v(c.focus);
	}

	void hLight(H& h, const LightParams& l)
	{
		for (const auto& r : l.v) { h.a(r); }
		h.a(l.plHead);
		for (const auto& r : l.pl) { h.a(r); }
		for (const auto& r : l.sh) { h.a(r); }
	}

	void hPlan(H& h, const Characters::FrameData& c);
	void hChars(H& h, const Characters::FrameData& c)
	{
		h.flat(c.skins); h.flat(c.insts); h.flat(c.list); h.v(c.count);
		hPlan(h, c);
	}
	void hPlan(H& h, const Characters::FrameData& c)
	{
		const auto grp = [&](const std::vector<Characters::Group>& g)
		{
			h.each(g, [&](const Characters::Group& e)
			{
				h.v(e.vbuf.id != 0); h.v(e.vbuf2.id != 0); h.v(e.ibuf.id != 0); h.v(e.indexCount);	// the ids themselves are the render side's pool slots: they differ run to run
				for (const auto& view : e.v) { h.v(view.id != 0); }
				h.v(e.first); h.v(e.count);
			});
		};
		grp(c.plan.body); grp(c.plan.armour); grp(c.plan.hair); grp(c.plan.items); grp(c.plan.standin);
		for (const auto& s : c.plan.shadow) { grp(s); }
	}

	void hDl(H& h, const dl::List& l)
	{
		h.v(l.width()); h.v(l.height());
		h.each(l.cmds(), [&](const dl::Cmd& c)
		{
			h.v((int)c.kind); h.v((int)c.prim); h.v((int)c.pipe); h.v(c.layer); h.v(c.tex);
			h.v(c.clip.x); h.v(c.clip.y); h.v(c.clip.w); h.v(c.clip.h); h.v(c.first); h.v(c.count); h.v(c.host);
		});
		h.each(l.vertices(), [&](const dl::Vertex& x) { h.v(x.x); h.v(x.y); h.v(x.u); h.v(x.v); h.v(x.rgba); });
		h.each(l.hosts(), [&](const dl::Host& x) { h.v(x.r.x); h.v(x.r.y); h.v(x.r.w); h.v(x.r.h); h.v((bool)x.draw); });
		h.each(l.uploads(), [&](const dl::Upload& u)
		{
			h.v((int)u.op); h.v(u.id); h.v(u.x); h.v(u.y); h.v(u.w); h.v(u.h); h.v(u.mips); h.v((int)u.filter); h.v(u.updatable);
			h.bytes(u.label, strlen(u.label)); h.flat(u.rgba);
		});
	}

	void hOvl(H& h, const ovl::List& l)
	{
		h.each(l.calls(), [&](const ovl::Call& c) { h.v((int)c.op); h.v(c.a); h.v(c.b); h.a(c.f); });
		h.s(l.text()); h.v(l.vertices());
	}

	using Block = std::pair<const char*, uint64_t>;

	std::vector<Block> blocks(const FramePacket& pk)
	{
		std::vector<Block> out;
		const auto blk = [&](const char* name, auto fill) { H h; fill(h); out.emplace_back(name, h.h); };
		blk("head", [&](H& h) { h.v((int)pk.path); h.v(pk.dt); h.v(pk.editor); h.v(pk.width); h.v(pk.height); h.v(pk.worldMap); h.s(pk.cursor); h.v(pk.look); });
		blk("views", [&](H& h) { h.v(pk.viewCount); for (const Camera& c : pk.views) { hCamera(h, c); } });
		blk("grass.head", [&](H& h) { h.v(pk.grass.ready); h.a(pk.grass.cam); });
		// Grass tiles and foliage cells arrive from worker threads in no fixed order: their keys, cell indexes and ranges differ between two runs of
		// the same mode. Hash what the order cannot change: the counts and sums.
		blk("grass.tiles", [&](H& h)
		{
			int sum = 0;
			for (const auto& r : pk.grass.ranges) { sum += r.count; }
			h.v(pk.grass.tiles.size()); h.v(pk.grass.ranges.size()); h.v(sum);
		});
		blk("particles", [&](H& h)
		{
			const auto& p = pk.particles;
			h.flat(p.inst); h.v(p.count); h.v(p.depthTest);
			h.each(p.batches, [&](const Particles::Batch& b) { h.v(b.tex); h.v(b.additive); h.v(b.clip); h.v(b.first); h.v(b.count); h.v(b.soft); });
			h.each(p.texNames, [&](const std::string& s) { h.s(s); });
		});
		blk("shadow", [&](H& h)
		{
			const auto& s = pk.shadow;
			h.bytes(&s.params, sizeof s.params);
			for (const auto& c : s.cascade) { h.bytes(&c, sizeof c); }
			for (const auto& g : s.geom) { h.a(g); }
			for (bool r : s.render) { h.v(r); }
			h.v(s.enabled); h.v(s.cacheOn); h.v(s.set); h.v(s.staticBudget);
		});
		blk("buildings", [&](H& h)
		{
			const auto& b = pk.buildings;
			h.flat(b.inst); h.v(b.lightCount);
			for (const auto& r : b.lights) { h.a(r); }
			h.a(b.lightOwner); h.a(b.lightLamp); h.v(b.shadowSig);
		});
		blk("chars.skins", [&](H& h) { h.flat(pk.chars.skins); h.v(pk.chars.count); });
		blk("chars.insts", [&](H& h) { h.flat(pk.chars.insts); });
		blk("chars.list", [&](H& h) { h.flat(pk.chars.list); });
		blk("chars.plan", [&](H& h) { hPlan(h, pk.chars); });
		blk("foliage.gen", [&](H& h) { h.v(pk.foliage.gen); h.a(pk.foliage.cam); });
		blk("foliage.draws", [&](H& h)
		{
			const auto& f = pk.foliage;
			long sum = 0;
			for (const auto& r : f.list.opaque) { sum += r.count; }
			h.v(f.draws.size()); h.v(sum);
			for (const auto& sl : f.shadow)
			{
				h.v(sl != nullptr);
				if (!sl) { continue; }
				long cs = 0;
				for (const auto& c : sl->cands) { cs += c.count; }
				h.a(sl->origin); h.v(sl->cands.size()); h.v(cs);
			}
		});
		blk("ui", [&](H& h) { hDl(h, pk.ui); h.v(pk.uiDrawn); });
		blk("labels", [&](H& h) { hDl(h, pk.labels); h.v(pk.labelsDrawn); });
		blk("sky", [&](H& h)
		{
			const auto& s = pk.sky;
			hLight(h, s.light); h.a(s.sunDir); h.a(s.sunColour); h.v(s.starScroll); h.v(s.hours); h.v(s.day); h.a(s.skyColourMult);
			h.v(s.cloudsDensity); h.v(s.clouds); h.a(s.windAcc); h.v(s.minLum); h.v(s.maxLum); h.v(s.heatHaze); h.v(pk.lightOff);
		});
		blk("post", [&](H& h) { h.v(pk.post.eyeAdapt); h.v(pk.post.scriptedJump); h.v(pk.post.fixedExposure); });
		blk("scene", [&](H& h)
		{
			const auto& s = pk.scene;
			h.v(s.bldSet); h.a(s.pos); h.v(s.foliageOn); h.v(s.grassOn); h.v(s.animTime); h.v(s.waterOn); h.v(s.scripted); h.v(s.rain);
			h.v(s.terrainLoads); h.v(s.terrainView); h.v(s.squads); h.a(s.mouse); h.v(s.sunShafts); h.v(s.dustMotes); h.v(s.sunShaftsDebug); h.v(s.windowW); h.v(s.windowH);
			h.v(s.simActive); h.v(s.interiorLight); h.v(s.sunBounce); h.v(s.lampOcclusion); h.v(s.interiorMask); h.v(s.groundOk); h.a(s.ground);
		});
		blk("overlay", [&](H& h)
		{
			const auto& o = pk.overlay;
			h.v(o.debug); h.v(o.hideUi); h.v(o.title); h.v(o.creatorShown); h.v(o.simActive); h.v(o.sessionLoad); h.v(o.sessionProgress);
		});
		blk("overlayLists", [&](H& h)
		{
			const auto& o = pk.overlayLists;
			hOvl(h, o.debug); hOvl(h, o.trails); h.v(o.trailsOn); hOvl(h, o.markers); hOvl(h, o.mapHud); h.v(o.width); h.v(o.height);
		});
		blk("arrows", [&](H& h) { h.each(pk.arrows, [&](const Arrows::Mark& m) { h.a(m.pos); h.a(m.dir); h.a(m.colour); }); });
		blk("portraits", [&](H& h)
		{
			h.each(pk.portraitJobs, [&](const PortraitJob& j)
			{
				h.v(j.key); h.v(j.index); h.v(j.serial); h.v(j.cell); h.v(j.run); h.v(j.posed); h.a(j.target); h.a(j.eye); hChars(h, j.frame);
			});
		});
		blk("early", [&](H& h)
		{
			const auto& e = pk.early;
			h.a(e.mouse); h.v(e.sessionLoad); h.v(e.loadFrac); h.s(e.loadStep); h.a(e.viewPos); hOvl(h, e.viewText);
		});
		blk("lab", [&](H& h)
		{
			const auto& l = pk.lab;
			h.v(l.draw); hChars(h, l.frame); hLight(h, l.light); h.a(l.mvp); h.a(l.eye); h.v(l.w); h.v(l.h); h.v(l.boneNames);
			h.each(l.lines, [&](const LabFrame::Line& x) { h.v(x.x0); h.v(x.y0); h.v(x.x1); h.v(x.y1); h.a(x.c); });
			h.each(l.labels, [&](const LabFrame::Label& x) { h.v(x.x); h.v(x.y); h.s(x.text); });
		});
		return out;
	}

	// ---- poison ----
	constexpr float kNanF = std::numeric_limits<float>::quiet_NaN();
	constexpr double kNanD = std::numeric_limits<double>::quiet_NaN();
	template <size_t N> void nan(float (&a)[N]) { for (float& x : a) { x = kNanF; } }
	template <size_t N> void nan(double (&a)[N]) { for (double& x : a) { x = kNanD; } }

	void poisonLight(LightParams& l)
	{
		for (auto& r : l.v) { nan(r); }
		nan(l.plHead);
		for (auto& r : l.pl) { nan(r); }
		for (auto& r : l.sh) { nan(r); }
	}
}

void packetPoison(FramePacket& pk)
{
	static const bool on = envOn("KCLIENT_PACKET_POISON");
	if (!on) { return; }
	// scalars: path stays (logic_paths sets it first thing; kBlank would be a lie), serial is set by beginPacket right after
	pk.dt = kNanD; pk.logicMs = kNanD;
	pk.editor = true;
	pk.viewCount = -1;
	for (Camera& c : pk.views)
	{
		nan(c.pos); nan(c.f); nan(c.r); nan(c.u);
		c.fovY = c.tanY = c.aspect = c.nearZ = c.farZ = c.focus = kNanF;
		c.width = c.height = -1;
	}
	pk.width = pk.height = -1;
	// containers
	pk.shadow = {}; pk.shadow.enabled = false; pk.shadow.set = -1; pk.shadow.staticBudget = -1; nan(pk.shadow.params.sinfo);
	pk.grass = {}; pk.grass.ready = true; nan(pk.grass.cam);
	pk.particles.clear(); pk.particles.depthTest = false;
	pk.buildings = {}; pk.buildings.lightCount = -1; pk.buildings.shadowSig = ~0ull;
	for (auto& r : pk.buildings.lights) { nan(r); }
	for (auto& o : pk.buildings.lightOwner) { o = 0xFFFFFFFFu; }
	for (auto& l : pk.buildings.lightLamp) { l = -1; }
	pk.chars.clear(); pk.chars.count = (size_t)-1;
	pk.foliage = {}; nan(pk.foliage.cam); pk.foliage.gen = ~0ull;
	pk.cursor = "\x01poison"; pk.look = true;
	pk.ui = {}; pk.labels = {}; pk.uiDrawn = pk.labelsDrawn = true;
	pk.sky = {}; poisonLight(pk.sky.light); nan(pk.sky.sunDir); nan(pk.sky.sunColour); pk.sky.starScroll = pk.sky.hours = kNanD; pk.sky.day = -1;
	nan(pk.sky.skyColourMult); pk.sky.cloudsDensity = pk.sky.minLum = pk.sky.maxLum = pk.sky.heatHaze = kNanF; nan(pk.sky.windAcc); pk.sky.clouds = true;
	pk.lightOff = 0xFFFFFFFFu;
	pk.post.eyeAdapt = pk.post.scriptedJump = true; pk.post.fixedExposure = kNanF;
	{
		auto& s = pk.scene;
		s = {};
		s.bldSet = s.foliageOn = s.grassOn = s.waterOn = s.scripted = s.simActive = s.lampOcclusion = s.interiorMask = s.groundOk = true;
		nan(s.pos); s.animTime = kNanD; s.rain = s.interiorLight = s.sunBounce = s.sunShafts = s.dustMotes = kNanF;
		s.terrainLoads = s.terrainView = s.squads = s.sunShaftsDebug = s.windowW = s.windowH = -1;
		nan(s.mouse); nan(s.ground);
	}
	{
		auto& o = pk.overlay;
		o = {};
		o.debug = o.hideUi = o.title = o.creatorShown = o.simActive = o.sessionLoad = true; o.sessionProgress = kNanF;
	}
	pk.overlayLists = {}; pk.overlayLists.trailsOn = true; pk.overlayLists.width = pk.overlayLists.height = kNanF;
	pk.arrows.clear(); pk.portraitJobs.clear();
	pk.lab = {}; pk.lab.draw = false;	// the lab draws only on a non-null chars + sokol handles: leave it off (a poisoned handle would abort the draw, not show a diff)
	pk.worldMap = true;
	{
		auto& e = pk.early;
		e = {};
		nan(e.mouse); e.sessionLoad = true; e.loadFrac = kNanF; e.loadStep = "\x01poison"; nan(e.viewPos);
	}
}

void packetDigest(const FramePacket& pk)
{
	static const bool on = envOn("KCLIENT_PACKET_DIGEST");
	if (!on) { return; }
	std::string line = "PKTDIGEST serial=" + std::to_string((unsigned long long)pk.serial) + " path=" + std::to_string((int)pk.path);
	char buf[48];
	for (const Block& b : blocks(pk))
	{
		snprintf(buf, sizeof buf, " %s=%016llx", b.first, (unsigned long long)b.second);
		line += buf;
	}
	fprintf(stderr, "%s\n", line.c_str());
}
}
