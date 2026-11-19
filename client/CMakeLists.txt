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
# KCLIENT_PLATFORM: the rest of the OS layer (src/platform/*.hpp): the simlink socket and the local server child (POSIX on Linux and macOS,
# Winsock / CreateProcess on Windows), the default game installs (installs.hpp, one per OS) and, on Windows, the GL loader (gl.hpp).
if(WIN32)
	set(KCLIENT_PLATFORM src/platform/windows/socket.cpp src/platform/windows/process.cpp src/platform/windows/installs.cpp
		src/platform/windows/gl.cpp)	# gl.cpp: the GL entry points past 1.1 (platform/gl.hpp)
	# sokol_app / sokol_gfx (GL) / sokol_audio (WASAPI) system libraries: MSVC takes them from the headers' #pragma comment, MinGW needs them
	# listed (sokol_app.h "with MINGW/MSYS2 gcc"); ws2_32: the simlink socket; advapi32: the registry (installs.cpp).
	set(KCLIENT_WIN32_LIBS kernel32 user32 shell32 gdi32 ole32 advapi32 ws2_32)
elseif(APPLE)
	set(KCLIENT_PLATFORM src/platform/posix/socket.cpp src/platform/posix/process.cpp src/platform/macos/installs.cpp)
	set(KCLIENT_WIN32_LIBS)
else()
	set(KCLIENT_PLATFORM src/platform/posix/socket.cpp src/platform/posix/process.cpp src/platform/linux/installs.cpp)
	set(KCLIENT_WIN32_LIBS)
endif()

# The client sources, compiled once into this OBJECT library and shared by kclient and OpenKenshi (kenshi-379.6.2.1). Only the files that
# depend on KENSHI_EMBED_SERVER (main.cpp, localserver.cpp; the only includers of localserver.hpp) compile per executable, below.
add_library(kclient_obj OBJECT
	src/render/ao.cpp
	src/render/lightvol.cpp
	src/world/buildings.cpp
	src/render/camera.cpp
	src/characters/characters.cpp
	src/characters/charload.cpp
	src/characters/charanim.cpp
	src/characters/chardraw.cpp
	src/characters/creator.cpp
	src/app/eventlog.cpp
	src/net/simlink.cpp
	src/net/simview.cpp
	src/app/datadir.cpp
	src/app/steamvdf.cpp
	src/app/datapicker.cpp
	third_party/tinyfiledialogs/tinyfiledialogs.c
	src/render/trails.cpp
	src/world/clientnav.cpp
	src/render/navoverlay.cpp
	src/input/collisionpick.cpp
	src/input/pick.cpp
	src/input/controlhints.cpp
	src/input/cursors.cpp
	src/app/sappcmd.cpp
	src/app/logicthread.cpp
	src/app/lastframe.cpp
	src/render/colourmap.cpp
	src/render/blooddecals.cpp
	src/render/renderqueue.cpp
	src/render/drawlist_gpu.cpp
	src/render/overlaylist.cpp
	src/characters/portraits.cpp
	src/assets/dds.cpp
	src/render/detail.cpp
	src/render/foliage.cpp
	src/world/floorfollow.cpp
	src/render/fogvolume.cpp
	src/render/sunshafts.cpp
	src/render/dust.cpp
	src/render/grass.cpp
	src/render/gputimer.cpp
	src/render/hdr.cpp
	src/render/reflection.cpp
	src/render/idbuffer.cpp
	src/render/readback.cpp
	src/render/renderframe.cpp
	src/render/post.cpp
	src/render/smaa_lut.cpp
	src/assets/materials.cpp
	src/assets/mods.cpp
	src/assets/ogremesh.cpp
	src/render/openwindows.cpp
	src/render/particles.cpp
	src/render/particleworld.cpp
	src/render/beamfield.cpp
	src/characters/physics.cpp
	src/characters/ragdolldrive.cpp
	src/characters/ragdoll.cpp
	src/assets/resources.cpp
	src/render/shadow.cpp
	src/render/sky.cpp
	src/characters/creatorscene.cpp
	src/render/speccube.cpp
	src/world/weatherinstances.cpp
	src/world/weathermap.cpp
	src/world/squads.cpp
	src/render/arrows.cpp
	src/render/terrain.cpp
	src/world/towns.cpp
	src/render/water.cpp
	src/render/lightbake.cpp
	src/world/worldbuildings.cpp
	src/world/wbload.cpp
	src/world/wbfloors.cpp
	src/world/wbdraw.cpp
	src/app/loadscreen.cpp
	src/host/pick.cpp
	src/world/worldmap.cpp
	${KCLIENT_SOKOL_IMPL}
	src/audio/audiohook.cpp
	src/app/config.cpp
	${KCLIENT_WINDOWGEOM}
	${KCLIENT_PLATFORM}
	src/app/windowgate.cpp
	src/app/serverrows.cpp
	src/app/workpool.cpp
	src/assets/stb_impl.cpp
)
add_dependencies(kclient_obj kclient_shaders)
# The sim's DEBUG VIEW (docs/formats/SIMLINK.md "DEBUG VIEW"): src/debug/. ON by default in every build type (a small decoder; the data flows
# only from a server built with KSIM_DEBUG_VIEW and run with --debug-view)
option(KCLIENT_DEBUG_VIEW "Build the sim debug view (src/debug/)" ON)
if(KCLIENT_DEBUG_VIEW)
	target_sources(kclient_obj PRIVATE src/debug/debugview.cpp)
	target_compile_definitions(kclient_obj PRIVATE KCLIENT_DEBUG_VIEW)
