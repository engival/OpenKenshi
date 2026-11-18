#include "ksim/ai/goalsel.hpp"

#include <algorithm>
#include <cmath>

namespace ksim::goalsel {

bool windowHolds(std::uint16_t start, std::uint16_t end, double hour) noexcept
{
	const double h = hour - std::floor(hour / 24.0) * 24.0;
	if (start == end)
		return true; // always on (dt14 section 3; the data's 0-0 windows)
	if (end > 24)
		return true;
	if (start < end)
		return h >= start && h < end;
	return h < end || h >= start; // the wrap
}

double crowdFactor(int others, double decay) noexcept
{
	if (others <= 0)
		return 1.0;
	if (decay <= 0.0)
		return 0.0;
	return std::pow(decay, others); // the exe's blocks of 4 make the same product
}

double scoreOf(const Offer& offer, const Current& current, double hour) noexcept
{
	if (!windowHolds(offer.start, offer.end, hour))
		return 0.0;
	if (offer.utility <= 0.0)
		return 0.0;
	double score = offer.utility * offer.weight * crowdFactor(offer.others, offer.crowdDecay);
	const bool same = current.task != 0 && offer.task == current.task && offer.target == current.target;
	if (same)
	{
		score *= kCurrentBonus;
		// The hold: a floor while its minimum duration runs, a cap once it has expired, so a held goal cannot outlast its hold
		// against anything that can compete (dt14 section 5; the post-hold cap's comparison constant is UNSURE, so ours applies
		// to every score, dt9's recorded reading).
		if (offer.holds && !offer.hold.none())
			score = current.insideHold ? std::max(score, kHoldFloor) : std::min(score, kHoldCap);
	} else if (current.task != 0 && score < kDefaultScore) {
		// The running task stays competitive (dt14 section 4; M in the spec: a candidate scoring under 1.0 is boosted).
		score *= kCurrentBonus;
	}
	return score;
}

namespace {

// The trace's verdict per offer, after the pass: the same pure scoreOf the pass used.
void fillVerdicts(const std::vector<Offer>& offers, const Current& current, double hour, const Choice& choice, std::vector<Verdict>& out)
{
	out.assign(offers.size(), Verdict{});
	std::vector<double> scores(offers.size());
	for (std::size_t i = 0; i < offers.size(); ++i)
		scores[i] = scoreOf(offers[i], current, hour);
	// The winner is the one select picked: the last offer of the winning tier whose score is the choice's (ties go to the later).
	std::size_t winner = offers.size();
	if (choice.found)
		for (std::size_t i = 0; i < offers.size(); ++i)
			if (offers[i].tier == choice.tier && offers[i].task == choice.task && offers[i].target == choice.target && scores[i] == choice.score)
				winner = i;
	for (std::size_t i = 0; i < offers.size(); ++i)
	{
		const Offer& offer = offers[i];
		Verdict& verdict = out[i];
		verdict.score = scores[i];
		if (!windowHolds(offer.start, offer.end, hour))
			verdict.why = Why::OutOfWindow;
		else if (scores[i] <= 0.0)
			verdict.why = Why::NoTarget;
		else if (i == winner)
			verdict.why = Why::Chosen;
		else if (choice.found && offer.tier < choice.tier)
			verdict.why = Why::LowerTier;
		else if (current.task != 0 && offer.task == current.task && offer.target == current.target && offer.holds && !offer.hold.none()
			&& !current.insideHold)
			verdict.why = Why::HoldCapped;
		else
			verdict.why = Why::Outscored;
	}
}

} // namespace

const char* whyName(Why why) noexcept
{
	switch (why)
	{
	case Why::Chosen: return "Chosen";
	case Why::NoTarget: return "NoTarget";
	case Why::OutOfWindow: return "OutOfWindow";
	case Why::LowerTier: return "LowerTier";
	case Why::Outscored: return "Outscored";
	case Why::HoldCapped: return "HoldCapped";
	}
	return "?";
}

namespace {

Choice pass(const std::vector<Offer>& offers, const Current& current, double hour) noexcept
{
	for (int tier = 3; tier >= 1; --tier)
	{
		const Offer* best = nullptr;
		double bestScore = 0.0;
		for (const Offer& offer : offers)
		{
			if (offer.tier != tier)
				continue;
			const double score = scoreOf(offer, current, hour);
			if (score <= 0.0)
				continue;
			if (best == nullptr || score >= bestScore)
			{
				best = &offer;
				bestScore = score;
			}
		}
		if (best != nullptr)
			return Choice{true, false, best->task, bestScore, tier, best->target};
	}
	return Choice{false, true, kIdleTask, kDefaultScore, 0, 0};
}

} // namespace

Choice select(const std::vector<Offer>& offers, const Current& current, double hour, std::vector<Verdict>* verdicts) noexcept
{
	const Choice choice = pass(offers, current, hour);
	if (verdicts != nullptr)
		fillVerdicts(offers, current, hour, choice, *verdicts);
	return choice;
}

} // namespace ksim::goalsel
