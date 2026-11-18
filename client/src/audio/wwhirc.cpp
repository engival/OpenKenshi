#include "audio/wwhirc.hpp"
#include "audio/wwbank.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio::hirc
{
	namespace
	{
		constexpr uint32_t kEntryCue = 43573010, kExitCue = 1539036744;	// wwiser generator/render/bnode_markers.py

		// Bounds-checked little-endian cursor; reads past the end return 0 and set bad.
		struct Reader
		{
			const uint8_t* p;
			size_t pos, end;
			bool bad = false;

			bool take(void* out, size_t n)
			{
				if (pos + n > end) { bad = true; pos = end; memset(out, 0, n); return false; }
				memcpy(out, p + pos, n);
				pos += n;
				return true;
			}
			template <class T> T get() { T v; take(&v, sizeof v); return v; }
			uint8_t u8() { return get<uint8_t>(); }
			uint16_t u16() { return get<uint16_t>(); }
			uint32_t u32() { return get<uint32_t>(); }
			int32_t s32() { return get<int32_t>(); }
			int16_t s16() { return get<int16_t>(); }
			float f32() { return get<float>(); }
			double f64() { return get<double>(); }
			void skip(size_t n) { if (pos + n > end) { bad = true; pos = end; } else pos += n; }
			std::vector<uint32_t> ids() { std::vector<uint32_t> v(std::min<size_t>(u32(), (end - pos) / 4)); for (uint32_t& x : v) x = u32(); return v; }
		};

		struct Props { std::vector<std::pair<uint8_t, uint32_t>> v; bool has(uint8_t id, uint32_t& raw) const { for (auto& e : v) if (e.first == id) { raw = e.second; return true; } return false; } };

		// AkPropBundle.
		Props readProps(Reader& r)
		{
			Props out;
			uint8_t n = r.u8();
			std::vector<uint8_t> ids(n);
			for (uint8_t& id : ids) id = r.u8();
			for (uint8_t id : ids) out.v.push_back({ id, r.u32() });
			return out;
		}
		float propFloat(uint32_t raw) { float f; memcpy(&f, &raw, 4); return f; }
		// AkPropBundle<RANGED_MODIFIERS>: (min, max) per prop.
		std::vector<RangedProp> readRangedProps(Reader& r)
		{
			uint8_t n = r.u8();
			std::vector<RangedProp> v(n);
			for (RangedProp& p : v) p.id = r.u8();
			for (RangedProp& p : v) { p.min = propFloat(r.u32()); p.max = propFloat(r.u32()); }
			return v;
		}
		void skipRangedProps(Reader& r) { readRangedProps(r); }

		std::vector<Point> readPoints(Reader& r, size_t n)
		{
			if (n * 12 > r.end - r.pos) { r.bad = true; return {}; }
			std::vector<Point> v(n);
			for (Point& p : v) { p.x = r.f32(); p.y = r.f32(); p.curve = Curve(std::min<uint32_t>(r.u32(), kConstant)); }
			return v;
		}
		// SetInitialRTPC<CAkParameterNodeBase> (v<=113).
		std::vector<Rtpc> readRtpcs(Reader& r)
		{
			std::vector<Rtpc> v;
			for (uint16_t n = r.u16(); n && !r.bad; n--)
			{
				Rtpc c;
				c.id = r.u32();
				c.type = r.u8();
				c.accum = r.u8();
				c.param = r.u8();
				c.curveId = r.u32();
				c.scaling = r.u8();
				c.points = readPoints(r, r.u16());
				v.push_back(std::move(c));
			}
			return v;
		}

		// CAkParameterNodeBase::SetNodeBaseParams, v112 (tools/wwise_bnk.py read_node_base).
		void readNodeBase(Reader& r, bool feedback, Params& p)
		{
			r.u8();	// fx override
			if (uint8_t nfx = r.u8()) { r.u8(); r.skip(size_t(nfx) * 7); }
			r.u8();	// override attachment
			p.bus = r.u32();
			p.parent = r.u32();
			r.u8();	// priority bits
			for (auto& [id, raw] : readProps(r).v) p.props.push_back({ id, raw });
			p.ranged = readRangedProps(r);
			uint8_t pos = r.u8();
			p.posOverride = pos & 1;
			p.is3d = pos >> 3 & 1;
			if (p.posOverride && p.is3d)
			{
				uint8_t bits3d = r.u8();
				p.attenuation = r.u32();
				if ((bits3d & 3) != 1)
				{
					r.u8();
					r.s32();
					r.skip(size_t(r.u32()) * 16);	// vertices
					uint32_t items = r.u32();
					r.skip(size_t(items) * 8 + size_t(items) * 12);	// path items + automation
				}
			}
			if (r.u8() >> 3 & 1) r.skip(16);	// aux sends
			r.skip(6);	// advanced settings
			for (uint32_t g = r.u32(); g && !r.bad; g--)
			{
				p.stateGroups.push_back(r.u32());
				r.u8();	// sync type
				r.skip(size_t(r.u16()) * 8);
			}
			p.rtpcs = readRtpcs(r);
			if (feedback) r.u32();
		}

		void readMusicNode(Reader& r, bool feedback, Node& n)
		{
			r.u8();	// flags
			Params p;
			readNodeBase(r, feedback, p);
			n.parent = p.parent;
			n.volumeDb = p.f(kPropVolume);
			n.children = r.ids();
			n.meter.gridMs = r.f64();
			n.meter.gridOffsetMs = r.f64();
			n.meter.bpm = r.f32();
			n.meter.beatsPerBar = r.u8();
			n.meter.beatValue = r.u8();
			r.u8();	// meter override
			r.skip(size_t(r.u32()) * 24);	// stingers
		}

		Fade readFade(Reader& r)
		{
			Fade f;
			f.ms = r.s32();
			f.curve = Curve(std::min<uint32_t>(r.u32(), kConstant));
			f.offsetMs = r.s32();
			return f;
		}

		void readRules(Reader& r, Node& n)
		{
			for (uint32_t k = r.u32(); k && !r.bad; k--)
			{
				Rule ru;
				ru.src = r.ids();
				ru.dst = r.ids();
				ru.srcFade = readFade(r);
				ru.sync = r.u32();
				r.u32();	// cue filter
				ru.postExit = r.u8();
				ru.dstFade = readFade(r);
				r.u32();	// cue filter
				ru.jumpTo = r.u32();
				ru.entry = r.u16();
				ru.preEntry = r.u8();
				r.u8();	// match source cue name
				if (r.u8())
				{
					ru.transitionSegment = r.u32();
					readFade(r);
					readFade(r);
					r.skip(2);
				}
				n.rules.push_back(std::move(ru));
			}
		}

		Item readItem(Reader& r, uint32_t& count, int depth)
		{
			Item it;
			count++;
			it.segment = r.u32();
			it.id = r.u32();
			uint32_t nchild = r.u32();
			it.type = r.s32();
			it.loop = r.s16();
			r.s16();	// loop min
			r.s16();	// loop max
			it.weight = r.u32();
			it.avoidRepeat = r.u16();
			it.useWeight = r.u8();
			it.shuffle = r.u8();
			if (depth > 32) { r.bad = true; return it; }
			for (uint32_t i = 0; i < nchild && !r.bad; i++) it.children.push_back(readItem(r, count, depth + 1));
			return it;
		}

		bool readTree(const uint8_t* blob, size_t size, uint32_t depth, TreeNode& root)
		{
			if (size % 12) return false;
			size_t n = size / 12;
			if (!n) return true;
			std::vector<uint8_t> seen(n);
			auto field = [&](size_t i, size_t off) { uint32_t v; memcpy(&v, blob + i * 12 + off, 4); return v; };
			auto build = [&](auto& self, size_t i, uint32_t level, TreeNode& out) -> bool
			{
				if (i >= n || seen[i]) return false;
				seen[i] = 1;
				out.key = field(i, 0);
				uint32_t a = field(i, 4);
				if (level == depth) { out.audio = a; return true; }
				uint32_t idx = a & 0xFFFF, cnt = a >> 16;
				out.children.resize(cnt);
				for (uint32_t k = 0; k < cnt; k++)
				{
					if (!self(self, idx + k, level + 1, out.children[k])) return false;
				}
				return true;
			};
			return build(build, 0, 0, root);
		}
	}

	const Node* Hierarchy::node(uint32_t id) const
	{
		if (auto it = segments.find(id); it != segments.end()) return &it->second;
		if (auto it = ranseqs.find(id); it != ranseqs.end()) return &it->second;
		if (auto it = switches.find(id); it != switches.end()) return &it->second;
		return nullptr;
	}

	const Object* Hierarchy::object(uint32_t id) const
	{
		if (auto it = sounds.find(id); it != sounds.end()) return &it->second;
		if (auto it = containers.find(id); it != containers.end()) return &it->second;
		if (auto it = switchCntrs.find(id); it != switchCntrs.end()) return &it->second;
		if (auto it = layerCntrs.find(id); it != layerCntrs.end()) return &it->second;
		if (auto it = mixers.find(id); it != mixers.end()) return &it->second;
		return nullptr;
	}

	bool Params::has(uint8_t id, uint32_t& raw) const
	{
		for (const Prop& p : props) if (p.id == id) { raw = p.raw; return true; }
		return false;
	}
	float Params::f(uint8_t id, float def) const { uint32_t raw; return has(id, raw) ? propFloat(raw) : def; }
	int32_t Params::i(uint8_t id, int32_t def) const { uint32_t raw; return has(id, raw) ? int32_t(raw) : def; }

	// CAkConversionTable lookup (wwiser bnode_rtpc.py _find): each segment interpolates with its first point's curve, Constant holds; the end
	// values hold outside the points.
	float evalPoints(const std::vector<Point>& points, float x)
	{
		if (points.empty()) return 0;
		if (x <= points.front().x) return points.front().y;
		for (size_t k = 1; k < points.size(); k++)
		{
			const Point &a = points[k - 1], &b = points[k];
			if (x >= b.x) continue;
			if (a.curve == kConstant) return a.y;
			return a.y + (b.y - a.y) * curveShape(a.curve, (x - a.x) / (b.x - a.x));
		}
		return points.back().y;
	}

	// Scaling (ApplyCurveScaling, v>=72): 0 none, 2 dB from the stored [-1, 1] (20 log10(y + 1); -1 = silence), 3 log, 4 dB to linear.
	float Rtpc::eval(float x) const
	{
		float y = evalPoints(points, x);
		switch (scaling)
		{
			case 2: y = std::clamp(y, -1.0f, 1.0f); return y <= -1 ? -96.3f : 20 * std::log10(y + 1);
			case 3: return std::pow(10.0f, y / 20);
			case 4: return std::pow(10.0f, y * 0.05f);
			default: return y;
		}
	}

	static float attEval(const AttCurve& c, float x)
	{
		float y = evalPoints(c.points, x);
		return c.scaling == 2 ? (std::clamp(y, -1.0f, 1.0f) <= -1 ? -96.3f : 20 * std::log10(std::clamp(y, -1.0f, 1.0f) + 1)) : y;
	}
	float Attenuation::volumeDb(float d) const { const AttCurve* c = volume(); return c ? attEval(*c, d) : 0; }
	float Attenuation::lpf(float d) const { const AttCurve* c = lowPass(); return c ? attEval(*c, d) : 0; }
	float Attenuation::maxDistance() const
	{
		float m = 0;
		for (const AttCurve& c : curves) for (const Point& p : c.points) m = std::max(m, p.x);
		return m;
	}

	Hierarchy::Positioning Hierarchy::positioning(uint32_t id) const
	{
		for (; id; )
		{
			const Object* o = object(id);
			if (!o) break;
			if (o->p.posOverride) return { o->p.is3d, o->p.attenuation };
			id = o->p.parent;
		}
		return {};
	}

	bool Hierarchy::add(const Bank& bank)
	{
		uint32_t size;
		const uint8_t* data = bank.hirc(size);
		for (const Media& m : bank.media) media.emplace(m.id, &bank);
		if (!data) return true;
		if (bank.version != 112) { error_ = bank.path + ": HIRC objects are only decoded for bank version 112 (this is " + std::to_string(bank.version) + ")"; return false; }
		Reader top { data, 0, size };
		uint32_t count = top.u32();
		bool feedback = bank.feedback != 0;
		for (uint32_t i = 0; i < count; i++)
		{
			uint8_t type = top.u8();
			uint32_t osz = top.u32();
			if (top.bad || top.pos + osz > size) { error_ = bank.path + ": truncated HIRC"; return false; }
			Reader r { data, top.pos, top.pos + osz };
			top.skip(osz);
			uint32_t id = r.u32();
			bool decoded = true;
			switch (type)
			{
				case 0x03:
				{
					Action a;
					a.type = r.u16();
					a.target = r.u32();
					r.u8();
					Props props = readProps(r);
					uint32_t raw;
					if (props.has(kPropTransitionTime, raw)) a.fadeMs = int32_t(raw);
					if (props.has(kPropDelayTime, raw)) a.delayMs = int32_t(raw);
					skipRangedProps(r);
					uint8_t cls = a.cls();
					if (cls == 0x01 || cls == 0x02 || cls == 0x03 || cls == 0x06 || cls == 0x07 || (cls >= 0x08 && cls <= 0x0F) || cls == 0x13 || cls == 0x14 || cls == 0x20 || cls == 0x22 || cls == 0x30)
					{
						a.curve = Curve(std::min<uint8_t>(r.u8() & 0x1F, kConstant));
						if (cls == 0x02 || cls == 0x03) r.u8();
						else if (cls == 0x13 || cls == 0x14) r.skip(1 + 1 + 12);
						else if (cls >= 0x08 && cls <= 0x0F)
						{
							a.propMeaning = r.u8();
							a.propBase = r.f32();
							a.propMin = r.f32();
							a.propMax = r.f32();
						}
						else if (cls != 0x01 && cls != 0x06 && cls != 0x07 && cls != 0x22) r.skip(1 + 12);
						r.skip(size_t(r.u32()) * 5);	// except list
					}
					else if (cls == 0x04 || cls == 0x05 || cls == 0x23) { a.curve = Curve(std::min<uint8_t>(r.u8() & 0x1F, kConstant)); r.u32(); }
					else if (cls == 0x12 || cls == 0x19) { a.group = r.u32(); a.value = r.u32(); }
					else if (cls == 0x1A || cls == 0x1B) { r.skip(2); r.skip(size_t(r.u32()) * 5); }
					else if (cls == 0x1E) { r.skip(1 + 12 + 1); r.skip(size_t(r.u32()) * 5); }
					actions[id] = a;
					break;
				}
				case 0x04:
					events[id].actions = r.ids();
					break;
				case 0x0A:
				{
					Segment s;
					s.id = id;
					readMusicNode(r, feedback, s);
					s.durationMs = r.f64();
					s.exitMs = s.durationMs;
					for (uint32_t k = r.u32(); k && !r.bad; k--)
					{
						uint32_t mid = r.u32();
						double posMs = r.f64();
						r.skip(r.u32());
						if (mid == kEntryCue) s.entryMs = posMs;
						else if (mid == kExitCue) s.exitMs = posMs;
					}
					segments.emplace(id, std::move(s));
					break;
				}
				case 0x0B:
				{
					Track t;
					t.id = id;
					r.u8();
					for (uint32_t k = r.u32(); k && !r.bad; k--)
					{
						uint32_t plugin = r.u32();
						uint8_t stream = r.u8();
						t.sources.push_back(r.u32());
						r.u32();	// file id
						if (stream != 2) r.u32();
						r.u32();	// in-memory size
						r.u8();
						if ((plugin & 0xF) == 2 || (plugin & 0xF) == 5) r.skip(r.u32());
					}
					uint32_t nclips = r.u32();
					for (uint32_t k = 0; k < nclips && !r.bad; k++)
					{
						Clip c;
						r.u32();	// track index
						c.sourceId = r.u32();
						c.playAtMs = r.f64();
						c.beginTrimMs = r.f64();
						c.endTrimMs = r.f64();
						c.srcDurationMs = r.f64();
						t.clips.push_back(c);
					}
					if (nclips) r.u32();	// sub-tracks
					for (uint32_t k = r.u32(); k && !r.bad; k--)
					{
						r.skip(8);
						r.skip(size_t(r.u32()) * 12);
					}
					Params p;
					readNodeBase(r, feedback, p);
					t.volumeDb = p.f(kPropVolume);
					t.type = r.u8();
					if (t.type == 3)
					{
						r.skip(1 + 4 + 4);
						r.skip(size_t(r.u32()) * 4);
						r.skip(12 + 4 + 4 + 12);
					}
					r.s32();	// look-ahead
					tracks.emplace(id, std::move(t));
					break;
				}
				case 0x0D:
				{
					RanSeq s;
					s.id = id;
					readMusicNode(r, feedback, s);
					readRules(r, s);
					uint32_t declared = r.u32(), parsed = 0;
					s.root = readItem(r, parsed, 0);
					if (parsed != declared) r.bad = true;
					ranseqs.emplace(id, std::move(s));
					break;
				}
				case 0x0C:
				{
					Switch s;
					s.id = id;
					readMusicNode(r, feedback, s);
					readRules(r, s);
					s.continuePlayback = r.u8();
					uint32_t depth = r.u32();
					if (depth > 8) { r.bad = true; break; }
					for (uint32_t k = 0; k < depth; k++) s.groups.push_back(r.u32());
					for (uint32_t k = 0; k < depth; k++) s.groupIsState.push_back(r.u8());
					uint32_t treeSize = r.u32();
					r.u8();	// mode
					if (r.pos + treeSize > r.end || !readTree(data + r.pos, treeSize, depth, s.tree)) { r.bad = true; break; }
					r.skip(treeSize);
					auto [it, fresh] = switches.emplace(id, s);
					if (!fresh)
					{
						for (uint32_t c : s.children)
						{
							if (std::find(it->second.children.begin(), it->second.children.end(), c) == it->second.children.end()) it->second.children.push_back(c);
						}
					}
					break;
				}
				case 0x02:	// CAkSound::SetInitialValues
				{
					Sound s;
					s.id = id;
					s.plugin = r.u32();
					s.stream = r.u8();
					s.source = r.u32();
					r.u32();	// file id
					if (s.stream != 2) r.u32();	// file offset
					r.u32();	// in-memory size
					r.u8();	// source bits
					if ((s.plugin & 0xF) == 2 || (s.plugin & 0xF) == 5)
					{
						uint32_t n = r.u32(), at = uint32_t(r.pos);
						if (s.plugin == kPluginSilence && n >= 12)	// wwiser wparser_plg.py CAkFxSrcSilenceParams
						{
							s.silenceS = r.f32();
							s.silenceMinusS = r.f32();
							s.silencePlusS = r.f32();
						}
						r.pos = at;
						r.skip(n);
					}
					readNodeBase(r, feedback, s.p);
					sounds.emplace(id, std::move(s));
					break;
				}
				case 0x05:	// CAkRanSeqCntr::SetInitialValues
				{
					Container c;
					c.id = id;
					readNodeBase(r, feedback, c.p);
					c.loopCount = r.u16();
					c.loopModMin = r.u16();
					c.loopModMax = r.u16();
					c.transitionMs = r.f32();
					c.transitionModMinMs = r.f32();
					c.transitionModMaxMs = r.f32();
					c.avoidRepeat = r.u16();
					c.transition = r.u8();
					c.randomMode = r.u8();
					c.mode = r.u8();
					uint8_t bits = r.u8();
					c.resetEachPlay = bits >> 1 & 1;
					c.restartBackward = bits >> 2 & 1;
					c.continuous = bits >> 3 & 1;
					c.global = bits >> 4 & 1;
					c.children = r.ids();
					for (uint16_t n = r.u16(); n && !r.bad; n--)
					{
						uint32_t item = r.u32();
						c.playlist.push_back({ item, r.s32() });
					}
					containers.emplace(id, std::move(c));
					break;
				}
				case 0x06:	// CAkSwitchCntr::SetInitialValues
				{
					SwitchCntr s;
					s.id = id;
					readNodeBase(r, feedback, s.p);
					s.groupType = r.u8();
					s.group = r.u32();
					s.defaultValue = r.u32();
					s.continuousValidation = r.u8();
					s.children = r.ids();
					for (uint32_t n = r.u32(); n && !r.bad; n--)
					{
						uint32_t value = r.u32();
						s.packages.push_back({ value, r.ids() });
					}
					for (uint32_t n = r.u32(); n && !r.bad; n--)
					{
						SwitchCntr::NodeParams np;
						np.node = r.u32();
						uint8_t bits = r.u8();
						np.firstOnly = bits & 1;
						np.continuePlayback = bits >> 1 & 1;
						np.onSwitch = r.u8() & 7;
						np.fadeOutMs = r.s32();
						np.fadeInMs = r.s32();
						s.nodeParams.push_back(np);
					}
					switchCntrs.emplace(id, std::move(s));
					break;
				}
				case 0x07:	// CAkActorMixer::SetInitialValues
				{
					Object m;
					m.id = id;
					readNodeBase(r, feedback, m.p);
					m.children = r.ids();
					mixers.emplace(id, std::move(m));
					break;
				}
				case 0x09:	// CAkLayerCntr::SetInitialValues
				{
					LayerCntr l;
					l.id = id;
					readNodeBase(r, feedback, l.p);
					l.children = r.ids();
					for (uint32_t n = r.u32(); n && !r.bad; n--)
					{
						Layer y;
						y.id = r.u32();
						y.rtpcs = readRtpcs(r);
						y.crossfadeRtpc = r.u32();
						y.crossfadeRtpcType = r.u8();
						for (uint32_t k = r.u32(); k && !r.bad; k--)
						{
							Layer::Assoc a;
							a.child = r.u32();
							a.curve = readPoints(r, r.u32());
							y.assocs.push_back(std::move(a));
						}
						l.layers.push_back(std::move(y));
					}
					layerCntrs.emplace(id, std::move(l));
					break;
				}
				case 0x0E:	// CAkAttenuation::SetInitialValues
				{
					Attenuation a;
					a.id = id;
					a.cone = r.u8();
					if (a.cone)
					{
						a.coneInside = r.f32();
						a.coneOutside = r.f32();
						a.coneOutsideDb = r.f32();
						a.coneLpf = r.f32();
						r.f32();	// high-pass
					}
					for (uint8_t& c : a.curveToUse) c = r.u8();
					for (uint8_t n = r.u8(); n && !r.bad; n--)
					{
						AttCurve c;
						c.scaling = r.u8();
						c.points = readPoints(r, r.u16());
						a.curves.push_back(std::move(c));
					}
					readRtpcs(r);	// none in the shipped banks
					attenuations.emplace(id, std::move(a));
					break;
				}
				case 0x08:	// CAkBus (Init.bnk): only the parent bus is read, the rest (props, ducking, effects) is not applied
					buses.emplace(id, r.u32());
					decoded = false;
					break;
				default:
					decoded = false;
					skipped++;
					break;
			}
			if (decoded && (r.bad || r.pos != r.end))
			{
				error_ = bank.path + ": HIRC object " + std::to_string(id) + " (type " + std::to_string(type) + ") parsed " + std::to_string(r.pos - (r.end - osz)) +
					" of " + std::to_string(osz) + " bytes";
				return false;
			}
		}
		return true;
	}
}
