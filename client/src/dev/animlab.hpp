#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"
#include "sky.hpp"	// LightParams
#include "ui/widget.hpp"	// ui::PortraitRef: the offscreen image the browser window shows
#include "dev/labframe.hpp"
#include "renderqueue.hpp"

class Characters;
class Shadow;
class ResourceIndex;
namespace kdata { class GameData; }

// The animation lab (docs/CLIENT_GOALS.md "Animation lab"): one lab character, never drawn in the world, rendered offscreen every frame (the portrait
// path) on a flat 1 u grid with its own camera, one clip on loop; playback control, the sole points with trails, and the measurement the tool exists
// for. Host side only: the window that shows it is ui::AnimBrowser (the UI session's); it binds to State + Actions. Headless: the `lab` script ops.
namespace animlab
{
	struct Clip
	{
		std::string name, kind;	// kind: "record" (an ANIMATION) or "raw" (a skeleton clip)
		std::string clip;	// the skeleton clip it plays: a raw clip's name, a record's `anim name`
		float length = 0;	// seconds
		int frames = 0;		// the original's 30 fps clip frames
		// the data groups it belongs to (State::groups), in that order; a clip several records/techniques name is in each
		std::vector<std::string> groups;
		std::string usedBy;	// one line: the records, techniques, pain overlays and species that name it
		bool relocates = false;	// a matching ANIMATION record's `relocates`
		bool autoFollow = false;	// the root lock is the game's handling: a relocates record, or a technique (not a pure block)
	};

	struct State
	{
		ui::PortraitRef view = {};	// the offscreen render; view.view.id 0: none yet
		std::vector<Clip> clips;
		std::string clip;
		bool playing = true;
		float time = 0;
		int frame = 0, frames = 0;
		double speed = 1;
		std::string camera = "side";	// "side" | "front" | "top" | "orbit"
		std::string plant;	// "lheel" | "lball" | "rheel" | "rball" | a bone name | ""
		bool trails = false, rootPath = false, boneNames = false;
		std::string readout;	// the measurement, multi-line
		std::string reply;	// the last action's one-line answer
		std::vector<std::string> bones, races;
		std::string race;
		bool female = false;
		bool open = false;	// the debug panel's "Animations" command opened it (the window binds)
		// the clip list's groups and its filter: every group name in list order, the one selected ("" = all)
		std::vector<std::string> groups;
		std::string group;
		// the root lock: the pose zeroes Bip01's x/z and the lab character's node carries them (the camera and grid follow it), as the game
		// hands a relocates record's or an action's root travel to the entity's position
		bool follow = false;
		float travel = 0;	// the current clip's Bip01 path over one loop, u
		std::string root;	// the root readout: "relocates (root-locked)" / "in place" / "travels N u per loop (unlocked)"
	};

	struct Actions
	{
		std::function<void(const std::string&)> select;
		std::function<void(bool)> play;
		std::function<void(int)> step;	// -1 / +1 clip frame
		std::function<void(double)> seek;	// seconds
		std::function<void(double)> speed;	// 0.1 .. 2
		std::function<void(const std::string&)> camera;
		std::function<void(double, double)> orbit;	// pixels
		std::function<void(double)> zoom;	// wheel steps
		std::function<void(const std::string&)> plant;
		std::function<void(const std::string&, bool)> overlay;	// "trails" | "root" | "bones", on
		std::function<void()> measure;
		std::function<void()> mark;
		std::function<void(const std::string&)> copy;	// to the clipboard (the readout)
		std::function<void(int, int)> viewSize;
		std::function<void(const std::string&)> race;
		std::function<void(bool)> female;
		std::function<void()> close;
		std::function<void(const std::string&)> group;	// "" = all
		std::function<void(bool)> follow;
	};

	class Lab
	{
	public:
		Lab();
		~Lab();
		Lab(const Lab&) = delete;
		Lab& operator=(const Lab&) = delete;

		// Cheap: records where to init from. The Characters set, the render target and the character are built on the first open or `lab` op,
		// not at startup (every game start paid for them), so a game that never opens the browser pays nothing.
		void setup(const ResourceIndex* res, const std::string& dataDir, const Shadow* shadow);	// the shadow's views: the character pipeline binds them (its own light is the portrait one)
		bool available() const { return res_ != nullptr; }	// set up: the browser may open (it inits on the way)
		void shutdown();

		const State& state() const { return st_; }
		Actions actions();	// bound to this
		bool ready() const { return ready_; }
		bool active() const { return st_.open || touched_; }	// renders only while open or scripted
		// The GPU half of the init or of a resize is queued and its packet not drawn yet: a `lab` op waits a frame for it (kenshi-k16.2.205.12.69)
		bool pending() const { return ready_ && tgtQueued_ && !tgt_.ready(); }
		void touch();	// a `lab` script op: init on first use
		void open(bool on);

		void select(const std::string& name);
		void play(bool on);
		void step(int dir);
		void seek(double t);
		void speed(double x);
		void camera(const std::string& name);
		void orbit(double dx, double dy);
		void zoom(double wheel);
		void plant(const std::string& point);
		void overlay(const std::string& which, bool on);
		void measure();	// one loop, the summary in readout and `lab:` lines
		void list();	// every clip name, kind, length and frames on stdout (`lab list`)
		void mark();
		void viewSize(int w, int h);
		void race(const std::string& name);
		void female(bool on);
		void close();
		void group(const std::string& name);	// the clip list's filter ("" or "all": every group); no name: list the groups
		void follow(bool on);	// the root lock, on a clip the game does not decide it for

