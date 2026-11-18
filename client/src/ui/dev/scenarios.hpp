#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/dev/playtest_state.hpp"
#include "ui/widget.hpp"

// The playtest scenarios window (docs/UI.md "Playtest windows"; docs/reference/PLAYTEST.md; client coordinator 2026-11-07): a
// resizable SCENARIOS window, its body from client/data/ui/scenarios.layout. Left: the scenarios in a Table (name, setup, last run, verdict).
// Right, for the selected one: its title, LOOK FOR, a fixed line that one run is evidence and not proof, and RESULTS: a block per version in
// the host's order ("vN  date", Changed / Answers, then a row per run: date, sim / client hash ("!" in Bad when the build differs from the
// version's), seed, a column per metric (the scenario's names, first seen first; "-" when not reported), verdict, the first note). The host's
// status line and PLAY at the bottom. The host's `open` shows it; the X / Escape hide it and call close(). Non-modal.
namespace ui
{
	class ScenariosWindow
	{
	public:
		explicit ScenariosWindow(ScenariosActions actions = {});
		~ScenariosWindow();
		void update(const ScenariosState& s);	// every frame (cheap when nothing changed); s.open shows / hides it
		bool shown() const;
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		const Layout& layout() const { return *layout_; }
		Table* list() const { return table_; }
		Label* title() const { return title_; }
		Label* lookFor() const { return lookFor_; }
		Label* recipe() const { return recipe_; }	// hidden (with its RECIPE heading) when the scenario has none
		Label* bead() const { return bead_; }	// "Bead: id", hidden when none
		Label* caveat() const { return caveat_; }
		Label* status() const { return status_; }
		Button* playButton() const { return play_; }
		ScrollView* results() const { return results_; }
		const std::vector<std::string>& metricNames() const { return metrics_; }	// the run rows' metric columns
		size_t versionBlocks() const { return heads_.size(); }
		Label* versionHead(size_t i) const { return i < heads_.size() ? heads_[i] : nullptr; }
		// Run r of version v: its cells (date, sim, client, seed, metrics..., verdict, note), or null.
		const std::vector<Label*>* runCells(size_t v, size_t r) const;

	private:
		void bind(Layout& l);
		void rebuildList();
		void rebuildResults();
		void hide();
		const PlaytestScenario* current() const;
		CaptionWindow* win_ = nullptr;
		std::unique_ptr<Layout> layout_;
		ScenariosActions act_;
		ScenariosState last_;
		bool built_ = false, syncing_ = false;
		Table* table_ = nullptr;
		Label *title_ = nullptr, *lookFor_ = nullptr, *caveat_ = nullptr, *status_ = nullptr;
		Label *recipe_ = nullptr, *recipeHead_ = nullptr, *bead_ = nullptr;
		Button* play_ = nullptr;
		ScrollView* results_ = nullptr;
		std::vector<std::string> metrics_;
		std::vector<Label*> heads_;
		std::vector<std::vector<std::vector<Label*>>> cells_;	// [version][run][cell]
	};
}
