/*
    Copyright 2012-2026, The AROS Development Team. All rights reserved.

    EGL driver backend for AROS — bridges EGL to mesa3dgl (GL/gla.h).
    Mesa 26.0.0 driver API (const _EGLDriver _eglDriver).

    Supports two platform types:
    - _EGL_PLATFORM_AROS:        Window-backed surfaces via glACreateContext
    - _EGL_PLATFORM_SURFACELESS: Offscreen pbuffer surfaces (no Window needed)
      Used by WebKit's PlatformDisplaySurfaceless for headless rendering.
*/

#include "eglsurface.h"
#include "eglcontext.h"
#include "eglconfig.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "egllog.h"

#include <stdlib.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <GL/gla.h>

#include <aros/debug.h>

/* GL library base — opened via gl.library (pass-through wrapper).
 * gl.library reads ENVARC:SYS/GL.default to find the versioned
 * mesa3dgl implementation (e.g. mesa3dgl26-0.library).            */
struct Library * GLBase = NULL;

/*
 * AROSMesa (gla.h) context creation is deferred to MakeCurrent because
 * it requires a Window pointer, which is only available from the surface.
 * For surfaceless displays, we use GLA_RastPort with an offscreen bitmap.
 */

/* A private render target for offscreen rendering: a bitmap in the default
 * public screen's format with a RastPort on it. GLA needs a screen (for the
 * pixel format) and a RastPort to render into; neither is ever shown. */
struct egl_arosmesa_offscreen
{
    struct Screen   * scr;
    struct BitMap   * bm;
    struct RastPort * rp;
};

struct egl_arosmesa_context
{
    _EGLContext base;
    GLAContext amesactx;
    struct Window * win;        /* window the GLA context renders to, if any */
    struct egl_arosmesa_offscreen surfaceless;  /* target with no surface */
};

static inline struct egl_arosmesa_context * egl_arosmesa_context(_EGLContext * ctx)
{
    return (struct egl_arosmesa_context *)ctx;
}

struct egl_arosmesa_surface
{
    _EGLSurface base;
    struct Window * win;        /* non-NULL for window surfaces */
    struct egl_arosmesa_offscreen offscreen;    /* pbuffer surfaces */
};

static void free_offscreen(struct egl_arosmesa_offscreen * off)
{
    if (off->rp)
        FreeVec(off->rp);
    if (off->bm)
        FreeBitMap(off->bm);
    if (off->scr)
        UnlockPubScreen(NULL, off->scr);
    off->rp = NULL;
    off->bm = NULL;
    off->scr = NULL;
}

static BOOL alloc_offscreen(struct egl_arosmesa_offscreen * off, LONG w, LONG h)
{
    struct BitMap * friend;

    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;

    off->scr = LockPubScreen(NULL);
    if (!off->scr)
        return FALSE;

    friend = off->scr->RastPort.BitMap;
    off->bm = AllocBitMap(w, h, GetBitMapAttr(friend, BMA_DEPTH),
                          BMF_CLEAR | BMF_MINPLANES, friend);
    off->rp = AllocVec(sizeof(struct RastPort), MEMF_PUBLIC | MEMF_CLEAR);
    if (!off->bm || !off->rp)
    {
        free_offscreen(off);
        return FALSE;
    }
    InitRastPort(off->rp);
    off->rp->BitMap = off->bm;

    return TRUE;
}

/* --- Helper: is this a surfaceless display? --- */
static inline EGLBoolean is_surfaceless(_EGLDisplay *disp)
{
    return disp->Platform == _EGL_PLATFORM_SURFACELESS;
}

/* --- Helper: GLA tags for rendering to a window --- */
static void window_attributes(struct Window * win, struct TagItem * attributes)
{
    int i = 0;

    attributes[i].ti_Tag = GLA_Window;      attributes[i++].ti_Data = (IPTR)win;
    attributes[i].ti_Tag = GLA_Left;        attributes[i++].ti_Data = win->BorderLeft;
    attributes[i].ti_Tag = GLA_Top;         attributes[i++].ti_Data = win->BorderTop;
    attributes[i].ti_Tag = GLA_Bottom;      attributes[i++].ti_Data = win->BorderBottom;
    attributes[i].ti_Tag = GLA_Right;       attributes[i++].ti_Data = win->BorderRight;
    attributes[i].ti_Tag = GLA_DoubleBuf;   attributes[i++].ti_Data = GL_TRUE;
    attributes[i].ti_Tag = GLA_RGBMode;     attributes[i++].ti_Data = GL_TRUE;
    attributes[i].ti_Tag = GLA_NoStencil;   attributes[i++].ti_Data = GL_TRUE;
    attributes[i].ti_Tag = GLA_NoAccum;     attributes[i++].ti_Data = GL_TRUE;
    attributes[i].ti_Tag = TAG_DONE;
}

