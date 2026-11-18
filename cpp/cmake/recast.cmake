# Recast/Detour for knav and the client (kenshi-379.32.1). The system package when there is one (Linux: installed system-wide; Homebrew:
# `brew install recastnavigation`, if the formula is absent the FetchContent fallback below builds the pinned tag of our fork: upstream main 9f4ce64,
# kenshi-379.32.4; every platform on that pin, the system package dropped: kenshi-379.34). One include per directory
# scope (the imported targets are directory scoped), so cpp/ and client/ both include this file.
if(NOT TARGET RecastNavigation::Detour)
	find_package(recastnavigation QUIET)
endif()
if(NOT TARGET RecastNavigation::Detour)
	message(STATUS "recastnavigation: not installed, fetching github.com/engival/recastnavigation ok-20260227-9f4ce64")
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
