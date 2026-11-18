#include "playtest.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
	std::string trim(const std::string& s)
	{
		const size_t a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) { return {}; }
		const size_t b = s.find_last_not_of(" \t\r\n");
		return s.substr(a, b - a + 1);
	}

	std::string lower(std::string s)
	{
		for (char& c : s) { c = (char)std::tolower((unsigned char)c); }
		return s;
	}

	// "key: value" -> the key lowercased, the value trimmed; false when the line is not a field.
	bool field(const std::string& line, std::string& key, std::string& value)
	{
		const size_t colon = line.find(':');
		if (colon == std::string::npos) { return false; }
		key = lower(trim(line.substr(0, colon)));
		value = trim(line.substr(colon + 1));
		return !key.empty();
	}

	// "a, b, c" -> a, b, c (trimmed, empty items dropped).
	std::vector<std::string> list(const std::string& text, char sep = ',')
	{
		std::vector<std::string> out;
		std::istringstream ss(text);
		for (std::string w; std::getline(ss, w, sep);) { if (!trim(w).empty()) { out.push_back(trim(w)); } }
		return out;
	}

	// The header's metrics: "name=value, name" (a name with no value yet: valid false).
	std::vector<ui::PlaytestMetric> metrics(const std::string& text)
	{
		std::vector<ui::PlaytestMetric> out;
		for (const std::string& item : list(text))
		{
			ui::PlaytestMetric m;
			const size_t eq = item.find('=');
			m.name = trim(item.substr(0, eq));
			if (eq != std::string::npos)
			{
				const std::string v = trim(item.substr(eq + 1));
				char* end = nullptr;
				m.value = std::strtod(v.c_str(), &end);
				m.valid = !v.empty() && end && *end == 0;
			}
			out.push_back(m);
		}
		return out;
	}

	// A run file, as read: the file's notes carry the coordinator's triage answers.
	struct ParsedRun
	{
		ui::PlaytestRun run;
		std::vector<ui::PlaytestNote> notes;
	};

	// The run file format is written by Playtest::writeRun and documented in docs/CLIENT_GOALS.md "Playtest".
	ParsedRun parseRun(const std::string& path, const std::string& rel)
	{
		ParsedRun out;
		out.run.file = rel;
		std::ifstream in(path);
		std::string line;
		int section = 0;	// 0 header, 1 notes, 2 triage
		while (std::getline(in, line))
		{
			line = trim(line);
			if (line.rfind("## ", 0) == 0)
			{
				const std::string h = lower(line.substr(3));
				section = h.rfind("note", 0) == 0 ? 1 : h.rfind("triage", 0) == 0 ? 2 : 0;
				continue;
			}
			if (line.empty() || line[0] == '#') { continue; }
			if (section == 0)
			{
				std::string k, v;
				if (!field(line, k, v)) { continue; }
				if (k == "date") { out.run.date = v; }
				else if (k == "sim") { out.run.simHash = v == "?" || lower(v) == "unknown" ? "" : v; }
				else if (k == "client") { out.run.clientHash = v == "?" || lower(v) == "unknown" ? "" : v; }
				else if (k == "seed") { out.run.seed = std::strtoull(v.c_str(), nullptr, 10); }
				else if (k == "metrics") { out.run.metrics = metrics(v); }
				else if (k == "verdict") { out.run.verdict = v; }
				continue;
			}
			const size_t dot = line.find('.');
			if (dot == std::string::npos || dot == 0 || dot > 9) { continue; }
			const int n = std::atoi(line.substr(0, dot).c_str());
			const std::string text = trim(line.substr(dot + 1));
			if (n <= 0) { continue; }
			if (section == 1)
			{
				ui::PlaytestNote note;
				note.n = n;
				note.text = text;
				out.notes.push_back(note);
				if (n == 1) { out.run.firstNote = text; }
			}
			else
			{
				for (ui::PlaytestNote& note : out.notes) { if (note.n == n) { note.triage = text; } }
			}
		}
		return out;
	}

	// The run files' names: "yyyymmdd-hhmm.md" (a minute is the unit; a second play in the same minute moves on a minute).
	std::string fileStamp(std::time_t t)
	{
		std::tm tm = {};
		localtime_r(&t, &tm);
		char buf[32];
		std::strftime(buf, sizeof buf, "%Y%m%d-%H%M", &tm);
		return buf;
	}

	std::string dateStamp(std::time_t t)
	{
		std::tm tm = {};
		localtime_r(&t, &tm);
		char buf[32];
		std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
		return buf;
	}

	bool exists(const std::string& path)
	{
		std::error_code ec;
		return std::filesystem::exists(path, ec);
	}

	// A run file's place in time: the stamp in its name ("20261107-1530").
	std::string stampOf(const std::string& file)
	{
		const size_t slash = file.find_last_of('/');
		const std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
		return name.size() > 3 && name.compare(name.size() - 3, 3, ".md") == 0 ? name.substr(0, name.size() - 3) : name;
	}

	// A metric value as the file writes it: 2, 3.4, 0.25 (no trailing zeros).
	std::string number(double v)
	{
		char buf[32];
		std::snprintf(buf, sizeof buf, "%g", v);
		return buf;
	}

	// "a=2, b=?" (valid false: the run never produced it), as CLIENT_GOALS "Playtest" writes the metrics line.
	std::string metricLine(const std::vector<ui::PlaytestMetric>& m)
	{
		std::string out;
		for (const ui::PlaytestMetric& x : m) { out += (out.empty() ? "" : ", ") + x.name + "=" + (x.valid ? number(x.value) : std::string("?")); }
		return out;
	}

	// SCENARIO_STARTED's fileHash as versions.md writes the scenario hash: 16 hex digits.
	std::string hex64(uint64_t v)
	{
		char buf[32];
		std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
		return buf;
	}

	// A server that does not know the SCENARIO wire (built before e7b46583) ignores 102 / 103 silently: nothing would ever come back.
	// The slowest real scenario load measured (2026-11-08, the five scenarios, a current server) was 0.35 s; x3 is ~1 s, so the 15 s
	// floor stands: a server that has not answered by then is not loading this scenario.
	constexpr double kAnswerSeconds = 15;
	constexpr const char* kNoAnswer = "The server did not answer the scenario request (older server build?)";
}