endif()
# Profiling (kenshi-k16.2.101): the GPU pass timers (src/gputimer) and the per-pass submit counters (src/drawstats), reported per scene in the
# scene test JSON. OFF compiles all of it out: GPU_BEGIN/GPU_END are empty statements, the draw wrappers are plain pass-throughs and the JSON
# has none of the profile fields.
option(KCLIENT_PROFILE "GPU pass timers and per-pass submit counters (scene test JSON)" ON)
target_compile_definitions(kclient_obj PRIVATE KCLIENT_PROFILE=$<BOOL:${KCLIENT_PROFILE}>)
if(KCLIENT_DEV_TOOLS)
	# one source per line, so two branches adding dev tools do not touch the same line (render hook 10)
	target_sources(kclient_obj PRIVATE
		src/dev/devhost.cpp
		src/dev/debughost.cpp
		src/dev/gaitlog.cpp
		src/dev/poslog.cpp
		src/dev/playtest.cpp
		src/dev/beadpost.cpp
		src/dev/animlab.cpp
		src/dev/charops.cpp
		src/dev/sgowner.cpp
		src/dev/pickboxes.cpp
		src/dev/pickmask.cpp
		src/dev/scenetest.cpp
		src/dev/probe.cpp
		src/dev/scriptlog.cpp
		src/dev/viewmesh.cpp
		src/dev/hintsop.cpp
		src/dev/lightterms.cpp
		src/dev/packetdigest.cpp
		src/dev/shotsidecars.cpp
	)
	target_compile_definitions(kclient_obj PRIVATE KCLIENT_DEV_TOOLS)
