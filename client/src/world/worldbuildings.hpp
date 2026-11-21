#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "world/buildings.hpp"
#include "assets/dds.hpp"
#include "world/floorfollow.hpp"
#include "render/drawstats.hpp"
#include "render/detail.hpp"
#include "assets/materials.hpp"
#include "render/lightbake.hpp"
#include "render/lightvol.hpp"
#include "assets/ogremesh.hpp"
#include "render/openwindows.hpp"
#include "render/renderqueue.hpp"
#include "assets/resources.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "sokol_gfx.h"

// near / far are empty macros once <windows.h> is in scope (it arrives through sokol_app.h, and libc++'s <locale> includes windows.h itself); the
// members named near / far below are plain identifiers (kenshi-379.31.3, caught by the local llvm-mingw cross-check).
#undef near
#undef far

// Streams and draws the placed buildings (M2b). Meshes and textures are cached by file and decoded on worker threads; the logic half (update, ensurePart,
// drain, evict) takes the decodes under a per-frame count (unlimited when `blocking`, used by scripted runs) and queues their GPU creates and destroys
// as render queue jobs (docs/RENDER_THREAD.md §7.2, kenshi-k16.2.205.12.35): the stream lane, whose jobs publish into an rq::Handle per mesh and texture,
// and a mesh or texture is Ready (named in a draw) only once its Handle is; a blocking update uses the frame lane instead, so what it waited for is
// drawn in the same frame, as before. One instanced draw per (part, submesh). update() fills the packet's FrameData (the frame's instances); upload()
// writes them into the transient instance buffer at the top of render_frame, before any pass; render() inside the pass.
class WorldBuildings
{
public:
	static constexpr double kCellSize = 4608.0;

	~WorldBuildings() { stopThreads(); }
	void init(const ResourceIndex* res, const MaterialLib* mats);
	void shutdown();
	// Tells the decode workers and the probe bake to stop and joins them; safe to call twice and without a GPU. cleanup() calls it on every quit path (its
	// early returns skip shutdown()), else the static App's destructor blocks in pthread_cond_destroy on the waiting workers (kenshi-379.12).
	void stopThreads();
	void setData(BuildingSet&& data);
	// Ground colour map (Terrain::colour): read by the TERRAIN feature mode. Must be valid views/samplers; call before the first render.
	void setGround(sg_view view, sg_sampler smp, sg_view alpha) { groundView_ = view; groundSmp_ = smp; groundAlphaView_ = alpha; }
	const TerrainDetail* detail = nullptr;	// biome layers for TERRAIN-mode map features (set before foliage init; null or !ok = those draw as triplanar)
	bool hasData() const { return !data_.instances.empty(); }

	// cam: world position; mvp: projection * rotation-only view; radius: load radius in zone cells. shadowFar > 0: also collect the instances within that
	// distance of the camera that are not visible (behind the camera, off screen) as shadow casters.
	// The packet's part (render hook 7a pattern; RENDER_THREAD.md §7.1 rule 2): written on every update().
	struct MeshGpu { sg_buffer vbuf = {}, ibuf = {}; };
	struct TexGpu { sg_image img = {}; sg_view view = {}; };	// view invalid: sokol refused the image (drawn grey; the next drain marks it Failed)

	// The resolved draw list (kenshi-k16.2.205.12.114, overlap review .85 T18 / section 1.4 A1): what the buildings' opaque and shadow draws need, by value,
	// built by update() on the logic side at its end (resolveDraws) and carried in the packet. render() and renderShadow() replay it and read no building
	// table (data_, partRt_, meshes_, textures_, groups_, active_, motion_). A GPU object is named by the rq::Handle that will hold it (the create is a
	// resource job that may run after the list is built), kept in the list's own tables; a draw names a table slot (-1: the fallback). The uniform blocks
	// are the shader's own layout (inst_vs_params_t, inst_fs_params_t, shadow_fs_params_t) without the matrix and light block the replay supplies.
	// The same resolver is what the glass / mask / id draws (T19) and the foliage (T20) are to reuse: resolveDraw() / resolveShadow().
	struct ResolvedDraw
	{
		int mesh = -1, ibuf = -1;	// DrawList::meshes slot; DrawList::ibufs slot (-1: the mesh's own index buffer)
		int firstVertex = 0, firstIndex = 0, indexCount = 0, instOffset = 0, count = 0;	// the submesh's ranges; the instances' byte offset in `inst` and their number
		sg_buffer inst = {};
		int tex[5] = { -1, -1, -1, -1, -1 };	// DrawList::textures slots of inst_tex, inst_nrm, inst_tex2, inst_nrm2, inst_mask (-1: the slot's fallback)
		int mode = 0;	// the shader mode (the stats' modeGroups index)
		bool cull = false;	// cullsBackFaces: the pipeline
		// The two fields a group sets and a part does not (kenshi-k16.2.205.12.119, as ResolvedShadow::base/fade): the replay writes them into vs.base and
		// fs.fade, so a part's resolved draw can be kept as a template (Foliage's draw list) and each group only fills these in with its instances.
		float base[3] = {};	// the group's origin, relative to the camera (instVsParams' base; 0 for the buildings' own world-space instances)
		float fade[2] = {};	// fadeStart, fadeEnd (inst_fs_params' fade); with fadep, fade[0] is the start FRACTION and fade[1] only a nonzero marker
		float fadep[3] = {};	// a foliage draw's per-instance fade end (inst_vs_params' fadep; 0: none, kenshi-k16.2.205.12.124)
		alignas(16) float vs[60];	// inst_vs_params_t, its mvp (the first 16 floats) set by the replay; base stays 0 in a template
		alignas(16) float fs[76];	// inst_fs_params_t; fade stays 0 in a template ([2] is the fade mode, which a part does set)
	};
	struct ResolvedShadow	// one (part, submesh) depth draw into a shadow cascade (drawShadow)
	{
		int mesh = -1, ibuf = -1;
		int firstVertex = 0, firstIndex = 0, indexCount = 0, instOffset = 0, count = 0;
		sg_buffer inst = {};
		int tex[2] = { -1, -1 };	// inst_tex, inst_nrm
		bool animated = false;	// a spinner or wind-aimed part: drawn each frame over the cached tile (renderShadow `which`)
		// The two fields a group sets and a part does not (kenshi-k16.2.205.12.119): the replay writes them into vs.base and fs.fade, so a part's resolved
		// draw can be kept as a template (Foliage's shadow lists) and each group only fills these in.
		float base[3] = {};	// the group's origin, relative to the camera (instVsParams' base)
		float fade[2] = {};	// fadeStart, fadeEnd (shadow_fs_params' fade); with fadep as ResolvedDraw's
		float fadep[3] = {};	// as ResolvedDraw::fadep
		alignas(16) float vs[60];	// inst_vs_params_t, mvp set by the replay (the cascade matrix); base stays 0 in a template
		alignas(16) float fs[12];	// shadow_fs_params_t: cut; the bias is the cascade's; fade stays 0 in a template
	};
	struct ResolvedMask	// one interior-mask (part, submesh) draw (renderMask: back faces then front faces)
	{
		int mesh = -1, ibuf = -1;
		int firstVertex = 0, firstIndex = 0, indexCount = 0, instOffset = 0, count = 0;
		sg_buffer inst = {};
		float offset[4] = {}, spin[4] = {};	// imask_vs_params_t without the mvp
	};
	struct ResolvedId	// one cursor-id draw: `count` instances of a floored building's part (renderIds; one draw per run, kenshi-k16.2.205.12.119)
	{
		int mesh = -1, ibuf = -1;
		int firstVertex = 0, firstIndex = 0, indexCount = 0, instOffset = 0, count = 1;
		sg_buffer inst = {};
		bool cull = false;
		float owner = 0;	// the id the fragment writes (the owner + 1)
		alignas(16) float vs[60];	// inst_vs_params_t, mvp set by the replay
	};
	struct DrawList
	{
		std::vector<ResolvedMask> mask;	// renderMask(): the interior hulls (update() collected them while maskWanted)
		std::vector<ResolvedDraw> glass;	// renderGlass(): the peeked parts, drawn twice (depth then blended)
		std::vector<ResolvedId> ids;	// renderIds()
		std::vector<ResolvedDraw> flip;	// renderFlip(): the cut-away walls fading after a camera turn (empty unless the eye moved), their uniforms carry the flip eye ring
		std::vector<ResolvedDraw> opaque;	// render(): the groups in order, each its opaque draw then its loading stand-in and fade draws
		std::vector<ResolvedShadow> shadow;	// renderShadow(): the groups that cast shadows
		std::vector<rq::Handle<MeshGpu>> meshes;
		std::vector<rq::Handle<TexGpu>> textures;
		std::vector<rq::Handle<sg_buffer>> ibufs;	// render.open_windows' patched index buffers
		std::vector<int> meshAt, texAt, ibufAt;	// the resolver's per-frame dedupe: table id -> slot (-1 none); empty: no dedupe
		void clear() { mask.clear(); glass.clear(); flip.clear(); ids.clear(); opaque.clear(); shadow.clear(); meshes.clear(); textures.clear(); ibufs.clear(); meshAt.clear(); texAt.clear(); ibufAt.clear(); }
	};

	struct FrameData
	{
		std::vector<float> inst;	// the instances of every group and mask group (kInstanceFloats each), as the groups' `first` index them
		// The nearest kMaxPoints building lights, sorted and ready for the light block (kenshi-k16.2.205.12.49: logic's lights_ is never read on the
		// draw side). Per frame: the count + kMaxPoints x 4 vec4 = 4 + 1024 bytes at most, copied once at the end of update().
		int lightCount = 0;
		float lights[LightParams::kMaxPoints * 4][4] = {};
		// render.lamp_occlusion (kenshi-jj3.9.1): per light, the building instance whose volume holds its lamp visibility and the lamp's slot there (lamp -1: none)
		uint32_t lightOwner[LightParams::kMaxPoints] = {};
		int lightLamp[LightParams::kMaxPoints] = {};
		// What the static groups' shadow draws read besides the camera (shadowSig), at update()'s end: the draw keys the shadow cache on this copy, never on
		// the live cuts / ghosts / peeks that logic's next frame moves (overlap review .85 section 1.3, kenshi-k16.2.205.12.95).
		uint64_t shadowSig = 0;
		// render.interior_light / shafts / lamps (T19): the nearest baked buildings as volumes, collected by the logic side after update() (lightVolumes), never by the draw
		std::vector<lightvol::Volume> volumes;
		DrawList draws;	// the opaque and shadow draws of update()'s groups, resolved (render() / renderShadow() replay it)
	};
	void update(const double cam[3], const float mvp[16], float fogDist, bool blocking, double time, double shadowFar, FrameData& out);
	void upload(const FrameData& d);	// render side, every draw of a packet (a write_transient buffer is written in each frame that draws it)
	void render(const DrawList& list, const float mvp[16], const LightParams& light);
	// The game's interior mask (materials/common/interiormask.material): the `interior mask` hulls (kind 2) of the buildings near the camera, back
	// faces into G and front faces into R of the mask target (Hdr::maskPass), each fragment its view depth + 1. The terrain and grass clips
	// (interiorclip.inc) and the particle clip (particles.glsl) read it. update() collects them only while `maskWanted` (render.interior_mask); drawn inside the mask pass,
	// per view (the mvp is that view's).
	bool maskWanted = false;
	void renderMask(const DrawList& list, const float mvp[16]);	// hasMask: !list.mask.empty()
	// CPU side of the same hulls (kenshi-kyat.2): is the world point inside an interior mask hull of a building near the camera (the buildings update() asked
	// interior parts of; independent of maskWanted)? A vertical ray in the building's frame crosses a closed hull an odd number of times above the point.
	// The kyat.2 floor-over-terrain rule's one query (kenshi-k16.2.254.1): inside a hull the terrain is the dune a sunk building sits in, clipped by the
	// mask and not drawn (interiorclip.inc), so the floor is the ground there — a walker inside draws at the sim's y and a click must take the floor.
	// With a ray (origin, dir) and exitT: where that ray leaves the hull's box, which the pick uses as its far bound so the buried terrain does not end the
	// click's search for the drawn floor. exitT is written only when the point is in a hull, and as a maximum over the hulls that hold it.
	bool insideInterior(const double p[3], const double* origin = nullptr, const double* dir = nullptr, double* exitT = nullptr, bool carveOnly = false) const;

