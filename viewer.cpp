// ============================================================
//  viewer.cpp — OpenGL-accelerated image viewer for Windows
//  Win32 + WIC + OpenGL (legacy fixed-function, no extra libs)
//
//  Build (MSVC):
//    cl viewer.cpp /O2 /W4 /MT /link opengl32.lib shell32.lib ole32.lib windowscodecs.lib
//
//  Keyboard: O=open  F/R=fit  1=100%  N=filter  +/-=zoom  Esc=quit
//  Mouse:    wheel=zoom  drag=pan  drop file=open
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wincodec.h>
#include <gl/GL.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <commdlg.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

// ── GL extension pointers ────────────────────────────────────
typedef void (APIENTRY *PFNGLGENERATEMIPMAPPROC)(GLenum);
typedef BOOL (WINAPI  *PFNWGLSWAPINTERVALEXTPROC)(int);
static PFNGLGENERATEMIPMAPPROC   glGenerateMipmap;
static PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;

#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE                  0x812F
#endif
#ifndef GL_BGRA_EXT
#  define GL_BGRA_EXT                       0x80E1
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#  define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#  define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

// ── Constants ────────────────────────────────────────────────
#define WINDOW_CLASS  L"WinViewerCls"
#define WINDOW_TITLE  L"Image Viewer"
#define TOOLBAR_H     44
#define TOOLBAR_PAD   8
#define BTN_H         28
#define BTN_GAP       8
#define ZOOM_STEP     1.12f
#define ZOOM_MIN      0.01f
#define ZOOM_MAX      1000.0f

enum { BTN_OPEN = 1, BTN_FIT, BTN_ACTUAL, BTN_NEAREST };
#define BTN_COUNT 4

// ── Types ────────────────────────────────────────────────────

// All OpenGL / window context state
struct GlState {
    HDC    dc;
    HGLRC  rc;
    HFONT  font;
    GLuint font_base;
    int    max_tex;     // GL_MAX_TEXTURE_SIZE
    bool   bgra;        // GL_EXT_bgra supported
    bool   aniso;       // GL_EXT_texture_filter_anisotropic supported
    float  max_aniso;
};

// Loaded image + view state
struct ImageState {
    GLuint tex;
    int    gpu_w, gpu_h;    // dimensions on GPU (may be downscaled)
    int    src_w, src_h;    // original decoded dimensions
    float  zoom, pan_x, pan_y;
    bool   mipmaps;
    bool   downscaled;      // GPU texture was rescaled from source
    bool   nearest;         // mag filter: nearest vs linear
};

// Toolbar button descriptor
struct Button {
    RECT          rect;
    int           id;
    const wchar_t *label;
};

// Mouse / drag state
struct InputState {
    bool  dragging, tracking;
    int   hot_id;
    int   drag_x0, drag_y0;
    float pan_x0,  pan_y0;
};

// ── Global state (grouped, minimized) ────────────────────────
static HWND       g_hwnd;
static GlState    g_gl;
static ImageState g_img;
static InputState g_in;
static IWICImagingFactory *g_wic;
static wchar_t    g_file[MAX_PATH];
static wchar_t    g_status[512];

static Button g_btns[BTN_COUNT] = {
    {{}, BTN_OPEN,    L"Open"},
    {{}, BTN_FIT,     L"Fit"},
    {{}, BTN_ACTUAL,  L"100%"},
    {{}, BTN_NEAREST, L"Nearest"},
};

// ── Misc helpers ─────────────────────────────────────────────
static inline void request_redraw() { InvalidateRect(g_hwnd, NULL, FALSE); }

