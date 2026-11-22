#pragma once
// The hooks the game code calls into the dev-only tools (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153).
//
// Dev-only code (the playtest store and its Notes / Scenarios windows, the bd writer behind --bead, scenario loading) lives in
// client/src/dev/ and is built only with the CMake option KCLIENT_DEV_TOOLS (default ON). The game calls these coarse hooks; with the
// option OFF they are inline no-ops, so the game compiles, links and plays without any of it. Keep this list short: one hook per
// place in the game's frame / session life, not one per call site of the tools. The ON definitions are in dev/devhost.cpp, which
// holds the whole host side of the playtest tooling; what it needs of the game it asks through the Env main.cpp hands to init().
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef KCLIENT_DEV_TOOLS
#include <functional>

#include "frame/labframe.hpp"
#include "net/simlink.hpp"

#include "dev/followstream.hpp"
#include "dev/launchguard.hpp"	// launchGuard: no window from a Claude session outside gamescope (kenshi-k16.2.392)
#endif

class SimView;
namespace dev { struct Context; }
namespace dbg { struct Tree; }
class Characters;
class CollisionPick;
class MaterialLib;
class ResourceIndex;
#include "render/shadow.hpp"
class GpuTimer;
struct FramePacket;
namespace kworld { class Heightmap; }

namespace dev
{
#ifdef KCLIENT_DEV_TOOLS
	// What the dev tools know of the running game: only main.cpp can answer. Filled once, before init().
	struct Env
	{
		std::function<bool()> simActive;	// a sim session (live or a replay) is up
		std::function<bool()> live;	// a live server session: the scenario wire has someone to talk to
		std::function<bool()> worldOn;	// the first world frame has been drawn
		std::function<bool()> paused;	// the sim's own pause bit
		std::function<void(bool)> pause;
		std::function<const simlink::SimLinkDecoder&()> decoder;
		std::function<bool()> scenarioList;	// SCENARIO_LIST_REQ
		std::function<bool(const std::string&, uint32_t)> scenarioLoad;	// SCENARIO_LOAD (seed 0: the server picks)
		std::function<void()> scenarioEnd;	// SCENARIO_END
		std::function<bool()> pumpLink;	// keeps only the link moving (no frame): false when it is gone
		std::function<void(const std::string&)> error;	// a wire request that was never answered: the message, and the load screen left
		std::function<std::string()> stamp;	// "HH:MM.S Name (x, z): ", the game time and the selected character
		std::function<std::string()> sessionSim;	// a local server's build ("" unknown)
		std::function<void(bool ok, bool hasCentre, double x, double z)> started;	// a scenario's world is in: the game's response (menus, camera)
		struct ShotInfo { std::string time, place, selected; };
		std::function<ShotInfo()> shotInfo;	// what an F12 shot says of the game: its time, where it looks, the selected character
		std::function<std::string()> restLine;	// the creator preview's rest pose: "rest STANCE knee L R pelvis H" ("" with no creator)
		std::function<bool(const std::string&)> treesPending;	// the shot's debug-trees capture is still running
		std::function<bool()> labScripted;	// the script holds a `lab` op: a headless lab run (no world characters)
		std::string repoRoot, clientHash;
	};

