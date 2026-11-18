#include "ui/dialogue.hpp"

#include "sokol_app.h"
#include "ui/ui.hpp"

namespace ui
{
	// The whole screen, not solid (the world stays clickable around the panels); it holds the keyboard while a conversation runs.
	class Dialogue::Stage : public Widget
	{
	public:
		Dialogue* owner = nullptr;
		bool focusable() const override { return true; }
		bool onKey(int key, bool down) override
		{
			if (key == SAPP_KEYCODE_ESCAPE)
			{
				if (down && owner->act_.leave) { auto f = owner->act_.leave; f(); }
				return true;
			}
			return owner->choices_->onKey(key, down);	// 1-9
		}
	};

	namespace
	{
		std::string initial(const std::string& name) { return name.empty() ? "?" : name.substr(0, 1); }

		// The speaker's name above each panel's text: plain text, as Dark UI draws Kenshi_GenericTextBoxFlat.
		Label& nameBox(Widget& parent, float x, float w)
		{
			Panel& p = parent.add<Panel>(Look::kNone, 0);
			p.rect = { x, 6, w, 38 };
			Label& l = p.add<Label>("", "Kenshi_StandardFont_Large");
			l.fit = false;
			l.halign = l.valign = Widget::Align::kStretch;
			l.rect = { 10, 0, 10, 0 };
			l.colour = res::colour("DialogueLabel");
			return l;
		}

		// Kenshi_ConversationPanel's portrait ImageBox: 128 x 128 at (25, 10) in either panel, no frame. Until the host has a rendered head:
		// the frame with the name's initial, as the Hud does.
		Button& portrait(Widget& parent, TextureImage*& face)
		{
			Button& b = parent.add<Button>("", Look::kPortrait, "Kenshi_PaintedTextFont_Medium");
			b.rect = { 25, 10, 128, 128 };
			b.solid = false;
			face = &b.add<TextureImage>();
			face->halign = face->valign = Widget::Align::kStretch;
			return b;
		}

		void showPortrait(Button& frame, TextureImage& face, const PortraitRef& p, const std::string& name)
		{
			face.set(p);
			frame.look = p.tex ? Look::kNone : Look::kPortrait;
			frame.text = p.tex ? "" : initial(name);
		}

	}

	Dialogue::Dialogue(DialogueActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		Stage& st = root().add<Stage>();
		stage_ = &st;
		st.owner = this;
		st.halign = st.valign = A::kStretch;
		st.visible = false;
		const Color ink = res::colour("DialogueText"), title = res::colour("Title");	// Dark UI's greys (capture darkui_dialogue_0943); StandardFont_Large

		// NPC panel (the original: 0.25 0.60 0.48 0.14 of a 1920 x 1080 screen).
		Panel& npc = st.add<Panel>(Look::kPanel, 0);
		npc.halign = A::kCenter;
		npc.valign = A::kEnd;
		npc.rect = { -15, 280, 930, 150 };
		npcPortrait_ = &portrait(npc, npcFace_);
		npcName_ = &nameBox(npc, 150, 300);
		Panel& npcBox = npc.add<Panel>(Look::kInset, 12);
		npcBox.rect = { 150, 48, 762, 92 };
		npcScroll_ = &npcBox.add<ScrollView>();
		npcScroll_->halign = npcScroll_->valign = A::kStretch;
		npcText_ = &npcScroll_->body().add<Label>("", "Kenshi_StandardFont_Large");
		npcText_->wrap = true;
		npcText_->rect.w = 724;
		npcText_->colour = ink;

		// Player panel (0.354 0.75 0.29 0.25), at the bottom.
		Panel& pl = st.add<Panel>(Look::kPanel, 0);
		pl.halign = A::kCenter;
		pl.valign = A::kEnd;
		pl.rect = { 0, 0, 555, 272 };
		playerPortrait_ = &portrait(pl, playerFace_);
		playerName_ = &nameBox(pl, 150, 220);
		Panel& money = pl.add<Panel>(Look::kNone, 0);	// "Money:" and the amount under the portrait, plain text
		money.rect = { 14, 140, 128, 36 };
		moneyBox_ = &money;
		money_ = &money.add<Label>("", "Kenshi_StandardFont_Large");
		money_->fit = false;
		money_->halign = money_->valign = A::kStretch;
		money_->align = TextAlign::kEnd;
		money_->rect = { 8, 0, 8, 0 };
		money_->colour = res::colour("DialogueLabel");
		Label& moneyLabel = money.add<Label>("Money:", "Kenshi_StandardFont_Large");
		moneyLabel.valign = A::kCenter;
		moneyLabel.rect.x = 8;
		moneyLabel.colour = res::colour("DialogueLabel");
		Panel& choiceBox = pl.add<Panel>(Look::kInset, 12);
		choiceBox.rect = { 150, 48, 390, 212 };
		choiceScroll_ = &choiceBox.add<ScrollView>();
		choiceScroll_->halign = choiceScroll_->valign = A::kStretch;
		choices_ = &choiceScroll_->body().add<List>("Kenshi_StandardFont_Large");
		choices_->halign = A::kStretch;
		choices_->numbered = true;
		choices_->colour = res::colour("DialogueChoice");
		choices_->padX = 16;	// the game's replies: indented, 28 units apart
		choices_->rowMin = 28;
		choices_->hoverColour = title;
		choices_->selectedColour = title;
		choices_->onPick = [this](int i) { pick(i); };
	}

	Dialogue::~Dialogue()
	{
		if (ready() && stage_) { root().remove(stage_); }
	}

	bool Dialogue::shown() const { return stage_->visible; }
	Widget* Dialogue::choiceList() const { return choices_; }
	Widget* Dialogue::npcLabel() const { return npcText_; }

	void Dialogue::pick(int i)
	{
		choices_->selected = -1;	// a reply is an action, not a selection
		if (last_.choices.empty()) { if (act_.leave) { auto f = act_.leave; f(); } return; }
		if (i < 0 || i >= (int)last_.choices.size()) { return; }
		sound("Dialogue_Choice", "Dialogue_Choice", "Continue_Conversation");	// the original's Continue button (140683c00)
		if (act_.choose) { auto f = act_.choose; f(i); }
	}

	void Dialogue::update(const DialogueState& s)
	{
		if (stage_->visible && !focusedWidget()) { focus(stage_); }	// a click in the world must not take 1-9 away mid-conversation
		if (s == last_) { return; }
		const bool opened = s.active && !last_.active, newTalk = s.conversation != last_.conversation;
		const bool newLine = s.npcText != last_.npcText || s.choices != last_.choices;
		last_ = s;
		stage_->visible = s.active;
		if (!s.active)
		{
			if (focused(stage_)) { focus(nullptr); }
			return;
		}
		if (opened || newTalk)
		{
			stage_->raise();
			focus(stage_);
		}
		npcName_->text = s.npcName;
		playerName_->text = s.playerName;
		showPortrait(*npcPortrait_, *npcFace_, s.npcPortrait, s.npcName);
		showPortrait(*playerPortrait_, *playerFace_, s.playerPortrait, s.playerName);
		npcText_->text = s.npcText;
		moneyBox_->visible = !s.money.empty();
		money_->text = s.money;
		choices_->items = s.choices;
		if (s.choices.empty()) { choices_->items = { "(Leave)" }; }
		if (newLine || newTalk)
		{
			npcScroll_->offset = 0;
			choiceScroll_->offset = 0;
			choices_->selected = -1;
		}
	}
}