// Extension string lookup (linear scan, called once per extension at init)
static bool gl_has_ext(const char *name) {
    const char *all = (const char *)glGetString(GL_EXTENSIONS);
    if (!all || !name) return false;
    size_t n = strlen(name);
    for (const char *p = all; (p = strstr(p, name)); p += n)
        if ((p == all || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return true;
    return false;
}

// ── Immediate-mode draw primitives ───────────────────────────
static void gl_col(unsigned char r, unsigned char g, unsigned char b) {
    glColor3f(r / 255.f, g / 255.f, b / 255.f);
}

static void draw_quad(float x, float y, float w, float h,
                      unsigned char r, unsigned char g, unsigned char b) {
    gl_col(r, g, b);
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x+w, y); glVertex2f(x+w, y+h); glVertex2f(x, y+h);
    glEnd();
}

static void draw_outline(float x, float y, float w, float h,
                         unsigned char r, unsigned char g, unsigned char b) {
    gl_col(r, g, b);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x+.5f, y+.5f);   glVertex2f(x+w-.5f, y+.5f);
    glVertex2f(x+w-.5f, y+h-.5f); glVertex2f(x+.5f, y+h-.5f);
    glEnd();
}

static void draw_text(float x, float y, const wchar_t *s,
                      unsigned char r, unsigned char g, unsigned char b) {
    if (!g_gl.font_base || !s || !*s) return;
    glPushAttrib(GL_LIST_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT);
    glDisable(GL_TEXTURE_2D);
    gl_col(r, g, b);
    glRasterPos2f(x, y + 14.f);
    glListBase(g_gl.font_base - 32);
    wchar_t buf[512]; int n = 0;
    while (s[n] && n < 511) { buf[n] = (s[n] >= 32 && s[n] < 256) ? s[n] : L'?'; n++; }
    buf[n] = 0;
    glCallLists(n, GL_UNSIGNED_SHORT, buf);
    glPopAttrib();
}

// ── View geometry ────────────────────────────────────────────
static void get_view(int *x, int *y, int *w, int *h) {
    RECT r; GetClientRect(g_hwnd, &r);
    *x = 0; *y = TOOLBAR_H;
    *w = r.right; *h = r.bottom - TOOLBAR_H;
    if (*h < 1) *h = 1;
}

// Scale that fits image inside the view at zoom=1
static float fit_scale(int vw, int vh) {
    if (!g_img.gpu_w || !g_img.gpu_h) return 1.f;
    return fminf((float)vw / g_img.gpu_w, (float)vh / g_img.gpu_h);
}

// Image position / size on screen, accounting for fit + zoom + pan
static void img_screen_rect(float *ox, float *oy, float *ow, float *oh) {
    int vx, vy, vw, vh; get_view(&vx, &vy, &vw, &vh);
    float s = fit_scale(vw, vh) * g_img.zoom;
    *ow = g_img.gpu_w * s; *oh = g_img.gpu_h * s;
    *ox = vx + (vw - *ow) * .5f + g_img.pan_x;
    *oy = vy + (vh - *oh) * .5f + g_img.pan_y;
}

// ── Image: filter, load, free ────────────────────────────────