/* --- Driver callbacks (Mesa 26 API: no _EGLDriver* parameter) --- */

static EGLBoolean
egl_arosmesa_terminate(_EGLDisplay *disp)
{
    _eglReleaseDisplayResources(disp);
    _eglCleanupDisplay(disp);

    disp->DriverData = NULL;

    return EGL_TRUE;
}

static EGLBoolean
egl_arosmesa_destroycontext(_EGLDisplay *dpy, _EGLContext *ctx)
{
    if (_eglPutContext(ctx))
    {
        struct egl_arosmesa_context * eglctx = egl_arosmesa_context(ctx);
        if (eglctx->amesactx)
        {
            glAMakeCurrent(NULL);
            glADestroyContext(eglctx->amesactx);
        }
        free_offscreen(&eglctx->surfaceless);
        free(eglctx);
    }

    return EGL_TRUE;
}

static EGLBoolean
egl_arosmesa_destroysurface(_EGLDisplay *disp, _EGLSurface *surf)
{
    if (_eglPutSurface(surf))
    {
        free_offscreen(&((struct egl_arosmesa_surface *)surf)->offscreen);
        free(surf);
    }

    return EGL_TRUE;
}

static EGLBoolean
egl_arosmesa_swapbuffers(_EGLDisplay *disp, _EGLSurface *draw)
{
    /* Surfaceless: no-op swap (no display to present to) */
    if (is_surfaceless(disp))
        return EGL_TRUE;

    if (disp->DriverData)
        glASwapBuffers(disp->DriverData);
    return EGL_TRUE;
}