endif()
# tinyfiledialogs is C that compiles as C++ (upstream: "rename this .c file as .cpp"); the project is CXX only. Vendored untouched, so no warnings from it.
set_source_files_properties(third_party/tinyfiledialogs/tinyfiledialogs.c PROPERTIES LANGUAGE CXX COMPILE_OPTIONS -w)
# the SMAA tables must match upstream bit for bit: no fused multiply-adds
set_source_files_properties(src/render/smaa_lut.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
target_include_directories(kclient_obj PRIVATE src ${SHADER_DIR})
target_include_directories(kclient_obj SYSTEM PRIVATE third_party third_party/sokol)
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
add_dependencies(kclient_obj kclient_githash)
target_sources(kclient_obj PRIVATE ${KCLIENT_GITHASH_CPP})

# The AI task names for CHAR_STATS' AI tail (rows 16 / 18; src/net/aitask.hpp, main.cpp): generated from the docs' table at build time, so no
# name is typed in the client and a doc rerun shows without a client edit. Rebuilt only when the doc or the generator changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(KCLIENT_AI_TASK_NAMES_CPP "${CMAKE_CURRENT_BINARY_DIR}/gen/ai_task_names.cpp")
add_custom_command(
	OUTPUT ${KCLIENT_AI_TASK_NAMES_CPP}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_ai_task_names.py
		--doc ${CMAKE_CURRENT_SOURCE_DIR}/../docs/formats/AI_TASK_ENUMS.md --out ${KCLIENT_AI_TASK_NAMES_CPP}
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../docs/formats/AI_TASK_ENUMS.md ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_ai_task_names.py
	COMMENT "generate ai_task_names.cpp"
	VERBATIM)
target_sources(kclient_obj PRIVATE ${KCLIENT_AI_TASK_NAMES_CPP})

target_compile_definitions(kclient_obj PRIVATE ${GFX_DEFINE})
# The build's own data dir as datadir.cpp's last search place: a dev build only (kenshi-379.13). A release resolves the game data at runtime,
# and a shipped binary must not carry the developer's home path (client/tools/dev_off_check.sh).
if(KCLIENT_DEV_TOOLS)
	target_compile_definitions(kclient_obj PRIVATE KENSHI_DATA="${KENSHI_DATA}")
endif()
target_compile_options(kclient_obj PRIVATE -Wall -Wextra)
if(WIN32)
	target_link_libraries(kclient_obj PRIVATE comdlg32 ole32 ${KCLIENT_WIN32_LIBS})	# comdlg32: tinyfiledialogs' native folder dialog
endif()
target_link_libraries(kclient_obj PRIVATE kworld knav kutil RecastNavigation::Detour kaudio kui Jolt Threads::Threads ${CMAKE_DL_LIBS} m)
if(UNIX AND NOT APPLE)
	target_link_libraries(kclient_obj PRIVATE PkgConfig::X11)
endif()
if(KENSHI_GFX STREQUAL "glcore")
	find_package(OpenGL REQUIRED)
	target_link_libraries(kclient_obj PRIVATE OpenGL::GL)
elseif(KENSHI_GFX STREQUAL "metal")
	target_link_libraries(kclient_obj PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework MetalKit")
else()
	find_package(Vulkan REQUIRED)
	target_compile_definitions(kclient_obj PRIVATE VK_USE_PLATFORM_XLIB_KHR)
	target_link_libraries(kclient_obj PRIVATE Vulkan::Vulkan)
endif()

# UI layer (src/ui/, docs/UI.md): widgets drawn with the game's skins and fonts. The host supplies the sokol, stb_image and stb_truetype
# implementations and config.cpp (kclient: sokol_impl, stb_impl, loadscreen).
add_library(kui STATIC src/render/drawlist.cpp src/ui/xml.cpp src/ui/gfx.cpp src/ui/skin.cpp src/ui/font.cpp src/ui/widget.cpp src/ui/ui.cpp src/ui/hud.cpp src/ui/diamond.cpp src/ui/statgrid.cpp src/ui/options.cpp src/ui/keys.cpp src/ui/creator.cpp src/ui/stats.cpp src/ui/prompt.cpp src/ui/squads.cpp src/ui/overview.cpp src/ui/savepanel.cpp src/ui/menus.cpp src/ui/newgame.cpp src/ui/tutorials.cpp src/ui/jobs.cpp src/ui/prospecting.cpp src/ui/advanced.cpp src/ui/build.cpp src/ui/research.cpp src/ui/crafting.cpp src/ui/faction.cpp src/ui/trade.cpp src/ui/icons.cpp src/ui/itemgrid.cpp src/ui/gear.cpp src/ui/itemwindows.cpp src/ui/dialogue.cpp src/ui/bubbles.cpp src/ui/progressbars.cpp src/ui/namelabels.cpp src/ui/buildingwindow.cpp src/ui/cast.cpp src/ui/layout.cpp src/ui/style.cpp)
target_include_directories(kui PUBLIC src)
# the UI counts its sgl batches for the Ui GPU section (kenshi-k16.2.101.1); compiled out with KCLIENT_PROFILE=OFF
target_compile_definitions(kui PRIVATE KCLIENT_PROFILE=$<BOOL:${KCLIENT_PROFILE}>)
# The dev-only screens (src/ui/dev/, kenshi-k16.2.153.2): the playtest windows, the debug panel, trails panel and animation browser. The debug
# view's decision tree (debugtree) comes with either option: main.cpp uses it under KCLIENT_DEBUG_VIEW.
if(KCLIENT_DEV_TOOLS)
	target_sources(kui PRIVATE src/ui/dev/debugpanel.cpp src/ui/dev/trailspanel.cpp src/ui/dev/lightspanel.cpp src/ui/dev/animbrowser.cpp src/ui/dev/scenarios.cpp src/ui/dev/notes.cpp)
	target_compile_definitions(kui PUBLIC KCLIENT_DEV_TOOLS)
endif()
if(KCLIENT_DEV_TOOLS OR KCLIENT_DEBUG_VIEW)
	target_sources(kui PRIVATE src/ui/dev/debugtree.cpp)
endif()
target_include_directories(kui SYSTEM PUBLIC third_party third_party/sokol)
target_compile_definitions(kui PUBLIC ${GFX_DEFINE})
target_compile_options(kui PRIVATE -Wall -Wextra)

# The dev tool executables (kenshi-k16.2.153): built only with KCLIENT_DEV_TOOLS, sources in src/dev/tools/.
if(KCLIENT_DEV_TOOLS)
	# Every widget on one screen (interactive, or --screenshot under tools/headless.sh).
	add_executable(kui_demo src/dev/tools/kui_demo.cpp src/render/drawlist_gpu.cpp src/assets/dds.cpp src/app/config.cpp src/app/serverrows.cpp ${KCLIENT_SOKOL_IMPL} src/assets/stb_impl.cpp)
	target_compile_options(kui_demo PRIVATE -Wall -Wextra)
	target_compile_definitions(kui_demo PRIVATE KENSHI_DATA="${KENSHI_DATA}")
	target_link_libraries(kui_demo PRIVATE kui Threads::Threads ${CMAKE_DL_LIBS} m)
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

	# The Steam VDF parsing behind the data-dir discovery (steamvdf.hpp): libraryfolders.vdf nesting, escapes, appmanifest installdir. ctest: kdatadir.
	add_executable(kdatadir_test src/dev/tools/kdatadir_test.cpp src/app/steamvdf.cpp)
	target_include_directories(kdatadir_test PRIVATE src)
	target_compile_options(kdatadir_test PRIVATE -Wall -Wextra)
	enable_testing()
	add_test(NAME kdatadir COMMAND kdatadir_test)

	# SIMLINK decoder test CLI (docs/formats/SIMLINK.md): decodes a recorded server stream, or --selftest.
	add_executable(ksimlink_dump src/dev/tools/ksimlink_dump.cpp src/net/simlink.cpp)
	target_include_directories(ksimlink_dump PRIVATE src)
	target_compile_options(ksimlink_dump PRIVATE -Wall -Wextra)
endif()

# Audio (src/audio/, docs/AUDIO_ENGINE.md): Wwise banks, Wwise Vorbis decoder (headers rebuilt on load) over system libvorbis, mixer, music player.
pkg_check_modules(VORBIS REQUIRED IMPORTED_TARGET vorbis ogg)
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
add_library(kaudio STATIC src/audio/wwbank.cpp src/audio/wwvorbis.cpp src/audio/wwhirc.cpp src/audio/mixer.cpp src/audio/music.cpp src/audio/sound.cpp src/audio/sokol_audio_impl.cpp ${CODEBOOKS_INC})
target_include_directories(kaudio PUBLIC src PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_include_directories(kaudio SYSTEM PRIVATE third_party/sokol)
target_compile_options(kaudio PRIVATE -Wall -Wextra)
target_link_libraries(kaudio PUBLIC PkgConfig::VORBIS PRIVATE Threads::Threads)
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

# The two executables (client/README.md "Release targets"): both link the kclient_obj objects; main.cpp and localserver.cpp compile per target.
# kclient is the client only (LocalServer fork()+execv of OpenKenshi-server); OpenKenshi is the full single-player target, with LocalServer running
# ksim::runServer on a std::thread (KENSHI_EMBED_SERVER, src/app/localserver.cpp), joined over loopback TCP, plus the ksim library. Defines, includes
# and libraries are taken from kclient_obj at the end of this file, so every later option reaches both. OpenKenshi stays out of the default build
# (cmake --build ... --target OpenKenshi).
get_target_property(KENSHI_DEFS kclient_obj COMPILE_DEFINITIONS)
get_target_property(KENSHI_LIBS kclient_obj LINK_LIBRARIES)
set(KENSHI_PER_TARGET_SOURCES src/main.cpp src/app/localserver.cpp)
foreach(tgt kclient OpenKenshi)
	if(tgt STREQUAL "kclient")
		add_executable(kclient ${KENSHI_PER_TARGET_SOURCES} $<TARGET_OBJECTS:kclient_obj>)
	else()
		add_executable(OpenKenshi EXCLUDE_FROM_ALL ${KENSHI_PER_TARGET_SOURCES} $<TARGET_OBJECTS:kclient_obj>)
		target_compile_definitions(OpenKenshi PRIVATE KENSHI_EMBED_SERVER)
		target_link_libraries(OpenKenshi PRIVATE ksim)
	endif()
	add_dependencies(${tgt} kclient_obj kclient_shaders kclient_githash)
	target_include_directories(${tgt} PRIVATE src ${SHADER_DIR})
	target_include_directories(${tgt} SYSTEM PRIVATE third_party third_party/sokol)
	target_compile_definitions(${tgt} PRIVATE ${KENSHI_DEFS})
	target_compile_options(${tgt} PRIVATE -Wall -Wextra)
	target_link_libraries(${tgt} PRIVATE ${KENSHI_LIBS})
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
