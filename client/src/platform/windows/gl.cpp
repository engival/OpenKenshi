// The GL entry points past 1.1 that the client calls itself (platform/gl.hpp), kenshi-379.31.1. opengl32.dll exports GL 1.1 only; each function
// here is the glext.h prototype, loading its driver pointer on the first call (wglGetProcAddress, under the render thread's current context; the
// pointers are the same for every context of one pixel format on one device, the one window sokol_app makes). sokol_gfx loads its own set.
// Add a function here when a new one fails to link on Windows (x86_64-w64-mingw32-nm -u lists them).
#include "platform/gl.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
void* load(const char* name)
{
	void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
	const auto v = reinterpret_cast<intptr_t>(p);
	if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1)	// failure values some drivers return instead of null
	{
		p = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("opengl32.dll"), name));
	}
	if (!p)
	{
		fprintf(stderr, "kclient: OpenGL: the driver has no %s (OpenGL 4.3 core is required)\n", name);
		abort();
	}
	return p;
}
}

#define KGL_PTR(name) static const auto p = reinterpret_cast<decltype(&::name)>(load(#name))

extern "C" {
void APIENTRY glBeginQuery(GLenum target, GLuint id) { KGL_PTR(glBeginQuery); p(target, id); }
void APIENTRY glEndQuery(GLenum target) { KGL_PTR(glEndQuery); p(target); }
void APIENTRY glGenQueries(GLsizei n, GLuint* ids) { KGL_PTR(glGenQueries); p(n, ids); }
void APIENTRY glDeleteQueries(GLsizei n, const GLuint* ids) { KGL_PTR(glDeleteQueries); p(n, ids); }
void APIENTRY glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) { KGL_PTR(glGetQueryObjectuiv); p(id, pname, params); }
void APIENTRY glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params) { KGL_PTR(glGetQueryObjectui64v); p(id, pname, params); }

void APIENTRY glGenBuffers(GLsizei n, GLuint* buffers) { KGL_PTR(glGenBuffers); p(n, buffers); }
void APIENTRY glDeleteBuffers(GLsizei n, const GLuint* buffers) { KGL_PTR(glDeleteBuffers); p(n, buffers); }
void APIENTRY glBindBuffer(GLenum target, GLuint buffer) { KGL_PTR(glBindBuffer); p(target, buffer); }
void APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) { KGL_PTR(glBufferData); p(target, size, data, usage); }
void* APIENTRY glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) { KGL_PTR(glMapBufferRange); return p(target, offset, length, access); }
GLboolean APIENTRY glUnmapBuffer(GLenum target) { KGL_PTR(glUnmapBuffer); return p(target); }

void APIENTRY glGenFramebuffers(GLsizei n, GLuint* framebuffers) { KGL_PTR(glGenFramebuffers); p(n, framebuffers); }
void APIENTRY glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) { KGL_PTR(glDeleteFramebuffers); p(n, framebuffers); }
void APIENTRY glBindFramebuffer(GLenum target, GLuint framebuffer) { KGL_PTR(glBindFramebuffer); p(target, framebuffer); }
GLenum APIENTRY glCheckFramebufferStatus(GLenum target) { KGL_PTR(glCheckFramebufferStatus); return p(target); }
void APIENTRY glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) { KGL_PTR(glFramebufferTexture2D); p(target, attachment, textarget, texture, level); }
void APIENTRY glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) { KGL_PTR(glFramebufferRenderbuffer); p(target, attachment, renderbuffertarget, renderbuffer); }
void APIENTRY glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) { KGL_PTR(glBlitFramebuffer); p(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter); }

void APIENTRY glGenRenderbuffers(GLsizei n, GLuint* renderbuffers) { KGL_PTR(glGenRenderbuffers); p(n, renderbuffers); }
void APIENTRY glDeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers) { KGL_PTR(glDeleteRenderbuffers); p(n, renderbuffers); }
void APIENTRY glBindRenderbuffer(GLenum target, GLuint renderbuffer) { KGL_PTR(glBindRenderbuffer); p(target, renderbuffer); }
void APIENTRY glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) { KGL_PTR(glRenderbufferStorage); p(target, internalformat, width, height); }
}
