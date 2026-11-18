#pragma once

// The debug view's decision sink (docs/formats/SIMLINK.md "DEBUG VIEW"): AiSystem::Config::decision, set by the server when
// --debug-view is on. It keeps the last goal pick per squad in debugview's own map (not saved, not in the ECS) for
// DEBUG_TREE. Compiled only with KSIM_DEBUG_VIEW.
// The per-member decision trace (Simulation::setDecisionTrace, decision_trace.hpp) also lands here: a ring of the last 8 picks per
// member, read by DEBUG_QUERY kind 1's decisions subtree.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/decision_trace.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace ksim::debugview {

using DecisionSink = std::function<void(std::uint64_t, const GoalChoice&, const std::vector<GoalChoice>&)>;

#if KSIM_DEBUG_VIEW
// The sink to install in AiSystem::Config::decision.
DecisionSink decisionSink();
// The Sink to give Simulation::setDecisionTrace: every pick of a member `wants` (null = all) goes into the ring, then to `also`
// (null = nothing; the server's --trace-member log).
trace::Sink traceSink(std::function<bool(std::uint64_t)> wants = {}, std::function<void(const trace::Pick&)> also = {});
#endif

} // namespace ksim::debugview