// Apply current nearest/linear filter + mip state to bound texture
static void apply_filter() {
    glBindTexture(GL_TEXTURE_2D, g_img.tex);
    GLenum mag = g_img.nearest ? GL_NEAREST : GL_LINEAR;
    GLenum min = g_img.nearest ? (g_img.mipmaps ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST)
                               : (g_img.mipmaps ? GL_LINEAR_MIPMAP_LINEAR  : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
}

static void free_image() {
    if (g_img.tex) glDeleteTextures(1, &g_img.tex);
    g_img = {};
}

static void fit_dims(UINT sw, UINT sh, int lim, UINT *dw, UINT *dh) {
    *dw = sw; *dh = sh;
    if (lim <= 0 || ((int)sw <= lim && (int)sh <= lim)) return;
    double s = fmin((double)lim / sw, (double)lim / sh);
    *dw = (UINT)fmax(1.0, floor(sw * s));
    *dh = (UINT)fmax(1.0, floor(sh * s));
}

static bool load_image(const wchar_t *path) {
    if (!g_wic) return false;
    free_image();

    IWICBitmapDecoder     *dec = NULL;
    IWICBitmapFrameDecode *frm = NULL;
    IWICBitmapScaler      *scl = NULL;
    IWICFormatConverter   *cvt = NULL;
    IWICBitmapSource      *src = NULL;
    unsigned char *data = NULL;

    HRESULT hr = g_wic->CreateDecoderFromFilename(path, NULL, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frm);

    UINT sw = 0, sh = 0, w, h;
    if (SUCCEEDED(hr)) hr = frm->GetSize(&sw, &sh);
    fit_dims(sw, sh, g_gl.max_tex, &w, &h);
    bool scaled = (w != sw || h != sh);

    if (SUCCEEDED(hr) && scaled) {
        hr = g_wic->CreateBitmapScaler(&scl);
        if (SUCCEEDED(hr)) hr = scl->Initialize(frm, w, h, WICBitmapInterpolationModeFant);
        src = scl;
    } else src = frm;

    const GUID *fmt = g_gl.bgra ? &GUID_WICPixelFormat32bppBGRA
                                 : &GUID_WICPixelFormat32bppRGBA;
    if (SUCCEEDED(hr)) hr = g_wic->CreateFormatConverter(&cvt);
    if (SUCCEEDED(hr)) hr = cvt->Initialize(src, *fmt, WICBitmapDitherTypeNone,
                                             NULL, 0.0, WICBitmapPaletteTypeCustom);

    // Overflow guard before malloc
    if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > (UINT)(INT_MAX / (4 * (int)h))))
        hr = E_OUTOFMEMORY;

    UINT stride = w * 4, sz = stride * h;
    if (SUCCEEDED(hr) && !(data = (unsigned char *)malloc(sz))) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = cvt->CopyPixels(NULL, stride, sz, data);

    if (cvt) cvt->Release();
    if (scl) scl->Release();
    if (frm) frm->Release();
    if (dec) dec->Release();

    if (FAILED(hr)) {
        free(data);
        MessageBoxW(g_hwnd, L"Could not decode image.", L"Error", MB_ICONERROR | MB_OK);
        return false;
    }

    // Upload to GPU
    glGenTextures(1, &g_img.tex);
    glBindTexture(GL_TEXTURE_2D, g_img.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (int)w, (int)h, 0,
                 g_gl.bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, data);
    free(data);

    if (glGenerateMipmap) { glGenerateMipmap(GL_TEXTURE_2D); g_img.mipmaps = true; }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (g_gl.aniso)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_gl.max_aniso);
    apply_filter();

    g_img.gpu_w = (int)w;  g_img.gpu_h = (int)h;
    g_img.src_w = (int)sw; g_img.src_h = (int)sh;
    g_img.zoom  = 1.f;     g_img.pan_x = g_img.pan_y = 0.f;
    g_img.downscaled = scaled;
    return true;
}

// ── UI text ──────────────────────────────────────────────────
static void update_ui() {
    wchar_t title[512];
    if (g_img.tex) {
        // Show just filename in title, not full path
        const wchar_t *fname = wcsrchr(g_file, L'\\');
        fname = fname ? fname + 1 : g_file;
        swprintf(title, 512, L"%s — %s", WINDOW_TITLE, fname);

        const wchar_t *qual = g_img.mipmaps
            ? (g_img.nearest ? L"mip+nearest" : L"mip+linear")
            : (g_img.nearest ? L"nearest"     : L"linear");

        if (g_img.downscaled)
            swprintf(g_status, 512, L"%d×%d → %d×%d  |  %.0f%%  |  %s  |  GPU max %d",
                     g_img.src_w, g_img.src_h, g_img.gpu_w, g_img.gpu_h,
                     g_img.zoom * 100.f, qual, g_gl.max_tex);
        else
            swprintf(g_status, 512, L"%d×%d  |  %.0f%%  |  %s  |  GPU max %d",
                     g_img.gpu_w, g_img.gpu_h, g_img.zoom * 100.f, qual, g_gl.max_tex);
    } else {
        wcscpy(title, WINDOW_TITLE);
        wcscpy(g_status, L"Open or drop an image  ·  scroll=zoom  ·  drag=pan  ·  N=filter");
    }
    SetWindowTextW(g_hwnd, title);
}

