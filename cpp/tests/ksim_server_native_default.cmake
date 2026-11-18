# ksim_server with no nav argument starts on the runtime mesh (MeshNavigator::openNative, patch cache on) and plans a route at the
# Hub; --native-nav is accepted as the same thing. Run with cmake -DSERVER=... -DWORK=DIR -P (CMakeLists.txt, ksim_server_native_default).
# The cache is a scratch directory (KENSHI_NAV_CACHE), so the first run is cold and the second reads what the first wrote.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/script.txt" "2 speed 1\n3 pause 0\n10 move PLAYER -50850 2780\n")
set(ENV{KENSHI_NAV_CACHE} "${WORK}/cache")

foreach(variant default alias)
	set(extra "")
	if(variant STREQUAL "alias")
		set(extra "--native-nav")
	endif()
	execute_process(
		COMMAND "${SERVER}" --scenario duel ${extra} --dump-snapshot "${WORK}/${variant}.txt" --dump-ticks 30 --dump-script "${WORK}/script.txt"
		RESULT_VARIABLE status ERROR_VARIABLE log OUTPUT_QUIET)
	if(NOT status EQUAL 0)
		message(FATAL_ERROR "${variant}: ksim_server exited ${status}\n${log}")
	endif()
	if(NOT log MATCHES "navmesh streaming the shipped navtiles \\([0-9]+ tiles")
		message(FATAL_ERROR "${variant}: the navigator is not the native one\n${log}")
	endif()
	if(log MATCHES "navmesh (baked|loaded)")
		message(FATAL_ERROR "${variant}: the server baked or loaded a Hub mesh\n${log}")
	endif()
	file(READ "${WORK}/${variant}.txt" snapshot)
	# The move from the duel's start (-50981, 2931) to a Hub street spot: planned (a CHAR_ROUTE with its end), not refused. About 200 u and
	# in the open, so since k16.6.16 (player moves under 300 u go on the way) the route is the end alone: points=1 is a pass.
	if(NOT snapshot MATCHES "CHAR_ROUTE [^\n]*points=[1-9]\\[[^\n]*x=-50850\\.000,y=[0-9.-]+,z=2780\\.000\\]")
		message(FATAL_ERROR "${variant}: no planned route to the Hub street spot\n${snapshot}")
	endif()
	message(STATUS "${variant}: native navigator, Hub route planned")
endforeach()
