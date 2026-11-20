// The host side of the playtest and scenario tooling (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.3): the store, the run being
// played, the Notes / Scenarios windows, --play, --bead's bd writer and shot copies, the scenario wire's answers. main.cpp reaches it only
// through dev/devhooks.hpp; what it needs of the game it asks through the dev::Env main.cpp hands to init(). Built only with
// KCLIENT_DEV_TOOLS.
#include "dev/devhooks.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <thread>

#include "app/userdirs.hpp"
#include "dev/beadpost.hpp"
#include "dev/playtest.hpp"
#include "dev/scenehost.hpp"
#include "dev/viewhost.hpp"
#include "render/readback.hpp"
#include "ui/dev/notes.hpp"
#include "ui/dev/scenarios.hpp"
#include "ui/ui.hpp"

namespace dev
{
namespace
{

// A --bead run's shot waiting to be copied to <findings>/asks/<item id>/ (kenshi-k16.2.127): the png and its sidecars, a line in that
// folder's CAPTIONS.txt, and a comment on the item. It waits for the shot's .trees.txt (written when the answers are in, up to 5 s
// later); after 6 s whatever is there is copied. A second F12 while the first is still waiting keeps its own entry.
struct ShotBead
{
	std::string base;		// the shot's path without .png ("" nothing pending)
	double deadline = 0;	// steady seconds
	std::string tag;		// "[<scenario> <vN> run <stem>]", the run's at the shot
	std::string caption;	// "<scenario> <vN>, <game time>, <place>, <selected>"
	std::string comment;	// "game: ..., place: ..., selected: ..., note N (just added)" (the item comment's second line)
};

struct State
{
	Env env;
	bool ready = false;
	std::string playtests;	// --playtests DIR: the playtest store (default: <repo>/playtests, next to client/)
	std::string bead;	// --bead ID: a run's notes, verdict and F12 shots are also posted as comments on this beads item
	std::string findings;	// --findings DIR: where a --bead run's shots are copied (default: <home>/git/kenshi/kenshi-inspect/findings)
	std::string play;	// --play NAME: start NAME's recorded run once the link is up, as Scenarios -> PLAY does
	bool playtestFlag = false;	// --playtest: the SCENARIOS button on the title and pause menus without the rest of --debug
	Playtest playtest;	// the store and the run being played; the UI's two windows bind scenarios() / notes()
	BeadPost beadPost;	// the bd writer behind the run's notes, verdict and shot comments, off the frame thread
	bool wasPaused = false;	// the sim's pause bit before the notes were typed (typing(false) restores it)
	std::unique_ptr<ui::ScenariosWindow> scenariosWindow;
	std::unique_ptr<ui::NotesWindow> notesWindow;
	std::deque<ShotBead> shotBeads;
	int playStep = 0;	// --play: 0 not sent, 1 sent and waiting for SCENARIO_STARTED, 2 done
	uint32_t seenMetrics = 0, seenList = 0, seenStarted = 0;
};

State& st()
{
	static State s;
	return s;
}

// Default home of a --bead run's shots (kenshi-k16.2.127): $HOME/git/kenshi/kenshi-inspect/findings, the project's private findings tree
// (screenshots are game-derived and never go in a repo); --findings overrides it. Its asks/<item id>/ is where a run's shots land. The home
// root is plat::userHomeDir (%USERPROFILE% on Windows, kenshi-379.31.3).
std::string default_findings_dir()
{
	return (std::filesystem::path(plat::userHomeDir()) / "git/kenshi/kenshi-inspect/findings").string();
}

double bead_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// A --bead failure the user sees: the client's message line, and stderr for a headless run's log.
void bead_warn(const std::string& text)
{
	ui::message(text);
	fprintf(stderr, "kclient: %s\n", text.c_str());
}

// F12 in a --bead run: what the caption and the comment say is taken now; the files go when the tree capture is done (shotFrame).
void shot_bead_begin(const std::string& shotPath)
{
	State& s = st();
	if (s.bead.empty() || !s.playtest.running()) { return; }
	ShotBead& sb = s.shotBeads.emplace_back();
	sb.base = shotPath.ends_with(".png") ? shotPath.substr(0, shotPath.size() - 4) : shotPath;
	sb.deadline = bead_seconds() + 6;	// the trees' own wait is 5 s: copy what came, whatever it was
	sb.tag = "[playtest " + s.playtest.runTag().substr(1);	// the waker skips this prefix: a shot never wakes (k16.2.135)
	const Env::ShotInfo info = s.env.shotInfo();
	sb.caption = s.playtest.runName() + ", " + info.time + ", " + info.place + ", " + info.selected;
	const std::string note = s.playtest.recentNote();
	sb.comment = "game: " + info.time + ", place: " + info.place + ", selected: " + info.selected + (note.empty() ? "" : ", note " + note);
}

// Copies the shot's files, appends its CAPTIONS.txt line and posts the item comment. A file that is not there (an .events.txt the log
// could not write, an .trees.txt of a run with no debug view) is skipped; a png that will not copy is a failure: no caption, no comment,
// a warning.
void shot_bead_finish(const ShotBead& sb)
{
	State& s = st();
	if (sb.base.empty()) { return; }
	readback::flushWrites();	// the PNG is the screenshot writer's until then
	const std::filesystem::path dir = std::filesystem::path(s.findings) / "asks" / s.bead;
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec) { bead_warn("bead: cannot make " + dir.string() + ": " + ec.message()); return; }
	const std::string png = std::filesystem::path(sb.base).filename().string();
	bool copied = false;
	for (const char* ext : { ".png", ".txt", ".trees.txt", ".events.txt", ".route.tsv" })	// .route.tsv: the selected characters' routes (kenshi-k16.2.231.1); .trails.tsv is left out (it can be large)
	{
		const std::string src = sb.base + ext;
		if (!std::filesystem::exists(src, ec)) { continue; }
		std::filesystem::copy_file(src, dir / (png + ext), std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) { bead_warn("bead: cannot copy " + std::string(src) + " to " + dir.string() + ": " + ec.message()); }
		else if (!std::strcmp(ext, ".png")) { copied = true; }
	}
	if (!copied) { bead_warn("bead: " + sb.base + ".png is not there: the shot is not copied for " + s.bead); return; }
	const std::string caption = png + ": " + sb.caption + "\n";
	FILE* f = fopen((dir / "CAPTIONS.txt").c_str(), "a");
	if (f) { fputs(caption.c_str(), f); fclose(f); printf("bead: %s", caption.c_str()); }
	else { bead_warn("bead: cannot append to " + (dir / "CAPTIONS.txt").string()); }
	const std::string text = sb.tag + " shot: " + (dir / png).string() + "\n" + sb.comment + "\n" + s.playtest.buildLine();
	s.beadPost.post(s.bead, text, [](bool ok, const std::string& why) { if (!ok) { bead_warn("bead: the shot's comment is not on the item: " + why); st().playtest.beadFailed(why); } });
}

// SCENARIO_METRICS (SIMLINK "SCENARIO" 56): the metrics of the run an END was sent for, or of the active one, to the store. Its own function so a
// quit can pump the link for it with no frame drawn (cleanup).
void scenario_metrics()
{
	State& s = st();
	const simlink::SimLinkDecoder& d = s.env.decoder();
	if (d.scenarioMetricsSerial == s.seenMetrics) { return; }
	s.seenMetrics = d.scenarioMetricsSerial;
	const auto& m = d.scenarioMetrics;
	std::vector<ui::PlaytestMetric> v;
	for (const simlink::SimLinkDecoder::ScenarioMetric& x : m.metrics) { v.push_back({ x.name, x.value, x.valid }); }
	s.playtest.ended(m.name, v);
}

}	// namespace