// ── Commands ─────────────────────────────────────────────────
static void cmd_fit() {
    if (!g_img.tex) return;
    g_img.zoom = 1.f; g_img.pan_x = g_img.pan_y = 0.f;
    update_ui(); request_redraw();
}

static void cmd_actual() {
    if (!g_img.tex) return;
    int vx, vy, vw, vh; get_view(&vx, &vy, &vw, &vh);
    float fs = fit_scale(vw, vh);
    if (fs > 0.f) {
        g_img.zoom = 1.f / fs; g_img.pan_x = g_img.pan_y = 0.f;
        update_ui(); request_redraw();
    }
}

static void cmd_filter() {
    if (!g_img.tex) return;
    g_img.nearest = !g_img.nearest;
    apply_filter(); update_ui(); request_redraw();
}

static void cmd_zoom_step(float factor) {
    if (!g_img.tex) return;
    g_img.zoom = fminf(fmaxf(g_img.zoom * factor, ZOOM_MIN), ZOOM_MAX);
    update_ui(); request_redraw();
}

static void cmd_open() {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = L"Images\0*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.tiff;*.gif;*.ico\0All\0*.*\0\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle  = L"Open Image";
    if (GetOpenFileNameW(&ofn) && load_image(path)) {
        wcsncpy(g_file, path, MAX_PATH - 1);
        update_ui(); request_redraw();
    }
}

static void do_button(int id) {
    switch (id) {
    case BTN_OPEN:    cmd_open();   break;
    case BTN_FIT:     cmd_fit();    break;
    case BTN_ACTUAL:  cmd_actual(); break;
    case BTN_NEAREST: cmd_filter(); break;
    }
}

// ── Toolbar layout ───────────────────────────────────────────
static void layout_toolbar() {
    static const int W[BTN_COUNT] = {72, 52, 52, 72};
    int x = TOOLBAR_PAD, y = (TOOLBAR_H - BTN_H) / 2;
    for (int i = 0; i < BTN_COUNT; i++) {
        g_btns[i].rect = {x, y, x + W[i], y + BTN_H};
        x += W[i] + BTN_GAP;
    }
}

static int hit_btn(int mx, int my) {
    for (int i = 0; i < BTN_COUNT; i++) {
        RECT *r = &g_btns[i].rect;
        if (mx >= r->left && mx < r->right && my >= r->top && my < r->bottom)
            return g_btns[i].id;
    }
    return 0;
}

// ── Rendering ────────────────────────────────────────────────
static void draw_toolbar(int win_w) {
    glDisable(GL_TEXTURE_2D);
    draw_quad(0, 0, (float)win_w, TOOLBAR_H,       34, 34, 36);  // bar
    draw_quad(0, TOOLBAR_H-1, (float)win_w, 1.f,   58, 58, 62);  // separator

    for (int i = 0; i < BTN_COUNT; i++) {
        Button *b = &g_btns[i];
        float bx = (float)b->rect.left,  by = (float)b->rect.top;
        float bw = (float)(b->rect.right - b->rect.left);
        float bh = (float)(b->rect.bottom - b->rect.top);
        bool hot = g_in.hot_id == b->id;
        bool on  = b->id == BTN_NEAREST && g_img.nearest;  // toggle active state

        // hot > active(on) > normal
        unsigned char br = hot ? 67 : (on ? 58 : 49);
        unsigned char bg = hot ? 67 : (on ? 68 : 49);
        unsigned char bb = hot ? 72 : (on ? 84 : 54);
        draw_quad(bx, by, bw, bh, br, bg, bb);
        draw_outline(bx, by, bw, bh, hot ? 112 : 79, hot ? 112 : 79, hot ? 120 : 86);
        draw_text(bx + 8.f, by + 6.f, b->label, 235, 235, 238);
    }

    float sx = (float)g_btns[BTN_COUNT-1].rect.right + 16.f;
    draw_text(sx, 14.f, g_status, 180, 180, 186);
}

