// Task port-animals: the animal and food goals of the generic path - 261 SLEEP_ON_FLOOR_FAKE_AMBUSH (175's floor sleep, woken at rested),
// 259 EAT_FOOD_ON_GROUND, 277 ANIMAL_FETCH_A_LIMB, 278 PLAY_BECAUSE_I_HAVE_A_LIMB_IN_MOUTH, 279 CHASE_ALLY_DOGS_WITH_MOUTH_LIMBS and 238
// LOOT_ANIMALS_JOB. None is a package goal in the data, so the test names the goal through Simulation::orderAnimalGoal (the finder's own
// answer) or a Routine, as the 28 / 102 / 165 tests do. A bare dt27 world: no navmesh, so the member starts within reach of its target.

#include "dt27_rig.hpp"

#include "ksim/combat_data.hpp"
#include "ksim/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

// The first ITEM record of the merged data with this `item function`.
std::string itemOfFunction(Rig& rig, std::int32_t function)
{
	std::string best;
	for (const auto& [sid, info] : rig.data->items)
		if (info.function == function && (best.empty() || sid < best))
			best = sid;
	test::require(!best.empty(), "no item of function " + std::to_string(function));
	return best;
}

std::uint64_t spawnGround(Rig& rig, const std::string& sid, double x, double z)
{
	state::GroundItem item;
	item.itemSid = sid;
	item.x = item.useX = x;
	item.y = item.useY = rig.fighterOf(rig.member).y;
	item.z = item.useZ = z;
	return rig.sim.spawnGroundItem(std::move(item)).get<state::Id>()->value;
}

bool groundExists(Rig& rig, std::uint64_t id)
{
	bool found = false;
	ecsOf(rig.sim).each([&](const state::Id& key, const state::GroundItem&)
	{
		found = found || key.value == id;
	});
	return found;
}

state::Inventory& packOf(Rig& rig, std::uint64_t id)
{
	flecs::entity entity = rig.entityOf(id);
	if (entity.get<state::Inventory>() == nullptr)
		entity.set<state::Inventory>(state::Inventory{});
	return *entity.get_mut<state::Inventory>();
}

// A Rig whose member is an animal that is not full (277's gate) and has no other goal.
std::unique_ptr<Rig> animalRig()
{
	auto rig = std::make_unique<Rig>(std::vector<Row>{{routines::task::WorkTheSlaves}}, 0);
	rig->fighterOf(rig->member).animal = true;
	rig->fighterOf(rig->member).hunger = 1.0;
	return rig;
}

// 261: the Routine of an order or a plan; the sleeper lies down asleep, and wakes when the rolled floor-sleep hold runs out.
void floorSleeper()
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	state::Routine routine;
	routine.goal = routines::task::SleepOnFloorFakeAmbush;
	routine.since = rig.sim.clockTick();
	routine.nextScore = rig.sim.clockTick() + 100000000;
	rig.entityOf(rig.member).set<state::Routine>(std::move(routine));
	test::require(rig.until(3.0, [&] { return rig.fighterOf(rig.member).asleep; }), "261: the member never lay down");
	test::require(rig.goalOf(rig.member) == routines::task::SleepOnFloorFakeAmbush, "261: the goal ended as it lay down");
	test::require(rig.until(12.0 * kTicksPerGameHour * kTickSeconds, [&] { return rig.goalOf(rig.member) == 0; }), "261: the sleeper never woke");
	test::require(!rig.fighterOf(rig.member).asleep, "261: the sleeper woke but stays asleep");
	std::cout << "  261: slept on the floor and woke at rested\n";
}

