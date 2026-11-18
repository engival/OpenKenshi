// Task ds/small-tasks: the planner rows of the small tasks (3, 12, 14, 17, 19, 24, 26, 28, 30, 31, 37, 47, 51, 62, 69, 102, 104, 105, 159, 164, 165,
// 231, 232, 238, 259, 261, 277, 278, 279), from the registry facts of dt24_plantasks / dt24_47 / dt24_eat / dt24_131. Pure: a staged world of
// symbols and the real task registry. The rows are orderOnly, so the chains that already have providers for their symbols keep their plans.

#include "ksim/ai/goap.hpp"
#include "ksim/ai/routines.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ksim;
using goap::Sym;
using V = std::vector<std::uint16_t>;

class Stage final : public goap::World
{
public:
	explicit Stage(std::set<Sym> held) : held_(std::move(held)) {}
	bool holds(Sym sym, std::uint64_t, double x, double z) const override
	{
		if (sym == Sym::AtLocation)
			return std::hypot(x - 500.0, z - 500.0) <= 4.0;
		return held_.count(sym) != 0;
	}

private:
	std::set<Sym> held_;
};

V planOf(const std::set<Sym>& held, std::uint16_t task, double x = 500.0, double z = 500.0)
{
	V out;
	for (const goap::Step& step : goap::plan(Stage(held), task, 7, x, z).steps)
		out.push_back(step.task);
	return out;
}

void require(const V& got, const V& want, const std::string& what)
{
	std::string text;
	for (const std::uint16_t t : got)
		text += std::to_string(t) + " ";
	test::require(got == want, what + " (plan: " + text + ")");
}

} // namespace

int main()
{
	try
	{
		// Bare Task_Blank / talk / stalker nodes: the plan is the node.
		for (const std::uint16_t node : {3, 12, 14, 26, 28, 31, 47, 62, 69, 102, 105, 159, 165, 232, 261, 278, 279})
			require(planOf({}, node), V{node}, "node " + std::to_string(node));

		// goal isAtLocation: the walk (67) is the provider when the actor is away.
		const std::set<Sym> mobile{Sym::IsMovementAllowed};
		for (const std::uint16_t task : {19, 24, 30})
		{
			require(planOf(mobile, task), V{task}, "at the spot, " + std::to_string(task));
			require(planOf(mobile, task, 0.0, 0.0), (V{goap::kRunning, task}), "away, " + std::to_string(task));
		}
		// 51 needs isAtLocation itself.
		require(planOf(mobile, 51), V{51}, "51 at the node");
		require(planOf(mobile, 51, 0.0, 0.0), (V{goap::kRunning, 51}), "51 away from the node");
		// 37 is peaceful: a drawn weapon is sheathed first (7).
		require(planOf({}, 37), V{37}, "37 unarmed");
		require(planOf({Sym::HasWeaponEquipped}, 37), (V{routines::task::UnequipWeapon, 37}), "37 armed sheathes first");
		// 17 needs both an aggressor and a patient; nothing provides them.
		require(planOf({Sym::IsAggressionTowards, Sym::NeedsFirstAid}, 17), V{17}, "17 with both");
		test::require(planOf({Sym::IsAggressionTowards}, 17).empty(), "17 planned without a patient");
		// 104 is a deliberate failure: nothing provides sym 20, so there is no plan (the goal re-plans next tick, never a held state).
		test::require(planOf({}, 104).empty(), "104 planned");
		require(planOf({Sym::Impossible}, 104), V{104}, "104 when the symbol is staged");
		// 164 follows once near; 231 needs a farm with food; 238 needs room; 259 / 277 need a food item.
		require(planOf({Sym::IsWithin50Meters}, 164), V{164}, "164 near the target");
		require(planOf({Sym::FarmHasFood}, 231), V{231}, "231 at a farm with food");
		test::require(planOf({}, 231).empty(), "231 planned without a farm");
		require(planOf({Sym::GotInventoryRoomForResources}, 238), V{238}, "238 with room");
		test::require(planOf({}, 238).empty(), "238 planned with a full pack");
		for (const std::uint16_t task : {259, 277})
			require(planOf({Sym::GotAFoodItem}, task), V{task}, "food item " + std::to_string(task));
		std::cout << "ksim_small_tasks: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_small_tasks: FAILED: " << e.what() << "\n";
		return 1;
	}
}