void Playtest::init(const std::string& dir, Host host, const std::string& clientHash)
{
	dir_ = dir;
	host_ = std::move(host);
	clientHash_ = clientHash;
	reload();
}

void Playtest::reload()
{
	scenarios_.clear();
	loaded_ = false;
	std::error_code ec;
	if (dir_.empty() || !std::filesystem::is_directory(dir_, ec))
	{
		state_.status = "no playtests directory: " + dir_;
		buildState();
		return;
	}
	std::vector<std::string> names;
	for (const auto& e : std::filesystem::directory_iterator(dir_, ec))
	{
		if (e.is_directory() && exists(e.path() / "versions.md")) { names.push_back(e.path().filename().string()); }
	}
	std::sort(names.begin(), names.end());
	for (const std::string& n : names) { loadScenario(n); }
	loaded_ = true;
	state_.status = std::to_string(scenarios_.size()) + " scenario" + (scenarios_.size() == 1 ? "" : "s") + " in " + dir_;
	buildState();
}

// playtests/<name>/versions.md: a header (title, setup, look_for, metrics) and one "## v<N> <date>" entry per version, newest
// first, each with sim / client / scenario hashes, what changed and the notes it answers. Then every run file of every version.
bool Playtest::loadScenario(const std::string& name)
{
	const std::string base = dir_ + "/" + name;
	std::ifstream in(base + "/versions.md");
	if (!in) { return false; }
	Scenario s;
	s.name = name;
	Version* v = nullptr;
	std::string line;
	while (std::getline(in, line))
	{
		line = trim(line);
		if (line.rfind("## ", 0) == 0)
		{
			std::istringstream hs(line.substr(3));
			Version nv;
			hs >> nv.name;	// "v2"
			std::string rest;
			std::getline(hs, rest);
			nv.date = trim(rest);
			s.versions.push_back(nv);
			v = &s.versions.back();
			continue;
		}
		if (line.empty() || line[0] == '#') { continue; }
		std::string k, val;
		if (!field(line, k, val)) { continue; }
		if (!v)
		{
			if (k == "title") { s.title = val; }
			else if (k == "setup") { s.setup = val; }
			else if (k == "look_for") { s.lookFor = val; }
			else if (k == "bead") { s.bead = val; }	// the beads feature item this playtest proves (h2v.8): a run of it records `bead:`
			else if (k == "metrics") { s.metricNames = list(val); }
			else if (k == "run") { s.recipe = lower(val) == "recipe"; }	// `run: recipe`: a game flow, no .kscen (CLIENT_GOALS "Playtest")
			continue;
		}
		if (k == "sim") { v->simHash = val == "?" ? "" : val; }
		else if (k == "client") { v->clientHash = val == "?" ? "" : val; }
		else if (k == "scenario") { v->scenarioHash = val == "?" ? "" : val; }
		else if (k == "changed") { v->changed = val; }
		else if (k == "answers") { v->answers = val; }
	}
	for (Version& ver : s.versions)
	{
		std::vector<std::string> files;
		const std::string vdir = base + "/" + ver.name;
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(vdir, ec))
		{
			if (e.is_regular_file() && e.path().extension() == ".md") { files.push_back(e.path().filename().string()); }
		}
		std::sort(files.begin(), files.end());
		for (const std::string& f : files)
		{
			const ui::PlaytestRun r = parseRun(vdir + "/" + f, name + "/" + ver.name + "/" + f).run;
			ver.runs.push_back(r);
		}
		std::reverse(ver.runs.begin(), ver.runs.end());	// newest first (the stamp sorts)
		for (ui::PlaytestRun& r : ver.runs)	// a run on a build that differs from its version's is still recorded, and flagged
		{
			r.mismatch = (!r.clientHash.empty() && !ver.clientHash.empty() && r.clientHash != ver.clientHash) ||
				(!r.simHash.empty() && !ver.simHash.empty() && r.simHash != ver.simHash);
		}
	}
	scenarios_.push_back(std::move(s));
	return true;
}