// 259: ground food within 100 u is picked up and eaten; a dead body's food comes second.
void eatsGroundFood()
{
	auto rig = animalRig();
	const std::string food = itemOfFunction(*rig, kItemFunctionFood);
	state::Fighter& f = rig->fighterOf(rig->member);
	const std::uint64_t ground = spawnGround(*rig, food, f.x + 8.0, f.z);
	const double before = f.hunger;
	test::require(rig->sim.orderAnimalGoal(rig->member, routines::task::EatFoodOnGround), "259: the finder named no food");
	test::require(rig->goalOf(rig->member) == routines::task::EatFoodOnGround, "259: no Routine carries the goal");
	test::require(rig->until(30.0, [&] { return !groundExists(*rig, ground) && rig->goalOf(rig->member) == 0; }), "259: the food was not taken");
	test::require(rig->until(30.0, [&] { return rig->fighterOf(rig->member).hunger > before; }), "259: the member did not eat it");
	test::require(rig->logged("animal 259 member"), "259: no log line");
	// Nothing on the ground and no body: the finder names nothing.
	test::require(!rig->sim.orderAnimalGoal(rig->member, routines::task::EatFoodOnGround), "259: ordered with no food about");
	std::cout << "  259: picked up and ate ground food\n";

	// The body pass: no food on the ground, a corpse within 100 u holding some.
	auto second = animalRig();
	const std::uint64_t corpse = second->spawn(second->body, second->fighterOf(second->member).x + 8.0, second->fighterOf(second->member).z);
	packOf(*second, corpse).add(food, 1);
	second->fighterOf(corpse).dead = true;
	second->fighterOf(second->member).hunger = 1.0;
	second->tick(30); // the sensor list sees the body
	test::require(second->sim.orderAnimalGoal(second->member, routines::task::EatFoodOnGround), "259: the finder skipped a corpse with food");
	test::require(second->until(30.0, [&] { return second->goalOf(second->member) == 0; }), "259: the body goal never ended");
	test::require(second->logged("took food off body"), "259: the food was not taken off the body");
	std::cout << "  259: took food off a corpse\n";
}

// 277 + 278: a dog fetches a loose limb, then plays with it - the run's destination is re-picked 1000 - 1400 u away.
void fetchesAndPlays()
{
	auto rig = animalRig();
	const std::string limb = itemOfFunction(*rig, kItemFunctionLimb);
	state::Fighter& f = rig->fighterOf(rig->member);
	const std::uint64_t ground = spawnGround(*rig, limb, f.x + 8.0, f.z);
	test::require(!rig->sim.carriesLimb(rig->member), "277: the dog holds a limb at the start");
	test::require(rig->sim.orderAnimalGoal(rig->member, routines::task::AnimalFetchALimb), "277: the finder named no limb");
	test::require(rig->until(30.0, [&] { return rig->sim.carriesLimb(rig->member); }), "277: the dog never took the limb");
	test::require(!groundExists(*rig, ground), "277: the limb is still on the ground");
	test::require(rig->until(2.0, [&] { return rig->goalOf(rig->member) == 0; }), "277: the fetch did not end");
	// The cooldown (AI+0x318) holds the finder shut; a full dog fetches nothing either.
	spawnGround(*rig, limb, f.x + 5.0, f.z);
	test::require(!rig->sim.orderAnimalGoal(rig->member, routines::task::AnimalFetchALimb), "277: fetched again inside the cooldown");
	std::cout << "  277: fetched a loose limb\n";

	test::require(rig->sim.orderAnimalGoal(rig->member, routines::task::PlayWithLimbInMouth), "278: refused with a limb in the mouth");
	test::require(rig->until(5.0, [&] { return rig->logged("animal 278 member"); }), "278: no run destination was picked");
	f.x = rig->sim.limbPlayDestination(rig->member).first; // arrive: the next pass re-picks 1000 - 1400 u away
	f.z = rig->sim.limbPlayDestination(rig->member).second;
	test::require(rig->until(5.0, [&] { return rig->sim.limbPlayPicks(rig->member) >= 2; }), "278: no second destination on arrival");
	const auto [px, pz] = rig->sim.limbPlayDestination(rig->member);
	const double away = std::hypot(px - f.x, pz - f.z);
	test::require(away >= 1000.0 && away <= 1400.0, "278: the re-pick is " + std::to_string(away) + " u away, not 1000 - 1400");
	test::require(rig->goalOf(rig->member) == routines::task::PlayWithLimbInMouth, "278: the play ended while the limb is in the mouth");
	// No finish: it ends only when the limb is gone.
	packOf(*rig, rig->member).items.clear();
	test::require(rig->until(3.0, [&] { return rig->goalOf(rig->member) == 0; }), "278: the play kept going without a limb");
	std::cout << "  278: ran with the limb and re-picked its destination\n";
}

