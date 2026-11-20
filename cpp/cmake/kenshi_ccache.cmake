# The shared kenshi ccache (kenshi-zae4; user 2026-11-19 on kenshi-vyr.161: "It should be automatic on all our build scripts. A simple
# 'if exists ccache.conf' then use it."). /var/cache/kenshi-ccache is shared by ACL between the desktop accounts and carries its own
# ccache.conf. Include right after project(): a configure on a host with that file, ccache installed and no launcher of its own
# (dev's test queue and worker launchers pass theirs) compiles through it. Hosts without the file (dev, CI, release export) are untouched.
# client/CMakeLists.txt may include this same file.
set(KENSHI_CCACHE_DIR "/var/cache/kenshi-ccache")
if(EXISTS "${KENSHI_CCACHE_DIR}/ccache.conf" AND NOT CMAKE_CXX_COMPILER_LAUNCHER AND NOT CMAKE_C_COMPILER_LAUNCHER)
	find_program(KENSHI_CCACHE_PROGRAM ccache)
	if(KENSHI_CCACHE_PROGRAM)
		set(CMAKE_C_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;CCACHE_DIR=${KENSHI_CCACHE_DIR};${KENSHI_CCACHE_PROGRAM}")
		set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;CCACHE_DIR=${KENSHI_CCACHE_DIR};${KENSHI_CCACHE_PROGRAM}")
		message(STATUS "ccache: shared kenshi cache ${KENSHI_CCACHE_DIR} (${KENSHI_CCACHE_PROGRAM})")
	endif()
endif()
