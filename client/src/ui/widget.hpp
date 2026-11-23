#pragma once
#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/gfx.hpp"
#include "ui/font.hpp"
#include "ui/skin.hpp"
#include "ui/style.hpp"

// The retained widget tree (docs/UI.md). Geometry is authored in UI units (1 unit = 1 pixel on a 1080-line screen at ui.scale 100) and laid out into
// framebuffer pixels every frame. Screens own their widgets and update properties (text, checked, visible) as the sim changes.
namespace ui
{
	enum class Btn { kLeft, kRight, kMiddle };

	// One input of the control hints panel (ui::setHints, docs/UI.md "Control hints"): "Shift+Click" and what it does; a greyed line says why not.
	struct HintLine
	{
		std::string input;	// "Click", "Shift+Click", "Double click", "Right click", "Hold right"
		std::string label;	// "Add to the selection"
		bool enabled = true;
		std::string why;	// shown after the label on a greyed line
	};

	inline const std::string kSilent = "-";	// Button::sound / ui::sound: post nothing

	class Widget
	{
	public:
		// Placement in the parent's content area. rect.x / rect.y are the distance from the aligned edge (centre: offset from the centre);
		// stretched: rect.x / rect.y is the near margin and rect.w / rect.h the far one.
		enum class Align { kStart, kEnd, kCenter, kStretch };
		Rect rect;
		Align halign = Align::kStart, valign = Align::kStart;
		bool visible = true, enabled = true;
		bool solid = false;	// takes the mouse over its area: clicks and the wheel stop here instead of reaching the world
		bool clipChildren = false;
		bool hoverSound = false;	// the buttons at or under it post Hover on mouse-over (ui.hover_sounds=original: only the title menu; docs/UI.md "Sounds", kenshi-k16.3.20)
		std::string tooltip;

		Widget() = default;
		Widget(const Widget&) = delete;
		Widget& operator=(const Widget&) = delete;
		virtual ~Widget();

		template<class T, class... A> T& add(A&&... a)
		{
			auto w = std::make_unique<T>(std::forward<A>(a)...);
			T& r = *w;
			w->parent_ = this;
			children_.push_back(std::move(w));
			return r;
		}
		Widget& adopt(std::unique_ptr<Widget> w);
		std::unique_ptr<Widget> take(Widget* child);	// detached, still alive (null: not a child)
		void remove(Widget* child);
		void clear();
		void raise();	// last among its siblings: drawn over them and hit first
		void lower();	// first among its siblings: drawn under them
		Widget* parent() const { return parent_; }
		const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
		const Rect& screen() const { return screen_; }	// pixels, valid after layout
		bool shown() const;	// visible up the whole chain

		// Units the widget wants (boxes and fitted labels ask); default: its rect.
		virtual Size preferred() const { return { rect.w, rect.h }; }
		// Positions this widget inside the parent's content area (pixels) and lays out its children.
		void layout(const Rect& area, float scale);
		// Sets the pixel rect directly (boxes place their children), then lays out the children.
		virtual void place(const Rect& px, float scale);
		virtual Rect content() const { return screen_; }

		void draw() const;
		// Topmost solid, enabled, visible widget under (x, y), searching children last-first.
		virtual Widget* hit(float x, float y);

		// Input, pixels. onPress returning true captures the mouse until release (drag); clicks are reported on release inside the widget.
		virtual bool onPress(Btn, float, float) { return true; }
		virtual void onRelease(Btn, float, float) {}
		virtual void onDrag(float, float) {}
		virtual void onClick(Btn) {}
		virtual void onDoubleClick(Btn b) { onClick(b); }
		virtual bool onScroll(float) { return false; }
		virtual bool focusable() const { return false; }
		virtual bool onKey(int, bool) { return false; }
		virtual std::string tip() const { return tooltip; }	// the tooltip under the pointer (a List answers per row)
		// Its inputs for the control hints panel: its own lines (ownHints; default none), then keyHints. Not virtual: a widget lists its own in ownHints.
		virtual void ownHints(std::vector<HintLine>&) const {}
		void hints(std::vector<HintLine>& out) const { ownHints(out); out.insert(out.end(), keyHints.begin(), keyHints.end()); }
		// Host-set key rows (the keys that act on this widget, the input named as the host gives it: "1", "V", "Grave"; kenshi-5npx). The host
		// sets them each frame from the current bindings (screens take them through their State); the UI never names a key itself.
		std::vector<HintLine> keyHints;
		virtual bool onChar(unsigned) { return false; }
		virtual void onBlur() {}	// lost the keyboard focus

	protected:
		virtual void paint() const {}		// own visuals, below the children
		virtual void paintOver() const {}	// above the children
		State state() const;	// from hover / press / enabled
		float scale_ = 1;
		Rect screen_;

	private:
		Widget* parent_ = nullptr;
		std::vector<std::unique_ptr<Widget>> children_;
	};

	// A styled rectangle (Look::kNone: invisible); padding (units) insets its content area. A zero rect.w / rect.h fits that side to the children.
	class Panel : public Widget
	{
	public:
		explicit Panel(Look look = Look::kWindow, float padding = 0);
		Look look;
		Color tint;
		float padding;
		Size preferred() const override;
		Rect content() const override;