// 279: the finder names an ally animal with a limb in its mouth; the chaser ends within 50 u of it.
void chasesAllyDog()
{
	auto rig = animalRig();
	const std::string limb = itemOfFunction(*rig, kItemFunctionLimb);
	state::Fighter& me = rig->fighterOf(rig->member);
	test::require(!rig->sim.orderAnimalGoal(rig->member, routines::task::ChaseAllyDogs), "279: named a dog with no limb");
	const std::uint64_t dog = rig->spawn(rig->body, me.x + 30.0, me.z);
	rig->join(dog, 0);
	rig->fighterOf(dog).animal = true;
	packOf(*rig, dog).add(limb, 1);
	rig->tick(30); // the sensor list sees the dog
	test::require(rig->sim.orderAnimalGoal(rig->member, routines::task::ChaseAllyDogs), "279: the finder missed the ally with a limb");
	test::require(rig->until(10.0, [&] { return rig->goalOf(rig->member) == 0; }), "279: the chase did not end within 50 u of the dog");
	test::require(rig->logged("animal 279 member"), "279: no log line");
	std::cout << "  279: found the ally dog with a limb and closed on it\n";
}

// 238: the finder takes an ANIMAL body holding something my storage accepts (one of my machines makes it); the executor is 241's.
void lootsAnimal()
{
	auto rig = animalRig();
	test::require(!rig->sim.orderAnimalGoal(rig->member, routines::task::LootAnimalsJob), "238: ordered with no animal body about");
	const std::string faction = recordSid(*rig->gd, "FACTION", "Nameless");
	state::Allegiance side;
	side.factionSid = faction;
	rig->entityOf(rig->member).set<state::Allegiance>(side);
	const std::string resource = itemOfFunction(*rig, 0);
	state::Producer machine;
	machine.ownerSid = faction;
	machine.output = resource;
	machine.progress = 0.5; // mid-batch: stepProducers drops a pristine producer with no player near
	machine.x = machine.useX = kX;
	machine.z = machine.useZ = kZ;
	ecsOf(rig->sim).entity().set<state::Id>(state::Id{9000001}).set<state::Producer>(std::move(machine));
	state::Fighter& me = rig->fighterOf(rig->member);
	const std::uint64_t beast = rig->spawn(rig->body, me.x + 2.0, me.z);
	packOf(*rig, beast).add(resource, 2);
	rig->fighterOf(beast).dead = true;
	rig->tick(30);
	// A human corpse with the resource is not 238's (animals only); an animal's is.
	test::require(!rig->sim.orderAnimalGoal(rig->member, routines::task::LootAnimalsJob), "238: took a human body");
	rig->fighterOf(beast).animal = true; // the sensor list holds the corpse; what it is comes from the fighter
	test::require(rig->sim.orderAnimalGoal(rig->member, routines::task::LootAnimalsJob), "238: the finder missed the animal body");
	test::require(rig->goalOf(rig->member) == routines::task::LootAnimalsJob, "238: no Routine carries the goal");
	test::require(rig->until(15.0, [&] { return rig->goalOf(rig->member) == 0; }), "238: the loot did not finish");
	bool carried = false;
	for (const state::Inventory::Entry& entry : packOf(*rig, rig->member).items)
		carried = carried || (entry.itemSid == resource && entry.count > 0);
	test::require(carried, "238: the resource did not move to the looter");
	std::cout << "  238: looted an animal body for what my machines take\n";
}

} // namespace

int main()
{
	try
	{
		floorSleeper();
		eatsGroundFood();
		fetchesAndPlays();
		chasesAllyDog();
		lootsAnimal();
		std::cout << "ksim_animals: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_animals: FAILED: " << e.what() << "\n";
		return 1;
	}
}