bool parseArg(int argc, char** argv, int& i, std::vector<std::string>& windowOff)
{
	State& s = st();
	const std::string a = argv[i];
	const bool more = i + 1 < argc;
	if (a == "--playtest") { s.playtestFlag = true; return true; }
	if (a == "--play" && more) { s.play = argv[++i]; return true; }
	if (a == "--bead" && more) { s.bead = argv[++i]; return true; }
	if (a == "--findings" && more) { s.findings = argv[++i]; return true; }
	if (a == "--playtests" && more) { s.playtests = argv[++i]; return true; }
	if (sceneArg(argc, argv, i, windowOff)) { return true; }	// --scene-test --scenes --scene-file --scene-hud --scene-frames (dev/scenetest.cpp)
	if (viewArg(argc, argv, i, windowOff)) { return true; }	// --view-mesh --tex --dist (dev/viewmesh.cpp)
	return sgOwnerArg(a);
}

void usage()
{
	fprintf(stderr, "               [--playtests DIR] (the playtest store; default <repo>/playtests) [--playtest] (SCENARIOS without --debug)\n"
		"               [--play NAME] (start NAME's recorded playtest run once connected, as Scenarios -> PLAY)\n"
		"               [--bead ID] (a --play run's notes, verdict and F12 shots are also comments on that beads item)\n"
		"               [--findings DIR] (where a --bead run's shots are copied; default <home>/git/kenshi/kenshi-inspect/findings)\n"
		"               [--sg-owner-probe] (an sg_* call from a second thread right after sg_setup: the thread-owner assert must abort)\n"
		"               [--sg-owner-logic] (logic_frame owns no sokol: an sg_* call from the logic half aborts, as on the logic thread to come)\n"
		"               [--scene-test OUTDIR [--scenes NAME,NAME] [--scene-file FILE]] [--scene-hud] [--scene-frames N]\n"
		"               [--view-mesh NAME_OR_PATH [--tex TEXTURE.dds] [--dist D]]\n");
}

