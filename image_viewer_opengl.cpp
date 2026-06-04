#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wincodec.h>
#include <gl/GL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")


typedef void (APIENTRY *PFNGLGENERATEMIPMAPPROC)(GLenum target);
PFNGLGENERATEMIPMAPPROC glGenerateMipmap = NULL;
typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int interval);
PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = NULL;



#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#define WINDOW_CLASS L"ImageViewerClass"
#define MAX_PATH_LEN 260
#define BTN_OPEN_ID 1001
#define BTN_FIT_ID 1002
#define BTN_ACTUAL_ID 1003
#define TOOLBAR_HEIGHT 44
#define TOOLBAR_PAD 8
#define BTN_HEIGHT 28

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

typedef struct {
    int width;
    int height;
    int channels;
    float zoom;
    float pan_x;  
    float pan_y;
    GLuint texture_id;
    bool has_mipmaps;
} Image;

Image current_image = {0};
wchar_t current_file[MAX_PATH_LEN] = {0};
HWND hwnd_main = NULL;
HDC hdc_gl = NULL;
HGLRC hglrc = NULL;
HFONT font_ui = NULL;
GLuint font_base = 0;

static bool is_dragging = false;
static bool tracking_mouse = false;
static int hot_button_id = 0;
int drag_start_x = 0;
int drag_start_y = 0;
float drag_start_pan_x = 0;
float drag_start_pan_y = 0;

typedef struct {
    RECT rect;
    int id;
    const wchar_t *label;
} UiButton;

UiButton toolbar_buttons[3] = {
    {{0, 0, 0, 0}, BTN_OPEN_ID, L"Open Image"},
    {{0, 0, 0, 0}, BTN_FIT_ID, L"Fit"},
    {{0, 0, 0, 0}, BTN_ACTUAL_ID, L"100%"}
};
wchar_t status_text[512] = L"Open an image or drop one here. Mouse wheel zooms, drag pans.";

static void request_render() {
    if (hwnd_main) {
        InvalidateRect(hwnd_main, NULL, FALSE);
    }
}

static void set_color(unsigned char r, unsigned char g, unsigned char b) {
    glColor3f(r / 255.0f, g / 255.0f, b / 255.0f);
}

static void draw_rect(float x, float y, float w, float h,
                      unsigned char r, unsigned char g, unsigned char b) {
    set_color(r, g, b);
    glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x, y + h);
    glEnd();
}

static void draw_outline(float x, float y, float w, float h,
                         unsigned char r, unsigned char g, unsigned char b) {
    set_color(r, g, b);
    glBegin(GL_LINE_LOOP);
        glVertex2f(x + 0.5f, y + 0.5f);
        glVertex2f(x + w - 0.5f, y + 0.5f);
        glVertex2f(x + w - 0.5f, y + h - 0.5f);
        glVertex2f(x + 0.5f, y + h - 0.5f);
    glEnd();
}

static void draw_text(float x, float y, const wchar_t *text,
                      unsigned char r, unsigned char g, unsigned char b) {
    if (!font_base || !text || !*text) return;

    glPushAttrib(GL_LIST_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT);
    glDisable(GL_TEXTURE_2D);
    set_color(r, g, b);
    glRasterPos2f(x, y + 14.0f);
    glListBase(font_base - 32);

    int count = 0;
    wchar_t ascii_text[512];
    while (text[count] && count < 511) {
        ascii_text[count] = (text[count] >= 32 && text[count] < 256) ? text[count] : L'?';
        count++;
    }
    ascii_text[count] = L'\0';
    glCallLists(count, GL_UNSIGNED_SHORT, ascii_text);
    glPopAttrib();
}

static int hit_test_button(int x, int y) {
    for (int i = 0; i < 3; i++) {
        RECT r = toolbar_buttons[i].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return toolbar_buttons[i].id;
    }
    return 0;
}

static bool has_gl_extension(const char *name) {
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extensions || !name || !*name) return false;

    const char *start = extensions;
    size_t name_len = strlen(name);
    while ((start = strstr(start, name)) != NULL) {
        const char *end = start + name_len;
        if ((start == extensions || start[-1] == ' ') && (*end == ' ' || *end == '\0'))
            return true;
        start = end;
    }
    return false;
}

static void get_view_rect(int *x, int *y, int *w, int *h) {
    RECT client;
    GetClientRect(hwnd_main, &client);
    *x = 0;
    *y = TOOLBAR_HEIGHT;
    *w = client.right - client.left;
    *h = (client.bottom - client.top) - TOOLBAR_HEIGHT;
    if (*h < 1) *h = 1;
}

static float get_fit_scale(int view_w, int view_h) {
    if (!current_image.width || !current_image.height || view_w <= 0 || view_h <= 0)
        return 1.0f;
    return fminf((float)view_w / current_image.width,
                 (float)view_h / current_image.height);
}