void Playtest::buildState()
{
	state_.scenarios.clear();
	for (const Scenario& s : scenarios_)
	{
		ui::PlaytestScenario ps;
		ps.name = s.name;
		ps.title = s.title;
		ps.setup = s.setup;
		ps.lookFor = s.lookFor;
		// The window's own fields (k16.3.14): a non-empty recipe is the setup's steps (the list's Setup reads "Recipe", the window's
		// RECIPE block shows it), and the bead is the feature item the playtest proves (a Bead line under the title).
		ps.recipe = s.recipe ? s.setup : std::string();
		ps.bead = beadOf(s.name);
		std::string lastFile;
		for (const Version& v : s.versions)
		{
			ui::PlaytestVersion pv;
			pv.name = v.name;
			pv.date = v.date;
			pv.changed = v.changed;
			pv.answers = v.answers;
			pv.runs = v.runs;
			ps.versions.push_back(pv);
			for (const ui::PlaytestRun& r : v.runs)
			{
				if (lastFile.empty() || stampOf(r.file) > stampOf(lastFile)) { lastFile = r.file; ps.lastDate = r.date; ps.lastVerdict = r.verdict; }
			}
		}
		state_.scenarios.push_back(std::move(ps));
	}
	for (const ServerScenario& s : serverList_)	// a scenario the server has and the store does not: listed, with no runs
	{
		if (find(s.name) != nullptr) { continue; }
		ui::PlaytestScenario ps;
		ps.name = s.name;
		ps.title = s.title;
		ps.setup = s.setup;
		ps.lookFor = s.lookFor;
		ps.bead = s.bead;	// the .kscen header's bead=, the wire's SCENARIO_LIST tail (no store header to fall back on)
		state_.scenarios.push_back(std::move(ps));
	}
	std::sort(state_.scenarios.begin(), state_.scenarios.end(), [](const ui::PlaytestScenario& a, const ui::PlaytestScenario& b) { return a.name < b.name; });
	notes_.scenario = active_.scenario;
	notes_.version = active_.version;
	notes_.verdict = active_.verdict;
	notes_.notes = active_.notes;
	notes_.warning = active_.beadWarn;
}

const Playtest::Scenario* Playtest::find(const std::string& name) const
{
	for (const Scenario& s : scenarios_) { if (s.name == name) { return &s; } }
	return nullptr;
}

const Playtest::ServerScenario* Playtest::serverFind(const std::string& name) const
{
	for (const ServerScenario& s : serverList_) { if (s.name == name) { return &s; } }
	return nullptr;
}

// The bead a scenario's list row and the run of it show: the server's (the loaded file's header) when it lists the scenario, else the
// store's versions.md header. "" when neither has one.
std::string Playtest::beadOf(const std::string& name) const
{
	if (const ServerScenario* ss = serverFind(name); ss != nullptr && !ss->bead.empty()) { return ss->bead; }
	const Scenario* s = find(name);
	return s != nullptr ? s->bead : std::string();
}

ui::ScenariosActions Playtest::scenariosActions()
{
	ui::ScenariosActions a;
	a.select = [this](const std::string& n) { select(n); };
	a.play = [this](const std::string& n)	// the picker's PLAY closes the window once the run has started (a server load: at STARTED), not on a failed start
	{
		closeOnRun_ = true;
		play(n);
		if (loading_.empty()) { closeOnRun_ = false; }	// started, or failed: no load is pending
	};
	a.close = [this] { openScenarios(false); };
	return a;
}