	// Sun shadows. `shadow` is the atlas the main pass samples (set once, must outlive render()). renderShadow draws the buildings and features of the last
	// update() into one cascade (inside Shadow::beginPass/endPass); drawShadow is the per-group draw for Foliage's casters (after beginShadowDraws).
	const Shadow::FrameData* sfd = nullptr;	// this frame's shadow data (the packet's; the render side points it there at the start of the draw)
	// which: 0 every group, 1 the static ones (the shadow cache's tiles), 2 the animated ones (spinners and wind-aimed parts: drawn each frame over the restored tile).
	// Returns whether anything was submitted. The static groups' signature is the packet's FrameData::shadowSig (kenshi-k16.2.205.12.2, .95).
	bool renderShadow(const DrawList& list, const ShadowCascade& c, int which = 0);
	void beginShadowDraws() { drawstats::pipeline(shadowPip_); shadowVsOk_ = shadowFsOk_ = false; }
	void drawShadow(uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], const float lvp[16], const float bias[4], float fadeStart = 0, float fadeEnd = 0);
	// Light probes (kenshi-jj3.1.1, debug only: debug.light_probes, `dump light_bake`; nothing in the picture reads them). lightProbes() picks the building
	// with interiors nearest the camera, starts its type's bake on a worker thread (or loads the cache) once its meshes are loaded, and fills `out` with
	// the baked probes of that building within `radius` of the camera, in world coordinates. False: no building near, or its bake is not finished.
	struct ProbePoint { double p[3]; float level; bool usable; };
	bool lightProbes(const double cam[3], double radius, std::vector<ProbePoint>& out);
	// render.interior_light (lightvol.hpp, kenshi-jj3.2.1): the baked buildings nearest the camera as volumes for the GPU; starts bakes (one at a time, nearest first) as lightProbes does.
	// render.sun_bounce (kenshi-jj3.4.1): sunOn, the sun transfer for a white sun of strength 1 from `sunDir` (world, clamped to the horizon like the shadow's) is attached to
	// each volume (sunNew the latest bake, sunOld the one it replaces while it fades); a type whose bake is older than about 5 degrees of sun starts a new one, one at a time,
	// only for buildings that hold the camera or a player character. `groundAlbedo` (RGB) is the colour map where it bakes.
	bool lightVolumes(const double cam[3], double radius, size_t max, std::vector<lightvol::Volume>& out, bool sunOn = false, const float* sunDir = nullptr, const float* groundAlbedo = nullptr);
	static float lightGroundAlbedo();	// what the bake's ground bounces (the shader's open-field reference)
	std::string lightBakeSlice(int layer) const;	// the last building's probes of one horizontal layer as text: '#' unusable, '0'..'9' the sky level (9: 1 or more); rows are z, columns x
	std::string lightBakeLine() const;	// `light_bake: ...` for the last building asked about: building, grid size, probes, usable, seconds, baked or cache
	// render.open_windows (kenshi-jj3.10.1) survey for `dump open_windows [MAXLUMA]`: every building type with interiors placed in the world, its shell
	// and interior parts' detached quads, the texel range each samples and whether it reads as the dark barrier patch. Slow (decodes meshes and
	// atlases on this thread); a findings run, not a per-frame query.
	std::string openWindowsReport(float maxLuma = 0.35f);
	bool partReady(uint32_t pid, float* radius, float* centreLen) const;	// mesh drawable; its bounding radius and centre distance
	// Every texture the part's own draw reads is here (Ready) or will never come (Failed, or none by the material rule): the draw list names the part only
	// then (kenshi-k16.2.298.1). A diffuse map still streaming used to draw the 1x1 grey fallback, which under the loading screen's exposure reads as an
	// unlit white building until the real albedo lands.
	bool partTexturesReady(uint32_t pid) const;

	// Extra instanced draws for other users of the mesh/texture caches (Foliage). All main thread. addPart before the first draw of the part (after setData:
	// setData replaces the parts); the mesh and textures are loaded through ensurePart/drain like building parts. draw() needs the pipeline applied
	// (beginDraws) inside a pass, the part's mesh Ready and ensurePart done; `inst` holds 12-float instances (position + scale, quaternion xyzw, stretch)
	// with positions relative to `base` (itself relative to the camera). Returns the shader mode used.
	uint32_t addPart(const BuildingPartDef& def);
	void beginDraws(bool foliage = false) { drawstats::pipeline(pip_); foliagePass_ = foliage; culled_ = false; drawVsOk_ = drawFsOk_ = false; }
	// The four uniform blocks every draw of a building-shader pass shares (the light, the AO, the light volumes, this frame's shadow params): set them
	// once per pass, after the pipeline is applied (sg_apply_uniforms needs one) and before the pass's draws (kenshi-k16.2.205.12.119). A pass that
	// replays WorldBuildings::draw() itself (renderFlip) gets them from it.
	void applyPassUniforms(const LightParams& light);
	// Picks the back-face culling pipeline from the part's game material. The building materials inherit StaticObject, whose script has no cull_hardware, so the
	// exe's rule is the only one (kenshi-rwh.131): a part is single-sided except BuildingShader FOLIAGE (`material type` 2, the one it marks DOUBLESIDED); a feature
	// mesh, except MapFeatureMode FOLIAGE (4), whose foliage.material cull_hardware none keeps it two-sided. The mesh's own Ogre cull_hardware is ignored. Call
	// before draw().
	void applyCull(uint32_t pid) { const bool c = cullsBackFaces(data_.parts[pid]); if (c != culled_) { culled_ = c; drawstats::pipeline(c ? pipInterior_ : pip_); } }
	static bool cullsBackFaces(const BuildingPartDef& d) { return d.featureMode >= 0 ? d.featureMode != 4 : d.materialType != 2; }
	// fadeMode (mesh.glsl inst_fs fade.z): 0 the dithered fade, 1 the opaque pass leaving the fade band out, 2 the band alone (beginFadeBand's pipelines)
	// limbSkin / limbBlood: a severed limb group's wire tint (null: the part draws as before)
	int draw(uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], const float mvp[16], const LightParams& light, float fadeStart = 0, float fadeEnd = 0, int fadeMode = 0,
		const float* limbSkin = nullptr, const float* limbBlood = nullptr, int standin = 0);
	// The fade band of Foliage: its nearest surface into depth (depthOnly), then blended over what is drawn at that depth, as renderGlass
	void beginFadeBand(bool depthOnly) { drawstats::pipeline(depthOnly ? pipGlassDepth_ : pipGlass_); drawVsOk_ = drawFsOk_ = false; }

	struct Stats
	{
		size_t instancesDrawn = 0, submeshDraws = 0, realTextured = 0, groups = 0;	// last frame
		size_t idDraws = 0, idInstances = 0;	// the cursor id pass's draws and the instances they carry (they merge into runs of consecutive instances)
		size_t featuresDrawn = 0;			// of instancesDrawn: hand-placed features (rocks, formations)
		size_t interiorsDrawn = 0;			// furniture instances drawn (not in instancesDrawn)
		size_t modeGroups[8] = {}, dualNoColour = 0;	// groups drawn per shader mode (plain, ALPHA, EMISSIVE, DUAL, TRIPLANAR, TERRAIN, DUAL_TRIPLANAR, severed limb); DUAL groups left plain for lack of vertex colours
		size_t meshesReady = 0, meshesFailed = 0, texturesReady = 0, texturesFailed = 0, pending = 0;
		size_t uploadedMeshes = 0, uploadedTextures = 0;			// since start
		size_t standing = 0, fading = 0, mixed = 0;	// instances drawn as the loading stand-in / in its switch this frame; instances that drew black and real parts together outside the switch (always 0)
	};
	Stats stats() const;