// The playtest store (CLIENT_GOALS "Playtest"): read now, so the script ops and a window see it; the host supplies what only the client
// knows: the sim pause while the notes are typed, the Mark stamp, the scenario wire.
void init(const Env& env)
{
	State& s = st();
	s.env = env;
	Playtest::Host host;
	host.paused = [](bool on)
	{
		State& t = st();
		if (!t.env.simActive()) { return; }
		if (on) { t.wasPaused = t.env.paused(); t.env.pause(true); }
		else { t.env.pause(t.wasPaused); }
	};
	// The scenario wire (SIMLINK "SCENARIO"): a live session swaps its world and answers; without one a play is a store-only run
	host.live = [] { return st().env.live(); };
	host.list = [] { return st().env.scenarioList(); };
	host.load = [](const std::string& name, uint32_t seed) { return st().env.scenarioLoad(name, seed); };
	host.end = [] { st().env.scenarioEnd(); };
	// A LOAD / LIST_REQ the wire never answered (Playtest::tick): the error as the client's message, and the load screen a scenario
	// load put up before the answer that never came is left (an older server ignores 102 / 103 silently).
	host.error = [](const std::string& text) { st().env.error(text); };
	host.stamp = [] { return st().env.stamp(); };
	// A recipe run's sim build (k16.2.97): the session's. A joined server's build only arrives with SCENARIO_STARTED (the store keeps
	// that); the client's own build stands in for the local server it started, which is built from this same checkout.
	host.sessionSim = [] { return st().env.sessionSim(); };
	// k16.2.127: a --bead run's notes and verdict as comments on the item, through the bd writer's own thread (a frame never waits on bd)
	host.post = [](const std::string& item, const std::string& text, std::function<void(bool, const std::string&)> done)
	{
		st().beadPost.post(item, text, std::move(done));
	};
	if (s.playtests.empty()) { s.playtests = (std::filesystem::path(env.repoRoot) / "playtests").string(); }
	s.playtest.init(s.playtests, std::move(host), env.clientHash);
	if (!s.bead.empty())	// the item the run posts to; the bd children run in the kenshi repo the playtests live in
	{
		if (s.findings.empty()) { s.findings = default_findings_dir(); }
		s.playtest.beadTarget(s.bead);
		s.beadPost.start(env.repoRoot);
		printf("playtest: --bead %s (comments through bd -C %s; shots to %s/asks/%s/)\n", s.bead.c_str(), env.repoRoot.c_str(), s.findings.c_str(), s.bead.c_str());
	}
	s.ready = true;
}

// The playtest scenarios: the button shows only with --debug or --playtest (k16.2.110); a --bead run is pinned to its playtest: no button
bool scenariosButton(bool debug) { return (debug || st().playtestFlag) && st().bead.empty(); }

void openScenarios() { st().playtest.openScenarios(true); }

// The playtest windows (ui/dev/scenarios.hpp, ui/dev/notes.hpp): the store's state every frame; their close() hides them at once.
void createWindows()
{
	State& s = st();
	s.scenariosWindow = std::make_unique<ui::ScenariosWindow>(s.playtest.scenariosActions());
	s.notesWindow = std::make_unique<ui::NotesWindow>(s.playtest.notesActions());
}

// The store's state every frame (cheap when unchanged); the windows call its actions back.
void updateUi()
{
	State& s = st();
	if (s.scenariosWindow) { s.scenariosWindow->update(s.playtest.scenarios()); }
	if (s.notesWindow) { s.notesWindow->update(s.playtest.notes()); }
}

