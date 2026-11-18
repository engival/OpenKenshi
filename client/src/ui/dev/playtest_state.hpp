#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The playtest windows' host structs (docs/reference/PLAYTEST.md in the main repo; client coordinator 2026-11-07). A local copy of the shape
// worker-58 writes on ds/playtest-host (this same path); the client coordinator reconciles the two at merge.
namespace ui
{
	struct PlaytestMetric
	{
		std::string name;
		double value = 0;
		bool valid = false;	// false: the sim didn't report it ("-")
		bool operator==(const PlaytestMetric&) const = default;
	};

	struct PlaytestRun
	{
		std::string date;	// "2026-11-07 15:30"
		std::string file;	// playtests/<scenario>/v<N>/<yyyymmdd-hhmm>.md
		std::string simHash, clientHash;
		std::string verdict;	// "good" | "off" | "broken" | ""
		std::string firstNote;
		uint64_t seed = 0;
		std::vector<PlaytestMetric> metrics;
		bool mismatch = false;	// built from other hashes than its version's
		bool operator==(const PlaytestRun&) const = default;
	};

	struct PlaytestVersion
	{
		std::string name;	// "v2"
		std::string date;
		std::string changed;	// what changed since the version before
		std::string answers;	// the notes it is meant to fix ("v1/20261107-1530 #3")
		std::vector<PlaytestRun> runs;
		bool operator==(const PlaytestVersion&) const = default;
	};

	struct PlaytestScenario
	{
		std::string name;	// the file's name ("bar_lure")
		std::string title, setup, lookFor;
		std::string lastDate, lastVerdict;
		std::vector<PlaytestVersion> versions;	// the host's order (versions.md: newest first)
		std::string recipe;	// non-empty: a recipe playtest (versions.md 'run: recipe'), the steps to run it; the list's Setup reads "Recipe"
		std::string bead;	// the feature's bead id ("kenshi-h2v.1"); empty: none
		bool operator==(const PlaytestScenario&) const = default;
	};

	struct ScenariosState
	{
		std::vector<PlaytestScenario> scenarios;
		std::string selected;	// a scenario's name; "": none
		std::string status;	// one line ("loading bar_lure...")
		bool open = false;	// the window shows
		bool operator==(const ScenariosState&) const = default;
	};

	struct ScenariosActions
	{
		std::function<void(const std::string&)> select, play;
		std::function<void()> close;	// the X or Escape (the window hides at once)
	};

	struct PlaytestNote
	{
		int n = 0;	// its number in the run
		std::string text;
		std::string triage;	// the coordinator's answer; "": none yet
		std::string post;	// why it did not reach the --bead item ("not on kenshi-h2v.2.1: bd not found on PATH"); "": posted, or no --bead (kenshi-k16.2.130)
		bool operator==(const PlaytestNote&) const = default;
	};

	struct NotesState
	{
		std::string scenario, version;
		std::string verdict;	// "good" | "off" | "broken" | ""
		std::vector<PlaytestNote> notes;
		std::string warning;	// one line at the window's bottom for the whole run (bd unreachable, the run file kept); "": none
		bool open = false;
		bool operator==(const NotesState&) const = default;
	};

	struct NotesActions
	{
		std::function<void(const std::string&)> add;	// a new note's text
		std::function<void(const std::string&)> verdict;	// "good" | "off" | "broken"
		std::function<std::string()> mark;	// the stamp ("00:42.3 Wanderer (x, z):"), put before the edit text
		std::function<void(bool)> typing;	// the edit box took / lost the keyboard: the host pauses the sim while true
		std::function<void()> close;	// the X or Escape (the window hides at once)
	};
}
