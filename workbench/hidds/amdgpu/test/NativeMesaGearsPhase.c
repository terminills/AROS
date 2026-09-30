/* Copyright 2026 The AROS Development Team. SPDX-License-Identifier: MIT
 * Bounded diagnostics around the unchanged contrib Mesa gears program.
 * No private driver, fake renderer, synthetic frame, or GL behavior replacement.
 * Keep each admitted ENTER/LEAVE pair even when the trace budget is exhausted.
 */
#include <GL/gl.h>
#include <GL/glut.h>
#include <aros/debug.h>
#include <stdarg.h>
#include <stdio.h>

static unsigned phase_sequence;
static unsigned phase_vertices;

static unsigned phase_begin(const char *name)
{
    unsigned ticket = ++phase_sequence;
    if (ticket <= 256)
        bug("[GEARSPHASE-v1] %u ENTER %s vertices=%u\n", ticket, name, phase_vertices);
    else if (ticket == 257)
        bug("[GEARSPHASE-v1] trace budget exhausted; later calls are not logged\n");
    return ticket <= 256 ? ticket : 0;
}

static void phase_end(unsigned ticket, const char *name)
{
    if (ticket)
        bug("[GEARSPHASE-v1] %u LEAVE %s vertices=%u\n", ticket, name, phase_vertices);
}

static void phase_glutInit(int *argc, char **argv)
{
    unsigned t = phase_begin("glutInit");
    glutInit(argc, argv);
    phase_end(t, "glutInit");
}

static int phase_glutCreateWindow(const char *name)
{
    unsigned t = phase_begin("glutCreateWindow");
    int result = glutCreateWindow(name);
    if (t) bug("[GEARSPHASE-v1] window=%d\n", result);
    phase_end(t, "glutCreateWindow");
    return result;
}

static GLuint phase_glGenLists(GLsizei range)
{
    unsigned t = phase_begin("glGenLists");
    GLuint result = glGenLists(range);
    if (t) bug("[GEARSPHASE-v1] list=%u range=%d\n", (unsigned)result, (int)range);
    phase_end(t, "glGenLists");
    return result;
}

static void phase_glNewList(GLuint list, GLenum mode)
{
    unsigned t = phase_begin("glNewList");
    glNewList(list, mode);
    phase_end(t, "glNewList");
}

#define PHASE_VOID0(name) static void phase_##name(void) { \
    unsigned t = phase_begin(#name); name(); phase_end(t, #name); }
PHASE_VOID0(glEndList)
PHASE_VOID0(glEnd)
PHASE_VOID0(glutSwapBuffers)
PHASE_VOID0(glutMainLoop)

static void phase_glBegin(GLenum mode)
{
    unsigned t = phase_begin("glBegin");
    glBegin(mode);
    phase_end(t, "glBegin");
}

static void phase_glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    unsigned t = 0;
    ++phase_vertices;
    if (phase_vertices <= 4096 && (phase_vertices % 128) == 0)
        t = phase_begin("glVertex3f/128");
    glVertex3f(x, y, z);
    phase_end(t, "glVertex3f/128");
}

static void phase_glClear(GLbitfield mask)
{
    unsigned t = phase_begin("glClear");
    glClear(mask);
    phase_end(t, "glClear");
}

static void phase_glCallList(GLuint list)
{
    unsigned t = phase_begin("glCallList");
    glCallList(list);
    phase_end(t, "glCallList");
}

static const GLubyte *phase_glGetString(GLenum name)
{
    unsigned t = phase_begin("glGetString");
    const GLubyte *result = glGetString(name);
    if (t) bug("[GEARSPHASE-v1] string enum=%x pointer=%p\n", (unsigned)name, result);
    phase_end(t, "glGetString");
    return result;
}

static void phase_glutDestroyWindow(int win)
{
    unsigned t = phase_begin("glutDestroyWindow");
    glutDestroyWindow(win);
    phase_end(t, "glutDestroyWindow");
}

static int phase_printf(const char *format, ...)
{
    unsigned t = phase_begin("printf");
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    phase_end(t, "printf");
    return result;
}

/* Includes below see already-declared GL/GLUT prototypes. Only calls within
 * the original gears program are redirected to the transparent wrappers. */
#define glutInit phase_glutInit
#define glutCreateWindow phase_glutCreateWindow
#define glGenLists phase_glGenLists
#define glNewList phase_glNewList
#define glEndList phase_glEndList
#define glBegin phase_glBegin
#define glEnd phase_glEnd
#define glVertex3f phase_glVertex3f
#define glClear phase_glClear
#define glCallList phase_glCallList
#define glGetString phase_glGetString
#define glutSwapBuffers phase_glutSwapBuffers
#define glutMainLoop phase_glutMainLoop
#define glutDestroyWindow phase_glutDestroyWindow
#define printf phase_printf
#define main stock_gears_main
#include "../../../../contrib/Demo/GL/MesaGL/gears.c"
#undef main

int main(int argc, char **argv)
{
    bug("[GEARSPHASE-v1] begin unchanged stock geometry; installed GL/GLUT; argc=%d\n", argc);
    int result = stock_gears_main(argc, argv);
    bug("[GEARSPHASE-v1] main returned=%d vertices=%u\n", result, phase_vertices);
    return result;
}
