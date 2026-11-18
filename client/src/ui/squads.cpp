#include "ui/squads.hpp"

#include <algorithm>
#include <cmath>

#include "sokol_app.h"
#include "ui/overview.hpp"
#include "ui/prompt.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// Units, from the capture at 1440 lines (the window 1140 x 1037 px there).
		const float kCell = 80, kNameW = 300;
		const size_t kNameMax = 32;	// the original's setMaxTextLength(0x20)
		const Widget* dropOver_ = nullptr;	// the row or the DISMISS zone a drag is over: it shows the drop

		std::string shownName(const SquadMember& m) { return m.name.empty() ? "#" + std::to_string(m.id) : m.name; }

		// Press, then a drag past 5 px: shared by the portraits and the rows.
		struct DragTrack
		{
			float px = 0, py = 0;
			bool dragging = false, dragged = false;

			bool press(Btn b, float x, float y)
			{
				px = x;
				py = y;
				dragging = dragged = false;
				return b == Btn::kLeft;
			}

			bool start(float x, float y, float scale)	// true once, when the drag begins
			{
				if (dragging || std::hypot(x - px, y - py) < 5 * scale) { return false; }
				dragging = true;
				return true;
			}

			bool release()	// true when a drag ends (no click follows)
			{
				if (!dragging) { return false; }
				dragging = false;
				dragged = true;
				dropOver_ = nullptr;
				setDragImage({});
				return true;
			}
		};

		template <class T> T* above(Widget* w)
		{
			for (; w; w = w->parent()) { if (T* t = dynamic_cast<T*>(w)) { return t; } }
			return nullptr;
		}
	}

	// A squad's row: dragged by its background to reorder the squads; the drop target for rows and (with its strip) portraits.
	class Squads::Block : public Panel
	{
	public:
		uint64_t id = 0;
		std::function<void(float, float)> onDrop;

		Block() : Panel(Look::kInset, 8) { solid = true; }

		bool onPress(Btn b, float x, float y) override { return drag_.press(b, x, y); }

		void onDrag(float x, float y) override
		{
			if (drag_.start(x, y, scale_))
			{
				const float w = screen_.w, h = screen_.h, ox = drag_.px - screen_.x, oy = drag_.py - screen_.y, s = scale_;
				setDragImage([=](float x, float y)
				{
					const Rect r = { std::round(x - ox), std::round(y - oy), std::round(w), std::round(h) };
					style().draw(Look::kInset, r, s, State::kNormal, false, Color{ 1, 1, 1, 0.5f });
					gfx::frame(r, std::max(1.0f, std::round(2 * s)), res::colour("Accent"));
				});
			}
			if (drag_.dragging)
			{
				Block* over = above<Block>(root().hit(x, y));
				dropOver_ = over != this ? over : nullptr;
			}
		}

		void onRelease(Btn, float x, float y) override
		{
			if (drag_.release() && onDrop) { auto f = onDrop; f(x, y); }
		}

	protected:
		void paintOver() const override
		{
			if (dropOver_ == this) { gfx::frame(screen_, std::max(1.0f, std::round(2 * scale_)), res::colour("Accent")); }
		}

	private:
		DragTrack drag_;
	};

	// A row's portraits; empty: a plain grey strip, as the original's empty squad.
	class Squads::Strip : public Box
	{
	public:
		Strip() : Box(false, 4)
		{
			fit = false;
			halign = Align::kStretch;
			rect.h = kCell;
		}

	protected:
		void paint() const override
		{
			if (children().empty()) { gfx::fill(screen_, res::colour("SquadEmpty", { 0.35f, 0.35f, 0.35f, 0.35f })); }
		}
	};

	// DISMISS: a drop zone (the original's DismissCharPanel); a click does nothing.
	class Squads::Zone : public Widget
	{
	public:
		const Font* font = fonts::get("Kenshi_PaintedTextFont_Small");

		Zone()
		{
			solid = true;
			tooltip = "Drag a character here to dismiss them";
		}

		Size preferred() const override { return { 160, 36 }; }

	protected:
		void paint() const override
		{
			const State st = dropOver_ == this ? State::kHighlighted : State::kNormal;
			style().draw(Look::kButton, screen_, scale_, st, false);
			if (!font) { return; }
			const Rect a = style().textArea(Look::kButton, screen_, scale_, st, false);
			const std::string t = "DISMISS";
			font->draw(std::round(a.x + (a.w - font->width(t)) / 2), std::round(a.y + (a.h - font->lineHeight()) / 2), t, style().textColour(Look::kButton, st, false));
			if (dropOver_ == this) { gfx::frame(screen_, std::max(1.0f, std::round(2 * scale_)), res::colour("Accent")); }
		}
	};

	// A member's portrait with the name over its bottom edge. Drag and drop only: no hover look, no click.
	class Squads::Cell : public Widget
	{
	public:
		uint64_t id = 0;
		PortraitFace* layers;	// the rendered portrait and its layers
		Label* initial;
		Label* name;
		std::function<void(float, float)> onDrop;

		Cell()
		{
			solid = true;
			rect = { 0, 0, kCell, kCell };
			layers = &add<PortraitFace>();
			layers->rect = { 3, 3, 3, 3 };
			initial = &add<Label>("", "Kenshi_PaintedTextFont_Medium");
			initial->halign = initial->valign = Align::kCenter;
			name = &add<Label>("", "Kenshi_StandardFont_MediumSmall");
			name->shadow = true;
			name->halign = Align::kCenter;
			name->valign = Align::kEnd;
			name->rect = { 0, 2, 0, 0 };
		}

		void set(const SquadMember& m)
		{
			id = m.id;
			layers->face->set(m.portrait);
			layers->set({ m.mark, m.back, m.dim, m.hurt }, m.down);
			const std::string n = shownName(m);
			name->text = n;
			name->colour = m.down ? res::colour("BadBright") : res::colour("Main");
			initial->text = m.portrait.tex ? "" : n.substr(0, 1);
		}

		bool onPress(Btn b, float x, float y) override { return drag_.press(b, x, y); }

		void onDrag(float x, float y) override
		{
			if (drag_.start(x, y, scale_))	// the ghost: the portrait in a green frame
			{
				const float w = screen_.w, h = screen_.h, ox = drag_.px - screen_.x, oy = drag_.py - screen_.y, s = scale_;
				const TextureImage* face = layers->face;
				const dl::TexId tex = face->tex;
				float uv[4];
				std::copy(std::begin(face->uv), std::end(face->uv), uv);
				const std::string initialText = initial->text;
				const Font* f = initial->font;
				setDragImage([=](float x, float y)
				{
					const Rect r = { std::round(x - ox), std::round(y - oy), std::round(w), std::round(h) };
					style().draw(Look::kPortrait, r, s, State::kNormal, false);
					if (tex) { gfx::textured(tex, r.inset(std::round(3 * s)), uv[0], uv[1], uv[2], uv[3], Color{ 1, 1, 1, 0.85f }); }
					else if (f) { f->draw(std::round(r.x + (r.w - f->width(initialText)) / 2), std::round(r.y + (r.h - f->lineHeight()) / 2), initialText, res::colour("Main")); }
					gfx::frame(r, std::max(1.0f, std::round(2 * s)), res::colour("DropOk").alpha(1));
				});
			}
			if (drag_.dragging)
			{
				Widget* w = root().hit(x, y);
				dropOver_ = above<Zone>(w);
				if (!dropOver_) { dropOver_ = above<Block>(w); }
			}
		}

		void onRelease(Btn, float x, float y) override
		{
			if (drag_.release() && onDrop) { auto f = onDrop; f(x, y); }
		}

	protected:
		void paint() const override { style().draw(Look::kPortrait, screen_, scale_, State::kNormal, false); }

	private:
		DragTrack drag_;
	};

	Squads::Squads(Overview& home, SquadsActions actions) : act_(std::move(actions)), home_(home)
	{
		using A = Widget::Align;
		Widget& body = home_.pageWidget(OverviewPage::kSquads);
		empty_ = &body.add<Label>("No squads received yet");
		empty_->halign = empty_->valign = A::kCenter;
		Widget& content = body.add<Widget>();
		content.halign = content.valign = A::kStretch;
		body_ = &content;
		// ADD SQUAD and the count, top right; the rows below; DISMISS bottom right.
		Box& top = content.add<Box>(false, 24);
		top.halign = A::kEnd;
		top.rect = { 20, 0, 0, 0 };
		add_ = &top.add<Button>("ADD SQUAD");
		add_->onClicked = [this]
		{
			if (last_.maxSquads > 0 && (int)last_.squads.size() >= last_.maxSquads) { tell("Maximum number of squads reached."); }
			else if (act_.newSquad) { act_.newSquad(); }
		};
		used_ = &top.add<Label>("", "Kenshi_StandardFont_Medium");
		used_->valign = A::kCenter;
		list_ = &content.add<ScrollView>();
		list_->halign = list_->valign = A::kStretch;
		list_->rect = { 0, 52, 0, 52 };
		dismiss_ = &content.add<Zone>();
		dismiss_->halign = dismiss_->valign = A::kEnd;
		dismiss_->rect = { 20, 4, 0, 0 };
		prompt_ = std::make_unique<Prompt>();
		refresh();
	}

	Squads::~Squads()
	{
		prompt_.reset();
		if (ready()) { home_.pageWidget(OverviewPage::kSquads).clear(); }
	}

	bool Squads::shown() const { return home_.shown(OverviewPage::kSquads); }

	void Squads::show(bool on)
	{
		if (on) { home_.open(OverviewPage::kSquads); }
		else if (shown()) { home_.show(false); }
		refresh();
	}

	Widget* Squads::window() const { return home_.window(); }
	Widget* Squads::addButton() const { return add_; }
	Widget* Squads::dismissZone() const { return dismiss_; }
	Widget* Squads::used() const { return used_; }

	Widget* Squads::block(uint64_t squad) const
	{
		for (const Row& r : rows_) { if (r.id == squad) { return r.block; } }
		return nullptr;
	}

	Widget* Squads::strip(uint64_t squad) const
	{
		for (const Row& r : rows_) { if (r.id == squad) { return r.strip; } }
		return nullptr;
	}

	Widget* Squads::nameBox(uint64_t squad) const
	{
		for (const Row& r : rows_) { if (r.id == squad) { return r.name; } }
		return nullptr;
	}

	Widget* Squads::count(uint64_t squad) const
	{
		for (const Row& r : rows_) { if (r.id == squad) { return r.count; } }
		return nullptr;
	}

	Widget* Squads::deleteButton(uint64_t squad) const
	{
		for (const Row& r : rows_) { if (r.id == squad) { return r.del; } }
		return nullptr;
	}

	const PortraitFace* Squads::memberFace(uint64_t id) const
	{
		Widget* c = member(id);
		return c ? static_cast<Cell*>(c)->layers : nullptr;
	}

	Widget* Squads::memberMark(uint64_t id) const
	{
		Widget* c = member(id);
		return c ? static_cast<Cell*>(c)->layers->mark : nullptr;
	}

	Widget* Squads::member(uint64_t id) const
	{
		for (const Row& r : rows_)
		{
			for (Cell* c : r.cells) { if (c->id == id) { return c; } }
		}
		return nullptr;
	}

	void Squads::update(const SquadsState& s)
	{
		const bool on = shown();
		if (wasShown_ && !on)	// the overview closed or went to another page: an open question goes with it
		{
			if (prompt_->shown()) { prompt_->close(); }
			if (act_.close) { act_.close(); }
		}
		wasShown_ = on;
		if (s == last_) { return; }
		// Rows are rebuilt when squads or their members change (ids, order); names and counts are written in place.
		bool same = s.squads.size() == last_.squads.size();
		for (size_t i = 0; same && i < s.squads.size(); i++)
		{
			const SquadInfo &a = s.squads[i], &b = last_.squads[i];
			same = a.id == b.id && a.members.size() == b.members.size();
			for (size_t k = 0; same && k < a.members.size(); k++) { same = a.members[k].id == b.members[k].id; }
		}
		last_ = s;
		if (!same) { rebuild(); }
		refresh();
	}

	void Squads::rebuild()
	{
		using A = Widget::Align;
		Widget& list = list_->body();
		list.clear();
		rows_.clear();
		for (const SquadInfo& q : last_.squads)
		{
			Row r{ q.id, nullptr, nullptr, nullptr, nullptr, nullptr, {} };
			const uint64_t id = q.id;
			Block& b = list.add<Block>();
			b.id = id;
			b.halign = A::kStretch;
			b.onDrop = [this, id](float x, float y) { dropSquad(id, x, y); };
			r.block = &b;
			Box& col = b.add<Box>(true, 12);
			col.halign = A::kStretch;
			Widget& head = col.add<Widget>();
			head.halign = A::kStretch;
			head.rect.h = 36;
			r.name = &head.add<EditBox>();
			r.name->rect = { 0, 0, kNameW, 32 };
			r.name->maxBytes = kNameMax;
			r.name->onChange = [this, id](const std::string& name) { if (act_.rename) { act_.rename(id, name); } };	// live, as the original
			r.count = &head.add<Label>("", "Kenshi_StandardFont_Medium");
			r.count->halign = A::kEnd;
			r.count->valign = A::kCenter;
			r.count->rect = { 150, 0, 0, 0 };
			r.del = &head.add<Button>("DELETE");
			r.del->halign = A::kEnd;
			r.del->valign = A::kCenter;
			r.del->onClicked = [this, id]
			{
				const auto q = std::find_if(last_.squads.begin(), last_.squads.end(), [id](const SquadInfo& s) { return s.id == id; });
				if (q == last_.squads.end()) { return; }
				if (!q->members.empty()) { tell("The squad needs to be empty before it can be removed."); }
				else if (last_.squads.size() < 2) { tell("You must have at least one squad."); }
				else if (act_.remove) { act_.remove(id); }
			};
			r.strip = &col.add<Strip>();
			for (const SquadMember& m : q.members)
			{
				Cell& c = r.strip->add<Cell>();
				const uint64_t mid = m.id;
				c.onDrop = [this, mid](float x, float y) { dropMember(mid, x, y); };
				r.cells.push_back(&c);
			}
			rows_.push_back(std::move(r));
		}
	}

	void Squads::refresh()
	{
		const SquadsState& s = last_;
		empty_->visible = !s.valid;
		body_->visible = s.valid;
		used_->visible = s.factionMax > 0;
		used_->text = std::to_string(s.used) + "/" + std::to_string(s.factionMax);
		used_->colour = s.used >= s.factionMax ? res::colour("BadBright") : res::colour("Main");
		const Color ink = style().textColour(Look::kInset, State::kNormal, false);	// dark on the original's parchment
		for (size_t i = 0; i < rows_.size() && i < s.squads.size(); i++)
		{
			const SquadInfo& q = s.squads[i];
			Row& r = rows_[i];
			if (!focused(r.name)) { r.name->text = q.name; }
			r.count->text = std::to_string(q.members.size()) + "/" + std::to_string(q.max);
			r.count->colour = (int)q.members.size() >= q.max ? res::colour("SquadFull", ink) : ink;
			for (size_t k = 0; k < r.cells.size() && k < q.members.size(); k++) { r.cells[k]->set(q.members[k]); }
		}
	}

	void Squads::tell(const std::string& text) { prompt_->open({ "", text, { "Ok" } }, {}); }

	void Squads::dropMember(uint64_t member, float x, float y)
	{
		Widget* hit = root().hit(x, y);
		if (above<Zone>(hit))
		{
			askDismiss(member);
			return;
		}
		const Block* to = above<Block>(hit);
		const Cell* onto = above<Cell>(hit);
		if (!to || (onto && onto->id == member)) { return; }	// nowhere, or back onto itself
		const SquadInfo *from = nullptr, *dest = nullptr;
		for (const SquadInfo& q : last_.squads)
		{
			if (q.id == to->id) { dest = &q; }
			for (const SquadMember& m : q.members) { if (m.id == member) { from = &q; } }
		}
		if (!from || !dest) { return; }
		const bool shift = mods() & SAPP_MODIFIER_SHIFT;
		if (shift && onto)
		{
			if (act_.swapMembers) { act_.swapMembers(member, onto->id); }
			return;
		}
		// A full squad refuses silently, Shift or not: the sim's squad_move never goes past the max (the original lets Shift reach max + 1).
		// A reorder inside one is always allowed: the original's check has no such exemption, which reads as an oversight (squads13_static.md 1).
		if (from != dest && (int)dest->members.size() >= dest->max) { return; }
		int index = -1;
		for (size_t k = 0; onto && k < dest->members.size(); k++) { if (dest->members[k].id == onto->id) { index = (int)k; } }
		if (act_.moveMember) { act_.moveMember(member, dest->id, index); }
	}

	void Squads::dropSquad(uint64_t squad, float x, float y)
	{
		const Block* to = above<Block>(root().hit(x, y));
		if (!to || to->id == squad) { return; }
		for (size_t i = 0; i < last_.squads.size(); i++)
		{
			if (last_.squads[i].id == to->id && act_.moveSquad) { act_.moveSquad(squad, (int)i); }
		}
	}

	void Squads::askDismiss(uint64_t member)
	{
		std::string name;
		size_t everyone = 0;
		for (const SquadInfo& q : last_.squads)
		{
			everyone += q.members.size();
			for (const SquadMember& m : q.members) { if (m.id == member) { name = shownName(m); } }
		}
		if (name.empty()) { return; }
		if (everyone < 2)
		{
			tell("Cannot dismiss the only remaining character in the faction.");
			return;
		}
		prompt_->open({ "DISMISS CHARACTER", "Dismiss character '" + name + "'?", { "Yes", "No" } }, [this, member](int i)
		{
			if (i == 0 && act_.dismiss) { act_.dismiss(member); }
		});
	}
}
