#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/dev/playtest_state.hpp"

// The playtest store (docs/CLIENT_GOALS.md "Playtest"): playtests/<scenario>/versions.md (the scenario header and its versions,
// newest first) and playtests/<scenario>/v<N>/<yyyymmdd-hhmm>.md (one run per play). The client reads and writes these files; they
// are the project's record of how the game plays, and nothing else is a source of truth. Alongside the store: the run being played
// now. Its version is the newest in versions.md at Play; the file is created at Play and rewritten on the notes closing, on leaving
// and when the metrics arrive. The scenario wire (SIMLINK "SCENARIO": LIST_REQ / LIST, LOAD / STARTED, END / METRICS) goes out through
// Host and comes back in listed() / started() / ended(); with no live server the scenarios come from the store alone and Play starts
// the run without one.
class Playtest
{
public:
	// The client side of a run: what only main.cpp knows.
	struct Host
	{
		std::function<void(bool)> paused;	// typing(true): pause while the notes are typed; false: back to the state before
		std::function<std::string()> stamp;	// Mark: "HH:MM.S Name (x, z): " from the game time and the selected character
		// A recipe run's sim build (the session's): what the host knows of the running server when the wire has not said ("" unknown)
		std::function<std::string()> sessionSim;
		// The scenario wire, false with no live server session (then the store alone plays)
		std::function<bool()> live;
		// k16.2.127: one comment on a --bead run's item, off the frame thread (main.cpp's bd writer). done(ok, why) is called on a
		// later frame. Unset, or beadTarget() empty: nothing is posted and the run is the file's alone.
		std::function<void(const std::string& item, const std::string& text, std::function<void(bool, const std::string&)> done)> post;
		std::function<bool()> list;	// SCENARIO_LIST_REQ (102)
		std::function<bool(const std::string&, uint32_t)> load;	// SCENARIO_LOAD (103); seed 0 = the server picks
		std::function<void()> end;	// SCENARIO_END (104)
		std::function<void(const std::string&)> error;	// a request the wire never answered: the client's message (ui::message)
	};
	// One scenario on the server (SCENARIO_LIST 54): its file's header, merged with the store by name
	struct ServerScenario
	{
		std::string name, title, setup, lookFor, bead;	// bead: the file header's bead=<feature id> ("" older server or none)
		uint64_t fileHash = 0;
		std::vector<std::string> metrics;
	};
	// The answer to a load (SCENARIO_STARTED 55)
	struct Started
	{
		bool ok = false;
		std::string name, error, simHash, bead;	// bead: "" on a refusal
		uint64_t fileHash = 0;
		uint32_t seed = 0;
	};

	// dir empty: <repo>/playtests, found the way default_scene_file finds scenes.txt. clientHash: the running client's build.
	void init(const std::string& dir, Host host, const std::string& clientHash);
	bool loaded() const { return loaded_; }
	const std::string& dir() const { return dir_; }
	const std::string& status() const { return state_.status; }
	void reload();	// re-reads every versions.md and run file (the user and the coordinator edit them)

	// What the UI binds (ui/playtest_state.hpp): the state, and the actions of the two windows.
	const ui::ScenariosState& scenarios() const { return state_; }
	ui::ScenariosActions scenariosActions();
	const ui::NotesState& notes() const { return notes_; }
	// A comment of the run (a shot's, not a note) did not reach the --bead item: the Notes window's warning line says so (k16.2.130.1).
	void beadFailed(const std::string& why);
	ui::NotesActions notesActions();

	// What the actions and the `playtest` script ops call.
	void openScenarios(bool on);
	void select(const std::string& name);
	void play(const std::string& name);	// name empty: the selected one
	void openNotes(bool on);
	void note(const std::string& text);
	void verdict(const std::string& v);
	void save();				// rewrites the run file (notes close, leaving, metrics)
	void leave();				// the run is over: its file rewritten and END sent (another scenario, quit, NEW_GAME)
	// The wire's answers (main.cpp polls the decoder's serials and calls these): the list, a load's outcome, a run's metrics
	void listed(std::vector<ServerScenario> v);
	void started(const Started& s);
	void ended(const std::string& name, const std::vector<ui::PlaytestMetric>& m);
	bool running() const { return active_.on; }	// a run is being played (the HUD's NOTES button)
	bool isRecipe(const std::string& name) const;	// NAME is a store scenario with `run: recipe` (no .kscen: its run needs no world)
	bool recipeRun() const { return active_.on && active_.recipe; }	// a recipe's run keeps going through a NEW_GAME: the flow is the run
	bool typingNotes() const { return typing_; }	// the notes box has the keyboard
	bool awaitingMetrics() const { return endingOn_; }	// an END has been sent: its METRICS still comes (the quit waits for it)
	void metricsTimeout();	// the wait is over and nothing came: the run file says so
	void tick();	// the wall clock on a LOAD (103) or LIST_REQ (102) the server never answered
	std::string mark() const;		// the Mark stamp ("" when there is no run)
	bool typing(bool on);		// pauses while the notes are typed; true while paused here

