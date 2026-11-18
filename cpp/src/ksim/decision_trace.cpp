#include "ksim/decision_trace.hpp"

#include <sstream>

namespace ksim::trace {

Pick makePick(std::uint64_t tick, std::uint64_t member, Source source, const goalsel::Current& running,
	const std::vector<goalsel::Offer>& offers, const std::vector<goalsel::Verdict>& verdicts)
{
	Pick pick;
	pick.tick = tick;
	pick.member = member;
	pick.source = source;
	pick.runningTask = running.task;
	pick.runningTarget = running.target;
	pick.hold = running.task == 0 ? Hold::None : running.insideHold ? Hold::Inside : Hold::Expired;
	pick.candidates.reserve(offers.size());
	for (std::size_t i = 0; i < offers.size(); ++i)
	{
		const goalsel::Offer& offer = offers[i];
		pick.candidates.push_back({offer.task, offer.tier, offer.target, offer.utility, offer.weight,
			goalsel::crowdFactor(offer.others, offer.crowdDecay), verdicts[i].score, verdicts[i].why});
	}
	return pick;
}

const char* sourceName(Source source) noexcept
{
	return source == Source::WorldSelector ? "WorldSelector" : source == Source::TownScorer ? "TownScorer" : "Combat";
}

const char* holdName(Hold hold) noexcept
{
	return hold == Hold::None ? "None" : hold == Hold::Inside ? "Inside" : "Expired";
}

const char* resultName(Result result) noexcept
{
	return result == Result::Chosen ? "Chosen" : result == Result::DefaultIdle ? "DefaultIdle" : "Fallback";
}

std::string formatPick(const Pick& pick)
{
	std::ostringstream line;
	line << "trace tick " << pick.tick << " member " << pick.member << " " << sourceName(pick.source) << " running " << pick.runningTask
		<< " hold " << holdName(pick.hold) << " -> " << resultName(pick.result) << " " << pick.task << " target " << pick.target
		<< " score " << pick.score << " tier " << pick.tier;
	for (const Candidate& c : pick.candidates)
		line << " | " << c.task << " t" << c.tier << " u" << c.utility << " w" << c.weight << " c" << c.crowd << " s" << c.score << " "
			<< goalsel::whyName(c.verdict);
	return line.str();
}

} // namespace ksim::trace
