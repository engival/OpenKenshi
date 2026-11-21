cmake_minimum_required(VERSION 3.20)
project(kclient LANGUAGES CXX)
if(APPLE)
	# sokol_app and the Metal backend are Objective-C: src/platform/macos/sokol_impl.mm (kenshi-379.32.1)
	enable_language(OBJCXX)
	set(CMAKE_OBJCXX_STANDARD 20)
	set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
	string(APPEND CMAKE_OBJCXX_FLAGS " -fobjc-arc")
endif()

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
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -g")	# symbols for the crash report's backtrace (cpp/include/kutil/crashlog.hpp); no runtime cost
# A shipped binary must not carry the developer's checkout path (kenshi-379.13; `strings` finds it in DWARF otherwise): record the tree as "."
# in the debug info. Backtraces keep working, with relative source paths.
get_filename_component(KENSHI_TREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
string(APPEND CMAKE_CXX_FLAGS " -ffile-prefix-map=${KENSHI_TREE_ROOT}=.")
string(APPEND CMAKE_C_FLAGS " -ffile-prefix-map=${KENSHI_TREE_ROOT}=.")	# the C sources too (flecs)

# ThreadSanitizer (kenshi-k16.2.205.12.29; docs/RENDER_THREAD.md 7.1 rule 9): races between the render thread and the logic thread
# (render.thread=lockstep), every target and the third-party trees instrumented. Its own build dir, built -j4 and nice: client/tests/tsan/run.sh
# configures, builds and runs it. Never a shipped binary: keep it out of client/build.
option(KCLIENT_TSAN "Build with ThreadSanitizer (-fsanitize=thread); client/tests/tsan/run.sh" OFF)
if(KCLIENT_TSAN)
	add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
	add_link_options(-fsanitize=thread)
	message(STATUS "KCLIENT_TSAN: ThreadSanitizer build (reports go to stderr or TSAN_OPTIONS' log_path)")
endif()

# macOS stops at OpenGL 4.1 and the shaders are glsl430: Metal is the default there (kenshi-379.32.1); Linux keeps glcore.
if(APPLE)
	set(KENSHI_GFX_DEFAULT "metal")
else()
	set(KENSHI_GFX_DEFAULT "glcore")
endif()
set(KENSHI_GFX "${KENSHI_GFX_DEFAULT}" CACHE STRING "sokol backend: glcore | vulkan | metal (metal: macOS only)")
set_property(CACHE KENSHI_GFX PROPERTY STRINGS glcore vulkan metal)
if(CMAKE_HOST_WIN32)
	set(KENSHI_SHDC_EXE ".exe")	# get_shdc.sh fetches the Windows build as sokol-shdc.exe (kenshi-379.31.1)
endif()
set(KENSHI_SHDC "${CMAKE_CURRENT_SOURCE_DIR}/bin/sokol-shdc${KENSHI_SHDC_EXE}" CACHE FILEPATH "sokol-shdc binary (tools/get_shdc.sh)")
if(NOT EXISTS "${KENSHI_SHDC}")
	message(FATAL_ERROR "sokol-shdc not found at ${KENSHI_SHDC}; run client/tools/get_shdc.sh")
endif()

if(KENSHI_GFX STREQUAL "glcore")
	set(GFX_DEFINE SOKOL_GLCORE)
	set(SHDC_SLANG glsl430:spirv_vk)
elseif(KENSHI_GFX STREQUAL "vulkan")
	set(GFX_DEFINE SOKOL_VULKAN)
	set(SHDC_SLANG glsl430:spirv_vk)
elseif(KENSHI_GFX STREQUAL "metal")
	if(NOT APPLE)
		message(FATAL_ERROR "KENSHI_GFX=metal needs macOS")
	endif()
	set(GFX_DEFINE SOKOL_METAL)
	set(SHDC_SLANG metal_macos)
else()
	message(FATAL_ERROR "KENSHI_GFX must be glcore, vulkan or metal")
endif()

# Shaders: shaders/<name>.glsl -> ${build}/shaders/<name>.glsl.h
file(GLOB SHADER_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl")
file(GLOB SHADER_INCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.inc")
set(SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(SHADER_HDRS)
foreach(src ${SHADER_SRCS})
	get_filename_component(name ${src} NAME)
	set(hdr "${SHADER_DIR}/${name}.h")
	add_custom_command(
		OUTPUT ${hdr}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
		COMMAND ${KENSHI_SHDC} -i ${src} -o ${hdr} --slang ${SHDC_SLANG}
		DEPENDS ${src} ${SHADER_INCS}
		COMMENT "sokol-shdc ${name}"
		VERBATIM)
	list(APPEND SHADER_HDRS ${hdr})
endforeach()
add_custom_target(kclient_shaders DEPENDS ${SHADER_HDRS})

# Windows (kenshi-379.31.1): M_PI and friends are hidden by the strict -std=c++20 unless _USE_MATH_DEFINES (MinGW and MSVC alike); windows.h's
# min / max macros would break std::min / std::max. Directory-wide, before cpp/ is added, so the sim's sources in this build get them too.
if(WIN32)
	add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
endif()
# MinGW: link the GCC runtime (libstdc++, libgcc, winpthread) and the pkg-config libraries (vorbis, ogg) statically, so OpenKenshi.exe runs
# beside its simdata/ with only Windows' own DLLs (the CI artifact the user runs under Proton, kenshi-379.31.1).
if(MINGW)
	add_link_options(-static)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
if(UNIX AND NOT APPLE)
	pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xi xcursor xrandr)
endif()

# Dev-only code (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153): the playtest store and its host, the bd writer behind --bead,
# scenario loading, and the dev tool executables below. ON by default; OFF builds the game without any of it (client/tools/dev_off_check.sh).
# The game reaches it through src/dev/devhooks.hpp, whose hooks are inline no-ops when OFF. Declared here, before the cpp/ subdirectory, so the
# server's built-in data path can follow it.
option(KCLIENT_DEV_TOOLS "Build the dev-only tools (src/dev/, the dev tool executables)" ON)

# kworld/kdata (town placements); the cpp/ tree is owned elsewhere, only built as a dependency.
# A release binary carries no developer path (kenshi-379.13): the game data is resolved at runtime, so the cpp/ server's built-in KENSHI_DATA
# fallback follows the dev tools. Not a user option here: it is exactly this build's fence.
set(KSIM_BAKE_DATA_PATH ${KCLIENT_DEV_TOOLS} CACHE BOOL "Compile KENSHI_DATA into runServer's fallback (follows KCLIENT_DEV_TOOLS)" FORCE)
# Shared kenshi ccache: the one copy of the logic lives in cpp/cmake/kenshi_ccache.cmake (kenshi-okpn.3); hosts without its ccache.conf see no change.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cpp/cmake/kenshi_ccache.cmake)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../cpp ${CMAKE_CURRENT_BINARY_DIR}/cpp EXCLUDE_FROM_ALL)

# Jolt Physics v5.6.0 (third_party/JoltPhysics, MIT): client ragdolls only (src/characters/physics.cpp, docs/reference/PHYSICS.md 2). The vendored tree is the
# library alone (no samples, tests, viewer or debug renderer). JPH_DOUBLE_PRECISION: the world spans +-147456 u (kworld::kWorldMin / kWorldExtent),
# see the measurement in src/characters/physics.cpp. Jolt.cmake makes its defines and instruction-set flags PUBLIC (a mismatch is an ABI break it detects).
set(PHYSICS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JoltPhysics")
set(DOUBLE_PRECISION ON)
include("${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake")
# No debug renderer: JPH_DEBUG_RENDERER stays off and its four sources are dropped (Jolt.cmake lists them unconditionally; without the define they
# would be empty objects).
foreach(f DebugRenderer.cpp DebugRendererPlayback.cpp DebugRendererRecorder.cpp DebugRendererSimple.cpp)
	set_source_files_properties("${PHYSICS_REPO_ROOT}/Jolt/Renderer/${f}" PROPERTIES HEADER_FILE_ONLY TRUE)
endforeach()
set(CMAKE_CXX_STANDARD 20)	# Jolt.cmake sets its own directory to 17

# Detour for the client navmesh (clientnav.cpp); imported targets found in cpp/ are not visible here
include(${CMAKE_CURRENT_SOURCE_DIR}/../cpp/cmake/recast.cmake)
# Recast builds as C++98, where MinGW's <stdint.h> hides INTPTR_MAX (RecastAlloc.h's RC_SIZE_MAX) unless __STDC_LIMIT_MACROS. Its home is the fork
# pin or cpp/cmake/recast.cmake (kenshi-379.31.1's report lists it for ksim); until then the client build sets it on the fetched target.
if(MINGW AND TARGET Recast)
	target_compile_definitions(Recast PRIVATE __STDC_LIMIT_MACROS)
endif()

# The one sokol implementation unit: Objective-C++ on macOS. The window geometry memory: X11 on Linux, Cocoa on macOS (the macOS tester's
# windowgeom_macos.mm, kenshi-379.32.13), Win32 on Windows (kenshi-379.31.1).
if(APPLE)
	set(KCLIENT_SOKOL_IMPL src/platform/macos/sokol_impl.mm)
	set(KCLIENT_WINDOWGEOM src/platform/macos/windowgeom.mm)
elseif(WIN32)
	set(KCLIENT_SOKOL_IMPL src/platform/sokol_impl.cpp)
	set(KCLIENT_WINDOWGEOM src/platform/windows/windowgeom.cpp)
else()
	set(KCLIENT_SOKOL_IMPL src/platform/sokol_impl.cpp)
	set(KCLIENT_WINDOWGEOM src/platform/linux/windowgeom.cpp)
endif()
# KCLIENT_PLATFORM: the rest of the OS layer (src/platform/*.hpp): the local server child (POSIX on Linux and macOS,
# Winsock / CreateProcess on Windows), the per-user dirs (app/userdirs.hpp, one file per OS) and, on Windows, the GL loader (gl.hpp).
if(WIN32)
	set(KCLIENT_PLATFORM src/platform/windows/process.cpp src/platform/windows/userdirs.cpp src/platform/windows/console.cpp
		src/platform/windows/gl.cpp src/platform/swapinfo_stub.cpp)	# gl.cpp: the GL entry points past 1.1 (platform/gl.hpp)
	# sokol_app / sokol_gfx (GL) / sokol_audio (WASAPI) system libraries: MSVC takes them from the headers' #pragma comment, MinGW needs them
	# listed (sokol_app.h "with MINGW/MSYS2 gcc"); ws2_32: the simlink socket; advapi32: the registry.
	set(KCLIENT_WIN32_LIBS kernel32 user32 shell32 gdi32 ole32 advapi32 ws2_32)
elseif(APPLE)
	set(KCLIENT_PLATFORM src/platform/posix/process.cpp src/platform/posix/console.cpp src/platform/macos/userdirs.cpp src/platform/swapinfo_stub.cpp)
	set(KCLIENT_WIN32_LIBS)
else()
	set(KCLIENT_PLATFORM src/platform/posix/process.cpp src/platform/posix/console.cpp src/platform/linux/userdirs.cpp src/platform/linux/swapinfo.cpp)
	set(KCLIENT_WIN32_LIBS)
endif()

# The client is one STATIC library per src/ directory (kenshi-k16.2.369.15.1; docs/CLIENT.md "Code map"): kc_<dir>, plus kui (src/ui) and kaudio
# (src/audio). Each declares the libraries below it and nothing else, so a layering break fails to link, and tools/layer_check.py (run by every
# build) fails an #include that is not a declared edge. Layers, low to high (a library links only the ones above it in this list):
#   kc_app (config, eventlog, workpool, logicthread ...)   kc_knobs (dev knobs read low: ablate, lightterms)   kc_net (the SIMLINK wire types and decode)
#   kc_platform (sokol, OS socket / process / user dirs, windowgeom, datadir)   kc_assets (mods, ogre meshes, materials, dds, stb)
#   kc_render (render core: passes, draw lists, GPU resources)   kc_world (buildings, foliage, grass, nav, weather instances)   kaudio   kui
#   kc_characters   kc_bridge (SimView: the wire -> Characters bridge; ragdoll driver)   kc_input   kc_frame (render_frame, trails, load screen)
#   kc_debug   kc_dev   kc_host (host/ + main.cpp and its per-target files)
# Add a library with kc_layer(name DIR dir SOURCES ... DEPS ...); list the directory in tools/layer_check.py's view through DIR.

# Python for the generators and the layering check.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# The sim's DEBUG VIEW (docs/formats/SIMLINK.md "DEBUG VIEW"): src/debug/. ON by default in every build type (a small decoder; the data flows
# only from a server built with KSIM_DEBUG_VIEW and run with --debug-view)
option(KCLIENT_DEBUG_VIEW "Build the sim debug view (src/debug/)" ON)
# Profiling (kenshi-k16.2.101): the GPU pass timers (src/gputimer) and the per-pass submit counters (src/drawstats), reported per scene in the
# scene test JSON. OFF compiles all of it out: GPU_BEGIN/GPU_END are empty statements, the draw wrappers are plain pass-throughs and the JSON
# has none of the profile fields.
option(KCLIENT_PROFILE "GPU pass timers and per-pass submit counters (scene test JSON)" ON)

# What every layer shares: defines, include roots, the system libraries, the cpp/ libraries.
add_library(kclient_base INTERFACE)
target_include_directories(kclient_base INTERFACE src ${SHADER_DIR})
target_include_directories(kclient_base SYSTEM INTERFACE third_party third_party/sokol)
target_compile_definitions(kclient_base INTERFACE ${GFX_DEFINE} KCLIENT_PROFILE=$<BOOL:${KCLIENT_PROFILE}>)
if(KCLIENT_DEBUG_VIEW)
	target_compile_definitions(kclient_base INTERFACE KCLIENT_DEBUG_VIEW)
endif()
if(KCLIENT_DEV_TOOLS)
	# The build's own data dir as datasetting.cpp's extra search place: a dev build only (kenshi-379.13). A release resolves the game data at runtime,
	# and a shipped binary must not carry the developer's home path (client/tools/dev_off_check.sh).
	target_compile_definitions(kclient_base INTERFACE KCLIENT_DEV_TOOLS KENSHI_DATA="${KENSHI_DATA}")
endif()
target_compile_options(kclient_base INTERFACE -Wall -Wextra)
target_link_libraries(kclient_base INTERFACE kworld knav kdata kutil RecastNavigation::Detour Jolt Threads::Threads ${CMAKE_DL_LIBS} m ${KCLIENT_WIN32_LIBS})
if(WIN32)
	target_link_libraries(kclient_base INTERFACE comdlg32 ole32)	# comdlg32: tinyfiledialogs' native folder dialog
endif()
if(UNIX AND NOT APPLE)
	target_link_libraries(kclient_base INTERFACE PkgConfig::X11)
endif()
if(KENSHI_GFX STREQUAL "glcore")
	find_package(OpenGL REQUIRED)
	target_link_libraries(kclient_base INTERFACE OpenGL::GL)
elseif(KENSHI_GFX STREQUAL "metal")
	target_link_libraries(kclient_base INTERFACE "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework MetalKit")
else()
	find_package(Vulkan REQUIRED)
	target_compile_definitions(kclient_base INTERFACE VK_USE_PLATFORM_XLIB_KHR)
	target_link_libraries(kclient_base INTERFACE Vulkan::Vulkan)
endif()

# The layering check (tools/layer_check.py): every cross-directory #include of src/ must be a declared edge of kc_layer / kc_edges. The edges are
# written to ${CMAKE_CURRENT_BINARY_DIR}/layers.txt at the end of configure; the check runs before any layer builds.
add_custom_target(kclient_layers
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/layer_check.py ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/layers.txt
	COMMENT "layering check" VERBATIM)
set_property(GLOBAL PROPERTY KC_LAYER_ROWS "")
# kc_edges(lib DIR dir DEPS libs...): records a library's directory and its declared dependencies for the check.
function(kc_edges lib)
	cmake_parse_arguments(E "" "DIR" "DEPS" ${ARGN})
	string(REPLACE ";" "," deps "${E_DEPS}")
	set_property(GLOBAL APPEND PROPERTY KC_LAYER_ROWS "${E_DIR}:${lib}:${deps}")
endfunction()
# kc_layer(lib DIR dir SOURCES ... DEPS libs...): a STATIC library of the sources, or an INTERFACE one (headers only) when there are none.
function(kc_layer lib)
	cmake_parse_arguments(L "" "DIR" "SOURCES;DEPS" ${ARGN})
	if(L_SOURCES)
		add_library(${lib} STATIC ${L_SOURCES})
		add_dependencies(${lib} kclient_shaders kclient_layers)
		target_link_libraries(${lib} PUBLIC kclient_base ${L_DEPS})
	else()
		add_library(${lib} INTERFACE)
		target_link_libraries(${lib} INTERFACE kclient_base ${L_DEPS})
	endif()
	kc_edges(${lib} DIR ${L_DIR} DEPS ${L_DEPS})
endfunction()

set(KC_SRC_APP
	src/app/config.cpp
	src/app/datapicker.cpp
	src/app/eventlog.cpp
	src/app/logicthread.cpp
	src/app/sappcmd.cpp
	src/app/serverrows.cpp
	src/app/workpool.cpp
	third_party/tinyfiledialogs/tinyfiledialogs.c
)
set(KC_SRC_ASSETS
	src/assets/dds.cpp
	src/assets/imagewrite.cpp
	src/assets/materials.cpp
	src/assets/mods.cpp
	src/assets/ogremesh.cpp
	src/assets/resources.cpp
	src/assets/stb_impl.cpp
)
set(KC_SRC_AUDIO
	src/audio/audiohook.cpp
)
set(KC_SRC_BRIDGE
	src/bridge/ragdolldrive.cpp
	src/bridge/simview.cpp
)
set(KC_SRC_CHARACTERS
	src/characters/characters.cpp
	src/characters/charanim.cpp
	src/characters/chardraw.cpp
	src/characters/charload.cpp
	src/characters/creator.cpp
	src/characters/creatorscene.cpp
	src/characters/physics.cpp
	src/characters/portraits.cpp
	src/characters/ragdoll.cpp
)
set(KC_SRC_FRAME
	src/frame/beamfield.cpp
	src/frame/lastframe.cpp
	src/frame/loadscreen.cpp
	src/frame/renderframe.cpp
	src/frame/trails.cpp
)
set(KC_SRC_HOST
	src/host/orders.cpp
	src/host/panels.cpp
	src/host/pick.cpp
	src/host/script.cpp
	src/host/session.cpp
	src/host/uihost.cpp
)
set(KC_SRC_INPUT
	src/input/collisionpick.cpp
	src/input/controlhints.cpp
	src/input/cursors.cpp
	src/input/pick.cpp
)
set(KC_SRC_NET
	src/net/simlink.cpp
)
set(KC_SRC_PLATFORM
	${KCLIENT_PLATFORM}
	${KCLIENT_SOKOL_IMPL}
	${KCLIENT_WINDOWGEOM}
	src/platform/datasetting.cpp
	src/platform/windowgate.cpp
)
set(KC_SRC_RENDER
	src/render/ao.cpp
	src/render/arrows.cpp
	src/render/blooddecals.cpp
	src/render/camera.cpp
	src/render/colourmap.cpp
	src/render/detail.cpp
	src/render/drawlist_gpu.cpp
	src/render/dust.cpp
	src/render/fogvolume.cpp
	src/render/gputimer.cpp
	src/render/hdr.cpp
	src/render/idbuffer.cpp
	src/render/lightbake.cpp
	src/render/lightvol.cpp
	src/render/navoverlay.cpp
	src/render/openwindows.cpp
	src/render/overlaylist.cpp
	src/render/particles.cpp
	src/render/post.cpp
	src/render/readback.cpp
	src/render/reflection.cpp
	src/render/renderqueue.cpp
	src/render/shadow.cpp
	src/render/sky.cpp
	src/render/smaa_lut.cpp
	src/render/speccube.cpp
	src/render/sunshafts.cpp
	src/render/terrain.cpp
	src/render/water.cpp
	src/render/weathermap.cpp
)
set(KC_SRC_WORLD
	src/world/buildings.cpp
	src/world/clientnav.cpp
	src/world/floorfollow.cpp
	src/world/foliage.cpp
	src/world/grass.cpp
	src/world/particleworld.cpp
	src/world/plants.cpp
	src/world/squads.cpp
	src/world/towns.cpp
	src/world/wbdraw.cpp
	src/world/wbfloors.cpp
	src/world/wbload.cpp
	src/world/wbpopin.cpp
	src/world/weatherinstances.cpp
	src/world/worldbuildings.cpp
	src/world/worldmap.cpp
)

# The build's git hash: --help shows it and every playtest run records it (src/playtest.cpp, src/app/githash.hpp). tools/githash.cmake writes
# githash.cpp in the build tree on every build, so a commit shows without a reconfigure; the file changes only when the hash does, so an
# unchanged hash rebuilds nothing. "-dirty" counts client/ paths only (the shared tree holds other sessions' work); no git says "unknown".
set(KCLIENT_GITHASH_CPP "${CMAKE_CURRENT_BINARY_DIR}/githash.cpp")
# once at configure: the file must exist before the first compile
execute_process(COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${KCLIENT_GITHASH_CPP}
	-P ${CMAKE_CURRENT_SOURCE_DIR}/tools/githash.cmake RESULT_VARIABLE KCLIENT_GITHASH_RC)
if(NOT KCLIENT_GITHASH_RC EQUAL 0)
	message(WARNING "tools/githash.cmake failed; an earlier githash.cpp is used if present")
endif()
add_custom_target(kclient_githash
	COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${KCLIENT_GITHASH_CPP}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/tools/githash.cmake
	BYPRODUCTS "${KCLIENT_GITHASH_CPP}"
	COMMENT "kclient git hash")

# The AI task names for CHAR_STATS' AI tail (rows 16 / 18; src/net/aitask.hpp, main.cpp): generated from the docs' table at build time, so no
# name is typed in the client and a doc rerun shows without a client edit. Rebuilt only when the doc or the generator changes.
set(KCLIENT_AI_TASK_NAMES_CPP "${CMAKE_CURRENT_BINARY_DIR}/gen/ai_task_names.cpp")
add_custom_command(
	OUTPUT ${KCLIENT_AI_TASK_NAMES_CPP}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_ai_task_names.py
		--doc ${CMAKE_CURRENT_SOURCE_DIR}/../docs/formats/AI_TASK_ENUMS.md --out ${KCLIENT_AI_TASK_NAMES_CPP}
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../docs/formats/AI_TASK_ENUMS.md ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_ai_task_names.py
	COMMENT "generate ai_task_names.cpp"
	VERBATIM)

# tinyfiledialogs is C that compiles as C++ (upstream: "rename this .c file as .cpp"); the project is CXX only. Vendored untouched, so no warnings from it.
set_source_files_properties(third_party/tinyfiledialogs/tinyfiledialogs.c PROPERTIES LANGUAGE CXX COMPILE_OPTIONS -w)
# the SMAA tables must match upstream bit for bit: no fused multiply-adds
set_source_files_properties(src/render/smaa_lut.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)

# Dev-only sources: one source per line, so two branches adding dev tools do not touch the same line (render hook 10)
set(KC_SRC_DEV)
set(KC_SRC_KNOBS)
if(KCLIENT_DEV_TOOLS)
	set(KC_SRC_KNOBS src/knobs/lightterms.cpp)
	set(KC_SRC_DEV
		src/dev/devhost.cpp
		src/dev/debughost.cpp
		src/dev/gaitlog.cpp
		src/dev/poslog.cpp
		src/dev/pacelog.cpp
		src/dev/playtest.cpp
		src/dev/beadpost.cpp
		src/dev/animlab.cpp
		src/dev/charops.cpp
		src/dev/sgowner.cpp
		src/dev/pickboxes.cpp
		src/dev/pickmask.cpp
		src/dev/routepreview.cpp
		src/dev/scenetest.cpp
		src/dev/probe.cpp
		src/dev/heightprobe.cpp
		src/dev/scriptlog.cpp
		src/dev/viewmesh.cpp
		src/dev/hintsop.cpp
		src/dev/packetdigest.cpp
		src/dev/shotsidecars.cpp
		src/dev/colloverlay.cpp
		src/dev/launchguard.cpp
	)
endif()
# The sim's DEBUG VIEW (docs/formats/SIMLINK.md "DEBUG VIEW"): src/debug/.
set(KC_SRC_DEBUG)
if(KCLIENT_DEBUG_VIEW)
	set(KC_SRC_DEBUG src/debug/debugview.cpp)
endif()

# The layers, low to high.
kc_layer(kc_app DIR app SOURCES ${KC_SRC_APP} ${KCLIENT_GITHASH_CPP})
add_dependencies(kc_app kclient_githash)
kc_layer(kc_knobs DIR knobs SOURCES ${KC_SRC_KNOBS})
kc_layer(kc_net DIR net SOURCES ${KC_SRC_NET} ${KCLIENT_AI_TASK_NAMES_CPP})
kc_layer(kc_platform DIR platform SOURCES ${KC_SRC_PLATFORM} DEPS kc_app)
kc_layer(kc_assets DIR assets SOURCES ${KC_SRC_ASSETS} DEPS kc_app)
# Lossless WebP for the shots (F12, scene_test, captures; kenshi-k16.2.470.1): Linux only, our own dev tooling; Windows and macOS keep PNG and
# take no dependency. libwebp is BSD-3-Clause (docs/LICENSING.md). Not found: PNG shots, with a note.
# Default follows KCLIENT_DEV_TOOLS: a release build (-DKCLIENT_DEV_TOOLS=OFF) never links libwebp and writes PNG.
option(KCLIENT_WEBP "Lossless WebP shots (Linux only; needs libwebp; default follows KCLIENT_DEV_TOOLS)" ${KCLIENT_DEV_TOOLS})
if(KCLIENT_WEBP AND UNIX AND NOT APPLE)
	pkg_check_modules(WEBP IMPORTED_TARGET libwebp)
	if(WEBP_FOUND)
		target_compile_definitions(kc_assets PUBLIC KCLIENT_WEBP=1)
		target_link_libraries(kc_assets PUBLIC PkgConfig::WEBP)
	else()
		message(STATUS "libwebp not found: screenshots stay PNG")
	endif()
endif()
kc_layer(kc_render DIR render SOURCES ${KC_SRC_RENDER} src/render/drawlist.cpp DEPS kc_app kc_assets kc_knobs kc_net kc_platform)
kc_layer(kc_world DIR world SOURCES ${KC_SRC_WORLD} DEPS kc_app kc_assets kc_knobs kc_net kc_platform kc_render)
kc_layer(kc_characters DIR characters SOURCES ${KC_SRC_CHARACTERS} DEPS kc_app kc_assets kc_knobs kc_render)
kc_layer(kc_bridge DIR bridge SOURCES ${KC_SRC_BRIDGE} DEPS kc_app kc_characters kc_net kc_platform)
kc_layer(kc_input DIR input SOURCES ${KC_SRC_INPUT} DEPS kc_app kc_bridge kc_characters kc_net kc_render)
kc_layer(kc_frame DIR frame SOURCES ${KC_SRC_FRAME} DEPS kc_app kc_assets kc_bridge kc_characters kc_input kc_knobs kc_net kc_platform kc_render kc_world kui)
kc_layer(kc_debug DIR debug SOURCES ${KC_SRC_DEBUG} DEPS kc_net)
kc_layer(kc_dev DIR dev SOURCES ${KC_SRC_DEV}
	DEPS kc_app kc_assets kaudio kc_bridge kc_characters kc_debug kc_frame kc_input kc_knobs kc_net kc_platform kc_render kui kc_world)
kc_layer(kc_host DIR host SOURCES ${KC_SRC_HOST}
	DEPS kc_app kc_assets kaudio kc_bridge kc_characters kc_debug kc_dev kc_frame kc_input kc_knobs kc_net kc_platform kc_render kui kc_world)

# UI layer (src/ui/, docs/UI.md): widgets drawn with the game's skins and fonts. The host supplies the sokol, stb_image and stb_truetype
# implementations (kc_platform, kc_assets) and config.cpp (kc_app); the draw list it records into is kc_render's.
add_library(kui STATIC src/ui/xml.cpp src/ui/gfx.cpp src/ui/skin.cpp src/ui/font.cpp src/ui/widget.cpp src/ui/ui.cpp src/ui/hud.cpp src/ui/diamond.cpp src/ui/statgrid.cpp src/ui/options.cpp src/ui/keys.cpp src/ui/creator.cpp src/ui/stats.cpp src/ui/prompt.cpp src/ui/squads.cpp src/ui/overview.cpp src/ui/savepanel.cpp src/ui/menus.cpp src/ui/newgame.cpp src/ui/tutorials.cpp src/ui/jobs.cpp src/ui/prospecting.cpp src/ui/minimap.cpp src/ui/statuspopup.cpp src/ui/advanced.cpp src/ui/build.cpp src/ui/research.cpp src/ui/crafting.cpp src/ui/faction.cpp src/ui/trade.cpp src/ui/icons.cpp src/ui/itemgrid.cpp src/ui/gear.cpp src/ui/itemwindows.cpp src/ui/dialogue.cpp src/ui/bubbles.cpp src/ui/progressbars.cpp src/ui/namelabels.cpp src/ui/buildingwindow.cpp src/ui/cast.cpp src/ui/layout.cpp src/ui/style.cpp)
# The dev-only screens (src/ui/dev/, kenshi-k16.2.153.2): the playtest windows, the debug panel, trails panel and animation browser. The debug
# view's decision tree (debugtree) comes with either option: main.cpp uses it under KCLIENT_DEBUG_VIEW.
if(KCLIENT_DEV_TOOLS)
	target_sources(kui PRIVATE src/ui/dev/debugpanel.cpp src/ui/dev/trailspanel.cpp src/ui/dev/lightspanel.cpp src/ui/dev/animbrowser.cpp src/ui/dev/scenarios.cpp src/ui/dev/notes.cpp)
endif()
if(KCLIENT_DEV_TOOLS OR KCLIENT_DEBUG_VIEW)
	target_sources(kui PRIVATE src/ui/dev/debugtree.cpp)
endif()
add_dependencies(kui kclient_shaders kclient_layers)
target_link_libraries(kui PUBLIC kclient_base kc_app kc_render)
kc_edges(kui DIR ui DEPS kc_app kc_render)

# The dev tool executables (kenshi-k16.2.153): built only with KCLIENT_DEV_TOOLS, sources in src/dev/tools/.
if(KCLIENT_DEV_TOOLS)
	# Every widget on one screen (interactive, or --screenshot under tools/headless.sh). launchguard.cpp is compiled in directly: the demo does
	# not link kc_dev, and the guard is the one dev file it needs (kenshi-k16.2.392).
	add_executable(kui_demo src/dev/tools/kui_demo.cpp src/dev/launchguard.cpp)
	target_compile_options(kui_demo PRIVATE -Wall -Wextra)
	target_compile_definitions(kui_demo PRIVATE KENSHI_DATA="${KENSHI_DATA}")
	target_link_libraries(kui_demo PRIVATE kui kc_platform kc_assets kc_app Threads::Threads ${CMAKE_DL_LIBS} m)
	target_include_directories(kui_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../cpp/include)	# kutil/executable.hpp (header only)
	if(UNIX AND NOT APPLE)
		target_link_libraries(kui_demo PRIVATE PkgConfig::X11)
	endif()
	target_link_libraries(kui_demo PRIVATE ${KCLIENT_WIN32_LIBS})
	if(KENSHI_GFX STREQUAL "glcore")
		target_link_libraries(kui_demo PRIVATE OpenGL::GL)
	elseif(KENSHI_GFX STREQUAL "metal")
		target_link_libraries(kui_demo PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework MetalKit")
	else()
		target_compile_definitions(kui_demo PRIVATE VK_USE_PLATFORM_XLIB_KHR)
		target_link_libraries(kui_demo PRIVATE Vulkan::Vulkan)
	endif()
endif()

if(KCLIENT_DEV_TOOLS)
	# Parity-test CLI for the Ogre mesh reader (no graphics).
	add_executable(kmeshdump src/dev/tools/kmeshdump.cpp src/assets/ogremesh.cpp src/assets/resources.cpp src/assets/materials.cpp)
	target_include_directories(kmeshdump PRIVATE src)
	target_compile_options(kmeshdump PRIVATE -Wall -Wextra)

	# imagewrite's round trip (PNG, lossless WebP), kenshi-k16.2.470.1.
	add_executable(kimagewrite_test src/dev/tools/kimagewrite_test.cpp src/assets/imagewrite.cpp src/assets/stb_impl.cpp)
	target_include_directories(kimagewrite_test PRIVATE src)
	target_include_directories(kimagewrite_test SYSTEM PRIVATE third_party)
	target_compile_options(kimagewrite_test PRIVATE -Wall -Wextra)
	if(TARGET PkgConfig::WEBP)
		target_compile_definitions(kimagewrite_test PRIVATE KCLIENT_WEBP=1)
		target_link_libraries(kimagewrite_test PRIVATE PkgConfig::WEBP)
	endif()

	# Window geometry memory unit check (windowgeom.hpp's pure half: layout fingerprint, rect parse): header-only, nothing to link.
	add_executable(kwindowgeom_test src/dev/tools/kwindowgeom_test.cpp)
	target_include_directories(kwindowgeom_test PRIVATE src)
	target_compile_options(kwindowgeom_test PRIVATE -Wall -Wextra)

	# The render-side resource job queue's unit check (renderqueue.hpp: lanes, serials, the stream budget, Handle): no sokol.
	add_executable(krq_test src/dev/tools/krq_test.cpp src/render/renderqueue.cpp)
	target_include_directories(krq_test PRIVATE src)
	target_compile_options(krq_test PRIVATE -Wall -Wextra)

	# The 2D draw list's unit check (drawlist.hpp, render hook 8: the recorder, colour packing, uploads, ids): no sokol.
	add_executable(kdl_test src/dev/tools/kdl_test.cpp src/render/drawlist.cpp)
	target_include_directories(kdl_test PRIVATE src)
	target_compile_options(kdl_test PRIVATE -Wall -Wextra)

	# The floor view model's table-driven traces (floorfollow.hpp: the manual level, the auto floor, the stair latch): no GL, no data.
	add_executable(kfloor_test src/dev/tools/kfloor_test.cpp src/world/floorfollow.cpp)
	target_include_directories(kfloor_test PRIVATE src)
	target_compile_options(kfloor_test PRIVATE -Wall -Wextra)


	# SIMLINK decoder test CLI (docs/formats/SIMLINK.md): decodes a recorded server stream, or --selftest.
	add_executable(ksimlink_dump src/dev/tools/ksimlink_dump.cpp src/net/simlink.cpp)
	target_include_directories(ksimlink_dump PRIVATE src)
	target_compile_options(ksimlink_dump PRIVATE -Wall -Wextra)
endif()

# Audio (src/audio/, docs/AUDIO_ENGINE.md): Wwise banks, Wwise Vorbis decoder (headers rebuilt on load) over system libvorbis, mixer, music player.
if(MINGW)
	# MinGW: the .dll.a import library sorts before the .a archive in a full-path lookup, so the exe would import libvorbis-0.dll / libogg-0.dll.
	# Prefer archives for this lookup and ask pkg-config for the static closure (--static), so libogg follows libvorbis.
	set(_kc_saved_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
	set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
	set(_kc_saved_argn "${PKG_CONFIG_ARGN}")
	set(PKG_CONFIG_ARGN --static)
endif()
pkg_check_modules(VORBIS REQUIRED IMPORTED_TARGET vorbis ogg)
if(MINGW)
	set(CMAKE_FIND_LIBRARY_SUFFIXES "${_kc_saved_suffixes}")
	set(PKG_CONFIG_ARGN "${_kc_saved_argn}")
endif()
if(UNIX AND NOT APPLE)
	pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)	# sokol_audio's Linux backend (macOS: CoreAudio, below)
endif()
set(CODEBOOKS_BIN "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wwise_codebooks/packed_codebooks_aoTuV_603.bin")
set(CODEBOOKS_INC "${CMAKE_CURRENT_BINARY_DIR}/gen/wwise_codebooks.inc")
add_custom_command(
	OUTPUT ${CODEBOOKS_INC}
	COMMAND ${CMAKE_COMMAND} -DIN=${CODEBOOKS_BIN} -DOUT=${CODEBOOKS_INC} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2inc.cmake
	DEPENDS ${CODEBOOKS_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2inc.cmake
	COMMENT "embed Wwise codebooks"
	VERBATIM)
add_library(kaudio STATIC src/audio/wwbank.cpp src/audio/wwvorbis.cpp src/audio/wwhirc.cpp src/audio/mixer.cpp src/audio/music.cpp src/audio/sound.cpp src/audio/sokol_audio_impl.cpp src/audio/audiohook.cpp ${CODEBOOKS_INC})
target_include_directories(kaudio PUBLIC src PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_include_directories(kaudio SYSTEM PRIVATE third_party/sokol)
target_compile_options(kaudio PRIVATE -Wall -Wextra)
target_link_libraries(kaudio PUBLIC PkgConfig::VORBIS kclient_base kc_app kc_platform kc_render PRIVATE Threads::Threads)
add_dependencies(kaudio kclient_shaders kclient_layers)
kc_edges(kaudio DIR audio DEPS kc_app kc_platform kc_render)
if(APPLE)
	target_link_libraries(kaudio PRIVATE "-framework AudioToolbox")
elseif(WIN32)
	target_link_libraries(kaudio PRIVATE ole32)	# sokol_audio's WASAPI backend (CoInitializeEx)
else()
	target_link_libraries(kaudio PRIVATE PkgConfig::PULSE)
endif()

if(KCLIENT_DEV_TOOLS)
	# Audio test CLI: list / decode-check / wav-dump bank media, offline mixer renders, device playback.
	add_executable(kaudio_dump src/dev/tools/kaudio_dump.cpp)
	target_compile_options(kaudio_dump PRIVATE -Wall -Wextra)
	target_link_libraries(kaudio_dump PRIVATE kaudio)
endif()

# The two executables (client/README.md "Release targets"): main.cpp and localserver.cpp (and host/hud, logicframe, events, which see
# KENSHI_EMBED_SERVER through app.hpp) compile per target; everything else is the kc_* libraries, linked through kc_host.
# kclient is the client only (LocalServer fork()+execv of OpenKenshi-server); OpenKenshi is the full single-player target, with LocalServer running
# ksim::runServer on a std::thread (KENSHI_EMBED_SERVER, src/platform/localserver.cpp), joined over loopback TCP, plus the ksim library. OpenKenshi
# stays out of the default build (cmake --build ... --target OpenKenshi).
set(KENSHI_PER_TARGET_SOURCES src/main.cpp src/platform/localserver.cpp src/host/hud.cpp src/host/logicframe.cpp src/host/events.cpp)
foreach(tgt kclient OpenKenshi)
	if(tgt STREQUAL "kclient")
		add_executable(kclient ${KENSHI_PER_TARGET_SOURCES})
	else()
		add_executable(OpenKenshi EXCLUDE_FROM_ALL ${KENSHI_PER_TARGET_SOURCES})
		target_compile_definitions(OpenKenshi PRIVATE KENSHI_EMBED_SERVER)
		target_link_libraries(OpenKenshi PRIVATE ksim)
	endif()
	add_dependencies(${tgt} kclient_shaders kclient_githash kclient_layers)
	target_link_libraries(${tgt} PRIVATE kc_host)
	if(MINGW)
		target_link_options(${tgt} PRIVATE -mwindows)	# GUI subsystem: no console window; the log goes to a file (platform/console.hpp, kenshi-379.31.9.1)
	endif()
endforeach()
# A dev-off OpenKenshi runs its embedded server's sim data from simdata/ beside it (cpp/src/ksim/core/simdata.cpp), as ksim_server does in cpp/
# (the macOS tester's patch, kenshi-379.32.13; same copy as cpp/CMakeLists.txt's ksim_server POST_BUILD).
if(NOT KCLIENT_DEV_TOOLS)
	add_custom_command(TARGET OpenKenshi POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:OpenKenshi>/simdata
		COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:OpenKenshi>/simdata
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../cpp/data/behaviour $<TARGET_FILE_DIR:OpenKenshi>/simdata/behaviour
		COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/../cpp/data/ksim_ai_behaviours.tsv $<TARGET_FILE_DIR:OpenKenshi>/simdata/
		VERBATIM)
endif()
# The release client binary is OpenKenshi-client (kenshi-379.11.1); a dev build keeps the file name kclient (scripts use client/build/kclient).
if(NOT KCLIENT_DEV_TOOLS)
	set_target_properties(kclient PROPERTIES OUTPUT_NAME OpenKenshi-client)
endif()

# The declared edges for tools/layer_check.py (every kc_layer / kc_edges call above)
get_property(KC_LAYER_ROWS_ALL GLOBAL PROPERTY KC_LAYER_ROWS)
string(REPLACE ";" "\n" KC_LAYER_TEXT "${KC_LAYER_ROWS_ALL}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/layers.txt" "${KC_LAYER_TEXT}\n")