static void render() {
    RECT cr; GetClientRect(g_hwnd, &cr);
    int ww = cr.right, wh = cr.bottom;
    if (ww <= 0 || wh <= 0) return;

    glViewport(0, 0, ww, wh);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0.0, ww, wh, 0.0, -1.0, 1.0);  // Y=0 at top
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    // View area background
    glDisable(GL_TEXTURE_2D);
    draw_quad(0, TOOLBAR_H, (float)ww, (float)(wh - TOOLBAR_H), 14, 14, 15);

    if (g_img.tex) {
        float ix, iy, iw, ih; img_screen_rect(&ix, &iy, &iw, &ih);
        bool visible = ix < ww && (ix+iw) > 0 && iy < wh && (iy+ih) > TOOLBAR_H;
        if (visible) {
            // Scissor to image area; GL Y is flipped (0 = window bottom), so
            // the top toolbar [0..TOOLBAR_H] screen maps to [wh-TOOLBAR_H..wh] GL.
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, ww, wh - TOOLBAR_H);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, g_img.tex);
            glColor3f(1, 1, 1);
            glBegin(GL_QUADS);
            glTexCoord2f(0,0); glVertex2f(ix,    iy);
            glTexCoord2f(1,0); glVertex2f(ix+iw, iy);
            glTexCoord2f(1,1); glVertex2f(ix+iw, iy+ih);
            glTexCoord2f(0,1); glVertex2f(ix,    iy+ih);
            glEnd();
            glDisable(GL_SCISSOR_TEST);
        }
    }

    draw_toolbar(ww);
    SwapBuffers(g_gl.dc);
}

// ── OpenGL init / cleanup ────────────────────────────────────
static bool init_gl(HWND hwnd) {
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32; pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    g_gl.dc = GetDC(hwnd);
    int fmt = ChoosePixelFormat(g_gl.dc, &pfd);
    if (!fmt || !SetPixelFormat(g_gl.dc, fmt, &pfd)) return false;
    g_gl.rc = wglCreateContext(g_gl.dc);
    if (!g_gl.rc) return false;
    wglMakeCurrent(g_gl.dc, g_gl.rc);

    // Extensions — queried once here, stored in g_gl
    glGenerateMipmap   = (PFNGLGENERATEMIPMAPPROC)  wglGetProcAddress("glGenerateMipmap");
    wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC) wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(1);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_gl.max_tex);
    if (g_gl.max_tex < 1) g_gl.max_tex = 4096;

    g_gl.bgra  = gl_has_ext("GL_EXT_bgra");
    g_gl.aniso = gl_has_ext("GL_EXT_texture_filter_anisotropic");
    if (g_gl.aniso) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_gl.max_aniso);

    // Fixed-function defaults
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glClearColor(.055f, .055f, .059f, 1.f);

    // Bitmap font for toolbar text
    g_gl.font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (g_gl.font) {
        SelectObject(g_gl.dc, g_gl.font);
        if ((g_gl.font_base = glGenLists(224)))
            wglUseFontBitmapsW(g_gl.dc, 32, 224, g_gl.font_base);
    }
    return true;
}

static void cleanup_gl() {
    if (g_gl.font_base) glDeleteLists(g_gl.font_base, 224);
    if (g_gl.font)      DeleteObject(g_gl.font);
    if (g_gl.rc)      { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_gl.rc); }
    if (g_gl.dc)        ReleaseDC(g_hwnd, g_gl.dc);
    g_gl = {};
}

