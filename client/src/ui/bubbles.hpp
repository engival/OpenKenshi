#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui/widget.hpp"

// Speech bubbles over characters' heads (the original's Kenshi_SpeechPanel; kenshi-inspect findings/speech_bubble.md): wrapped, centred text on the
// speech skin, under every other widget and never taking the mouse. The host projects each speaker's head (Bip01 Head + 4 up) every frame and passes
// the lines being said (SIMLINK event 41), one per speaker, for the speech timer. As in the original: fitted to the text up to 450 units wide (wrapped
// at 385), growing upwards from its bottom centre, no line cap; no time fade (the host's distance alpha only); bubbles overlap freely; only an
// important one is kept inside the screen. Interjections look the same.
namespace ui
{
	struct SpeechBubble
	{
		uint64_t id = 0;	// the speaker
		std::string text;
		float x = 0, y = 0;	// framebuffer pixels: the bubble's bottom centre
		float alpha = 1;	// the host's distance fade, saturate((2500 - dist x factor) / 1500); 0: not drawn
		bool large = false;	// the large-font events (Kenshi_StandardFont_Large)
		bool important = false;	// the important events (the large ones too): clamped inside the screen; others may go off it
	};

	class SpeechBubbles
	{
	public:
		SpeechBubbles();
		~SpeechBubbles();
		void update(std::vector<SpeechBubble> bubbles);	// every frame, before ui::frame; a bubble is gone when not passed
		// For tests: where a bubble was last drawn (pixels), its wrapped lines and alpha; empty / 0 when not shown.
		Rect placed(uint64_t id) const;
		int lines(uint64_t id) const;
		float alpha(uint64_t id) const;

	private:
		class Layer;
		Layer* layer_ = nullptr;
	};
}