static void get_image_rect(float *x, float *y, float *w, float *h) {
    int view_x, view_y, view_w, view_h;
    get_view_rect(&view_x, &view_y, &view_w, &view_h);
    float scale = get_fit_scale(view_w, view_h) * current_image.zoom;

    *w = current_image.width * scale;
    *h = current_image.height * scale;
    *x = view_x + ((float)view_w - *w) * 0.5f + current_image.pan_x;
    *y = view_y + ((float)view_h - *h) * 0.5f + current_image.pan_y;
}

void free_image() {
    if (current_image.texture_id) {
        glDeleteTextures(1, &current_image.texture_id);
        current_image.texture_id = 0;
    }
    current_image.width = 0;
    current_image.height = 0;
    current_image.channels = 0;
    current_image.has_mipmaps = false;
}

bool init_opengl(HWND hwnd) {
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR),
        1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA,
        32,  
        0, 0, 0, 0, 0, 0,
        0, 0, 0,
        0, 0, 0, 0,
        24,  
        8,   
        0,
        PFD_MAIN_PLANE,
        0, 0, 0, 0
    };
    
    hdc_gl = GetDC(hwnd);
    int pixel_format = ChoosePixelFormat(hdc_gl, &pfd);
    if (!pixel_format) return false;
    
    if (!SetPixelFormat(hdc_gl, pixel_format, &pfd)) return false;
    
    hglrc = wglCreateContext(hdc_gl);
    if (!hglrc) return false;
    
    wglMakeCurrent(hdc_gl, hglrc);
    glGenerateMipmap = (PFNGLGENERATEMIPMAPPROC)wglGetProcAddress("glGenerateMipmap");
    wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(1);
    
    
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glClearColor(0.08f, 0.08f, 0.085f, 1.0f);
    
    
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

    font_ui = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (font_ui) {
        SelectObject(hdc_gl, font_ui);
        font_base = glGenLists(224);
        if (font_base) {
            wglUseFontBitmapsW(hdc_gl, 32, 224, font_base);
        }
    }
    
    return true;
}

void cleanup_opengl() {
    if (font_base) {
        glDeleteLists(font_base, 224);
        font_base = 0;
    }
    if (font_ui) {
        DeleteObject(font_ui);
        font_ui = NULL;
    }
    if (hglrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(hglrc);
        hglrc = NULL;
    }
    if (hdc_gl) {
        ReleaseDC(hwnd_main, hdc_gl);
        hdc_gl = NULL;
    }
}

bool load_image(const wchar_t *filename) {
    free_image();

    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    unsigned char *image_data = NULL;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        MessageBoxW(hwnd_main, L"Could not initialize the Windows image decoder.", L"Load Error", MB_OK | MB_ICONERROR);
        return false;
    }

    hr = factory->CreateDecoderFromFilename(filename, NULL, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr))
        hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, NULL, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&width, &height);

    if (SUCCEEDED(hr)) {
        if (width == 0 || height == 0 || width > (UINT)(INT_MAX / 4) ||
            height > (UINT)(INT_MAX / (width * 4))) {
            hr = E_OUTOFMEMORY;
        }
    }

    UINT stride = width * 4;
    UINT image_size = stride * height;
    if (SUCCEEDED(hr)) {
        image_data = (unsigned char *)malloc(image_size);
        if (!image_data)
            hr = E_OUTOFMEMORY;
    }

    if (SUCCEEDED(hr))
        hr = converter->CopyPixels(NULL, stride, image_size, image_data);

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();

    if (FAILED(hr)) {
        if (image_data) free(image_data);
        MessageBoxW(hwnd_main, L"Could not decode the selected image.", L"Load Error", MB_OK | MB_ICONERROR);
        return false;
    }

    current_image.width = (int)width;
    current_image.height = (int)height;
    current_image.channels = 4;
    current_image.zoom = 1.0f;
    current_image.pan_x = 0.0f;
    current_image.pan_y = 0.0f;
    
    
    glGenTextures(1, &current_image.texture_id);
    glBindTexture(GL_TEXTURE_2D, current_image.texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, current_image.width, current_image.height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
    
    free(image_data);
    image_data = NULL;
    
    
    if (glGenerateMipmap) {
        glGenerateMipmap(GL_TEXTURE_2D);
        current_image.has_mipmaps = true;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        current_image.has_mipmaps = false;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    
    if (has_gl_extension("GL_EXT_texture_filter_anisotropic")) {
        float max_aniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
        if (max_aniso > 1.0f) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_aniso);
    }
    
    return true;
}