	protected:
		void paint() const override;
	};

	enum class TextAlign { kStart, kCenter, kEnd };

	// Text. fit sizes it to the text (wrapped to rect.w when wrap is set); shadow draws a dark copy one unit down-right.
	class Label : public Widget
	{
	public:
		Label(std::string text = {}, std::string font = "Kenshi_StandardFont_Medium");
		std::string text;
		const Font* font;
		Color colour;
		TextAlign align = TextAlign::kStart, vAlign = TextAlign::kCenter;
		bool wrap = false, fit = true, shadow = false;
		Size preferred() const override;

	protected:
		void paint() const override;
	};

	// Rich text, the tooltips' markup: a line per row, "{Colour}" (a palette name) at a line's start or anywhere in its value, "label\tvalue" with the value
	// right-aligned, an empty line for a gap; untabbed lines wrap. Rows point into the text, which must outlive the layout. Pixels.
	struct RichRow
	{
		std::string_view text, value;
		Color ink, valueInk;
		float h = 0;
	};

	struct RichLayout
	{
		std::vector<RichRow> rows;
		float w = 0, h = 0;	// the widest row (label + gap + value, or a wrapped line), the rows' total
	};

	// A row is rowH tall, a wrapped paragraph runs at the font's own spacing.
	RichLayout layoutRich(std::string_view text, const Font& f, float wrapW, float rowH, float gap, const Color& ink);
	void drawRich(const RichLayout& t, const Font& f, float x, float y, float w);	// values end at x + w

	// Rich text in the tree: as wide as its rect (or stretched), as tall as its rows.
	class RichText : public Widget
	{
	public:
		RichText(std::string text = {}, std::string font = "Kenshi_StandardFont_Medium");
		std::string text;
		const Font* font;
		Color ink;
		float rowH = 24, gap = 24;	// units
		Size preferred() const override;

	protected:
		void paint() const override;

	private:
		RichLayout layout(float w) const;
	};

	// Part of the atlas.
	class Image : public Widget
	{
	public:
		Image() = default;
		Image(const Rect& texels) : texels(texels) {}
		Image(const std::string& set, const std::string& index);	// gui/images image set entry
		Rect texels;
		Color tint;
		bool keepAspect = true;

	protected:
		void paint() const override;
	};

	// A screen-level window: a panel that takes the keyboard (focus lands here when one of its plain widgets is pressed) and reports Escape.
	// A left press anywhere in it (body, controls, border) raises it over its siblings before the press reaches the widget under it
	// (ui.cpp raiseOnPress, kenshi-k16.3.30.1).
	class Window : public Panel
	{
	public:
		explicit Window(Look look = Look::kWindow, float padding = 24) : Panel(look, padding) {}
		std::function<void()> onEscape;
		std::function<bool()> takesEscape;	// false: Escape passes the window by (to the host); unset: always taken
		std::function<bool(int key, bool down)> keyHook;	// a key while the window itself has the keyboard, before Escape: true takes it
		std::function<bool(unsigned c)> charHook;	// a typed character while the window itself has the keyboard: true takes it
		bool focusable() const override { return true; }
		bool onKey(int key, bool down) override;
		bool onChar(unsigned c) override { return charHook && charHook(c); }
	};

	// A styled button with centred text and/or an atlas icon. checkable buttons toggle `checked` on click (onToggle reports it).
	class Button : public Widget
	{
	public:
		Button(std::string text = {}, Look look = Look::kButton, std::string font = "Kenshi_PaintedTextFont_Small");
		Look look;
		std::string text;
		const Font* font;
		Rect icon;		// atlas texels; empty = none
		Color iconTint;
		Color textColour{ 0, 0, 0, 0 };	// alpha 0: the look's (a layout's own TextColour, e.g. a red X)
		bool checkable = false, checked = false;
		bool held = false;	// drawn pushed while enabled: a click the host holds until it can act on it (MainMenu::setPending)
		TextAlign align = TextAlign::kCenter;	// the text in the look's text area (kStart: from its left edge)
		std::function<void()> onClicked, onDoubleClicked;
		std::function<void(bool)> onToggle;
		std::function<void(Btn)> onOtherClick;	// right / middle clicks
		std::string clickHint, doubleClickHint;	// the control hints panel's labels for onClicked / onDoubleClicked ("" = not listed)
		// The sound a left click posts (ui::sound): empty = General_Click, an event name = that one, kSilent = none (the action posts its own).
		std::string sound;
		Size preferred() const override;
		void onClick(Btn b) override;
		void onDoubleClick(Btn b) override;
		void ownHints(std::vector<HintLine>& out) const override;

	protected:
		void paint() const override;
	};

	// A tick box (drawn at its natural size, vertically centred) with a label to its right; the whole row is clickable.
	class TickBox : public Button
	{
	public:
		TickBox(std::string text = {}, std::string font = "Kenshi_StandardFont_Medium");
		Size preferred() const override;

	protected:
		void paint() const override;
	};