ui::NotesActions Playtest::notesActions()
{
	ui::NotesActions a;
	a.add = [this](const std::string& t) { note(t); };
	a.verdict = [this](const std::string& v) { verdict(v); };
	a.mark = [this] { return mark(); };
	a.typing = [this](bool on) { typing(on); };
	a.close = [this] { openNotes(false); };
	return a;
}

void Playtest::openScenarios(bool on)
{
	if (on)	// the user and the coordinator edit the files while the client runs; the server's list is the wire's half
	{
		reload();
		if (host_.live && host_.live() && host_.list)
		{
			if (host_.list()) { listPending_ = true; listAt_ = std::chrono::steady_clock::now(); }
			else { state_.status = "the scenario list did not go out"; }
		}
	}
	state_.open = on;
}

void Playtest::select(const std::string& name)
{
	if (const Scenario* s = find(name))
	{
		state_.selected = s->name;
		state_.status = s->title.empty() ? s->name : s->title;	// a recipe's setup is the window's RECIPE block (buildState)
		if (!s->recipe && serverListed_ && serverFind(name) == nullptr) { state_.status += " (not on the server)"; }
		printf("playtest: selected %s: %s\n", s->name.c_str(), s->lookFor.c_str());
		return;
	}
	if (const ServerScenario* s = serverFind(name))	// on the server, no playtests/<name>/versions.md: listed, no run can be recorded
	{
		state_.selected = s->name;
		state_.status = (s->title.empty() ? s->name : s->title) + " (no version in the store)";
		printf("playtest: selected %s: %s (server only)\n", s->name.c_str(), s->lookFor.c_str());
		return;
	}
	state_.status = "no scenario " + name;
	printf("playtest: no scenario %s\n", name.c_str());
}

// The store's newest version is the run's; the file is named now and starts empty. Called when the run starts (from STARTED on the
// wire: its seed and hashes are the server's).
Playtest::Active Playtest::beginRun(const std::string& name, uint32_t seed)
{
	const Scenario* s = find(name);
	Active a;
	a.on = true;
	a.recipe = s->recipe;
	a.bead = s->bead;	// the store's header: a wire run's STARTED overrides it with the loaded file's
	a.scenario = name;
	a.version = s->versions.front().name;
	a.date = dateStamp(std::time(nullptr));
	a.seed = seed;
	const std::string vdir = dir_ + "/" + name + "/" + a.version;
	std::time_t t = std::time(nullptr);
	while (exists(vdir + "/" + fileStamp(t) + ".md")) { t += 60; }	// both plays in one minute keep the name format
	a.file = vdir + "/" + fileStamp(t) + ".md";
	return a;
}

bool Playtest::isRecipe(const std::string& name) const
{
	const Scenario* s = find(name);
	return s != nullptr && s->recipe;
}

void Playtest::play(const std::string& name)
{
	const std::string want = name.empty() ? state_.selected : name;
	const Scenario* s = find(want);
	const bool serverOnly = s == nullptr && serverFind(want) != nullptr;
	if (!s && !serverOnly) { state_.status = "no scenario " + want; printf("playtest: no scenario %s\n", want.c_str()); return; }
	if (serverOnly || s->versions.empty())	// no run record: the server's scenario has nobody's verdict to write one against
	{
		state_.status = "no playtests/" + want + "/versions.md: the coordinator adds v1";
		printf("playtest: no playtests/%s/versions.md: the coordinator adds v1\n", want.c_str());
		return;
	}
	if (active_.on) { leave(); }	// the previous run: rewritten, and its END sent (a recipe's not)
	// A recipe is the session's own game flow (no .kscen to load): the run records the session as it runs, so nothing goes to the server
	if (!s->recipe && host_.live && host_.live() && host_.load)	// the server swaps its world; STARTED starts the run with its seed and hashes
	{
		if (!host_.load(want, 0)) { state_.status = want + ": the load did not go out"; return; }
		loading_ = want;
		loadingAt_ = std::chrono::steady_clock::now();	// tick: a server that ignores 103 never answers
		state_.status = "loading " + want + "...";
		printf("playtest: load %s, waiting for the server\n", want.c_str());
		buildState();
		return;
	}
	active_ = beginRun(want, 0);
	if (s->recipe)	// seed 0, and the header's sim is the running session's build: the wire's if this session loaded a scenario, else the host's
	{
		active_.simHash = !simHash_.empty() ? simHash_ : (host_.sessionSim ? host_.sessionSim() : std::string());
	}
	state_.selected = s->name;
	writeRun(active_, true);
	if (s->recipe)	// the notes dock at the side for the run, as a wire run's start does
	{
		notes_.open = true;
		printf("playtest: play %s (recipe: no load, sim %s)\n", s->name.c_str(), active_.simHash.empty() ? "?" : active_.simHash.c_str());
	}
	else { printf("playtest: play %s (no live server)\n", s->name.c_str()); }
	printf("playtest: run %s (%s)\n", relFile(active_).c_str(), active_.version.c_str());
	state_.status = "playing " + active_.scenario + " " + active_.version;
	runStarted();
	buildState();
}