		void update(double dt);	// advances the clip time (playing)
		void frame(dev::LabFrame& f);	// logic thread, at the packet's fill: the draw's values (f.draw false: nothing to render)
		static void render(const dev::LabFrame& f);	// render thread: the offscreen pass from the frame alone, outside any pass (like Portraits::update)
		bool csv(const std::string& path);	// per-frame rows, one loop
		bool shot(const std::string& path);	// the last offscreen render as a PNG

	private:
		struct Sample
		{
			double p[2][2][3] = {};	// foot (L, R) x sole (heel, ball) x xyz, model space (the lab character stands at the origin)
			double root[3] = {};	// Bip01 (model space): the body's own motion, for the in-place check
			bool ok = false;
		};
		struct Window
		{
			int foot = 0, sole = 0, from = 0, count = 0;	// sample indices; the run wraps past the sample count
			double travel = 0, speed = 0;	// u, u/s
			double fwd = 0, lat = 0;	// the body's motion while the point is planted (u/s, the character's frame)
			double slipRms = 0, slipMax = 0;
		};
		bool init(const kdata::GameData& gd, std::string& err);	// the lazy init: the race list, the render target and the character
		bool ensure();	// init on first use (loads the merged game data then); false with st_.reply set
		void build();	// (re)creates the character and the clip list for the race
		void applyTime();	// the clip time onto the layers, and plant mode's body displacement
		void measureLoop(std::vector<Sample>& samples, int n) const;	// n samples over one loop (the measure: kMeasureSamples, any playback rate)
		void analyse(const std::vector<Sample>& samples, std::vector<Window>& windows, std::string& summary) const;
		bool target(int w, int h, std::string& err);
		void dropTarget();
		// the groups and the root lock (task lab slice 3): the ANIMATION / COMBAT_TECHNIQUE / LOCATIONAL_DAMAGE / CROSSBOW / ANIMAL_CHARACTER
		// figures the classifications and the `relocates` rule read, from the merged data of this load
		struct Rec
		{
			std::string name, clip;
			int layer = 1, category = 0, combat = 2, handL = 0, handR = 0, stumbleFrom = 0;
			unsigned carry = 0;	// being carried 1, carrying left 2, carrying right 4
			bool idle = false, action = false, prone = false, relocates = false, big = false;
			float move = 0, min = 0, max = 0, strafe = 0;
			std::vector<std::string> stumbles;	// the LOCATIONAL_DAMAGE parts it is the stumble for
		};
		struct Tech { std::string clip, by; bool defence = false, pureBlock = false; };	// a COMBAT_TECHNIQUE: its clip, its "used by" text
		void index(const kdata::GameData& gd);
		void classify();	// every clip's groups, usedBy and root-lock rule (build)
		void applyFollow(bool on);	// the root lock on the character: the pose zeroes the root, the node carries it
		void place();	// the character's node: the plant shift, plus the followed root
		float rootTravel();	// the current clip's Bip01 path over one loop, u
		std::string rootLine() const;	// the root readout for the current clip
		const Clip* current() const;
		std::unordered_map<std::string, Rec> recs_;	// ANIMATION by folded record name
		std::vector<Tech> techs_;
		std::unordered_map<std::string, std::string> pains_;	// folded ANIMATION name -> "pain overlay: Head (60%)"
		std::unordered_map<std::string, std::string> reloads_;	// folded ANIMATION record name -> "CROSSBOW reload anim (Junkbow, ...)"
		std::unordered_map<std::string, std::string> animals_;	// folded clip name -> "ANIMAL_CHARACTER X (animations, strafe); ..."
		bool follow_ = false;
		double root_[2] = {};	// the followed root x/z the node carries (0 while unlocked)

		std::unique_ptr<Characters> chars_;
		const ResourceIndex* res_ = nullptr;	// setup(): the lazy init's inputs
		std::string dataDir_;
		bool ready_ = false, touched_ = false;
		State st_;
		float time_ = 0;	// unwrapped clip time
		// camera
		double orbitYaw_ = 0.6, orbitPitch_ = 0.28, dist_ = 48;
		// render target: made and resized by frame-lane jobs (renderqueue.hpp, kenshi-k16.2.205.12.69), as the logic half never calls sg_make_*;
		// the job publishes what it made into tgt_, and logic names it in a packet only once tgt_.ready(). The overlay contexts live as long as the lab;
		// shutdown (cleanup, one thread, after rq::flushAll) drops them with the target.
		struct Target
		{
			bool ok = false;	// the colour image was made
			sg_image color = {}, depth = {};
			sg_view colorTex = {}, colorAtt = {}, depthAtt = {};
			sg_sampler sampler = {};
			sgl_context sgl = {};
			sdtx_context sdtx = {};
		};
		rq::Handle<Target> tgt_;
		bool tgtQueued_ = false;	// a target job was queued (tgt_ names it)
		int texW_ = 0, texH_ = 0;	// the size asked for (logic side; the job makes it)
		static void destroyTarget(const Target& t, bool contexts);	// render side (or one thread: shutdown)
		LightParams light_ = {};
		// trails / root path: the last loop's points, sampled as it plays
		std::vector<float> trail_[2][2];
		std::vector<float> rootPath_;
		// plant mode: the body displacement held across contact windows
		std::vector<Window> keep_ = {};
		double shift_[3] = {};
		double plantBase_[3] = {};	// the shift when the current contact window began: the body walks on across windows
		int plantWindow_ = -1;
		size_t plantClip_ = std::string::npos;	// st_ clips index keep_ was measured for
	};
}