	// A window in the original's two parts (Kenshi_WindowCX): a header bar with the caption centred and a close button at its right, over the
	// body box. Content goes into body(); the window is as wide as the wider of the caption (clear of the button) and the body. Escape and
	// the button call onClose (no button without one). A left drag on the header (not its buttons) raises and moves the window, as the
	// original's MyGUI windows: kept on the screen, snapped to an edge within 10 units (ui.windows_movable; movable false: fixed).
	class CaptionWindow : public Window
	{
	public:
		static constexpr float kHeaderH = 38;	// units, the template's
		explicit CaptionWindow(std::string caption = {}, std::function<void()> onClose = {}, float padding = 14);
		Label& caption() const { return *caption_; }
		Panel& body() const { return *body_; }
		Button& closeButton() const { return *close_; }
		Button& pinButton() const { return *pin_; }	// hidden unless shown; checked = pinned
		Button& targetButton() const { return *target_; }	// the item windows' quick right-click target (ours, kenshi-k16.20.13.1.1): hidden unless shown, beside the pin; checked = marked
		bool pinned() const { return pin_->visible && pin_->checked; }	// Escape leaves a pinned window open; its close button still closes it
		Size preferred() const override;
		void place(const Rect& px, float scale) override;	// adds the move, kept on the screen (a resize clamps it again)
		bool movable = true;	// false: the header doesn't drag it (the original's message box and advanced options)
		float moveX = 0, moveY = 0;	// units from where its screen lays it out; the window keeps them, so a reopen keeps the spot
		// Moves with lead from now on (the inventory's windows, user 2026-11-05): the lead's offset; a drag on any of them moves the group,
		// kept on the screen as one box.
		void follow(CaptionWindow& lead);
		void unfollow();	// moves on its own again (from where its screen lays it out)
		void raiseGroup();	// over its siblings with its lead and the lead's followers, itself last (a header drag; any left press: ui.cpp)
		~CaptionWindow() override;	// leaves its lead's group; its followers move on their own
		// A grip at the bottom-right corner resizes it (press, drag, release: a ray pointer works too). The size is its laid-out one plus
		// (sizeW, sizeH), at least minSize (a zero side: the laid-out size), at most the UI's area; the body fills it, so its content should
		// stretch (a Flex). Session only, like the move.
		bool resizable = false;
		Size minSize;	// units
		float sizeW = 0, sizeH = 0;	// units added to the laid-out size
		Widget& grip() const { return *grip_; }

	private:
		class Header;
		class Grip;
		void resizeTo(float w, float h);	// units, the whole window; clamped and snapped
		CaptionWindow& anchor() { return lead_ ? *lead_ : *this; }
		Rect extent() const;	// the group's visible laid-out rects joined (a lead's), pixels
		Rect shift(const Rect& box, float x, float y, float snap) const;	// box moved by (x, y) units, kept on the screen
		void moveTo(float x, float y);	// a lead's, units; snapped and clamped
		Rect base_;	// the laid-out rect before the move (resized), pixels
		Size laid_;	// the laid-out size before the resize, units
		CaptionWindow* lead_ = nullptr;
		std::vector<CaptionWindow*> group_;	// a lead's followers
		Panel* header_;
		Label* caption_;
		Button *close_, *pin_, *target_;
		Panel* body_;
		Widget* grip_;
	};

	// A horizontal slider (after the game's Kenshi_ScrollBar): step arrows at the ends, a track and a thumb, all scaled to the widget's height. Drag
	// or click the track, use the arrows or the wheel; onChange reports each new value (clamped to [min, max] and rounded to step).
	class Slider : public Widget
	{
	public:
		Slider(double min = 0, double max = 100, double step = 1);
		double min, max, step, value;
		// Notches (user 2026-11-05): a drag that crosses one holds the thumb on it until the pointer is kNotchPull units past where it
		// crossed, then lets go; coming back across it holds again. Ticks under the track mark them. Clicks, arrows and the wheel ignore them.
		std::vector<double> notches;
		static constexpr float kNotchPull = 14;
		std::function<void(double)> onChange;
		Size preferred() const override;
		bool onPress(Btn, float x, float y) override;
		void onRelease(Btn, float, float) override { arrow_ = 0; held_ = false; }
		void onDrag(float x, float y) override;
		bool onScroll(float dy) override;
		void setValue(double v);	// as the user would: onChange when it changed
		static constexpr double kSoundGap = 0.05;	// at most one Slider sound per this many seconds (deferred: k16.3.17)

	protected:
		void paint() const override;

	private:
		int arrow_ = 0;	// the arrow held down: -1 left, 1 right
		bool held_ = false;	// on a notch, at heldNotch_, since the pointer was at heldX_
		double heldNotch_ = 0, lastRaw_ = 0;
		float heldX_ = 0;
		bool dragged_ = false;	// lastRaw_ is this drag's
		double lastSound_ = -1;
		void userSet(double v);	// a change by the user: setValue, then the Slider sound
		float arrowW(bool right) const;
		float thumbW() const;
		Rect trackPx() const;	// the thumb's travel, pixels
	};