void Playtest::runStarted()
{
	if (closeOnRun_) { openScenarios(false); }
	closeOnRun_ = false;
}

void Playtest::listed(std::vector<ServerScenario> v)
{
	listPending_ = false;
	serverList_ = std::move(v);
	serverListed_ = true;
	buildState();
	state_.status = std::to_string(serverList_.size()) + " scenario" + (serverList_.size() == 1 ? "" : "s") + " on the server";
	printf("playtest: server lists %zu:", serverList_.size());
	for (const ServerScenario& s : serverList_) { printf(" %s", s.name.c_str()); }
	printf("\n");
}

void Playtest::started(const Started& s)
{
	loading_.clear();
	const bool fromPicker = closeOnRun_;
	closeOnRun_ = false;
	if (!s.ok)	// a refusal is an answer: the reason in the window, no run file
	{
		state_.status = (s.name.empty() ? std::string("the scenario") : s.name) + ": " + s.error;
		printf("playtest: %s refused: %s\n", s.name.c_str(), s.error.c_str());
		buildState();
		return;
	}
	simHash_ = s.simHash;
	const Scenario* sc = find(s.name);
	if (sc == nullptr || sc->versions.empty())	// on the server, no run record: nothing to write it into
	{
		state_.status = "no playtests/" + s.name + "/versions.md: the coordinator adds v1";
		printf("playtest: %s started (sim %s, seed %u), but no playtests/%s/versions.md: the coordinator adds v1\n", s.name.c_str(), s.simHash.c_str(), s.seed, s.name.c_str());
		buildState();
		return;
	}
	active_ = beginRun(s.name, s.seed);
	active_.simHash = s.simHash;
	active_.bead = s.bead;	// the started file's bead= is the run's, even empty (h2v.8)
	active_.scenarioHash = hex64(s.fileHash);
	state_.selected = s.name;
	writeRun(active_, true);
	loadTriage(active_);
	notes_.open = true;	// the notes dock at the side for the run (user 2026-11-07); it takes no keys until clicked or Ctrl+N
	state_.status = "playing " + active_.scenario + " " + active_.version;
	// the version's own hashes: a run on another scenario file or sim build is still recorded (its file carries the real ones), flagged
	const Version& v = sc->versions.front();
	std::string note;
	if (!v.scenarioHash.empty() && v.scenarioHash != active_.scenarioHash) { note += ", scenario file " + active_.scenarioHash + " not " + v.scenarioHash; }
	if (!v.simHash.empty() && v.simHash != s.simHash) { note += ", sim " + s.simHash + " not " + v.simHash; }
	if (!note.empty()) { state_.status += " (" + v.name + ":" + note.substr(1) + ")"; }
	printf("playtest: run %s (%s, sim %s, seed %u, scenario %s%s)\n", relFile(active_).c_str(), active_.version.c_str(), s.simHash.c_str(), s.seed,
		active_.scenarioHash.c_str(), note.c_str());
	if (fromPicker) { openScenarios(false); }
	buildState();
}

void Playtest::ended(const std::string& name, const std::vector<ui::PlaytestMetric>& m)
{
	if (endingOn_ && ending_.scenario == name)	// the run an END was sent for: its own file, whichever run is active now
	{
		ending_.metrics = m;
		writeRun(ending_, true);
		printf("playtest: metrics %s: %s\n", relFile(ending_).c_str(), metricLine(m).c_str());
		ending_ = Active();
		endingOn_ = false;
		return;
	}
	if (active_.on && active_.scenario == name)
	{
		active_.metrics = m;
		save();
		printf("playtest: metrics %s: %s\n", relFile(active_).c_str(), metricLine(m).c_str());
		return;
	}
	printf("playtest: metrics for %s, no run of it\n", name.c_str());
}