void hudState(bool& playtestRun, bool& notesOpen)
{
	State& s = st();
	playtestRun = s.playtest.running();
	notesOpen = s.playtest.notes().open;
}

// Ctrl+N and the HUD's NOTES button. Closed: open it with the caret in the box; open: the caret; typing: close it (and save). The
// button only toggles (a click on it already moved the keyboard off the box). focusEntry needs the store's open in the same frame.
void notesKey(bool button)
{
	State& s = st();
	if (!s.notesWindow) { return; }
	const bool open = s.playtest.notes().open;
	if (open && (button || s.playtest.typingNotes())) { s.playtest.openNotes(false); return; }
	if (!open) { s.playtest.openNotes(true); }
	if (s.playtest.notes().open) { s.notesWindow->focusEntry(); }
}

bool restMetric()
{
	const std::string line = st().env.restLine ? st().env.restLine() : std::string();
	if (line.empty()) { return false; }
	printf("%s\n", line.c_str());
	return true;
}

// playtest list | select NAME | play [NAME] | note TEXT | verdict good|off|broken | mark | save | end (CLIENT_GOALS "Playtest")
bool scriptOp(const std::vector<std::string>& a)
{
	State& s = st();
	if (a.empty()) { return false; }
	std::string rest;
	for (size_t i = 1; i < a.size(); i++) { rest += (i > 1 ? " " : "") + a[i]; }	// scenario names and note text hold spaces
	if (a[0] == "list" && a.size() == 1)	// the files as written, and with a live server its list too (SCENARIO_LIST_REQ)
	{
		s.playtest.reload();
		if (s.env.live()) { s.env.scenarioList(); }
		printf("%s", s.playtest.dump().c_str());
	}
	else if (a[0] == "select" && a.size() >= 2) { s.playtest.select(rest); }
	else if (a[0] == "play" && a.size() <= 2) { s.playtest.play(a.size() == 2 ? rest : ""); }
	else if (a[0] == "note" && a.size() >= 2)
	{
		// a script's quotes are the script's (`playtest note "text"`): one surrounding pair comes off here, the parser keeps quotes for the rest
		std::string text = rest;
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"') { text = text.substr(1, text.size() - 2); }
		s.playtest.note(text);
	}
	else if (a[0] == "verdict" && a.size() == 2) { s.playtest.verdict(a[1]); }
	else if (a[0] == "mark" && a.size() == 1) { printf("playtest: mark %s\n", s.playtest.mark().c_str()); }
	else if (a[0] == "save" && a.size() == 1) { s.playtest.save(); }
	else if (a[0] == "end" && a.size() == 1) { s.playtest.leave(); }	// the run is over: its file rewritten, END sent (its metrics still answers)
	else { return false; }
	return true;
}

std::string buildLine() { return st().playtest.buildLine(); }

void shotBegin(const std::string& shotPath) { shot_bead_begin(shotPath); }

// Every frame after a shot: a pending --bead copy goes when its .trees.txt is written (the capture's answers are in, or its 5 s are up;
// a newer shot's capture means the older one's is done). Copies go in the order the shots were taken.
void shotFrame()
{
	State& s = st();
	while (!s.shotBeads.empty())
	{
		const ShotBead& sb = s.shotBeads.front();
		const bool waiting = bead_seconds() < sb.deadline && s.env.treesPending(sb.base);	// the trees' capture is 5 s at most: the deadline is its backstop
		if (waiting) { return; }
		shot_bead_finish(sb);
		s.shotBeads.pop_front();
	}
}

// A playtest run ends with a NEW_GAME (END: its metrics still answers, and its file is rewritten); a recipe's run does not: the new game
// is the flow it records (k16.2.97), and its file is written when it is left.
void newGameLeave()
{
	if (!st().playtest.recipeRun()) { st().playtest.leave(); }
}

void leaveRun() { st().playtest.leave(); }