	// A scroll bar (the map's, after Kenshi_ScrollViewMap): step arrows at the ends (square, the bar's thickness), a track and a thumb; vertical
	// or horizontal. value in [min, max]; page is the span shown (the thumb's share of the track: page / (max - min + page)). The thumb drags,
	// an arrow steps, a track click pages; onChange reports each new value. The owner keeps min / max / page / value in step with its view.
	class ScrollBar : public Widget
	{
	public:
		explicit ScrollBar(bool vertical = true);
		bool vertical;
		double min = 0, max = 1, value = 0, page = 0.1, step = 0.05;
		std::function<void(double)> onChange;
		std::function<void()> onDone;	// a press released (after the onChange calls it made: an arrow, the track, a thumb drag)
		Rect thumb() const;	// pixels
		Rect arrow(bool far) const;	// pixels: up / left, or down / right
		bool onPress(Btn b, float x, float y) override;
		void onDrag(float x, float y) override;
		void onRelease(Btn, float, float) override
		{
			grab_ = -1;
			if (onDone) { onDone(); }
		}

	protected:
		void paint() const override;

	private:
		Rect track() const;
		void set(double v);
		float grab_ = -1;	// the pointer's offset into the thumb while it drags, pixels; -1: none
	};

	// A one-line text entry. Click to place the caret; typing, Backspace / Delete, Left / Right / Home / End (Ctrl: a word), a selection
	// (Shift + those keys, a drag, Shift + click, Ctrl+A, a double click: all) that typing, paste, Backspace and Delete replace; Ctrl+C / X / V
	// with the system clipboard (when the host enables it); the keypad navigates while NumLock is off (kenshi-k16.3.18). Enter or losing focus
	// commits (onCommit, when the text changed since focus); Escape restores it. While focused it takes every key but F1..F25, so typing never
	// reaches the world.
	class EditBox : public Widget
	{
	public:
		explicit EditBox(std::string text = {}, std::string font = "Kenshi_StandardFont_Medium");
		std::string text;
		size_t maxBytes = 256;
		const Font* font;
		std::function<void(const std::string&)> onChange, onCommit;
		std::function<void()> onEnter;	// Enter pressed (after any onCommit), changed or not: a form's default action
		bool keepsKeyboard = false;	// Enter calls onEnter and the box keeps the keyboard (a find field: Enter again for the next)
		std::function<bool()> onEscape;	// Escape pressed: true takes it (no revert, the focus stays unless it moves it); unset or false: revert and leave
		std::string hint;	// drawn dim while the text is empty
		Size preferred() const override;
		bool focusable() const override { return true; }
		bool onPress(Btn, float x, float y) override;
		void onDrag(float x, float y) override;
		void onDoubleClick(Btn b) override;
		bool onKey(int key, bool down) override;
		bool onChar(unsigned c) override;
		void onBlur() override;
		std::string selected() const;	// the selected text ("" when none)
		size_t caret() const { return caret_; }
		void select(size_t from, size_t to);	// byte offsets, clamped; the caret at `to`
		// The clipboard, set by the host through ui::setClipboardFns (scripted tests set their own); unset: no clipboard.
		static inline std::function<std::string()> clipboardGet;
		static inline std::function<void(const std::string&)> clipboardSet;

	protected:
		void paint() const override;

	private:
		size_t caret_ = 0;	// byte offset
		size_t anchor_ = 0;	// the selection's other end; == caret_: none
		// A keypad key-down navigates; if its digit (NumLock on) comes as a char in the same frame, the state before it is put back and the digit typed.
		struct Kp { std::string text; size_t caret, anchor; double at; };
		std::optional<Kp> kp_;
		std::string start_;	// text when focus came
		bool editing_ = false;
		mutable float scroll_ = 0;	// pixels the text is shifted left to keep the caret in view
		void begin();
		void finish(bool keep);
		void insert(std::string_view s);
		bool eraseSelection();	// true when there was one
		size_t at(float x) const;	// the code point boundary nearest x
		void changed();
		Rect textArea() const;
	};

	// A closed list (Look::kInput, an arrow at its right) showing the picked item, else the placeholder dimmed. A click opens a Menu of the items
	// under it, at least as wide; a pick sets selected, then calls onPick. The host may set items / selected at any time (no onPick).
	class Menu;
	class Dropdown : public Button
	{
	public:
		explicit Dropdown(std::string placeholder = {}, std::string font = "Kenshi_StandardFont_Medium");
		std::vector<std::string> items;
		int selected = -1;	// -1: none, the placeholder shows
		std::string placeholder;
		std::function<void(int)> onPick;
		Size preferred() const override;
		void onClick(Btn b) override;
		Menu* menu() const { return menu_; }	// the menu it last opened (scripted tests): valid only while ui::popupsOpen()

	protected:
		void paint() const override;

	private:
		Menu* menu_ = nullptr;
	};

	// Children in a row or column, in order, ignoring their rect.x/y; the cross axis follows each child's align (stretch fills).
	class Box : public Widget
	{
	public:
		explicit Box(bool vertical, float spacing = 4, float padding = 0) : vertical(vertical), spacing(spacing), padding(padding) {}
		bool vertical;
		float spacing, padding;
		bool fit = true;	// size to the children (else the rect's size)
		Size preferred() const override;
		void place(const Rect& px, float scale) override;
	};

	class ScrollView;