// ── Window procedure ─────────────────────────────────────────
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE:
        layout_toolbar();
        DragAcceptFiles(hwnd, TRUE);
        update_ui();
        if (!init_gl(hwnd)) {
            MessageBoxW(hwnd, L"OpenGL initialization failed.", L"Error", MB_ICONERROR|MB_OK);
            return -1;
        }
        return 0;

    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        render();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        layout_toolbar();
        if (g_gl.rc) request_redraw();
        return 0;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize = {400, 300};
        return 0;

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, MAX_PATH) && load_image(path)) {
            wcsncpy(g_file, path, MAX_PATH - 1);
            update_ui(); request_redraw();
        }
        DragFinish(drop);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int id = hit_btn(mx, my);
        if (id) {
            do_button(id);
        } else if (g_img.tex && my >= TOOLBAR_H) {
            g_in.dragging = true;
            g_in.drag_x0 = mx; g_in.drag_y0 = my;
            g_in.pan_x0 = g_img.pan_x; g_in.pan_y0 = g_img.pan_y;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (!g_in.tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd};
            g_in.tracking = TrackMouseEvent(&tme) != 0;
        }
        int next = hit_btn(mx, my);
        if (next != g_in.hot_id) { g_in.hot_id = next; request_redraw(); }
        if (g_in.dragging) {
            g_img.pan_x = g_in.pan_x0 + (mx - g_in.drag_x0);
            g_img.pan_y = g_in.pan_y0 + (my - g_in.drag_y0);
            request_redraw();
        }
        return 0;
    }

    case WM_LBUTTONUP:
        g_in.dragging = false;
        ReleaseCapture();
        return 0;

    case WM_MOUSELEAVE:
        g_in.tracking = false;
        if (g_in.hot_id) { g_in.hot_id = 0; request_redraw(); }
        return 0;

    case WM_MOUSEWHEEL: {
        if (!g_img.tex) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.y < TOOLBAR_H) return 0;

        // Anchor the pixel under the cursor during zoom
        float ox, oy, ow, oh; img_screen_rect(&ox, &oy, &ow, &oh);
        if (ow <= 0 || oh <= 0) return 0;
        float u = (pt.x - ox) / ow, v = (pt.y - oy) / oh;

        float factor = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? ZOOM_STEP : 1.f / ZOOM_STEP;
        float old_z  = g_img.zoom;
        g_img.zoom = fminf(fmaxf(g_img.zoom * factor, ZOOM_MIN), ZOOM_MAX);
        if (g_img.zoom == old_z) return 0;

        float nx, ny, nw, nh; img_screen_rect(&nx, &ny, &nw, &nh);
        g_img.pan_x += (float)pt.x - (nx + u * nw);
        g_img.pan_y += (float)pt.y - (ny + v * nh);
        update_ui(); request_redraw();
        return 0;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case 'O':            cmd_open();                    break;
        case 'F': case 'R':  cmd_fit();                     break;
        case '1':            cmd_actual();                  break;
        case 'N':            cmd_filter();                  break;
        case VK_OEM_PLUS:
        case VK_ADD:         cmd_zoom_step(ZOOM_STEP);      break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:    cmd_zoom_step(1.f/ZOOM_STEP);  break;
        case VK_ESCAPE:      DestroyWindow(hwnd);           break;
        }
        return 0;

    case WM_DESTROY:
        free_image();
        cleanup_gl();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry point ──────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, wchar_t *, int show_cmd) {
    HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(ci))
        CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&g_wic));

    WNDCLASSW wc      = {};
    wc.lpfnWndProc    = wnd_proc;
    wc.hInstance      = inst;
    wc.lpszClassName  = WINDOW_CLASS;
    wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.style          = CS_OWNDC;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE,
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1024, 768, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    // Auto-load from command line: viewer.exe "path\to\image.png"
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1 && load_image(argv[1])) {
        wcsncpy(g_file, argv[1], MAX_PATH - 1);
        update_ui();
    }
    LocalFree(argv);

    ShowWindow(g_hwnd, show_cmd);
    UpdateWindow(g_hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_wic) { g_wic->Release(); g_wic = NULL; }
    if (SUCCEEDED(ci)) CoUninitialize();
    return (int)msg.wParam;
}