// The quit's wait for METRICS is over and the answer never came: the run's file is written anyway, saying why (an invalid metric writes its name alone).
void Playtest::metricsTimeout()
{
	if (!endingOn_) { return; }
	ending_.metrics = { { "(not received: quit)", 0, false } };
	writeRun(ending_, true);
	printf("playtest: metrics %s: (not received: quit)\n", relFile(ending_).c_str());
	ending_ = Active();
	endingOn_ = false;
	buildState();
}

// A LOAD or LIST_REQ the server never answered (main.cpp calls this every frame): the Play leaves its loading state with the error as a
// message, the window's status says it instead of an empty list, and no run file is written (the file is the answer's, in started()).
void Playtest::tick()
{
	const auto waited = [](const std::chrono::steady_clock::time_point& since) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count(); };
	if (listPending_ && !state_.open) { listPending_ = false; }	// the window closed: nobody is reading the answer
	if (listPending_ && waited(listAt_) >= kAnswerSeconds)
	{
		listPending_ = false;
		state_.status = kNoAnswer;
		printf("playtest: no answer to the scenario list after %.0f s\n", kAnswerSeconds);
	}
	if (!loading_.empty() && waited(loadingAt_) >= kAnswerSeconds)
	{
		printf("playtest: %s: no answer to the load after %.0f s (older server build?)\n", loading_.c_str(), kAnswerSeconds);
		loading_.clear();
		closeOnRun_ = false;
		state_.status = kNoAnswer;
		if (host_.error) { host_.error(kNoAnswer); }
	}
}

void Playtest::leave()
{
	if (!active_.on) { return; }
	writeRun(active_, true);
	if (!active_.recipe && host_.live && host_.live() && host_.end)	// a recipe has no .kscen run on the server: no END, no metrics
	{
		ending_ = active_;	// END's METRICS still comes and rewrites its file (the quit's wait, main.cpp cleanup)
		endingOn_ = true;
		host_.end();
	}
	printf("playtest: left %s\n", relFile(active_).c_str());
	active_ = Active();
	state_.status = "no run";
	buildState();
}

void Playtest::openNotes(bool on)
{
	if (on)
	{
		if (!active_.on) { state_.status = "no run: play a scenario first"; return; }
		loadTriage(active_);
	}
	notes_.open = on;
	if (!on)
	{
		if (typing_) { typing(false); }
		save();
	}
}

void Playtest::note(const std::string& text)
{
	if (!active_.on) { state_.status = "no run: play a scenario first"; printf("playtest: note needs a run (play a scenario first)\n"); return; }
	ui::PlaytestNote n;
	n.n = (int)active_.notes.size() + 1;
	n.text = text;
	for (char& c : n.text) { if (c == '\n' || c == '\r') { c = ' '; } }	// one note per line in the file
	active_.notes.push_back(n);
	lastNote_ = n.n;
	lastNoteAt_ = std::chrono::steady_clock::now();
	printf("playtest: note %d\n", n.n);
	buildState();
	// k16.2.127: the note is also a comment on the --bead item, posted now (off the frame thread), so a crash loses nothing from it.
	// Mark's stamp is put before the text unless it is already there (MARK pastes it into the box).
	if (!beadTarget_.empty() && host_.post)
	{
		const std::string stamp = mark();
		std::string body = "[playtest " + runTag().substr(1) + " note " + std::to_string(n.n) + " ";
		if (!stamp.empty() && n.text.rfind(stamp, 0) != 0) { body += "@" + stamp; }
		body += n.text + "\n" + buildLine();
		const std::string what = "note " + std::to_string(n.n), file = active_.file;
		const int number = n.n;
		host_.post(beadTarget_, body, [this, file, what, number](bool ok, const std::string& why) { posted(file, what, number, ok, why); });
	}
}

void Playtest::verdict(const std::string& v)
{
	if (v != "good" && v != "off" && v != "broken") { state_.status = "verdict: good, off or broken"; printf("playtest: verdict must be good, off or broken\n"); return; }
	if (!active_.on) { state_.status = "no run: play a scenario first"; printf("playtest: verdict needs a run (play a scenario first)\n"); return; }
	active_.verdict = v;
	printf("playtest: verdict %s\n", v.c_str());
	buildState();
	if (!beadTarget_.empty() && host_.post)	// k16.2.127: its own comment, as soon as it is set
	{
		const std::string what = "verdict " + v, file = active_.file;
		// no "[playtest " prefix: the waker skips that prefix, and the verdict is the one comment that wakes (k16.2.135)
		host_.post(beadTarget_, "verdict (" + runName() + " run " + std::filesystem::path(active_.file).stem().string() + "): " + v + "\n" + buildLine(),
			[this, file, what](bool ok, const std::string& why) { posted(file, what, 0, ok, why); });
	}
}