	// A tab bar, header only (docs/UI.md "Tabs"): the screen shows the page in onSelect. kTop: a strip of text tabs, the selected one bright
	// over an accent underline, the others dimmed; wider than its rect, it shows < > arrows at its right end that step the first tab shown
	// (so does the wheel). kLeft: a list, a row per tab with an accent bar on the selected one, optional group headings; it scrolls when
	// longer than its rect. It lays out into the rect it is given. The original style draws the tabs with Kenshi_Button1Skin.
	class Tabs : public Widget
	{
	public:
		enum class Side { kTop, kLeft };
		explicit Tabs(Side side = Side::kTop, std::string font = "Kenshi_StandardFont_Medium");
		const Side side;
		float tabH = 32;	// units: the strip's height (kTop), a row's (kLeft)
		float gap = 2;	// units between tabs
		std::function<void(size_t)> onSelect;	// select(), or a click on another tab
		size_t add(const std::string& caption, const std::string& tip = {});
		void heading(const std::string& text);	// kLeft: a group heading before the next tab; kTop: none
		void clear();
		void select(size_t i);
		size_t selected() const { return selected_; }
		size_t count() const { return tabs_.size(); }
		Button* tab(size_t i) const { return i < tabs_.size() ? tabs_[i] : nullptr; }
		Button* arrow(bool next) const { return next ? next_ : prev_; }	// kTop
		size_t first() const { return first_; }	// kTop: the first tab shown
		Size preferred() const override;
		void place(const Rect& px, float scale) override;
		bool onScroll(float dy) override;

	protected:
		void paint() const override;

	private:
		float width(size_t from, size_t to) const;	// units: tabs [from, to) side by side
		std::string font_;
		ScrollView* list_ = nullptr;	// kLeft
		Button *prev_ = nullptr, *next_ = nullptr;	// kTop
		std::vector<Button*> tabs_;
		size_t selected_ = 0, first_ = 0;
		bool reveal_ = false;	// kTop: bring the selected tab into view at the next layout
	};

	// How a Flex child takes the main axis: its basis (or its preferred size), clamped to [min, max], then a fill share of what is left.
	struct FlexItem
	{
		float basis = -1;	// units; < 0: the child's preferred size (a fill child: 0)
		float fill = 0;	// weight in the space left over; 0: none
		float min = 0, max = 1e9f;	// units
	};

	// A row or column that shares its length (docs/UI.md "Flex"): fixed, preferred and fill children with min / max, padding, a gap, the
	// children's cross align (kStretch fills across), and justify for what no fill child takes. It lays out into the rect it is given and
	// reads nothing else (no screen size). Its preferred size is the least it needs: fill children at their basis / min.
	class Flex : public Widget
	{
	public:
		explicit Flex(bool vertical = false, float gap = 0, float padding = 0) : vertical(vertical), gap(gap), padding(padding) {}
		bool vertical;
		float gap, padding;
		Align justify = Align::kStart;	// kStart, kCenter or kEnd: where the children sit when none fills
		template<class T, class... A> T& add(FlexItem it, A&&... a)
		{
			T& w = Widget::add<T>(std::forward<A>(a)...);
			items_.push_back({ &w, it });
			return w;
		}
		using Widget::add;	// plain add: FlexItem{} (the preferred size)
		void set(const Widget& child, FlexItem it);
		FlexItem item(const Widget& child) const;
		Size preferred() const override;
		void place(const Rect& px, float scale) override;

	private:
		float start(const Widget& c, const FlexItem& it) const;	// units: the basis, unclamped
		float base(const Widget& c, const FlexItem& it) const;	// units: the basis within [min, max]
		std::vector<std::pair<const Widget*, FlexItem>> items_;
	};

	// Tabs over pages (after the original's Kenshi_TabControlEmpty: Kenshi_Button1 tabs on a 35-unit header). add() makes a tab and its page;
	// the first is selected, only the selected page shows. A page fills the area under the header (its rect / aligns as in any parent). A
	// header-only use (Options: one view rebuilt per tab) leaves the pages empty and listens to onSelect.
	class TabView : public Widget
	{
	public:
		explicit TabView(float buttonW = 140, float headerH = 35);
		float buttonW, headerH;	// buttonW 0: each tab fits its caption; else that wide, or narrower to share the header
		float gap = 4;	// between the tabs, and between the header and the page
		Align tabAlign = Align::kStart;	// the tab row in the header: kStart, kCenter or kEnd
		std::function<void(size_t)> onSelect;	// a tab picked (by click or select())
		Widget& add(const std::string& caption);
		void clearTabs();
		void select(size_t i);
		size_t selected() const { return selected_; }
		size_t count() const { return tabs_.size(); }
		Button* tab(size_t i) const { return i < tabs_.size() ? tabs_[i] : nullptr; }
		Widget* page(size_t i) const { return i < pages_.size() ? pages_[i] : nullptr; }
		Size preferred() const override;
		void place(const Rect& px, float scale) override;

	private:
		Box* header_;
		std::vector<Button*> tabs_;
		std::vector<Widget*> pages_;
		size_t selected_ = 0;
	};

