/* Render thread: GL calls recorded by the game thread, replayed on another core.
 *
 * With the render thread on (the default; render_thread=0 in igi.cfg or
 * IGI_RENDER_THREAD=0 turns it off) every GL call of the renderer goes through
 * a q_gl* wrapper that appends it to the current frame's command stream; a
 * thread on another core owns the GL context and executes the stream. The game
 * thread runs at most one frame ahead of it. Off, the wrappers call GL directly.
 *
 * Include after the GL headers: the gl* names used by render_gl.c are redirected
 * to the wrappers below.
 *
 * Rules for callers while the thread is on:
 *  - client pointers handed to GL (vertex/colour/... arrays, vglBufferData) must
 *    stay valid and unchanged until the frame has been executed: use rq_data or
 *    rq_vtx_reserve memory (or GPU memory the frame owns);
 *  - texture, buffer and index data passed by pointer is copied when recorded,
 *    except through q_glTexImage2D_owned (pixels already in rq_data memory);
 *  - GL names from glGenTextures/glGenBuffers are the recorder's own and only
 *    mean something to the wrappers.
 */
#ifndef IGI_RENDER_QUEUE_H
#define IGI_RENDER_QUEUE_H

#include <stddef.h>
#include <stdint.h>

/* 1 while GL calls are being recorded for the render thread. */
extern int rq_on;

/* Called once before the first GL call: decides the mode, sets up the GL context
 * on the thread that will own it (starting the render thread if on). */
void rq_start(void);
/* Swap buffers (recorded), then end the frame: hands it to the render thread and
 * waits until the previous frame has been executed. Returns the microseconds waited. */
uint64_t rq_swap_end_frame(void);
/* Waits until the render thread has executed everything recorded so far. */
void rq_sync(void);
/* Drains the stream and stops the render thread (GL calls are direct afterwards). */
void rq_shutdown(void);
/* Render thread busy time since the last call, in microseconds. */
uint64_t rq_take_busy_us(void);

/* Memory valid until the current frame has been executed (16-byte aligned). */
void *rq_data(size_t n);
/* Streaming vertex storage: room for up to n bytes, then the bytes actually used
 * are committed (one reservation open at a time). */
void *rq_vtx_reserve(size_t n);
void rq_vtx_commit(size_t used);

/* ---- wrappers ---- */
typedef double rq_gldouble;

void q_glEnable(GLenum cap);
void q_glDisable(GLenum cap);
void q_glEnableClientState(GLenum cap);
void q_glDisableClientState(GLenum cap);
void q_glBlendFunc(GLenum s, GLenum d);
void q_glDepthFunc(GLenum f);
void q_glDepthMask(GLboolean m);
void q_glAlphaFunc(GLenum f, GLfloat ref);
void q_glCullFace(GLenum m);
void q_glFrontFace(GLenum m);
void q_glShadeModel(GLenum m);
void q_glPolygonMode(GLenum face, GLenum m);
void q_glActiveTexture(GLenum u);
void q_glClientActiveTexture(GLenum u);
void q_glTexEnvi(GLenum target, GLenum pname, GLint v);
void q_glTexEnvf(GLenum target, GLenum pname, GLfloat v);
void q_glTexEnvfv(GLenum target, GLenum pname, const GLfloat *v);
void q_glFogi(GLenum pname, GLint v);
void q_glFogf(GLenum pname, GLfloat v);
void q_glFogfv(GLenum pname, const GLfloat *v);
void q_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void q_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void q_glDepthRange(rq_gldouble n, rq_gldouble f);
void q_glMatrixMode(GLenum m);
void q_glLoadMatrixf(const GLfloat *m);
void q_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void q_glClearDepth(rq_gldouble d);
void q_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void q_glClear(GLbitfield mask);
void q_glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void q_glBegin(GLenum mode);
void q_glEnd(void);
void q_glVertex2f(GLfloat x, GLfloat y);
void q_glTexCoord2f(GLfloat s, GLfloat t);
void q_glPixelStorei(GLenum pname, GLint v);
void q_glTexParameteri(GLenum target, GLenum pname, GLint v);
void q_glLightfv(GLenum light, GLenum pname, const GLfloat *v);
void q_glLightModelfv(GLenum pname, const GLfloat *v);
void q_glMaterialfv(GLenum face, GLenum pname, const GLfloat *v);
void q_glMaterialf(GLenum face, GLenum pname, GLfloat v);
void q_glColorMaterial(GLenum face, GLenum mode);
void q_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p);
void q_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *p);
void q_glNormalPointer(GLenum type, GLsizei stride, const void *p);
void q_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *p);
void q_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void q_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices);
void q_glGenTextures(GLsizei n, GLuint *names);
void q_glDeleteTextures(GLsizei n, const GLuint *names);
void q_glBindTexture(GLenum target, GLuint name);
void q_glGenBuffers(GLsizei n, GLuint *names);
void q_glDeleteBuffers(GLsizei n, const GLuint *names);
void q_glBindBuffer(GLenum target, GLuint name);
void q_glBufferData(GLenum target, size_t size, const void *data, GLenum usage);
void q_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt,
                    GLenum type, const void *pixels);