void poll()
{
	State& s = st();
	if (!s.ready) { return; }
	const simlink::SimLinkDecoder& d = s.env.decoder();
	// SCENARIO (SIMLINK "SCENARIO", the playtest's wire): the server's list, a load's answer and a run's metrics, to the playtest store
	if (d.scenarioListSerial != s.seenList)
	{
		s.seenList = d.scenarioListSerial;
		std::vector<Playtest::ServerScenario> v;
		for (const simlink::SimLinkDecoder::ScenarioInfo& si : d.scenarioList)
		{
			Playtest::ServerScenario ps;
			ps.name = si.name; ps.title = si.title; ps.setup = si.setup; ps.lookFor = si.lookFor; ps.bead = si.bead; ps.fileHash = si.fileHash; ps.metrics = si.metrics;
			v.push_back(std::move(ps));
		}
		s.playtest.listed(std::move(v));
	}
	// --play NAME (kenshi-k16.2.126): a launcher's fixture loaded at server start sends no SCENARIO_STARTED, so no run would begin (no NOTES, no run
	// file). The client asks for the run itself, the menu's PLAY path; a launch that can't record one says why on screen.
	// A --bead run is pinned to its playtest (kenshi-k16.2.136.1): no SCENARIOS button, that scenario's UI (the Scenarios window with its
	// look_for, the Notes dock) is up from the start. A recipe journey has no world to wait for: its run starts at the title screen.
	const bool pinnedRecipe = !s.bead.empty() && s.playtest.isRecipe(s.play);
	if (s.playStep == 0 && pinnedRecipe)
	{
		s.playtest.openScenarios(true);
		s.playtest.play(s.play);
		s.playStep = 2;
		if (!s.playtest.running()) { ui::message("Not a recorded run: " + s.playtest.scenarios().status); }
	}
	if (s.playStep == 0 && !s.play.empty() && s.env.live() && s.env.worldOn())	// the world is on screen: a refusal is seen
	{
		if (!s.bead.empty()) { s.playtest.openScenarios(true); }	// pinned: the scenario's window stays up over the run (the picker's PLAY closes it, k16.2.155.1)
		s.playtest.play(s.play);
		s.playStep = s.playtest.running() ? 2 : 1;
		const std::string& why = s.playtest.scenarios().status;
		if (s.playStep == 1 && why.rfind("loading ", 0) != 0)
		{
			// "no scenario": the store has no playtests/NAME/versions.md (the server's list isn't asked for yet)
			ui::message("Not a recorded run: " + (why.rfind("no scenario", 0) == 0 ? "no playtests/" + s.play + "/versions.md (the coordinator adds v1)" : why));
			s.playStep = 2;
		}
	}
	if (d.scenarioStartedSerial != s.seenStarted)
	{
		s.seenStarted = d.scenarioStartedSerial;
		const auto& sc = d.scenarioStarted;
		Playtest::Started started;
		started.ok = sc.status == 0;	// 0 ok; else the reason in error
		started.name = sc.name; started.error = sc.error; started.simHash = sc.simHash; started.bead = sc.bead; started.fileHash = sc.fileHash; started.seed = sc.seed;
		s.playtest.started(started);
		if (s.playStep == 1)
		{
			s.playStep = 2;
			if (!s.playtest.running()) { ui::message("Not a recorded run: " + s.playtest.scenarios().status); }
		}
		// a started run is a session to watch, as a load or a new game: the game's response (the world chosen, the menus closed, the camera on the
		// scenario's centre) is main.cpp's
		s.env.started(started.ok, std::isfinite(sc.centreX) && std::isfinite(sc.centreZ), sc.centreX, sc.centreZ);
	}
	s.playtest.tick();	// a LOAD or LIST_REQ the server never answered: its timeout (an older server ignores them silently)
	s.beadPost.pump();	// the bd writer's answers: the note / verdict comments that landed, or why not (k16.2.127)
	scenario_metrics();
}

void cleanup()
{
	State& s = st();
	if (s.ready)
	{
		s.playtest.leave();	// leaving: the run being played is rewritten and its END sent (no-op with none)
		s.beadPost.stop();	// the --bead comments already queued for bd go first; the rest are dropped (the run file has the notes, k16.2.127)
		// The run an END was just sent for: its METRICS is on the way. Keep only the link moving for it, no frame drawn, at most 1.5 s; then the
		// run's file is written anyway, saying the answer did not come.
		if (s.playtest.awaitingMetrics())
		{
			const auto start = std::chrono::steady_clock::now();
			while (s.playtest.awaitingMetrics() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500))
			{
				if (!s.env.pumpLink()) { break; }
				scenario_metrics();
				if (s.playtest.awaitingMetrics()) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
			}
			s.playtest.metricsTimeout();
		}
	}
	s.scenariosWindow.reset();
	s.notesWindow.reset();
}

// The animation lab's own copy (dev/debughost.cpp) asks whether the script is a headless lab run: the answer is main.cpp's (Env::labScripted).
bool labScripted()
{
	State& s = st();
	return s.env.labScripted && s.env.labScripted();
}

}	// namespace dev
