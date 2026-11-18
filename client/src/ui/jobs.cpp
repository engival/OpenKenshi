#include "ui/jobs.hpp"

#include <cmath>

#include "ui/ui.hpp"

namespace ui
{
	// A row: the ConnectBar strip with the text; X at the right. Pressed and dragged, it reorders.
	struct JobList::Row : public Widget
	{
		JobList* owner = nullptr;
		int index = 0;
		Label* text = nullptr;
		Button* remove = nullptr;
		bool moved_ = false;
		float pressY_ = 0;

		Row()
		{
			solid = true;
			halign = Align::kStretch;
			rect.h = kRowH;
			text = &add<Label>("", "Kenshi_StandardFont_Medium");	// OrderText (Kenshi_TextboxStandardText) at 10
			text->fit = false;
			text->halign = Align::kStretch;
			text->rect = { 10, 0, 45 + 4, kRowH };	// cut before the X, no ellipsis
			remove = &add<Button>("X", Look::kButton, "Kenshi_StandardFont_Medium");	// RemoveButton: Kenshi_Button1, a red X, 30 x 15 at 720 lines
			remove->textColour = { 0.8f, 0.266f, 0.266f, 1 };
			remove->halign = Align::kEnd;
			remove->valign = Align::kCenter;
			remove->rect = { 0, 0, 45, 22 };
			remove->tooltip = "Remove the job";
		}

		bool onPress(Btn b, float, float y) override
		{
			moved_ = false;
			pressY_ = y;
			return b == Btn::kLeft;
		}

		void onDrag(float x, float y) override
		{
			if (!moved_ && std::abs(y - pressY_) < 4 * scale_) { return; }
			moved_ = true;
			owner->dragTo(index, x, y);
		}

		void onRelease(Btn, float x, float y) override
		{
			if (moved_) { owner->dropAt(index, x, y); }
			moved_ = false;
		}

	protected:
		void paint() const override { style().draw(Look::kField, screen_, scale_, State::kNormal, false); }	// Kenshi_ConnectBarSkin
	};

	JobList::JobList() : Panel(Look::kInset, 10)	// Kenshi_OrdersTextboxSkin; JobsPanel sits 10 in
	{
		scroll_ = &add<ScrollView>();
		scroll_->halign = scroll_->valign = Align::kStretch;
		static_cast<Box&>(scroll_->body()).spacing = kGap;
		empty_ = &add<Label>("[No jobs assigned]\n[To add jobs, hold shift key when giving an order]", "Kenshi_StandardFont_Medium");	// JobsPanelInfo
		empty_->halign = empty_->valign = Align::kStretch;
		empty_->wrap = true;
		empty_->fit = false;
		empty_->vAlign = TextAlign::kStart;
		empty_->colour = style().textColour(Look::kInset, State::kNormal, false).alpha(0.5f);
	}

	Widget* JobList::row(size_t i) const { return i < jobs_.size() ? rows_[i] : nullptr; }
	Button* JobList::removeButton(size_t i) const { return i < jobs_.size() ? rows_[i]->remove : nullptr; }

	void JobList::setJobs(const std::vector<Job>& jobs)
	{
		if (jobs == jobs_) { return; }
		jobs_ = jobs;
		while (rows_.size() < jobs_.size())
		{
			Row& r = scroll_->body().add<Row>();
			r.owner = this;
			r.index = (int)rows_.size();
			r.remove->onClicked = [this, i = r.index] { if (i < (int)jobs_.size() && onRemove) { auto f = onRemove; f(jobs_[i].id); } };
			rows_.push_back(&r);
		}
		for (size_t i = 0; i < rows_.size(); i++)
		{
			rows_[i]->visible = i < jobs_.size();
			if (i >= jobs_.size()) { continue; }
			rows_[i]->text->text = std::to_string(i + 1) + ": " + jobs_[i].text;
			rows_[i]->text->colour = res::colour("HudNeutral").alpha(jobs_[i].dim ? 0.5f : 1);
		}
		empty_->visible = jobs_.empty();
		from_ = drop_ = -1;
	}

	int JobList::indexAt(float x, float y) const
	{
		const Rect& c = scroll_->screen();
		if (x < c.x || x >= c.right() || y < c.y || y >= c.bottom()) { return -1; }
		for (size_t i = 0; i < jobs_.size(); i++)
		{
			const Rect& r = rows_[i]->screen();
			if (y >= r.y && y < r.bottom()) { return (int)i; }
		}
		return jobs_.empty() ? -1 : (int)jobs_.size() - 1;	// empty space: last
	}

	void JobList::dragTo(int from, float x, float y)
	{
		from_ = from;
		drop_ = indexAt(x, y);
		dragY_ = y;
	}

	void JobList::dropAt(int from, float x, float y)
	{
		const int to = indexAt(x, y);
		from_ = drop_ = -1;
		if (to >= 0 && to != from && from < (int)jobs_.size() && onMove) { auto f = onMove; f(jobs_[from].id, to); }
	}

	void JobList::paintOver() const
	{
		if (from_ < 0 || from_ >= (int)jobs_.size()) { return; }
		// The ghost: a copy of the row, centred on the pointer's height (the row itself stays).
		const Row& r = *rows_[from_];
		const Rect g = { r.screen().x, std::round(dragY_ - r.screen().h / 2), r.screen().w, r.screen().h };
		style().draw(Look::kField, g, scale_, State::kNormal, false, Color{}.alpha(0.7f));
		const Label& t = *r.text;
		gfx::pushClip({ g.x + 10 * scale_, g.y, t.screen().w, g.h });
		t.font->draw(g.x + 10 * scale_, std::round(g.y + (g.h - t.font->lineHeight()) / 2), t.text, t.colour.alpha(0.7f));
		gfx::popClip();
	}
}