/* Same, for RGBA8 pixels in rq_data memory (not copied again). */
void q_glTexImage2D_owned(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt,
                          GLenum type, const void *pixels);
void q_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                       const void *pixels);
/* Synchronous: waits for the render thread. */
void q_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out);
#if defined(__vita__)
void q_glUseProgram(GLuint p);
void q_vglBufferData(GLenum target, const void *data);
void *q_vglForceAlloc(uint32_t size);
#else
void q_glLightf(GLenum light, GLenum pname, GLfloat v);
void q_glFogCoordPointer(GLenum type, GLsizei stride, const void *p);
void q_glFogCoordf(GLfloat f);
void q_glMultiTexCoord2f(GLenum unit, GLfloat s, GLfloat t);
#endif

#ifndef RQ_NO_REDIRECT
#define glEnable q_glEnable
#define glDisable q_glDisable
#define glEnableClientState q_glEnableClientState
#define glDisableClientState q_glDisableClientState
#define glBlendFunc q_glBlendFunc
#define glDepthFunc q_glDepthFunc
#define glDepthMask q_glDepthMask
#define glAlphaFunc q_glAlphaFunc
#define glCullFace q_glCullFace
#define glFrontFace q_glFrontFace
#define glShadeModel q_glShadeModel
#define glPolygonMode q_glPolygonMode
#define glActiveTexture q_glActiveTexture
#define glClientActiveTexture q_glClientActiveTexture
#define glTexEnvi q_glTexEnvi
#define glTexEnvf q_glTexEnvf
#define glTexEnvfv q_glTexEnvfv
#define glFogi q_glFogi
#define glFogf q_glFogf
#define glFogfv q_glFogfv
#define glColor4f q_glColor4f
#define glViewport q_glViewport
#define glDepthRange q_glDepthRange
#define glMatrixMode q_glMatrixMode
#define glLoadMatrixf q_glLoadMatrixf
#define glClearColor q_glClearColor
#define glClearDepth q_glClearDepth
#define glColorMask q_glColorMask
#define glClear q_glClear
#define glScissor q_glScissor
#define glBegin q_glBegin
#define glEnd q_glEnd
#define glVertex2f q_glVertex2f
#define glTexCoord2f q_glTexCoord2f
#define glPixelStorei q_glPixelStorei
#define glTexParameteri q_glTexParameteri
#define glLightfv q_glLightfv
#define glLightModelfv q_glLightModelfv
#define glMaterialfv q_glMaterialfv
#define glMaterialf q_glMaterialf
#define glColorMaterial q_glColorMaterial
#define glVertexPointer q_glVertexPointer
#define glColorPointer q_glColorPointer
#define glNormalPointer q_glNormalPointer
#define glTexCoordPointer q_glTexCoordPointer
#define glDrawArrays q_glDrawArrays
#define glDrawRangeElements q_glDrawRangeElements
#define glGenTextures q_glGenTextures
#define glDeleteTextures q_glDeleteTextures
#define glBindTexture q_glBindTexture
#define glGenBuffers q_glGenBuffers
#define glDeleteBuffers q_glDeleteBuffers
#define glBindBuffer q_glBindBuffer
#define glBufferData q_glBufferData
#define glTexImage2D q_glTexImage2D
#define glTexSubImage2D q_glTexSubImage2D
#define glReadPixels q_glReadPixels
#if defined(__vita__)
#define glUseProgram q_glUseProgram
#define vglBufferData q_vglBufferData
#define vglForceAlloc q_vglForceAlloc
#else
#define glLightf q_glLightf
#define glFogCoordPointer q_glFogCoordPointer
#define glFogCoordf q_glFogCoordf
#define glMultiTexCoord2f q_glMultiTexCoord2f
#endif
#endif

#endif