void draw_toolbar(int win_width) {
    glDisable(GL_TEXTURE_2D);
    draw_rect(0.0f, 0.0f, (float)win_width, (float)TOOLBAR_HEIGHT, 34, 34, 36);
    draw_rect(0.0f, (float)TOOLBAR_HEIGHT - 1.0f, (float)win_width, 1.0f, 58, 58, 62);

    for (int i = 0; i < 3; i++) {
        UiButton *button = &toolbar_buttons[i];
        int bw = button->rect.right - button->rect.left;
        int bh = button->rect.bottom - button->rect.top;
        bool hot = hot_button_id == button->id;
        draw_rect((float)button->rect.left, (float)button->rect.top, (float)bw, (float)bh,
                  hot ? 67 : 49, hot ? 67 : 49, hot ? 72 : 54);
        draw_outline((float)button->rect.left, (float)button->rect.top, (float)bw, (float)bh,
                     hot ? 112 : 79, hot ? 112 : 79, hot ? 120 : 86);
        draw_text((float)button->rect.left + 11.0f, (float)button->rect.top + 6.0f,
                  button->label, 235, 235, 238);
    }

    draw_text((float)toolbar_buttons[2].rect.right + 16.0f, 14.0f,
              status_text, 198, 198, 204);
}

void render_frame() {
    RECT client;
    GetClientRect(hwnd_main, &client);
    int win_width = client.right;
    int win_height = client.bottom;
    if (win_width <= 0 || win_height <= 0) return;
    
    glViewport(0, 0, win_width, win_height);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, win_width, win_height, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_TEXTURE_2D);
    draw_rect(0.0f, (float)TOOLBAR_HEIGHT, (float)win_width,
              (float)(win_height - TOOLBAR_HEIGHT), 14, 14, 15);

    if (!current_image.texture_id) {
        draw_toolbar(win_width);
        SwapBuffers(hdc_gl);
        return;
    }

    float img_x, img_y, img_w, img_h;
    get_image_rect(&img_x, &img_y, &img_w, &img_h);

    int view_height = win_height - TOOLBAR_HEIGHT;
    if (view_height < 1) view_height = 1;

    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, win_width, view_height);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, current_image.texture_id);
    glColor3f(1.0f, 1.0f, 1.0f);
    
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(img_x, img_y);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(img_x + img_w, img_y);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(img_x + img_w, img_y + img_h);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(img_x, img_y + img_h);
    glEnd();

    glDisable(GL_SCISSOR_TEST);
    draw_toolbar(win_width);
    SwapBuffers(hdc_gl);
}

void update_title() {
    wchar_t title[512];
    swprintf(title, 512, L"Image Viewer - %s", current_file[0] ? current_file : L"No image");
    SetWindowTextW(hwnd_main, title);

    if (current_image.texture_id) {
        swprintf(status_text, 512, L"%dx%d  |  %.0f%%  |  %s",
                 current_image.width, current_image.height, current_image.zoom * 100.0f,
                 current_image.has_mipmaps ? L"mipmaps" : L"linear");
    } else {
        swprintf(status_text, 512, L"Open an image or drop one here. Mouse wheel zooms, drag pans.");
    }
}

void reset_view() {
    if (!current_image.texture_id) return;
    current_image.zoom = 1.0f;
    current_image.pan_x = 0.0f;
    current_image.pan_y = 0.0f;
    update_title();
    request_render();
}

void actual_size_view() {
    if (!current_image.texture_id) return;
    int view_x, view_y, view_w, view_h;
    get_view_rect(&view_x, &view_y, &view_w, &view_h);
    float fit_scale = get_fit_scale(view_w, view_h);
    if (fit_scale > 0.0f) {
        current_image.zoom = 1.0f / fit_scale;
        current_image.pan_x = 0.0f;
        current_image.pan_y = 0.0f;
        update_title();
        request_render();
    }
}

void layout_controls(HWND hwnd) {
    int x = TOOLBAR_PAD;
    int y = (TOOLBAR_HEIGHT - BTN_HEIGHT) / 2;
    int widths[3] = {94, 72, 72};

    for (int i = 0; i < 3; i++) {
        toolbar_buttons[i].rect.left = x;
        toolbar_buttons[i].rect.top = y;
        toolbar_buttons[i].rect.right = x + widths[i];
        toolbar_buttons[i].rect.bottom = y + BTN_HEIGHT;
        x += widths[i] + 8;
    }
}

bool open_current_file(const wchar_t *filename) {
    if (load_image(filename)) {
        wcsncpy(current_file, filename, MAX_PATH_LEN - 1);
        current_file[MAX_PATH_LEN - 1] = L'\0';
        update_title();
        request_render();
        return true;
    }
    return false;
}