static EGLBoolean
egl_arosmesa_makecurrent(_EGLDisplay *disp, _EGLSurface *dsurf,
        _EGLSurface *rsurf, _EGLContext *ctx)
{
    _EGLContext *old_ctx;
    _EGLSurface *old_dsurf, *old_rsurf;

    if (!_eglBindContext(ctx, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))
        return EGL_FALSE;

    /* Do nothing */
    if ((ctx == NULL) && (old_ctx == NULL))
        return EGL_TRUE;

    /* Unbind the current context */
    if ((ctx == NULL) && (old_ctx != NULL))
    {
        glAMakeCurrent(NULL);
        _eglPutSurface(old_dsurf);
        _eglPutSurface(old_rsurf);
        _eglPutContext(old_ctx);
        disp->DriverData = NULL;
        return EGL_TRUE;
    }

    /* Create when needed and bind new context */
    if (ctx != NULL)
    {
        struct egl_arosmesa_context * eglctx = egl_arosmesa_context(ctx);

        if (old_ctx != NULL)
        {
            _eglPutSurface(old_dsurf);
            _eglPutSurface(old_rsurf);
            _eglPutContext(old_ctx);
            disp->DriverData = NULL;
        }

        /* What the context renders to follows the surface being bound, not
         * the display's platform: an AROS-window display still binds
         * pbuffers, and EGL_KHR_surfaceless_context allows no surface at
         * all (a client's first MakeCurrent is often one of those). */
        struct Window * win = dsurf ?
            ((struct egl_arosmesa_surface *)dsurf)->win : NULL;

        if (eglctx->amesactx == NULL)
        {
            struct TagItem attributes[14];
            int i = 0;

            if (win == NULL)
            {
                /* Offscreen: render into the pbuffer's private RastPort, or
                 * a 1x1 one owned by the context when there is no surface */
                EGLint w = dsurf ? dsurf->Width  : 1;
                EGLint h = dsurf ? dsurf->Height : 1;
                struct egl_arosmesa_offscreen * off = dsurf ?
                    &((struct egl_arosmesa_surface *)dsurf)->offscreen :
                    &eglctx->surfaceless;

                if (!off->rp && !alloc_offscreen(off, w, h))
                    return EGL_FALSE;

                attributes[i].ti_Tag = GLA_Screen;      attributes[i++].ti_Data = (IPTR)off->scr;
                attributes[i].ti_Tag = GLA_RastPort;    attributes[i++].ti_Data = (IPTR)off->rp;
                attributes[i].ti_Tag = GLA_Width;       attributes[i++].ti_Data = (IPTR)w;
                attributes[i].ti_Tag = GLA_Height;      attributes[i++].ti_Data = (IPTR)h;
                attributes[i].ti_Tag = GLA_DoubleBuf;   attributes[i++].ti_Data = GL_FALSE;
                attributes[i].ti_Tag = GLA_RGBMode;     attributes[i++].ti_Data = GL_TRUE;
                attributes[i].ti_Tag = GLA_NoStencil;   attributes[i++].ti_Data = GL_TRUE;
                attributes[i].ti_Tag = GLA_NoAccum;     attributes[i++].ti_Data = GL_TRUE;
                attributes[i].ti_Tag = TAG_DONE;

                D(bug("[EGL] offscreen: glACreateContext %dx%d\n", w, h));
            }
            else
            {
                window_attributes(win, attributes);
            }

            eglctx->amesactx = glACreateContext(attributes);
            eglctx->win = win;
            D(bug("[EGL] glACreateContext = %p (window=%p)\n",
                  eglctx->amesactx, win));
        }
        else if (win != NULL && win != eglctx->win)
        {
            /* A context made current offscreen first, now bound to a window
             * surface: point its rendering at the window. Binding a window
             * context back to a pbuffer keeps the window target; offscreen
             * rendering goes to framebuffer objects anyway. */
            struct TagItem attributes[14];

            window_attributes(win, attributes);
            glASetRast(eglctx->amesactx, attributes);
            eglctx->win = win;
            D(bug("[EGL] glASetRast(%p, window=%p)\n", eglctx->amesactx, win));
        }

        if (eglctx->amesactx != NULL)
        {
            glAMakeCurrent(eglctx->amesactx);
            disp->DriverData = eglctx->amesactx;
            return EGL_TRUE;
        }
    }

    return EGL_FALSE;
}

static _EGLSurface *
egl_arosmesa_createwindowsurface(_EGLDisplay *disp, _EGLConfig *conf,
        void *native_window, const EGLint *attrib_list)
{
    EGLNativeWindowType window = (EGLNativeWindowType)native_window;
    struct egl_arosmesa_surface * surf = calloc(1, sizeof(struct egl_arosmesa_surface));

    if (!surf)
        return NULL;

    if (!_eglInitSurface(&surf->base, disp, EGL_WINDOW_BIT, conf, attrib_list, native_window))
    {
        free(surf);
        return NULL;
    }

    surf->base.Width = window->Width - window->BorderLeft - window->BorderRight;
    surf->base.Height = window->Height - window->BorderTop -  window->BorderBottom;
    surf->win = window;

    return &surf->base;
}

/* Pbuffer surface — used by surfaceless platform for offscreen rendering */
static _EGLSurface *
egl_arosmesa_createpbuffersurface(_EGLDisplay *disp, _EGLConfig *conf,
        const EGLint *attrib_list)
{
    struct egl_arosmesa_surface * surf = calloc(1, sizeof(struct egl_arosmesa_surface));

    if (!surf)
        return NULL;

    if (!_eglInitSurface(&surf->base, disp, EGL_PBUFFER_BIT, conf, attrib_list, NULL))
    {
        free(surf);
        return NULL;
    }

    /* Width/Height come from attrib_list (EGL_WIDTH/EGL_HEIGHT),
     * already parsed by _eglInitSurface into surf->base.Width/Height */
    surf->win = NULL;
    if (!is_surfaceless(disp) &&
        !alloc_offscreen(&surf->offscreen, surf->base.Width, surf->base.Height))
    {
        free(surf);
        return NULL;
    }

    D(bug("[EGL] pbuffer surface %dx%d created\n",
          surf->base.Width, surf->base.Height));

    return &surf->base;
}

