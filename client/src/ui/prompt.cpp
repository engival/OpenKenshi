#include "ui/prompt.hpp"

#include "sokol_app.h"

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const float kW = 520, kPad = 18;	// the window's width and body padding (units)
	}

	// Covers the screen under the prompt: dims it and swallows the mouse. A click on it gives the keyboard back to the prompt.
	class Prompt::Backdrop : public Widget
	{
	public:
		Widget* owner = nullptr;

		Backdrop()
		{
			solid = true;
			halign = valign = Align::kStretch;
		}

		void onClick(Btn) override { if (owner) { focus(owner); } }

	protected:
		void paint() const override { gfx::fill(screen_, Color{ 0, 0, 0, 0.35f }); }
	};

	// Enter picks the default button; Escape goes through onEscape (the caption's close).
	class Prompt::Frame : public CaptionWindow
	{
	public:
		std::function<void()> onEnter;
		using CaptionWindow::CaptionWindow;

		bool onKey(int key, bool down) override
		{
			if (key != SAPP_KEYCODE_ENTER && key != SAPP_KEYCODE_KP_ENTER) { return CaptionWindow::onKey(key, down); }
			if (down && onEnter) { auto f = onEnter; f(); }
			return true;
		}
	};

	Prompt::Prompt()
	{
		using A = Widget::Align;
		backdrop_ = &root().add<Backdrop>();
		backdrop_->visible = false;
		win_ = &root().add<Frame>("", [this] { pick(spec_.escape >= 0 ? spec_.escape : (int)buttons_.size() - 1); }, kPad);
		win_->movable = false;	// as the original's Kenshi_MessageBox
		backdrop_->owner = win_;
		win_->halign = win_->valign = A::kCenter;
		win_->rect.w = kW;
		win_->visible = false;
		win_->onEnter = [this] { if (spec_.enter >= 0 && spec_.enter < (int)buttons_.size()) { pick(spec_.enter); } };
		Box& body = win_->body().add<Box>(true, 16);
		body.halign = A::kStretch;
		text_ = &body.add<RichText>();
		text_->rect.w = kW - 2 * kPad;
		text_->ink = style().textColour(Look::kWindow, State::kNormal, false);
		row_ = &body.add<Box>(false, 10);
		row_->halign = A::kCenter;
	}

	Prompt::~Prompt()
	{
		if (!ready()) { return; }
		if (win_) { root().remove(win_); }
		if (backdrop_) { root().remove(backdrop_); }
	}

	bool Prompt::shown() const { return win_->visible; }
	Widget* Prompt::window() const { return win_; }
	Widget* Prompt::text() const { return text_; }

	void Prompt::open(PromptSpec spec, std::function<void(int)> done)
	{
		spec_ = std::move(spec);
		done_ = std::move(done);
		if (spec_.buttons.empty()) { spec_.buttons = { "OK" }; }
		win_->caption().text = spec_.title;
		text_->text = spec_.text;
		row_->clear();
		buttons_.clear();
		for (size_t i = 0; i < spec_.buttons.size(); i++)
		{
			Button& b = row_->add<Button>(spec_.buttons[i]);
			b.onClicked = [this, i] { pick((int)i); };
			buttons_.push_back(&b);
		}
		if (!win_->visible)
		{
			// The screen, not the widget: a screen lives as long as its owner, a widget inside may be rebuilt while the prompt is up.
			before_ = focusedWidget();
			while (before_ && before_->parent() != &root()) { before_ = before_->parent(); }
		}
		backdrop_->visible = win_->visible = true;
		backdrop_->raise();
		win_->raise();
		focus(win_);
	}

	void Prompt::close()
	{
		backdrop_->visible = win_->visible = false;
		done_ = nullptr;
		Widget* back = before_;
		before_ = nullptr;
		bool alive = false;
		for (const auto& c : root().children()) { alive = alive || c.get() == back; }
		if (focused(win_)) { focus(alive && back->shown() && back->focusable() ? back : nullptr); }
	}

	void Prompt::pick(int i)
	{
		auto done = std::move(done_);
		close();
		if (done) { done(i); }
	}
}