	// Vertical scrolling of one content widget (usually a column Box), clipped; the wheel scrolls; a thin bar shows the position. The bar's
	// column takes the mouse: drag the thumb, or click above / below it for a page. While it shows, the content keeps 14 units clear of the
	// right edge (the 6-unit column and an 8-unit gap).
	class ScrollView : public Widget
	{
	public:
		ScrollView();
		float offset = 0;	// units scrolled
		bool stickToEnd = false;	// stays at the bottom while it was there (logs)
		Widget& body() { return *children().front(); }
		void place(const Rect& px, float scale) override;
		bool onScroll(float dy) override;
		bool onPress(Btn b, float x, float y) override;
		void onDrag(float x, float y) override;
		void onRelease(Btn, float, float) override { grab_ = -1; }
		void scrollToEnd() { toEnd_ = true; }
		// Keeps the body's child-th widget whole in view (scrolling the least) at every layout, until the user scrolls (wheel, bar) or
		// another reveal: one early layout (the window not yet its size, rows not yet laid out) can't leave it short.
		void reveal(size_t child) { reveal_ = (int)child; }
		Rect thumb() const;	// pixels; empty when nothing overflows (for scripted tests)

	protected:
		void paintOver() const override;

	private:
		bool overflows() const { return contentH_ > screen_.h / scale_; }
		float contentH_ = 0;	// units
		float grab_ = -1;	// pixels from the thumb's top while dragging it
		bool toEnd_ = false, atEnd_ = true;
		int reveal_ = -1;
	};

	// Selectable rows of wrapped text (dialogue choices, lists): hover highlight, onPick on click, onActivate on double click.
	class List : public Widget
	{
	public:
		explicit List(std::string font = "Kenshi_StandardFont_Medium");
		std::vector<std::string> items;
		int selected = -1;
		const Font* font;
		Color colour, hoverColour, selectedColour;
		bool numbered = false;	// "1. " prefixes; keys 1-9 pick (when focused)
		float padX = 3, padY = 3;	// units around each row's text
		float rowMin = 0;		// units: rows at least this tall, the text centred (the dialogue's replies)
		std::function<void(int)> onPick, onActivate;
		std::vector<Color> itemColours;	// per row, optional (shorter, or alpha 0 = colour): the row's text keeps it when hovered or selected
		std::vector<std::string> itemTips;	// per row, optional; else tooltip
		std::string tip() const override;
		Size preferred() const override;
		bool onPress(Btn, float, float y) override;
		void onClick(Btn b) override;
		void onDoubleClick(Btn b) override;
		bool focusable() const override { return numbered; }
		bool onKey(int key, bool down) override;

	protected:
		void paint() const override;

	private:
		struct Row { float y, h; std::vector<std::string_view> lines; std::string label; };
		mutable std::vector<Row> rows_;
		int pressRow_ = -1;
		void build(float widthPx) const;
		int rowAt(float y) const;
	};

	// Rows under column headers (after the original's Kenshi_MultiListBox: a 30-unit header over a sunken list), one line per cell, cut at the
	// column's edge. A header click sorts by that column (again: reversed); the sort is stable and keeps the selected row. Rows are named by id,
	// which survives setRows and sorting. Click selects (onSelect), double click or Enter activates (onActivate), Delete asks onDelete. Up / Down /
	// Page Up / Page Down / Home / End move the selection while focused; other keys go to the nearest focusable parent (a window's Escape). The
	// wheel scrolls; a thin bar shows the position.
	class Table : public Widget
	{
	public:
		struct Column
		{
			std::string title;
			float width = 0;	// units; 0 shares what the others leave
			TextAlign align = TextAlign::kStart;
		};
		struct Row
		{
			std::string id;
			std::vector<std::string> cells;
			std::vector<double> keys;	// per column, optional: sorts by number instead of text (dates); NaN or missing: by the text
			Color colour{ 0, 0, 0, 0 };	// alpha 0 (the default): the table's
			std::string tip;	// else the table's tooltip
		};
		explicit Table(std::vector<Column> columns = {}, std::string font = "Kenshi_StandardFont_Medium");
		std::vector<Column> columns;
		const Font *font, *headerFont;
		Color colour, hoverColour, selectedColour;
		float headerH = 30, rowH = 28, padX = 8;	// units
		std::function<void(const std::string&)> onSelect, onActivate, onDelete;
		std::function<void(int, bool)> onSort;	// column, descending

		void setRows(std::vector<Row> rows);	// re-sorted by the current column; the selection stays on its id (gone: none)
		const std::vector<Row>& rows() const { return rows_; }	// in display order
		int selected() const { return selected_; }	// display index, -1 none
		std::string selectedId() const { return selected_ >= 0 ? rows_[selected_].id : std::string(); }
		void select(const std::string& id);	// "" clears; scrolls it into view; onSelect when it changed
		void sortBy(int column, bool descending);	// -1: the order rows came in (from the next setRows)
		int sortColumn() const { return sortCol_; }
		bool sortDescending() const { return sortDesc_; }
		float offset = 0;	// units scrolled

		bool focusable() const override { return true; }
		bool onPress(Btn, float x, float y) override;
		void onClick(Btn b) override;
		void onDoubleClick(Btn b) override;
		bool onScroll(float dy) override;
		bool onKey(int key, bool down) override;
		std::string tip() const override;