static _EGLContext *
egl_arosmesa_createcontext(_EGLDisplay *disp, _EGLConfig *conf,
        _EGLContext *share_list, const EGLint *attrib_list)
{
    struct egl_arosmesa_context * ctx = calloc(1, sizeof(struct egl_arosmesa_context));

    if (!ctx)
        return NULL;

    if (!_eglInitContext(&ctx->base, disp, conf, share_list, attrib_list))
    {
        free(ctx);
        return NULL;
    }

    ctx->amesactx = NULL; /* Deferred to MakeCurrent */

    return &ctx->base;
}

static void create_configs(_EGLDisplay *dpy)
{
    _EGLConfig * cfg = calloc(1, sizeof(_EGLConfig));
    _eglInitConfig(cfg, dpy, 1);

    cfg->RenderableType = EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT;
    cfg->Conformant = EGL_OPENGL_BIT;

    if (is_surfaceless(dpy))
        cfg->SurfaceType = EGL_PBUFFER_BIT;
    else
        cfg->SurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;

    _eglSetConfigKey(cfg, EGL_RED_SIZE, 8);
    _eglSetConfigKey(cfg, EGL_GREEN_SIZE, 8);
    _eglSetConfigKey(cfg, EGL_BLUE_SIZE, 8);
    _eglSetConfigKey(cfg, EGL_ALPHA_SIZE, 8);
    /* The colour buffer's total depth: clients ask for it (EGL_BUFFER_SIZE
       is matched as "at least"), and _eglInitConfig leaves it 0. */
    _eglSetConfigKey(cfg, EGL_BUFFER_SIZE, 32);
    _eglSetConfigKey(cfg, EGL_DEPTH_SIZE, 24);
    _eglSetConfigKey(cfg, EGL_STENCIL_SIZE, 8);

    _eglLinkConfig(cfg);
}

static EGLBoolean
egl_arosmesa_initialize(_EGLDisplay *dpy)
{
    D(bug("[EGL] Initialize: platform=%d (AROS=%d, surfaceless=%d)\n",
          dpy->Platform, _EGL_PLATFORM_AROS, _EGL_PLATFORM_SURFACELESS));

    if (!GLBase)
    {
        GLBase = OpenLibrary("gl.library", 20L);
        D(bug("[EGL] OpenLibrary(\"gl.library\", 20) = %p\n", GLBase));
    }

    if (!GLBase)
    {
        _eglLog(_EGL_WARNING, "egl_arosmesa: cannot open gl.library");
        return EGL_FALSE;
    }

    create_configs(dpy);

    dpy->ClientAPIs |= EGL_OPENGL_BIT | EGL_OPENGL_ES_BIT;
    dpy->Version = 14; /* EGL 1.4 */

    /* Extensions supported by our Mesa/softpipe backend */
    dpy->Extensions.KHR_config_attribs = EGL_TRUE;
    dpy->Extensions.KHR_create_context = EGL_TRUE;
    dpy->Extensions.KHR_create_context_no_error = EGL_TRUE;
    dpy->Extensions.KHR_surfaceless_context = EGL_TRUE;
    dpy->Extensions.KHR_image_base = EGL_TRUE;

    D(bug("[EGL] display initialized: platform=%d surfaceless=%d\n",
          dpy->Platform, is_surfaceless(dpy)));

    return EGL_TRUE;
}

/* Mesa 26.0.0: single const global driver struct */
const _EGLDriver _eglDriver = {
    .Initialize          = egl_arosmesa_initialize,
    .Terminate           = egl_arosmesa_terminate,
    .CreateContext        = egl_arosmesa_createcontext,
    .DestroyContext       = egl_arosmesa_destroycontext,
    .MakeCurrent          = egl_arosmesa_makecurrent,
    .CreateWindowSurface  = egl_arosmesa_createwindowsurface,
    .CreatePbufferSurface = egl_arosmesa_createpbuffersurface,
    .DestroySurface       = egl_arosmesa_destroysurface,
    .SwapBuffers          = egl_arosmesa_swapbuffers,
};

static VOID CloseMesa()
{
    if (GLBase)
    {
        CloseLibrary(GLBase);
        GLBase = NULL;
    }
}

ADD2EXPUNGELIB(CloseMesa, 5)
