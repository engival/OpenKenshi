// The one translation unit that holds the sokol implementations (backend chosen by CMake).
#define SOKOL_IMPL
#ifdef KCLIENT_DEV_TOOLS
#define SOKOL_TRACE_HOOKS	// dev/sgowner.cpp: the thread-owner assert
#endif
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"
