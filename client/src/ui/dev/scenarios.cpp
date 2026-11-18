#include "ui/dev/scenarios.hpp"

#include <algorithm>
#include <cstdio>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		constexpr float kRowH = 24, kDateW = 120, kHashW = 72, kSeedW = 60, kMetricW = 76, kVerdictW = 60, kNoteMin = 120;

		std::string number(double v)
		{
			char b[32];
			snprintf(b, sizeof b, "%g", v);
			return b;
		}

		Color verdictColour(const std::string& v)
		{
			if (v == "good") { return res::colour("Good"); }
			if (v == "broken") { return res::colour("Bad"); }
			if (v == "off") { return res::colour("Accent"); }
			return res::colour("Main").alpha(0.6f);
		}

		// A row of clipped cells; basis 0 = fill. Returns its labels.
		std::vector<Label*> cellRow(Widget& body, const std::vector<std::pair<float, std::string>>& cells, const std::vector<Color>& colours,
			const std::vector<std::string>& tips, const char* font)
		{
			Flex& line = body.add<Flex>(false, 8);
			line.halign = Widget::Align::kStretch;
			line.rect.h = kRowH;
			std::vector<Label*> out;
			for (size_t i = 0; i < cells.size(); i++)
			{
				const float w = cells[i].first;
				Widget& cell = line.add<Widget>(w > 0 ? FlexItem{ .basis = w } : FlexItem{ .fill = 1, .min = kNoteMin });
				cell.valign = Widget::Align::kStretch;
				cell.clipChildren = true;
				if (i < tips.size() && !tips[i].empty())
				{
					cell.solid = true;
					cell.tooltip = tips[i];
				}
				Label& l = cell.add<Label>(cells[i].second, font);
				l.valign = Widget::Align::kCenter;
				l.colour = i < colours.size() ? colours[i] : res::colour("Main");
				out.push_back(&l);
			}
			return out;
		}
	}

	ScenariosWindow::ScenariosWindow(ScenariosActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<CaptionWindow>("SCENARIOS", [this]
		{
			hide();
			if (act_.close) { act_.close(); }
		}, 0);
		win_->halign = win_->valign = Widget::Align::kCenter;
		win_->rect = { 0, 0, 1500, 780 };
		win_->resizable = true;
		win_->minSize = { 1000, 560 };
		win_->visible = false;
		win_->body().rect.h = 780 - CaptionWindow::kHeaderH;
		layout_ = std::make_unique<Layout>(win_->body(), "scenarios", [this](Layout& l) { bind(l); });
		if (!play_) { bind(*layout_); }	// the file failed: spares, so the window works under the red box
	}

	ScenariosWindow::~ScenariosWindow()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void ScenariosWindow::bind(Layout& l)
	{
		Widget& slot = l.at<Widget>("list");
		if (slot.children().empty())	// a slot's children stay across a reload
		{
			Table& t = slot.add<Table>(std::vector<Table::Column>{ { "Name", 140 }, { "Setup", 0 }, { "Last run", 0 }, { "Verdict", 62 } });
			t.halign = t.valign = Widget::Align::kStretch;
		}
		table_ = static_cast<Table*>(slot.children().front().get());
		table_->onSelect = [this](const std::string& id) { if (!syncing_ && act_.select) { act_.select(id); } };
		title_ = &l.at<Label>("title");
		lookFor_ = &l.at<Label>("lookfor");
		recipe_ = &l.at<Label>("recipe");
		recipeHead_ = &l.at<Label>("recipehead");
		bead_ = &l.at<Label>("bead");
		caveat_ = &l.at<Label>("caveat");
		status_ = &l.at<Label>("status");
		results_ = &l.at<ScrollView>("results");
		static_cast<Box&>(results_->body()).spacing = 2;
		play_ = &l.at<Button>("play");
		play_->textColour = res::colour("Main");
		play_->onClicked = [this] { if (current() && act_.play) { act_.play(last_.selected); } };
		if (built_)
		{
			rebuildList();
			rebuildResults();
		}
	}

	const PlaytestScenario* ScenariosWindow::current() const
	{
		for (const PlaytestScenario& s : last_.scenarios) { if (s.name == last_.selected) { return &s; } }
		return nullptr;
	}

	void ScenariosWindow::rebuildList()
	{
		std::vector<Table::Row> rows;
		for (const PlaytestScenario& s : last_.scenarios)
		{
			const bool recipe = !s.recipe.empty();	// the Setup cell says so (no per-cell ink; the word also sorts them together), the tip has the steps
			Table::Row r{ s.name, { s.name, recipe ? "Recipe" : s.setup, s.lastDate.empty() ? "never" : s.lastDate, s.lastVerdict.empty() ? "-" : s.lastVerdict } };
			const std::string& what = recipe ? s.recipe : s.setup;
			r.tip = s.title.empty() ? what : s.title + ": " + what;
			rows.push_back(std::move(r));
		}
		syncing_ = true;
		table_->setRows(std::move(rows));
		table_->select(last_.selected);
		syncing_ = false;
	}

	void ScenariosWindow::rebuildResults()
	{
		const PlaytestScenario* s = current();
		title_->text = s ? (s->title.empty() ? s->name : s->title) : "Pick a scenario";
		lookFor_->text = s ? s->lookFor : std::string();
		recipe_->text = s ? s->recipe : std::string();
		recipe_->visible = recipeHead_->visible = !recipe_->text.empty();
		bead_->text = s && !s->bead.empty() ? "Bead: " + s->bead : std::string();
		bead_->visible = !bead_->text.empty();
		play_->enabled = s != nullptr;
		metrics_.clear();
		if (s)
		{
			for (const PlaytestVersion& v : s->versions)
			{
				for (const PlaytestRun& r : v.runs)
				{
					for (const PlaytestMetric& m : r.metrics) { if (std::find(metrics_.begin(), metrics_.end(), m.name) == metrics_.end()) { metrics_.push_back(m.name); } }
				}
			}
		}
		Widget& body = results_->body();
		body.clear();
		heads_.clear();
		cells_.clear();
		if (!s) { return; }
		const Color main = res::colour("Main"), dim = main.alpha(0.6f);
		auto line = [&](const std::string& text, const Color& c)
		{
			Label& l = body.add<Label>(text, "Kenshi_StandardFont_Medium");
			l.wrap = true;
			l.halign = Widget::Align::kStretch;
			l.colour = c;
			return &l;
		};
		if (s->versions.empty()) { line("No versions yet.", dim); }
		for (const PlaytestVersion& v : s->versions)
		{
			Label* h = line(v.name + (v.date.empty() ? "" : "   " + v.date), res::colour("DebugHeading"));
			h->font = fonts::get("Kenshi_PaintedTextFont_Medium");
			h->wrap = false;
			heads_.push_back(h);
			if (!v.changed.empty()) { line("Changed: " + v.changed, main); }
			if (!v.answers.empty()) { line("Answers: " + v.answers, main); }
			cells_.emplace_back();
			if (v.runs.empty())
			{
				line("No runs yet.", dim);
				continue;
			}
			std::vector<std::pair<float, std::string>> head = { { kDateW, "Date" }, { kHashW, "Sim" }, { kHashW, "Client" }, { kSeedW, "Seed" } };
			std::vector<std::string> headTips = { "", "", "", "" };
			for (const std::string& m : metrics_)
			{
				head.push_back({ kMetricW, m });
				headTips.push_back(m);
			}
			head.push_back({ kVerdictW, "Verdict" });
			head.push_back({ 0, "First note" });
			cellRow(body, head, std::vector<Color>(head.size(), dim), headTips, "Kenshi_StandardFont_MediumSmall");
			for (const PlaytestRun& r : v.runs)
			{
				const std::string bang = r.mismatch ? "! " : "";
				std::vector<std::pair<float, std::string>> c = { { kDateW, r.date }, { kHashW, bang + r.simHash }, { kHashW, bang + r.clientHash }, { kSeedW, std::to_string(r.seed) } };
				const Color hashC = r.mismatch ? res::colour("Bad") : main;
				std::vector<Color> col = { main, hashC, hashC, main };
				const std::string why = r.mismatch ? "Built from other hashes than " + v.name + "'s: compare with care" : std::string();
				std::vector<std::string> tips = { r.file, why, why, "" };
				for (const std::string& m : metrics_)
				{
					std::string t = "-";
					for (const PlaytestMetric& pm : r.metrics) { if (pm.name == m && pm.valid) { t = number(pm.value); } }
					c.push_back({ kMetricW, t });
					col.push_back(t == "-" ? dim : main);
					tips.push_back(m);
				}
				c.push_back({ kVerdictW, r.verdict.empty() ? "-" : r.verdict });
				col.push_back(verdictColour(r.verdict));
				tips.push_back("");
				c.push_back({ 0, r.firstNote });
				col.push_back(main);
				tips.push_back(r.firstNote);
				cells_.back().push_back(cellRow(body, c, col, tips, "Kenshi_StandardFont_Medium"));
			}
			body.add<Widget>().rect = { 0, 0, 0, 10 };
		}
	}

	const std::vector<Label*>* ScenariosWindow::runCells(size_t v, size_t r) const
	{
		return v < cells_.size() && r < cells_[v].size() ? &cells_[v][r] : nullptr;
	}

	void ScenariosWindow::update(const ScenariosState& s)
	{
		if (built_ && s == last_) { return; }
		const bool list = !built_ || s.scenarios.size() != last_.scenarios.size() || s.selected != last_.selected
			|| [&] { for (size_t i = 0; i < s.scenarios.size(); i++) { const auto &a = s.scenarios[i], &b = last_.scenarios[i];
				if (a.name != b.name || a.setup != b.setup || a.recipe != b.recipe || a.title != b.title || a.lastDate != b.lastDate || a.lastVerdict != b.lastVerdict) { return true; } } return false; }();
		const bool results = !built_ || s.selected != last_.selected || s.scenarios != last_.scenarios;
		last_ = s;
		built_ = true;
		if (list) { rebuildList(); }
		if (results)
		{
			rebuildResults();
			results_->offset = 0;
		}
		status_->text = s.status;
		if (s.open != win_->visible)
		{
			win_->visible = s.open;
			if (s.open)
			{
				win_->raise();
				focus(win_);
			}
			else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
		}
	}

	void ScenariosWindow::hide()
	{
		win_->visible = false;
		last_.open = false;	// the host's next state says so too
		if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool ScenariosWindow::shown() const { return win_->visible; }
}