#ifdef KCLIENT_DEV_TOOLS
	// Building pop-in measurement (kenshi-k16.2.258.2, the report behind the user's "buildings pop-in" on the BDC fly launch): per placed
	// building, when it entered the load radius, was first drawn and was fully textured; per mesh / texture, the read + parse on a worker (and
	// the drawn-triangle BVH beside it) and the GPU create in the render queue. Off unless KCLIENT_POPIN is set at start or `dump popin` asked
	// for it; a plain run costs one bool test an instance a frame. `dump popin` prints it: popinReport.
	struct PopIn
	{
		bool on = false;
		bool cold = false;			// KCLIENT_COLD_READ: each asset's pages dropped from the page cache before it is read (a cold disk)
		bool blocking = false;		// the last update() waited for every decode (a scripted / --screenshot run, debug.stream_scripted off)
		bool streaming = false;		// debug.stream_scripted: a scripted run that streams as a live one does
		double clock0 = -1;			// steady-clock seconds at the first update (the timeline's zero)
		double now = 0;				// this update's seconds since clock0
		uint64_t frame = 0;
		size_t inRange = 0, drawn = 0, full = 0;	// running counts over rows
		double liftAt = -1;			// s since clock0 when the load screen lifted (the ready-set gate; kenshi-k16.2.258.3), -1 never
		struct Row
		{
			uint32_t name = 0;			// BuildingSet::names index
			double x = 0, z = 0;		// the instance origin
			double dist = 0;			// camera distance when it entered the load radius
			double in = -1, draw = -1, complete = -1;	// s since clock0
			uint32_t parts = 0, shells = 0, interiors = 0;
			uint32_t unlitAtDraw = 0;	// shell parts still without their textures when it was first drawn
			double black = -1, swtch = -1;	// s since clock0: first drawn as the loading stand-in, the switch to the real look begun (kenshi-k16.2.258.4)
			uint32_t mixedFrames = 0;	// packs that drew black and real parts of it together (the loading stand-in's contract: 0)
		};
		std::unordered_map<uint32_t, Row> rows;		// instance index -> timeline
		struct Mesh
		{
			double decode = 0, pick = 0, create = 0;	// s: worker read+parse, worker pick BVH, GPU create (all jobs)
			size_t fileBytes = 0, cpuBytes = 0;			// the .mesh file, the decoded vertices + indices
			int tris = 0;
		};
		struct Tex
		{
			double decode = 0, create = 0;	// s: worker read (BC data kept as is), GPU create (all slices)
			size_t fileBytes = 0, gpuBytes = 0;
			int w = 0, h = 0, mips = 0;
		};
		std::unordered_map<std::string, Mesh> meshes;
		std::unordered_map<std::string, Tex> textures;
		std::mutex mu;				// the creates are timed on the render thread
		struct Curve { double t; uint64_t f; size_t inRange, drawn, full, meshesReady, texturesReady, pending; };
		std::vector<Curve> curve;	// one a frame while something changed
		uint64_t mixedInstanceFrames = 0, standingInstanceFrames = 0, fadingInstanceFrames = 0;	// the loading stand-in (kenshi-k16.2.258.4): packs summed over instances; mixed must stay 0
	};
	PopIn popin;
	std::string popinReport() const;	// `dump popin`: the timelines, the asset costs, the per-name precache table
	void popinMark(uint32_t ii, const BuildingInstance& b);	// in range: the instance entered a load cell this frame
	void popinLift() { if (popin.on) { popin.liftAt = popin.now; } }	// the load screen lifted: the pop-in after it is what a player sees
	void popinDrawn(uint32_t ii);							// first drawn / fully textured (the cull pass's any)
