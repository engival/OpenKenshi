// Ground-blood trackers (ksim/combat/bloodtrail.hpp; SIMLINK events 2, 16, 17): a KO'd bleeder lying still grows a
// monotonic pool capped at 2; a dragged one smears; a standing one that does not move sends no drops; a walking
// one does; the pool restarts after a move; the dead freeze; determinism.

#include "ksim/combat/bloodtrail.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

using ksim::BloodEvent;
using ksim::BloodInput;
using ksim::BloodTrail;

std::vector<BloodEvent> run(BloodTrail& t, BloodInput c, std::uint64_t from, std::uint64_t ticks, double dx = 0.0)
{
	std::vector<BloodEvent> out;
	for (std::uint64_t k = 0; k < ticks; ++k)
	{
		t.observe(c, from + k, out);
		c.x += dx;
	}
	return out;
}

void testPool()
{
	BloodTrail t;
	BloodInput c;
	c.id = 7;
	c.x = 100;
	c.z = 50;
	c.rate = 0.3;
	c.unconscious = true;
	double last = 0.0;
	int sent = 0;
	// 6 simulated hours is far past the cap (about 5 h at 0.3).
	for (std::uint64_t base = 0; base < 20 * 3600 * 6; base += 20)
	{
		const std::vector<BloodEvent> ev = run(t, c, base, 20);
		for (const BloodEvent& e : ev)
		{
			test::require(e.kind == 16 && e.id == 7, "only pool events for a KO'd still bleeder");
			test::require(e.value > last, "pool grows monotonically");
			test::require(e.value <= 2.0, "pool capped at 2");
			test::require(e.pos[0] == 100 && e.pos[2] == 50, "pool at its origin");
			last = e.value;
			++sent;
		}
	}
	test::require(sent > 100 && last == 2.0, "pool reaches the cap");
	// Below the 0.1 knee the pool grows linearly at the NEAR-camera cadence: 10.8 steps per second of 0.2 x rate / 54 each (a
	// minute at 0.3 is past the knee, so check the first 5 s: g = 0.2 * 0.3 / 54 * 10.8 * 5 = 0.06). Events are rate limited:
	// one per 0.005 of growth, never one per tick.
	BloodTrail u;
	const std::vector<BloodEvent> five = run(u, c, 0, 20 * 5);
	test::require(!five.empty() && five.size() <= 12 && std::fabs(five.back().value - 0.06) < 0.0051, "closed form below the knee, near cadence");
	for (std::size_t i = 1; i < five.size(); ++i)
		test::require(five[i].value - five[i - 1].value >= BloodTrail::kPoolSendDelta - 1e-12, "events only after a 0.005 change");
	const std::vector<BloodEvent> minute = run(u, c, 20 * 5, 20 * 55);
	// Moving more than sqrt(10) and lying again restarts the pool; the dead freeze it.
	c.x += 4.0;
	const std::vector<BloodEvent> again = run(u, c, 20 * 60, 20 * 2);
	test::require(!again.empty() && again[0].value < minute.back().value && again[0].pos[0] == 104, "new pool far from the old one");
	c.dead = true;
	test::require(run(u, c, 20 * 61, 200).empty(), "dead bodies stop growing");
	c.dead = false;
	c.unconscious = false;
	test::require(run(u, c, 20 * 80, 200).empty(), "no pool while standing");
	c.rate = 0.0;
	c.unconscious = true;
	test::require(run(u, c, 20 * 90, 200).empty(), "no blood without a rate");
}

// The pool's drawn width is 72 g units (bleed_pool.md). A KO'd fighter bleeding at the measured 0.3 (live KO'd fighters bleed
// 0.09 - 0.39) leaves the measured 7 - 15 unit pool after a few minutes: 7 units at 90 s, 14 at 225 s (bleed_pool.md time table).
// At the 0.044 of a single katana cut the same pool needs about 10 minutes to reach 7 units.
// bleed_pool.md's table was taken at the far cadence (1 step per second); ours grows at the near one, kPoolStepsPerSecond steps
// per second, and g depends only on steps x rate, so the same pool is reached in 1 / kPoolStepsPerSecond of the time.
double poolWidthAfter(double rate, double farSeconds)
{
	const double seconds = farSeconds / BloodTrail::kPoolStepsPerSecond;
	BloodTrail t;
	BloodInput c;
	c.id = 21;
	c.rate = rate;
	c.unconscious = true;
	double g = 0.0;
	for (const BloodEvent& e : run(t, c, 0, static_cast<std::uint64_t>(seconds * 20)))
		g = e.value;
	return 72.0 * g;
}

void testPoolSize()
{
	const double w90 = poolWidthAfter(0.3, 90.0);
	const double w225 = poolWidthAfter(0.3, 225.0);
	test::require(w90 > 6.5 && w90 < 8.0, "0.3 / s: a 7 unit pool after 90 s");
	test::require(w225 > 13.0 && w225 < 15.5, "0.3 / s: a 14 unit pool after 225 s");
	test::require(poolWidthAfter(0.3, 180.0) >= 7.0 && poolWidthAfter(0.3, 240.0) <= 15.5, "7 - 15 units within 3 - 4 minutes at 0.3 / s");
	test::require(poolWidthAfter(0.044, 300.0) < 4.0, "a single 30-point cut's 0.044 (never clotting) leaves a 3.5 unit speck after 5 minutes");
	test::require(poolWidthAfter(0.044, 600.0) > 6.5, "and about 7 units after 10 minutes");
}

void testSmear()
{
	BloodTrail t;
	BloodInput c;
	c.id = 3;
	c.rate = 2.5;
	c.down = true;
	const std::vector<BloodEvent> ev = run(t, c, 0, 20 * 10, 0.5); // dragged 10 units per second
	int smears = 0;
	for (const BloodEvent& e : ev)
	{
		if (e.kind == 16 && &e == &ev.front())
			continue; // a fresh tracker has not seen the character move yet: its first tick is a still one (a pool speck)
		test::require(e.kind == 17, "dragged and lying: smears");
		test::require(e.value == 1.0, "alpha = clamp(rate, 0, 1)");
		test::require(e.dir == 0, "heading +x is dir 0");
		++smears;
	}
	test::require(smears >= 8, "a smear every 4 units of travel");
	c.carried = true;
	for (const BloodEvent& e : run(t, c, 1000, 20 * 10, 0.5))
		test::require(e.kind != 17, "no smear while carried");
}

void testDrops()
{
	BloodTrail t;
	BloodInput c;
	c.id = 11;
	c.rate = 0.4;
	c.x = 10;
	test::require(run(t, c, 0, 20 * 600).empty(), "standing still sends nothing");
	const std::vector<BloodEvent> ev = run(t, c, 20 * 600, 20 * 600, 0.1); // walking 2 units per second
	test::require(!ev.empty(), "walking drops blood");
	for (const BloodEvent& e : ev)
		test::require(e.kind == 2 && e.value == 0.4, "drop amount = rate");
	BloodTrail t2;
	test::require(run(t2, c, 20 * 600, 20 * 600, 0.1).size() == ev.size(), "deterministic");
}

} // namespace

int main()
{
	try
	{
		testPool();
		testPoolSize();
		testSmear();
		testDrops();
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_blood OK\n";
	return 0;
}
