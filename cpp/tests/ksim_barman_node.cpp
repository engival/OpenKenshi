// The Hub bar's barman stands at the layout's Node Shopkeeper marker, behind the counter, not on the customers' side (kenshi-k16.6.106):
// the counter piece is where shoppers walk to, the shopkeeper's slot is the marker (ShopkeeperNode). The Shop Counter is at x -50558.7 and the
// markers' x is about -50549 (the data); the test lets the Bar blackmarket barman settle and requires him east of the counter and near a marker.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "trade_rig.hpp"

#include <cmath>
#include <cstdio>

namespace
{

constexpr double kCounterX = -50558.7; // the Shop Counter's centre
constexpr double kMarkerX = -50549.0; // the Node Shopkeeper points

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_barman_node HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const std::string saveDir = ks::prepareSaveDir(std::string(KSIM_BINARY_DIR) + "/ksim_barman_node_test/saves");
		Rig r(order, gd, data, nav, saveDir);
		r.newGame("Nobodies");
		std::uint64_t barman = 0;
		r.ecs().each([&](flecs::entity e, const state::SquadInstance& sq, const state::TraderStock&, const state::Position& at)
		{
			const kdata::Record* record = gd->find(sq.templateSid);
			if (record == nullptr || record->name != "Bar blackmarket" || std::hypot(at.x + 50560.0, at.z + 1190.0) > 100.0)
				return;
			r.ecs().query_builder<const state::Id>().with<state::MemberOf>(e).build().each([&](flecs::entity, const state::Id& id)
			{
				barman = barman == 0 ? id.value : barman;
			});
		});
		test::require(barman != 0, "the Hub's Bar blackmarket squad has a barman");
		r.tick(1200); // the walk to the post is over well before
		const state::Fighter& at = r.fighter(barman);
		std::printf("barman at (%.1f, %.1f)\n", at.x, at.z);
		test::require(at.x > kCounterX + 3.0, "the barman stands east of the counter, behind it (not on the customers' side)");
		test::require(std::fabs(at.x - kMarkerX) < 8.0, "the barman stands within 8 u (x) of the Node Shopkeeper markers");
	} catch (const std::exception& e) {
		std::fprintf(stderr, "ksim_barman_node: %s\n", e.what());
		return 1;
	}
	return 0;
}