	protected:
		void paint() const override;

	private:
		std::vector<Row> rows_;
		int selected_ = -1, sortCol_ = -1, pressRow_ = -1, pressCol_ = -1, clickRow_ = -1;	// clickRow_: the row the last click picked
		bool sortDesc_ = false;
		std::vector<float> columnX(float w) const;	// pixel edges, columns.size() + 1 of them from 0
		Rect body() const;
		float maxOffset() const;
		int rowAt(float x, float y) const;
		int columnAt(float x, float y) const;	// a header cell, -1 elsewhere
		void sort();
		void pick(int i, bool notify);
		void reveal(int i);
	};

	// Coloured, wrapped lines, oldest dropped past maxLines; scrolls, sticks to the newest line.
	class TextLog : public ScrollView
	{
	public:
		explicit TextLog(std::string font = "Kenshi_StandardFont_Medium", size_t maxLines = 200);
		void push(std::string text, const Color& c);
		void clearLines();

	private:
		class Lines;
		Lines* lines_;
	};

	// A popup menu of text items. Shown with ui::openMenu; closes on a pick, a press outside or Escape. Held mode (hold-RMB menus): the item under
	// the pointer when the button is released is picked.
	class Menu : public Panel
	{
	public:
		// title: a header row (the target's name): painted font, centred, a rule under it; never highlighted or picked.
		// value: a short column beside the text (ContextMenu: a chance, "45%"); empty = none.
		struct Item { std::string text; std::function<void()> action; bool enabled = true; bool title = false; std::string value = {}; };
		explicit Menu(std::vector<Item> items, std::string font = "Kenshi_StandardFont_Medium");
		std::vector<Item> items;
		const Font* font;
		const Font* titleFont;
		bool escapeCloses = true;	// false: Escape passes the menu by (ContextMenu, as the original's)
		bool keepOnMiss = false;	// held: a release on no pickable row leaves it open for a click (ItemMenu)
		bool pickable(int i) const { return i >= 0 && i < (int)items.size() && items[i].enabled && !items[i].title; }
		Size preferred() const override;
		bool onPress(Btn, float, float) override { return true; }
		void onClick(Btn) override;
		virtual int itemAt(float y) const;
		virtual int pickAt(float x, float y) const { (void)x; return itemAt(y); }	// the row a release or click at (x, y) picks; -1 none
		void pick(int i);

	protected:
		void paint() const override;
	};

	// The hold-RMB order menu, after Kenshi_ContextMenu.layout and kinspect's live measure (findings/ui_captures/ctxmenu.md; 720 lines: 160
	// wide, title 23, rows 25, 1 under the last; the button 154 x 22 at +3, +1, or 126 wide on every row once any row has a Value, the Value
	// 30 wide at +130): an eighth of the screen wide on the floating panel (Look::kPanel), never wider (long captions are cut); a title row is
	// the target's name on a flat plate (NameText); each order a Kenshi_Button1, its caption in capitals, gold under the mouse; the Value
	// ("29%") red -> yellow -> green with the number (ctx18_static: (1 - s, s, 0), s = value / 100; not a number: the accent colour). No
	// Escape, no greyed rows (every row is enabled). Opened with ui::openContextMenu, or ui::openMenu.
	class ContextMenu : public Menu
	{
	public:
		static constexpr float kWidth = 0.125f;	// of the screen's width, whatever the labels (240 units at 16:9)
		static constexpr float kTitleH = 34.5f, kRowH = 37.5f, kFootH = 1.5f, kValueW = 45, kInsetX = 4.5f, kInsetY = 1.5f;
		explicit ContextMenu(std::vector<Item> items);
		Size preferred() const override;
		int itemAt(float y) const override;
		int pickAt(float x, float y) const override;	// on a row's button only: the title, the margins and the Value pick nothing
		Rect row(size_t i) const;	// the item's button (its plate for a title), in pixels; for scripted tests
		static Color valueColour(const std::string& value);	// "45%": its colour

	protected:
		void paint() const override;

	private:
		float top(size_t i) const;	// the row's top, in units from the menu's
		bool values() const;	// any row has a Value: every button narrows
	};

	// Messages stacked downwards from the widget's top, each fading out at the end of its time (ui::now: UI time, never game time). A text
	// equal to a shown one bumps its count ("text (x5)") and restarts its time instead of stacking; past maxShown the oldest leaves early
	// (kenshi-k16.2.300.5.2).
	class Toasts : public Widget
	{
	public:
		static constexpr double kFade = 0.6;	// the last part of a message's time, fading out
		explicit Toasts(std::string font = "Kenshi_StandardFont_Large");
		const Font* font;
		size_t maxShown = 3;
		float bubbleAlpha = 0.8f;	// the black behind each message, before its fade
		void push(std::string text, const Color& c, double seconds);
		void clear() { msgs_.clear(); }
		bool empty() const;	// nothing showing
		size_t shown() const;	// the messages showing
		int count(const std::string& text) const;	// how many pushes a showing message stands for; 0: not showing
		const std::vector<Rect>& bubbles() const { return bubbles_; }	// the last paint's bubbles, top down (pixels; for scripted tests)

