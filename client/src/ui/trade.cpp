#include "ui/trade.hpp"

#include <algorithm>

#include "ui/prompt.hpp"

namespace ui::trade
{
	std::string cats(int64_t n)
	{
		std::string digits = std::to_string(n < 0 ? -n : n), out;
		for (size_t i = 0; i < digits.size(); i++)
		{
			if (i > 0 && (digits.size() - i) % 3 == 0) { out += ','; }
			out += digits[i];
		}
		return (n < 0 ? "-c." : "c.") + out;
	}

	void askFence(Prompt& p, int fencingChance, std::function<void(bool yes)> done)
	{
		const int caught = 100 - std::clamp(fencingChance, 0, 100);
		p.open({ "FENCING ITEM", "You have a " + std::to_string(caught) + "% chance of getting caught selling this stolen item.\nFence it anyway?", { "Yes", "No" } },
			[done = std::move(done)](int i) { if (done) { done(i == 0); } });
	}
}
