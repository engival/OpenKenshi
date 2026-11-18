#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The jobs list (docs/UI.md), after Kenshi_MainPanel.layout's OrdersPanel JobsPanel (an ItemBox with drag and drop) and its rows,
// Kenshi_OrdersPanelOrder.layout: one character's jobs in priority order, "1: Medic" in #afa68b (HudNeutral). X removes one; a row dragged (a ghost
// follows the pointer, the row stays) and dropped on another row takes that row's place, on empty space goes last. No jobs: the layout's JobsPanelInfo
// text, faded. Live and static: kinspect `findings/ui_captures/jobs.md`, `findings/jobs17_static.md`. The host owns the jobs; every change is its order.
namespace ui
{
	struct Job
	{
		uint64_t id = 0;
		std::string text;	// "Medic", "Operating machine: Ore Drill"; the number is ours
		bool dim = false;	// the AI skips it (JOBS off, except the types it always runs): half alpha
		bool operator==(const Job&) const = default;
	};

	class JobList : public Panel
	{
	public:
		static constexpr float kRowH = 24, kGap = 0;	// 16 px at 720 lines, no gap
		JobList();
		std::function<void(uint64_t id)> onRemove;
		std::function<void(uint64_t id, int index)> onMove;	// index: its place in the new order (the drop row's; last on empty space)
		void setJobs(const std::vector<Job>& jobs);
		const std::vector<Job>& jobs() const { return jobs_; }
		// For scripted tests.
		Widget* row(size_t i) const;
		Button* removeButton(size_t i) const;
		Label* emptyText() const { return empty_; }
		int dropIndex() const { return drop_; }	// while dragging over the list, else -1

	protected:
		void paintOver() const override;

	private:
		struct Row;
		ScrollView* scroll_ = nullptr;
		Label* empty_ = nullptr;
		std::vector<Row*> rows_;
		std::vector<Job> jobs_;
		int from_ = -1, drop_ = -1;
		float dragY_ = 0;
		int indexAt(float x, float y) const;	// the drop place for a pointer (pixels); -1 off the list
		void dragTo(int from, float x, float y);
		void dropAt(int from, float x, float y);
	};
}
