// The checkout's source and build directories, compiled into ONE generated file (cmake/build_paths.cpp.in -> <build>/ksim_build_paths.cpp,
// library ksim_paths) so no other compile command carries a per-tree absolute path: ccache then hits across dev trees (kenshi-k16.15.18).
// Tests and dev tools only; a release has no use for them. The macros below keep the old -D names, now function calls (const char*).
#pragma once

namespace ksim::dev {

const char* sourceDir();	// cpp/ of the checkout
const char* binaryDir();	// the cpp/ build directory
const char* scenariosDir();	// <repo>/scenarios; "" in a KSIM_DEV=OFF build (kenshi-379.13.3)

// sourceDir() + "/" + rel / binaryDir() + "/" + rel. The result lives for the program (a small interned set), so it is safe as a const char*.
const char* srcPath(const char* rel);
const char* binPath(const char* rel);

} // namespace ksim::dev

#define KSIM_SOURCE_DIR ksim::dev::sourceDir()
#define KWORLD_SOURCE_DIR ksim::dev::sourceDir()
#define KSIM_BINARY_DIR ksim::dev::binaryDir()
#define KSIM_SCENARIOS_DIR ksim::dev::scenariosDir()
#define KSIM_AI_TABLE ksim::dev::srcPath("data/ksim_ai_behaviours.tsv")
#define KSIM_AI_DATA ksim::dev::srcPath("data")
#define KWORLD_REF_TOOLS ksim::dev::srcPath("../tools")
#define KDATA_KMOD ksim::dev::srcPath("../tools/kmod.py")
#define KSIM_KSCEN_FIXTURE ksim::dev::srcPath("tests/data/flat1.kscen")
#define KSIM_COMBAT_FIGHTS ksim::dev::srcPath("../docs/reference/combat/fights")
#define KSIM_SAMPLE_BIN ksim::dev::binPath("simlink_sample.bin")
#define KSIM_SCENARIO_FIXTURE_DIR ksim::dev::srcPath("tests/data/scenarios")
#define KSIM_ISLANDS_BASELINE ksim::dev::srcPath("tests/data/nav_islands_baseline.tsv")
#define KSIM_SHIPPED_GATE_BASELINE ksim::dev::srcPath("tests/data/nav_shipped_gate_baseline.tsv")
#define KSIM_DOOR_SWEEP_GAPS ksim::dev::srcPath("tests/data/door_sweep_known_gaps.tsv")
#define KSIM_HEIGHT_SWEEP_GAPS ksim::dev::srcPath("tests/data/height_sweep_known_gaps.tsv")
#define KSIM_DEFAULT_SCRIPT ksim::dev::srcPath("data/behaviour/default/faction_intent.lua")
#define KSIM_FAR_SCEN ksim::dev::srcPath("tests/data/far_attack.kscen")
#define KSIM_GAME_OPTIONS_FIXTURE ksim::dev::srcPath("tests/fixtures/game_options.hex")
#define KSIM_FIXTURE ksim::dev::srcPath("tests/fixtures/cage_prisoner_cages.hex")
