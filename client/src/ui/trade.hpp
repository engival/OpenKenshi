#pragma once
#include <cstdint>
#include <functional>
#include <string>

// Trading helpers (docs/UI.md "gear.*" trading; kenshi-inspect findings/ui_captures/trade_window.md). The trader's window is the Gear's container
// window with GearState::trade active; these are the pieces around it the host calls.
namespace ui
{
	class Prompt;

	namespace trade
	{
		std::string cats(int64_t n);	// "c.25,000", the game's money format ("-c.10" below zero)
		// The fence prompt before selling a stolen item: "FENCING ITEM", the chance of being caught (100 - the item's fencing chance, %), Yes / No.
		// done(true) on Yes; No, Escape and the close button answer false.
		void askFence(Prompt& p, int fencingChance, std::function<void(bool yes)> done);
	}
}