	// The run's item (kenshi-k16.2.127, kclient --bead): its notes, the verdict and the F12 shots are also posted there as comments
	// (the run file keeps them too, unchanged). "" none: nothing is posted.
	void beadTarget(const std::string& id) { beadTarget_ = id; }
	const std::string& beadTarget() const { return beadTarget_; }
	std::string runName() const;	// "<scenario> <vN>" of the run being played ("" none)
	std::string runTag() const;		// "[<scenario> <vN> run <file stem>]" for a comment's first line ("" no run)
	std::string buildLine() const;	// "build: client <hash> sim <hash or ?>" (the run's sim, else the wire's)
	std::string recentNote() const;	// the note a shot cites: "3 (just added)" within 30 s, "4 (being typed)", else ""

	std::string dump() const;	// every scenario, version and run as plain lines (`playtest list`)

private:
	struct Version
	{
		std::string name, date, simHash, clientHash, scenarioHash, changed, answers;
		std::vector<ui::PlaytestRun> runs;	// newest first
	};
	struct Scenario
	{
		std::string name, title, setup, lookFor;
		std::string bead;	// the header's `bead:` (the feature item this playtest proves; "" none: then a run has no bead line)
		bool recipe = false;	// the header's `run: recipe`: a game-flow recipe, no scenario file, so Play sends no LOAD (k16.2.97)
		std::vector<std::string> metricNames;
		std::vector<Version> versions;	// newest first
	};
	struct Active	// a run: the one being played now, or the one an END was sent for
	{
		bool on = false;
		bool recipe = false;	// its scenario is a recipe: leaving sends no END
		std::string scenario, version, date, file;	// file: absolute
		std::string bead;	// stamped into the file: the started run's, else the store header's ("" -> no line)
		std::string simHash;	// the running sim's build stamped into the file: a recipe's session hash; "" -> the wire's member
		std::string scenarioHash;	// the loaded file's hash, hex (SCENARIO_STARTED); "" from the store alone
		uint64_t seed = 0;
		std::string verdict;
		std::vector<ui::PlaytestNote> notes;
		std::vector<ui::PlaytestMetric> metrics;
		std::string beadWarn;	// the Notes window's warning line: set once a comment for this run did not reach the --bead item (k16.2.130.1)
	};

	bool loadScenario(const std::string& name);
	void loadTriage(Active& a);		// the run file's answers -> its notes (a rewrite keeps what the coordinator wrote)
	void buildState();			// scenarios_ / serverList_ / Active -> the two UI states
	std::string writeRun(Active& a, bool write);	// a run as its file's text; write: create it
	std::string relFile(const Active& a) const;		// its file relative to dir_
	Active beginRun(const std::string& name, uint32_t seed);	// the store's newest version, the file name, the header
	void runStarted();	// the picker's PLAY (closeOnRun_) closes the scenarios window; any other play leaves it
	const Scenario* find(const std::string& name) const;
	const ServerScenario* serverFind(const std::string& name) const;
	std::string beadOf(const std::string& name) const;	// the server list's bead, else the store header's ("" none)

	std::string dir_;
	Host host_;
	std::string clientHash_;	// the running client's build (kclient_git_hash())
	std::string simHash_;		// the running sim's build (SCENARIO_STARTED; "" without the wire)
	bool loaded_ = false;
	std::vector<Scenario> scenarios_;
	std::vector<ServerScenario> serverList_;	// the last SCENARIO_LIST
	bool serverListed_ = false;
	ui::ScenariosState state_;
	ui::NotesState notes_;
	Active active_;
	Active ending_;			// the run an END was sent for: its METRICS still comes, and rewrites its file
	bool endingOn_ = false;
	bool closeOnRun_ = false;	// the picker's PLAY is in flight: the window closes when the run starts (not on a failed start)
	std::string loading_;		// the scenario a LOAD was sent for ("" none): STARTED starts its run
	std::chrono::steady_clock::time_point loadingAt_, listAt_;	// when each went out (tick's timeout)
	bool listPending_ = false;	// a LIST_REQ went out and no LIST answered it yet
	bool typing_ = false;
	std::string beadTarget_;	// --bead ID: the item the notes, verdict and shots are also posted to ("" none)
	std::chrono::steady_clock::time_point lastNoteAt_;	// when the last note was added (recentNote's 30 s)
	int lastNote_ = 0;			// its number (0: none yet)
	void posted(const std::string& file, const std::string& what, int note, bool ok, const std::string& why);	// a comment's answer (file: its run; note: its number, 0 none): the status line, the note's post line, the warning
};
