#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// A modal question (docs/UI.md): a CaptionWindow with wrapped text over a row of buttons, centred over a dimmed backdrop that takes the mouse
// until it closes. For the fence prompt ("You have a {1}% chance of getting caught selling this stolen item.\nFence it anyway?", kenshi-inspect
// inv_click10_static.md) and later confirmations. Text is rich text (layoutRich: "{Colour}", "label\tvalue" rows, wrapped paragraphs).
namespace ui
{
	struct PromptSpec
	{
		std::string title;	// the caption
		std::string text;
		std::vector<std::string> buttons = { "OK" };	// left to right
		int enter = 0;		// the button Enter picks; < 0 none
		int escape = -1;	// the button Escape and the caption's close pick; < 0 the last
	};

	class Prompt
	{
	public:
		Prompt();
		~Prompt();
		// Shows the question; done gets the picked button's index once, after the prompt has closed. A prompt already open is replaced, its
		// done not called.
		void open(PromptSpec spec, std::function<void(int)> done);
		void close();	// without an answer; the keys go back to the screen that had them
		bool shown() const;
		// For scripted tests.
		Widget* window() const;
		Widget* button(size_t i) const { return i < buttons_.size() ? buttons_[i] : nullptr; }
		Widget* text() const;	// a RichText

	private:
		class Backdrop;
		class Frame;
		Backdrop* backdrop_ = nullptr;
		Frame* win_ = nullptr;
		RichText* text_ = nullptr;
		Box* row_ = nullptr;
		std::vector<Button*> buttons_;
		PromptSpec spec_;
		std::function<void(int)> done_;
		Widget* before_ = nullptr;	// the screen (a child of root()) that had the keys when it opened: it gets them back
		void pick(int i);
	};
}
