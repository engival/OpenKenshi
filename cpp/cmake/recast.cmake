# Recast/Detour for knav and the client (kenshi-379.32.1). The system package when there is one (Linux: installed system-wide; Homebrew:
# `brew install recastnavigation`, if the formula is absent the FetchContent fallback below builds the pinned tag of our fork: upstream main 9f4ce64,
# kenshi-379.32.4; every platform on that pin, the system package dropped: kenshi-379.34). One include per directory
# scope (the imported targets are directory scoped), so cpp/ and client/ both include this file.
# Always the pinned fork, built here: the system package is 32-bit dtPolyRef, and we need DT_POLYREF64 (a tile past 512 polygons; kenshi-k16.7.189).
if(NOT TARGET RecastNavigation::Detour)
	message(STATUS "recastnavigation: fetching github.com/engival/recastnavigation ok-20260227-9f4ce64")
	include(FetchContent)
	set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
	set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
	set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(recastnavigation_src
		GIT_REPOSITORY https://github.com/engival/recastnavigation.git
		GIT_TAG ok-20260227-9f4ce64)
	FetchContent_MakeAvailable(recastnavigation_src)
	foreach(t Recast Detour DetourCrowd DetourTileCache DebugUtils)
		if(TARGET ${t} AND NOT TARGET RecastNavigation::${t})
			add_library(RecastNavigation::${t} ALIAS ${t})
		endif()
	endforeach()
endif()
# 64-bit polygon refs everywhere Detour's headers are seen (the layout of dtNavMesh depends on it).
foreach(t Detour DetourCrowd DetourTileCache DebugUtils)
	if(TARGET ${t})
		target_compile_definitions(${t} PUBLIC DT_POLYREF64 DT_VIRTUAL_QUERYFILTER)
	endif()
endforeach()
# The virtual query filter (a flat door cost, knav/query_filter.hpp) needs dtQueryFilter's typeinfo; the fork builds Detour -fno-rtti.
if(NOT MSVC)
	foreach(t Detour DetourCrowd DetourTileCache DebugUtils)
		if(TARGET ${t})
			target_compile_options(${t} PRIVATE -frtti)
		endif()
	endforeach()
endif()
# Recast builds as C++98, where MinGW's <stdint.h> hides INTPTR_MAX (RecastAlloc.h's RC_SIZE_MAX) unless __STDC_LIMIT_MACROS; PUBLIC because
# consumer TUs include RecastAlloc.h too (kenshi-379.31.4).
if(MINGW)
	foreach(t Recast Detour DetourCrowd DetourTileCache DebugUtils)
		if(TARGET ${t})
			target_compile_definitions(${t} PUBLIC __STDC_LIMIT_MACROS __STDC_CONSTANT_MACROS)
		endif()
	endforeach()
endif()
