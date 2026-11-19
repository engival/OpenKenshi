#pragma once
// The GL entry points the client calls itself, beside sokol_gfx (readbacks, the GPU pass timers, the id buffer, the last-frame copy): glcore
// builds only. The prototypes are declared (GL_GLEXT_PROTOTYPES); Linux's libGL exports every one of them. Windows' opengl32 exports GL 1.1
// alone: platform/windows/gl.cpp defines the newer ones, each loading itself through wglGetProcAddress (kenshi-379.31.1). Windows' gl.h also
// needs windows.h, whose near / far / NEAR / FAR macros would eat the client's variables of those names: they are emptied here.
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#if defined(_WIN32)
#include <windows.h>
#undef near
#undef far
#undef NEAR
#undef FAR
#define NEAR
#define FAR
#endif
#include <GL/gl.h>
#include <GL/glext.h>