#endif
	// One line: lowest mesh point of each drawn instance minus the terrain under its origin, and the record's relative y, as p10/p50/p90.
	std::string fitReport() const;
	// Meshes whose submeshes use a material name other than StaticObject (one line each), for the texture-rule review.
	std::string materialReport() const;
	int radius = 3;
	bool streamPriority = true;	// render.stream_priority: the streams run in view and near first (false: submit order)
	bool streamAll = false;	// the world is not on screen (the session load screen): every finished decode goes to the stream lane at once (kenshi-k16.2.258.3)

	// Interiors: interior parts and layout furniture are resident and drawn by camera distance, never by "a character is inside" (the game's unload is a
	// presentation limit we do not keep). Parts within interiorRadius world units of the camera load and draw; the last 20 % of it fades out with the
	// dithered distance fade (shaders/fade.inc), so nothing pops.
	double interiorRadius = 2500.0;
	bool interiors = true;		// false: interior parts and furniture are not loaded or drawn (--no-interiors)
	// Roof / floor slicing (kinspect camera_rts.md 6, the original's): a building with floors (BUILDING_PART `building floor`) whose bounds (its interior
	// mask, else its ground-floor parts) hold one of the occupants is sliced: its parts and furniture on floors above the visible floor are not drawn.
	// The camera plays no part. One visible floor for all of them (the floor control, R / F): the lead's floor, written only while the camera follows a
	// character and only when that changes (stairs, a door), so a floor stepped to holds until then; 0 when the lead is outdoors, and taken afresh once
	// nobody is inside anything.
	// the wire's CHAR_DYNAMIC `building` (kworld::doorKey of the placement, 0 outside) and `floor`; instance >= 0: a character standing on that
	// floorable building's roof deck (roofBuilding, a client test: the wire's indoors bit is clear up there), which counts as the building's top floor
	struct Occupant { uint64_t building = 0; int floor = 0; int instance = -1; };
	std::vector<Occupant> occupants;	// set per frame: every player character (selected or not)
	bool hasLead = false;
	Occupant lead;	// the floor focus / name: the followed character, else the first selected (the original names the sliced building's lead)
	uint64_t leadId = 0;	// the lead character's wire id (main.cpp): the auto floor threshold log names it
	bool logFloor = false;	// dev (KCLIENT_DEV_TOOLS and --debug): print a line at each auto floor threshold
	// The camera subject's wire state and sim position, set per frame by main.cpp: updateFocus builds the floor model's sample from them (ff::Sample,
	// floorfollow.hpp; kenshi-k16.2.236.14). The position is the sim's (sim.simPos), never the drawn one: its display slop lags a climb.
	uint64_t subjectId = 0;	// the camera subject (main.cpp camera_subject), 0 none
	bool subjectKnown = false;	// its character record is decoded (before that its floor is not known)
	bool subjectIndoors = false;	// the wire's indoors bit (CHAR_DYNAMIC flag 19) at the render tick (SimView::stateAt, kenshi-k16.2.236.17)
	uint64_t subjectBuilding = 0;	// the wire's `building` key (0 outside)
	int subjectFloor = 0;		// the wire's floor
	int subjectRoof = -1;		// roofBuilding(sim position, wire key) with the indoors bit clear: the building whose roof deck it stands on, -1 none
	bool hasSubjectPos = false;	// the subject's sim position this frame
	double subjectPos[3] = {};
	bool following = false;	// the camera follows a character: the auto floor follows `lead` (camera_rts.md 6); not following, the level is the player's alone
	float xray[4] = {};	// the occluder cone, set per frame: the focus character relative to the camera, the cone's widest radius (0: off)
	// The cone's exemption (kenshi-k16.2.405.1): the building the camera is in or that is cut open (sliced) never fades: its foundation, furniture and floor
	// edge stay solid. A yaw-turned box in the world: xrayEx[0..2] centre, [3] 1 = on; [4..6] half extents, [7] yaw. Render only: the click ray keeps the cone (the cut-open building's near wall shape is not drawn there).
	double xrayEx[8] = {};
	// Occluders (ours; the original has none, camera_rts.md 7; user 2026-11-04): with autoGhost, a building between the camera and `seen` (the followed
	// character) fades whole to glass (renderGlass, the base too) at the fadeGhosts rate while it blocks; the click ray passes it (cutAway).
	bool autoGhost = false;
	bool hasSeen = false;
	// The seen character's inside test (findOccluders): the wire's indoors bit (CHAR_DYNAMIC flag 19: in a building's interior AND under a ceiling) and its
	// `building` key. Replaced the interior-bounds box, which counted a porch as inside (the bit is what the roof cut-away and the slicing follow too).
	bool seenIndoors = false;
	uint64_t seenBuilding = 0;
	// The occluder's real shape (user 2026-11-05: standing on The Hub's gate walkway faded it, its box holds the chest): a building whose box the
	// line crosses is an occluder only when this says the line hits one of its collision shapes (main.cpp: CollisionPick). Unset: the box alone.
	std::function<bool(const double from[3], const double to[3], uint32_t building)> lineHits;
	// Peek (camera.peek; ours, user 2026-11-04): buildings the cursor looks into from outside, kept by main.cpp (update_peek). Each is cut at `floor`
	// as the inside view cuts (the floors above it, the roof), but the cut parts are drawn see-through (renderGlass) at an opacity that falls with
	// `fade` (0 solid .. 1 fully peeked) instead of left out; they stop casting shadows past half. While `on` the click ray passes the cut parts.
	struct Peek { int floor = 0; float fade = 0; bool on = false; };
	std::unordered_map<uint32_t, Peek> peeks;
	bool peekPass = true;	// false: cutAway ignores the peeks (the camera's ground)
	int floorAt(int building, const double p[3]);	// p's floor in the building (its floor starts), 0 below the first
	int topFloor(int building) const;
	// a peeked part is visible (!list.glass.empty()): the caller splits the scene pass before renderGlass (Hdr::copyOpaqueDepth)
	void renderGlass(const DrawList& list, const float mvp[16], const LightParams& light);	// the peeked parts; after every opaque thing in the scene pass
	// The cut-away flip fade (kenshi-k16.2.354.1): the back faces of a cut building's single-sided parts, blended over the scene while a wall turned away within the
	// last kFlipFade seconds. Stateless per face: update() keeps a ring of past eye positions, the shader finds when the face last faced one. No depth write
	// (the cursor ID pass and everything after see the solid scene); a still camera draws nothing.
	void renderFlip(const DrawList& list, const float mvp[16], const LightParams& light);
	// Cursor ID pass (idbuffer.hpp): the ray the next update() lists the buildings along (those with floors whose box it crosses); renderIds()
	// draws what the scene drew of them, each with its id, in the ID pass.
	void cursorRay(const double dir[3], bool on);
	void renderIds(const DrawList& list, const float mvp[16]);
	static constexpr double kFloorTop = 2;	// MINE: local units above the interior's bottom that the occluder fade keeps solid: the ground floor's slab (the Storm House floor is 0.3 thick, walks at +0.3)
	static constexpr double kFoundSlab = 2;	// MINE: local units under the interior's bottom that still count as its floor (belowFloors; the foundation the occluder fade keeps)
	bool belowFloors(int building, const double p[3]);	// p is under its ground floor (the base, a foundation, outside steps): not the building's inside
	// the nearest building with an interior whose parts' box the ray enters; with at (a point on the ray: the pick's hit), the one whose box
	// holds it, the last entered (a big neighbour's box the ray only crosses on the way does not win: Defensive Gate IV before the Storm House)
	int rayBuilding(const double origin[3], const double dir[3], double far, const double* at = nullptr);
	// where (distances) the ray enters and leaves the building's parts' box, or its interior bounds
	bool rayBox(int building, const double origin[3], const double dir[3], double far, double& enter, double& leave, bool interior = false);
	bool sliced(int building) const { return std::find(cuts_.begin(), cuts_.end(), (uint32_t)building) != cuts_.end(); }
	std::string floorablesReport(double x, double z, double radius) const;	// dump floorables X Z R: the floorable buildings (maxFloor >= 1, not a ruin: a ruin has no interior parts) within R of the point, nearest first (kenshi-k16.2.355)
	std::string cutsReport() const;	// dump cuts: every building the floor cut slices (one per occupied building, kenshi-k16.2.332.1), its shown / top floor, the selector level
	// What cutHides (and so a prop's hidden state in main.cpp) reads: the sliced buildings and the visible floor. A cheap hash, so the per-frame props
	// list can be kept as is while it does not move (kenshi-k16.2.205.11).
	uint64_t cutSig() const
	{
		uint64_t h = 1469598103934665603ull;
		auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
		mix((uint64_t)visible_);
		mix(cuts_.size());
		for (const uint32_t b : cuts_) { mix(b); }
		return h;
	}
	bool ghosted(int building) const { return ghosts_.count((uint32_t)building) > 0; }
	void fadeGhosts(double dt, double in, double out);	// the occluders' glass fades, seconds to full and back
	std::string ghostState() const;	// the buildings now ghosted: name, fade (0 solid .. 1 glass) and whether still an occluder (the per-click record, kenshi-k16.2.259.3)
	std::string occluderReport(const double cam[3]) { std::string r; findOccluders(cam, &r); return r; }	// dump occluders (seen as set)
	bool holds(int building, const double p[3], bool interior = false);	// p is in the building's parts' box, or its interior bounds (the floor cut's)
	double seen[3] = {};
	struct FloorStatus
	{
		bool valid = false;		// something is sliced or peeked: the floor control has work
		std::string name;		// the lead's building, else the first sliced one
		int shown = 0, top = 0;		// the player's level (visible_, shown as is: only player input changes it) and the building's top floor (0..top)
		int autoFloor = -1;		// the computed auto floor (-1: not following, so there is none)
		bool peek = false;		// nothing sliced: the floor is the peek's (name: the peeked building)
		int leadFloor = -1;		// the lead character's own floor (0 outdoors; -1: no lead character at all), following or not (main.cpp logs it)
		bool following = false;		// the camera follows the lead: autoFloor is live and writes the level at a threshold
	};
	FloorStatus floorStatus() const { return floor_; }
	// The camera subject's floor from the model (ff::FloorFollow::autoFloor), -1 when it is not followed or `id` is not the subject: char_cut_hidden asks it
	// for the subject instead of the wire's floor (kenshi-k16.2.236.15), so a followed lead is hidden only by a manual step above its floor.
	int modelFloor(uint64_t id) const { return view_.subject() == id ? view_.autoFloor() : -1; }
	const ff::FloorFollow::LatchInfo& latchInfo() const { return view_.latchInfo(); }	// the stair latch's state for the F12 sidecar (k16.2.236.8.4.3)
	int focus() const { return focus_; }	// the building named in floorStatus, -1 = none
	// R = +1, F = -1. Always live (k16.2.24.1 / h2v.24: the original keeps the selector available at all times): with a sliced or peeked building it
	// steps from the level clamped to that building's top, else the global level 0..3 alone; either way the level is what an order's click ray cuts at
	// (levelCuts, clamped to each building's top).
	bool stepFloor(int dir);
	int level() const { return visible_; }	// the global level 0..3 (the original's pi+0x1e4)
	// An explicit engage (double click, portrait double click, F, V, Z, the session start's centring: main.cpp set_rts / set_follow_id): the player's
	// stepped level is dropped, so the next updateFocus shows the subject's floor even when it is the one already centred (kenshi-k16.2.236.5). The one
	// place the routes call.
	void engage() { view_.engage(); }
	// A save load's own view (camera sidecar): the saved level; the subject the load centres on (0: none) holds it until its next threshold.
	void restoreView(int level, uint64_t subject) { view_.load(level, subject); visible_ = view_.shown(); }
	void resetFloors() { view_.load(3, 0); visible_ = view_.shown(); }
	// A world point on the roof deck of a floorable building: xz inside its parts' box, feet at or over the top floor's slab and not far above the box.
	// Returns the instance, -1 for none. The roof is the building's top floor (maxFloor): the wire's floor says nothing there (client rule, brief h2v.24.1 a).
	int roofBuilding(const double p[3]);
	int roofBuilding(const double p[3], uint64_t wireBuilding);	// plus: the wire still names a floorable building with its indoors bit clear (the stairs' top step, before the feet reach the roof test's box): that building's roof deck (kenshi-k16.2.266)
	std::string recordOf(uint64_t key) const;	// the BUILDING record name of the placement with the wire's `building` key ("" none)
	std::string nameOfInstance(int index) const;	// the instance's building name ("" bad index): the debug shot's clicked-building name (kenshi-k16.2.231.1)
	int bedPiece(const double at[3]) const;	// the layout furniture piece holding a bed's sleeping spot (bedRay's search), -1 none: the F12 shot's pick report (k16.2.234.1)
	std::string describeInstance(int index) const;	// one line: name, placement, kind, part mesh files and their box: the F12 shot's pick report
	// Set by main.cpp around the click pick (ray_hit): buildings nobody of ours is in are cut at the level, as the original's pick mask follows pi+0x1e4,
	// so an order at level 0 lands inside on the ground floor and at the roof's level on the roof deck. A level above a building's top clamps to it.
	bool levelCuts = false;
	// The click ray's filter (main.cpp ray_hit): p (world) is in a ghosted occluder, or in a sliced or peeked building at or above the first hidden
	// floor's slab, so a click there passes through to what is drawn below. bottom (world y): the hit shape's low face; the floor
	// test then takes the shape whole by where it starts, as the renderer hides a part by its floor (a staircase rising past the slab stays drawn).
	// why (the F12 pick report, kenshi-k16.2.259): why it is not drawn there ("an occluder faded to glass", "above the floor cut of Longhouse"); empty drawn.
	bool inSolidZone(const double p[3]);	// kenshi-k16.2.412.3
	bool cutAway(const double p[3], double bottom = 1e30, std::string* why = nullptr);
	// p is on a floor the cut hides (a sliced building, at or above the slab over the shown floor): its characters and items go. bottom: the solid
	// shape p stands on, by its low face (the click pick's staircase rule), so a character climbing a stair keeps the stair's starting floor.
	bool cutHides(const double p[3], double bottom = 1e30);
	// The floor of a hover peek (an unoccupied building the cursor looks into) that p lies above, -1 when no peek that is on cuts p: a character
	// there is hidden by its wire floor against this floor, as against the shown level in an occupied cut (kenshi-k16.2.311).
	int peekCutFloor(const double p[3], double bottom = 1e30);
	static constexpr double kFocusRange = 1500.0;
	friend class Foliage;
	// Axis frame of BUILDING_PART offset X/Y/Z and rotation axis: 0 = same as the mesh (x, y, z), 1 = Z-up authoring, mapped (x, z, -y).
	int offsetFrame = 0;
	bool hideMarkers = false;	// parts flagged `is for position marker`: the model is drawn without offset by default (camp beds, dummies)
	double drawLimit = 0;		// 0 = none; else caps every part's draw distance (exteriors: render.building_far and the fog)
	// Construction (docs/CLIENT_GOALS.md "Construction"): progress 0..1 of a building instance (work done, from the sim); >= 1 or never set = complete. Solid below a cut at
	// progress x `building height`, a dithered ghost above it, a glowing band at the cut; the ghost casts no shadow. Furniture of an unfinished building is hidden.
	void setConstruction(uint32_t instance, float progress);
	void clearConstruction() { construction_.clear(); }
	// Building lights (LIGHT instances on drawn parts; kenshi-inspect light_record.md): indoor ones (interior parts and furniture) always, as they show
	// with the interior; outdoor ones only while night. lightMinutes = game time in minutes, the PULSE / SHIMMER phase. fillLights copies the nearest
	// kMaxPoints of the packet's FrameData (update() packs them) into the shared light block.
	bool night = false;
	float wind[3] = {};	// set per frame: the live wind direction (toward) x, z, and 1 when known (rotation function 4 parts aim; 0: rest pose)
	float windSpeed = -1;	// set per frame: the live wind speed (u/s) for rotation function 3's spin ramp; < 0 unknown (spins at full speed)
	void setSwayMates(const std::vector<uint32_t>& ids);	// the parts of one tree record
	float swayTop(uint32_t pid) const;	// render.leaf_sway: the tree's height (the tallest mesh of its record), 0 when the part does not bend
	void resetTurns();	// wind-aimed parts face the wind at once (a scene change: no turn from the last scene's wind)
	double lightMinutes = 0;
	static void fillLights(const FrameData& d, LightParams& light, const lightvol::Volumes* vols = nullptr);	// vols: the live volumes (lamp codes)	// render side: reads the packet's copy only
	void packLights(FrameData& out) const;	// logic side: the nearest kMaxPoints of lights_, sorted, into the packet
	bool lampOcclusion = false;	// render.lamp_occlusion (kenshi-jj3.9.1), set by the logic half each frame: the lamps of baked buildings register for a baked visibility volume (lightVolumes); false: nothing registers, no light carries a code
	size_t lightsInView() const { return lights_.size(); }

	// Placed EFFECT instances that are in view this frame (slice 1: collected and dumped, not drawn). The transform, range and frustum
	// gates are the light pass's; a part effect is collected only where its part passes the floor / interior-distance filters. `world` is
	// absolute, `rel` camera-relative; `seed` is placement-derived so a later particle simulation is order-independent.
	struct PlacedEffectRt
	{
		uint32_t def = 0;		// BuildingSet::effects
		uint32_t instance = 0;		// BuildingSet::instances (building, furniture or feature)
		int32_t part = -1;		// BuildingSet::parts when the effect lives on a part, else -1
		int floor = 0;
		double world[3] = {};
		float rel[3] = {};
		float quat[4] = { 1, 0, 0, 0 };	// the effect node's world orientation: the instance's rotation x the EFFECT instance's own (w, x, y, z)
		float distance = 0;
		float scale = 1;		// the owning instance's node scale (BuildingInstance::scale)
		uint32_t seed = 0;
	};
	const std::vector<PlacedEffectRt>& placedEffects() const { return placed_; }
	const std::string& effectScript(uint32_t def) const { return data_.effects[def].script; }
	std::vector<std::string> effectScripts() const;	// distinct `particle system` names in the loaded set, sorted (dump)
	size_t effectDefs() const { return data_.effects.size(); }
	size_t ownLightDefs() const { return data_.ownLights.size(); }
	std::string describe(uint32_t instance) const;	// "NAME at x,y,z (height H)"
	std::string partsReport(uint32_t instance) const;	// describe() + one line per part: kind, floor, mesh, texture, material type, offset, load state
	int nearestBuilding(double x, double z) const;	// building instance (not a feature or furniture) with its origin nearest (x, z) within kFocusRange, -1 if none
	// debug (dump feetfloor): the top of the DRAWN floor under (x, z): for the building the character's wire says it is in (key, CHAR_DYNAMIC
	// `building`; 0 or unknown: the nearest building by origin), every drawn part's render triangles then their world y (the instance transform the
	// shader uses), the highest at or below fromY within range; one line per part and the top overall. Kinds: 'cut' a part the floor cut hides (not
	// drawn), 'spin' one that turns (its drawn pose needs the clock). Text only, no graphics (kenshi-k16.17.7.24.4).
	// kenshi-k16.2.414.2: the walkable-surface bit per part of the placed buildings whose name contains `name`: parts walkable / not by kind (0 shell, 1 interior, 2 mask, 3 furniture), one line per distinct building
	std::string walkableReport(const std::string& name) const;
	std::string drawnFloorReport(uint64_t key, double x, double z, double fromY, double range) const;
	// Read side of dump pickboxes (src/dev/pickboxes.cpp, kenshi-k16.2.260.1): an instance's part geometry and its mesh-to-world transform, the one
	// the instance vertex shader draws with and instanceRay inverts. partGeometry: part k's mesh, uploaded bounds, toMesh(part offset) and (withVerts)
	// the decoded triangle vertices as xyz triples; false: not a drawn part. partToWorld: p = origin + qrot(q, (v + off) * stretch * scale).
	struct PartGeometry
	{
		std::string mesh;
		double bmin[3] = {}, bmax[3] = {}, off[3] = {};
		std::vector<float> xyz;
	};
	bool partGeometry(uint32_t index, uint32_t k, bool withVerts, PartGeometry& out) const;
	void partToWorld(uint32_t index, const double v[3], const double off[3], double out[3]) const;
	const BuildingInstance* instanceAt(uint32_t index) const { return index < data_.instances.size() ? &data_.instances[index] : nullptr; }
	int instanceFloor(uint32_t index);	// a layout piece's floor (-1: not a layout piece)
	int instanceShown(uint32_t index) const;	// the shown floor of a layout piece's building (3: not a layout piece)
	// Sim props (SIMLINK containers): each is drawn as an instance of its BUILDING's storage parts (BuildingSet::storage), unless a placed building of
	// the world already stands at its position (the world's containers are placed instances; the scenario crates are not). Replaces the last call's
	// props; call before update(). hasMesh: the prop is drawn, by a prop instance or a placed one (false: unknown record, keep a marker).
	// propRay: the nearest t >= 0 at which the ray (origin + t dir) enters the box of one of the prop's loaded part meshes; false = missed / not loaded.
	// placedOnly: only a world placement there (pick boxes), never drawn anew; item: a ground item (BuildingSet::items), never a placement
	// limb: a ground severed limb whose record is `item function` 0x11; limbSkin / limbBlood 0..1 and limbScale from the wire's limb tail (SIMLINK), all
	// zero / one when the server sent none. Without a tail the part draws as before (the limb material needs the tint).
	struct Prop { uint64_t id = 0; std::string sid; double pos[3] = {}; bool placedOnly = false, item = false; float yaw = 0; bool cut = false;
		bool limb = false; float limbSkin[3] = { 0, 0, 0 }, limbBlood[3] = { 0, 0, 0 }, limbScale[3] = { 1, 1, 1 };
		bool building = false; float rot[4] = { 1, 0, 0, 0 }, progress = 1;
		// a container's wire tail (SIMLINK kind 3): the layout piece that draws it, by its building's doorKey and the piece's data index (hasPiece false:
		// none, the client matches a piece by position as before)
		uint64_t pieceBuilding = 0;
		uint32_t pieceIndex = 0xFFFF;
		bool hasPiece = false; };	// building: a player's building (SIMLINK BUILDING 47), sid its record: the record's shell parts (BuildingSet::shells) at pos turned by rot (w, x, y, z), under construction while progress < 1 (setConstruction)	// yaw: about +y (a ground item's rest yaw); cut: on a floor the cut hides (main.cpp cut_hidden: the stair rule)
	void setProps(const std::vector<Prop>& props);
	// Door leaves (BuildingInstance::door, a BUILDING's `doors`): each leaf draws moved by its door's offset (world axes, the pose function of doorpose.hpp
	// the pick uses too); a door not listed stays in its frame. Replaces the last call's; call before update().
	// a moved door repacks: the leaf offsets are applied in the cull / pack pass the dirty tracking skips (kenshi-k16.2.205.11 x k16.2.241.1)
	void setDoors(const std::unordered_map<uint64_t, std::array<double, 3>>& offsets) { if (offsets != doorOffsets_) { doorOffsets_ = offsets; packValid_ = false; doorGen_++; } }
	// The character editor's platform mesh at pos (its origin; BuildingSet::editorPlatformRef), turned by q (w, x, y, z; null = upright), drawn with
	// the props by the next setProps. A moved or turned platform (the editor's orbit, k16.2.433.1) moves its instance in place instead: the props are
	// rebuilt only when the sim's things change, so the instance setProps added is where the platform follows the character.
	void setEditor(bool on, const double pos[3] = nullptr, const float q[4] = nullptr);
	bool isShopCounter(const std::string& sid) const { return data_.shopCounters.count(sid) != 0; }
	bool hasMesh(uint64_t id) const { return propMeshes_.count(id) != 0; }
	int propInstance(uint64_t id) const { const auto it = propMeshes_.find(id); return it == propMeshes_.end() ? -1 : (int)it->second; }	// the instance a prop (or a player's building) is drawn by, -1 none
	// A prop drawn by an interior layout piece, by the wire's tail (the container's building doorKey and piece index): its own mesh is never drawn,
	// so its pick shape is the piece's boxes alone (no pick sphere: main.cpp pick_at)
	bool propFromLayout(uint64_t id) const { return propLayout_.count(id) != 0; }
	// The interior furniture instance a container's wire tail names, -1 when this build has no such piece (an older server, a layout not joined)
	int layoutPieceInstance(uint64_t building, uint32_t piece) const;
	// The instance a prop (a container) is drawn and picked by and each part's pick box in world space (dev `dump prop ID`)
	std::string propReport(uint64_t id) const;
	// drawn: refine each part's box entry against the mesh's own triangles (a round barrel's box corners are not the barrel; k16.2.239.4.1): t is then the
	// nearest drawn surface, and a ray through only the box's empty corners misses.
	bool propRay(uint64_t id, const double origin[3], const double dir[3], double& t, bool drawn = false) const;
	// drawnHitIn: the nearest drawn triangle hit with t in [tMin, tMax] on the building shells and layout pieces near the world box lo..hi (the box of a collision
	// solid): false = nothing is drawn along the ray inside that span, so the solid there is an undrawn proxy volume.
	bool drawnHitIn(const double origin[3], const double dir[3], const double lo[3], const double hi[3], double tMin, double tMax, double& t, bool walkableOnly = false) const;
	// What walkableRay hit (the dev `dump pick` report, kenshi-k16.2.412.3 rework: the wall-right part's name)
	struct WalkHit { uint32_t instance = 0; uint32_t part = 0; bool walkable = false; int kind = 0; std::string mesh; };
	// kenshi-k16.2.412.3: the nearest drawn triangle of the walkable parts along a ray (the click's ground layer): any direction, no box; t in [tMin, tMax].
	// Only a triangle at or under the bake's walk slope counts (knav::NavConfig::agentMaxSlope, 40 deg): the walkable bit is per part, so a house's walls carry it
	// too, and a wall face is not ground a body stands on (the wall-right click).
	// who: the part hit (its instance, part index, kind, walkable bit and mesh path), for the pick report.
	bool walkableRay(const double origin[3], const double dir[3], double tMin, double tMax, double& t, WalkHit* who = nullptr) const;
	// kenshi-k16.2.344: the drawn building surface nearest `wireY` under (x, z) within up above / down below: the standing ray's fallback where a part has no
	// collision. The nearest of the first drawn triangle hit going down and going up. 1: a drawn surface at y in the window; 0: none in the window but one within 12 u of the wire (a building covers x, z); 3: nothing
	// drawn within 12 u (open ground); -1: none in the window but a building near x, z has a mesh still loading (kenshi-k16.2.382.2: the stand ray's only building source, collision meshes left it).
	int groundDrawn(double x, double z, double wireY, double up, double down, double& y) const;
	const void* groundDrawnGen() const;	// changes with the instance set and the Ready meshes (what groundDrawn reads): the per-character ray cache's key
	// The world box of everything a prop draws (its parts' mesh boxes, this frame), grown by `grow`; false: no such prop or nothing drawn yet.
	// pick_at: a click whose world hit point is on the prop's own drawn box is that prop (its collision proxy may sit just outside it)
	bool propBox(uint64_t id, double lo[3], double hi[3], double grow = 0) const;
	int bedRay(const double at[3], const double origin[3], const double dir[3], double& t, bool drawn = false) const;	// drawn: the entry into the piece's drawn triangles, not its box (a character lying beside the bed, k16.2.224.3). The layout furniture piece holding a bed's sleeping spot `at`: the ray's entry into its oriented mesh box (k16.2.220); -1: no piece holds it, 0: miss, 1: hit