// A comment's answer (the bd writer, on the frame thread). Success: the Scenarios window's status says where it went, and the note's post
// line goes (a later ok after a failure). Failure: the client's message line says which comment is not on the item and why (the run file
// still has it), the note gets its post line ("not on <item>: <why>"), and the Notes window's warning line says the run's notes are only in the
// file (k16.2.130.1). A verdict's failure goes to the warning line only. An answer for a run that is no longer the active one only logs.
void Playtest::posted(const std::string& file, const std::string& what, int note, bool ok, const std::string& why)
{
	const std::string where = beadTarget_.empty() ? std::string("the item") : beadTarget_;
	const bool current = active_.on && active_.file == file;
	if (ok)
	{
		state_.status = what + " on " + where;
		printf("playtest: %s on %s\n", what.c_str(), where.c_str());
		if (current && note > 0)
		{
			for (ui::PlaytestNote& n : active_.notes) { if (n.n == note) { n.post.clear(); } }
			buildState();
		}
		return;
	}
	const std::string line = what + " not on " + where + ": " + why;
	state_.status = line;
	printf("playtest: %s\n", line.c_str());
	if (host_.error) { host_.error(line); }
	if (!current) { return; }
	for (ui::PlaytestNote& n : active_.notes) { if (n.n == note) { n.post = "not on " + where + ": " + why; } }
	beadFailed(why);
}

void Playtest::beadFailed(const std::string& why)
{
	if (!active_.on) { return; }
	(void)why;	// each note carries its own reason in its post line; a verdict's or shot's is on the client's message line and stdout
	active_.beadWarn = "bd unreachable: notes kept in " + active_.file;
	buildState();
}

// The coordinator's answers arrive in the file (Triage); a rewrite keeps them.
void Playtest::loadTriage(Active& a)
{
	const ParsedRun file = parseRun(a.file, relFile(a));
	for (ui::PlaytestNote& n : a.notes)
	{
		for (const ui::PlaytestNote& f : file.notes) { if (f.n == n.n && n.triage.empty()) { n.triage = f.triage; } }
	}
	buildState();
}

std::string Playtest::mark() const
{
	if (!active_.on || !host_.stamp) { return {}; }
	return host_.stamp();
}

std::string Playtest::runName() const
{
	if (!active_.on) { return {}; }
	return active_.scenario + " " + active_.version;
}

std::string Playtest::runTag() const
{
	if (!active_.on) { return {}; }
	return "[" + runName() + " run " + std::filesystem::path(active_.file).stem().string() + "]";
}

// The build line every item comment, F12 .txt sidecar and events file starts with (kenshi-k16.2.127): the client's git hash, and the
// sim's (the run's own, else the wire's; "?" when neither has said).
std::string Playtest::buildLine() const
{
	const std::string& sim = active_.on && !active_.simHash.empty() ? active_.simHash : simHash_;
	return "build: client " + (clientHash_.empty() ? "?" : clientHash_) + " sim " + (sim.empty() ? "?" : sim);
}

// The note a shot taken now cites: the one added in the last 30 s, else the one being typed (the number it will get); "" none.
std::string Playtest::recentNote() const
{
	if (!active_.on) { return {}; }
	if (typing_) { return std::to_string(active_.notes.size() + 1) + " (being typed)"; }
	const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastNoteAt_).count();
	if (lastNote_ > 0 && age <= 30) { return std::to_string(lastNote_) + " (just added)"; }
	return {};
}

bool Playtest::typing(bool on)
{
	if (on == typing_) { return typing_; }
	typing_ = on;
	if (host_.paused) { host_.paused(on); }	// the host pauses the sim, and on false restores its state before
	return typing_;
}

void Playtest::save()
{
	if (!active_.on) { return; }
	writeRun(active_, true);
	printf("playtest: saved %s\n", relFile(active_).c_str());
}

std::string Playtest::relFile(const Active& a) const
{
	const std::string prefix = dir_ + "/";
	return a.file.rfind(prefix, 0) == 0 ? a.file.substr(prefix.size()) : a.file;
}