bool open_file_dialog(wchar_t *filename, int max_len) {
    OPENFILENAMEW ofn = {0};
    wchar_t filter[] = L"Image Files\0*.jpg;*.jpeg;*.png;*.bmp;*.tga;*.gif\0All Files\0*.*\0\0";
    
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hwnd_main;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = max_len;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"Open Image";
    
    return GetOpenFileNameW(&ofn);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        layout_controls(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        update_title();
        if (!init_opengl(hwnd)) {
            MessageBoxA(hwnd, "Failed to initialize OpenGL", "Error", MB_OK);
            return -1;
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        render_frame();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wparam) == BTN_OPEN_ID) {
            wchar_t selected_file[MAX_PATH_LEN] = {0};
            if (open_file_dialog(selected_file, MAX_PATH_LEN)) {
                open_current_file(selected_file);
            }
        } else if (LOWORD(wparam) == BTN_FIT_ID) {
            reset_view();
        } else if (LOWORD(wparam) == BTN_ACTUAL_ID) {
            actual_size_view();
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wparam;
        wchar_t dropped_file[MAX_PATH_LEN] = {0};
        if (DragQueryFileW(drop, 0, dropped_file, MAX_PATH_LEN)) {
            open_current_file(dropped_file);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mouse_x = GET_X_LPARAM(lparam);
        int mouse_y = GET_Y_LPARAM(lparam);
        int button_id = hit_test_button(mouse_x, mouse_y);
        if (button_id == BTN_OPEN_ID) {
            SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(BTN_OPEN_ID, 0), 0);
        } else if (button_id == BTN_FIT_ID) {
            reset_view();
        } else if (button_id == BTN_ACTUAL_ID) {
            actual_size_view();
        } else if (current_image.texture_id && mouse_y >= TOOLBAR_HEIGHT) {
            is_dragging = true;
            drag_start_x = mouse_x;
            drag_start_y = mouse_y;
            drag_start_pan_x = current_image.pan_x;
            drag_start_pan_y = current_image.pan_y;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int current_x = GET_X_LPARAM(lparam);
        int current_y = GET_Y_LPARAM(lparam);
        if (!tracking_mouse) {
            TRACKMOUSEEVENT tme = {0};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tracking_mouse = TrackMouseEvent(&tme) != 0;
        }

        int next_hot = hit_test_button(current_x, current_y);
        if (next_hot != hot_button_id) {
            hot_button_id = next_hot;
            request_render();
        }

        if (is_dragging && current_image.texture_id) {
            current_image.pan_x = drag_start_pan_x + (current_x - drag_start_x);
            current_image.pan_y = drag_start_pan_y + (current_y - drag_start_y);
            request_render();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_mouse = false;
        if (hot_button_id) {
            hot_button_id = 0;
            request_render();
        }
        return 0;
    case WM_LBUTTONUP: {
        is_dragging = false;
        ReleaseCapture();
        request_render();
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        if (!current_image.texture_id)
            return 0;

        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(hwnd, &pt);
        if (pt.y < TOOLBAR_HEIGHT)
            return 0;

        float old_x, old_y, old_w, old_h;
        get_image_rect(&old_x, &old_y, &old_w, &old_h);
        if (old_w <= 0.0f || old_h <= 0.0f)
            return 0;

        float u = ((float)pt.x - old_x) / old_w;
        float v = ((float)pt.y - old_y) / old_h;

        int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        float old_zoom = current_image.zoom;
        current_image.zoom *= (delta > 0) ? 1.12f : (1.0f / 1.12f);
        current_image.zoom = fminf(fmaxf(current_image.zoom, 0.01f), 1000.0f);
        if (current_image.zoom == old_zoom)
            return 0;

        float new_x, new_y, new_w, new_h;
        get_image_rect(&new_x, &new_y, &new_w, &new_h);

        current_image.pan_x += (float)pt.x - (new_x + u * new_w);
        current_image.pan_y += (float)pt.y - (new_y + v * new_h);

        update_title();
        request_render();
        return 0;
    }
    case WM_KEYDOWN: {
        if (wparam == 'O') {
            SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(BTN_OPEN_ID, 0), 0);
        } else if (wparam == 'R' || wparam == 'F') {
            reset_view();
        } else if (wparam == '1') {
            actual_size_view();
        }
        return 0;
    }
    case WM_SIZE: {
        layout_controls(hwnd);
        if (hglrc) {
            request_render();
        }
        return 0;
    }
    case WM_DESTROY:
        free_image();
        cleanup_opengl();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, wchar_t *cmd_line, int show_cmd) {
    HRESULT coinit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.style = CS_OWNDC;  
    
    RegisterClassW(&wc);
    
    hwnd_main = CreateWindowExW(0, WINDOW_CLASS, L"Image Viewer (OpenGL Accelerated)",
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
                               NULL, NULL, instance, NULL);
    
    ShowWindow(hwnd_main, show_cmd);
    UpdateWindow(hwnd_main);
    
    MSG msg = {0};
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(coinit)) {
        CoUninitialize();
    }

    return (int)msg.wParam;
}
