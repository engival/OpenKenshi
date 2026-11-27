cmake_minimum_required(VERSION 3.20)
# CMake 3.29+: drop a static library repeated on a link line where the linker does not need it (Apple ld warned
# "ignoring duplicate libraries: libDetour.a, libkworld.a", kenshi-379.32.19).
if(POLICY CMP0156)
	cmake_policy(SET CMP0156 NEW)
endif()
project(kdata LANGUAGES CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/kenshi_ccache.cmake")	# the shared kenshi ccache when its ccache.conf exists (kenshi-zae4)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Apple libc++ gates std::jthread (ksim/world/region.cpp) behind -fexperimental-library (compile and link) (kenshi-379.32.1).
if(APPLE AND NOT CMAKE_CXX_FLAGS MATCHES "fexperimental-library")
	string(APPEND CMAKE_CXX_FLAGS " -fexperimental-library")
	string(APPEND CMAKE_EXE_LINKER_FLAGS " -fexperimental-library")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# The sim's debug view (docs/formats/SIMLINK.md "DEBUG VIEW"): the frames 200-219 and the code that fills them. ON in every build
# type: the runtime gate is --debug-view plus a loopback peer, so a Release server still needs the code. With the option off the
# frames are still accepted and dropped silently.
option(KSIM_DEBUG_VIEW "Compile the sim's debug view (SIMLINK frames 200-219)" ON)
if(KSIM_DEBUG_VIEW)
	add_compile_definitions(KSIM_DEBUG_VIEW=1)
else()
	add_compile_definitions(KSIM_DEBUG_VIEW=0)
endif()

# Dev-only code (ARCHITECTURE "Dev-only code is fenced"): cpp/src/ksim/dev/ and cpp/include/ksim/dev/, and the dev tools that use it. ON by default; OFF drops it
# (the tools are not built, the tests that need it skip with 77).
option(KSIM_DEV "Compile the dev-only code (src/ksim/dev, the dev tools)" ON)
if(KSIM_DEV)
	add_compile_definitions(KSIM_DEV=1)
else()
	add_compile_definitions(KSIM_DEV=0)
endif()

# Per-system tick profiling (docs/PROFILING.md). OFF by default: the tick holds no profile code (the macros in ksim/core/profile.hpp expand to
# nothing). ON times each stage of Simulation::tick into a fixed ring and adds the breakdown to the soak report.
option(KSIM_PROFILE "Compile the per-system tick profiler" OFF)
if(KSIM_PROFILE)
	add_compile_definitions(KSIM_PROFILE=1)
else()
	add_compile_definitions(KSIM_PROFILE=0)
endif()

# The checkout's source/binary/scenario directories live in ONE generated file (never a -D define), so ccache hits across trees
# (kenshi-k16.15.18). Tests and tools read them through ksim/dev/build_paths.hpp; the scenario path is dev-only (kenshi-379.13.3).
set(KSIM_PATHS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(KSIM_PATHS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
if(KSIM_DEV)
	set(KSIM_PATHS_SCENARIOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../scenarios")
else()
	set(KSIM_PATHS_SCENARIOS_DIR "")
endif()
configure_file(cmake/build_paths.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/ksim_build_paths.cpp" @ONLY)
add_library(ksim_paths STATIC "${CMAKE_CURRENT_BINARY_DIR}/ksim_build_paths.cpp")
target_include_directories(ksim_paths PUBLIC include)

# Locations used only by the tests. The game install is read-only.
# Default: $KENSHI_DATA from the environment, else the dev machines' path (cmake/dev_paths.cmake, absent from a release), else the usual Linux
# Steam install; pass -DKENSHI_DATA=... for anywhere else. Without the data the tests that need it skip (exit 77); a dev checkout fails to
# configure instead.
include(cmake/dev_paths.cmake OPTIONAL RESULT_VARIABLE KSIM_DEV_PATHS)
if(DEFINED ENV{KENSHI_DATA})
	set(KENSHI_DATA_DEFAULT "$ENV{KENSHI_DATA}")
elseif(KENSHI_DATA_DEV)
	set(KENSHI_DATA_DEFAULT "${KENSHI_DATA_DEV}")
elseif(APPLE)
	set(KENSHI_DATA_DEFAULT "$ENV{HOME}/Library/Application Support/Steam/steamapps/common/Kenshi/data")
else()
	set(KENSHI_DATA_DEFAULT "$ENV{HOME}/.steam/steam/steamapps/common/Kenshi/data")
endif()
set(KENSHI_DATA "${KENSHI_DATA_DEFAULT}"
	CACHE PATH "Kenshi game data directory (read-only)")
set(KDATA_KMOD "${CMAKE_CURRENT_SOURCE_DIR}/../tools/kmod.py"
	CACHE FILEPATH "Reference parser used by the parity test")
set(KDATA_DATA_DIR "${KENSHI_DATA}")
# The data path server_main.cpp falls back to when --data is not given (dev and test builds; a release carries none, kenshi-379.13). The client
# sets it OFF with the dev tools so a shipped binary holds no developer path.
option(KSIM_BAKE_DATA_PATH "Compile the KENSHI_DATA path into runServer's fallback" ON)

# Warnings are errors on our own targets (user 2026-11-09, INTENT.md "-Werror"); third-party code (flecs, lua) is not covered.
# -DKSIM_WERROR=OFF is the escape hatch for a new compiler's new warnings, never the way to land a warning.
option(KSIM_WERROR "Treat warnings as errors on our targets" ON)
function(kdata_warnings target)
	target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
	if (KSIM_WERROR)
		target_compile_options(${target} PRIVATE -Werror)
	endif()
endfunction()

# The crash recorder kclient and ksim_server share (kutil/crashlog.hpp): a signal handler that writes the signal, the caller's tick,
# a backtrace and the memory map, then re-raises. The client's copy lives here so the server has one too (one file, not two).
# OS code is in src/kutil/platform/<posix|windows>/ (crashlog, mapped_file, socket), picked here (kenshi-379.31.5).
if(WIN32)
	set(KUTIL_PLATFORM windows)
else()
	set(KUTIL_PLATFORM posix)
endif()
add_library(kutil
	src/kutil/platform/${KUTIL_PLATFORM}/crashlog.cpp
	src/kutil/platform/${KUTIL_PLATFORM}/mapped_file.cpp
	src/kutil/platform/${KUTIL_PLATFORM}/socket.cpp
)
target_include_directories(kutil PUBLIC include)
if(WIN32)
	target_link_libraries(kutil PUBLIC ws2_32 dbghelp) # sockets, crashlog
endif()
kdata_warnings(kutil)

add_library(kdata
	src/format.cpp
	src/gamedata.cpp
	src/kdata/datadir.cpp
	src/kdata/steamvdf.cpp
)
target_include_directories(kdata PUBLIC include)
kdata_warnings(kdata)

add_executable(kdump tools/kdump.cpp)
target_link_libraries(kdump PRIVATE kdata)
kdata_warnings(kdump)

add_executable(kmerge tools/kmerge.cpp)
target_link_libraries(kmerge PRIVATE kdata)
kdata_warnings(kmerge)

enable_testing()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_cap.cmake)	# every test is timed in CPU seconds (kenshi-k16.15.55)

# The data-dir discovery (kdata/datadir.hpp, from the client): Steam VDF parsing, candidate order, the macOS bundle places. No game needed.
add_executable(kdata_datadir tests/datadir.cpp)
target_link_libraries(kdata_datadir PRIVATE kdata)
kdata_warnings(kdata_datadir)
add_test(NAME kdatadir COMMAND kdata_datadir)

add_executable(kdata_roundtrip tests/roundtrip.cpp)
target_link_libraries(kdata_roundtrip PRIVATE kdata)
target_compile_definitions(kdata_roundtrip PRIVATE KDATA_DATA_DIR="${KDATA_DATA_DIR}")
kdata_warnings(kdata_roundtrip)
add_test(NAME roundtrip COMMAND kdata_roundtrip)

add_executable(kdata_parity tests/parity.cpp)
target_link_libraries(kdata_parity PRIVATE kdata)
target_compile_definitions(kdata_parity PRIVATE
	KDATA_DATA_DIR="${KDATA_DATA_DIR}"
)
kdata_warnings(kdata_parity)
add_test(NAME parity COMMAND kdata_parity)

add_executable(kdata_overlay tests/overlay.cpp)
target_link_libraries(kdata_overlay PRIVATE kdata)
kdata_warnings(kdata_overlay)
add_test(NAME overlay COMMAND kdata_overlay)

add_executable(kdata_versions tests/versions.cpp)
target_link_libraries(kdata_versions PRIVATE kdata)
kdata_warnings(kdata_versions)
add_test(NAME versions COMMAND kdata_versions)

add_executable(kdata_fields tests/fields.cpp)
target_link_libraries(kdata_fields PRIVATE kdata)
kdata_warnings(kdata_fields)
add_test(NAME fields COMMAND kdata_fields)

add_executable(kdata_malformed tests/malformed.cpp)
target_link_libraries(kdata_malformed PRIVATE kdata)
target_compile_definitions(kdata_malformed PRIVATE KDATA_DATA_DIR="${KDATA_DATA_DIR}")
kdata_warnings(kdata_malformed)
add_test(NAME malformed COMMAND kdata_malformed)

if(NOT MINGW) # no ASan/UBSan runtime on MinGW
# The malformed test is built a second time with the parser itself
# instrumented, so the sanitizer actually checks parser memory accesses.
add_library(kdata_asan STATIC
	src/format.cpp
	src/gamedata.cpp
)
target_include_directories(kdata_asan PUBLIC include)
target_compile_options(kdata_asan PRIVATE
	-fsanitize=address,undefined -fno-omit-frame-pointer -g
)

add_executable(kdata_malformed_asan tests/malformed.cpp)
target_link_libraries(kdata_malformed_asan PRIVATE kdata_asan)
target_compile_definitions(kdata_malformed_asan PRIVATE KDATA_DATA_DIR="${KDATA_DATA_DIR}")
target_compile_options(kdata_malformed_asan PRIVATE
	-fsanitize=address,undefined -fno-omit-frame-pointer -g
)
target_link_options(kdata_malformed_asan PRIVATE -fsanitize=address,undefined)
add_test(NAME malformed_asan COMMAND kdata_malformed_asan)
else()
	list(APPEND KSIM_PLATFORM_ABSENT_TESTS malformed_asan) # validity rows may name it (ksim_validity_check, kenshi-k16.15.38)
endif()

# ---------------------------------------------------------------------------
# kworld — milestone 1, step 2 (world readers)
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)

set(KWORLD_REF_TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
	CACHE PATH "Python reference tools (terrain.py, kmod.py, global_pathing.py)")

add_library(kworld
	src/kworld/heightmap.cpp
	src/kworld/gamedata.cpp
	src/kworld/loadorder.cpp
	src/kworld/zones.cpp
	src/kworld/towns.cpp
	src/kworld/pathing.cpp
	src/kworld/features.cpp
	src/kworld/foliage.cpp
	src/kworld/biomemap.cpp
	src/kworld/roadoverlay.cpp
	src/kworld/roads.cpp
	src/kworld/roads_normalize.cpp
	src/kworld/roadfixups.cpp
	src/kworld/collision.cpp
	src/kworld/collision_area.cpp
	src/kworld/hull_tree.cpp
	src/kworld/layouts.cpp
	src/kworld/navhash.cpp
	src/kworld/buildingparts.cpp
	src/kworld/navhash_source.cpp
	src/kworld/ogreskeleton.cpp
)
target_include_directories(kworld PUBLIC include)
# The source tree's road fixup table (docs/formats/ROAD_FIXUPS.md), the default of kworld::defaultRoadFixupsPath.
target_compile_definitions(kworld PRIVATE KSIM_ROAD_FIXUPS_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../data/fixups/roads.tsv")
# stb_image (PNG only) decodes biomemap.png; vendored, public domain / MIT.
target_include_directories(kworld SYSTEM PRIVATE third_party/stb)
target_link_libraries(kworld PUBLIC kdata kutil)
kdata_warnings(kworld)

add_executable(kworld_survey tools/kworld_survey.cpp)
target_link_libraries(kworld_survey PRIVATE kworld)
target_compile_definitions(kworld_survey PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(kworld_survey)

# kenshi-k16.6.716: offline road normalization analysis (a shipped maintainer tool: reads the data dir, writes a change list as TSV).
add_executable(kroads_normalize tools/kroads_normalize.cpp)
target_link_libraries(kroads_normalize PRIVATE kworld)
target_compile_definitions(kroads_normalize PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(kroads_normalize)

add_executable(kworld_features_dump tools/kworld_features.cpp)
target_link_libraries(kworld_features_dump PRIVATE kworld)
target_compile_definitions(kworld_features_dump PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(kworld_features_dump)

add_executable(kheight tools/kheight.cpp)
target_link_libraries(kheight PRIVATE kworld)
target_compile_definitions(kheight PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(kheight)

add_executable(kworld_terrain tests/kworld_terrain.cpp)
target_link_libraries(kworld_terrain PRIVATE kworld)
target_compile_definitions(kworld_terrain PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_terrain)
add_test(NAME kworld_terrain COMMAND kworld_terrain)

add_executable(kworld_zones tests/kworld_zones.cpp)
target_link_libraries(kworld_zones PRIVATE kworld)
target_compile_definitions(kworld_zones PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_zones)
add_test(NAME kworld_zones COMMAND kworld_zones)

add_executable(kworld_loadorder tests/kworld_loadorder.cpp)
target_link_libraries(kworld_loadorder PRIVATE kworld)
target_compile_definitions(kworld_loadorder PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(kworld_loadorder)
add_test(NAME kworld_loadorder COMMAND kworld_loadorder)

add_executable(kworld_layers tests/kworld_layers.cpp)
target_link_libraries(kworld_layers PRIVATE kworld)
target_compile_definitions(kworld_layers PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_layers)
add_test(NAME kworld_layers COMMAND kworld_layers)

add_executable(kworld_zone_floor tests/kworld_zone_floor.cpp)
target_link_libraries(kworld_zone_floor PRIVATE kworld)
target_compile_definitions(kworld_zone_floor PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_zone_floor)
add_test(NAME kworld_zone_floor COMMAND kworld_zone_floor)

add_executable(kskeleton_dump tools/kskeleton_dump.cpp)
target_link_libraries(kskeleton_dump PRIVATE kworld)
kdata_warnings(kskeleton_dump)

add_executable(kworld_skeleton tests/kworld_skeleton.cpp)
target_link_libraries(kworld_skeleton PRIVATE kworld)
target_compile_definitions(kworld_skeleton PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_skeleton)
add_test(NAME kworld_skeleton COMMAND kworld_skeleton)

add_executable(kworld_foliage tests/kworld_foliage.cpp)
target_link_libraries(kworld_foliage PRIVATE kworld Threads::Threads)
target_compile_definitions(kworld_foliage PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_foliage)
add_test(NAME kworld_foliage COMMAND kworld_foliage)

add_executable(kworld_foliage_grass tests/kworld_foliage_grass.cpp)
target_link_libraries(kworld_foliage_grass PRIVATE kworld)
target_compile_definitions(kworld_foliage_grass PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_foliage_grass)
add_test(NAME kworld_foliage_grass COMMAND kworld_foliage_grass)

add_executable(kworld_foliage_live tests/kworld_foliage_live.cpp)
target_link_libraries(kworld_foliage_live PRIVATE kworld)
target_compile_definitions(kworld_foliage_live PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_foliage_live)
add_test(NAME kworld_foliage_live COMMAND kworld_foliage_live)

add_executable(kworld_foliage_orient tests/kworld_foliage_orient.cpp)
target_link_libraries(kworld_foliage_orient PRIVATE kworld)
target_compile_definitions(kworld_foliage_orient PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_foliage_orient)
add_test(NAME kworld_foliage_orient COMMAND kworld_foliage_orient)

add_executable(kworld_height_live tests/kworld_height_live.cpp)
target_link_libraries(kworld_height_live PRIVATE kworld)
target_compile_definitions(kworld_height_live PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_height_live)
add_test(NAME kworld_height_live COMMAND kworld_height_live)

add_executable(kworld_pathing tests/kworld_pathing.cpp)
target_link_libraries(kworld_pathing PRIVATE kworld)
target_compile_definitions(kworld_pathing PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_pathing)
add_test(NAME kworld_pathing COMMAND kworld_pathing)

add_executable(kworld_placement tests/kworld_placement.cpp)
target_link_libraries(kworld_placement PRIVATE kworld)
target_compile_definitions(kworld_placement PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_placement)
add_test(NAME kworld_placement COMMAND kworld_placement)

add_executable(kworld_towns tests/kworld_towns.cpp)
target_link_libraries(kworld_towns PRIVATE kworld)
target_compile_definitions(kworld_towns PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(kworld_towns)
add_test(NAME kworld_towns COMMAND kworld_towns)

add_executable(kworld_malformed tests/kworld_malformed.cpp)
target_link_libraries(kworld_malformed PRIVATE kworld)
kdata_warnings(kworld_malformed)
add_test(NAME kworld_malformed COMMAND kworld_malformed)

add_executable(kworld_features_malformed tests/kworld_features_malformed.cpp)
target_link_libraries(kworld_features_malformed PRIVATE kworld)
kdata_warnings(kworld_features_malformed)
add_test(NAME kworld_features_malformed COMMAND kworld_features_malformed)

add_executable(kworld_features tests/kworld_features.cpp)
target_link_libraries(kworld_features PRIVATE kworld)
target_compile_definitions(kworld_features PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(kworld_features)
add_test(NAME kworld_features COMMAND kworld_features)

if(NOT MINGW)
# Same malformed/truncation source against ASan+UBSan-instrumented world and
# record parsers.
add_library(kworld_asan STATIC
	src/kworld/heightmap.cpp
	src/kworld/gamedata.cpp
	src/kworld/loadorder.cpp
	src/kworld/zones.cpp
	src/kworld/towns.cpp
	src/kworld/pathing.cpp
	src/kworld/features.cpp
)
target_include_directories(kworld_asan PUBLIC include)
target_link_libraries(kworld_asan PUBLIC kdata_asan kutil)
target_compile_options(kworld_asan PRIVATE
	-fsanitize=address,undefined -fno-omit-frame-pointer -g
)

add_executable(kworld_malformed_asan tests/kworld_malformed.cpp)
target_link_libraries(kworld_malformed_asan PRIVATE kworld_asan)
target_compile_options(kworld_malformed_asan PRIVATE
	-fsanitize=address,undefined -fno-omit-frame-pointer -g
)
target_link_options(kworld_malformed_asan PRIVATE -fsanitize=address,undefined)
add_test(NAME kworld_malformed_asan COMMAND kworld_malformed_asan)

add_executable(kworld_features_malformed_asan tests/kworld_features_malformed.cpp)
target_link_libraries(kworld_features_malformed_asan PRIVATE kworld_asan)
target_compile_options(kworld_features_malformed_asan PRIVATE
	-fsanitize=address,undefined -fno-omit-frame-pointer -g
)
target_link_options(kworld_features_malformed_asan PRIVATE -fsanitize=address,undefined)
add_test(NAME kworld_features_malformed_asan COMMAND kworld_features_malformed_asan)
else()
	list(APPEND KSIM_PLATFORM_ABSENT_TESTS kworld_malformed_asan kworld_features_malformed_asan)
endif()

# ---------------------------------------------------------------------------
# ksim — milestone 1, step 3 (world model + headless tick skeleton)
# ---------------------------------------------------------------------------

# flecs.c is C; the rest of the project stays C++20. Built once as a static
# library so every ksim target shares the same object (Release by default).
enable_language(C)

add_library(flecs STATIC third_party/flecs/flecs.c)
target_include_directories(flecs PUBLIC third_party/flecs)
# gnu99: flecs' POSIX socket/time code needs the GNU feature macros.
set_target_properties(flecs PROPERTIES
	C_STANDARD 99
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS ON
)
target_compile_options(flecs PRIVATE -O2)
if(WIN32)
	target_link_libraries(flecs PUBLIC ws2_32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(flecs PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# Lua 5.5.0, pristine (third_party/lua-5.5.0), for the behaviour layer
# (ARCHITECTURE.md "Behaviour layer: Lua"). C only: no lua.c / luac.c, and none of
# the libraries a script must not have (io, os, debug, package, coroutine, the
# linit.c that opens them all) is compiled at all. Its warnings are not ours.
set(LUA_SRC third_party/lua-5.5.0/src)
add_library(lua55 STATIC
	${LUA_SRC}/lapi.c ${LUA_SRC}/lauxlib.c ${LUA_SRC}/lbaselib.c ${LUA_SRC}/lcode.c
	${LUA_SRC}/lctype.c ${LUA_SRC}/ldebug.c ${LUA_SRC}/ldo.c ${LUA_SRC}/ldump.c
	${LUA_SRC}/lfunc.c ${LUA_SRC}/lgc.c ${LUA_SRC}/llex.c ${LUA_SRC}/lmathlib.c
	${LUA_SRC}/lmem.c ${LUA_SRC}/lobject.c ${LUA_SRC}/lopcodes.c ${LUA_SRC}/lparser.c
	${LUA_SRC}/lstate.c ${LUA_SRC}/lstring.c ${LUA_SRC}/lstrlib.c ${LUA_SRC}/ltable.c
	${LUA_SRC}/ltablib.c ${LUA_SRC}/ltm.c ${LUA_SRC}/lundump.c ${LUA_SRC}/lutf8lib.c
	${LUA_SRC}/lvm.c ${LUA_SRC}/lzio.c
)
target_include_directories(lua55 SYSTEM PUBLIC ${LUA_SRC})
# POSIX (not on Windows: luaconf rejects it under MinGW; plain setjmp there): _setjmp/_longjmp for errors (faster than setjmp, no signal mask). Nothing else
# of it matters without the os and io libraries.
if(NOT WIN32)
	target_compile_definitions(lua55 PRIVATE LUA_USE_POSIX)
endif()
set_target_properties(lua55 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(lua55 PRIVATE -O2 -w)
target_link_libraries(lua55 PUBLIC m)

add_library(ksim
	src/ksim/world/world.cpp
	src/ksim/core/simulation.cpp
	src/ksim/world/region.cpp
	src/ksim/core/state.cpp
	src/ksim/world/world_state.cpp
	src/ksim/core/clock.cpp
	src/ksim/core/rng.cpp
	src/ksim/world/spawn.cpp
	src/ksim/nav/placement.cpp
	src/ksim/world/wilderness.cpp
	src/ksim/world/squad_alive.cpp
	src/ksim/combat/corpses.cpp
	src/ksim/combat/stored_medical.cpp
	src/ksim/combat/token_fight.cpp
	src/ksim/combat/token_fights.cpp
	src/ksim/world/weather.cpp
	src/ksim/world/beams.cpp
	src/ksim/save/save.cpp
	src/ksim/save/snapshot.cpp
	src/ksim/ai/behaviours.cpp
	src/ksim/nav/navigator.cpp
	src/ksim/ai/ai.cpp
	src/ksim/world/factions.cpp
	src/ksim/world/factions_script.cpp
	src/ksim/dialogue/dialogue_book.cpp
	src/ksim/dialogue/dialogue_packages.cpp
	src/ksim/dialogue/dialogue_events.cpp
	src/ksim/dialogue/dialogue_sim.cpp
	src/ksim/ai/individual_goals.cpp
	src/ksim/ai/far_orders.cpp
	src/ksim/nav/doors.cpp
	src/ksim/core/script.cpp
	src/ksim/core/soak.cpp
	src/ksim/core/profile.cpp
	src/ksim/combat/combat_data.cpp
	src/ksim/combat/combat_rules.cpp
	src/ksim/combat/combat.cpp
	src/ksim/world/join_rules.cpp
	src/ksim/ai/perception.cpp
	src/ksim/ai/sensor.cpp
	src/ksim/ai/decision_trace.cpp
	src/ksim/ai/goalsel.cpp
	src/ksim/ai/goap.cpp
	src/ksim/ai/goap_sim.cpp
	src/ksim/ai/server_ai.cpp
	src/ksim/ai/blackboard.cpp
	src/ksim/world/interact.cpp
	src/ksim/economy/inventory_grid.cpp
	src/ksim/economy/inventory_orders.cpp
	src/ksim/world/player_squads.cpp
	src/ksim/ai/jobs.cpp
	src/ksim/economy/trader.cpp
	src/ksim/economy/economy.cpp
	src/ksim/economy/economy_script.cpp
	src/ksim/nav/los.cpp
	src/ksim/ai/routines.cpp
	src/ksim/world/residents.cpp
	src/ksim/world/world_squad_log.cpp
	src/ksim/ai/routine_scorer.cpp
	src/ksim/economy/shopping.cpp
	src/ksim/economy/shopping_sim.cpp
	src/ksim/nav/navcoverage.cpp
	src/ksim/world/prospect.cpp
	src/ksim/economy/machines.cpp
	src/ksim/economy/producers.cpp
	src/ksim/economy/trade.cpp
	src/ksim/world/buildings.cpp
	src/ksim/world/discovery.cpp
	src/ksim/world/town_state.cpp
	src/ksim/save/newgame.cpp
	src/ksim/save/creator.cpp
	src/ksim/nav/door_plan.cpp
	src/ksim/nav/door_goap.cpp
	src/ksim/combat/carry.cpp
	src/ksim/law/slave_state.cpp
	src/ksim/combat/stealth_ko.cpp
	src/ksim/world/world_tasks.cpp
	src/ksim/economy/loot_storage.cpp
	src/ksim/law/crime_goals.cpp
	src/ksim/server_main.cpp
	src/ksim/core/simdata.cpp
	src/ksim/ai/animal_goals.cpp
	src/ksim/ai/raid_phases.cpp
	src/ksim/ai/talk_goals.cpp
	src/ksim/ai/standing_goals.cpp
	src/ksim/nav/lock_tasks.cpp
	src/ksim/law/arrest.cpp
	src/ksim/economy/crops.cpp
	src/ksim/combat/char_stats.cpp
	src/ksim/ai/task_names.cpp
	src/ksim/law/crime.cpp
	src/ksim/economy/research.cpp
	src/ksim/economy/recipes.cpp
	src/ksim/ai/gatecrew.cpp
	src/ksim/combat/medical.cpp
	src/ksim/combat/bloodtrail.cpp
	src/ksim/net/simlink.cpp
	src/ksim/net/simlink_saves.cpp
	src/ksim/net/simlink_playtest.cpp
	src/ksim/net/simlink_server.cpp
	src/ksim/net/simlink_scenario.cpp
	src/ksim/save/scenario_file.cpp
	src/ksim/save/save_import.cpp
	src/ksim/save/import_squad.cpp
)
if(KSIM_DEBUG_VIEW)
	# All of it lives here (docs/formats/SIMLINK.md "DEBUG VIEW"); the hooks outside are the four listed there.
	target_sources(ksim PRIVATE src/ksim/debugview/view.cpp)
endif()
if(KSIM_DEV)
	target_sources(ksim PRIVATE src/ksim/dev/checkpoint_log.cpp src/ksim/dev/walk_stats.cpp src/ksim/dev/arena_strides.cpp src/ksim/dev/test_phases.cpp)
endif()
target_sources(ksim PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/knav_bake_gen/knav_bake_srchash.h)	# the nav cache tree name folds the bake source hash (kenshi-k16.7.161); rule at knav_bake
target_include_directories(ksim PUBLIC include PRIVATE src/ksim ${CMAKE_CURRENT_BINARY_DIR}/knav_bake_gen)	# private headers beside their sources, as "<subsystem>/x.hpp" (kenshi-k16.6.387)
target_link_libraries(ksim PUBLIC kworld knav flecs ksim_paths PRIVATE lua55 kutil)
# The shipped default behaviour scripts (data/behaviour/default).
# The build's git hash (SIMLINK SCENARIO_STARTED simHash, server --version): written at build time into one generated file, so a new HEAD
# recompiles that object only (never a compile define on the library).
set(KSIM_BUILD_INFO_CPP "${CMAKE_CURRENT_BINARY_DIR}/build_info.cpp")
add_custom_target(ksim_build_info BYPRODUCTS "${KSIM_BUILD_INFO_CPP}"
	COMMAND "${CMAKE_COMMAND}" "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${KSIM_BUILD_INFO_CPP}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake"
	COMMENT "build info")
add_dependencies(ksim ksim_build_info)
target_sources(ksim PRIVATE "${KSIM_BUILD_INFO_CPP}")
set_source_files_properties("${KSIM_BUILD_INFO_CPP}" PROPERTIES GENERATED TRUE)
# The playtest scenarios (docs/reference/PLAYTEST.md 1): `scenarios/<name>.kscen` at the repository root, the dev server's default
# directory. Scenarios are dev-only (kenshi-379.13.3): a KSIM_DEV=OFF build compiles no scenario path (the generated build_paths file holds it).
set(KSIM_SCENARIOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../scenarios")
# Sim data (behaviour/, ksim_ai_behaviours.tsv) is resolved at runtime (src/ksim/core/simdata.cpp): --sim-data, else simdata/ beside the
# executable, else the source tree's data/ only when KSIM_BAKE_DATA_PATH.
if(KSIM_BAKE_DATA_PATH)
	set_source_files_properties(src/ksim/core/simdata.cpp PROPERTIES COMPILE_DEFINITIONS
		"KSIM_SIMDATA_BAKED=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
endif()
kdata_warnings(ksim)

# Squad AI (ai.hpp): the package -> behaviour table, the coverage report and
# the soak runner. The AI tests use the world_sample bake (knav section below).
set(KSIM_AI_TABLE "${CMAKE_CURRENT_SOURCE_DIR}/data/ksim_ai_behaviours.tsv")

add_executable(ksim_ai_coverage tools/ksim_ai_coverage.cpp)
target_link_libraries(ksim_ai_coverage PRIVATE ksim)
target_compile_definitions(ksim_ai_coverage PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_ai_coverage)

add_executable(ksim_soak tools/ksim_soak.cpp)
target_link_libraries(ksim_soak PRIVATE ksim)
target_compile_definitions(ksim_soak PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_soak)

if(KSIM_DEV)
	# kenshi-k16.7.122: dev dump of our navmesh and collision round one building, in the common format of the original's export (k16.7.121).
	add_executable(ksim_navcoll_export tools/ksim_navcoll_export.cpp)
	target_link_libraries(ksim_navcoll_export PRIVATE ksim knav RecastNavigation::Detour)
	target_compile_definitions(ksim_navcoll_export PRIVATE KENSHI_DATA="${KENSHI_DATA}")
	kdata_warnings(ksim_navcoll_export)
	# kenshi-k16.7.190: fills the native nav cache tree for a range of regions (the whole-world bake into the shared cache).
	add_executable(ksim_navcache_bake tools/ksim_navcache_bake.cpp)
	target_link_libraries(ksim_navcache_bake PRIVATE ksim knav RecastNavigation::Detour)
	target_compile_definitions(ksim_navcache_bake PRIVATE KENSHI_DATA="${KENSHI_DATA}")
	kdata_warnings(ksim_navcache_bake)
	# kenshi-k16.7.131: grades the geometric navmesh generator (knav/surfgen.hpp) on a building against the original's live capture.
	add_executable(ksim_surfgen_grade tools/ksim_surfgen_grade.cpp)
	target_link_libraries(ksim_surfgen_grade PRIVATE knav)
	target_compile_definitions(ksim_surfgen_grade PRIVATE KENSHI_DATA="${KENSHI_DATA}")
	kdata_warnings(ksim_surfgen_grade)
endif()

if(KSIM_DEV)
	# kenshi-k16.6.109: our per-NPC checkpoint log of the spectate_fight squads, the shape of the original's rwh.91 log.
	add_executable(ksim_checkpoints tools/ksim_checkpoints.cpp)
	target_link_libraries(ksim_checkpoints PRIVATE ksim)
	target_compile_definitions(ksim_checkpoints PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(ksim_checkpoints)
endif()

add_executable(ksim_stats tools/ksim_stats.cpp)
target_link_libraries(ksim_stats PRIVATE ksim)
target_compile_definitions(ksim_stats PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_stats)

add_executable(ksim_server tools/ksim_server.cpp)
target_link_libraries(ksim_server PRIVATE ksim kutil)
# The dedicated server ships as OpenKenshi-server (kenshi-379.17); the CMake target keeps its name so the tests' add_test lines work.
set_target_properties(ksim_server PROPERTIES OUTPUT_NAME OpenKenshi-server)
# TRANSITIONAL: a ksim_server symlink beside it keeps the client's callers (client/src/app/localserver.cpp, client/tools/*.sh,
# client/tests/*/run.sh, client/src/main.cpp's cpp/ksim_server) working until the client lead moves them; then this goes.
add_custom_command(TARGET ksim_server POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E create_symlink OpenKenshi-server $<TARGET_FILE_DIR:ksim_server>/ksim_server)
# -g in every build type: the crash report's backtrace is resolved with addr2line, which needs the debug info (kutil/crashlog.hpp);
# no runtime cost, as the client's CMakeLists says of the same flags.
target_compile_options(ksim_server PRIVATE -g)
# runServer (src/ksim/server_main.cpp, in the ksim library) reads the data and the AI table; KSIM_SCENARIOS_DIR is on ksim already.
# The data path is compiled in only with KSIM_BAKE_DATA_PATH (a release resolves it at runtime, --data). KSIM_AI_TABLE is source-relative.
if(KSIM_BAKE_DATA_PATH)
	set_source_files_properties(src/ksim/server_main.cpp PROPERTIES COMPILE_DEFINITIONS "KENSHI_DATA=\"${KENSHI_DATA}\"")
endif()
# The release layout: simdata/ beside the server binary (a copy at build time without the baked source path, and the install rule,
# which kenshi-379.14's export uses).
set(KSIM_SIMDATA_FILES data/ksim_ai_behaviours.tsv)
if(NOT KSIM_BAKE_DATA_PATH)
	add_custom_command(TARGET ksim_server POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:ksim_server>/simdata
		COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:ksim_server>/simdata
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data/behaviour $<TARGET_FILE_DIR:ksim_server>/simdata/behaviour
		COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/data/ksim_ai_behaviours.tsv $<TARGET_FILE_DIR:ksim_server>/simdata/)
endif()
install(TARGETS ksim_server RUNTIME DESTINATION .)
install(DIRECTORY data/behaviour DESTINATION simdata)
install(FILES data/ksim_ai_behaviours.tsv DESTINATION simdata)
kdata_warnings(ksim_server)

# ksim_server_lib (task 379.5): parseServerArgs and runServer without the process around them.
add_executable(ksim_server_lib tests/ksim_server_lib.cpp)
target_link_libraries(ksim_server_lib PRIVATE ksim)
add_test(NAME ksim_server_lib COMMAND ksim_server_lib
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
kdata_warnings(ksim_server_lib)
# The real binary prints its kProtocol for the deploy script (kenshi-k16.2.306.4.1): one number, exit 0.
add_test(NAME ksim_server_wire_version COMMAND ksim_server --wire-version)
set_tests_properties(ksim_server_wire_version PROPERTIES PASS_REGULAR_EXPRESSION "^[0-9]+\n$")

# Scenario files (docs/reference/SCENARIO_IMPORT.md): the fight of a KSCEN file, N seeds, per-run summary JSON + a distribution table.
add_executable(ksim_save_import tools/ksim_save_import.cpp)
target_link_libraries(ksim_save_import PRIVATE ksim)
target_compile_definitions(ksim_save_import PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_save_import)

add_executable(ksim_fight tools/ksim_fight.cpp)
target_link_libraries(ksim_fight PRIVATE ksim)
target_compile_definitions(ksim_fight PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_fight)

add_executable(ksim_run tools/ksim_run.cpp)
target_link_libraries(ksim_run PRIVATE ksim)
target_compile_definitions(ksim_run PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_run)

add_executable(ksim_hub_census tools/ksim_hub_census.cpp)
target_link_libraries(ksim_hub_census PRIVATE ksim)
target_compile_definitions(ksim_hub_census PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_hub_census)

add_executable(ksim_world tests/ksim_world.cpp)
target_link_libraries(ksim_world PRIVATE ksim)
target_compile_definitions(ksim_world PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_world)
add_test(NAME ksim_world COMMAND ksim_world)

add_executable(ksim_commands tests/ksim_commands.cpp)
target_link_libraries(ksim_commands PRIVATE ksim)
target_compile_definitions(ksim_commands PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_commands)
add_test(NAME ksim_commands COMMAND ksim_commands)

add_executable(ksim_schedule tests/ksim_schedule.cpp)
target_link_libraries(ksim_schedule PRIVATE ksim)
target_compile_definitions(ksim_schedule PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_schedule)
add_test(NAME ksim_schedule COMMAND ksim_schedule)

add_executable(ksim_determinism tests/ksim_determinism.cpp)
target_link_libraries(ksim_determinism PRIVATE ksim)
target_compile_definitions(ksim_determinism PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_determinism)
add_test(NAME ksim_determinism COMMAND ksim_determinism)

add_executable(ksim_save tests/ksim_save.cpp)
target_link_libraries(ksim_save PRIVATE ksim)
target_compile_definitions(ksim_save PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_save)
add_test(NAME ksim_save COMMAND ksim_save)

add_executable(ksim_weather tests/ksim_weather.cpp)
target_link_libraries(ksim_weather PRIVATE ksim)
target_compile_definitions(ksim_weather PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_weather)
add_test(NAME ksim_weather COMMAND ksim_weather)

add_executable(ksim_continue tests/ksim_continue.cpp)
target_link_libraries(ksim_continue PRIVATE ksim)
target_compile_definitions(ksim_continue PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_continue)
add_test(NAME ksim_continue COMMAND ksim_continue)

add_executable(ksim_trader_refresh tests/ksim_trader_refresh.cpp)
target_link_libraries(ksim_trader_refresh PRIVATE ksim)
target_compile_definitions(ksim_trader_refresh PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_trader_refresh)
add_test(NAME ksim_trader_refresh COMMAND ksim_trader_refresh)

add_executable(ksim_trader_circuit tests/ksim_trader_circuit.cpp)
target_link_libraries(ksim_trader_circuit PRIVATE ksim)
target_compile_definitions(ksim_trader_circuit PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_trader_circuit)
add_test(NAME ksim_trader_circuit COMMAND ksim_trader_circuit)

# Town routines, pure part (routines.hpp, TOWN_LIFE_SLICE.md section 5, T3a): package compile, windows, site index, reservation book.
add_executable(ksim_town_goals tests/ksim_town_goals.cpp)
target_link_libraries(ksim_town_goals PRIVATE ksim)
target_compile_definitions(ksim_town_goals PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_goals)
add_test(NAME ksim_town_goals COMMAND ksim_town_goals)

# Residents as Fighters (residents.cpp, TOWN_LIFE_SLICE.md section 5, T3b1): MemberRole, promote / demote, save v48, the rebuilt book.
add_executable(ksim_town_fighters tests/ksim_town_fighters.cpp)
target_link_libraries(ksim_town_fighters PRIVATE ksim)
target_compile_definitions(ksim_town_fighters PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_fighters)
add_test(NAME ksim_town_fighters COMMAND ksim_town_fighters)

# The town goal scorer (routine_scorer.cpp, TOWN_LIFE_SLICE.md 5.3, T3b2): behaviours, seeded placement, blocked pairs, release rules, determinism, save / load.
add_executable(ksim_town_scorer tests/ksim_town_scorer.cpp)
target_link_libraries(ksim_town_scorer PRIVATE ksim)
target_compile_definitions(ksim_town_scorer PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_scorer)
add_test(NAME ksim_town_scorer COMMAND ksim_town_scorer)

# The special-function finder's owner permission reads the Allegiance faction (k16.6.758).
add_executable(ksim_owner_part tests/ksim_owner_part.cpp)
target_link_libraries(ksim_owner_part PRIVATE ksim)
target_compile_definitions(ksim_owner_part PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_owner_part)
add_test(NAME ksim_owner_part COMMAND ksim_owner_part)

# Beds, tiredness, the resident door routine and shop closing (routine_scorer.cpp, TOWN_LIFE_SLICE.md 5.16, T3c).
add_executable(ksim_town_beds tests/ksim_town_beds.cpp)
target_link_libraries(ksim_town_beds PRIVATE ksim)
target_compile_definitions(ksim_town_beds PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_beds)
# Split into parts for ctest to run in parallel (kenshi-k16.5.21): one test per case; the argument names the case.
foreach(c sleepAndWake sharedYard shopNight staleDoors strangers shooEscalation determinism)
	add_test(NAME ksim_town_beds_${c} COMMAND ksim_town_beds ${c})
endforeach()

# Guard shifts (routine_scorer.cpp + carry.cpp, TOWN_LIFE_SLICE.md 5.18, T3e1): the halves flip at the window hour, the beds fill
# and no further, the night-shift addon's windows, and the intruder pickup (223 adopts stepIntruders) on the Hub mesh.
add_executable(ksim_town_guards tests/ksim_town_guards.cpp)
target_link_libraries(ksim_town_guards PRIVATE ksim)
target_compile_definitions(ksim_town_guards PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
	KSIM_NAVWORLD_DIR="${KSIM_NAVWORLD}"
)
kdata_warnings(ksim_town_guards)
add_test(NAME ksim_town_guards COMMAND ksim_town_guards ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_town_guards PROPERTIES FIXTURES_REQUIRED knav_cache)

# Player sleep (task Y): the bed rates (FEATURES 160, `kBedRates`) and the wake on a replacing order.
add_executable(ksim_bed_pose tests/ksim_bed_pose.cpp)
target_link_libraries(ksim_bed_pose PRIVATE ksim)
target_compile_definitions(ksim_bed_pose PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_bed_pose)
add_test(NAME ksim_bed_pose COMMAND ksim_bed_pose)

add_executable(ksim_player_sleep tests/ksim_player_sleep.cpp)
target_link_libraries(ksim_player_sleep PRIVATE ksim)
target_compile_definitions(ksim_player_sleep PRIVATE KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_player_sleep)
add_test(NAME ksim_player_sleep COMMAND ksim_player_sleep ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_player_sleep PROPERTIES FIXTURES_REQUIRED knav_cache)

# The bar hangout (routine_scorer.cpp, TOWN_LIFE_SLICE.md 5.17, T3d): table claims, RELAX 214, pretend SHOPPING 118, the trader's stay.
add_executable(ksim_town_bar tests/ksim_town_bar.cpp)
target_link_libraries(ksim_town_bar PRIVATE ksim)
target_compile_definitions(ksim_town_bar PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_bar)
# Parts (kenshi-k16.5.21): argv[2..] name the cases.
add_test(NAME ksim_town_bar_synthetic COMMAND ksim_town_bar ${KSIM_NAVWORLD} hubBar fallThrough shoppingAndSave traderStay)
add_test(NAME ksim_town_bar_realHub COMMAND ksim_town_bar ${KSIM_NAVWORLD} realHub)
add_test(NAME ksim_town_bar_realHeng COMMAND ksim_town_bar ${KSIM_NAVWORLD} realHeng)

# The shopping trip (shopping.cpp / shopping_sim.cpp / routine_scorer.cpp, task CH group 3): the day's list from the CHARACTER refs,
# the 118 resolver, 119's transaction, and the trip in the real tick across a save / load.
add_executable(ksim_shopping tests/ksim_shopping.cpp)
target_link_libraries(ksim_shopping PRIVATE ksim)
target_compile_definitions(ksim_shopping PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_shopping)
add_test(NAME ksim_shopping COMMAND ksim_shopping)

# World squads as Fighters (residents.cpp, TOWN_LIFE_SLICE.md 5.14): hostile / neutral world squads near the player, hold-while-fighting, save mid-fight.
add_executable(ksim_world_fighters tests/ksim_world_fighters.cpp)
target_link_libraries(ksim_world_fighters PRIVATE ksim)
target_compile_definitions(ksim_world_fighters PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_world_fighters)
add_test(NAME ksim_world_fighters COMMAND ksim_world_fighters ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_world_fighters PROPERTIES FIXTURES_REQUIRED knav_cache)

# World squads follow their Fighters (residents.cpp prepareWorldCarry / stepWorldSquads, TOWN_LIFE_SLICE.md 5.14): the leader walks the group's route, the others follow.
add_executable(ksim_world_follow tests/ksim_world_follow.cpp)
target_link_libraries(ksim_world_follow PRIVATE ksim)
target_compile_definitions(ksim_world_follow PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_world_follow)
# Parts (kenshi-k16.5.21): argv[2..] name the cases.
foreach(c traceReadOnly scenario saveLoad carrotRecovery dayRegression)
	add_test(NAME ksim_world_follow_${c} COMMAND ksim_world_follow ${CMAKE_CURRENT_BINARY_DIR}/hub.knav ${c})
	set_tests_properties(ksim_world_follow_${c} PROPERTIES FIXTURES_REQUIRED knav_cache)
endforeach()

# Streaming navmesh coverage (navcoverage.cpp, NAVMESH_CACHE.md "Streaming coverage"): needs the whole-world bake (knav_bake --world DIR, ~3 GB, 28 min);
# skipped (exit 77) when KSIM_NAVWORLD has no region file for the Hub.
if(NOT KSIM_NAVWORLD_DEV)
	set(KSIM_NAVWORLD_DEV "${CMAKE_BINARY_DIR}/navworld")
endif()
set(KSIM_NAVWORLD "${KSIM_NAVWORLD_DEV}" CACHE PATH "knav_bake --world directory for ksim_nav_regions")
add_executable(ksim_nav_regions tests/ksim_nav_regions.cpp)
target_link_libraries(ksim_nav_regions PRIVATE ksim)
target_compile_definitions(ksim_nav_regions PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_nav_regions)
add_test(NAME ksim_nav_regions COMMAND ksim_nav_regions ${KSIM_NAVWORLD})
set_tests_properties(ksim_nav_regions PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_move_no_route tests/ksim_move_no_route.cpp)
target_link_libraries(ksim_move_no_route PRIVATE ksim)
target_compile_definitions(ksim_move_no_route PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_move_no_route)
add_test(NAME ksim_move_no_route COMMAND ksim_move_no_route ${KSIM_NAVWORLD})
set_tests_properties(ksim_move_no_route PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_stair_silent tests/ksim_stair_silent.cpp)
target_link_libraries(ksim_stair_silent PRIVATE ksim)
target_compile_definitions(ksim_stair_silent PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_stair_silent)
add_test(NAME ksim_stair_silent COMMAND ksim_stair_silent ${KSIM_NAVWORLD})
set_tests_properties(ksim_stair_silent PROPERTIES SKIP_RETURN_CODE 77)

# The Hub tower's stair (user play 2026-11-07: the player on the roof could not walk down, every order no_route). Routes the order's
# own legs on the shipped world bake, both body radii and both directions; needs the bake only, skipped (exit 77) without it.
add_executable(ksim_straight_walk tests/ksim_straight_walk.cpp)
target_link_libraries(ksim_straight_walk PRIVATE ksim)
target_compile_definitions(ksim_straight_walk PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_straight_walk)
add_test(NAME ksim_straight_walk COMMAND ksim_straight_walk ${KSIM_NAVWORLD})
set_tests_properties(ksim_straight_walk PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_route_corners tests/ksim_route_corners.cpp)
target_link_libraries(ksim_route_corners PRIVATE ksim)
target_compile_definitions(ksim_route_corners PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_route_corners)
add_test(NAME ksim_route_corners COMMAND ksim_route_corners ${KSIM_NAVWORLD})
set_tests_properties(ksim_route_corners PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_zigzag_leg tests/ksim_zigzag_leg.cpp)
target_link_libraries(ksim_zigzag_leg PRIVATE ksim)
target_compile_definitions(ksim_zigzag_leg PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_zigzag_leg)
# probes (kenshi-e9s9.15), not tests: ksim_zigzag_leg NAVWORLD [ring] prints the gate_heft leg's corners and a walkability map; ksim_zigzag_walk NAVWORLD OUT.tsv [s] logs every route install of the gate -> stone camp walk
add_executable(ksim_zigzag_walk tests/ksim_zigzag_walk.cpp)
target_link_libraries(ksim_zigzag_walk PRIVATE ksim)
target_compile_definitions(ksim_zigzag_walk PRIVATE KENSHI_DATA="${KENSHI_DATA}")
add_executable(ksim_route_longhouse tests/ksim_route_longhouse.cpp)
target_link_libraries(ksim_route_longhouse PRIVATE ksim)
target_compile_definitions(ksim_route_longhouse PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_route_longhouse)
add_test(NAME ksim_route_longhouse COMMAND ksim_route_longhouse ${KSIM_NAVWORLD})
set_tests_properties(ksim_route_longhouse PROPERTIES SKIP_RETURN_CODE 77)

add_executable(knav_tower_exit tests/knav_tower_exit.cpp)
target_link_libraries(knav_tower_exit PRIVATE ksim)
target_compile_definitions(knav_tower_exit PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_tower_exit)
add_test(NAME knav_tower_exit COMMAND knav_tower_exit ${KSIM_NAVWORLD})
set_tests_properties(knav_tower_exit PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_combat_replay tests/ksim_combat_replay.cpp)
target_link_libraries(ksim_combat_replay PRIVATE ksim)
target_compile_definitions(ksim_combat_replay PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_combat_replay)
add_test(NAME ksim_combat_replay COMMAND ksim_combat_replay)

add_executable(ksim_combat_armed tests/ksim_combat_armed.cpp)
target_link_libraries(ksim_combat_armed PRIVATE ksim)
target_compile_definitions(ksim_combat_armed PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_combat_armed)
add_test(NAME ksim_combat_armed COMMAND ksim_combat_armed)

# The squad scenario must start: navmesh bake, spawn of the players and the real bandits, the first ticks
# (it broke silently once when the snap went 3D). WORKING_DIRECTORY keeps the dump out of the source tree.
add_test(NAME ksim_server_squad
	COMMAND ksim_server --scenario squad --dump-snapshot ksim_server_squad.txt --dump-ticks 6
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(ksim_server_squad PROPERTIES PASS_REGULAR_EXPRESSION "squad: players .* enemies .*dumped 6 ticks")

# --give-item (task BR2): the flag is parsed, checked against the data and configured for every new game's player characters
# (the inventory itself is covered by ksim_newgame's --give-item block).
add_test(NAME ksim_server_give_item
	COMMAND ksim_server --scenario newgame --give-item 1946-gamedata.base 3 --dump-snapshot ksim_server_give_item.txt --dump-ticks 6
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(ksim_server_give_item PROPERTIES PASS_REGULAR_EXPRESSION
	"every new game's player characters get 3 x 'Bread' .*dumped 6 ticks")

# No nav argument (and the --native-nav alias): the server starts on the runtime mesh over the shipped navtiles and plans a Hub route
# (NAVMESH_CACHE.md "Runtime mesh"). A scratch patch cache, so a cold start then a warm one; needs the install's navtiles.
add_test(NAME ksim_server_native_default
	COMMAND ${CMAKE_COMMAND} -DSERVER=$<TARGET_FILE:ksim_server> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/ksim_server_native_default
		-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ksim_server_native_default.cmake)

# duel_armour: the servant is dressed in one plate piece per slot (hat, body, legs, boots) picked from the data by fields.
add_test(NAME ksim_server_duel_armour
	COMMAND ksim_server --scenario duel_armour --dump-snapshot ksim_server_duel_armour.txt --dump-ticks 6
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(ksim_server_duel_armour PROPERTIES PASS_REGULAR_EXPRESSION
	"duel_armour: slot 3 .*duel_armour: slot 5 .*duel_armour: slot 6 .*duel_armour: slot 9 .*dumped 6 ticks")

# Scenario files (docs/reference/SCENARIO_IMPORT.md): the synthetic hand-written file through the server and through the N-seed fight runner.
# ksim_fight and the server run on the default runtime mesh (no --navworld; kenshi-k16.6.252); the real, game-derived fixture lives in kenshi-inspect and its test skips when absent.
add_test(NAME ksim_server_scenario_file
	COMMAND ksim_server --scenario-file ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/scenario_synthetic.kscen
		--dump-snapshot ksim_server_scenario_file.txt --dump-ticks 6
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(ksim_server_scenario_file PROPERTIES PASS_REGULAR_EXPRESSION "scenario file .* 3 characters \\(1 player-controlled\\).*dumped 6 ticks")
add_test(NAME ksim_fight_synthetic
	COMMAND ksim_fight ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/scenario_synthetic.kscen --runs 2 --seed 6 --check
		--out ${CMAKE_CURRENT_BINARY_DIR}/ksim_fight_synthetic
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(ksim_fight_synthetic PROPERTIES SKIP_RETURN_CODE 77)
# The converted fight fixtures are game-derived and live in kenshi-inspect (RUNBOOK "Fight fixtures"; copied to the same path on dev).
# A cached path that does not exist is reset to that default: the test queue's cache held an older default (~/git/kenshi-inspect,
# no kenshi/), so every queue run skipped the ksim_fight_* gates without a word (kenshi-k16.6.252).
set(KSIM_FIGHT_FIXTURE_HOME "$ENV{HOME}/git/kenshi/kenshi-inspect/findings/fight_fixtures")
foreach(fixture_var KSIM_FIGHT_FIXTURE_DIR KSIM_FIGHT_FIXTURE KSIM_CROWD_FIXTURE)
	if(DEFINED CACHE{${fixture_var}} AND NOT EXISTS "${${fixture_var}}" AND IS_DIRECTORY "${KSIM_FIGHT_FIXTURE_HOME}")
		message(STATUS "${fixture_var}: cached ${${fixture_var}} does not exist; reset to ${KSIM_FIGHT_FIXTURE_HOME}")
		unset(${fixture_var} CACHE)
	endif()
endforeach()
set(KSIM_FIGHT_FIXTURE_DIR "${KSIM_FIGHT_FIXTURE_HOME}" CACHE PATH "converted real fight fixtures (game-derived, outside the repo)")
set(KSIM_FIGHT_FIXTURE "${KSIM_FIGHT_FIXTURE_DIR}/sb1_flashbox/scenario.kscen" CACHE FILEPATH "converted real fight fixture (game-derived, outside the repo)")
add_test(NAME ksim_fight_real
	COMMAND ksim_fight ${KSIM_FIGHT_FIXTURE} --optional --runs 3 --check
		--out ${CMAKE_CURRENT_BINARY_DIR}/ksim_fight_real
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(ksim_fight_real PROPERTIES SKIP_RETURN_CODE 77)
# The crowd gate (SCENARIO_IMPORT.md "First comparison", BAR_FIGHT_LOOP.md Finding 0e): the original's sb1_flashbox numbers are her swings 2.6,
# landed 1.2, swings at her 13.8, KO 24.9 s; before the crowd spacing work ours were 19.3 / 15.8 / 15.1 / 53.6. The bounds are fixed (seeds 1..10 are deterministic); tightened after BQ12 (ours: her swings 6.4, landed 4.2, KO 30.2 s, swings at her 10.7); the 1v1 fixture (original: her swings 10.3, landed 4.0) must not regress. The 1v1 runs 30 seeds: the rolls hang on entity ids, so an unrelated spawn change (ruins losing their doors) reshuffles them, and 10 seeds moved its means by 1.8 landed / 4 s KO (30 seeds: 4.9 / 5.1). Both skip (77) without the fixtures.
# Re-pinned after task AL (2026-11-06): the interior-layout storage boxes now spawn with the Hub furniture, before the scenario's characters, so the ids and their rolls moved (the same reshuffle). The branch's 10 seeds give her swings 8.8 / landed 7.2 / at her 17.2 / KO 39.4 s; 40 seeds on the branch and on the previous build give 6.9 / 5.2 / 15.4 / 36.5 vs 5.5 / 4.2 / 14.9 / 34.3 (each difference inside its standard error, z 0.4-1.5), so it is the documented reshuffle, not a shift. The bounds cover 3 standard errors of a 10-seed mean on the branch's 40-run spread (1.3 swings, 1.2 landed, 1.8 at her, 4.4 s KO).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
	add_test(NAME ksim_fight_crowd
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/fight_gate.py $<TARGET_FILE:ksim_fight>
			${KSIM_FIGHT_FIXTURE_DIR}/sb1_flashbox/scenario.kscen - ${CMAKE_CURRENT_BINARY_DIR}/ksim_fight_crowd --runs 10
			--max her_swings=11 --max her_landed=9 --min swings_at=9 --max swings_at=22 --max ko=50)
	set_tests_properties(ksim_fight_crowd PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
	add_test(NAME ksim_fight_1v1
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/fight_gate.py $<TARGET_FILE:ksim_fight>
			${KSIM_FIGHT_FIXTURE_DIR}/1v1_flashbox/scenario.kscen - ${CMAKE_CURRENT_BINARY_DIR}/ksim_fight_1v1 --runs 30
			--min her_swings=7 --max her_swings=13 --min her_landed=2.5 --max her_landed=6 --min ko=45 --max ko=85)
	# RUN_SERIAL (task CB, the peer): 30 runs of a heavy combat fixture take ~205 s alone but 719 s in the -j16 suite - it hogs
	# cores and inflates the whole run. Running it alone costs ~3.5 min of the suite's wall clock and nothing else moves; its checks
	# are statistical over the runs, not wall-clock. fight_gate.py runs the 30 seeds in 4 shard processes (kenshi-q0ou: serial it passed the queue's 1200 s cap). TIMEOUT stays 1800 as a hang guard.
	set_tests_properties(ksim_fight_1v1 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 RUN_SERIAL TRUE)
endif()
# bar_crowd (28 bandits against the Hub bar's residents, KSCEN 2 orders, no player in the fight): the converted fixture, optional like ksim_fight_real.
# Seeds 4-5: the fixture's orders end when the ordered targets fall, and a survivor that is still aware of the scenario's distant Flashbox dummy
# (Nameless_0, ~1400 u north) can walk off to it while the bar fight is still live, so a run stalls at the 300 s cap. Measured over seeds 1-8
# (2026-11-06): each build stalls on one seed but not the same one -- master (T3e1) on 3, the facing branch on 2 -- and 4-8 settle everywhere
# (119-221 s). The pair is re-picked to 4-5 rather than let the gate ride on a coin flip.
# Seed 5 settles slowly since the chase rework (k16.8.24): it wins at 404 s, because a lone fighter holds ten wave-2 bandits ~500 u from her allies for
# ~150 s. That is faithful (kenshi-rwh.189: the original has no help call; an idle NPC joins only through its own PROTECT_OWN_SQUAD / PROTECT_ALLIES,
# gated on its OWN sensor record), so the cap is 600 s rather than the default 300 (k16.8.50).
set(KSIM_CROWD_FIXTURE "${KSIM_FIGHT_FIXTURE_DIR}/bar_crowd/scenario.kscen" CACHE FILEPATH "converted bar_crowd fight fixture (game-derived, outside the repo)")
add_test(NAME ksim_fight_crowd_real
	COMMAND ksim_fight ${KSIM_CROWD_FIXTURE} --optional --runs 2 --seed 4 --check --max-seconds 600
		--out ${CMAKE_CURRENT_BINARY_DIR}/ksim_fight_crowd_real
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(ksim_fight_crowd_real PROPERTIES SKIP_RETURN_CODE 77)
# A loaded scenario's non-player squads run the world AI (a resident joins a fight beside him).
add_executable(ksim_scenario_ai tests/ksim_scenario_ai.cpp)
target_link_libraries(ksim_scenario_ai PRIVATE ksim)
target_compile_definitions(ksim_scenario_ai PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_scenario_ai)
add_test(NAME ksim_scenario_ai COMMAND ksim_scenario_ai ${KSIM_NAVWORLD})
set_tests_properties(ksim_scenario_ai PROPERTIES SKIP_RETURN_CODE 77)
# Task BI: the hand-written flat1 fight (an unarmed 1v1, the player's order at tick 80) reaches blows on both sides.
add_executable(ksim_kscen_fight tests/ksim_kscen_fight.cpp)
target_link_libraries(ksim_kscen_fight PRIVATE ksim)
target_compile_definitions(ksim_kscen_fight PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_kscen_fight)
add_test(NAME ksim_kscen_fight COMMAND ksim_kscen_fight ${KSIM_NAVWORLD})
set_tests_properties(ksim_kscen_fight PROPERTIES SKIP_RETURN_CODE 77)
# Task CC: a fighter's dice key hangs on its stable identity, not its Id: flat1's fight is bit-identical with an unrelated entity
# spawned first (the Ids all move by one).
add_executable(ksim_stable_dice tests/ksim_stable_dice.cpp)
target_link_libraries(ksim_stable_dice PRIVATE ksim)
target_compile_definitions(ksim_stable_dice PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_stable_dice)
add_test(NAME ksim_stable_dice COMMAND ksim_stable_dice ${KSIM_NAVWORLD})
set_tests_properties(ksim_stable_dice PROPERTIES SKIP_RETURN_CODE 77)
# Task BM: with the world AI on, flat1's area stays free of the default world's later spawns and the ordered fight still lands.
add_executable(ksim_kscen_radius tests/ksim_kscen_radius.cpp)
target_link_libraries(ksim_kscen_radius PRIVATE ksim)
target_compile_definitions(ksim_kscen_radius PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_kscen_radius)
add_test(NAME ksim_kscen_radius COMMAND ksim_kscen_radius ${KSIM_NAVWORLD})
set_tests_properties(ksim_kscen_radius PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_medical tests/ksim_medical.cpp)
target_link_libraries(ksim_medical PRIVATE ksim)
target_compile_definitions(ksim_medical PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_medical)
add_test(NAME ksim_medical COMMAND ksim_medical)

# Task BT: the new-game advanced options (state::WorldOptions, save v77), the synced medical clock and the debug setter's logic.
add_executable(ksim_world_options tests/ksim_world_options.cpp)
target_link_libraries(ksim_world_options PRIVATE ksim)
target_compile_definitions(ksim_world_options PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_world_options)
add_test(NAME ksim_world_options COMMAND ksim_world_options)

add_executable(ksim_bleed_decay tests/ksim_bleed_decay.cpp)
target_link_libraries(ksim_bleed_decay PRIVATE ksim)
target_compile_definitions(ksim_bleed_decay PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_bleed_decay)
add_test(NAME ksim_bleed_decay COMMAND ksim_bleed_decay)

add_executable(ksim_ko_care tests/ksim_ko_care.cpp)
target_link_libraries(ksim_ko_care PRIVATE ksim)
target_compile_definitions(ksim_ko_care PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_ko_care)
add_test(NAME ksim_ko_care COMMAND ksim_ko_care)

add_executable(ksim_ko_fate tests/ksim_ko_fate.cpp)
target_link_libraries(ksim_ko_fate PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_ko_fate PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_ko_fate)
# The fist duel (section 6) runs on the Hub cache; the medical sections do not need it.
add_test(NAME ksim_ko_fate COMMAND ksim_ko_fate ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_ko_fate PROPERTIES FIXTURES_REQUIRED knav_cache)

# ko-wake: a collapsed leg leaves the servant down and conscious until first aid (MEDICAL_HUNGER_LIVE (d)/(e)). Needs the Hub cache.
add_executable(ksim_ko_wake tests/ksim_ko_wake.cpp)
target_link_libraries(ksim_ko_wake PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_ko_wake PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_ko_wake)
add_test(NAME ksim_ko_wake COMMAND ksim_ko_wake ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_ko_wake PROPERTIES FIXTURES_REQUIRED knav_cache)

# A KO'd body drops its technique whatever its combat state (a block's pointer survived a KO in Stumble). Needs the Hub cache.
add_executable(ksim_ko_stale_attack tests/ksim_ko_stale_attack.cpp)
target_link_libraries(ksim_ko_stale_attack PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_ko_stale_attack PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_ko_stale_attack)
add_test(NAME ksim_ko_stale_attack COMMAND ksim_ko_stale_attack ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_ko_stale_attack PROPERTIES FIXTURES_REQUIRED knav_cache)

# Task BC: a player-ordered finish-off of a downed servant ends in a wake or a death, not a multi-minute KO. The duel takes
# orders, so it needs the Hub cache.
add_executable(ksim_ko_long tests/ksim_ko_long.cpp)
target_link_libraries(ksim_ko_long PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_ko_long PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_ko_long)
add_test(NAME ksim_ko_long COMMAND ksim_ko_long ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_ko_long PROPERTIES FIXTURES_REQUIRED knav_cache)

# Task BF: a conscious down character crawls on a move order (a KO'd one does not move). Player orders only, Hub cache.
add_executable(ksim_crawl tests/ksim_crawl.cpp)
target_link_libraries(ksim_crawl PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_crawl PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_crawl)
add_test(NAME ksim_crawl COMMAND ksim_crawl ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_crawl PROPERTIES FIXTURES_REQUIRED knav_cache)

# Task BH: the legs scale the run speed (min left/right leg health ratio on the excess over the 11 u/s floor).
add_executable(ksim_leg_speed tests/ksim_leg_speed.cpp)
target_link_libraries(ksim_leg_speed PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_leg_speed PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_leg_speed)
add_test(NAME ksim_leg_speed COMMAND ksim_leg_speed ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_leg_speed PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_limb_leak tests/ksim_limb_leak.cpp)
target_link_libraries(ksim_limb_leak PRIVATE ksim)
target_compile_definitions(ksim_limb_leak PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_limb_leak)
add_test(NAME ksim_limb_leak COMMAND ksim_limb_leak)

add_executable(ksim_combat_sim tests/ksim_combat_sim.cpp)
target_link_libraries(ksim_combat_sim PRIVATE ksim)
target_compile_definitions(ksim_combat_sim PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_combat_sim)
add_test(NAME ksim_combat_sim COMMAND ksim_combat_sim)

# The crowd penalty in target selection and the state-6 wait ring (crowd_melee.md §10, task AU).
add_executable(ksim_crowd_penalty tests/ksim_crowd_penalty.cpp)
target_link_libraries(ksim_crowd_penalty PRIVATE ksim)
target_compile_definitions(ksim_crowd_penalty PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_crowd_penalty)
add_test(NAME ksim_crowd_penalty COMMAND ksim_crowd_penalty)

# The combat-joining goals 143 / 15 / 16 through their resolvers and GOAP plans, no NPC order involved (task CF).
add_executable(ksim_combat_join tests/ksim_combat_join.cpp)
target_link_libraries(ksim_combat_join PRIVATE ksim)
target_compile_definitions(ksim_combat_join PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_combat_join)
add_test(NAME ksim_combat_join COMMAND ksim_combat_join)

# The raid tests on the navworld bake and live world squads: ksim_raid_goals the goals 270 ASSAULT_FORTIFICATIONS_PREFER_GATES and 276 LIBERATE_ALL_THE_PRISONERS
# (task raid-goals); ksim_raid_occupy the stay: Crab Raiders in a player town assault it, occupy it, throw a downed player out and leave 4 s after the last one is
# gone (task raid-stay); ksim_raid_victory_stay a force that won its town holds it past the stay and the retreat, off it walks home (kenshi-h2v.22.10);
# ksim_raid_phases the phase packages on the whole force and the stale target (task raid-phases).
foreach(raid_test ksim_raid_goals ksim_raid_occupy ksim_raid_victory_stay ksim_raid_phases)
	add_executable(${raid_test} tests/${raid_test}.cpp)
	target_link_libraries(${raid_test} PRIVATE ksim)
	target_compile_definitions(${raid_test} PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(${raid_test})
	add_test(NAME ${raid_test} COMMAND ${raid_test} ${KSIM_NAVWORLD})
	set_tests_properties(${raid_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# kenshi-k16.6.529: residents seated on an upper storey are placed at it (the bar next to Outpost s-IV), on the live world.
add_executable(ksim_siv_residents tests/ksim_siv_residents.cpp)
target_link_libraries(ksim_siv_residents PRIVATE ksim)
target_compile_definitions(ksim_siv_residents PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_siv_residents)
add_test(NAME ksim_siv_residents COMMAND ksim_siv_residents ${KSIM_NAVWORLD})
set_tests_properties(ksim_siv_residents PROPERTIES SKIP_RETURN_CODE 77)

# The selector's live Situation feed (mission / assaultTown / atTown) and the loaded world's own selector re-stamping a patrol (task board-feed).
add_executable(ksim_board_feed tests/ksim_board_feed.cpp)
target_link_libraries(ksim_board_feed PRIVATE ksim)
target_compile_definitions(ksim_board_feed PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_board_feed)
add_test(NAME ksim_board_feed COMMAND ksim_board_feed ${KSIM_NAVWORLD})
set_tests_properties(ksim_board_feed PROPERTIES SKIP_RETURN_CODE 77)

# A spawned fighter's stats without a STATS record: `strength` 0 means unset, the base is `combat stats` (task AR).
add_executable(ksim_spawn_stats tests/ksim_spawn_stats.cpp)
target_link_libraries(ksim_spawn_stats PRIVATE ksim)
target_compile_definitions(ksim_spawn_stats PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_spawn_stats)
add_test(NAME ksim_spawn_stats COMMAND ksim_spawn_stats)

# Movement on the navmesh in the sim (SIMLINK.md section 12), on the Hub cache of the knav tests.
add_executable(ksim_move tests/ksim_move.cpp)
target_link_libraries(ksim_move PRIVATE ksim)
target_compile_definitions(ksim_move PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_move)
add_test(NAME ksim_move COMMAND ksim_move ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_move PROPERTIES FIXTURES_REQUIRED knav_cache)

# The limb collapse law end to end (below the KO point, not 0), on the same Hub cache: a leg at -50 walks, at -52
# the character is down and conscious, a severed limb is down and leaks.
add_executable(ksim_collapse tests/ksim_collapse.cpp)
target_link_libraries(ksim_collapse PRIVATE ksim)
target_compile_definitions(ksim_collapse PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_collapse)
add_test(NAME ksim_collapse COMMAND ksim_collapse ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_collapse PROPERTIES FIXTURES_REQUIRED knav_cache)

# Walker kinematics (acceleration, corner braking, turn rate, turn in place), on the same Hub cache.
add_executable(ksim_walk_kinematics tests/ksim_walk_kinematics.cpp)
target_link_libraries(ksim_walk_kinematics PRIVATE ksim)
target_compile_definitions(ksim_walk_kinematics PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_walk_kinematics)
add_test(NAME ksim_walk_kinematics COMMAND ksim_walk_kinematics ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_walk_kinematics PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_medic_hold_order tests/ksim_medic_hold_order.cpp)
target_link_libraries(ksim_medic_hold_order PRIVATE ksim)
target_compile_definitions(ksim_medic_hold_order PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_medic_hold_order)
add_test(NAME ksim_medic_hold_order COMMAND ksim_medic_hold_order ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_medic_hold_order PROPERTIES FIXTURES_REQUIRED knav_cache)

# The follow task and the fight join (SQUAD_FOLLOW.md), on the same Hub cache.
add_executable(ksim_medic_follower_patient tests/ksim_medic_follower_patient.cpp)
target_link_libraries(ksim_medic_follower_patient PRIVATE ksim)
target_compile_definitions(ksim_medic_follower_patient PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_medic_follower_patient)
add_test(NAME ksim_medic_follower_patient COMMAND ksim_medic_follower_patient ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_medic_follower_patient PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_follow tests/ksim_follow.cpp)
target_link_libraries(ksim_follow PRIVATE ksim)
target_compile_definitions(ksim_follow PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_follow)
add_test(NAME ksim_follow COMMAND ksim_follow ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_follow PROPERTIES FIXTURES_REQUIRED knav_cache)

# A player group move settles like the original (kenshi-k16.6.231, SQUAD_FOLLOW.md s12).
add_executable(ksim_group_settle tests/ksim_group_settle.cpp)
target_link_libraries(ksim_group_settle PRIVATE ksim)
target_compile_definitions(ksim_group_settle PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_group_settle)
add_test(NAME ksim_group_settle COMMAND ksim_group_settle ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_group_settle PROPERTIES FIXTURES_REQUIRED knav_cache)

# A passing runner pushes the bodies it overlaps (kenshi-k16.6.425, oracle kenshi-rwh.188).
add_executable(ksim_group_stop tests/ksim_group_stop.cpp)
target_link_libraries(ksim_group_stop PRIVATE ksim)
target_compile_definitions(ksim_group_stop PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_group_stop)
add_test(NAME ksim_group_stop COMMAND ksim_group_stop ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_group_stop PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_overlap_push tests/ksim_overlap_push.cpp)
target_link_libraries(ksim_overlap_push PRIVATE ksim)
target_compile_definitions(ksim_overlap_push PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_overlap_push)
add_test(NAME ksim_overlap_push COMMAND ksim_overlap_push ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_overlap_push PROPERTIES FIXTURES_REQUIRED knav_cache)

# Arena strides: the fight tier every step, the rest every second (kenshi-k16.6.636).
add_executable(ksim_arena_strides tests/ksim_arena_strides.cpp)
target_link_libraries(ksim_arena_strides PRIVATE ksim)
target_compile_definitions(ksim_arena_strides PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_arena_strides)
add_test(NAME ksim_arena_strides COMMAND ksim_arena_strides ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_arena_strides PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_speed_modes tests/ksim_speed_modes.cpp)
target_link_libraries(ksim_speed_modes PRIVATE ksim)
target_compile_definitions(ksim_speed_modes PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_speed_modes)
add_test(NAME ksim_speed_modes COMMAND ksim_speed_modes ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_speed_modes PROPERTIES FIXTURES_REQUIRED knav_cache)

# SIMLINK server (docs/formats/SIMLINK.md): a loopback session against the client session's
# own decoder (client/src/net/simlink.cpp, compiled in unchanged), replay from the order log, and
# the recorded sample stream. The sample is recorded at test time by ksim_simlink_sample into the build dir, never kept in the tree: it
# carries game-data strings (kenshi-379.14.9, user 2026-11-18: "If it's not part of our running system, then no, we don't need a bin capture").
add_executable(ksim_simlink tests/ksim_simlink.cpp ../client/src/net/simlink.cpp)
target_include_directories(ksim_simlink PRIVATE ../client/src)
target_link_libraries(ksim_simlink PRIVATE ksim)
target_compile_definitions(ksim_simlink PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_simlink)
add_test(NAME ksim_simlink_sample COMMAND ksim_server --no-world
	--dump-snapshot ${CMAKE_CURRENT_BINARY_DIR}/simlink_sample.txt --dump-bin ${CMAKE_CURRENT_BINARY_DIR}/simlink_sample.bin
	--dump-ticks 400 --dump-script ${CMAKE_CURRENT_SOURCE_DIR}/../docs/formats/simlink_sample_script.txt)
set_tests_properties(ksim_simlink_sample PROPERTIES FIXTURES_SETUP simlink_sample)
add_test(NAME ksim_simlink COMMAND ksim_simlink)
set_tests_properties(ksim_simlink PROPERTIES FIXTURES_REQUIRED simlink_sample)

# The debug view's spawn order (SIMLINK.md "DEBUG VIEW"): DEBUG_SPAWN_SQUAD (212) over a real session, answered DEBUG_SPAWNED
# (203): the spawned squad shows in SQUADS and walks to the goal town, and each refusal carries its reason.
add_executable(ksim_debug_spawn tests/ksim_debug_spawn.cpp)
target_link_libraries(ksim_debug_spawn PRIVATE ksim)
target_compile_definitions(ksim_debug_spawn PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_debug_spawn)
add_test(NAME ksim_debug_spawn COMMAND ksim_debug_spawn)
# -DKSIM_DEBUG_VIEW=OFF accepts the debug frames and drops them, so 212 is never answered: the test skips itself (77).
set_tests_properties(ksim_debug_spawn PROPERTIES SKIP_RETURN_CODE 77)

# The debug view's weather and time commands (SIMLINK.md "DEBUG VIEW"): DEBUG_SET_WEATHER (213) pins a group's weather past its
# season end and lifetime and is released for a fresh pick; DEBUG_SET_TIME (214) jumps forward and its send carries CLOCK; each
# answers DEBUG_ACK (204), refusals with their reason.
add_executable(ksim_debug_weather_time tests/ksim_debug_weather_time.cpp)
target_link_libraries(ksim_debug_weather_time PRIVATE ksim)
target_compile_definitions(ksim_debug_weather_time PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_debug_weather_time)
add_test(NAME ksim_debug_weather_time COMMAND ksim_debug_weather_time)
# -DKSIM_DEBUG_VIEW=OFF accepts the debug frames and drops them, so 213/214 are never answered: the test skips itself (77).
set_tests_properties(ksim_debug_weather_time PROPERTIES SKIP_RETURN_CODE 77)

# Save / load over SIMLINK (SIMLINK.md SAVES): orders 80-83, frame 32, the metadata block, load = full resend.
add_executable(ksim_saves tests/ksim_saves.cpp)
target_link_libraries(ksim_saves PRIVATE ksim)
target_compile_definitions(ksim_saves PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_saves)
add_test(NAME ksim_saves COMMAND ksim_saves ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_saves PROPERTIES FIXTURES_REQUIRED knav_cache)

# Single-player new game (SIMLINK.md order 84, STARTS 33, NEW_GAME 111 / 62; MULTIPLAYER.md section 4).
add_executable(ksim_newgame tests/ksim_newgame.cpp ../client/src/net/simlink.cpp)
target_include_directories(ksim_newgame PRIVATE ../client/src)
target_link_libraries(ksim_newgame PRIVATE ksim)
target_compile_definitions(ksim_newgame PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_newgame)
add_test(NAME ksim_newgame COMMAND ksim_newgame ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_newgame PROPERTIES FIXTURES_REQUIRED knav_cache)

# Player buildings (BUILDINGS_SLICE.md Phases 1-2): placement (orders 90 / 91, BUILDING 47 through the client's decoder, save) and the construction math.
add_executable(ksim_buildings_place tests/ksim_buildings_place.cpp ../client/src/net/simlink.cpp)
target_include_directories(ksim_buildings_place PRIVATE ../client/src tests)
target_link_libraries(ksim_buildings_place PRIVATE ksim)
target_compile_definitions(ksim_buildings_place PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_buildings_place)
add_test(NAME ksim_buildings_place COMMAND ksim_buildings_place ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_buildings_place PROPERTIES FIXTURES_REQUIRED knav_cache)
add_executable(ksim_buildings_build tests/ksim_buildings_build.cpp ../client/src/net/simlink.cpp)
target_include_directories(ksim_buildings_build PRIVATE ../client/src tests)
target_link_libraries(ksim_buildings_build PRIVATE ksim)
target_compile_definitions(ksim_buildings_build PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_buildings_build)
add_test(NAME ksim_buildings_build COMMAND ksim_buildings_build ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_buildings_build PROPERTIES FIXTURES_REQUIRED knav_cache)

# Player squads (FACTION_SQUADS_LIVE.md): orders 70-75, frame 31, event 18, the dismissal's AI goal, save and continue.
add_executable(ksim_squads tests/ksim_squads.cpp)
target_link_libraries(ksim_squads PRIVATE ksim)
target_compile_definitions(ksim_squads PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_squads)
add_test(NAME ksim_squads COMMAND ksim_squads ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_squads PROPERTIES FIXTURES_REQUIRED knav_cache)

# Jobs V1 (SIMLINK.md JOBS 34, orders 76-78, toggle 10 which 4): the addTask and drag rules, every answer, frame 34, the executors, save and continue.
add_executable(ksim_jobs tests/ksim_jobs.cpp)
target_link_libraries(ksim_jobs PRIVATE ksim)
target_compile_definitions(ksim_jobs PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_jobs)
add_test(NAME ksim_jobs COMMAND ksim_jobs ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_jobs PROPERTIES FIXTURES_REQUIRED knav_cache)

# Task dt25: the three finder-less goals - 45 BODYGUARD (the protected hand and the picker's +10), 42 DON'T BE UNARMED (the
# rearm loop, the animal exemption) and 169 PUT_DOWN_CARRIED_DUDE (the can-walk gate). Bare worlds: no navmesh needed.
foreach(dt25_test ksim_bodyguard ksim_rearm ksim_putdown_carried ksim_interact_abandon)
	add_executable(${dt25_test} tests/${dt25_test}.cpp)
	target_link_libraries(${dt25_test} PRIVATE ksim)
	target_compile_definitions(${dt25_test} PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(${dt25_test})
	add_test(NAME ${dt25_test} COMMAND ${dt25_test})
	set_tests_properties(${dt25_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# Task dt27: 230 EAT_A_RANDOM_DEAD_BODY + 135 KILL_A_RANDOM_CAGE_OCCUPANT, 176 HUNTING_BLOODSMELL, 130 / 53 TRAVEL_TO_TARGET_TOWN. Bare worlds.
foreach(dt27_test ksim_eat_kill ksim_hunting_smell ksim_travel_town ksim_army_travel ksim_slave_trade ksim_home_walk ksim_fog_eat ksim_rescue_leader ksim_carrier_drop ksim_escape ksim_cannibal_pot ksim_peeler ksim_slave_loot ksim_graft_cage_camp ksim_announce ksim_spawn_sex ksim_locks)
	add_executable(${dt27_test} tests/${dt27_test}.cpp)
	target_link_libraries(${dt27_test} PRIVATE ksim)
	target_compile_definitions(${dt27_test} PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(${dt27_test})
	add_test(NAME ${dt27_test} COMMAND ${dt27_test})
	set_tests_properties(${dt27_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# Severed limbs as ground items (Simulation::onLimbSevered, CombatArena::torture / eat; rwh42): a hit, the peeler, Dismemberment 0, the eat path, the save.
add_executable(ksim_char_names tests/ksim_char_names.cpp)
target_link_libraries(ksim_char_names PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_char_names PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_char_names)
add_test(NAME ksim_char_names COMMAND ksim_char_names ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_char_names PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_limb_tint tests/ksim_limb_tint.cpp)
target_link_libraries(ksim_limb_tint PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_limb_tint PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_limb_tint)
add_test(NAME ksim_limb_tint COMMAND ksim_limb_tint ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_limb_tint PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_severed_limbs tests/ksim_severed_limbs.cpp)
target_link_libraries(ksim_severed_limbs PRIVATE ksim)
target_compile_definitions(ksim_severed_limbs PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_severed_limbs)
add_test(NAME ksim_severed_limbs COMMAND ksim_severed_limbs)
set_tests_properties(ksim_severed_limbs PROPERTIES SKIP_RETURN_CODE 77)

# The small-task executors (task ds/small-tasks-2): 28, 102 / 159, 164 and 165 on the dt27 rig; ksim_animals (task port-animals): 238 / 259 / 261 / 277 - 279; ksim_raid_loot (task raid-food-loot): 131 / 284 for a raider.
foreach(small_test ksim_small_exec ksim_animals ksim_standing ksim_raid_loot)
	add_executable(${small_test} tests/${small_test}.cpp)
	target_link_libraries(${small_test} PRIVATE ksim)
	target_compile_definitions(${small_test} PRIVATE KENSHI_DATA="${KENSHI_DATA}")
	kdata_warnings(${small_test})
	add_test(NAME ${small_test} COMMAND ${small_test})
	set_tests_properties(${small_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# Lock state in the save (task locks-save): an opened cage, a half-cut and a half-picked pair of shackles round-trip; a v86 save loads untouched.
add_executable(ksim_crops tests/ksim_crops.cpp)
target_link_libraries(ksim_crops PRIVATE ksim)
target_compile_definitions(ksim_crops PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_crops)
add_test(NAME ksim_crops COMMAND ksim_crops)
set_tests_properties(ksim_crops PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_save_mid_order tests/ksim_save_mid_order.cpp)
target_link_libraries(ksim_save_mid_order PRIVATE ksim)
target_compile_definitions(ksim_save_mid_order PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_save_mid_order)
# Parts (kenshi-k16.5.21): argv[1..] name the cases (each runs reloaded and fresh).
add_test(NAME ksim_save_mid_order_a COMMAND ksim_save_mid_order arrest arrestWaits huntBounties smash)
add_test(NAME ksim_save_mid_order_b COMMAND ksim_save_mid_order build lock crops)
add_test(NAME ksim_save_mid_order_c COMMAND ksim_save_mid_order standing limbPlay alarmWait)
foreach(c a b c)
	set_tests_properties(ksim_save_mid_order_${c} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
# Assault -> Terrorism (task ds/terrorism, kinspect dt43): a diplomat in its faction's territory.
add_executable(ksim_terrorism tests/ksim_terrorism.cpp)
target_link_libraries(ksim_terrorism PRIVATE ksim)
target_compile_definitions(ksim_terrorism PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_terrorism)
add_test(NAME ksim_terrorism COMMAND ksim_terrorism)
set_tests_properties(ksim_terrorism PROPERTIES SKIP_RETURN_CODE 77)

# The G1 package Board in the save (task ds/board-save): the contract, the change-AI table, the applied package and running away round-trip.
# kenshi-k16.6.82: a standing character loads standing (Simulation::reload), not down.
add_executable(ksim_load_pose tests/ksim_load_pose.cpp)
target_link_libraries(ksim_load_pose PRIVATE ksim)
target_compile_definitions(ksim_load_pose PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_load_pose)
add_test(NAME ksim_load_pose COMMAND ksim_load_pose ${KSIM_NAVWORLD})
set_tests_properties(ksim_load_pose PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_slice_load_parity tests/ksim_slice_load_parity.cpp)
target_link_libraries(ksim_slice_load_parity PRIVATE ksim)
target_compile_definitions(ksim_slice_load_parity PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_slice_load_parity)
add_test(NAME ksim_slice_load_parity COMMAND ksim_slice_load_parity)
set_tests_properties(ksim_slice_load_parity PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_board_save tests/ksim_board_save.cpp)
target_link_libraries(ksim_board_save PRIVATE ksim)
target_compile_definitions(ksim_board_save PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_board_save)
add_test(NAME ksim_board_save COMMAND ksim_board_save)
set_tests_properties(ksim_board_save PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_lock_save tests/ksim_lock_save.cpp)
target_link_libraries(ksim_lock_save PRIVATE ksim)
target_compile_definitions(ksim_lock_save PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_lock_save)
add_test(NAME ksim_lock_save COMMAND ksim_lock_save)
set_tests_properties(ksim_lock_save PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_provenance tests/ksim_provenance.cpp)
target_link_libraries(ksim_provenance PRIVATE ksim)
target_compile_definitions(ksim_provenance PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_provenance)
add_test(NAME ksim_provenance COMMAND ksim_provenance)
set_tests_properties(ksim_provenance PROPERTIES SKIP_RETURN_CODE 77)
# Task ds/loot-storage: 184 PUT_LOOT_IN_STORAGE (ksim_loot_storage). Task ds/machines: the machine-work goals (113 / 152 pull, 93 / 124 / 88 / 114 / 242 store). Bare worlds.
foreach(dt27_test ksim_machines ksim_loot_storage)
	add_executable(${dt27_test} tests/${dt27_test}.cpp)
	target_link_libraries(${dt27_test} PRIVATE ksim)
	target_compile_definitions(${dt27_test} PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(${dt27_test})
	add_test(NAME ${dt27_test} COMMAND ${dt27_test})
	set_tests_properties(${dt27_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# Lock tasks part 2 (task locks-save): the lift twins 170 / 213 / 225 / 240 / 246, 203, 187, 272 as planner rows. Pure.
add_executable(ksim_locks2 tests/ksim_locks2.cpp)
target_link_libraries(ksim_locks2 PRIVATE ksim)
kdata_warnings(ksim_locks2)
add_test(NAME ksim_locks2 COMMAND ksim_locks2)

# Task ds/construction: the construction tasks 2 / 71 / 95 / 96 / 97 / 156 / 200 on player buildings. Bare worlds.
foreach(dt27_test ksim_construction)
	add_executable(${dt27_test} tests/${dt27_test}.cpp)
	target_link_libraries(${dt27_test} PRIVATE ksim)
	target_compile_definitions(${dt27_test} PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(${dt27_test})
	add_test(NAME ${dt27_test} COMMAND ${dt27_test})
	set_tests_properties(${dt27_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# Task ds/jobs-3: the Engineer (125) and Robotics (57) job rows. Bare worlds.
foreach(dt27_test ksim_jobs3)
	add_executable(${dt27_test} tests/${dt27_test}.cpp)
	target_link_libraries(${dt27_test} PRIVATE ksim)
	target_compile_definitions(${dt27_test} PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(${dt27_test})
	add_test(NAME ${dt27_test} COMMAND ${dt27_test})
	set_tests_properties(${dt27_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# Task ds/machines-2: the jam goals 89 / 90 / 91 and 241. Bare worlds.
foreach(dt27_test ksim_machines2)
	add_executable(${dt27_test} tests/${dt27_test}.cpp)
	target_link_libraries(${dt27_test} PRIVATE ksim)
	target_compile_definitions(${dt27_test} PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(${dt27_test})
	add_test(NAME ${dt27_test} COMMAND ${dt27_test})
	set_tests_properties(${dt27_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

add_executable(ksim_ranged_plan tests/ksim_ranged_plan.cpp)
target_link_libraries(ksim_ranged_plan PRIVATE ksim)
kdata_warnings(ksim_ranged_plan)
add_test(NAME ksim_ranged_plan COMMAND ksim_ranged_plan)
add_executable(ksim_ranged_fire tests/ksim_ranged_fire.cpp)
target_link_libraries(ksim_ranged_fire PRIVATE ksim)
target_compile_definitions(ksim_ranged_fire PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_ranged_fire)
add_test(NAME ksim_ranged_fire COMMAND ksim_ranged_fire)
add_executable(ksim_crossbow_templates tests/ksim_crossbow_templates.cpp)
target_link_libraries(ksim_crossbow_templates PRIVATE ksim)
target_compile_definitions(ksim_crossbow_templates PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_crossbow_templates)
add_test(NAME ksim_crossbow_templates COMMAND ksim_crossbow_templates)
add_executable(ksim_ranged_goal tests/ksim_ranged_goal.cpp)
target_link_libraries(ksim_ranged_goal PRIVATE ksim)
target_compile_definitions(ksim_ranged_goal PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_ranged_goal)
add_test(NAME ksim_ranged_goal COMMAND ksim_ranged_goal)
add_executable(ksim_turret tests/ksim_turret.cpp)
target_link_libraries(ksim_turret PRIVATE ksim)
target_compile_definitions(ksim_turret PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_turret)
add_test(NAME ksim_turret COMMAND ksim_turret)
add_executable(ksim_stealth_ko tests/ksim_stealth_ko.cpp)
target_link_libraries(ksim_stealth_ko PRIVATE ksim)
target_compile_definitions(ksim_stealth_ko PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_stealth_ko)
add_test(NAME ksim_stealth_ko COMMAND ksim_stealth_ko)

add_executable(ksim_small_tasks tests/ksim_small_tasks.cpp)
target_link_libraries(ksim_small_tasks PRIVATE ksim)
kdata_warnings(ksim_small_tasks)
add_test(NAME ksim_small_tasks COMMAND ksim_small_tasks)

add_executable(ksim_leg_sever_ko tests/ksim_leg_sever_ko.cpp)
target_link_libraries(ksim_leg_sever_ko PRIVATE ksim)
target_compile_definitions(ksim_leg_sever_ko PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_leg_sever_ko)
add_test(NAME ksim_leg_sever_ko COMMAND ksim_leg_sever_ko)

# Prospecting (SIMLINK.md order 79, PROSPECT 35): the trap / N / r / falloff maths, the zone table, the order and its cancel paths, the 1.0 s duration, frame 35, save and continue.
add_executable(ksim_prospect tests/ksim_prospect.cpp)
target_link_libraries(ksim_prospect PRIVATE ksim)
target_compile_definitions(ksim_prospect PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_prospect)
add_test(NAME ksim_prospect COMMAND ksim_prospect ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_prospect PROPERTIES FIXTURES_REQUIRED knav_cache)

# Map discovery (SIMLINK.md DISCOVERED 37, event 19): the radius per town type, visited, the view focus term, frame 37, save and continue.
add_executable(ksim_discovery tests/ksim_discovery.cpp)
target_link_libraries(ksim_discovery PRIVATE ksim)
target_compile_definitions(ksim_discovery PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_discovery)
add_test(NAME ksim_discovery COMMAND ksim_discovery ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_discovery PROPERTIES FIXTURES_REQUIRED knav_cache)

# TOWN_STATE 66 (SIMLINK.md): visited towns only, the owner, the resident count, the 5 s rate limit, the placeholder bits.
add_executable(ksim_town_state tests/ksim_town_state.cpp)
target_link_libraries(ksim_town_state PRIVATE ksim)
target_compile_definitions(ksim_town_state PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_state)
add_test(NAME ksim_town_state COMMAND ksim_town_state ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_town_state PROPERTIES FIXTURES_REQUIRED knav_cache)

# TASK_PROGRESS 39 and event 20 lock_roll (SIMLINK.md): lockpick and prospect bars, the reset at each roll, the end, an interruption, two characters, save and continue.
add_executable(ksim_taskprogress tests/ksim_taskprogress.cpp)
target_link_libraries(ksim_taskprogress PRIVATE ksim)
target_compile_definitions(ksim_taskprogress PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_taskprogress)
add_test(NAME ksim_taskprogress COMMAND ksim_taskprogress ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_taskprogress PROPERTIES FIXTURES_REQUIRED knav_cache)

# Order 60 actions 15 / 16 / 17 (pick, cut, break worn shackles), ITEM_LOCK 57, TASK_PROGRESS kinds 1 and 7, event 13 (SIMLINK.md; kenshi-h2v.15.1).
add_executable(ksim_shackle_orders tests/ksim_shackle_orders.cpp)
target_link_libraries(ksim_shackle_orders PRIVATE ksim)
target_compile_definitions(ksim_shackle_orders PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_shackle_orders)
add_test(NAME ksim_shackle_orders COMMAND ksim_shackle_orders ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_shackle_orders PROPERTIES FIXTURES_REQUIRED knav_cache)

# Venge sky beams (VENGE_BEAMS_STATIC.md, SIMLINK.md BEAMS 58): schedule, day gating, burning damage, the frame, save and load.
add_executable(ksim_venge_beams tests/ksim_venge_beams.cpp)
target_link_libraries(ksim_venge_beams PRIVATE ksim)
target_compile_definitions(ksim_venge_beams PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_venge_beams)
add_test(NAME ksim_venge_beams COMMAND ksim_venge_beams ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_venge_beams PROPERTIES FIXTURES_REQUIRED knav_cache)
add_executable(ksim_venge_population tests/ksim_venge_population.cpp)
target_link_libraries(ksim_venge_population PRIVATE ksim)
target_compile_definitions(ksim_venge_population PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_venge_population)
add_test(NAME ksim_venge_population COMMAND ksim_venge_population)

# FACTION_RELATIONS 38 and order 85 faction_rename (SIMLINK.md): frame 38, the rename answers, PLAYER_SQUADS, squad_add, replay, save.
add_executable(ksim_factionrel tests/ksim_factionrel.cpp)
target_link_libraries(ksim_factionrel PRIVATE ksim)
target_compile_definitions(ksim_factionrel PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_factionrel)
add_test(NAME ksim_factionrel COMMAND ksim_factionrel ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_factionrel PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_clock tests/ksim_clock.cpp)
target_link_libraries(ksim_clock PRIVATE ksim)
kdata_warnings(ksim_clock)
add_test(NAME ksim_clock COMMAND ksim_clock)

add_executable(ksim_spawn tests/ksim_spawn.cpp)
target_link_libraries(ksim_spawn PRIVATE ksim)
target_compile_definitions(ksim_spawn PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_spawn)
add_test(NAME ksim_spawn COMMAND ksim_spawn)

# Region-owned two-phase tick (stand-in workload, no game data needed).
foreach(test_name IN ITEMS
	ksim_regions_determinism
	ksim_regions_ownership
	ksim_regions_migration
	ksim_regions_perception
	ksim_regions_areas
	ksim_regions_target
	ksim_regions_squads
	ksim_regions_perf
)
	add_executable(${test_name} tests/${test_name}.cpp)
	target_link_libraries(${test_name} PRIVATE ksim)
	kdata_warnings(${test_name})
	add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ---------------------------------------------------------------------------
# knav — Recast/Detour navmesh (offline bake, cache file, path queries)
# ---------------------------------------------------------------------------

# Separate from ksim: the bake needs kworld only, and the sim links just the
# query half it needs. Recast/Detour 1.6.0 is installed system-wide.
include(cmake/recast.cmake)

# Clipper2 2.0.1 (Boost licence, third_party/clipper2-2.0.1/README): exact integer polygon booleans and offsets for knav::surfgen (kenshi-ffmi.4).
add_library(clipper2 STATIC
	third_party/clipper2-2.0.1/src/clipper.engine.cpp
	third_party/clipper2-2.0.1/src/clipper.offset.cpp
	third_party/clipper2-2.0.1/src/clipper.rectclip.cpp
	third_party/clipper2-2.0.1/src/clipper.triangulation.cpp
)
target_include_directories(clipper2 SYSTEM PUBLIC third_party/clipper2-2.0.1/include)

add_library(knav
	src/knav/config.cpp
	src/knav/navmesh.cpp
	src/knav/door.cpp
	src/knav/coarse.cpp
	src/knav/walk.cpp
	src/knav/native.cpp
	src/knav/navgeom.cpp
	src/knav/compare.cpp
	src/knav/native_tiles.cpp
	src/knav/surfgen.cpp
	src/knav/ground.cpp
	src/knav/zone_gen.cpp
	src/knav/seeds.cpp
	src/knav/route.cpp
)
target_include_directories(knav PUBLIC include)
target_link_libraries(knav PUBLIC kworld RecastNavigation::Detour PRIVATE
	Threads::Threads clipper2)
kdata_warnings(knav)

# knav_bake caches its results by input (NAVMESH_CACHE.md "Bake cache"). KNAV_BAKE_CACHE: the cache root compiled in
# (empty = off; the KNAV_BAKE_CACHE env var and --cache override it per run).
set(KNAV_BAKE_CACHE "" CACHE PATH "knav_bake result cache directory (empty = off)")
# The source hash is generated at build time (any generator), from the contents of the sources knav_bake is built from.
file(GLOB_RECURSE KNAV_BAKE_SRCS CONFIGURE_DEPENDS src/knav/* include/knav/* src/kworld/* include/kworld/* src/kutil/* include/kutil/*
	src/kdata/* include/kdata/* third_party/clipper2-2.0.1/*)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/knav_bake_gen/knav_bake_srchash.h
	COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/knav_bake_gen/knav_bake_srchash.h -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
		-DBUILDID=${CMAKE_CURRENT_BINARY_DIR}/knav_bake_gen/buildid.txt
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/knav_srchash.cmake
	DEPENDS ${KNAV_BAKE_SRCS} src/format.cpp src/gamedata.cpp tools/knav_bake.cpp tools/knav_bake_cache.hpp cmake/recast.cmake
		cmake/knav_srchash.cmake cmake/knav_buildid.cmake ${CMAKE_CURRENT_BINARY_DIR}/knav_bake_gen/buildid.txt
	COMMENT "knav_bake source hash")
add_executable(knav_bake tools/knav_bake.cpp ${CMAKE_CURRENT_BINARY_DIR}/knav_bake_gen/knav_bake_srchash.h)
target_include_directories(knav_bake PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/knav_bake_gen)
target_compile_definitions(knav_bake PRIVATE KNAV_BAKE_CACHE_DEFAULT="${KNAV_BAKE_CACHE}")
target_link_libraries(knav_bake PRIVATE knav RecastNavigation::Detour)
include(cmake/knav_buildid.cmake)	# compiler / flags / Recast identity -> buildid.txt, hashed into the key (kenshi-k16.15.45)
# A top-down picture of a world bake over a box (areas, pruned polygons, routes).
add_executable(knav_draw tools/knav_draw.cpp)
target_link_libraries(knav_draw PRIVATE knav RecastNavigation::Detour)
target_compile_definitions(knav_draw PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_draw)
# World-wide count of the open walkable sheet faces between the walk and stair slope limits, against the shipped navtiles (k16.7.58).
add_executable(knav_slope_probe tools/knav_slope_probe.cpp)
target_link_libraries(knav_slope_probe PRIVATE knav RecastNavigation::Detour)
target_compile_definitions(knav_slope_probe PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_slope_probe)

# kenshi-k16.7.187: the placed-object census against our ground grid (a maintainer tool: reads the data dir, writes TSVs).
add_executable(knav_object_census tools/knav_object_census.cpp)
target_link_libraries(knav_object_census PRIVATE knav)
target_compile_definitions(knav_object_census PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_object_census)
# kenshi-k16.7.213: terrain-only first pass and straightening probe (maintainer tool).
add_executable(knav_terrain_first tools/knav_terrain_first.cpp)
target_link_libraries(knav_terrain_first PRIVATE knav)
target_compile_definitions(knav_terrain_first PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_terrain_first)
add_executable(knav_object_probe tools/knav_object_probe.cpp)
target_link_libraries(knav_object_probe PRIVATE knav)
target_compile_definitions(knav_object_probe PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_object_probe)
target_compile_definitions(knav_bake PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_bake)

# The original's shipped navtiles as Detour region files (the runtime base mesh, NAVMESH_CACHE "Runtime mesh").
add_executable(knav_native tools/knav_native.cpp)
target_link_libraries(knav_native PRIVATE knav RecastNavigation::Detour)
target_compile_definitions(knav_native PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_native)
add_executable(knav_ground_vs_shipped tools/knav_ground_vs_shipped.cpp)
target_link_libraries(knav_ground_vs_shipped PRIVATE knav)
target_compile_definitions(knav_ground_vs_shipped PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_ground_vs_shipped)
add_executable(knav_diag_check tools/knav_diag_check.cpp)
target_link_libraries(knav_diag_check PRIVATE knav)
target_compile_definitions(knav_diag_check PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_diag_check)
add_executable(knav_hkt_dump tools/knav_hkt_dump.cpp)
target_link_libraries(knav_hkt_dump PRIVATE knav)
target_compile_definitions(knav_hkt_dump PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_hkt_dump)

add_executable(knav_navhash tools/knav_navhash.cpp)
target_link_libraries(knav_navhash PRIVATE kworld knav)
target_compile_definitions(knav_navhash PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_navhash)

if(KSIM_DEV)
# kenshi-k16.7.128: the geometric stale test over the shipped tiles with a stale hash.
	add_executable(knav_stalegeom tools/knav_stalegeom.cpp)
	target_link_libraries(knav_stalegeom PRIVATE ksim knav RecastNavigation::Detour)
	target_compile_definitions(knav_stalegeom PRIVATE KENSHI_DATA="${KENSHI_DATA}")
	kdata_warnings(knav_stalegeom)
endif()
add_test(NAME knav_navhash COMMAND knav_navhash --check)
set_tests_properties(knav_navhash PROPERTIES SKIP_RETURN_CODE 77)

# Our bake against the original's baked navmesh (knav/native.hpp); heavy over a whole zone, run it on the dev box.
add_executable(knav_compare tools/knav_compare.cpp)
target_link_libraries(knav_compare PRIVATE knav RecastNavigation::Detour)
target_compile_definitions(knav_compare PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_compare)

add_executable(knav_graph tools/knav_graph.cpp)
target_link_libraries(knav_graph PRIVATE knav)
target_compile_definitions(knav_graph PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_graph)



add_executable(knav_coarse tests/knav_coarse.cpp)
target_link_libraries(knav_coarse PRIVATE knav)
target_compile_definitions(knav_coarse PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_coarse)

add_executable(knav_hub tests/knav_hub.cpp)
target_link_libraries(knav_hub PRIVATE knav)
target_compile_definitions(knav_hub PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_hub)



add_executable(knav_water tests/knav_water.cpp)
target_link_libraries(knav_water PRIVATE knav RecastNavigation::Detour)
target_compile_definitions(knav_water PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_water)

add_executable(knav_snap tests/knav_snap.cpp)
target_link_libraries(knav_snap PRIVATE knav RecastNavigation::Detour)
kdata_warnings(knav_snap)


add_executable(knav_door tests/knav_door.cpp)
target_link_libraries(knav_door PRIVATE knav RecastNavigation::Detour)
target_compile_definitions(knav_door PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_door)

add_executable(knav_door_occupied tests/knav_door_occupied.cpp)
target_link_libraries(knav_door_occupied PRIVATE knav RecastNavigation::Detour)
target_compile_definitions(knav_door_occupied PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_door_occupied)

add_executable(knav_gate_leaf tests/knav_gate_leaf.cpp)
target_link_libraries(knav_gate_leaf PRIVATE knav RecastNavigation::Detour)
kdata_warnings(knav_gate_leaf)
add_test(NAME knav_gate_leaf COMMAND knav_gate_leaf ${KSIM_NAVWORLD})
set_tests_properties(knav_gate_leaf PROPERTIES SKIP_RETURN_CODE 77)

# The bake runs twice (4 threads, 1 thread); the files must be identical.
add_test(NAME knav_bake_hub
	COMMAND knav_bake --out ${CMAKE_CURRENT_BINARY_DIR}/hub.knav --threads 4)
# Stack (zone 19,29), a walled town whose gates have doors: the Hub's gates are ruins (no doors), so the gate tests use this.
add_test(NAME knav_bake_stack
	COMMAND knav_bake --out ${CMAKE_CURRENT_BINARY_DIR}/stack.knav --zones 18 28 20 30 --threads 4)
add_test(NAME knav_bake_hub_single
	COMMAND knav_bake --out ${CMAKE_CURRENT_BINARY_DIR}/hub_single.knav --threads 1)
# Cached and uncached bakes into one output dir (k16.15.47): both succeed, the cache entry is untouched.
add_test(NAME knav_bake_cache_reuse
	COMMAND ${CMAKE_COMMAND} -DBAKE=$<TARGET_FILE:knav_bake> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/bake_cache_reuse
		-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/knav_bake_cache_reuse.cmake)
if(WIN32)
	set_tests_properties(knav_bake_cache_reuse PROPERTIES DISABLED TRUE)	# the bake cache is POSIX-only (tools/knav_bake_cache.hpp, kenshi-k16.6.804)
endif()
add_test(NAME knav_deterministic
	COMMAND ${CMAKE_COMMAND} -E compare_files
		${CMAKE_CURRENT_BINARY_DIR}/hub.knav ${CMAKE_CURRENT_BINARY_DIR}/hub_single.knav)
add_test(NAME knav_hub COMMAND knav_hub ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
add_test(NAME knav_door COMMAND knav_door ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
add_test(NAME knav_door_occupied COMMAND knav_door_occupied ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
add_executable(knav_storm_house tests/knav_storm_house.cpp)
target_link_libraries(knav_storm_house PRIVATE knav)
target_compile_definitions(knav_storm_house PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_storm_house)
add_test(NAME knav_storm_house COMMAND knav_storm_house ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(knav_storm_house PROPERTIES FIXTURES_REQUIRED knav_cache)
add_executable(knav_interior_walk tests/knav_interior_walk.cpp)
target_link_libraries(knav_interior_walk PRIVATE ksim)
target_compile_definitions(knav_interior_walk PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_interior_walk)
add_test(NAME knav_interior_walk COMMAND knav_interior_walk ${KSIM_NAVWORLD})
# Geometric navmesh generator (knav/surfgen.hpp) on the Hub bar and the Longhouse (kenshi-k16.7.131).
add_executable(knav_surfgen tests/knav_surfgen.cpp)
target_link_libraries(knav_surfgen PRIVATE knav)
target_compile_definitions(knav_surfgen PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_surfgen)
add_test(NAME knav_surfgen COMMAND knav_surfgen)
# The ground navmesh generator (knav/ground.hpp): invariants on zone 46.16 and the Heft patch (kenshi-k16.7.186).
add_executable(knav_ground tests/knav_ground.cpp)
target_link_libraries(knav_ground PRIVATE knav)
target_compile_definitions(knav_ground PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_ground)
add_test(NAME knav_ground COMMAND knav_ground)
add_executable(knav_ground_features tests/knav_ground_features.cpp)
target_link_libraries(knav_ground_features PRIVATE knav)
target_compile_definitions(knav_ground_features PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_ground_features)
add_test(NAME knav_ground_features COMMAND knav_ground_features)
add_executable(knav_ground_objects tests/knav_ground_objects.cpp)
target_link_libraries(knav_ground_objects PRIVATE knav)
target_compile_definitions(knav_ground_objects PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_ground_objects)
add_test(NAME knav_ground_objects COMMAND knav_ground_objects)
add_executable(knav_ground_buildings tests/knav_ground_buildings.cpp)
target_link_libraries(knav_ground_buildings PRIVATE knav)
target_compile_definitions(knav_ground_buildings PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_ground_buildings)
add_test(NAME knav_ground_buildings COMMAND knav_ground_buildings)
add_executable(knav_cutover tests/knav_cutover.cpp)
target_link_libraries(knav_cutover PRIVATE knav clipper2)
target_compile_definitions(knav_cutover PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_cutover)
add_test(NAME knav_cutover COMMAND knav_cutover)

add_executable(knav_longhouse_door tests/knav_longhouse_door.cpp)
target_link_libraries(knav_longhouse_door PRIVATE knav)
target_compile_definitions(knav_longhouse_door PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_longhouse_door)
add_test(NAME knav_longhouse_door COMMAND knav_longhouse_door)
add_executable(knav_outpost_floor2_bake tests/knav_outpost_floor2_bake.cpp)
target_link_libraries(knav_outpost_floor2_bake PRIVATE knav)
target_compile_definitions(knav_outpost_floor2_bake PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_outpost_floor2_bake)
add_test(NAME knav_outpost_floor2_bake COMMAND knav_outpost_floor2_bake)
add_test(NAME knav_snap COMMAND knav_snap ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
add_executable(knav_walk tests/knav_walk.cpp)
target_link_libraries(knav_walk PRIVATE knav)
target_compile_definitions(knav_walk PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_walk)
add_test(NAME knav_walk COMMAND knav_walk ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(knav_walk PROPERTIES FIXTURES_REQUIRED knav_cache)
add_executable(knav_furniture tests/knav_furniture.cpp)
target_link_libraries(knav_furniture PRIVATE knav)
target_compile_definitions(knav_furniture PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_furniture)
add_test(NAME knav_furniture COMMAND knav_furniture ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(knav_furniture PROPERTIES FIXTURES_REQUIRED knav_cache)
set_tests_properties(knav_snap PROPERTIES FIXTURES_REQUIRED knav_cache)
set_tests_properties(knav_door PROPERTIES FIXTURES_REQUIRED knav_cache)
set_tests_properties(knav_door_occupied PROPERTIES FIXTURES_REQUIRED knav_cache)
set_tests_properties(knav_bake_hub PROPERTIES FIXTURES_SETUP knav_cache)
set_tests_properties(knav_bake_hub_single PROPERTIES FIXTURES_SETUP knav_cache_single)
set_tests_properties(knav_bake_stack PROPERTIES FIXTURES_SETUP knav_cache_stack)
set_tests_properties(knav_hub PROPERTIES FIXTURES_REQUIRED knav_cache)
set_tests_properties(knav_deterministic PROPERTIES FIXTURES_REQUIRED "knav_cache;knav_cache_single")
# The shipped navtiles converted to Detour round the Hub: area, connectivity, the bar stair (reads the install at run time).
# The runtime native loader (MeshNavigator::openNative): the Hub streamed from the shipped navtiles, the bar route at radius 4,
# a walked route (docs/formats/NAVMESH_CACHE.md "Runtime mesh"). Reads the install at run time; - as argv[1] = an optional
# world bake to compare load time and RSS with.
add_executable(ksim_path_walk tests/ksim_path_walk.cpp)
target_link_libraries(ksim_path_walk PRIVATE ksim)
target_compile_definitions(ksim_path_walk PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_path_walk)
add_test(NAME ksim_path_walk COMMAND ksim_path_walk)
set_tests_properties(ksim_path_walk PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_hub_west_corridor tests/ksim_hub_west_corridor.cpp)
target_link_libraries(ksim_hub_west_corridor PRIVATE ksim)
target_compile_definitions(ksim_hub_west_corridor PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_hub_west_corridor)
add_test(NAME ksim_hub_west_corridor COMMAND ksim_hub_west_corridor)
set_tests_properties(ksim_hub_west_corridor PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_move_detour tests/ksim_move_detour.cpp)
target_link_libraries(ksim_move_detour PRIVATE ksim)
target_compile_definitions(ksim_move_detour PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_move_detour)
add_test(NAME ksim_move_detour COMMAND ksim_move_detour)
set_tests_properties(ksim_move_detour PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_stand_building tests/ksim_stand_building.cpp)
target_link_libraries(ksim_stand_building PRIVATE ksim)
target_compile_definitions(ksim_stand_building PRIVATE KENSHI_DATA="${KENSHI_DATA}" KSIM_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
kdata_warnings(ksim_stand_building)
add_test(NAME ksim_stand_building COMMAND ksim_stand_building)
set_tests_properties(ksim_stand_building PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_native_nav tests/ksim_native_nav.cpp)
target_link_libraries(ksim_native_nav PRIVATE ksim)
target_compile_definitions(ksim_native_nav PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_native_nav)
add_test(NAME ksim_native_nav COMMAND ksim_native_nav)
set_tests_properties(ksim_native_nav PROPERTIES SKIP_RETURN_CODE 77)

# The original's stale-tile check in the runtime loader (NAVMESH_CACHE.md "Runtime mesh"): the pinned zone and interior
# decisions (zone 10,10 shipped, the Hub rebaked, one interior of each kind). Reads the install at run time.
add_executable(ksim_native_stale tests/ksim_native_stale.cpp)
target_link_libraries(ksim_native_stale PRIVATE ksim)
target_compile_definitions(ksim_native_stale PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_native_stale)
add_test(NAME ksim_native_stale COMMAND ksim_native_stale)
set_tests_properties(ksim_native_stale PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_nav_resident tests/ksim_nav_resident.cpp)
target_link_libraries(ksim_nav_resident PRIVATE ksim)
target_compile_definitions(ksim_nav_resident PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_nav_resident)
add_test(NAME ksim_nav_resident COMMAND ksim_nav_resident)
set_tests_properties(ksim_nav_resident PROPERTIES SKIP_RETURN_CODE 77)
add_executable(knav_components_incremental tests/knav_components_incremental.cpp)
target_link_libraries(knav_components_incremental PRIVATE ksim)
target_compile_definitions(knav_components_incremental PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_components_incremental)
add_test(NAME knav_components_incremental COMMAND knav_components_incremental)
set_tests_properties(knav_components_incremental PROPERTIES SKIP_RETURN_CODE 77)

# A heightless goal prefers the start's connected set; a walker off the mesh or in a smaller set is re-seated (PATHFINDING_STATIC 1.11).

# The query-time clearance (NAVMESH_CACHE.md "Clearance"): the mesh is built in the test (no bake, no game data), two
# walls across a floor. Radius 0 walks a 7 u gap, a radius 4 body refuses it (under the original's 9 u) and takes 12 u.

add_executable(knav_first_corner tests/knav_first_corner.cpp)
target_link_libraries(knav_first_corner PRIVATE knav)
kdata_warnings(knav_first_corner)
add_test(NAME knav_first_corner COMMAND knav_first_corner)

add_executable(ksim_path_corridor_save tests/ksim_path_corridor_save.cpp)
target_link_libraries(ksim_path_corridor_save PRIVATE ksim)
target_compile_definitions(ksim_path_corridor_save PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_path_corridor_save)
add_test(NAME ksim_path_corridor_save COMMAND ksim_path_corridor_save)
set_tests_properties(ksim_path_corridor_save PROPERTIES SKIP_RETURN_CODE 77)


# The native patch cache (NAVMESH_CACHE.md "Patch cache"): a cold open and a warm one of the Hub (same mesh, the warm one
# faster and from the cache), one changed zone rebuilding only its own entry, a corrupt entry rebuilt, and the cache off.
add_executable(ksim_native_cache tests/ksim_native_cache.cpp)
target_link_libraries(ksim_native_cache PRIVATE ksim)
target_compile_definitions(ksim_native_cache PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_native_cache)
add_test(NAME ksim_native_cache COMMAND ksim_native_cache)
set_tests_properties(ksim_native_cache PROPERTIES SKIP_RETURN_CODE 77)

# The cache root keeps the newest 3 recipe trees (NAVMESH_CACHE.md "Prune"; kenshi-k16.7.101).
add_executable(ksim_nav_cache_prune tests/ksim_nav_cache_prune.cpp)
target_link_libraries(ksim_nav_cache_prune PRIVATE ksim)
kdata_warnings(ksim_nav_cache_prune)
add_test(NAME ksim_nav_cache_prune COMMAND ksim_nav_cache_prune)

# The nav cache's canary: a fresh bake of one zone, its digest pinned beside kNativeCacheRecipe (kenshi-k16.7.67).
add_executable(ksim_nav_cache_canary tests/ksim_nav_cache_canary.cpp)
target_link_libraries(ksim_nav_cache_canary PRIVATE ksim)
target_compile_definitions(ksim_nav_cache_canary PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_nav_cache_canary)
# The cache tree key folds the bake source hash (injected here, kenshi-k16.7.161).
add_executable(ksim_nav_cache_key tests/ksim_nav_cache_key.cpp)
target_link_libraries(ksim_nav_cache_key PRIVATE ksim)
target_compile_definitions(ksim_nav_cache_key PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_nav_cache_key)
add_test(NAME ksim_nav_cache_key COMMAND ksim_nav_cache_key)
set_tests_properties(ksim_nav_cache_key PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME ksim_nav_cache_canary COMMAND ksim_nav_cache_canary)
set_tests_properties(ksim_nav_cache_canary PROPERTIES SKIP_RETURN_CODE 77)

# gate_heft open-sand hole (kenshi-k16.7.154): the hole is in the shipped tile and the bake alike; pins it and a near-straight leg beside it
add_executable(ksim_gate_heft_hole tests/ksim_gate_heft_hole.cpp)
target_link_libraries(ksim_gate_heft_hole PRIVATE ksim)
target_compile_definitions(ksim_gate_heft_hole PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_gate_heft_hole)
add_test(NAME ksim_gate_heft_hole COMMAND ksim_gate_heft_hole ${KSIM_NAVWORLD})
set_tests_properties(ksim_gate_heft_hole PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_nav_islands tests/ksim_nav_islands.cpp)
target_link_libraries(ksim_nav_islands PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_nav_islands PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_nav_islands)
# 7.5 GB peak (k16.6.290): one at a time on the host, across ctest runs (CI, rc, workers), not only within one (RUN_SERIAL). flock waits;
# the lock is held only for this test, so it cannot cycle with remote_ctest's ctest lock (kenshi-k16.6.336, kenshi-80j.29).
find_program(KSIM_FLOCK flock)
if(KSIM_FLOCK)
	add_test(NAME ksim_nav_islands COMMAND ${KSIM_FLOCK} /tmp/ksim_nav_islands.lock $<TARGET_FILE:ksim_nav_islands> ${CMAKE_BINARY_DIR}/nav_islands)
else()
	add_test(NAME ksim_nav_islands COMMAND ksim_nav_islands ${CMAKE_BINARY_DIR}/nav_islands)
endif()
# RUN_SERIAL: the world graph peaks at 7.6 GB RSS (k16.7.43), which beside a -j12 suite's other tests on the 31 GB dev box can
# exhaust memory; alone it costs the suite about its own wall time (4 min cached).
set_tests_properties(ksim_nav_islands PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600 RUN_SERIAL TRUE)
# kenshi-k16.5.21: a whole-world union-find (not splittable): 350 s alone on a warm cache, 873 s in the queue on a cold one. Label `soak`: out of the test queue.
set_property(TEST ksim_nav_islands APPEND PROPERTY LABELS soak)

# The shipped tile as a two-sided gate for the patch bake (kenshi-k16.7.66); shares the island test's nav cache tree.

add_executable(ksim_door_sweep tests/ksim_door_sweep.cpp)
target_link_libraries(ksim_door_sweep PRIVATE ksim)
target_compile_definitions(ksim_door_sweep PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_door_sweep)
add_test(NAME ksim_door_sweep COMMAND ksim_door_sweep ${CMAKE_BINARY_DIR}/door_sweep)
set_tests_properties(ksim_door_sweep PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 RUN_SERIAL TRUE)

add_executable(knav_native_tiles tests/knav_native_tiles.cpp)
target_link_libraries(knav_native_tiles PRIVATE knav RecastNavigation::Detour)
target_compile_definitions(knav_native_tiles PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_native_tiles)
add_test(NAME knav_native_tiles COMMAND knav_native_tiles)
add_test(NAME knav_water COMMAND knav_water)
# Nine world regions around the Hub (4 x 4 zones each, ~1 minute): the coarse graph is built over them.
add_test(NAME knav_bake_world_sample
	COMMAND knav_bake --world ${CMAKE_CURRENT_BINARY_DIR}/world_sample --regions 4 7 6 9 --threads 4)
add_test(NAME knav_coarse COMMAND knav_coarse ${CMAKE_CURRENT_BINARY_DIR}/world_sample 4 7 6 9)
set_tests_properties(knav_bake_world_sample PROPERTIES FIXTURES_SETUP knav_world_sample)
set_tests_properties(knav_coarse PROPERTIES FIXTURES_REQUIRED knav_world_sample)

# Squad AI on the world_sample window (see the ksim section for the tools).
add_executable(ksim_ai tests/ksim_ai.cpp)
target_link_libraries(ksim_ai PRIVATE ksim)
target_compile_definitions(ksim_ai PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_ai)
add_test(NAME ksim_ai COMMAND ksim_ai ${CMAKE_CURRENT_BINARY_DIR}/world_sample 4 7 6 9)
set_tests_properties(ksim_ai PROPERTIES FIXTURES_REQUIRED knav_world_sample)

# The cost of the clearance every character route asks for (NAVMESH_CACHE.md "Clearance"): a sweep of radius-4 routes over
# the world_sample window, the worst single request under the fail-fast budget (50 ms).
add_executable(ksim_route_soak tests/ksim_route_soak.cpp)
target_link_libraries(ksim_route_soak PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_route_soak PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_route_soak)
add_test(NAME ksim_route_soak COMMAND ksim_route_soak ${CMAKE_CURRENT_BINARY_DIR}/world_sample)
# RUN_SERIAL: its limits are wall-clock route times; under a -j16 suite the hopeless pair passed 250 ms (44 ms alone).
set_tests_properties(ksim_route_soak PROPERTIES FIXTURES_REQUIRED knav_world_sample SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

add_executable(ksim_route_walk tests/ksim_route_walk.cpp)
target_link_libraries(ksim_route_walk PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_route_walk PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_route_walk)
add_test(NAME ksim_route_walk COMMAND ksim_route_walk ${CMAKE_CURRENT_BINARY_DIR}/world_sample)
set_tests_properties(ksim_route_walk PROPERTIES FIXTURES_REQUIRED knav_world_sample SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# Town life T3f: the unloaded phase and the soak gate (TOWN_LIFE_SLICE 5.3 LOD / 5.7). No player area, so every town squad runs the
# unloaded path: the soak stays at 0 goal-less / stuck / off route with no leaked reservation, and the counters see the night half
# asleep, guards at their posts, people at work and bar patrons relaxing.
add_executable(ksim_town_soak tests/ksim_town_soak.cpp)
target_link_libraries(ksim_town_soak PRIVATE ksim)
target_compile_definitions(ksim_town_soak PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_soak)
add_test(NAME ksim_town_soak COMMAND ksim_town_soak ${CMAKE_CURRENT_BINARY_DIR}/world_sample)
set_tests_properties(ksim_town_soak PROPERTIES FIXTURES_REQUIRED knav_world_sample SKIP_RETURN_CODE 77)

# The soak's living-world CSVs (docs/reference/LIVING_WORLD.md): the header and one row per game-hour, and no effect on the state hash.
add_executable(ksim_soak_csv tests/ksim_soak_csv.cpp)
target_link_libraries(ksim_soak_csv PRIVATE ksim)
target_compile_definitions(ksim_soak_csv PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_soak_csv)
add_test(NAME ksim_soak_csv COMMAND ksim_soak_csv ${CMAKE_CURRENT_BINARY_DIR}/world_sample)
set_tests_properties(ksim_soak_csv PROPERTIES FIXTURES_REQUIRED knav_world_sample SKIP_RETURN_CODE 77)

# Off-screen token speeds (ai.token_speeds, COMBAT_OPTIONS.md 7): a token walks Hub -> Squin at the flat 15 u/s (off) or 55 / 25 u/s (on).
add_executable(ksim_token_speed tests/ksim_token_speed.cpp)
target_link_libraries(ksim_token_speed PRIVATE ksim)
target_compile_definitions(ksim_token_speed PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_token_speed)
add_test(NAME ksim_token_speed COMMAND ksim_token_speed)

# Faction layer first slice: campaigns, events, raids (straight-line navigator,
# no bake needed).
add_executable(ksim_faction tests/ksim_faction.cpp)
target_link_libraries(ksim_faction PRIVATE ksim)
target_compile_definitions(ksim_faction PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_faction)
add_test(NAME ksim_faction COMMAND ksim_faction)

# The real-economy overlay's first phase (TOWN_LIFE_SLICE 7, T4a, save v69): the mode flag, the seeded TownStock, the hourly
# aggregate tick and its soak counters.
add_executable(ksim_economy_flow tests/ksim_economy_flow.cpp)
target_link_libraries(ksim_economy_flow PRIVATE ksim)
target_compile_definitions(ksim_economy_flow PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_economy_flow)
add_test(NAME ksim_economy_flow COMMAND ksim_economy_flow)

# The camps (TOWN_LIFE_SLICE 7.3, T4b): EconomyProducers seeded from the placed production buildings, the staffing rule, and the
# real-converter path at Full detail.
add_executable(ksim_economy_prod tests/ksim_economy_prod.cpp)
target_link_libraries(ksim_economy_prod PRIVATE ksim)
target_compile_definitions(ksim_economy_prod PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_economy_prod)
add_test(NAME ksim_economy_prod COMMAND ksim_economy_prod)

add_executable(ksim_auto_producer tests/ksim_auto_producer.cpp)
target_link_libraries(ksim_auto_producer PRIVATE ksim)
target_compile_definitions(ksim_auto_producer PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_auto_producer)
add_test(NAME ksim_auto_producer COMMAND ksim_auto_producer)

# The supply board and its delivery caravans (TOWN_LIFE_SLICE 7.4, T4c): offers and needs from the town stock, expiring claims,
# both ends reserved, a trade-circuit squad carrying a delivery, and the loss on death. The v69 board/caravan kinds carry the state.
add_executable(ksim_economy_caravan tests/ksim_economy_caravan.cpp)
target_link_libraries(ksim_economy_caravan PRIVATE ksim)
target_compile_definitions(ksim_economy_caravan PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_economy_caravan)
add_test(NAME ksim_economy_caravan COMMAND ksim_economy_caravan)

# Prices and the draw filter (TOWN_LIFE_SLICE 7.4, T4d): the town stock's factor on top of the trade chain and the T1 refresh drawing
# only what the town holds.
add_executable(ksim_economy_price tests/ksim_economy_price.cpp)
target_link_libraries(ksim_economy_price PRIVATE ksim)
target_compile_definitions(ksim_economy_price PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_economy_price)
add_test(NAME ksim_economy_price COMMAND ksim_economy_price)

# The dialogue book (include/ksim/dialogue/dialogue_book.hpp, docs/DIALOGUE.md 1): the compile census, the package
# sets and the static pre-pass. The coverage tool prints the census; the test asserts it and, through
# tests/ksim_py/dialogue_compare.py, compares the sets with tools/dialogue.py.
add_executable(ksim_dialogue_coverage tools/ksim_dialogue_coverage.cpp)
target_link_libraries(ksim_dialogue_coverage PRIVATE ksim)
target_compile_definitions(ksim_dialogue_coverage PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_dialogue_coverage)
add_executable(ksim_dialogue_book tests/ksim_dialogue_book.cpp)
target_link_libraries(ksim_dialogue_book PRIVATE ksim)
target_compile_definitions(ksim_dialogue_book PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_dialogue_book)
add_test(NAME ksim_dialogue_book COMMAND ksim_dialogue_book)
# The dialogue events, the gate and the root selection (include/ksim/dialogue/dialogue_events.hpp, docs/DIALOGUE.md 8 slice 1 steps 3 and 4).
add_executable(ksim_dialogue_events tests/ksim_dialogue_events.cpp)
target_link_libraries(ksim_dialogue_events PRIVATE ksim)
target_compile_definitions(ksim_dialogue_events PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_dialogue_events)
add_test(NAME ksim_dialogue_events COMMAND ksim_dialogue_events ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_dialogue_events PROPERTIES FIXTURES_REQUIRED knav_cache)
# 35 RUN_AWAY / 32 SELF_PRESERVATION and 280 RUN_AWAY_FORCED (Task_Runaway; individual_goals.cpp) on the Hub navmesh.
add_executable(ksim_run_away tests/ksim_run_away.cpp)
target_link_libraries(ksim_run_away PRIVATE ksim)
target_compile_definitions(ksim_run_away PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_run_away)
add_test(NAME ksim_run_away COMMAND ksim_run_away ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_run_away PROPERTIES FIXTURES_REQUIRED knav_cache)
# Recruitment through dialogue (DIALOGUE.md 5 / 6): DA_JOIN_SQUAD_* as ChangeSquad, TAKE / GIVE_MONEY on the player faction's wallet,
# DC_PLAYERMONEY and DC_IS_RECRUITABLE's roster cap, on the real data; no navmesh needed.
add_executable(ksim_dialogue_recruit tests/ksim_dialogue_recruit.cpp)
target_link_libraries(ksim_dialogue_recruit PRIVATE ksim)
target_compile_definitions(ksim_dialogue_recruit PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_dialogue_recruit)
add_test(NAME ksim_dialogue_recruit COMMAND ksim_dialogue_recruit)
# The live WORLD_EVENT_STATE set (world_state.hpp, WORLD_MODEL.md "WORLD_EVENT_STATE semantics"): the evaluator and the 69/152 fresh world,
# the dialogue `world state` gate on the data's own "Tinfist is not alive" line (nothing before the kill, the line after it), the involvement
# set from the player's kill and a direct imprisonment, and save v72 keeping them.
add_executable(ksim_world_state tests/ksim_world_state.cpp)
target_link_libraries(ksim_world_state PRIVATE ksim)
target_compile_definitions(ksim_world_state PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_world_state)
add_test(NAME ksim_world_state COMMAND ksim_world_state)
# Fights across floors (task AQ, ENGAGEMENT.md "Fights across floors"): the melee gates measure the navmesh walk, so a
# player ordered to attack an enemy one floor above it gets no hit through the ceiling; ordered up the stair, they fight.
# The dialogue economy and membership effects (DIALOGUE.md 5, D5): TAKE_MONEY's credit to the speaker's faction (save v70), give item,
# SEPARATE_TO_MY_OWN_SQUAD, REMOVE_SLAVE_STATUS, GIVE_TARGET_MY_SLAVES and KNOCKOUT, each on its real data line where one exists.
add_executable(ksim_dialogue_econ tests/ksim_dialogue_econ.cpp)
target_link_libraries(ksim_dialogue_econ PRIVATE ksim)
target_compile_definitions(ksim_dialogue_econ PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_dialogue_econ)
add_test(NAME ksim_dialogue_econ COMMAND ksim_dialogue_econ)
# The reply menu at the server / wire level (DIALOGUE.md 13.2-13.4, 13.8 rows 1-3): an open menu freezes the tick and CLOCK says so; a
# reply or end_talk lifts it; a dropped session clears the hold and the reconnect snapshot resends the menu; the grace ends it (reason 5).
add_executable(ksim_dialogue_menu tests/ksim_dialogue_menu.cpp ../client/src/net/simlink.cpp)
target_include_directories(ksim_dialogue_menu PRIVATE ../client/src)
target_link_libraries(ksim_dialogue_menu PRIVATE ksim)
target_compile_definitions(ksim_dialogue_menu PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_dialogue_menu)
add_test(NAME ksim_dialogue_menu COMMAND ksim_dialogue_menu ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
# RUN_SERIAL: its checks are wall-clock (2 s of frozen ticks, a 30 s grace) on a real-time server; beside a -j16 suite the server can be
# starved and the sleeps are not what they say.
set_tests_properties(ksim_dialogue_menu PROPERTIES FIXTURES_REQUIRED knav_cache RUN_SERIAL TRUE)
if(Python3_FOUND)
	add_test(NAME ksim_dialogue_compare
		COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ksim_py/dialogue_compare.py
			$<TARGET_FILE:ksim_dialogue_coverage> ${KENSHI_DATA})
endif()

# The sandboxed Lua host (script.hpp): environment, budgets, data boundary. No game data.
add_executable(ksim_script tests/ksim_script.cpp)
target_link_libraries(ksim_script PRIVATE ksim)
kdata_warnings(ksim_script)
add_test(NAME ksim_script COMMAND ksim_script)

# The Lua behaviour layer, first slice: the default faction script against the native
# scoring (hash equality), every way a script can fail, subscriptions, saved fields.
add_executable(ksim_board_pick tests/ksim_board_pick.cpp)
target_link_libraries(ksim_board_pick PRIVATE ksim)
target_compile_definitions(ksim_board_pick PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_board_pick)
add_test(NAME ksim_board_pick COMMAND ksim_board_pick)
set_tests_properties(ksim_board_pick PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_behaviour tests/ksim_behaviour.cpp)
target_link_libraries(ksim_behaviour PRIVATE ksim)
target_compile_definitions(ksim_behaviour PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_behaviour)
add_test(NAME ksim_behaviour COMMAND ksim_behaviour)

# The Hub's 11 market stalls pick the original's parts variants (rwh.206).
add_executable(kworld_building_parts tests/kworld_building_parts.cpp)
target_link_libraries(kworld_building_parts PRIVATE kworld)
target_compile_definitions(kworld_building_parts PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(kworld_building_parts)
add_test(NAME kworld_building_parts COMMAND kworld_building_parts)

# Town gates: door data, postures, overrides, the Hub on the world_sample bake, determinism.
add_executable(ksim_doors tests/ksim_doors.cpp)
target_link_libraries(ksim_doors PRIVATE ksim)
target_compile_definitions(ksim_doors PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_doors)
add_test(NAME ksim_doors COMMAND ksim_doors ${CMAKE_CURRENT_BINARY_DIR}/world_sample 4 7 6 9)
set_tests_properties(ksim_doors PROPERTIES FIXTURES_REQUIRED knav_world_sample)

# The bash / pick / wait decision for a locked door (DOORS.md section 6).
add_executable(ksim_door_plan tests/ksim_door_plan.cpp)
target_link_libraries(ksim_door_plan PRIVATE ksim)
kdata_warnings(ksim_door_plan)
add_test(NAME ksim_door_plan COMMAND ksim_door_plan)

# The grid inventory (SIMLINK.md sections 6 and 7, INVENTORY_GRID_STATIC.md): rules, orders 64-68 and ITEM_GRID through a Session.
add_executable(ksim_inventory_grid tests/ksim_inventory_grid.cpp)
target_link_libraries(ksim_inventory_grid PRIVATE ksim)
target_compile_definitions(ksim_inventory_grid PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_inventory_grid)
add_test(NAME ksim_inventory_grid COMMAND ksim_inventory_grid ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_inventory_grid PROPERTIES FIXTURES_REQUIRED knav_cache)

# ITEM_STATS (SIMLINK.md "ITEM_STATS"): order 69 and the type 29 reply, decoded by hand, through a Session on the Hub cache.
add_executable(ksim_item_stats tests/ksim_item_stats.cpp)
target_link_libraries(ksim_item_stats PRIVATE ksim)
target_compile_definitions(ksim_item_stats PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_item_stats)
add_test(NAME ksim_item_stats COMMAND ksim_item_stats ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_item_stats PROPERTIES FIXTURES_REQUIRED knav_cache)

# The Goal row's live task (task AT, SIMLINK.md "Goal names"): a fight, a routine slot, a flee, a carry, a group goal and a corpse.
add_executable(ksim_goal_label tests/ksim_goal_label.cpp)
target_link_libraries(ksim_goal_label PRIVATE ksim)
target_compile_definitions(ksim_goal_label PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_goal_label)
add_test(NAME ksim_goal_label COMMAND ksim_goal_label)

# The economy's Lua hooks (task AV, TOWN_LIFE_SLICE 7.13): price, demand, on_delivery, on_loss and the failing-hook fallback.
add_executable(ksim_economy_hooks tests/ksim_economy_hooks.cpp)
target_link_libraries(ksim_economy_hooks PRIVATE ksim)
target_compile_definitions(ksim_economy_hooks PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_economy_hooks)
add_test(NAME ksim_economy_hooks COMMAND ksim_economy_hooks)

add_executable(ksim_task_names tests/ksim_task_names.cpp)
target_link_libraries(ksim_task_names PRIVATE ksim)
target_compile_definitions(ksim_task_names PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_task_names)
add_test(NAME ksim_task_names COMMAND ksim_task_names)

add_executable(ksim_char_stats tests/ksim_char_stats.cpp)
target_link_libraries(ksim_char_stats PRIVATE ksim)
target_compile_definitions(ksim_char_stats PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_char_stats)
add_test(NAME ksim_char_stats COMMAND ksim_char_stats ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_char_stats PROPERTIES FIXTURES_REQUIRED knav_cache)

# Interactables and the interact order (SIMLINK.md section 13), through a Session on the Hub cache.
add_executable(ksim_interact tests/ksim_interact.cpp)
target_link_libraries(ksim_interact PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_interact PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_interact)
add_test(NAME ksim_interact COMMAND ksim_interact ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_interact PROPERTIES FIXTURES_REQUIRED knav_cache)

# The talk / trade approach goal (FEATURES.md "Talk / trade approach stops on the near side of a counter"), on hand-built navmeshes.
add_executable(ksim_talk_approach tests/ksim_talk_approach.cpp)
target_link_libraries(ksim_talk_approach PRIVATE ksim RecastNavigation::Detour)
kdata_warnings(ksim_talk_approach)
add_test(NAME ksim_talk_approach COMMAND ksim_talk_approach)

# Carrying (carry.hpp): lift, carry, put down, encumbrance, the NPC take-out of KO'd intruders, building lock levels.
add_executable(ksim_carry tests/ksim_carry.cpp)
target_link_libraries(ksim_carry PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_carry PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_carry)
add_test(NAME ksim_carry COMMAND ksim_carry ${CMAKE_CURRENT_BINARY_DIR}/hub.knav ${CMAKE_CURRENT_BINARY_DIR}/stack.knav)
set_tests_properties(ksim_carry PROPERTIES FIXTURES_REQUIRED "knav_cache;knav_cache_stack")

# Crime foundation (crime.hpp): records, witnesses, bounties, the trespass sensor.
# Research state from the FCS data and the bench's craftable list (kenshi-h2v.17.7, CRAFTING_SLICE 2.3, 3, 9.2).
add_executable(ksim_research_state tests/ksim_research_state.cpp)
target_link_libraries(ksim_research_state PRIVATE ksim)
target_compile_definitions(ksim_research_state PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_research_state)
add_test(NAME ksim_research_state COMMAND ksim_research_state)

add_executable(ksim_crime tests/ksim_crime.cpp)
target_link_libraries(ksim_crime PRIVATE ksim)
target_compile_definitions(ksim_crime PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_crime)
add_test(NAME ksim_crime COMMAND ksim_crime)
add_executable(ksim_steal_chance tests/ksim_steal_chance.cpp)
target_link_libraries(ksim_steal_chance PRIVATE ksim)
target_compile_definitions(ksim_steal_chance PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_steal_chance)
add_test(NAME ksim_steal_chance COMMAND ksim_steal_chance)

add_executable(ksim_lockpick_seq tests/ksim_lockpick_seq.cpp)
target_link_libraries(ksim_lockpick_seq PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_lockpick_seq PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_lockpick_seq)
add_test(NAME ksim_lockpick_seq COMMAND ksim_lockpick_seq ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_lockpick_seq PROPERTIES FIXTURES_REQUIRED "knav_cache")

# Gate crews (gatecrew.hpp, spawn.hpp): spawn points and counts from the data, the squad layer, the Hub leader on the real mesh.
add_executable(ksim_gates tests/ksim_gates.cpp)
target_link_libraries(ksim_gates PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_gates PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_gates)
add_test(NAME ksim_gates COMMAND ksim_gates)

# Perception: the measured sensor (perception.hpp), pure rules, scans and the arena with a save mid-meter.
# Decision cadences scaled by min(game speed, 3) (k16.6.633).
add_executable(ksim_far_squad_step tests/ksim_far_squad_step.cpp)
target_link_libraries(ksim_far_squad_step PRIVATE ksim)
target_compile_definitions(ksim_far_squad_step PRIVATE KENSHI_DATA="${KENSHI_DATA}" KSIM_AI_TABLE="${KSIM_AI_TABLE}")
kdata_warnings(ksim_far_squad_step)
add_test(NAME ksim_far_squad_step COMMAND ksim_far_squad_step)
add_executable(ksim_hunger_stride tests/ksim_hunger_stride.cpp)
target_link_libraries(ksim_hunger_stride PRIVATE ksim)
target_compile_definitions(ksim_hunger_stride PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_hunger_stride)
add_test(NAME ksim_hunger_stride COMMAND ksim_hunger_stride)
add_executable(ksim_save_stamps tests/ksim_save_stamps.cpp)
target_link_libraries(ksim_save_stamps PRIVATE ksim)
target_compile_definitions(ksim_save_stamps PRIVATE KENSHI_DATA="${KENSHI_DATA}" KSIM_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
kdata_warnings(ksim_save_stamps)
add_test(NAME ksim_save_stamps COMMAND ksim_save_stamps)
add_executable(ksim_cadence_scale tests/ksim_cadence_scale.cpp)
target_link_libraries(ksim_cadence_scale PRIVATE ksim)
target_compile_definitions(ksim_cadence_scale PRIVATE KENSHI_DATA="${KENSHI_DATA}" KSIM_AI_TABLE="${KSIM_AI_TABLE}")
kdata_warnings(ksim_cadence_scale)
add_test(NAME ksim_cadence_scale COMMAND ksim_cadence_scale)

add_executable(ksim_perception tests/ksim_perception.cpp)
target_link_libraries(ksim_perception PRIVATE ksim)
target_compile_definitions(ksim_perception PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_perception)
add_test(NAME ksim_perception COMMAND ksim_perception)

# The G2 goal selector (goalsel.hpp) and the G3 consumer resolvers (world_resolvers.hpp) a world squad runs (task BW). Pure, no game data.
add_executable(ksim_goal_select tests/ksim_goal_select.cpp)
target_link_libraries(ksim_goal_select PRIVATE ksim)
kdata_warnings(ksim_goal_select)
add_test(NAME ksim_goal_select COMMAND ksim_goal_select)

# G4 (goap.hpp, task CB): the GOAP planner (kinspect dt18) - a chain found, a true precondition dropping its subtree, no plan at
# score 0 (re-planned next update), the cycle guard. Pure, no game data.
add_executable(ksim_goap_plan tests/ksim_goap_plan.cpp)
target_link_libraries(ksim_goap_plan PRIVATE ksim)
kdata_warnings(ksim_goap_plan)
add_test(NAME ksim_goap_plan COMMAND ksim_goap_plan)

# The G1 package Blackboard (blackboard.hpp, task BY): the priority ladder, contracts, change AI, and the row merge. Pure, no game data.
add_executable(ksim_blackboard tests/ksim_blackboard.cpp)
target_link_libraries(ksim_blackboard PRIVATE ksim)
kdata_warnings(ksim_blackboard)
add_test(NAME ksim_blackboard COMMAND ksim_blackboard)
add_executable(ksim_package_leads tests/ksim_package_leads.cpp)
target_link_libraries(ksim_package_leads PRIVATE ksim)
target_compile_definitions(ksim_package_leads PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_package_leads)
add_test(NAME ksim_package_leads COMMAND ksim_package_leads)

# The sensor list's record flags and masks (sensor.hpp, G5 task BU) and the 167 capture resolver over it. Pure, no game data.
add_executable(ksim_sensor_bits tests/ksim_sensor_bits.cpp)
target_link_libraries(ksim_sensor_bits PRIVATE ksim)
kdata_warnings(ksim_sensor_bits)
add_test(NAME ksim_sensor_bits COMMAND ksim_sensor_bits)

# The sensor flags' world inputs (task CF): Simulation::sensorContext resolves a fighter's faction state::Id through the faction map.
add_executable(ksim_sensor_context tests/ksim_sensor_context.cpp)
target_link_libraries(ksim_sensor_context PRIVATE ksim)
target_compile_definitions(ksim_sensor_context PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_sensor_context)
add_test(NAME ksim_sensor_context COMMAND ksim_sensor_context)

# The towns' circles (task town-circle): the exe's Town radius (the base by type x mult), and each town's own Bar inside its circle.
add_executable(ksim_town_circle tests/ksim_town_circle.cpp)
target_link_libraries(ksim_town_circle PRIVATE ksim)
target_compile_definitions(ksim_town_circle PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_circle)
add_test(NAME ksim_town_circle COMMAND ksim_town_circle)

# Ground-blood trackers (bloodtrail.hpp): pool, smear, drops. Pure, no game data.
add_executable(ksim_blood tests/ksim_blood.cpp)
target_link_libraries(ksim_blood PRIVATE ksim)
kdata_warnings(ksim_blood)
add_test(NAME ksim_blood COMMAND ksim_blood)

# Producers (MINE_SELL_SLICE M1-M3): deposits and mines as one generic producer, operating, the output slot, wire kind 4, money, save v49.
add_executable(ksim_producers tests/ksim_producers.cpp)
target_link_libraries(ksim_producers PRIVATE ksim)
target_compile_definitions(ksim_producers PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_producers)
add_test(NAME ksim_producers COMMAND ksim_producers ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_producers PROPERTIES FIXTURES_REQUIRED knav_cache)

# kenshi-k16.5.22: a machine record for every functional building at the South Stone Camp (producer, converter, power, turret, furnace).
add_executable(ksim_machine_records tests/ksim_machine_records.cpp)
target_link_libraries(ksim_machine_records PRIVATE ksim)
target_compile_definitions(ksim_machine_records PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_machine_records)
add_test(NAME ksim_machine_records COMMAND ksim_machine_records)
add_executable(ksim_machine_wire tests/ksim_machine_wire.cpp)
target_link_libraries(ksim_machine_wire PRIVATE ksim)
target_compile_definitions(ksim_machine_wire PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_machine_wire)
add_test(NAME ksim_machine_wire COMMAND ksim_machine_wire)

add_executable(ksim_machine_order tests/ksim_machine_order.cpp)
target_link_libraries(ksim_machine_order PRIVATE ksim)
target_compile_definitions(ksim_machine_order PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_machine_order)
add_test(NAME ksim_machine_order COMMAND ksim_machine_order)

# kenshi-k16.19.4 / .19.5: the Hub copper node's navmesh cut follows its collision (a rock shell cuts per triangle, not its hull); the miner's stand point.
add_executable(ksim_mine_node_cut tests/ksim_mine_node_cut.cpp)
target_link_libraries(ksim_mine_node_cut PRIVATE ksim)
target_compile_definitions(ksim_mine_node_cut PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_mine_node_cut)
add_test(NAME ksim_mine_node_cut COMMAND ksim_mine_node_cut)
set_tests_properties(ksim_mine_node_cut PROPERTIES SKIP_RETURN_CODE 77)

# Gear prices (s72): Iron Club 198 / 49, Rag Loincloth 50 / 12, from the real data. No navmesh needed.
add_executable(ksim_gear_price tests/ksim_gear_price.cpp)
target_link_libraries(ksim_gear_price PRIVATE ksim)
target_compile_definitions(ksim_gear_price PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_gear_price)
add_test(NAME ksim_gear_price COMMAND ksim_gear_price)

add_executable(ksim_trade tests/ksim_trade.cpp)
target_link_libraries(ksim_trade PRIVATE ksim)
target_compile_definitions(ksim_trade PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_trade)
add_test(NAME ksim_trade COMMAND ksim_trade ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_trade PROPERTIES FIXTURES_REQUIRED knav_cache)

# kenshi-k16.6.7: the stolen mark's rules (dt43 D): first victim sticks, same-victim merge, the clears. The ksim_trade rig.
add_executable(ksim_stolen_mark tests/ksim_stolen_mark.cpp)
target_link_libraries(ksim_stolen_mark PRIVATE ksim)
target_compile_definitions(ksim_stolen_mark PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_stolen_mark)
add_test(NAME ksim_stolen_mark COMMAND ksim_stolen_mark ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_stolen_mark PROPERTIES FIXTURES_REQUIRED knav_cache)

# kenshi-k16.9.3: a merchant's talk runs his dialogue; the trade reply opens the trade (DA_TRADE). The ksim_trade rig.
add_executable(ksim_barman_trade tests/ksim_barman_trade.cpp)
target_link_libraries(ksim_barman_trade PRIVATE ksim)
target_compile_definitions(ksim_barman_trade PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_barman_trade)
add_test(NAME ksim_barman_trade COMMAND ksim_barman_trade ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_barman_trade PROPERTIES FIXTURES_REQUIRED knav_cache)

# kenshi-k16.6.106: the Hub bar's barman stands at the layout's Node Shopkeeper marker, behind the counter. The ksim_trade rig.
add_executable(ksim_barman_node tests/ksim_barman_node.cpp)
target_link_libraries(ksim_barman_node PRIVATE ksim)
target_compile_definitions(ksim_barman_node PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_barman_node)
add_test(NAME ksim_barman_node COMMAND ksim_barman_node ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_barman_node PROPERTIES FIXTURES_REQUIRED knav_cache)

# The journey scenarios (k16.18.1 / .19.1 / .20.1) on the trade rig: load, bead= on the wire, the step driven by its order.
add_executable(ksim_journey tests/ksim_journey.cpp)
target_link_libraries(ksim_journey PRIVATE ksim)
target_compile_definitions(ksim_journey PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_journey)
add_test(NAME ksim_journey COMMAND ksim_journey ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_journey PROPERTIES FIXTURES_REQUIRED knav_cache)

# The server's new game at the Hub with the squad AI: the Dust Bandit world squads over game hours (TOWN_LIFE_SLICE 5.14, "Hub debug").
add_executable(ksim_world_hub tests/ksim_world_hub.cpp)
target_link_libraries(ksim_world_hub PRIVATE ksim)
target_compile_definitions(ksim_world_hub PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_world_hub)
add_test(NAME ksim_world_hub COMMAND ksim_world_hub ${KSIM_NAVWORLD})
set_tests_properties(ksim_world_hub PROPERTIES SKIP_RETURN_CODE 77)

# A STAY_IN_HOME resident sits (TOWN_LIFE_SLICE 5.4, kenshi-k16.6.30): a home chair, else the floor; a fight stands it up.
add_executable(ksim_home_sit tests/ksim_home_sit.cpp)
target_link_libraries(ksim_home_sit PRIVATE ksim)
target_compile_definitions(ksim_home_sit PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_home_sit)
add_test(NAME ksim_home_sit COMMAND ksim_home_sit ${KSIM_NAVWORLD})
set_tests_properties(ksim_home_sit PROPERTIES SKIP_RETURN_CODE 77)

# NEW_GAME twice on one server process, after a load, and a load after a NEW_GAME (TOWN_LIFE_SLICE 5.14 "second NEW_GAME").
add_executable(ksim_new_game_twice tests/ksim_new_game_twice.cpp)
target_link_libraries(ksim_new_game_twice PRIVATE ksim)
target_compile_definitions(ksim_new_game_twice PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_new_game_twice)
add_test(NAME ksim_new_game_twice COMMAND ksim_new_game_twice ${KSIM_NAVWORLD})
set_tests_properties(ksim_new_game_twice PROPERTIES SKIP_RETURN_CODE 77)

# A fighting world squad off the streaming mesh is demoted (TOWN_LIFE_SLICE 5.14 "off the mesh", the position_corrected -1 spam).
add_executable(ksim_world_offmesh tests/ksim_world_offmesh.cpp)
target_link_libraries(ksim_world_offmesh PRIVATE ksim)
target_compile_definitions(ksim_world_offmesh PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_world_offmesh)
add_test(NAME ksim_world_offmesh COMMAND ksim_world_offmesh ${KSIM_NAVWORLD})
set_tests_properties(ksim_world_offmesh PROPERTIES SKIP_RETURN_CODE 77)
# The bar-fight loop (docs/reference/BAR_FIGHT_LOOP.md): lure the Dust Bandits, fight, loot, sell, on the server's own Hub game.
add_executable(ksim_bar_fight tests/ksim_bar_fight.cpp)
target_link_libraries(ksim_bar_fight PRIVATE ksim)
target_compile_definitions(ksim_bar_fight PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_bar_fight)
add_test(NAME ksim_bar_fight COMMAND ksim_bar_fight ${KSIM_NAVWORLD})
# RESOURCE_LOCK ksim_bigmem: peak RSS 3.7 GB (k16.6.290; the other world tests stay under 2.1 GB except ksim_nav_islands, 7.5 GB, RUN_SERIAL):
# the 2-4 GB tests never overlap each other in one ctest run.
set_tests_properties(ksim_bar_fight PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ksim_bigmem)
# The aggro drop and the fleeing player (task BE): the player kites the Hub bandits in and out of the Bar; the crew joins by its own
# rules. Asserts that the bandits turn on the crew fighting them and logs the player's speed under its move order.
add_executable(ksim_aggro_drop tests/ksim_aggro_drop.cpp)
target_link_libraries(ksim_aggro_drop PRIVATE ksim)
target_compile_definitions(ksim_aggro_drop PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_aggro_drop)
add_test(NAME ksim_aggro_drop COMMAND ksim_aggro_drop ${KSIM_NAVWORLD})
set_tests_properties(ksim_aggro_drop PROPERTIES SKIP_RETURN_CODE 77)
# Task CE: every playtest scenario (`scenarios/<name>.kscen`, KSIM_SCENARIOS_DIR) parses, checks against the data, loads by name into the
# pristine world and runs; every metric its header names answers with a number (the SCENARIO_METRICS values).
add_executable(ksim_scenario_load tests/ksim_scenario_load.cpp)
target_link_libraries(ksim_scenario_load PRIVATE ksim)
target_compile_definitions(ksim_scenario_load PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_scenario_load)
add_test(NAME ksim_scenario_load COMMAND ksim_scenario_load ${KSIM_NAVWORLD})
set_tests_properties(ksim_scenario_load PROPERTIES SKIP_RETURN_CODE 77)
# The metrics answer the run, not the load: bar_lure left idle counts no target, the provoke counts them.
add_executable(ksim_playtest_metrics tests/ksim_playtest_metrics.cpp)
target_link_libraries(ksim_playtest_metrics PRIVATE ksim)
target_compile_definitions(ksim_playtest_metrics PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_playtest_metrics)
add_test(NAME ksim_playtest_metrics COMMAND ksim_playtest_metrics ${KSIM_NAVWORLD})
set_tests_properties(ksim_playtest_metrics PROPERTIES SKIP_RETURN_CODE 77)
# k16.6.103: the Blood Raiders in the Hub, a town with the town scorer on, take their package's combat goals on sight.
add_executable(ksim_town_aggro tests/ksim_town_aggro.cpp)
target_link_libraries(ksim_town_aggro PRIVATE ksim)
target_compile_definitions(ksim_town_aggro PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_aggro)
add_test(NAME ksim_town_aggro COMMAND ksim_town_aggro ${KSIM_NAVWORLD})
set_tests_properties(ksim_town_aggro PROPERTIES SKIP_RETURN_CODE 77)
# k16.6.104: a locked-on chaser's speed on the Bar stairs against flat ground.
add_executable(ksim_stair_chase tests/ksim_stair_chase.cpp)
target_link_libraries(ksim_stair_chase PRIVATE ksim)
target_compile_definitions(ksim_stair_chase PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_stair_chase)
add_test(NAME ksim_stair_chase COMMAND ksim_stair_chase ${KSIM_NAVWORLD})
set_tests_properties(ksim_stair_chase PROPERTIES SKIP_RETURN_CODE 77)
# Spectator mode: the player-less scenario, two hostile NPC squads that have to fight on their own.
add_executable(ksim_spectate_fight tests/ksim_spectate_fight.cpp)
target_link_libraries(ksim_spectate_fight PRIVATE ksim)
target_compile_definitions(ksim_spectate_fight PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_spectate_fight)
add_test(NAME ksim_spectate_fight COMMAND ksim_spectate_fight ${KSIM_NAVWORLD})
set_tests_properties(ksim_spectate_fight PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_chase_scen tests/ksim_chase_scen.cpp)
target_link_libraries(ksim_chase_scen PRIVATE ksim)
target_compile_definitions(ksim_chase_scen PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_chase_scen)
add_test(NAME ksim_chase_scen COMMAND ksim_chase_scen ${KSIM_NAVWORLD})
set_tests_properties(ksim_chase_scen PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_spectate_approach tests/ksim_spectate_approach.cpp)
target_link_libraries(ksim_spectate_approach PRIVATE ksim)
target_compile_definitions(ksim_spectate_approach PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_spectate_approach)
add_test(NAME ksim_spectate_approach COMMAND ksim_spectate_approach ${KSIM_NAVWORLD})
set_tests_properties(ksim_spectate_approach PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_spectate_stagger tests/ksim_spectate_stagger.cpp)
target_link_libraries(ksim_spectate_stagger PRIVATE ksim)
target_compile_definitions(ksim_spectate_stagger PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_spectate_stagger)
add_test(NAME ksim_spectate_stagger COMMAND ksim_spectate_stagger ${KSIM_NAVWORLD})
set_tests_properties(ksim_spectate_stagger PROPERTIES SKIP_RETURN_CODE 77)
# The fight's decision trace (kenshi-k16.6.59): combat picks on spectate_fight, once per change.
add_executable(ksim_fight_trace tests/ksim_fight_trace.cpp)
target_link_libraries(ksim_fight_trace PRIVATE ksim)
target_compile_definitions(ksim_fight_trace PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_fight_trace)
add_test(NAME ksim_fight_trace COMMAND ksim_fight_trace ${KSIM_NAVWORLD})
set_tests_properties(ksim_fight_trace PROPERTIES SKIP_RETURN_CODE 77)
# Feet on the ground (kenshi-k16.7.14): characters on open ground stand at the terrain heightmap's height.
add_executable(ksim_place_ground tests/ksim_place_ground.cpp)
target_link_libraries(ksim_place_ground PRIVATE ksim)
target_compile_definitions(ksim_place_ground PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_place_ground)
add_test(NAME ksim_place_ground COMMAND ksim_place_ground ${KSIM_NAVWORLD})
set_tests_properties(ksim_place_ground PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_height_sweep tests/ksim_height_sweep.cpp)
target_link_libraries(ksim_height_sweep PRIVATE ksim)
target_compile_definitions(ksim_height_sweep PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_height_sweep)
add_test(NAME ksim_height_sweep COMMAND ksim_height_sweep ${KSIM_NAVWORLD})
set_tests_properties(ksim_height_sweep PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.31: a fresh New Game's Hub residents walk to their sites (not placed on them).
add_executable(ksim_bar_spread tests/ksim_bar_spread.cpp)
target_link_libraries(ksim_bar_spread PRIVATE ksim)
target_compile_definitions(ksim_bar_spread PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_bar_spread)
add_test(NAME ksim_bar_spread COMMAND ksim_bar_spread ${KSIM_NAVWORLD})
set_tests_properties(ksim_bar_spread PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_thug_walk tests/ksim_thug_walk.cpp)
target_link_libraries(ksim_thug_walk PRIVATE ksim)
target_compile_definitions(ksim_thug_walk PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_thug_walk)
add_test(NAME ksim_thug_walk COMMAND ksim_thug_walk ${KSIM_NAVWORLD})
set_tests_properties(ksim_thug_walk PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_residents_arrive tests/ksim_residents_arrive.cpp)
target_link_libraries(ksim_residents_arrive PRIVATE ksim)
target_compile_definitions(ksim_residents_arrive PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_residents_arrive)
add_test(NAME ksim_residents_arrive COMMAND ksim_residents_arrive ${KSIM_NAVWORLD})
set_tests_properties(ksim_residents_arrive PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_fresh_carry tests/ksim_fresh_carry.cpp)
target_link_libraries(ksim_fresh_carry PRIVATE ksim)
target_compile_definitions(ksim_fresh_carry PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
	KSIM_AI_TABLE="${KSIM_AI_TABLE}"
)
kdata_warnings(ksim_fresh_carry)
add_test(NAME ksim_fresh_carry COMMAND ksim_fresh_carry ${KSIM_NAVWORLD})
set_tests_properties(ksim_fresh_carry PROPERTIES SKIP_RETURN_CODE 77)

# A New Game settles before the first frame (kenshi-k16.17.9, TOWN_LIFE_SLICE 5.40).
add_executable(ksim_newgame_settled tests/ksim_newgame_settled.cpp)
target_link_libraries(ksim_newgame_settled PRIVATE ksim)
target_compile_definitions(ksim_newgame_settled PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_newgame_settled)
add_test(NAME ksim_newgame_settled COMMAND ksim_newgame_settled ${KSIM_NAVWORLD})
set_tests_properties(ksim_newgame_settled PROPERTIES SKIP_RETURN_CODE 77)

# Every NEW_GAME_STARTOFF spawns as its data says and nobody moves by itself in 3 game minutes (kenshi-k16.17.13.2).
add_executable(ksim_newgame_starts tests/ksim_newgame_starts.cpp)
target_include_directories(ksim_newgame_starts PRIVATE tests)
target_link_libraries(ksim_newgame_starts PRIVATE ksim)
target_compile_definitions(ksim_newgame_starts PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
	KSIM_AI_TABLE="${KSIM_AI_TABLE}"
)
kdata_warnings(ksim_newgame_starts)
# Twelve shards of the start-offs (kenshi-k16.5.21, k16.15.49): argv[2] = "k/n".
foreach(k 0 1 2 3 4 5 6 7 8 9 10 11)
	add_test(NAME ksim_newgame_starts_${k} COMMAND ksim_newgame_starts ${KSIM_NAVWORLD} ${k}/12)
	set_tests_properties(ksim_newgame_starts_${k} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# A patrol squad's members carry the attack join goals: a hostile in sight gets an attack candidate (kenshi-k16.6.543).
add_executable(ksim_patrol_threat tests/ksim_patrol_threat.cpp)
target_include_directories(ksim_patrol_threat PRIVATE tests)
target_link_libraries(ksim_patrol_threat PRIVATE ksim)
target_compile_definitions(ksim_patrol_threat PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_patrol_threat)
add_test(NAME ksim_patrol_threat COMMAND ksim_patrol_threat ${KSIM_NAVWORLD})
set_tests_properties(ksim_patrol_threat PROPERTIES SKIP_RETURN_CODE 77)

# New-game start "Guy with a dog": the dog spawns and nobody in the player squad spins or walks unordered (kenshi-k16.17.13.1).
add_executable(ksim_start_guy_dog tests/ksim_start_guy_dog.cpp)
target_link_libraries(ksim_start_guy_dog PRIVATE ksim)
target_compile_definitions(ksim_start_guy_dog PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
	KSIM_AI_TABLE="${KSIM_AI_TABLE}"
)
kdata_warnings(ksim_start_guy_dog)
add_test(NAME ksim_start_guy_dog COMMAND ksim_start_guy_dog ${KSIM_NAVWORLD})
set_tests_properties(ksim_start_guy_dog PROPERTIES SKIP_RETURN_CODE 77)

# Task BV: the Hub's Bar thugs (SQUAD_TEMPLATE 66159, a `bar squads` entry) spawned inside the bar with no order, the player inside; no
# terminal state, and any fight must have come from dialogue.
add_executable(ksim_bar_thugs tests/ksim_bar_thugs.cpp)
target_link_libraries(ksim_bar_thugs PRIVATE ksim)
target_compile_definitions(ksim_bar_thugs PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_bar_thugs)
add_test(NAME ksim_bar_thugs COMMAND ksim_bar_thugs ${KSIM_NAVWORLD})
set_tests_properties(ksim_bar_thugs PROPERTIES SKIP_RETURN_CODE 77)

# Task k16.6.83: the bar brawl diagnosis: the player attacks a seated patron in the Hub bar nearest (-51193, 2678); the fight trace shows who joins.
add_executable(ksim_bar_brawl tests/ksim_bar_brawl.cpp)
target_link_libraries(ksim_bar_brawl PRIVATE ksim)
target_compile_definitions(ksim_bar_brawl PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_bar_brawl)
add_test(NAME ksim_bar_brawl COMMAND ksim_bar_brawl ${KSIM_NAVWORLD})
set_tests_properties(ksim_bar_brawl PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME ksim_bar_brawl_thug COMMAND ksim_bar_brawl ${KSIM_NAVWORLD} Vagrants)
set_tests_properties(ksim_bar_brawl_thug PROPERTIES SKIP_RETURN_CODE 77)

# The client's bar lure at master after BW (the peer's report): the placed squad still engages the player, and an order or a fight
# outranks a package goal (a free squadmate takes the same 167 the ordered one must not).
add_executable(ksim_bw_orders tests/ksim_bw_orders.cpp)
target_link_libraries(ksim_bw_orders PRIVATE ksim)
target_compile_definitions(ksim_bw_orders PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_bw_orders)
add_test(NAME ksim_bw_orders COMMAND ksim_bw_orders ${KSIM_NAVWORLD})
set_tests_properties(ksim_bw_orders PROPERTIES SKIP_RETURN_CODE 77)

# What happens when a fight is over (task BD): a DOWN (conscious) enemy is finished off until KO'd, a KO'd one is left, and
# the crew drops its targets and goes back to its routine. Player-only, the Hub (the player provokes the Dust Bandit patrol).
add_executable(ksim_after_fight tests/ksim_after_fight.cpp)
target_link_libraries(ksim_after_fight PRIVATE ksim)
target_compile_definitions(ksim_after_fight PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_after_fight)
add_test(NAME ksim_after_fight COMMAND ksim_after_fight ${KSIM_NAVWORLD})
set_tests_properties(ksim_after_fight PROPERTIES SKIP_RETURN_CODE 77)
# The package goals a built world squad runs after a fight (task BO): the slaver caravan's capture chain (167) and the winners' food
# looting (177). Player-only, the Hub.
add_executable(ksim_slaver_caravan tests/ksim_slaver_caravan.cpp)
target_link_libraries(ksim_slaver_caravan PRIVATE ksim)
target_compile_definitions(ksim_slaver_caravan PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slaver_caravan)
add_test(NAME ksim_slaver_caravan COMMAND ksim_slaver_caravan ${KSIM_NAVWORLD})
set_tests_properties(ksim_slaver_caravan PROPERTIES SKIP_RETURN_CODE 77)
# The slaver caravan's delivery (k16.6.14, rwh.8 / rwh.9): the deliver package, the finder, the walk and the hand-over.
add_executable(ksim_slaver_deliver tests/ksim_slaver_deliver.cpp)
target_link_libraries(ksim_slaver_deliver PRIVATE ksim)
target_compile_definitions(ksim_slaver_deliver PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slaver_deliver)
add_test(NAME ksim_slaver_deliver COMMAND ksim_slaver_deliver ${KSIM_NAVWORLD})
set_tests_properties(ksim_slaver_deliver PROPERTIES SKIP_RETURN_CODE 77)
# The slaver caravan's trip to its depot on the live world (k16.6.21): scenarios/slavers_downed.kscen on the runtime mesh.
add_executable(ksim_slaver_route tests/ksim_slaver_route.cpp)
target_link_libraries(ksim_slaver_route PRIVATE ksim)
target_compile_definitions(ksim_slaver_route PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slaver_route)
# One test per idle phase shift (kenshi-k16.5.21).
foreach(idle 0 5)
	add_test(NAME ksim_slaver_route_${idle} COMMAND ksim_slaver_route ${idle})
	set_tests_properties(ksim_slaver_route_${idle} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
add_test(NAME ksim_slaver_route_npc COMMAND ksim_slaver_route 0 npc)
set_tests_properties(ksim_slaver_route_npc PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.96: the slavers_downed caravan leaves Trading when the player attacks its Boss.
add_executable(ksim_slavers_trading tests/ksim_slavers_trading.cpp)
target_link_libraries(ksim_slavers_trading PRIVATE ksim)
target_compile_definitions(ksim_slavers_trading PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slavers_trading)
add_test(NAME ksim_slavers_trading COMMAND ksim_slavers_trading)
set_tests_properties(ksim_slavers_trading PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_slave_obedience tests/ksim_slave_obedience.cpp)
target_link_libraries(ksim_slave_obedience PRIVATE ksim)
target_compile_definitions(ksim_slave_obedience PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slave_obedience)
add_test(NAME ksim_slave_obedience COMMAND ksim_slave_obedience)
set_tests_properties(ksim_slave_obedience PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_slave_escape tests/ksim_slave_escape.cpp)
target_link_libraries(ksim_slave_escape PRIVATE ksim)
target_compile_definitions(ksim_slave_escape PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slave_escape)
add_test(NAME ksim_slave_escape COMMAND ksim_slave_escape)
set_tests_properties(ksim_slave_escape PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.110: the slavers_downed capture chain, timed against the original's live runs (rwh.89).
add_executable(ksim_slavers_capture tests/ksim_slavers_capture.cpp)
target_link_libraries(ksim_slavers_capture PRIVATE ksim)
target_compile_definitions(ksim_slavers_capture PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slavers_capture)
add_test(NAME ksim_slavers_capture COMMAND ksim_slavers_capture)
set_tests_properties(ksim_slavers_capture PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.702: a world squad's capture of an unreachable body stays blocked (addBlockedTarget re-arm).
add_executable(ksim_capture_unreachable tests/ksim_capture_unreachable.cpp)
target_link_libraries(ksim_capture_unreachable PRIVATE ksim)
target_compile_definitions(ksim_capture_unreachable PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_capture_unreachable)
add_test(NAME ksim_capture_unreachable COMMAND ksim_capture_unreachable)
set_tests_properties(ksim_capture_unreachable PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.288: after the slavers_downed capture no member keeps a fight goal or a weapon in hand.
add_executable(ksim_slavers_sheathe tests/ksim_slavers_sheathe.cpp)
target_link_libraries(ksim_slavers_sheathe PRIVATE ksim)
target_compile_definitions(ksim_slavers_sheathe PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slavers_sheathe)
add_test(NAME ksim_slavers_sheathe COMMAND ksim_slavers_sheathe)
set_tests_properties(ksim_slavers_sheathe PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.137: the capture's restrain time and the capturer's CHAR_DYNAMIC actions on the scenario path.
add_executable(ksim_capture_actions tests/ksim_capture_actions.cpp)
target_link_libraries(ksim_capture_actions PRIVATE ksim)
target_compile_definitions(ksim_capture_actions PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_capture_actions)
add_test(NAME ksim_capture_actions COMMAND ksim_capture_actions)
set_tests_properties(ksim_capture_actions PROPERTIES SKIP_RETURN_CODE 77)
# A travel goal to a town off the streamed mesh walks to the mesh edge (k16.7.12): the Hub caravans of scenarios/slavers_downed.kscen leave after their stay.
add_executable(ksim_offmesh_travel tests/ksim_offmesh_travel.cpp)
target_link_libraries(ksim_offmesh_travel PRIVATE ksim)
target_compile_definitions(ksim_offmesh_travel PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_offmesh_travel)
add_test(NAME ksim_offmesh_travel COMMAND ksim_offmesh_travel)
set_tests_properties(ksim_offmesh_travel PROPERTIES SKIP_RETURN_CODE 77)
# The machine and storage pull tasks (dt24_emptymachine_dismantle): 92 / 247 empty a machine's output into a carrier and stop at
# the vf5 quantity, 284 takes from a storage box; the quantity and picker functions too. The executor through a direct Routine
# on a built world squad member (the vanilla game's job queue issues these; the job-to-goal table is not decoded, UNSURE).
add_executable(ksim_empty_machine tests/ksim_empty_machine.cpp)
target_link_libraries(ksim_empty_machine PRIVATE ksim)
target_compile_definitions(ksim_empty_machine PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_empty_machine)
add_test(NAME ksim_empty_machine COMMAND ksim_empty_machine ${KSIM_NAVWORLD})
set_tests_properties(ksim_empty_machine PROPERTIES SKIP_RETURN_CODE 77)
# The homebase rows of the native selector (dt24_243 / dt24_275 / dt24_175): 243 AQUIRE_FOOD_AT_HOMEBASE (the bandit
# 'occupy town' package; a food box of the squad's faction), 275 SIT_AROUND (a building in the Hub's town circle) and
# 175 SLEEP_ON_FLOOR (an animal template). The live path: a placed squad, the selector, the GOAP plan and the executor.
add_executable(ksim_homebase tests/ksim_homebase.cpp)
target_link_libraries(ksim_homebase PRIVATE ksim)
target_compile_definitions(ksim_homebase PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_homebase)
add_test(NAME ksim_homebase COMMAND ksim_homebase ${KSIM_NAVWORLD})
set_tests_properties(ksim_homebase PROPERTIES SKIP_RETURN_CODE 77)
# The live capture that did not happen (task BR): the client's own setup, the caravan placed at the Hub and the player attacking
# the Boss. The caravan must leave the fight after the KO and take the capture chain.
# The patrol family (task patrol): 36 walks to the package's target, 22 around the member outside town, 216 searches a town's
# buildings (two members in different buildings at once; every point inside a building).
add_executable(ksim_town_patrol tests/ksim_town_patrol.cpp)
target_link_libraries(ksim_town_patrol PRIVATE ksim)
target_compile_definitions(ksim_town_patrol PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_town_patrol)
add_test(NAME ksim_town_patrol COMMAND ksim_town_patrol ${KSIM_NAVWORLD})
set_tests_properties(ksim_town_patrol PROPERTIES SKIP_RETURN_CODE 77)

# The corpse pot 137 (task CH town-residents-3): the Cannibal Capital's own residents take a corpse to the town's Corpse Furnace -
# 138 finds and lifts it, 136 feeds it, the body is consumed.
add_executable(ksim_corpse_pot tests/ksim_corpse_pot.cpp)
target_link_libraries(ksim_corpse_pot PRIVATE ksim)
target_compile_definitions(ksim_corpse_pot PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_corpse_pot)
add_test(NAME ksim_corpse_pot COMMAND ksim_corpse_pot)
set_tests_properties(ksim_corpse_pot PROPERTIES SKIP_RETURN_CODE 77)

# Animals eat on their own (task animal-feed; kinspect dt24_eat section 9): stepMeals' own pack, own backpack, squadmate (pack and
# backpack, 200 u) and an outdoor food box; food kept inside a building is refused.
add_executable(ksim_animal_feed tests/ksim_animal_feed.cpp)
target_link_libraries(ksim_animal_feed PRIVATE ksim)
target_compile_definitions(ksim_animal_feed PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_animal_feed)
add_test(NAME ksim_animal_feed COMMAND ksim_animal_feed)
set_tests_properties(ksim_animal_feed PROPERTIES SKIP_RETURN_CODE 77)

# A squad animal rolls its age at spawn (rwh.193 section 5; kenshi-k16.17.13.14), and the save keeps it (v119).
add_executable(ksim_animal_age tests/ksim_animal_age.cpp)
target_link_libraries(ksim_animal_age PRIVATE ksim)
target_compile_definitions(ksim_animal_age PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_animal_age)
add_test(NAME ksim_animal_age COMMAND ksim_animal_age)
set_tests_properties(ksim_animal_age PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_capture_live tests/ksim_capture_live.cpp)
target_link_libraries(ksim_capture_live PRIVATE ksim)
target_compile_definitions(ksim_capture_live PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_capture_live)
add_test(NAME ksim_capture_live COMMAND ksim_capture_live ${KSIM_NAVWORLD})
set_tests_properties(ksim_capture_live PROPERTIES SKIP_RETURN_CODE 77)
# The slave goals of the native selector (task ds/slave-goals): a slave of a Heft shop walks into a cage of its captor's faction
# (172), a kidnapper squad lifts a downed non-ally and its routine carries it to a cage (254, the home-town case staged with the
# cage owner's faction), and a wandering trader's circuit on arrival satisfies the data-only 39 row's three preconditions.
add_executable(ksim_slave_goals tests/ksim_slave_goals.cpp)
target_link_libraries(ksim_slave_goals PRIVATE ksim)
target_compile_definitions(ksim_slave_goals PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_slave_goals)
add_test(NAME ksim_slave_goals COMMAND ksim_slave_goals ${KSIM_NAVWORLD})
set_tests_properties(ksim_slave_goals PROPERTIES SKIP_RETURN_CODE 77)
# The follow family (task ds/follow-bed; kinspect dt24_141): 141 / 171 are one class and one resolver (the captor hand, else the platoon
# leader) on the live selector, on the Heft mesh.
add_executable(ksim_follow_goals tests/ksim_follow_goals.cpp)
target_link_libraries(ksim_follow_goals PRIVATE ksim)
target_include_directories(ksim_follow_goals PRIVATE src/ksim) # world_resolvers.hpp, the resolver's own unit cases
target_compile_definitions(ksim_follow_goals PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_follow_goals)
add_test(NAME ksim_follow_goals COMMAND ksim_follow_goals ${KSIM_NAVWORLD})
set_tests_properties(ksim_follow_goals PROPERTIES SKIP_RETURN_CODE 77)
# The get-out-of-bed class (task ds/follow-bed; kinspect dt24_208 / s63): the registry rows, the planned wake and 208's emergency on a
# sleeping resident.
add_executable(ksim_get_out_of_bed tests/ksim_get_out_of_bed.cpp)
target_link_libraries(ksim_get_out_of_bed PRIVATE ksim)
target_compile_definitions(ksim_get_out_of_bed PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_get_out_of_bed)
add_test(NAME ksim_get_out_of_bed COMMAND ksim_get_out_of_bed)
add_executable(ksim_sheathe tests/ksim_sheathe.cpp)
target_link_libraries(ksim_sheathe PRIVATE ksim)
target_compile_definitions(ksim_sheathe PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_sheathe)
add_test(NAME ksim_sheathe COMMAND ksim_sheathe)
add_executable(ksim_dead_guy tests/ksim_dead_guy.cpp)
target_link_libraries(ksim_dead_guy PRIVATE ksim)
target_compile_definitions(ksim_dead_guy PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_dead_guy)
add_test(NAME ksim_dead_guy COMMAND ksim_dead_guy ${KSIM_NAVWORLD})
set_tests_properties(ksim_dead_guy PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_quarry_camp tests/ksim_quarry_camp.cpp)
target_link_libraries(ksim_quarry_camp PRIVATE ksim)
target_compile_definitions(ksim_quarry_camp PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_quarry_camp)
add_test(NAME ksim_quarry_camp COMMAND ksim_quarry_camp ${KSIM_NAVWORLD})
set_tests_properties(ksim_quarry_camp PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_bed_choice tests/ksim_bed_choice.cpp)
target_link_libraries(ksim_bed_choice PRIVATE ksim)
target_compile_definitions(ksim_bed_choice PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_bed_choice)
add_test(NAME ksim_bed_choice COMMAND ksim_bed_choice ${KSIM_NAVWORLD})
set_tests_properties(ksim_bed_choice PROPERTIES SKIP_RETURN_CODE 77)
# The manhunters' hunt (task CA): the Manhunters' dialogue (54937, DA_ATTACK_CHASE_FOREVER) in the wild and its town refusal (the Hub),
# then the 167 capture chain on the KO'd player. The patrol is the real world squad (55016-rebirth.mod).
add_executable(ksim_manhunter_hunt tests/ksim_manhunter_hunt.cpp)
target_link_libraries(ksim_manhunter_hunt PRIVATE ksim)
target_compile_definitions(ksim_manhunter_hunt PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_manhunter_hunt)
add_test(NAME ksim_manhunter_hunt COMMAND ksim_manhunter_hunt ${KSIM_NAVWORLD})
set_tests_properties(ksim_manhunter_hunt PROPERTIES SKIP_RETURN_CODE 77)
# The frozen bar brawl (task AR): three debug-spawned squads into the Storm House; the layer that leaves each fighter standing.
# A diagnostic (KSIM_BRAWL_VERBOSE prints the timeline), not a gate: in this reproduction the brawl never starts.
add_executable(ksim_brawl_freeze tests/ksim_brawl_freeze.cpp)
target_link_libraries(ksim_brawl_freeze PRIVATE ksim)
target_compile_definitions(ksim_brawl_freeze PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_brawl_freeze)
add_test(NAME ksim_brawl_freeze COMMAND ksim_brawl_freeze ${KSIM_NAVWORLD})
set_tests_properties(ksim_brawl_freeze PROPERTIES SKIP_RETURN_CODE 77 DISABLED TRUE)
add_executable(ksim_corpses tests/ksim_corpses.cpp)
target_link_libraries(ksim_corpses PRIVATE ksim)
target_compile_definitions(ksim_corpses PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_corpses)
add_test(NAME ksim_corpses COMMAND ksim_corpses)

add_executable(ksim_combat_flag tests/ksim_combat_flag.cpp)
target_link_libraries(ksim_combat_flag PRIVATE ksim)
target_compile_definitions(ksim_combat_flag PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_combat_flag)
add_test(NAME ksim_combat_flag COMMAND ksim_combat_flag ${KSIM_NAVWORLD})
set_tests_properties(ksim_combat_flag PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_combat_flag_scen tests/ksim_combat_flag_scen.cpp)
target_link_libraries(ksim_combat_flag_scen PRIVATE ksim)
target_compile_definitions(ksim_combat_flag_scen PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_combat_flag_scen)
add_test(NAME ksim_combat_flag_scen COMMAND ksim_combat_flag_scen ${KSIM_NAVWORLD})
set_tests_properties(ksim_combat_flag_scen PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME ksim_combat_flag_scen_late COMMAND ksim_combat_flag_scen ${KSIM_NAVWORLD} 177)
set_tests_properties(ksim_combat_flag_scen_late PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_chase_intercept tests/ksim_chase_intercept.cpp)
target_link_libraries(ksim_chase_intercept PRIVATE ksim)
target_compile_definitions(ksim_chase_intercept PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_chase_intercept)
add_test(NAME ksim_chase_intercept COMMAND ksim_chase_intercept ${KSIM_NAVWORLD})
set_tests_properties(ksim_chase_intercept PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_chase_giveup tests/ksim_chase_giveup.cpp)
target_link_libraries(ksim_chase_giveup PRIVATE ksim)
target_compile_definitions(ksim_chase_giveup PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_chase_giveup)
add_test(NAME ksim_chase_giveup COMMAND ksim_chase_giveup)

# A fighter steered straight at a target it cannot walk at (behind a wall within the 50 u direct band): 5 s of no
# progress switches to the routed chase, which goes round (combat.cpp's stuck timer; a hand-built mesh).
add_executable(ksim_chase_stuck tests/ksim_chase_stuck.cpp)
target_link_libraries(ksim_chase_stuck PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_chase_stuck PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_chase_stuck)
add_test(NAME ksim_chase_stuck COMMAND ksim_chase_stuck)

# Task BN: an unreachable melee target is never a terminal state. A bake pinch narrower than the body is walked through
# by the clearance-free route; a target no walk reaches is given up, retried after the block, then left for the goal.
# A hand-built mesh (a wall with a 4 u gap, then with none), like ksim_chase_stuck's.
add_executable(ksim_route_negative_cache tests/ksim_route_negative_cache.cpp)
target_link_libraries(ksim_route_negative_cache PRIVATE ksim RecastNavigation::Detour)
kdata_warnings(ksim_route_negative_cache)
add_test(NAME ksim_route_negative_cache COMMAND ksim_route_negative_cache)

add_executable(ksim_unreachable_recovery tests/ksim_unreachable_recovery.cpp)
target_link_libraries(ksim_unreachable_recovery PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_unreachable_recovery PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_unreachable_recovery)
add_test(NAME ksim_unreachable_recovery COMMAND ksim_unreachable_recovery)

# MQ14 path 2: the squad goal ATTACK_ENEMIES_AND_NEUTRALS (Fighter::attacksNeutrals, CombatArena::bonded).
add_executable(ksim_aggressive tests/ksim_aggressive.cpp)
target_link_libraries(ksim_aggressive PRIVATE ksim)
target_compile_definitions(ksim_aggressive PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_aggressive)
add_test(NAME ksim_aggressive COMMAND ksim_aggressive)


# Building membership (Fighter::building): from where the character is, not only from a door crossing. Needs the Hub cache.
add_executable(ksim_building_membership tests/ksim_building_membership.cpp)
target_link_libraries(ksim_building_membership PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_building_membership PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_building_membership)
add_test(NAME ksim_building_membership COMMAND ksim_building_membership ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_building_membership PROPERTIES FIXTURES_REQUIRED knav_cache)

# Indoors state (Fighter::indoors): ground floor / roof deck / open ground / porch, acid rain gated by it, the weapon indoors mod.
add_executable(ksim_indoors tests/ksim_indoors.cpp)
target_link_libraries(ksim_indoors PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_indoors PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_indoors)
add_test(NAME ksim_indoors COMMAND ksim_indoors ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_indoors PROPERTIES FIXTURES_REQUIRED knav_cache)

# Stealth breaks when a hostile targets the character (task sneak-break): the targeted sneaker answers, a move order still wins,
# and an unnoticed sneaker is not pulled into an ally's fight.
add_executable(ksim_sneak_break tests/ksim_sneak_break.cpp)
target_link_libraries(ksim_sneak_break PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_sneak_break PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_sneak_break)
add_test(NAME ksim_sneak_break COMMAND ksim_sneak_break ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_sneak_break PROPERTIES FIXTURES_REQUIRED knav_cache)

# Containers: the interior-layout storage of a real building as boxes, their `has lock`, `itemtype limit`, save / load.
add_executable(ksim_containers tests/ksim_containers.cpp)
target_link_libraries(ksim_containers PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_containers PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_containers)
add_test(NAME ksim_containers COMMAND ksim_containers ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_containers PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_item_nodes tests/ksim_item_nodes.cpp)
target_link_libraries(ksim_item_nodes PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_item_nodes PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_item_nodes)
add_test(NAME ksim_item_nodes COMMAND ksim_item_nodes ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_item_nodes PROPERTIES FIXTURES_REQUIRED knav_cache)

# The wilderness spawner against the exe rules: the Hub is Border Zone, the weight rule caps live copies (kinspect s71_static.md).
add_executable(ksim_wilderness tests/ksim_wilderness.cpp)
target_link_libraries(ksim_wilderness PRIVATE ksim)
target_compile_definitions(ksim_wilderness PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_wilderness)
add_test(NAME ksim_wilderness COMMAND ksim_wilderness)

add_executable(ksim_spawn_bounty tests/ksim_spawn_bounty.cpp)
target_link_libraries(ksim_spawn_bounty PRIVATE ksim)
target_compile_definitions(ksim_spawn_bounty PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_spawn_bounty)
add_test(NAME ksim_spawn_bounty COMMAND ksim_spawn_bounty)

add_executable(ksim_spawn_biome tests/ksim_spawn_biome.cpp)
target_link_libraries(ksim_spawn_biome PRIVATE ksim)
target_compile_definitions(ksim_spawn_biome PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_spawn_biome)
add_test(NAME ksim_spawn_biome COMMAND ksim_spawn_biome)

# One nest-source rule for placement / spawn / wilderness, judged on the game's facts (kenshi-k16.6.563).
add_executable(ksim_nest_sources tests/ksim_nest_sources.cpp)
target_link_libraries(ksim_nest_sources PRIVATE ksim)
target_compile_definitions(ksim_nest_sources PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_nest_sources)
add_test(NAME ksim_nest_sources COMMAND ksim_nest_sources)

# Crowd spacing (BAR_FIGHT_LOOP.md Finding 0e): avoidance comfort radii, KO'd bodies, depenetration floor, hit-list threats, waiting attackers.
add_executable(ksim_crowd_spacing tests/ksim_crowd_spacing.cpp)
target_link_libraries(ksim_crowd_spacing PRIVATE ksim)
target_compile_definitions(ksim_crowd_spacing PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_crowd_spacing)
add_test(NAME ksim_crowd_spacing COMMAND ksim_crowd_spacing)
add_executable(ksim_crowd_ring tests/ksim_crowd_ring.cpp)
target_link_libraries(ksim_crowd_ring PRIVATE ksim)
target_compile_definitions(ksim_crowd_ring PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_crowd_ring)
add_test(NAME ksim_crowd_ring COMMAND ksim_crowd_ring)

# The body layer (task AW): lying bodies have their own collision layer, spread slowly to rest, are walked over, keep a bed or a
# cage, get a free put-down spot and stay in their building; save/load mid-spread continues the same. Needs the Hub cache.
add_executable(ksim_body_rest tests/ksim_body_rest.cpp)
target_link_libraries(ksim_body_rest PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_body_rest PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_body_rest)
add_test(NAME ksim_body_rest COMMAND ksim_body_rest ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_body_rest PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_body_layer tests/ksim_body_layer.cpp)
target_link_libraries(ksim_body_layer PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_body_layer PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_body_layer)
add_test(NAME ksim_body_layer COMMAND ksim_body_layer ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_body_layer PROPERTIES FIXTURES_REQUIRED knav_cache)

# A fighter backed against a wall never crosses it and no dodge teleports (kenshi-h2v.26; ENGAGEMENT.md "Walls").
add_executable(ksim_fight_wall tests/ksim_fight_wall.cpp)
target_link_libraries(ksim_fight_wall PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_fight_wall PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_fight_wall)
add_test(NAME ksim_fight_wall COMMAND ksim_fight_wall ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_fight_wall PROPERTIES FIXTURES_REQUIRED knav_cache)

# Crowd-fight spacing, measured: attacker-to-target and nearest-neighbour distances, crowd size per target (kenshi-h2v.26 part 1).
add_executable(ksim_fight_spread tests/ksim_fight_spread.cpp)
target_link_libraries(ksim_fight_spread PRIVATE ksim)
target_compile_definitions(ksim_fight_spread PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_fight_spread)
add_test(NAME ksim_fight_spread COMMAND ksim_fight_spread)

# Line of sight phase 1 (task AZ): the per-region collision hull trees and the segment query on real Hub buildings, memory,
# load/unload, the border and the cost. Reads the install at run time; no navmesh needed.
add_executable(ksim_los tests/ksim_los.cpp)
target_link_libraries(ksim_los PRIVATE ksim)
target_compile_definitions(ksim_los PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_los)
add_test(NAME ksim_los COMMAND ksim_los ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_los PROPERTIES FIXTURES_REQUIRED knav_cache)

# CHAR_DYNAMIC fwdSpeed/latSpeed are the body's actual motion (SIMLINK "CHAR_DYNAMIC"): a pushed fighter reports the speed it
# was pushed at, in its own frame, even though it never walked.
add_executable(ksim_velocity_report tests/ksim_velocity_report.cpp)
target_link_libraries(ksim_velocity_report PRIVATE ksim)
target_compile_definitions(ksim_velocity_report PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_velocity_report)
add_test(NAME ksim_velocity_report COMMAND ksim_velocity_report)

add_executable(ksim_combat_facing tests/ksim_combat_facing.cpp)
target_link_libraries(ksim_combat_facing PRIVATE ksim)
target_compile_definitions(ksim_combat_facing PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_combat_facing)
add_test(NAME ksim_combat_facing COMMAND ksim_combat_facing)

add_executable(ksim_beak_zigzag tests/ksim_beak_zigzag.cpp)
target_link_libraries(ksim_beak_zigzag PRIVATE ksim)
target_compile_definitions(ksim_beak_zigzag PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_beak_zigzag)
add_test(NAME ksim_beak_zigzag COMMAND ksim_beak_zigzag)

# The shared crash recorder (kutil/crashlog.hpp): a crasher installs it, sets its tick and raises SIGSEGV; the test expects the signal
# death, exactly one report, and the line, backtrace header and maps dump a reader (addr2line) needs.
add_executable(kutil_crashlog_crasher tests/kutil_crashlog_crasher.cpp)
target_link_libraries(kutil_crashlog_crasher PRIVATE kutil)
kdata_warnings(kutil_crashlog_crasher)
add_executable(kutil_crashlog tests/kutil_crashlog.cpp)
target_link_libraries(kutil_crashlog PRIVATE kutil)
kdata_warnings(kutil_crashlog)
add_test(NAME kutil_crashlog COMMAND kutil_crashlog $<TARGET_FILE:kutil_crashlog_crasher>)
# Task CG: 58 JOB_MEDIC as a package goal - a squadmate treats an injured one, within the 40 u distance cap (dt24_58 / dt16 section 6).
add_executable(ksim_medic tests/ksim_medic.cpp)
target_link_libraries(ksim_medic PRIVATE ksim)
target_compile_definitions(ksim_medic PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_medic)
add_test(NAME ksim_medic COMMAND ksim_medic ${KSIM_NAVWORLD})
set_tests_properties(ksim_medic PROPERTIES SKIP_RETURN_CODE 77)

# Task k16.6.4: self-aid (269 HEAL_MY_LEGS) and who treats a down NPC (MEDICAL_HUNGER_LIVE.md (f)).
add_executable(ksim_self_aid tests/ksim_self_aid.cpp)
target_link_libraries(ksim_self_aid PRIVATE ksim)
target_compile_definitions(ksim_self_aid PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_self_aid)
add_test(NAME ksim_self_aid COMMAND ksim_self_aid ${KSIM_NAVWORLD})
set_tests_properties(ksim_self_aid PROPERTIES SKIP_RETURN_CODE 77)

# The shared crash recorder, in one place (2026-11-08): every ksim / knav test binary compiles tests/test_crashlog.cpp, which installs
# kutil::crashlog from a static initializer with the target's own name, so a test that dies of a signal under ctest leaves
# ~/.local/share/ksim/crashes/crash-<time>-<pid>.txt (the lines name the test) instead of nothing; ksim_simlink's SIGSEGV in a ctest
# -j8 run had no report at all because only kutil_crashlog_crasher and ksim_server installed the handler. The test is the targets whose
# sources are under tests/; the tools in tools/ (ksim_server installs its own) and the *_asan binaries (ASan owns the signals) are out.
get_property(allTargets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(crashRecorderTests 0)
foreach(target IN LISTS allTargets)
	if(NOT target MATCHES "^(ksim|knav)_")
		continue()
	endif()
	get_target_property(targetSources ${target} SOURCES)
	set(isTestBinary FALSE)
	foreach(source IN LISTS targetSources)
		if(source MATCHES "^tests/")
			set(isTestBinary TRUE)
		endif()
	endforeach()
	if(NOT isTestBinary)
		continue()
	endif()
	target_sources(${target} PRIVATE tests/test_crashlog.cpp)
	target_compile_definitions(${target} PRIVATE TEST_CRASHLOG_NAME="${target}")
	target_link_libraries(${target} PRIVATE kutil)
	math(EXPR crashRecorderTests "${crashRecorderTests} + 1")
endforeach()
message(STATUS "crash recorder in ${crashRecorderTests} test binaries")

# Task ds/arrest: 272 SMASH_BUILDING on a bare world.
foreach(dt27_test ksim_smash)
	add_executable(${dt27_test} tests/${dt27_test}.cpp)
	target_link_libraries(${dt27_test} PRIVATE ksim)
	target_compile_definitions(${dt27_test} PRIVATE
		KENSHI_DATA="${KENSHI_DATA}"
	)
	kdata_warnings(${dt27_test})
	add_test(NAME ${dt27_test} COMMAND ${dt27_test})
	set_tests_properties(${dt27_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# Task ds/arrest: the arrest chain, the sentence at the put-in (cage owner's legal faction only) and 211, on the Heft mesh.
add_executable(ksim_arrest tests/ksim_arrest.cpp)
target_link_libraries(ksim_arrest PRIVATE ksim)
target_compile_definitions(ksim_arrest PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_arrest)
add_test(NAME ksim_arrest COMMAND ksim_arrest ${KSIM_NAVWORLD})
set_tests_properties(ksim_arrest PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.6.125 (rwh.104): the guard's arrest order and the slaver's capture chain on one body (Heft).
add_executable(ksim_arrest_vs_capture tests/ksim_arrest_vs_capture.cpp)
target_link_libraries(ksim_arrest_vs_capture PRIVATE ksim)
target_compile_definitions(ksim_arrest_vs_capture PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_arrest_vs_capture)
# One test per case (kenshi-k16.5.21: one run was 1682 s in the queue; each case builds a fresh world per Ids bump, 6 worlds), so ctest runs them in parallel.
foreach(c a b c d e f g)
	add_test(NAME ksim_arrest_vs_capture_${c} COMMAND ksim_arrest_vs_capture ${KSIM_NAVWORLD} ${c})
	set_tests_properties(ksim_arrest_vs_capture_${c} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# Task ds/arrest-2: a law guard sees a player with a bounty and arrests them through the dialogue (Heft).
add_executable(ksim_arrest_auto tests/ksim_arrest_auto.cpp)
target_link_libraries(ksim_arrest_auto PRIVATE ksim)
target_compile_definitions(ksim_arrest_auto PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_arrest_auto)
add_test(NAME ksim_arrest_auto COMMAND ksim_arrest_auto ${KSIM_NAVWORLD})
set_tests_properties(ksim_arrest_auto PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_stair_walks tests/ksim_stair_walks.cpp)
target_link_libraries(ksim_stair_walks PRIVATE ksim)
target_compile_definitions(ksim_stair_walks PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_stair_walks)
add_test(NAME ksim_stair_walks COMMAND ksim_stair_walks)
set_tests_properties(ksim_stair_walks PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_path_offroute tests/ksim_path_offroute.cpp)
target_link_libraries(ksim_path_offroute PRIVATE ksim)
target_compile_definitions(ksim_path_offroute PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_path_offroute)
add_test(NAME ksim_path_offroute COMMAND ksim_path_offroute)
set_tests_properties(ksim_path_offroute PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_path_retry tests/ksim_path_retry.cpp)
target_link_libraries(ksim_path_retry PRIVATE ksim)
target_compile_definitions(ksim_path_retry PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_path_retry)
add_test(NAME ksim_path_retry COMMAND ksim_path_retry)

add_executable(kworld_roads_normalize tests/kworld_roads_normalize.cpp)
target_link_libraries(kworld_roads_normalize PRIVATE ksim)
target_compile_definitions(kworld_roads_normalize PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(kworld_roads_normalize)
add_test(NAME kworld_roads_normalize COMMAND kworld_roads_normalize)

add_executable(ksim_road_route tests/ksim_road_route.cpp)
target_link_libraries(ksim_road_route PRIVATE ksim)
target_compile_definitions(ksim_road_route PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_road_route)
add_test(NAME ksim_road_route COMMAND ksim_road_route)
# kenshi-k16.6.712: the road fixup table (kworld/roadfixups.hpp): parse / apply / write, guard and version refusals, the Hub spike moved off the far route.
add_executable(ksim_road_fixups tests/ksim_road_fixups.cpp)
target_link_libraries(ksim_road_fixups PRIVATE ksim)
target_compile_definitions(ksim_road_fixups PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_road_fixups)
add_test(NAME ksim_road_fixups COMMAND ksim_road_fixups)

add_executable(ksim_retarget_hold tests/ksim_retarget_hold.cpp)
target_link_libraries(ksim_retarget_hold PRIVATE ksim)
target_compile_definitions(ksim_retarget_hold PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_retarget_hold)
add_test(NAME ksim_retarget_hold COMMAND ksim_retarget_hold)

# h2v.2.7: a waiter on a crowded target defends a free enemy's blows and re-picks per pickTarget (kenshi-k16.6.526).
add_executable(ksim_wait_slot_aggro tests/ksim_wait_slot_aggro.cpp)
target_link_libraries(ksim_wait_slot_aggro PRIVATE ksim)
target_compile_definitions(ksim_wait_slot_aggro PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_wait_slot_aggro)
add_test(NAME ksim_wait_slot_aggro COMMAND ksim_wait_slot_aggro)
add_executable(ksim_target_lock tests/ksim_target_lock.cpp)
target_link_libraries(ksim_target_lock PRIVATE ksim)
target_compile_definitions(ksim_target_lock PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_target_lock)
add_test(NAME ksim_target_lock COMMAND ksim_target_lock)
# kenshi-k16.6.65: Simulation::reload leaves nothing of the replaced world (a replayed scenario and a reloaded save equal the fresh-process runs, tick for tick).
add_executable(ksim_reload_hygiene tests/ksim_reload_hygiene.cpp)
target_link_libraries(ksim_reload_hygiene PRIVATE ksim)
target_compile_definitions(ksim_reload_hygiene PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_reload_hygiene)
add_test(NAME ksim_reload_hygiene COMMAND ksim_reload_hygiene ${KSIM_NAVWORLD})
# 2.9 GB peak RSS (k16.6.290), see ksim_bar_fight.
set_tests_properties(ksim_reload_hygiene PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ksim_bigmem)
# kenshi-k16.6.468: a scenario's player squad keeps its full-detail area and nav tiles wherever it walks (gate_heft, 10k u in hops).
add_executable(ksim_player_detail_follow tests/ksim_player_detail_follow.cpp)
target_link_libraries(ksim_player_detail_follow PRIVATE ksim)
target_compile_definitions(ksim_player_detail_follow PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_player_detail_follow)
add_test(NAME ksim_player_detail_follow COMMAND ksim_player_detail_follow ${KSIM_NAVWORLD})
set_tests_properties(ksim_player_detail_follow PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.481: a far move order (goal in an unloaded zone) walks by legs along the line and the roads, never refused (gate_heft, 17.6k u).
add_executable(ksim_far_move tests/ksim_far_move.cpp)
target_link_libraries(ksim_far_move PRIVATE ksim)
target_compile_definitions(ksim_far_move PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_far_move)
add_test(NAME ksim_far_move COMMAND ksim_far_move ${KSIM_NAVWORLD})
set_tests_properties(ksim_far_move PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.720: road look-ahead following vs the exe's marker rule (925 zig-zag, 152 switchback, Heft walk). argv[2]: the rwh295 frames tsv (optional).
add_executable(ksim_road_lookahead tests/ksim_road_lookahead.cpp)
target_link_libraries(ksim_road_lookahead PRIVATE ksim)
target_compile_definitions(ksim_road_lookahead PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_road_lookahead)
add_test(NAME ksim_road_lookahead COMMAND ksim_road_lookahead ${KSIM_NAVWORLD})
set_tests_properties(ksim_road_lookahead PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_road_exe_rule tests/ksim_road_exe_rule.cpp)
target_link_libraries(ksim_road_exe_rule PRIVATE ksim)
target_compile_definitions(ksim_road_exe_rule PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_road_exe_rule)
add_test(NAME ksim_road_exe_rule COMMAND ksim_road_exe_rule ${KSIM_NAVWORLD})
set_tests_properties(ksim_road_exe_rule PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.36: a scenario character's `cage=` key puts it in a cage through Simulation::imprison (state 5 / 6 on the wire is ksim_char_stats'); nothing
# shoots, hits or targets the prisoner while the slavers fight the free Wanderer beside it.
add_executable(ksim_cage_scenario tests/ksim_cage_scenario.cpp)
target_link_libraries(ksim_cage_scenario PRIVATE ksim)
target_compile_definitions(ksim_cage_scenario PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_cage_scenario)
add_test(NAME ksim_cage_scenario COMMAND ksim_cage_scenario ${KSIM_NAVWORLD})
set_tests_properties(ksim_cage_scenario PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.6.254: the Slaver caravan spawned inside Heft (cage_prisoner) gets the arrival stay and a real routine, not 141 at score 0.
add_executable(ksim_caravan_idle tests/ksim_caravan_idle.cpp)
target_link_libraries(ksim_caravan_idle PRIVATE ksim)
target_compile_definitions(ksim_caravan_idle PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_caravan_idle)
add_test(NAME ksim_caravan_idle COMMAND ksim_caravan_idle ${KSIM_NAVWORLD})
set_tests_properties(ksim_caravan_idle PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.6.463: Heft's Defensive Gate IV crew at full detail stands on item31's layout, one member per spot.
add_executable(ksim_gate_crew tests/ksim_gate_crew.cpp)
target_link_libraries(ksim_gate_crew PRIVATE ksim)
target_compile_definitions(ksim_gate_crew PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_gate_crew)
add_test(NAME ksim_gate_crew COMMAND ksim_gate_crew ${KSIM_NAVWORLD})
set_tests_properties(ksim_gate_crew PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.6.820: a Lock order on Heft's Gate IV door ends with an event 13 (kenshi-xu27).
add_executable(ksim_interact_gate_lock tests/ksim_interact_gate_lock.cpp)
target_link_libraries(ksim_interact_gate_lock PRIVATE ksim)
target_compile_definitions(ksim_interact_gate_lock PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_interact_gate_lock)
add_test(NAME ksim_interact_gate_lock COMMAND ksim_interact_gate_lock ${KSIM_NAVWORLD})
set_tests_properties(ksim_interact_gate_lock PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.6.132: CHAR_DYNAMIC kFlagDrawn follows the weapon in the hand (tasks 6 / 7), cage_prisoner's guards after the fight.
add_executable(ksim_drawn_flag tests/ksim_drawn_flag.cpp ../client/src/net/simlink.cpp)
target_include_directories(ksim_drawn_flag PRIVATE ../client/src)
target_link_libraries(ksim_drawn_flag PRIVATE ksim)
target_compile_definitions(ksim_drawn_flag PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_drawn_flag)
add_test(NAME ksim_drawn_flag COMMAND ksim_drawn_flag ${KSIM_NAVWORLD})
set_tests_properties(ksim_drawn_flag PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.5.7: the cage wire: CAGES (64), MENU_ENTRIES bits 16-18 + pickChance on a cage key, order 60 actions 18 / 19 / 20, against the committed frame dump
# tests/fixtures/cage_prisoner_cages.hex (client's decoder fixture, k16.5.6).
add_executable(ksim_cage_wire tests/ksim_cage_wire.cpp)
target_link_libraries(ksim_cage_wire PRIVATE ksim)
target_compile_definitions(ksim_cage_wire PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_cage_wire)
add_test(NAME ksim_cage_wire COMMAND ksim_cage_wire ${KSIM_NAVWORLD})
set_tests_properties(ksim_cage_wire PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.6.739: a carrier's order 60 on a free cage key puts the carried body in (exe PUT_IN_CAGE 0x6c carry swap).
add_executable(ksim_carry_cage tests/ksim_carry_cage.cpp)
target_link_libraries(ksim_carry_cage PRIVATE ksim)
target_compile_definitions(ksim_carry_cage PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_carry_cage)
add_test(NAME ksim_carry_cage COMMAND ksim_carry_cage ${KSIM_NAVWORLD})
set_tests_properties(ksim_carry_cage PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_cage_capture tests/ksim_cage_capture.cpp)
target_link_libraries(ksim_cage_capture PRIVATE ksim)
target_compile_definitions(ksim_cage_capture PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_cage_capture)
add_test(NAME ksim_cage_capture COMMAND ksim_cage_capture ${KSIM_NAVWORLD})
set_tests_properties(ksim_cage_capture PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-h2v.21.3: scenarios/food_raid.kscen loaded as the server does (the `town` / `container` / `raid` records): the Sand ninja force reaches the player
# town, a raider carries Bread out of the chest, the force leaves.
add_executable(ksim_food_raid_scenario tests/ksim_food_raid_scenario.cpp)
target_link_libraries(ksim_food_raid_scenario PRIVATE ksim)
target_compile_definitions(ksim_food_raid_scenario PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_food_raid_scenario)
add_test(NAME ksim_food_raid_scenario COMMAND ksim_food_raid_scenario ${KSIM_NAVWORLD})
set_tests_properties(ksim_food_raid_scenario PROPERTIES SKIP_RETURN_CODE 77)
# kenshi-k16.5.17: scenarios/perception_cheese.kscen headless (rwh.166 / rwh.167): a crossbow ranger outside the King Gorillo's nest wall holds the ranged
# attack for 3 sim-minutes; Perception and Crossbows follow the aim-XP closed form, no bolt lands, the task stays the ranged attack.
add_executable(ksim_perception_cheese tests/ksim_perception_cheese.cpp)
target_link_libraries(ksim_perception_cheese PRIVATE ksim)
target_compile_definitions(ksim_perception_cheese PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_perception_cheese)
add_test(NAME ksim_perception_cheese COMMAND ksim_perception_cheese ${KSIM_NAVWORLD})
set_tests_properties(ksim_perception_cheese PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-h2v.22.2: scenarios/raid_takeover.kscen: the Crab Raiders assault the player town, occupy it, throw the downed Wanderer out; the occupation ends when they die.
add_executable(ksim_raid_takeover_scenario tests/ksim_raid_takeover_scenario.cpp)
target_link_libraries(ksim_raid_takeover_scenario PRIVATE ksim)
target_compile_definitions(ksim_raid_takeover_scenario PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_raid_takeover_scenario)
add_test(NAME ksim_raid_takeover_scenario COMMAND ksim_raid_takeover_scenario ${KSIM_NAVWORLD})
set_tests_properties(ksim_raid_takeover_scenario PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-h2v.13.3: the dismemberment scenarios (dismember_peeler: armour, limbs in order, torso, release; dismember_fight: a hit takes an arm off), one limb item each.
add_executable(ksim_dismember_scen tests/ksim_dismember_scen.cpp)
target_link_libraries(ksim_dismember_scen PRIVATE ksim)
target_compile_definitions(ksim_dismember_scen PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_dismember_scen)
add_test(NAME ksim_dismember_scen COMMAND ksim_dismember_scen ${KSIM_NAVWORLD})
set_tests_properties(ksim_dismember_scen PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-h2v.16.3: scenarios/progress_bars.kscen starts every kind of task a bar shows at once (TASK_PROGRESS kinds 1, 7, 10, 11); a dropped task drops its kind.
add_executable(ksim_progress_scen tests/ksim_progress_scen.cpp)
target_link_libraries(ksim_progress_scen PRIVATE ksim)
target_compile_definitions(ksim_progress_scen PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_progress_scen)
add_test(NAME ksim_progress_scen COMMAND ksim_progress_scen ${KSIM_NAVWORLD})
set_tests_properties(ksim_progress_scen PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-h2v.15.3: scenarios/shackled_escape.kscen; pick, cut, a refused break and a break by the orders the inventory menu gives.
add_executable(ksim_shackled_escape_scen tests/ksim_shackled_escape_scen.cpp)
target_link_libraries(ksim_shackled_escape_scen PRIVATE ksim)
target_compile_definitions(ksim_shackled_escape_scen PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_shackled_escape_scen)
add_test(NAME ksim_shackled_escape_scen COMMAND ksim_shackled_escape_scen ${KSIM_NAVWORLD})
set_tests_properties(ksim_shackled_escape_scen PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_hub_residents tests/ksim_hub_residents.cpp)
target_link_libraries(ksim_hub_residents PRIVATE ksim)
target_compile_definitions(ksim_hub_residents PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_hub_residents)
add_test(NAME ksim_hub_residents COMMAND ksim_hub_residents)
# kenshi-h2v.6.3: the debug cheat's knock out gets the slaver handling (down, strip, cuff, lift) a KO from damage gets (slavers_downed on the Hub's runtime mesh).
add_executable(ksim_cheat_slavers tests/ksim_cheat_slavers.cpp)
target_link_libraries(ksim_cheat_slavers PRIVATE ksim)
target_compile_definitions(ksim_cheat_slavers PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_cheat_slavers)
add_test(NAME ksim_cheat_slavers COMMAND ksim_cheat_slavers)
set_tests_properties(ksim_cheat_slavers PROPERTIES SKIP_RETURN_CODE 77)

# The member delta store (residents.cpp demoteWorldSquad / buildMember, RUNTIME_SAVE.md v99): what a world squad's members keep while it is a token.
add_executable(ksim_delta_store tests/ksim_delta_store.cpp)
target_link_libraries(ksim_delta_store PRIVATE ksim)
target_compile_definitions(ksim_delta_store PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_delta_store)
add_test(NAME ksim_delta_store COMMAND ksim_delta_store ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_delta_store PROPERTIES FIXTURES_REQUIRED knav_cache)

# kenshi-k16.5.13: a demoted member with a Routine keeps its weapon and name (demoteWorldSquad read a Fighter moved by remove<Routine>).
add_executable(ksim_demote_weapon tests/ksim_demote_weapon.cpp)
target_link_libraries(ksim_demote_weapon PRIVATE ksim)
target_compile_definitions(ksim_demote_weapon PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_demote_weapon)
add_test(NAME ksim_demote_weapon COMMAND ksim_demote_weapon ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_demote_weapon PROPERTIES FIXTURES_REQUIRED knav_cache)

# An untouched squad's demote + promote leaves the world hash as it was before the delta store (pinned on the base).
add_executable(ksim_delta_untouched tests/ksim_delta_untouched.cpp)
target_link_libraries(ksim_delta_untouched PRIVATE ksim)
target_compile_definitions(ksim_delta_untouched PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_delta_untouched)
add_test(NAME ksim_delta_untouched COMMAND ksim_delta_untouched ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_delta_untouched PROPERTIES FIXTURES_REQUIRED knav_cache)

# The off-screen medical step and the corpses it leaves (stored_medical.cpp, corpses.cpp, RUNTIME_SAVE.md v100): behind setOffscreenMedical.
add_executable(ksim_offscreen_medical tests/ksim_offscreen_medical.cpp)
target_link_libraries(ksim_offscreen_medical PRIVATE ksim)
target_compile_definitions(ksim_offscreen_medical PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_offscreen_medical)
add_test(NAME ksim_offscreen_medical COMMAND ksim_offscreen_medical ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_offscreen_medical PROPERTIES FIXTURES_REQUIRED knav_cache)

# A member's build slot (state::MemberSlot, residents.cpp templateFighter, RUNTIME_SAVE.md v101): a squadmate leaving does not move the others' dice.
add_executable(ksim_member_slot tests/ksim_member_slot.cpp)
target_link_libraries(ksim_member_slot PRIVATE ksim)
target_compile_definitions(ksim_member_slot PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_member_slot)
add_test(NAME ksim_member_slot COMMAND ksim_member_slot ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_member_slot PROPERTIES FIXTURES_REQUIRED knav_cache)

# Stragglers (state::StragglerOf, residents.cpp demoteWorldSquad / stepStragglers, RUNTIME_SAVE.md v102): a member left apart at demotion is a token of its own.
# Original-save import stage 2 (kenshi-cym.2): a hand-written save (kdata::write) imported into a new game's world; needs no navmesh.
add_executable(ksim_profile tests/ksim_profile.cpp)
target_link_libraries(ksim_profile PRIVATE ksim)
target_compile_definitions(ksim_profile PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_profile)
add_test(NAME ksim_profile COMMAND ksim_profile)

add_executable(ksim_skin_harvest tests/ksim_skin_harvest.cpp)
target_link_libraries(ksim_skin_harvest PRIVATE ksim)
target_compile_definitions(ksim_skin_harvest PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_skin_harvest)
add_test(NAME ksim_skin_harvest COMMAND ksim_skin_harvest)
set_tests_properties(ksim_skin_harvest PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_save_import_test tests/ksim_save_import.cpp)
target_link_libraries(ksim_save_import_test PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_save_import_test PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_save_import_test)
add_test(NAME ksim_save_import COMMAND ksim_save_import_test ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_save_import PROPERTIES FIXTURES_REQUIRED knav_cache)
# Stage 3a also checks the real SkinHouse save when this is set (it is a game-derived file, not in the repo).
# kenshi-cym.42: ksim_server --import-save / --paused / --import-report, end to end (in-process server, raw client). The fixture is a small save the
# save-import test binary writes (--write-fixture) into the build dir.
add_executable(ksim_server_import tests/ksim_server_import.cpp ../client/src/net/simlink.cpp)
target_include_directories(ksim_server_import PRIVATE ../client/src)
target_link_libraries(ksim_server_import PRIVATE ksim)
target_compile_definitions(ksim_server_import PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_server_import)
add_test(NAME ksim_server_import_fixture COMMAND ksim_save_import_test --write-fixture ${CMAKE_CURRENT_BINARY_DIR}/server_import_save)
set_tests_properties(ksim_server_import_fixture PROPERTIES FIXTURES_SETUP server_import_save)
add_test(NAME ksim_server_import COMMAND ksim_server_import ${CMAKE_CURRENT_BINARY_DIR}/server_import_save)
set_tests_properties(ksim_server_import PROPERTIES FIXTURES_REQUIRED server_import_save TIMEOUT 600)

set(KSIM_SKINHOUSE "" CACHE PATH "the SkinHouse save folder (quick.save, platoon/) for ksim_save_import's world-state section")
if(KSIM_SKINHOUSE)
	set_tests_properties(ksim_save_import PROPERTIES ENVIRONMENT "KSIM_SKINHOUSE=${KSIM_SKINHOUSE}")
endif()

add_executable(ksim_stragglers tests/ksim_stragglers.cpp)
target_link_libraries(ksim_stragglers PRIVATE ksim)
target_compile_definitions(ksim_stragglers PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_stragglers)
add_test(NAME ksim_stragglers COMMAND ksim_stragglers ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_stragglers PROPERTIES FIXTURES_REQUIRED knav_cache)

# Off-screen fights between hostile tokens (token_fight.cpp, token_fights.cpp, COMBAT_OPTIONS.md 9): behind setOffscreenFights.
add_executable(ksim_token_fights tests/ksim_token_fights.cpp)
target_link_libraries(ksim_token_fights PRIVATE ksim)
target_compile_definitions(ksim_token_fights PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_token_fights)
add_test(NAME ksim_token_fights COMMAND ksim_token_fights ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_token_fights PROPERTIES FIXTURES_REQUIRED knav_cache)

# kenshi-h2v.14.3: scenarios/venge_beams*.kscen (day, approach, night): the pin, the burn, the shelter, the Dustcoat, the border.
add_executable(ksim_venge_scen tests/ksim_venge_scen.cpp)
target_link_libraries(ksim_venge_scen PRIVATE ksim)
target_compile_definitions(ksim_venge_scen PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_venge_scen)
add_test(NAME ksim_venge_scen COMMAND ksim_venge_scen ${KSIM_NAVWORLD})
set_tests_properties(ksim_venge_scen PROPERTIES SKIP_RETURN_CODE 77)

# A fighter pushed or backing off a ledge stays on its own floor: no snap between levels (kenshi-h2v.30; ENGAGEMENT.md "Walls").
add_executable(ksim_fight_levels tests/ksim_fight_levels.cpp)
target_link_libraries(ksim_fight_levels PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_fight_levels PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_fight_levels)
add_test(NAME ksim_fight_levels COMMAND ksim_fight_levels ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_fight_levels PROPERTIES FIXTURES_REQUIRED knav_cache)

# k16.8.41: surface under every step of the Hub bar stair (zone 20,32).
add_executable(ksim_hub_stair_seam tests/ksim_hub_stair_seam.cpp)
target_link_libraries(ksim_hub_stair_seam PRIVATE ksim RecastNavigation::Detour)
kdata_warnings(ksim_hub_stair_seam)
add_test(NAME ksim_hub_stair_seam COMMAND ksim_hub_stair_seam ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_hub_stair_seam PROPERTIES FIXTURES_REQUIRED knav_cache)

# The profile query over SIMLINK (SIMLINK.md PROFILE, order 120, frame 70): runs in both KSIM_PROFILE builds (OFF answers unsupported).
add_executable(ksim_profile_wire tests/ksim_profile_wire.cpp)
target_link_libraries(ksim_profile_wire PRIVATE ksim)
target_compile_definitions(ksim_profile_wire PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_profile_wire)
add_test(NAME ksim_profile_wire COMMAND ksim_profile_wire ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_profile_wire PROPERTIES FIXTURES_REQUIRED knav_cache)

# The research state over SIMLINK (SIMLINK.md RESEARCH_STATE, frame 63; CRAFTING_SLICE 9.8).
add_executable(ksim_research_wire tests/ksim_research_wire.cpp)
target_link_libraries(ksim_research_wire PRIVATE ksim)
target_compile_definitions(ksim_research_wire PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_research_wire)
add_test(NAME ksim_research_wire COMMAND ksim_research_wire ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_research_wire PROPERTIES FIXTURES_REQUIRED knav_cache)

# kenshi-51x.5: world.npc_raids off (the default) targets player-owned towns only; a world with no player town starts no raid.
add_executable(ksim_player_raids tests/ksim_player_raids.cpp)
target_link_libraries(ksim_player_raids PRIVATE ksim)
target_compile_definitions(ksim_player_raids PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_player_raids)
add_test(NAME ksim_player_raids COMMAND ksim_player_raids)

add_executable(ksim_storm_door tests/ksim_storm_door.cpp)
target_link_libraries(ksim_storm_door PRIVATE ksim)
target_compile_definitions(ksim_storm_door PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_storm_door)
add_test(NAME ksim_storm_door COMMAND ksim_storm_door ${KSIM_NAVWORLD})
set_tests_properties(ksim_storm_door PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_route_wall_start tests/ksim_route_wall_start.cpp)
target_link_libraries(ksim_route_wall_start PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_route_wall_start PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_route_wall_start)
add_test(NAME ksim_route_wall_start COMMAND ksim_route_wall_start ${KSIM_NAVWORLD})
set_tests_properties(ksim_route_wall_start PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_foyer_route tests/ksim_foyer_route.cpp)
target_link_libraries(ksim_foyer_route PRIVATE ksim)
target_compile_definitions(ksim_foyer_route PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_foyer_route)
add_test(NAME ksim_foyer_route COMMAND ksim_foyer_route ${KSIM_NAVWORLD})
set_tests_properties(ksim_foyer_route PROPERTIES SKIP_RETURN_CODE 77)

add_executable(knav_route_shared tests/knav_route_shared.cpp)
target_link_libraries(knav_route_shared PRIVATE ksim)
target_compile_definitions(knav_route_shared PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_route_shared)
add_test(NAME knav_route_shared COMMAND knav_route_shared ${KSIM_NAVWORLD})
set_tests_properties(knav_route_shared PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_stair_click tests/ksim_stair_click.cpp)
target_link_libraries(ksim_stair_click PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_stair_click PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_stair_click)
add_test(NAME ksim_stair_click COMMAND ksim_stair_click ${KSIM_NAVWORLD})
set_tests_properties(ksim_stair_click PROPERTIES SKIP_RETURN_CODE 77)

add_executable(knav_counter_arm tests/knav_counter_arm.cpp)
target_link_libraries(knav_counter_arm PRIVATE knav)
target_compile_definitions(knav_counter_arm PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_counter_arm)
add_test(NAME knav_counter_arm COMMAND knav_counter_arm ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(knav_counter_arm PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_counter_path tests/ksim_counter_path.cpp)
target_link_libraries(ksim_counter_path PRIVATE ksim)
target_compile_definitions(ksim_counter_path PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_counter_path)
add_test(NAME ksim_counter_path COMMAND ksim_counter_path ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_counter_path PROPERTIES FIXTURES_REQUIRED knav_cache)

add_executable(ksim_stand_at_node tests/ksim_stand_at_node.cpp)
target_link_libraries(ksim_stand_at_node PRIVATE ksim)
target_compile_definitions(ksim_stand_at_node PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
	KSIM_AI_TABLE="${KSIM_AI_TABLE}"
	KSIM_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
kdata_warnings(ksim_stand_at_node)
add_test(NAME ksim_stand_at_node COMMAND ksim_stand_at_node)

# Tests of 20 s or more: label `slow` (ctest cost data on dev, 2026-11-13; user 2026-11-13: "automate the test like you said to skip the
# long tests"). Workers run `ctest -LE slow` plus the slow tests their change touches; the sim lead runs the full suite on each ksim head
# (docs/RUNBOOK.md "Dev box"). A new test that runs 20 s or more is added here. if(TEST): a name missing from this tree is skipped.
set(KSIM_SLOW_TESTS
	knav_ground_objects
	ksim_road_lookahead
	ksim_road_exe_rule
	ksim_quarry_camp
	ksim_gate_crew
	ksim_interact_gate_lock
	ksim_siv_residents
	ksim_far_move
	ksim_server_import
	ksim_venge_population
	ksim_newgame_starts_0
	ksim_newgame_starts_1
	ksim_newgame_starts_2
	ksim_newgame_starts_3
	ksim_newgame_starts_4
	ksim_newgame_starts_5
	ksim_newgame_starts_6
	ksim_newgame_starts_7
	ksim_newgame_starts_8
	ksim_newgame_starts_9
	ksim_newgame_starts_10
	ksim_newgame_starts_11
	ksim_nav_islands
	ksim_door_sweep
	ksim_combat_join
	knav_interior_walk
	knav_bake_world_sample
	knav_bake_cache_reuse
	knav_navhash
	ksim_after_fight
	ksim_aggressive
	ksim_aggro_drop
	ksim_ai
	ksim_animals
	ksim_army_travel
	ksim_arrest
	ksim_arrest_auto
	ksim_arrest_vs_capture_a
	ksim_arrest_vs_capture_b
	ksim_arrest_vs_capture_c
	ksim_arrest_vs_capture_d
	ksim_arrest_vs_capture_e
	ksim_arrest_vs_capture_f
	ksim_arrest_vs_capture_g
	ksim_bar_brawl
	ksim_bar_brawl_thug
	ksim_bar_fight
	ksim_bar_spread
	ksim_barman_node
	ksim_barman_trade
	ksim_bed_choice
	ksim_behaviour
	ksim_board_save
	ksim_body_layer
	ksim_body_rest
	ksim_building_membership
	ksim_buildings_build
	ksim_bw_orders
	ksim_cage_capture
	ksim_cage_scenario
	ksim_cage_wire
	ksim_cannibal_pot
	ksim_capture_actions
	ksim_capture_live
	ksim_carrier_drop
	ksim_carry
	ksim_chase_giveup
	ksim_chase_intercept
	ksim_cheat_slavers
	ksim_construction
	ksim_corpses
	ksim_crime
	ksim_steal_chance
	ksim_lockpick_seq
	ksim_delta_store
	ksim_dialogue_compare
	ksim_dialogue_econ
	ksim_dialogue_events
	ksim_dialogue_menu
	ksim_dialogue_recruit
	ksim_discovery
	ksim_dismember_scen
	ksim_doors
	ksim_drawn_flag
	ksim_eat_kill
	ksim_economy_caravan
	ksim_economy_flow
	ksim_economy_hooks
	ksim_escape
	ksim_faction
	ksim_factionrel
	ksim_fight_1v1
	ksim_fight_crowd
	ksim_fight_crowd_real
	ksim_fight_real
	ksim_fight_trace
	ksim_fog_eat
	ksim_follow
	ksim_follow_goals
	ksim_food_raid_scenario
	ksim_perception_cheese
	ksim_gates
	ksim_goal_label
	ksim_home_sit
	ksim_home_walk
	ksim_homebase
	ksim_indoors
	ksim_interact
	ksim_jobs
	ksim_jobs3
	ksim_journey
	ksim_limb_tint
	ksim_load_pose
	ksim_lock_save
	ksim_locks
	ksim_loot_storage
	ksim_machines
	ksim_machines2
	ksim_manhunter_hunt
	ksim_medic
	ksim_medic_follower_patient
	ksim_medic_hold_order
	ksim_move
	ksim_move_detour
	ksim_native_cache
	ksim_native_nav
	ksim_nav_regions
	ksim_new_game_twice
	ksim_newgame
	ksim_newgame_settled
	ksim_start_guy_dog
	ksim_offmesh_travel
	ksim_offscreen_medical
	ksim_path_offroute
	ksim_path_retry
	ksim_peeler
	ksim_perception
	ksim_player_raids
	ksim_playtest_metrics
	ksim_producers
	ksim_progress_scen
	ksim_prospect
	ksim_provenance
	ksim_putdown_carried
	ksim_raid_goals
	ksim_raid_loot
	ksim_raid_takeover_scenario
	ksim_raid_victory_stay
	ksim_reload_hygiene
	ksim_rescue_leader
	ksim_run_away
	ksim_save_import
	ksim_save_mid_order_a
	ksim_save_mid_order_b
	ksim_save_mid_order_c
	ksim_scenario_load
	ksim_self_aid
	ksim_severed_limbs
	ksim_shackled_escape_scen
	ksim_sheathe
	ksim_simlink
	ksim_slave_escape
	ksim_slave_goals
	ksim_slave_loot
	ksim_slave_obedience
	ksim_slave_trade
	ksim_slaver_caravan
	ksim_slaver_deliver
	ksim_slaver_route_0
	ksim_slaver_route_5
	ksim_slaver_route_npc
	ksim_slavers_capture
	ksim_slavers_sheathe
	ksim_slavers_trading
	ksim_slavers_trading_s12
	ksim_small_exec
	ksim_sneak_break
	ksim_spectate_fight
	ksim_spectate_stagger
	ksim_squads
	ksim_stair_chase
	ksim_stair_walks
	ksim_standing
	ksim_stragglers
	ksim_taskprogress
	ksim_thug_walk
	ksim_token_fights
	ksim_town_aggro
	ksim_town_bar_synthetic
	ksim_town_bar_realHub
	ksim_town_bar_realHeng
	ksim_town_beds_sleepAndWake
	ksim_town_beds_sharedYard
	ksim_town_beds_shopNight
	ksim_town_beds_staleDoors
	ksim_town_beds_strangers
	ksim_town_beds_shooEscalation
	ksim_town_beds_determinism
	ksim_town_guards
	ksim_town_patrol
	ksim_town_scorer
	ksim_cadence_scale
	ksim_far_squad_step
	ksim_trade
	ksim_trader_circuit
	ksim_travel_town
	ksim_unreachable_recovery
	ksim_venge_beams
	ksim_venge_scen
	ksim_weather
	ksim_world_fighters
	ksim_world_follow_traceReadOnly
	ksim_world_follow_scenario
	ksim_world_follow_saveLoad
	ksim_world_follow_carrotRecovery
	ksim_world_follow_dayRegression
	ksim_world_hub
	ksim_world_offmesh
	ksim_patrol_threat
	ksim_path_walk
	ksim_hub_west_corridor
)
# The world tests run on the runtime mesh and need only KENSHI_DATA; only the bake-kept ones (KSIM_BAKE_TESTS below) take the navmesh bake
# (KSIM_NAVWORLD) and they skip (77) on their own when it holds no region files.
file(GLOB navworldRegions "${KSIM_NAVWORLD}/region_*.knav")
if(NOT navworldRegions)
	message(STATUS "no navmesh bake at ${KSIM_NAVWORLD}: the bake-kept tests (ksim_nav_regions, knav_interior_walk, ...) skip")
endif()
add_executable(knav_outpost_floor2 tests/knav_outpost_floor2.cpp)
target_link_libraries(knav_outpost_floor2 PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(knav_outpost_floor2 PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(knav_outpost_floor2)
add_test(NAME knav_outpost_floor2 COMMAND knav_outpost_floor2)
set_tests_properties(knav_outpost_floor2 PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.6.253: no character of cage_prisoner (spawn, then 60 game s of the fight) stands inside a solid prop's collision (tests/prop_clip.hpp, usable by any scenario test).


# kenshi-k16.6.368: fights at the Storm House counter and barrel never put a fighter inside a solid prop (KSIM_PROP_BODY=<u> grows the solids by a body radius: fails today).
add_executable(ksim_fight_prop tests/ksim_fight_prop.cpp)
target_link_libraries(ksim_fight_prop PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_fight_prop PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_fight_prop)
add_test(NAME ksim_fight_prop COMMAND ksim_fight_prop ${KSIM_NAVWORLD})
set_tests_properties(ksim_fight_prop PROPERTIES SKIP_RETURN_CODE 77)
add_executable(ksim_fight_across_prop tests/ksim_fight_across_prop.cpp)
target_link_libraries(ksim_fight_across_prop PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_fight_across_prop PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_fight_across_prop)
add_test(NAME ksim_fight_across_prop COMMAND ksim_fight_across_prop ${KSIM_NAVWORLD})
set_tests_properties(ksim_fight_across_prop PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.6.416: no melee engage or swing with a furniture ring (a table) between attacker and target (oracle rwh.186 / rwh.187).
add_executable(ksim_melee_table tests/ksim_melee_table.cpp)
target_link_libraries(ksim_melee_table PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_melee_table PRIVATE KENSHI_DATA="${KENSHI_DATA}" KSIM_AI_TABLE="${KSIM_AI_TABLE}")
kdata_warnings(ksim_melee_table)
add_test(NAME ksim_melee_table COMMAND ksim_melee_table ${KSIM_NAVWORLD})
set_tests_properties(ksim_melee_table PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.7.89 tool, not a test: classify points against the furniture ring (tests/.cpp).

# kenshi-k16.6.370: the cage_prisoner Prisoner's sim position is inside its cage's collision box (tests/prop_clip.hpp), at the spawn and over 60 game s.
add_executable(ksim_cage_inside tests/ksim_cage_inside.cpp)
target_link_libraries(ksim_cage_inside PRIVATE ksim)
target_compile_definitions(ksim_cage_inside PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_cage_inside)
add_test(NAME ksim_cage_inside COMMAND ksim_cage_inside ${KSIM_NAVWORLD})
set_tests_properties(ksim_cage_inside PROPERTIES SKIP_RETURN_CODE 77)

# kenshi-k16.6.442: the Heft slave shop's goal-183 watchers stand at distinct spots near the cage, not stacked on the prisoner's old spawn spot.
add_executable(ksim_cage_watchers tests/ksim_cage_watchers.cpp)
target_link_libraries(ksim_cage_watchers PRIVATE ksim)
target_compile_definitions(ksim_cage_watchers PRIVATE
	KENSHI_DATA="${KENSHI_DATA}"
)
kdata_warnings(ksim_cage_watchers)
add_test(NAME ksim_cage_watchers COMMAND ksim_cage_watchers ${KSIM_NAVWORLD})
set_tests_properties(ksim_cage_watchers PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ksim_siv_floors tests/ksim_siv_floors.cpp)
target_link_libraries(ksim_siv_floors PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_siv_floors PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_siv_floors)
add_test(NAME ksim_siv_floors COMMAND ksim_siv_floors)
set_tests_properties(ksim_siv_floors PROPERTIES SKIP_RETURN_CODE 77)
# A running player under attack keeps its move order: no shove off the route, the original's cost per hit (kenshi-k16.6.343).
add_executable(ksim_move_under_attack tests/ksim_move_under_attack.cpp)
target_link_libraries(ksim_move_under_attack PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_move_under_attack PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_move_under_attack)
add_test(NAME ksim_move_under_attack COMMAND ksim_move_under_attack ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_move_under_attack PROPERTIES FIXTURES_REQUIRED knav_cache)
add_executable(ksim_order_attack_far tests/ksim_order_attack_far.cpp)
target_link_libraries(ksim_order_attack_far PRIVATE ksim RecastNavigation::Detour)
target_compile_definitions(ksim_order_attack_far PRIVATE KENSHI_DATA="${KENSHI_DATA}")
kdata_warnings(ksim_order_attack_far)
add_test(NAME ksim_order_attack_far COMMAND ksim_order_attack_far ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
set_tests_properties(ksim_order_attack_far PROPERTIES FIXTURES_REQUIRED knav_cache)

# Tile assembly: Recast polygons as native faces, cut by the generator's surfaces, one nativeTiles call (knav/assemble.hpp, kenshi-k16.7.144).

# After every test that reads the install (kenshi-379.14.8: tests declared below this block got no data guard, no skip code and no disable).
# Without the Kenshi install the tests that read it skip (exit 77) instead of failing (kenshi-379.15): every test binary compiled with
# KENSHI_DATA takes tests/test_needs_data.cpp, and every test treats 77 as a skip. Tests that need the navmesh bake (KSIM_NAVWORLD) skip on their own.
get_property(allTargets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS allTargets)
	get_target_property(targetSources ${target} SOURCES)
	get_target_property(targetDefinitions ${target} COMPILE_DEFINITIONS)
	if(targetSources MATCHES "(^|;)tests/" AND targetDefinitions MATCHES "(^|;)KENSHI_DATA=")
		target_sources(${target} PRIVATE tests/test_needs_data.cpp)
	endif()
endforeach()
get_property(allTests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS allTests)
	set_tests_properties(${test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
# The tests that run a tool or the server on the game install (not a test binary of their own), and the ones that need the navmesh bake fixtures
# built from it, are disabled (reported "Not Run (Disabled)", not a failure) when the install is missing.
if(NOT EXISTS "${KENSHI_DATA}/gamedata.base")
	if(KSIM_DEV_PATHS)
		message(FATAL_ERROR "no game data at ${KENSHI_DATA} (no gamedata.base): the dev tree would skip ~140 tests silently. Pass -DKENSHI_DATA=<Kenshi data dir>.")
	endif()
	message(STATUS "no game data at ${KENSHI_DATA}: tests that need it skip")
	foreach(test IN LISTS allTests)
		get_test_property(${test} FIXTURES_REQUIRED testFixtures)
		get_test_property(${test} FIXTURES_SETUP testSetups)
		if(test MATCHES "^(roundtrip|parity|ksim_server_(lib|squad|give_item|duel_armour|scenario_file|native_default)|ksim_simlink_sample|ksim_fight_synthetic)$" OR testFixtures MATCHES "knav_" OR testSetups MATCHES "knav_")
			set_tests_properties(${test} PROPERTIES DISABLED TRUE)
		endif()
	endforeach()
endif()
# Navmesh tests off while the navmesh rework lands (user 2026-11-23, docs/INTENT.md): the tests of the mesh itself and of walking it are disabled
# ("Not Run (Disabled)", not a failure) until the new system works and the user has judged it live against the collision overlay. The four fixture
# bakes stay on (other tests read their files), and so do the new generators' own tests, knav_ground and knav_surfgen (ksim 2026-11-25: they
# test the new system, not the old mesh). -DKSIM_NAV_TESTS=ON runs them for diagnosis.
option(KSIM_NAV_TESTS "run the navmesh tests (off while the navmesh rework lands, user 2026-11-23)" OFF)
if(NOT KSIM_NAV_TESTS)
	foreach(test IN LISTS allTests)
		if(test MATCHES "^(knav_|ksim_nav_|ksim_native_|ksim_door_(sweep|plan)$|ksim_foyer_route$|ksim_storm_door$|ksim_route_|ksim_path_walk$|ksim_walk_kinematics$|ksim_stair_|ksim_move_(detour|no_route)$|ksim_offmesh_travel$|ksim_siv_|ksim_stand_building$)"
			AND NOT test MATCHES "^knav_(bake_(hub|hub_single|stack|world_sample)|ground|ground_features|ground_objects|ground_buildings|surfgen|cutover|longhouse_door|hub|native_tiles|door)$")
			set_tests_properties(${test} PROPERTIES DISABLED TRUE)
		endif()
	endforeach()
	# Behaviour tests that walk the mesh and went red on the reworked mesh (CI 61da2d72; bisected to cc2eab22 / c6df7cb9 /
	# the 1bfa3cbc chain), fenced with the rest so the red stop does not hold unrelated work while the rework is on hold
	# behind kenshi-ffmi.1. Owners: kenshi-c2w6, kenshi-eq2y, kenshi-bdyl; they come back on at the surfgen landing
	# (kenshi-k16.7.182), and each is fixed or bisected again then.
	foreach(test IN ITEMS ksim_straight_walk ksim_home_sit ksim_aggro_drop ksim_height_sweep ksim_town_patrol ksim_slave_goals
		ksim_arrest ksim_arrest_vs_capture_b ksim_arrest_vs_capture_f ksim_arrest_vs_capture_g ksim_arrest_auto ksim_path_offroute
		ksim_far_move ksim_perception_cheese ksim_fight_prop  ksim_interact ksim_chase_scen ksim_caravan_idle
		ksim_counter_path ksim_melee_table ksim_bar_fight)
		if(TEST ${test})
			set_tests_properties(${test} PROPERTIES DISABLED TRUE)
		endif()
	endforeach()
endif()


# World tests run on the runtime mesh (the shipped navtiles converted at load, the mesh the server and the game use; tests/nav_source.hpp),
# with the nav cache in the build tree: the stale check and the rebakes are paid once per build, not per test (kenshi-k16.6.252).
# Kept on the world bake (KSIM_NAVWORLD_BAKE=1), because they need the whole world at once or test the bake itself:
#  - ksim_nav_regions: the bake's streaming region coverage (knav_graph / NavMesh over all 256 region files);
#  - knav_interior_walk, ksim_route_soak: walk the bake's interiors / its world_sample, not the shipped tiles;

#  - the soak (tools/ksim_soak, RUNBOOK): 256 regions and the AI's knav_graph .kgraph come from the world bake; it is no ctest.
# (The knav_* tests that take no KSIM_NAVWORLD bake their own small fixtures and never open a world navigator.)
list(APPEND KSIM_BAKE_TESTS ksim_nav_regions knav_interior_walk ksim_route_soak)
foreach(bake_test IN LISTS KSIM_BAKE_TESTS)
	if(TEST ${bake_test})
		set_property(TEST ${bake_test} APPEND PROPERTY ENVIRONMENT KSIM_NAVWORLD_BAKE=1)
	endif()
endforeach()
# The scenario goldens again at the production arena strides (fight 1 / idle 2; kenshi-k16.6.658): the same executables with KSIM_ARENA_STRIDES=1/2,
# which the dev-only CombatRules hook reads (src/ksim/dev/arena_strides.cpp). Defaults stay 1/1 everywhere else. The long scenarios (town beds / bar /
# scorer, slaver route, ksim_fight_1v1) pass at 1/2 too but stay out of this set for time.
if(KSIM_DEV)
	function(ksim_stride_test name)
		add_test(NAME ${name}_s12 COMMAND ${name} ${ARGN})
		set_tests_properties(${name}_s12 PROPERTIES ENVIRONMENT KSIM_ARENA_STRIDES=1/2 SKIP_RETURN_CODE 77)
	endfunction()
	foreach(stride_base ksim_raid_phases ksim_raid_occupy ksim_bar_brawl ksim_bar_thugs ksim_fight_trace)
		ksim_stride_test(${stride_base} ${KSIM_NAVWORLD})
	endforeach()
	add_test(NAME ksim_bar_brawl_thug_s12 COMMAND ksim_bar_brawl ${KSIM_NAVWORLD} Vagrants)
	set_tests_properties(ksim_bar_brawl_thug_s12 PROPERTIES ENVIRONMENT KSIM_ARENA_STRIDES=1/2 SKIP_RETURN_CODE 77)
	ksim_stride_test(ksim_town_guards ${CMAKE_CURRENT_BINARY_DIR}/hub.knav)
	set_tests_properties(ksim_town_guards_s12 PROPERTIES FIXTURES_REQUIRED knav_cache)
	foreach(stride_base ksim_town_fighters ksim_slavers_trading ksim_crowd_penalty)
		ksim_stride_test(${stride_base})
	endforeach()
endif()
if(NOT KSIM_NAV_TESTS)
	# Whole-world navmesh landing (user 2026-11-25: "DISABLE ANY TESTS that goes anywhere near this. Off screen combat, any combat, anything! DO NOT
	# GET BOGGED DOWN with 6 hours of testing before you merge a single line"; kenshi-k16.7.204): every test that reads a navmesh bake fixture, and
	# every combat / movement / squad / off-screen test by name, is off too. The generator tests and the fixture bakes above stay on. Back on
	# test by test after the whole-world mesh works (kenshi-k16.15.76 judges the reds then).
	# It runs after every test is defined (the _s12 stride variants come after the fences above; CI df7ca1c4 still ran them, kenshi-3thz).
	# By name too: the CI df7ca1c4 reds that walk, route or load the world without a matching name (kenshi-3thz, kenshi-edo3, kenshi-by2t).
	set(navFenced 0)
	get_property(fenceTests DIRECTORY PROPERTY TESTS)
	foreach(test IN LISTS fenceTests)
		get_test_property(${test} FIXTURES_REQUIRED testFixtures)
		get_test_property(${test} DISABLED testDisabled)
		if(testDisabled OR test MATCHES "^knav_(bake_(hub|hub_single|stack|world_sample)|ground|ground_features|ground_objects|ground_buildings|surfgen|cutover|longhouse_door|native_tiles|door)$")
			continue()
		endif()
		if(testFixtures MATCHES "knav_" OR test MATCHES "^(knav_hub|ksim_(bw_orders|empty_machine|gate_crew|medic|self_aid|mine_node_cut|newgame_settled|reload_hygiene|road_exe_rule|road_lookahead|server_import|server_native_default|simlink|world_hub|cheat_slavers|shackle_orders))$" OR test MATCHES "(fight|combat|brawl|melee|duel|chase|aggro|attack|raid|army|squad|patrol|offscreen|off_screen|wild|hunt|caravan|travel|walk|route|move|follow|crowd|scen|town|guard|arrest|capture|slave|cage|carry|bar_|door|stair|nav|interact|path|stride|detail|token|promot|spawn|resident|home|bed|sleep)")
			set_tests_properties(${test} PROPERTIES DISABLED TRUE)
			math(EXPR navFenced "${navFenced} + 1")
		endif()
	endforeach()
	# Every reader of the world_sample fixture is fenced above, and its bake runs past the 1200 s cap since the bakeZone cutover
	# (kenshi-i6u4; CI f974f052 / 3bc7b3e7 spent 40 min per run on it, rerun included): off with its readers.
	set_tests_properties(knav_bake_world_sample PROPERTIES DISABLED TRUE)
	message(STATUS "navmesh landing fence: ${navFenced} more tests disabled (KSIM_NAV_TESTS=OFF)")
endif()
# A read-only shared nav cache root (kenshi-k16.7.190): empty = none. Only the dev bake job writes it; set it in a build dir's config.
set(KSIM_NAV_CACHE_SHARED "" CACHE PATH "Read-only shared nav cache root (read through before baking)")
get_property(all_tests DIRECTORY PROPERTY TESTS)
foreach(any_test IN LISTS all_tests)
	if(KSIM_NAV_CACHE_SHARED)
		set_property(TEST ${any_test} APPEND PROPERTY ENVIRONMENT KENSHI_NAV_CACHE_SHARED=${KSIM_NAV_CACHE_SHARED})
	endif()
	set_property(TEST ${any_test} APPEND PROPERTY ENVIRONMENT KENSHI_NAV_CACHE=${CMAKE_BINARY_DIR}/navcache)
endforeach()

# Launcher-run workers do not run tests; the test queue does (user 2026-11-17; tests/worker_guard.cmake). Every test requires the guard,
# so `ctest -R x` runs it first and a refusal leaves the rest "Not Run".
add_test(NAME ksim_worker_guard COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_guard.cmake)
set_tests_properties(ksim_worker_guard PROPERTIES FIXTURES_SETUP worker_guard)
foreach(any_test IN LISTS all_tests)
	set_property(TEST ${any_test} APPEND PROPERTY FIXTURES_REQUIRED worker_guard)
endforeach()
# Worker test lane (user 2026-11-19, tools/wtest.sh): every test start waits while a worker's run holds the worker test lock.
# Dev checkouts only (cmake/dev_paths.cmake present): the launcher and its dev host state dir are absent from a release (kenshi-379.38).
if(NOT WIN32 AND KSIM_DEV_PATHS)
	foreach(any_test IN LISTS all_tests ITEMS ksim_worker_guard)
		set_property(TEST ${any_test} PROPERTY TEST_LAUNCHER ${CMAKE_CURRENT_SOURCE_DIR}/tests/yield_to_worker.sh)
	endforeach()
endif()

# The 15 s test cap (kenshi-k16.15.55, user 2026-11-23: "a test should be forbidden if it takes 15 seconds or more"; docs/INTENT.md). Every
# test runs under tools/test_cpu_wrap.py (cmake/test_cap.cmake), which appends its CPU seconds (user+sys of the process tree: load-independent) to test_cpu.tsv in
# the build dir; ksim_test_cap_check runs after every other test of the run (DEPENDS) and fails on a test over 15 CPU-s with no line in
# tests/test_cap_exceptions.tsv, or an exception naming no ctest (a listed test that runs fast, e.g. warm nav cache, is only a note). So wtest, the queue and CI all apply it. docs/RUNBOOK.md "Test cap".
if(KSIM_TEST_CAP)
	get_property(cap_tests DIRECTORY PROPERTY TESTS)
	string(REPLACE ";" "\n" cap_names_text "${cap_tests};ksim_test_cap_check;ksim_test_cap_selftest")
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/test_cap_names.txt "${cap_names_text}\n")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cap_exceptions.tsv)
	add_test(NAME ksim_test_cap_check COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_cap_check.py
		--log ${KSIM_TEST_CPU_LOG} --exceptions ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cap_exceptions.tsv --names ${CMAKE_CURRENT_BINARY_DIR}/test_cap_names.txt)
	set_property(TEST ksim_test_cap_check PROPERTY DEPENDS ${cap_tests})
	set_property(TEST ksim_test_cap_check PROPERTY FIXTURES_REQUIRED worker_guard)
	set_property(TEST ksim_test_cap_check APPEND PROPERTY LABELS infra)
	add_test(NAME ksim_test_cap_selftest COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_cap_check.py --selftest)
	set_property(TEST ksim_test_cap_selftest PROPERTY FIXTURES_REQUIRED worker_guard)
	set_property(TEST ksim_test_cap_selftest APPEND PROPERTY LABELS infra)
else()
	list(APPEND KSIM_PLATFORM_ABSENT_TESTS ksim_test_cap_check ksim_test_cap_selftest) # their validity rows stay valid with the cap off (Windows CI, kenshi-k16.6.829)
endif()

# Validity labels (kenshi-k16.15.26): golden / pin / infra (+ unverified, wrong) per test, generated from cpp/tests/validity/*.tsv, the one home
# of the classification; a test with no rows is 'unclassified'. Appended, so `slow` below and any other label stay. docs/RUNBOOK.md "Test labels".

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
	message(WARNING "validity labels skipped: no Python3")
else()
	file(GLOB KSIM_VALIDITY_TSVS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/validity/*.tsv)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KSIM_VALIDITY_TSVS})
	set(KSIM_VALIDITY_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../tools/validity_labels.py)
	execute_process(COMMAND ${Python3_EXECUTABLE} -I ${KSIM_VALIDITY_SCRIPT} --out ${CMAKE_CURRENT_BINARY_DIR}/validity_labels.cmake RESULT_VARIABLE validity_rc)
	if(NOT validity_rc EQUAL 0)
		message(FATAL_ERROR "tools/validity_labels.py failed (a malformed row in cpp/tests/validity/*.tsv?)")
	endif()
	include(${CMAKE_CURRENT_BINARY_DIR}/validity_labels.cmake)
	# The check test: a row naming a missing ctest, or a ctest with no rows, fails. The names are this tree's own test list.
	get_property(validity_names DIRECTORY PROPERTY TESTS)
	list(APPEND validity_names ksim_validity_check)
	string(REPLACE ";" "\n" validity_names_text "${validity_names}")
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/validity_ctest_names.txt "${validity_names_text}\n")
	# Tests this platform does not build (the ASan twins on MinGW): their rows stay valid (kenshi-k16.15.38, Windows CI round 8).
	string(REPLACE ";" "\n" validity_absent_text "${KSIM_PLATFORM_ABSENT_TESTS}")
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/validity_ctest_absent.txt "${validity_absent_text}\n")
	add_test(NAME ksim_validity_check COMMAND ${Python3_EXECUTABLE} -I ${KSIM_VALIDITY_SCRIPT} --check --names ${CMAKE_CURRENT_BINARY_DIR}/validity_ctest_names.txt
		--absent ${CMAKE_CURRENT_BINARY_DIR}/validity_ctest_absent.txt)
	set_property(TEST ksim_validity_check PROPERTY FIXTURES_REQUIRED worker_guard)
	set_property(TEST ksim_validity_check APPEND PROPERTY LABELS infra)
endif()

# Last in the file: a test defined after this loop never got its slow label (kenshi-cotr, and the k16.6.253 tests).
# The smoke set (kenshi-k16.15.51, user 2026-11-22: "258?! Oh yeah, REAL QUICK. :p no wonder the test queue is collapsing under its
# own weight."): what every test-queue job runs before its named tests, about 300 s of test time. Every test under 1 s, then one or
# two per system: startup and wire, save / load, determinism, nav, combat, AI goals, town, world, economy, dialogue, medical, items,
# ranged, spawn. The whole suite stays with CI on every ksim and master push. Choosing a test here is a cost call, not a coverage one:
# what CI catches that this set misses gets an item.
set(KSIM_SMOKE_TESTS
	# under 1 s each
	fields kdatadir  ksim_blackboard ksim_blood ksim_brawl_freeze ksim_clock ksim_door_plan ksim_goal_select
	ksim_goap_plan ksim_locks2  ksim_profile ksim_ranged_plan ksim_script ksim_sensor_bits
	ksim_server_import_fixture ksim_server_wire_version ksim_skin_harvest ksim_small_tasks 
	ksim_talk_approach ksim_worker_guard kutil_crashlog kworld_height_live kworld_loadorder kworld_terrain overlay versions
	knav_deterministic knav_walk ksim_hub_stair_seam ksim_regions_areas ksim_validity_check ksim_test_cap_check kworld_features_malformed
	kworld_malformed malformed ksim_regions_perception ksim_regions_squads ksim_regions_target kworld_malformed_asan
	kworld_skeleton kworld_pathing ksim_regions_ownership ksim_regions_perf roundtrip ksim_regions_migration malformed_asan
	kworld_features_malformed_asan
	# one or two per system
	ksim_server_lib ksim_server_scenario_file ksim_simlink_sample
	ksim_save ksim_path_corridor_save
	ksim_determinism ksim_regions_determinism
	knav_route_shared ksim_route_walk knav_door ksim_nav_cache_canary
	ksim_melee_table ksim_combat_armed ksim_ko_fate
	ksim_town_goals ksim_package_leads ksim_schedule
	ksim_hub_residents
	kworld_zones kworld_towns ksim_world
	ksim_economy_price
	ksim_dialogue_book
	ksim_medical
	ksim_item_stats
	ksim_ranged_fire
	ksim_spawn
)
foreach(smoke_test IN LISTS KSIM_SMOKE_TESTS)
	if(TEST ${smoke_test})
		set_property(TEST ${smoke_test} APPEND PROPERTY LABELS smoke)
	else()
		message(WARNING "KSIM_SMOKE_TESTS names ${smoke_test}, which is not a test")
	endif()
endforeach()

foreach(slow_test IN LISTS KSIM_SLOW_TESTS)
	if(TEST ${slow_test})
		set_property(TEST ${slow_test} APPEND PROPERTY LABELS slow)
	else()
		message(WARNING "KSIM_SLOW_TESTS names ${slow_test}, which is not a test")
	endif()
endforeach()


# Tests that read the checkout paths (ksim/dev/build_paths.hpp) without linking ksim (kenshi-k16.15.18).
foreach(t kdata_parity kworld_terrain kworld_zones kworld_skeleton kworld_foliage_live kworld_foliage_orient kworld_height_live kworld_pathing kworld_placement kworld_towns)
	if(TARGET ${t})
		target_link_libraries(${t} PRIVATE ksim_paths)
	endif()
endforeach()

# Startup benchmark (kenshi-k16.15.44): per-phase wall time to the first tick. Dev-only, off by default; not a ctest.
option(KSIM_STARTUP_BENCH "Build ksim_startup_bench (dev-only startup timing)" OFF)
if(KSIM_STARTUP_BENCH AND KSIM_DEV)
	add_executable(ksim_startup_bench tests/ksim_startup_bench.cpp)
	target_link_libraries(ksim_startup_bench PRIVATE ksim)
	target_compile_definitions(ksim_startup_bench PRIVATE KENSHI_DATA="${KENSHI_DATA}" KSIM_AI_TABLE="${KSIM_AI_TABLE}")
	kdata_warnings(ksim_startup_bench)
endif()