// A run as its file's text (the format in docs/CLIENT_GOALS.md "Playtest"); write: create it (and its directories).
std::string Playtest::writeRun(Active& a, bool write)
{
	loadTriage(a);
	std::ostringstream o;
	o << "# " << a.scenario << " " << a.version << "\n\n";
	o << "date: " << a.date << "\n";
	const std::string& sim = a.simHash.empty() ? simHash_ : a.simHash;	// the run's own (a recipe's session build), else the wire's
	o << "sim: " << (sim.empty() ? "?" : sim) << "\n";
	o << "client: " << (clientHash_.empty() ? "?" : clientHash_) << "\n";
	o << "seed: " << a.seed << "\n";
	if (!a.bead.empty()) { o << "bead: " << a.bead << "\n"; }	// the feature item this playtest proves (h2v.8; none: no line)
	if (!beadTarget_.empty()) { o << "playtest: " << beadTarget_ << "\n"; }	// k16.2.127: the item kclient --bead posts the notes, verdict and shots to
	o << "metrics:";
	for (size_t i = 0; i < a.metrics.size(); i++)	// "name=value, name": an invalid one is its name alone
	{
		const ui::PlaytestMetric& m = a.metrics[i];
		o << (i ? ", " : " ") << m.name << (m.valid ? "=" + number(m.value) : "");
	}
	o << "\n";
	o << "verdict: " << a.verdict << "\n\n";
	o << "## Notes\n\n";
	for (const ui::PlaytestNote& n : a.notes) { o << n.n << ". " << n.text << "\n"; }
	o << "\n## Triage\n\n";
	for (const ui::PlaytestNote& n : a.notes) { if (!n.triage.empty()) { o << n.n << ". " << n.triage << "\n"; } }
	if (write)
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(a.file).parent_path(), ec);
		std::ofstream out(a.file);
		out << o.str();
		if (!out) { printf("playtest: cannot write %s\n", a.file.c_str()); }
	}
	return o.str();
}

std::string Playtest::dump() const
{
	std::string out;
	if (!loaded_) { out += "playtest: " + state_.status + "\n"; }
	for (const Scenario& s : scenarios_)
	{
		std::string lastDate, lastVerdict, lastFile;
		const std::string bead = beadOf(s.name);
		out += "scenario " + s.name + " \"" + s.title + "\"" + (s.recipe ? " (recipe)" : "") + (bead.empty() ? "" : " bead " + bead) + "\n";
		out += "  setup: " + s.setup + "\n";
		out += "  look_for: " + s.lookFor + "\n";
		out += "  metrics:";
		for (const std::string& m : s.metricNames) { out += " " + m; }
		out += "\n";
		for (const Version& v : s.versions)
		{
			out += "  " + v.name + " " + v.date + " sim " + (v.simHash.empty() ? "?" : v.simHash) + " client " + (v.clientHash.empty() ? "?" : v.clientHash) +
				" scenario " + (v.scenarioHash.empty() ? "?" : v.scenarioHash) + "\n";
			if (!v.changed.empty()) { out += "    changed: " + v.changed + "\n"; }
			if (!v.answers.empty()) { out += "    answers: " + v.answers + "\n"; }
			for (const ui::PlaytestRun& r : v.runs)
			{
				out += "    run " + r.file + " " + r.date + " sim " + (r.simHash.empty() ? "?" : r.simHash) + " client " + (r.clientHash.empty() ? "?" : r.clientHash) +
					" seed " + std::to_string(r.seed) + " verdict " + (r.verdict.empty() ? "-" : r.verdict) + (r.mismatch ? " MISMATCH" : "") + "\n";
				for (const ui::PlaytestMetric& m : r.metrics) { out += "      " + m.name + "=" + (m.valid ? number(m.value) : std::string("?")) + "\n"; }
				if (!r.firstNote.empty()) { out += "      first: " + r.firstNote + "\n"; }
				if (lastFile.empty() || stampOf(r.file) > stampOf(lastFile)) { lastFile = r.file; lastDate = r.date; lastVerdict = r.verdict; }
			}
		}
		out += "  last: " + (lastDate.empty() ? std::string("-") : lastDate + " " + (lastVerdict.empty() ? "-" : lastVerdict)) + "\n";
	}
	if (serverListed_)	// the wire's half: what the server can load, with the file hash versions.md records
	{
		out += "server: " + std::to_string(serverList_.size()) + " scenario" + (serverList_.size() == 1 ? "" : "s") + "\n";
		for (const ServerScenario& s : serverList_)
			out += "  server " + s.name + " " + hex64(s.fileHash) + (s.bead.empty() ? "" : " bead " + s.bead) + (find(s.name) ? "" : " (no version in the store)") + "\n";
	}
	return out;
}