	protected:
		void paint() const override;

	private:
		struct Msg { std::string text; Color colour; double at, until; int count; };	// at: the last push (the oldest leaves first)
		void prune() const;	// drops the expired
		mutable std::deque<Msg> msgs_;
		mutable std::vector<Rect> bubbles_;
	};

	// A host texture region (a rendered portrait: Portraits::get), comparable for the screens' state structs.
	struct PortraitRef
	{
		dl::TexId tex = 0;	// the host's id (dl::gpu::bind carries its view and sampler); 0: none
		float uv[4] = { 0, 0, 1, 1 };	// u0, v0 (top), u1, v1 (bottom)
		bool operator==(const PortraitRef& o) const { return tex == o.tex && std::equal(std::begin(uv), std::end(uv), std::begin(o.uv)); }
	};

	// A mark over a portrait's face (prison bars, shackle, eaten): the original's Kenshi_PortraitImage FrontOverlay group. Game state, not
	// skin, so every style draws it. One per member; the host picks which when several apply (kenshi-rwh.112).
	enum class PortraitMark { kNone, kPrison, kSlave, kEaten };
	// Its atlas art (gui/images/kenshi_images.xml); empty for kNone or when the image set is missing.
	Rect portraitMarkArt(PortraitMark m);
	// The layer behind the face (the original's BackImage, BackOverlay group: red stripes in combat, yellow when hungry). One per member, the
	// host's precedence (the original: Combat > Hunger).
	enum class PortraitBack { kNone, kCombat, kHunger };
	Rect portraitBackArt(PortraitBack b);

	// The game state drawn on a portrait (kenshi-inspect findings/rwh112_portrait_marks.md); the host decides each.
	struct PortraitLayers
	{
		PortraitMark mark = PortraitMark::kNone;	// over the face: caged (kPrison), wears shackles (kSlave), kEaten; the original: Eaten > Prison > Slave
		PortraitBack back = PortraitBack::kNone;	// behind the face
		bool dim = false;	// the face at alpha 0.25 (the original: carried, in a bed or put in something); the marks and background stay full
		bool hurt = false;	// the Hurt background (a red grid) pulsing in and out, 0.5 s a sweep
		bool operator==(const PortraitLayers&) const = default;
	};

	// A texture region the host owns (a rendered portrait), unlike Image's atlas part: drawn over its rect, never takes the mouse. tex 0: nothing.
	class TextureImage : public Widget
	{
	public:
		void set(const PortraitRef& p) { tex = p.tex; std::copy(std::begin(p.uv), std::end(p.uv), uv); }
		dl::TexId tex = 0;
		float uv[4] = { 0, 0, 1, 1 };	// u0, v0 (top), u1, v1 (bottom)
		Color tint;

	protected:
		void paint() const override;
	};

	// The inside of a portrait frame, bottom to top as the original's Kenshi_PortraitCharacter.layout: the Hurt background (painted here),
	// the back overlay, the face (the host's render), the front mark. Stretched over its parent; set its rect for the inset. Takes no mouse.
	class PortraitFace : public Widget
	{
	public:
		PortraitFace();
		Image* back;
		TextureImage* face;
		Image* mark;
		void set(const PortraitLayers& l, bool down);	// down: the face greyed (KO or dead)
		float hurtAlpha() const;	// the Hurt background's alpha now; 0 when not hurt

	protected:
		void paint() const override;

	private:
		PortraitLayers layers_;
		Rect hurtArt_;
	};

	// The zone banner (Kenshi_MainPanel.layout BiomeTextPanel): a big name between two rules across the upper middle, fading in and out.
	// Shown on entering a zone ("The Great Desert"); a new name replaces the one up.
	class Banner : public Widget
	{
	public:
		explicit Banner(std::string font = "Kenshi_BannerTextFont");
		const Font* font;
		void show(std::string text, double seconds);
		void clear() { text_.clear(); }
		bool shown() const;	// still up

	protected:
		void paint() const override;

	private:
		std::string text_;
		double from_ = 0, until_ = 0;
	};

	// The HUD message box (Kenshi_MainPanel.layout MessagePanel, content Kenshi_MessagePanel.layout): one message, wrapped and centred in a panel
	// at the bottom centre ("You can't afford that."). A new message replaces the one up; it fades out over its last 0.6 s.
	class HudMessage : public Widget
	{
	public:
		explicit HudMessage(std::string font = "Kenshi_StandardFont_Large");
		const Font* font;
		static constexpr double kFade = 0.33;	// after the hold
		// Queued behind the one up (the original's message roller: FIFO, one at a time); a text already up or queued is dropped. Empty: clear.
		void show(std::string text, const Color& c, double seconds);
		const std::string& text() const;	// the message up, empty when none (for scripted tests)
		size_t queued() const;	// waiting behind it
		float alpha() const;	// 0 when gone
		void clear() { msgs_.clear(); }

	protected:
		void paint() const override;

	private:
		struct Msg { std::string text; Color colour; double seconds, until = 0; };	// until: set when it comes up
		mutable std::deque<Msg> msgs_;	// [0] up
		void advance() const;	// drops the faded one, brings up the next
	};
}
