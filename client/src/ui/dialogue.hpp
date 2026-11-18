#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The conversation screen (docs/UI.md), after the original's Kenshi_ConversationPanel.layout: the NPC's panel (name, portrait, what they say) above
// the player's panel (name, portrait, money, numbered reply choices). Like the Hud it knows nothing about SIMLINK or the dialogue data: the host fills
// a DialogueState every frame and turns DialogueActions into orders. No choices: a "(Leave)" row ends it. Keys 1-9 pick, Escape leaves.
// Speech bubbles over heads (Kenshi_SpeechPanel) are a separate, world-anchored thing, not this.
namespace ui
{
	struct DialogueState
	{
		bool active = false;	// false: hidden
		uint64_t conversation = 0;	// a new id resets the view (scroll, selection)
		std::string npcName, playerName;
		std::string npcText;	// the line said to the player, wrapped
		std::vector<std::string> choices;	// the player's replies, in order
		std::string money;	// e.g. "c. 1,250"; empty hides the money box
		PortraitRef npcPortrait, playerPortrait;	// rendered heads; none: the name's initial in a frame
		bool operator==(const DialogueState&) const = default;
	};

	struct DialogueActions
	{
		std::function<void(int index)> choose;	// a reply, index into choices
		std::function<void()> leave;		// Escape or "(Leave)"
	};

	class Dialogue
	{
	public:
		explicit Dialogue(DialogueActions actions);
		~Dialogue();
		void update(const DialogueState& s);	// every frame, before ui::frame (cheap when nothing changed)
		bool shown() const;
		// For scripted tests.
		Widget* choiceList() const;
		Widget* npcLabel() const;

	private:
		class Stage;
		DialogueActions act_;
		Stage* stage_ = nullptr;
		Label *npcName_ = nullptr, *playerName_ = nullptr, *npcText_ = nullptr, *money_ = nullptr;
		Button *npcPortrait_ = nullptr, *playerPortrait_ = nullptr;
		TextureImage *npcFace_ = nullptr, *playerFace_ = nullptr;
		Widget* moneyBox_ = nullptr;
		ScrollView *npcScroll_ = nullptr, *choiceScroll_ = nullptr;
		List* choices_ = nullptr;
		DialogueState last_;
		void pick(int i);
	};
}