private:
	uint64_t shadowSig() const;	// update() writes it into FrameData::shadowSig on every return
	void refreshTopBoxes() const;
	bool instanceRay(uint32_t index, const double origin[3], const double dir[3], double& t, bool drawn = false, double tMin = 0, double tMax = 1e30, bool walkableOnly = false, WalkHit* who = nullptr, double minUp = 0) const;
	bool triangleRay(int mesh, const double o[3], const double d[3], const double off[3], double& t, double tMin = 0, double tMax = 1e30, double minUp = 0) const;
	// A mesh's drawn triangles with a BVH over them (kenshi-k16.2.382.2): built on the worker beside the decode (buildPick), moved into pickTris_ by uploadMesh.
	// tris: xyz x 3 per triangle, mesh units, in leaf order; node: its box, and for an inner node (count 0) its children, for a leaf the first triangle and the count.
	struct PickNode { float lo[3], hi[3]; int32_t a, b, count; };
	struct PickMesh { std::vector<float> tris; std::vector<PickNode> nodes; size_t bytes() const { return tris.size() * sizeof(float) + nodes.size() * sizeof(PickNode); } };
	static void buildPick(PickMesh& out, const std::vector<float>& tris);
	// The top-level instances' world boxes (what partWorldBox gives for their Ready parts), one per topLevel_ slot: drawnHitIn tests only the instances whose box
	// the vertical window reaches (kenshi-k16.2.382.2: the per-query instance loop was the stand ray's cost). A new instance set rebuilds them all; after that a
	// box is recomputed only when the stamp of its parts (each part's mesh and that mesh's load state) moved, so the set's streaming churn costs a walk of the
	// parts, not a box per instance (the first cast's 2-4 ms; kenshi-k16.2.382.2).
	struct TopBox { uint32_t index; float lo[3], hi[3]; uint64_t stamp = ~0ull; };
	mutable std::vector<TopBox> topBoxes_;			// parallel to topLevel_; an instance with no Ready part has lo > hi and culls out
	mutable uint64_t topBoxData_ = ~0ull, topBoxHold_ = ~0ull;	// the dataGen_ / holdGen_ the boxes were last checked against
	void topBoxRebuild(uint32_t pos) const;		// one instance's box from its Ready parts' boxes
	uint64_t topBoxStamp(const BuildingInstance& b) const;	// its parts' meshes and their load states: what its box depends on besides the instance transform
	size_t pickBytes_ = 0;	// the total of pickTris_ (logged as evlog pickmem)
	mutable std::unordered_map<std::string, PickMesh> pickTris_;	// mesh path -> triangle corners (xyz x 3 per triangle), filled by uploadMesh when the mesh loads (drawn picks and the standing ray)
	// bedRay's piece search (bedPiece) by the bed's sleeping spot, kept while dataGen_ and holdGen_ stand (kenshi-k16.2.205.12.111): the search reads
	// only furnGrid_ (setData), the instances (dataGen_) and which part meshes are Ready with their boxes (holdGen_). Not stateGen_: texture uploads
	// bump it too, most frames while Heft streams. Logic thread only, like pickTris_: written and read by bedRay inside pick_at (hover, click, the
	// F12 report); the draw (render thread) never touches it.
	struct BedMemo { double at[3]; uint64_t dataGen, holdGen; int piece; };
	mutable std::unordered_map<uint64_t, BedMemo> bedMemo_;
	bool partWorldBox(const BuildingInstance& b, uint32_t k, double lo[3], double hi[3]) const;
	bool instanceHolds(uint32_t index, const double p[3], double slack) const;
	std::map<std::pair<uint64_t, uint32_t>, uint32_t> layoutPieces_;	// interior furniture by (building doorKey, piece data index): the wire's container tail
	std::unordered_set<uint64_t> propLayout_;				// the last setProps' props drawn by such a piece (WorldBuildings::propFromLayout)
	// Loading: the decode is queued or running. Uploading: decoded (the CPU fields below are set), its create is a stream-lane job not run yet.
	// Ready: drawable (its Handle is ready, or its create is a frame-lane job of the packet being filled).
	enum State { Unloaded, Loading, Uploading, Ready, Failed };
	struct Sub
	{
		int firstVertex = 0, firstIndex = 0, indexCount = 0;
		std::string material;
		std::vector<openwindows::Island> islands;	// render.open_windows: the submesh's small detached quads (openwindows::find, at decode)
	};
	struct MeshEntry
	{
		std::string path;
		State state = Unloaded;
		rq::Handle<MeshGpu> gpu;	// a fresh one per load; the create's job publishes it, an evict's destroy job holds the old one
		rq::Lane lane = rq::Lane::kStream;	// the lane of its create: its destroy goes there too
		rq::PrioPtr prio = std::make_shared<rq::Prio>();	// the stream lane's order of its create (kenshi-k16.2.258.3); logic rewrites it each frame
		float score = 1e30f;	// the lowest score of a part needing it this frame (scoreFrame): 0 in view and near, 1e5 in view and far, 2e5 + out of view; + distance
		uint64_t scoreFrame = 0;
		std::vector<Sub> subs;
		std::vector<uint32_t> indices;	// the CPU index list, kept for a part's render.open_windows patch (eviction keeps it)
		float bmin[3] = {}, bmax[3] = {}, center[3] = {}, radius = 0;
		bool hasColour = false;
		bool keepHull = false;	// an interior mask part (kind 2) uses it: the CPU triangles below stay (insideInterior)
		std::vector<float> hullPos;	// xyz per vertex, mesh units
		std::vector<uint32_t> hullIdx;	// triangle list, indices into hullPos
		uint64_t lastUsed = 0;
	};
	struct TexEntry
	{
		std::string file;
		State state = Unloaded;
		rq::Handle<TexGpu> gpu;
		rq::Lane lane = rq::Lane::kStream;
		rq::PrioPtr prio = std::make_shared<rq::Prio>();	// as MeshEntry::prio
		float score = 1e30f;
		uint64_t scoreFrame = 0;
		std::vector<uint8_t> thumb;	// render.open_windows: the albedo's luma at <= 128 px (openwindows::Texel), from the decode
		int tw = 0, th = 0;
		uint64_t lastUsed = 0;
	};
	// A turning part's motion, advanced once per update(): rotation function 3's spin angle (its rate follows the wind speed) and the wind aim's
	// turn (function 4 and its nested parts): the wind it turns from and to, and when the turn began.
	struct Motion
	{
		double spin = 0;	// rad
		float from[2] = { 0, 1 }, to[2] = { 0, 1 };
		double since = 0;
		bool aimed = false;	// faces a wind (false: the next known wind is taken at once)
	};
	std::vector<Motion> motion_;	// by part id
	std::vector<uint32_t> turning_;	// the part ids with a motion (function 3 spinners, wind-aimed parts)
	double motionTime_ = 0;
	void advanceMotion();
	const float* turnOf(uint32_t pid, float out[6]) const;
	struct PartRt
	{
		int mesh = -2;			// -2 unresolved, -1 no file, else index into meshes_
		bool resolved = false;
		std::vector<int> subTex;	// per submesh: index into textures_, -1 = grey
		std::vector<char> subFlat;	// per submesh: 1 = not the part's texture, so a flat normal map
		int normalTex = -1, tex2Tex = -1, nrm2Tex = -1;	// the normal map (its alpha: ALPHA / EMISSIVE / FOLIAGE extras), DUAL's second texture and its normal map
		int maskTex = -1;		// a severed limb's `color map` (the race body mask, r = skin tone mask)
		int ownTex = -1;		// a severed limb's `texture map` itself (SeveredLimbItem binds the item's maps, not the submesh's script material)
		// render.open_windows: the mesh's index buffer with the part's barrier quads collapsed to degenerate triangles (the sub ranges stay valid),
		// built once when the mesh and its textures are loaded; openHas false leaves the part drawn as before. openHide: a black cavity lining
		// (a *black*-named shell part or interior part, kind 0 / 1): left out of a building whose windows the switch opens.
		// openIbuf is made by a frame-lane render queue job (logic builds the list; render.thread=lockstep makes no sg_* call on logic) and freed
		// by one (setData); a frame-lane job runs before the packet that names it is drawn.
		rq::Handle<sg_buffer> openIbuf;
		bool openDone = false, openHas = false, openHide = false;
		// the occluder fade keeps the foundation and the ground floor solid (kenshi-k16.2.411.1): building-local height (mesh units, the part's offset in) under which this
		// part's fragments never fade, the interior's bottom + kFloorTop; set by infoFor from the building that holds the part, -1e30 = none
		float foundY = -1e30f;
	};
	struct MeshCpu
	{
		std::vector<float> verts;	// 13 words per vertex: pos, normal, uv, RGBA8 colour (0xffffffff when absent), tangent xyzw (w: handedness)
		bool hasColour = false;
		std::vector<uint32_t> indices;
		std::vector<Sub> subs;
		float bmin[3] = {}, bmax[3] = {};
		PickMesh pick;	// worker-built (buildPick)
	};
	// Building-local geometry (mesh units, before the instance scale) of a building with interiors, from its loaded meshes: bounds of the interior mask (else the interior and ground-floor parts) and
	// the start height of each floor (its interior slab, else an even split), for the inside test and for placing furniture on floors.
	struct BInfo
	{
		bool hasBounds = false;
		float mn[3] = {}, mx[3] = {};
		float outerMn[3] = { 1e30f, 1e30f, 1e30f }, outerMx[3] = { -1e30f, -1e30f, -1e30f };	// every loaded part's bounds (walls and slabs reach past the mask)
		float floorY[4] = { -1e30f, 1e30f, 1e30f, 1e30f };
		float foundY = -1e30f;	// the solid zone's top (the parts' foundY): drawn opaque while the building is ghosted (kenshi-k16.2.412.3: the click ray hits it)
	};
	// A staircase's flight (kenshi-k16.2.236.3, the floor model's ff::Flight since kenshi-k16.2.236.14), built from the staircase's own mesh (`is stairs`,
	// keepHull) in the building's local mesh units (before the instance scale, like BInfo). Measured on the Storm House's InteriorStairs mesh: tread
	// 4.1 u, rise 3.9 u, the top tread one stair deep across the full width. The old hotspot zone (top 3 treads, armed / held) is in
	// client/retired/floor_threshold_rules.cpp.
	struct StairHot
	{
		int lower = 0, upper = 1;		// the floors the staircase joins
		float fmn[2] = {}, fmx[2] = {};	// the whole flight's footprint, building-local x/z (mesh units)
		float y = 0;					// the top tread's height, building-local
		float rise = 0;					// one tread's rise (mesh units): the floor model's stair latch switches at top - 1 and top - 2 rises (kenshi-k16.2.236.11)
		float yLow = 0;					// the foot tread's height
		float tread = 0;				// one tread's run (mesh units)
		float footC[2] = {}, topC[2] = {};	// the foot and top treads' centres, local x/z: the flight's slope for the floor model's "on the flight" test
	};
	struct Job
	{
		bool isMesh;
		int id;
		std::string path;
		float prio = 0;	// lower = sooner: the worker takes the best of the queue (rerank() rewrites it as the camera moves; kenshi-k16.2.258.3)
	};
	struct Result
	{
		bool isMesh;
		int id;
		bool ok;
		MeshCpu mesh;
		DdsImage tex;
#ifdef KCLIENT_DEV_TOOLS
		double decodeS = 0, pickS = 0;	// the worker's read + parse, and its drawn-triangle BVH (popin)
		size_t fileBytes = 0;
#endif
	};
	struct Group
	{
		uint32_t part;
		int sub;
		std::vector<float> onlyShadow, inRange, outRange;	// 12 floats per instance (kInstanceFloats): shadow casters that are not visible; visible and within the shadow range; visible only
		std::vector<float> glass;	// visible peeked parts (renderGlass), after outRange
		std::vector<float> flip;	// visible single-sided parts of a cut building while the camera moves (renderFlip), after glass
		std::vector<float> standin;	// the loading stand-in (kenshi-k16.2.258.4): visible parts of an instance whose textures are not all ready, drawn flat black; after flip
		std::vector<float> fade;	// the real look dithering in over the stand-in's switch (the same instances' parts), after standin
		int first = 0;			// instance index in the frame buffer: onlyShadow, inRange, outRange, glass in that order
		// A ground severed limb's tint (one group per instance: it comes from the wire, per item); limb false draws the part as before
		float skin[3] = { 0, 0, 0 }, blood[3] = { 0, 0, 0 };
		bool limb = false;
	};

	int modeFor(uint32_t pid) const;	// shader mode of a part (draw())
	// The resolver (see ResolvedDraw): update()'s end builds `out.draws`; draw() / drawShadow() resolve one draw into scratch_ and replay it at once, so a
	// caller that has not moved to a list (Foliage, the glass and flip passes) draws exactly as the list replays.
	void resolveDraws(FrameData& out);
	void resolveDraw(DrawList& L, ResolvedDraw& r, uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], float fadeStart, float fadeEnd, int fadeMode,
		const float* limbSkin, const float* limbBlood, int standin, bool flip = false) const;
	void resolveShadow(DrawList& L, ResolvedShadow& r, uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], float fadeStart, float fadeEnd) const;
	int meshRef(DrawList& L, int mesh) const;
	int texRef(DrawList& L, int t) const;	// -1 unless the texture is Ready (texView's first test)
	int ibufRef(DrawList& L, uint32_t pid) const;	// -1: the mesh's own index buffer
	sg_view viewOf(const DrawList& L, int slot, sg_view fallback) const;	// render side: texView on a list slot
	int replayDraw(const DrawList& L, const ResolvedDraw& r, sg_buffer inst, const float mvp[16]);
	void replayShadow(const DrawList& L, const ResolvedShadow& r, sg_buffer inst, const float lvp[16], const float bias[4]);
	// The per-draw uniform blocks are uploaded only when they change: a group's submeshes (and the id pass's instances of one part) carry identical
	// ones, and the upload is a glUniform call per block member on the GL backend. The last uploaded block, per shader program: the building shader's
	// (drawVs/drawFs, reset by beginDraws / beginFadeBand / applyPassUniforms), the shadow shader's (shadowVs/shadowFs, by beginShadowDraws) and the
	// id shader's (idVs by renderIds). The GL uniform state lives in the program, so what another program applied in between does not invalidate it.
	alignas(16) float drawVs_[60] = {}, drawFs_[76] = {};	// ResolvedDraw::vs / ::fs's layout
	bool drawVsOk_ = false, drawFsOk_ = false;
	alignas(16) float shadowVs_[60] = {}, shadowFs_[12] = {};	// ResolvedShadow::vs / ::fs's layout
	bool shadowVsOk_ = false, shadowFsOk_ = false;
	alignas(16) float idVs_[60] = {};	// ResolvedId::vs's layout
	float idOwner_ = -1;
	bool idVsOk_ = false;
	DrawList scratch_;	// draw() / drawShadow()'s one-draw list
	float cutOf(uint32_t pid) const;	// alpha test cutoff of a part, 0..1 (0 = none)
	void resolveOpenWindows(uint32_t pid);	// render.open_windows: classify a part's islands and build its patched index buffer
	sg_buffer ibufFor(uint32_t pid) const;	// the index buffer a part draws with: its open_windows patch when the switch is on, else the mesh's
	int meshFor(const std::string& file);
	int textureFor(const std::string& file);
	// The texture rule, in one place. scriptTexture (out, optional): the submesh's own Ogre material supplied the file. missingAsBlack: a BUILDING_PART's
	// record path the install lacks resolves to the engine's default black.dds (rwh.160), not ""; the window survey and a feature record keep the
	// no-albedo answer (unmeasured there).
	std::string textureFile(const BuildingPartDef& def, const std::string& submeshMaterial, bool* scriptTexture = nullptr, bool missingAsBlack = true) const;
	std::string recordTexture(const std::string& recordPath, bool missingAsBlack) const;	// a MATERIAL_SPEC texture path -> the file to bind
	void ensurePart(uint32_t part);
	void queue(bool isMesh, int id, const std::string& path, float prio);
	void worker();
	void drain(bool wait, int maxItems);
	void uploadMesh(MeshEntry& m, MeshCpu& cpu, bool blocking);	// blocking: drain(wait), a blocking update's; the creates go to the frame lane
	void uploadTexture(TexEntry& t, DdsImage&& tex, bool blocking);
	void promote();	// Uploading entries whose Handle is ready -> Ready (or Failed)
	sg_view texView(int t, sg_view fallback) const;	// render side: the texture's view if it is Ready and sokol took it, else fallback
	std::vector<std::pair<bool, int>> uploading_;	// (isMesh, id): Uploading, or Ready from a frame-lane create whose texture may still fail
	void evict();
	const BInfo* infoFor(uint32_t building);
	int floorableOf(uint64_t key) const;
	// The building's staircases' flights (StairHot), built once from their meshes; null until every staircase mesh is loaded (retry next frame), empty
	// when the building has no `is stairs` part.
	std::vector<StairHot>* stairHots(uint32_t building);
	static bool stairTopStep(const MeshEntry& m, const float offset[3], StairHot& hot);	// measure one staircase's flight from its mesh
	void updateFocus();
	int shownOf(uint32_t building) const;
	bool aboveSlab(uint32_t building, int floor, const double p[3], double bottom);
	void findOccluders(const double cam[3], std::string* report = nullptr);	// report: a line per building the segment crosses, and why it is not one
	int floorOfInstance(const BuildingInstance& b);
	static bool decodeMesh(const std::string& path, MeshCpu& out);

	const ResourceIndex* res_ = nullptr;
	const MaterialLib* mats_ = nullptr;
	BuildingSet data_;
	std::vector<PartRt> partRt_;
	std::vector<uint32_t> partStamp_;
	std::vector<float> partScore_;	// parallel to partStamp_: valid when the stamp is this frame's
	void noteScore(float& score, uint64_t& scoreFrame, rq::Prio& p, float s);
	float streamScore(const BuildingInstance& b, const double cam[3], const float mvp[16]) const;
	static constexpr double kNearView = 6000.0;	// in view and nearer than this: the first class
	void rerank();	// the queued decodes' priorities from the entries' scores (update, once a frame)
	float partPrio(uint32_t pid) const { return partStamp_[pid] == (uint32_t)frame_ ? partScore_[pid] : 3e5f; }
	std::vector<MeshEntry> meshes_;
	std::vector<TexEntry> textures_;
	std::unordered_map<std::string, int> meshIds_, texIds_;
	std::vector<Group> groups_;
	std::unordered_map<uint64_t, int> groupIds_;
	std::vector<int> active_;
	std::vector<float> frameData_;
	std::vector<float> fitGround_, fitRel_;
	std::vector<uint32_t> needed_;
	std::unordered_map<uint32_t, BInfo> binfo_;	// by building instance, once all its meshes are loaded
	std::unordered_map<uint32_t, std::vector<StairHot>> stairs_;	// by building instance, once its staircases' meshes are loaded
	std::unordered_map<uint32_t, float> construction_;	// by building instance: progress < 1
	std::vector<int8_t> furnFloor_;			// per furniture instance: derived floor, -2 = not yet known
	std::unordered_map<uint64_t, std::array<double, 3>> doorOffsets_;	// setDoors
	size_t staticCount_ = 0;			// instances from setData; the props follow them in data_.instances
	// The static instances that are neither a feature nor furniture (parent < 0), ascending; and those of them with floors (maxFloor != 0). Built in setData
	// (static instances never change after it): the per-frame scans that only ever accept these (findOccluders, the cursor's buildings, rayBuilding)
	// walk them instead of all ~50k static instances, in the same order, so their results are unchanged (kenshi-k16.2.205.12.71: 2-3 ms a frame).
	std::vector<uint32_t> topLevel_, topFloors_;
	bool editorOn_ = false;
	double editorPos_[3] = {};
	float editorQ_[4] = { 1, 0, 0, 0 };		// setEditor's rotation (w, x, y, z)
	int editorInstance_ = -1;			// the instance setProps added for it, -1 = none in this instance set
	std::vector<uint32_t> propList_;		// the prop instances, an extra cell list for update()
	double farDist_ = 0;		// this frame's exterior draw distance (render.building_far, the fog, drawLimit; the load radius at least)
	void fadeOf(uint32_t pid, float& start, float& end) const;
	std::unordered_map<int64_t, std::vector<uint32_t>> furnGrid_;	// layout furniture (parent >= 0) by 4 u square (furnKey): a container thing standing on one is that piece (setProps)
	static int64_t furnKey(double x, double z) { return ((int64_t)std::floor(x / 4) << 32) ^ (int64_t)(uint32_t)(int32_t)std::floor(z / 4); }
	std::unordered_map<uint64_t, uint32_t> propMeshes_;	// props with a mesh (hasMesh) -> the instance drawing it
	FloorStatus floor_;
	int focus_ = -1;
	std::vector<uint32_t> cuts_;		// the sliced buildings (an occupant inside)
	int viewLogged_ = 3;	// the level the last "floor: view A -> B" line ended on (the session starts on 3, like visible_)
	const char* viewWhy_ = "player";	// the floor model's reason for the level since (ff::reasonName)
	int visible_ = 3;	// the visible floor: the floor model's level (view_), written by updateFocus each frame and by stepFloor / restoreView / resetFloors
	ff::FloorFollow view_;	// the floor view's one model (floorfollow.hpp, kenshi-k16.2.236.14)
	std::vector<ff::Flight> flights_;	// this frame's sample flights (the sample building's StairHot list as ff::Flight)
	std::unordered_set<uint32_t> occluders_;	// between the camera and seen this frame (autoGhost)
	std::unordered_map<uint32_t, float> ghosts_;	// occluders fading: 0 solid .. 1 glass
	struct LightRt
	{
		float rel[4];		// camera-relative position, radius
		float colour[4];	// x power, w = 1: spot
		float dir[4];
		float cone[4];		// cos inner / 2, cos outer / 2, falloff
		float near;		// distance from the camera to the light's sphere (fillLights' order)
		uint64_t id = 0;	// the light debugger's lamp id (knobs/lightterms.hpp): instance << 32 | light index, bit 31 set = the building's own list
		uint32_t owner = 0xffffffffu;	// the building (not furniture) instance the lamp belongs to, with lamp = its slot in lampReg_ (render.lamp_occlusion, kenshi-jj3.9.1); lamp < 0: no visibility volume
		int lamp = -1;
	};
	std::vector<LightRt> lights_;			// update(): the lights in view
	// (kenshi-k16.2.205.11) One light the pack's cull accepted, before the PULSE / SHIMMER phase: the cull/pack pass is skipped while its
	// inputs do not move, but the phase moves with the clock every frame, so pulseLights() rebuilds lights_ from these each frame.
	struct LightCand
	{
		float rel[4];		// camera-relative position, radius
		float colour[3];	// the record's colour, unscaled
		float base = 0;		// brightness + the instance's roll, pre-phase
		float spot = 0;
		float dir[4];
		float cone[4];
		float near = 0;
		int effect = 0;
		uint64_t id = 0;
		uint32_t owner = 0xffffffffu;	// render.lamp_occlusion: see LightRt
		int lamp = -1;
	};
	std::vector<LightCand> lightCands_;		// the pack pass's candidates (pulseLights)
	void pulseLights();
	std::vector<PlacedEffectRt> placed_;		// update(): the effects in view
	// Dirty tracking (kenshi-k16.2.205.11): the frame's cull/pack inputs (cam, mvp, fog, shadow far, the flags and the floor / peek / ghost /
	// construction state) hashed after the streaming pass, so an unchanged frame keeps last frame's groups, instances and mask.
	uint64_t packSignature(const double cam[3], const float mvp[16], float fog, double shadowFar) const;
	uint64_t packSig_ = 0;
	bool packValid_ = false;
	uint64_t dataGen_ = 0;		// setData / setProps / addPart: the instance and part set changed
	uint64_t doorGen_ = 0;		// setDoors changed a door's pose (the shadow cache's key)
	// The loading stand-in of a building instance (kenshi-k16.2.258.4; the character pattern, render.loading_standin): per instance, all or nothing.
	// Its shell meshes not all through: nothing drawn. Meshes through, a texture still streaming: every part flat black (the Group::standin lists).
	// The last texture ready: the whole instance switches to its real look over kStandinFade (the Group::fade lists beside the stand-in's, dithered
	// against each other), never a frame of black and textured parts of one instance outside that switch.
	struct Stand { bool stood = false; double readyAt = -1; };
	std::unordered_map<uint32_t, Stand> stand_;	// instance index -> its switch (only instances that stood in; erased when the switch is done)
	double standNow_ = 0;		// this update's clock for the switch: wall seconds, a scripted (blocking) run's frame / 60
	double standT0_ = -1;		// standNow_ at the first update (dev.building_standin_hold counts from it)
	bool standHeld_ = false;	// dev.building_standin_hold: every building's textures count as not ready
	bool standFading_ = false;	// this pack has a switch under way: the next frame repacks (the cover moves with the clock)
	int standMode_ = 0;			// render.loading_standin: 0 silhouette, 1 double (black too), 2 off
	uint64_t stateGen_ = 0;		// a mesh or texture became ready, failed or was evicted
	uint64_t holdGen_ = 0;		// a mesh's state moved or a part found its mesh: what instanceHolds sees changed (bedRay's memo, kenshi-k16.2.205.12.111). Logic thread
	void addLight(const BuildingInstance& b, uint32_t ii, const BuildingPartDef& def, uint32_t li, const double cam[3], const float planes[4][4], double maxDist);
	void addOwnLight(const BuildingInstance& b, uint32_t ii, uint32_t li, const double cam[3], const float planes[4][4], double maxDist);
	void addEffect(const BuildingInstance& b, uint32_t ii, int32_t part, const BuildingPartDef* def, uint32_t ei, const double cam[3], const float planes[4][4]);

	// light probes: by building type (BuildingInstance::nameId); the bake runs on probeThread_ (one at a time, at most 4 threads inside it)
	struct ProbeEntry
	{
		std::shared_ptr<const lightbake::Grid> grid;	// null while pending
		bool failed = false;
		std::string why;
		bool openWindows = false;	// render.open_windows when this bake started: a switch change re-bakes
	};
	struct ProbePart
	{
		std::string mesh;
		BuildingPartDef def;
		float off[3];
	};
	void bakeProbes(uint32_t nameId, std::string name, std::vector<ProbePart> parts, std::array<float, 3> mn, std::array<float, 3> mx, float groundY);
	// The bake's triangles of the parts, from the meshes' position/normal/uv and each submesh's diffuse texture (the albedo the light bounces off); the
	// sky bake uses the luma, the sun bake the RGB tint. Shared by both bakes; one mesh texture decode per file.
	std::vector<lightbake::Tri> buildTris(const std::vector<ProbePart>& parts, bool openWindows) const;
	mutable std::mutex probeMu_;
	std::unordered_map<uint32_t, ProbeEntry> probes_;
	std::thread probeThread_;
	std::atomic<bool> probeBusy_{ false };
	std::shared_ptr<const lightbake::Grid> gridFor(uint32_t bi);	// the baked grid of building bi's type (null while baking; starts the bake)
	// lamp occlusion (render.lamp_occlusion, kenshi-jj3.9.1): the lamps of a building instance (its own, its parts' and its furniture's) as update() meets them, up to lightbake::kMaxLamps,
	// by owner. A new lamp bumps `version`; a worker job (occThread_) traces one ray per probe per lamp through the type's triangles (an Occluder, kept for a few types) and
	// publishes the bytes under the owner (lampVis_, probeMu_).
	struct LampReg
	{
		std::vector<uint64_t> keys;	// instance << 33 | own << 32 | light index
		std::vector<std::array<float, 3>> local;	// building-local mesh units (the probe grid's frame)
		int version = 1;
	};
	struct LampVis
	{
		std::shared_ptr<const std::vector<uint8_t>> data;	// kMaxLamps bytes per probe; null: the job found nothing to trace
		int version = 0;
		int count = 0;	// the lamps it was traced for
	};
	void registerLamp(const BuildingInstance& b, uint32_t ii, uint64_t key, const double world[3], LightCand& out);
	void kickLamps(const double cam[3]);	// logic side, end of update(): start the trace job of the nearest building whose lamp list is newer than its visibility
	void lampJob(uint32_t owner, uint32_t nameId, std::vector<ProbePart> parts, std::shared_ptr<const lightbake::Grid> grid, std::vector<std::array<float, 3>> lamps, int version);
	std::vector<ProbePart> probePartsFor(uint32_t bi);	// the type's baked parts (gridFor's rule); empty until every mesh is loaded
	std::unordered_map<uint32_t, LampReg> lampReg_;
	std::unordered_map<uint32_t, LampVis> lampVis_;
	std::unordered_map<uint32_t, std::shared_ptr<lightbake::Occluder>> lampOccluders_;	// by type; only the occThread_ touches it while occBusy_
	std::thread occThread_;
	std::atomic<bool> occBusy_{ false };
	// the parts and bounds the bakes trace, once every mesh of building bi is loaded (false: not yet)
	bool probeParts(uint32_t bi, std::vector<ProbePart>& parts, std::array<float, 3>& mn, std::array<float, 3>& mx, float& groundY);
	// The sun transfer (render.sun_bounce, kenshi-jj3.4.1), one per building type: the latest bake and the one it replaced (cross-faded at run time). The
	// direction is the world one it was baked for; a bake is started when it is more than kSunStep degrees off and no other bake runs.
	struct SunEntry
	{
		std::shared_ptr<const lightbake::Grid> cur, old;	// the baking thread publishes here under sunMu_
		float dir[3] = {};					// world sun direction cur was baked for
		float ground[3] = {};
		bool pending = false, failed = false;
		bool openWindows = false;	// render.open_windows when this bake ran
		std::string why;
	};
	bool sunFor(uint32_t bi, const float sunDir[3], const float groundAlbedo[3], std::shared_ptr<const lightbake::Grid>& cur, std::shared_ptr<const lightbake::Grid>& old);	// starts a bake if stale; false: none
	void bakeSun(uint32_t nameId, std::string name, std::vector<ProbePart> parts, std::array<float, 3> mn, std::array<float, 3> mx, float groundY, const float worldDir[3], const float localDir[3], const float ground[3], std::string trigger);
	bool camInBuilding(uint32_t bi, const double cam[3]);	// the camera inside the building's interior bounds
	mutable std::mutex sunMu_;
	std::unordered_map<uint32_t, SunEntry> sun_;
	std::thread sunThread_;
	std::atomic<bool> sunBusy_{ false };
	std::atomic<size_t> sunBakes_{ 0 };		// since start (the report line)
	std::atomic<double> sunLastSeconds_{ 0 };	// the last sun bake's wall time
	std::atomic<double> sunFinishedAt_{ -1e9 };	// steady-clock seconds when the last sun bake finished (duty cycle: next stepped bake waits 2x sunLastSeconds_)
	bool sunDeferLogged_ = false;			// the duty-cycle deferral was logged for the current wait (render thread only)
	std::vector<uint32_t> volNear_;	// lightVolumes(): the nearest buildings with interiors, rescanned every 30 calls
	int volScan_ = 0;
	int probeTarget_ = -1, probeScan_ = 0;	// the nearest building with interiors, rescanned every 30 calls
	size_t probeDrawn_ = 0;					// the points lightProbes() returned last
	double probeAt_[3] = {};				// that building's origin
	double probeDist_ = 0;					// the camera's distance to that building's origin
	uint32_t probeAsked_ = 0xffffffffu;		// nameId of the last building lightProbes() was asked about

	std::vector<std::thread> threads_;
	std::mutex mu_;
	std::condition_variable jobCv_, resCv_;
	std::deque<Job> jobs_;
	std::deque<Result> results_;
	std::atomic<int> outstanding_{ 0 };
	bool quit_ = false;

	struct MaskGroup	// a mask part's submesh and its instances (12 floats each), packed into frameData_ after the drawn groups
	{
		uint32_t part;
		int sub, first = 0;
		std::vector<float> inst;
	};
	std::vector<MaskGroup> maskGroups_;	// update() while maskWanted; renderMask draws them
	struct HullRef { uint32_t instance, part; };
	std::vector<HullRef> hulls_;	// update(): the interior mask parts of the buildings near the camera (insideInterior)

	sg_shader shd_ = {};
	sg_pipeline pip_ = {}, pipInterior_ = {}, pipGlassDepth_ = {}, pipGlass_ = {}, pipFlip_ = {};
	sg_shader maskShd_ = {};
	sg_pipeline pipMaskFront_ = {}, pipMaskBack_ = {};
	sg_shader idShd_ = {};
	sg_pipeline pipId_ = {}, pipIdInterior_ = {};
	double cursorDir_[3] = {};
	bool cursorOn_ = false;
	std::vector<uint32_t> cursorBuildings_;	// this frame's buildings along the cursor ray
	struct IdDraw { int group; uint8_t list; uint32_t index, owner; };	// list 0 inRange, 1 outRange, 2 glass; index within it
	std::vector<IdDraw> idDraws_;
	bool culled_ = false;
	sg_shader shadowShd_ = {};
	sg_pipeline shadowPip_ = {};
	bool foliagePass_ = false;
	sg_sampler smp_ = {};
	sg_view groundView_ = {};
	sg_view groundAlphaView_ = {};	// the colour map's alpha mosaic (TERRAIN-mode gloss scale)
	sg_sampler groundSmp_ = {};
	double cam_[3] = {};
	sg_buffer instBuf_ = {};
	sg_view grey_ = {}, flat_ = {};
	sg_image greyImg_ = {}, flatImg_ = {};
	sg_view dust_ = {};	// Turbulent.dds, the dust coat's noise (grey_ when missing)
	sg_image dustImg_ = {};
	sg_view blood_ = {};	// materials/bloody.png, a severed limb's blood map (grey_ when missing)
	sg_image bloodImg_ = {};
	size_t instCapacity_ = 0;
	uint64_t frame_ = 0;
	float fog_ = 60000.0f;
	double time_ = 0;
	// the cut-away flip fade's eye ring (update()): the eye history in wall-clock seconds, and the eye at (k + 1) x kFlipFade / 8 seconds ago relative to the eye now
	struct EyeSample { double t; double p[3]; };
	std::deque<EyeSample> eyeHist_;
	float flipEye_[8][4] = {};	// logic side only: update() writes it, resolveDraws() copies it into the flip draws' uniforms
	bool flipMoving_ = false;	// logic side only
	Stats stats_;
};