	bool parseArg(int argc, char** argv, int& i, std::vector<std::string>& windowOff);	// a dev command-line flag (--playtest --play --bead --findings --playtests --sg-owner-probe --scene-* --view-mesh --tex --dist): true when taken
	// The sokol thread-owner assert (dev/sgowner.cpp, kenshi-k16.2.205.12.19): every traced sg_* call off the owning thread aborts.
	bool sgOwnerArg(const std::string& a);	// --sg-owner-probe (parseArg's)
	void sgOwnerInstall();	// right after sg_setup: the calling thread owns sokol, the hooks go in (and --sg-owner-probe's off-thread call runs)
	void sgOwnerTake();	// the calling thread becomes sokol's owner (the render thread, when it starts)
	// --sg-owner-logic (kenshi-k16.2.205.12.22): the assert forced on one thread. Inside an SgLogicScope (logic_frame) sokol has no owner, so any
	// traced sg_* call aborts, as it will on the logic thread; an SgAllowScope gives it back for a caller that is legal there (a load step,
	// rule 7) or not yet converted to the resource queue (renderqueue.hpp; the scope names the follow-up). Without the flag both are no-ops.
	struct SgLogicScope { SgLogicScope(); ~SgLogicScope(); SgLogicScope(const SgLogicScope&) = delete; std::thread::id prev; };
	struct SgAllowScope { explicit SgAllowScope(const char* why); ~SgAllowScope(); SgAllowScope(const SgAllowScope&) = delete; std::thread::id prev; };
	void usage();	// their lines of the usage text
	// Side-by-side follow mode (dev/followstream.cpp, docs/FOLLOW_STREAM.md, kenshi-cym.40.2.5): --follow-stream HOST:PORT --import-save DIR --import-report PATH.
	bool followArg(int argc, char** argv, int& i);	// parseArg's: --follow-stream --import-save --import-report
	void followLockToggle();	// the F8 panel's row
	bool followLockOn();
	bool followActive();	// --follow-stream was given (the panel shows its row)
	void followValidate(bool debug);	// after parsing: the flags need --debug (and a well-formed address) or the run exits 2
	bool followEnabled();	// --follow-stream was given: the title is skipped and the local server is started with the import
	void followServerArgs(std::vector<std::string>& args);	// start_local_server: --import-save, --import-report, --paused
	void followInit(const FollowHost& host);
	void followPoll();	// every frame: the connection, the records, the camera easing, the clock beat
	void followQuit();	// quitting: `bye`
	bool mirrorLocked();	// the stream holds the player's camera and mouse orders
	bool followCamera();	// the stream moves the camera (update_rts keeps the pivot height it was given)
	void init(const Env& env);	// the store and, for --bead, the bd writer; call once the options are parsed
	bool scenariosButton(bool debug);	// the SCENARIOS menu button shows (--debug or --playtest, never in a --bead run)
	void openScenarios();	// the SCENARIOS button's action
	void createWindows();	// the playtest's two windows, with the UI up
	void updateUi();	// every frame: the windows' state
	void hudState(bool& playtestRun, bool& notesOpen);	// the HUD's NOTES button
	void notesKey(bool button);	// Ctrl+N / the NOTES button
	bool restMetric();	// the script op `restmetric`: prints the creator preview's rest pose (knee angles, pelvis height) for IK on / off comparison (kenshi-k16.17.7.4)
	bool gaitOp(const std::string& arg);	// the script op `log gait ID|players|combat|all|off` (dev/gaitlog.cpp, kenshi-h2v.1.9.1)
	void gaitFrame(const SimView& sim, Characters& chars, const double cam[3], double dt);	// every frame: the gait lines of the characters it logs
	bool posOp(const std::string& arg);	// the script op `log pos ID|players|all|off` (dev/poslog.cpp, kenshi-k16.2.180.1)
	void posFrame(const SimView& sim, const Characters& chars, const double cam[3], double dt);	// every frame: the sim / drawn positions, the drawn's frame move, the wire samples, the clips and the body's draw state
	bool scriptOp(const std::vector<std::string>& args);	// the script op `playtest ...`: false for a malformed one
	std::string buildLine();	// "build: client <hash> sim <hash or ?>": the first line of a shot's .txt and .events.txt
	void shotBegin(const std::string& shotPath);	// a shot was just saved: a --bead run copies it to the item and comments
	void shotWritten(const std::string& shotPath);	// the screenshot writer is done with shotPath (ok or not): its pending --bead copy may go
	void shotDrain();	// every quit path, after the writes done callbacks ran: every pending --bead copy and comment goes now (deadline and trees wait ignored), the bd writer drained
	void shotFrame();	// every frame after a shot: a pending --bead copy goes once its trees are done
	void newGameLeave();	// a new game starts: the run being played ends (a recipe's does not)
	void leaveRun();	// the session ends (back to the title): the run being played is rewritten and its END sent
	void poll();	// every frame: the scenario wire's answers (list, started, metrics), --play, the playtest timeouts, the bd writer's answers
	void cleanup();	// quitting: the run is rewritten, the queued --bead comments go first, the END's metrics are waited for (1.5 s); the windows go
	// The animation lab (dev/animlab.*, its host side in dev/debughost.cpp): one character offscreen on a floor grid, one clip on loop, its own
	// camera; the browser window shows it. The lab inits on its first open or `lab` op, so a game that never opens it pays nothing.
	void labSetup(const ResourceIndex& res, const std::string& dataDir);	// the lazy init's inputs (the character's asset set)
	bool labScripted();	// the script holds a `lab` op (Env::labScripted)
	bool labAvailable();	// set up: the debug panel's Animations row is enabled
	bool labOpen();	// the lab's window is open (overlap: its frames run gated, logicthread::renderIdle)
	bool labOp(const std::vector<std::string>& args);	// the script op `lab clip|race|play|...`: false for a malformed one
	// Before a `lab` op: inits the lab on first use; true while its GPU half (the render queue's job, kenshi-k16.2.205.12.69) is not drawn yet,
	// so the op waits a frame and runs on the next one
	bool labWait();
	void labUpdate(double dt);	// every frame: the lab's own clock (real time, not the sim's dtSim)
	// The lab's draw crosses the threads on the packet (kenshi-k16.2.205.12.56): labFrame (logic thread, at the packet's fill) copies the lab's
	// values into FramePacket::lab; labRender (render thread, every world frame after the shadow pass) draws from that copy alone, its own
	// offscreen pass, when the frame's flag says so (only while open or scripted).
	void labFrame(LabFrame& out);
	void labRender(const LabFrame& f, const Shadow::FrameData& shadow);
	void labShutdown();	// quitting: its render target and character go, while sokol still is
	void animationsCommand();	// the debug panel's Animations row: opens or closes the lab and its window
	void createAnimBrowser();	// with the UI up: the lab's window, its actions bound to the lab
	void animBrowserFrame();	// every frame: the window's view of the lab
	void resetAnimBrowser();	// the UI goes (quit)
	// The debug panel (F8) and trails panel (Shift+F6) host side (dev/debughost.cpp, kenshi-k16.2.153.12): the windows are owned there.
	void createDebugPanels(const Context& cx, bool debug);	// with the UI up: the windows (only with --debug), their actions bound to the host
	void resetDebugPanels();	// the UI goes (quit)
	bool toggleDebugPanel();	// F8: false when there is no panel (no --debug)
	void toggleTrailsPanel();	// Shift+F6
	void debugPanelFrame(const Context& cx);	// every frame: the debug view's reply line, the panel's state
	void trailsPanelFrame(const Context& cx);	// every frame: the trails panel's view of the trails
	// The collision debug overlay (dev/colloverlay.cpp; debug.collision, Ctrl+F7; kenshi-k16.2.383.1, wording kenshi-k16.2.382.4): the building collision
	// shapes (the barricades that block movement and line of sight; bodies do not stand on them, k16.2.382.2) and the drawn terrain bodies stand on,
	// round the camera focus. Toggled by its key and the debug panel's row; the draw runs in logicframe.cpp beside the navmesh overlay's, into the caller's list.
	void collisionToggle();
	void collisionFillToggle();	// debug.collision_fill: the faces filled in the kind colours (kenshi-k16.2.502)
	bool collisionFillOn();
	bool collisionOn();	// the settings key (a dev key: --debug gates the draw, as the navmesh overlay's)
	bool collisionLegend(std::string& line);	// the debug text's line while it is on (its last draw's counts): false when it is off
	void collisionOverlay(const CollisionPick& pick, const kworld::Heightmap* hm, const double focus[3], const float mvp[16], const double cam[3], float width, float height);
	bool dumpOp(const Context& cx, const std::vector<std::string>& args);	// the `dump` ops that read the sim, the buildings, the sky / weather / gpu / effects and the item windows (dev/debughost.cpp): false when args is not one of them
	bool machineOp(const Context& cx, const std::vector<std::string>& args);	// the `machine id=N` script op: opens N's machine window (dev/debughost.cpp); false when args is not one
	bool hintsOp(const Context& cx, const std::vector<std::string>& args);	// the `hints at X Y` / `hints kinds` script ops (dev/hintsop.cpp): the resolved control hints at a screen point, or the kind registry; false when args is not one
	bool charsOp(const Context& cx, const std::string& op, const std::vector<std::string>& args);	// the chars / anim ops (dev/charops.cpp): `dump contacts|feet|bones|anim|idle|motion|bars`, `log feet|hits` (op "dump" or "log"); false when not one of them
	void footFrame(const Context& cx, double dt);	// every frame: the `log feet` lines (dev/charops.cpp)
	bool debugOp(const Context& cx, const std::vector<std::string>& args);	// the `debug weather|weff|time` script ops (dev/debughost.cpp; main.cpp calls it only under KCLIENT_DEBUG_VIEW): false when args is not one of them
	bool physicsOp(const Context& cx, const std::string& op, const std::vector<std::string>& args);	// the physics / ragdoll / foliage / trails / sound ops (dev/debughost.cpp): `dump shapes|ragdolls|physics|ragdoll|foliage|trails`, `physics box|smoke|ragdoll`, `trails ...`, `sound at|char`; false when args is not one of them
	// The shot's debug sidecars (dev/shotsidecars.cpp, kenshi-k16.2.153.13): the text files beside a screenshot. The PNG itself is the game's.
	void shotSidecars(const Context& cx, const std::string& shotPath);	// <shot>.txt (camera, options, pick, owner / primitive / rule, hints, time), .events.txt, .trails.tsv
	void shotTreesBegin(const Context& cx, const std::string& shotPath);	// <shot>.trees.txt: one DEBUG_QUERY per on-screen character and its squad (KCLIENT_DEBUG_VIEW)
	void shotTreesFrame(const Context& cx);	// every frame after a shot: the trees capture's squad pass, its file, its 5 s timeout
	void shotTreesFinish(const Context& cx, const char* why);	// the run's `quit`: keep the answers that came in, list the rest missing
	void shotTreesTree(const Context& cx, const dbg::Tree& t);	// one DEBUG_TREE landed: the pending capture keeps it when it asked
	bool shotTreesPending(const std::string& base);	// this shot's trees capture is still running (a --bead run waits for it)
	void shotTreesReset();	// a sim session opens: no capture is pending
	// The scene test (dev/scenetest.cpp, kenshi-k16.2.153.11 pass 7): client/scenes.txt, --scene-test, `set scene` and the debug panel's rows.
	void sceneSetup(bool debug);	// after parsing: the scenes file and the test's list (exits on a bad one); the scene flags need --debug
	bool sceneTesting();	// a --scene-test run: scripted mode and the held clock
	double sceneSimTime(double animTime);	// a scene shot's fixed animation time, else animTime
	bool scenesNeedCharacters();	// a scene places char= specs: the game data is wanted
	std::vector<std::string> sceneNames();	// the loaded scenes' names (the debug panel's rows)
	bool sceneSetName(const Context& cx, const std::string& name);	// `set scene NAME` and a panel row: false when there is no such scene
	void sceneTestPre(const Context& cx);	// every world frame: apply the next scene
	void sceneTestPost(const Context& cx);	// every world frame after its draw: the frame counts, the floor step, the shot and its JSON
	// The mesh viewer (dev/viewmesh.cpp): --view-mesh renders one mesh instead of the world (dev/meshview.hpp holds the MeshView class).
	bool viewing();
	void viewInit(const ResourceIndex& res, const MaterialLib& mats);	// the mesh, its report and the fit distance
	bool viewFrame(const Context& cx, FramePacket& pk);	// true when viewing: pk is the viewer's own frame (kView), its camera position and text in pk.early
	void viewDraw(const FramePacket& pk, GpuTimer& gpu);	// the viewer's pass and its text, from the packet alone (the render half: no Context)
	void viewShutdown();
	void setViewDist(double d);	// `set dist D`
	void viewScroll(double y);	// the wheel's zoom step (e->scroll_y)
	// The `probe depth` op (dev/probe.cpp) and the `log gait` / `log pos` dispatch (dev/scriptlog.cpp).
	void probeOp(const Context& cx, const std::vector<std::string>& args);
	bool logOp(const Context& cx, const std::vector<std::string>& args);	// false when args is not gait / pos or their argument is bad
	// The vertical height probe (dev/heightprobe.cpp, kenshi-k16.2.384.1): every surface the world-Y line through a point crosses, and the stand ray's window and pick.
	bool heightProbeMode();	// probe mode (Ctrl+F10, --debug): a left click on the world sets the probe instead of acting
	void heightProbeToggle();
	bool heightProbeActive();	// a probe is set at a point (the panel's Clear command is enabled only then, kenshi-k16.2.413.1)
	void heightProbeClear();	// forget the probe: no chart, no line, no sidecar lines
	void heightProbeSet(const Context& cx, double x, double z, bool print);	// probe at x, z (print: the listing to stdout, the script op)
	void heightProbeFrame(const Context& cx);	// every logic frame: the probe's character part (and the drawn meshes' late loads) refreshed
	std::vector<std::string> heightProbeLines();	// the chart, for the debug text (empty with no probe)
	std::string heightProbeReport();	// the same lines for the F12 sidecar ("probe: ..." each)
	void heightProbeDraw(const float mvp[16], const double camPos[3], float width, float height);	// the line and a tick per crossing, through ovl::
	bool heightProbeOp(const Context& cx, const std::vector<std::string>& args);	// `probe height X Z`
#else
	inline void launchGuard(const char*) {}	// a release build has no guard (kenshi-k16.2.392)
	inline bool parseArg(int, char**, int&, std::vector<std::string>&) { return false; }
	inline void sgOwnerInstall() {}
	inline void sgOwnerTake() {}
	struct SgLogicScope { SgLogicScope() {} };
	struct SgAllowScope { explicit SgAllowScope(const char*) {} };
	inline void usage() {}
	inline bool followEnabled() { return false; }
	inline void followServerArgs(std::vector<std::string>&) {}
	inline bool mirrorLocked() { return false; }
	inline void followLockToggle() {}
	inline bool followCamera() { return false; }
	inline bool scenariosButton(bool) { return false; }
	inline void openScenarios() {}
	inline void createWindows() {}
	inline void updateUi() {}
	inline void hudState(bool& a, bool& b) { a = false; b = false; }
	inline void notesKey(bool) {}
	inline bool restMetric() { return false; }
	inline bool gaitOp(const std::string&) { return false; }
	inline void gaitFrame(const SimView&, Characters&, const double[3], double) {}
	inline bool posOp(const std::string&) { return false; }
	inline void posFrame(const SimView&, const Characters&, const double[3], double) {}
	inline bool scriptOp(const std::vector<std::string>&) { return false; }
	inline std::string buildLine() { return {}; }
	inline void shotBegin(const std::string&) {}
	inline void shotWritten(const std::string&) {}
	inline void shotDrain() {}
	inline void shotFrame() {}
	inline void newGameLeave() {}
	inline void leaveRun() {}
	inline void poll() {}
	inline void cleanup() {}
	inline void labSetup(const ResourceIndex&, const std::string&) {}
	inline bool labScripted() { return false; }
	inline bool labAvailable() { return false; }
	inline bool labOpen() { return false; }
	inline bool labOp(const std::vector<std::string>&) { return false; }
	inline bool labWait() { return false; }
	inline void labUpdate(double) {}
	inline void labShutdown() {}
	inline void animationsCommand() {}
	inline void createAnimBrowser() {}
	inline void animBrowserFrame() {}
	inline void resetAnimBrowser() {}
	inline void createDebugPanels(const Context&, bool) {}
	inline void resetDebugPanels() {}
	inline bool toggleDebugPanel() { return false; }
	inline void toggleTrailsPanel() {}
	inline void debugPanelFrame(const Context&) {}
	inline void trailsPanelFrame(const Context&) {}
	inline void collisionToggle() {}
	inline bool collisionOn() { return false; }
	inline bool collisionLegend(std::string&) { return false; }
	inline void collisionOverlay(const CollisionPick&, const kworld::Heightmap*, const double[3], const float[16], const double[3], float, float) {}
	inline bool dumpOp(const Context&, const std::vector<std::string>&) { return false; }
	inline bool machineOp(const Context&, const std::vector<std::string>&) { return false; }
	inline bool hintsOp(const Context&, const std::vector<std::string>&) { return false; }
	inline bool debugOp(const Context&, const std::vector<std::string>&) { return false; }
	inline bool physicsOp(const Context&, const std::string&, const std::vector<std::string>&) { return false; }
	inline bool charsOp(const Context&, const std::string&, const std::vector<std::string>&) { return false; }
	inline void footFrame(const Context&, double) {}
	inline void shotSidecars(const Context&, const std::string&) {}
	inline void shotTreesBegin(const Context&, const std::string&) {}
	inline void shotTreesFrame(const Context&) {}
	inline void shotTreesFinish(const Context&, const char*) {}
	inline void shotTreesTree(const Context&, const dbg::Tree&) {}
	inline bool shotTreesPending(const std::string&) { return false; }
	inline void shotTreesReset() {}
	inline void sceneSetup(bool) {}
	inline bool sceneTesting() { return false; }
	inline double sceneSimTime(double animTime) { return animTime; }
	inline bool scenesNeedCharacters() { return false; }
	inline std::vector<std::string> sceneNames() { return {}; }
	inline bool sceneSetName(const Context&, const std::string&) { return false; }
	inline void sceneTestPre(const Context&) {}
	inline void sceneTestPost(const Context&) {}
	inline bool viewing() { return false; }
	inline void viewInit(const ResourceIndex&, const MaterialLib&) {}
	inline bool viewFrame(const Context&, FramePacket&) { return false; }
	inline void viewDraw(const FramePacket&, GpuTimer&) {}
	inline void viewShutdown() {}
	inline void setViewDist(double) {}
	inline void viewScroll(double) {}
	inline void probeOp(const Context&, const std::vector<std::string>&) { fprintf(stderr, "kclient: script: probe depth needs a GL dev build (KCLIENT_DEV_TOOLS + SOKOL_GLCORE)\n"); }
	inline bool logOp(const Context&, const std::vector<std::string>&) { return false; }
	inline bool heightProbeMode() { return false; }
	inline void heightProbeToggle() {}
	inline bool heightProbeActive() { return false; }
	inline void heightProbeClear() {}
	inline void heightProbeSet(const Context&, double, double, bool) {}
	inline void heightProbeFrame(const Context&) {}
	inline std::vector<std::string> heightProbeLines() { return {}; }
	inline std::string heightProbeReport() { return {}; }
	inline void heightProbeDraw(const float[16], const double[3], float, float) {}
	inline bool heightProbeOp(const Context&, const std::vector<std::string>&) { return false; }
#endif
}
