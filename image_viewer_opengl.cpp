#include <windows.h>
#include <windowsx.h>
#include <gl/GL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#pragma comment(lib, "opengl32.lib")


#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif
#ifndef GLsizeiptr
#define GLsizeiptr ptrdiff_t
#endif


typedef void (APIENTRY *PFNGLGENERATEMIPMAPPROC)(GLenum target);
PFNGLGENERATEMIPMAPPROC glGenerateMipmap = NULL;
typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int interval);
PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = NULL;


typedef void (APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei n, GLuint *buffers);
PFNGLGENBUFFERSPROC glGenBuffers = NULL;
typedef void (APIENTRY *PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);
PFNGLBINDBUFFERPROC glBindBuffer = NULL;
typedef void (APIENTRY *PFNGLBUFFERDATAPROC)(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
PFNGLBUFFERDATAPROC glBufferData = NULL;
typedef GLvoid* (APIENTRY *PFNGLMAPBUFFERPROC)(GLenum target, GLenum access);
PFNGLMAPBUFFERPROC glMapBuffer = NULL;
typedef GLboolean (APIENTRY *PFNGLUNMAPBUFFERPROC)(GLenum target);
PFNGLUNMAPBUFFERPROC glUnmapBuffer = NULL;
typedef void (APIENTRY *PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint *buffers);
PFNGLDELETEBUFFERSPROC glDeleteBuffers = NULL;



#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "lib/stb_image.h"

#define WINDOW_CLASS L"ImageViewerClass"
#define MAX_PATH_LEN 260
#define BTN_OPEN_ID 1001

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
HWND hwnd_btn_open = NULL;
HDC hdc_gl = NULL;
HGLRC hglrc = NULL;

static bool is_dragging = false;
int drag_start_x = 0;
int drag_start_y = 0;
float drag_start_pan_x = 0;
float drag_start_pan_y = 0;

void free_image() {
    if (current_image.texture_id) {
        glDeleteTextures(1, &current_image.texture_id);
        current_image.texture_id = 0;
    }
    current_image.width = 0;
    current_image.height = 0;
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
    
    
    glGenBuffers = (PFNGLGENBUFFERSPROC)wglGetProcAddress("glGenBuffers");
    glBindBuffer = (PFNGLBINDBUFFERPROC)wglGetProcAddress("glBindBuffer");
    glBufferData = (PFNGLBUFFERDATAPROC)wglGetProcAddress("glBufferData");
    glMapBuffer = (PFNGLMAPBUFFERPROC)wglGetProcAddress("glMapBuffer");
    glUnmapBuffer = (PFNGLUNMAPBUFFERPROC)wglGetProcAddress("glUnmapBuffer");
    glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)wglGetProcAddress("glDeleteBuffers");
    
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(0);
    
    
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    
    
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    
    return true;
}

void cleanup_opengl() {
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

bool load_image(const char *filename) {
    free_image();
    
    
    unsigned char *image_data = stbi_load(filename, &current_image.width, &current_image.height, 
                                          &current_image.channels, STBI_rgb_alpha);
    
    if (!image_data) {
        MessageBoxA(hwnd_main, stbi_failure_reason(), "Load Error", MB_OK | MB_ICONERROR);
        return false;
    }
    
    
    current_image.zoom = 1.0f;
    current_image.pan_x = 0.0f;
    current_image.pan_y = 0.0f;
    
    
    glGenTextures(1, &current_image.texture_id);
    glBindTexture(GL_TEXTURE_2D, current_image.texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, current_image.width, current_image.height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
    
    
    stbi_image_free(image_data);
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
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    
    
    float max_aniso = 0.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
    if (max_aniso > 1.0f) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_aniso);
    
    return true;
}

void render_frame() {
    if (!current_image.texture_id) return;
    
    RECT client;
    GetClientRect(hwnd_main, &client);
    int win_width = client.right;
    int win_height = client.bottom;
    
    glViewport(0, 0, win_width, win_height);
    glClear(GL_COLOR_BUFFER_BIT);
    
    
    float img_aspect = (float)current_image.width / current_image.height;
    float win_aspect = (float)win_width / win_height;
    
    float scale_x, scale_y;
    if (win_aspect > img_aspect) {
        scale_y = current_image.zoom;
        scale_x = (img_aspect / win_aspect) * current_image.zoom;
    } else {
        scale_x = current_image.zoom;
        scale_y = (win_aspect / img_aspect) * current_image.zoom;
    }
    
    
    float pan_x_norm = (2.0f * current_image.pan_x) / win_width;
    float pan_y_norm = -(2.0f * current_image.pan_y) / win_height;
    
    
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(pan_x_norm, pan_y_norm, 0.0f);
    glScalef(scale_x, scale_y, 1.0f);
    
    
    glBindTexture(GL_TEXTURE_2D, current_image.texture_id);
    glColor3f(1.0f, 1.0f, 1.0f);
    
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
    glEnd();
    
    
    SwapBuffers(hdc_gl);
}

void update_title() {
    wchar_t title[512];
    swprintf(title, 512, L"Image Viewer (OpenGL) - %s | Zoom: %.2fx | %dx%d %s", 
             current_file, current_image.zoom, current_image.width, current_image.height,
             current_image.has_mipmaps ? L"[Mipmapped]" : L"");
    SetWindowTextW(hwnd_main, title);
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
        hwnd_btn_open = CreateWindowW(L"BUTTON", L"Open Image",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      10, 10, 100, 30,
                                      hwnd, (HMENU)(intptr_t)BTN_OPEN_ID, 
                                      GetModuleHandleW(NULL), NULL);
        if (!init_opengl(hwnd)) {
            MessageBoxA(hwnd, "Failed to initialize OpenGL", "Error", MB_OK);
            return -1;
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        render_frame();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wparam) == BTN_OPEN_ID) {
            if (open_file_dialog(current_file, MAX_PATH_LEN)) {
                char ansi_file[MAX_PATH_LEN];
                WideCharToMultiByte(CP_UTF8, 0, current_file, -1, ansi_file, MAX_PATH_LEN, NULL, NULL);
                if (load_image(ansi_file)) {
                    update_title();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (current_image.texture_id) {
            is_dragging = true;
            drag_start_x = GET_X_LPARAM(lparam);
            drag_start_y = GET_Y_LPARAM(lparam);
            drag_start_pan_x = current_image.pan_x;
            drag_start_pan_y = current_image.pan_y;
            if (wglSwapIntervalEXT) wglSwapIntervalEXT(1);
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (is_dragging && current_image.texture_id) {
            int current_x = GET_X_LPARAM(lparam);
            int current_y = GET_Y_LPARAM(lparam);
            current_image.pan_x = drag_start_pan_x + (current_x - drag_start_x);
            current_image.pan_y = drag_start_pan_y + (current_y - drag_start_y);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        is_dragging = false;
        ReleaseCapture();
        if (wglSwapIntervalEXT) wglSwapIntervalEXT(0);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        if (!current_image.texture_id)
            return 0;

        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(hwnd, &pt);

        RECT rc;
        GetClientRect(hwnd, &rc);
        float win_w = (float)rc.right;
        float win_h = (float)rc.bottom;

        float base_scale = fminf(win_w / current_image.width,
                                win_h / current_image.height);
        if (base_scale <= 0.0f)
            base_scale = 1.0f;

        float img_aspect = (float)current_image.width / current_image.height;
        float win_aspect = win_w / win_h;

        float aspect_x = 1.0f;
        float aspect_y = 1.0f;

        if (win_aspect > img_aspect)
            aspect_x = img_aspect / win_aspect;
        else
            aspect_y = win_aspect / img_aspect;

        float old_scale_x = base_scale * current_image.zoom * aspect_x;
        float old_scale_y = base_scale * current_image.zoom * aspect_y;

        float old_w = current_image.width  * old_scale_x;
        float old_h = current_image.height * old_scale_y;

        float old_x = (win_w - old_w) * 0.5f + current_image.pan_x;
        float old_y = (win_h - old_h) * 0.5f + current_image.pan_y;

        float u = (pt.x - old_x) / old_scale_x;
        float v = (pt.y - old_y) / old_scale_y;

        int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        current_image.zoom *= (delta > 0) ? 1.1f : (1.0f / 1.1f);
        current_image.zoom = fminf(fmaxf(current_image.zoom, 0.01f), 1000.0f);

        float new_scale_x = base_scale * current_image.zoom * aspect_x;
        float new_scale_y = base_scale * current_image.zoom * aspect_y;

        float new_w = current_image.width  * new_scale_x;
        float new_h = current_image.height * new_scale_y;

        float new_x_centered = (win_w - new_w) * 0.5f;
        float new_y_centered = (win_h - new_h) * 0.5f;

        current_image.pan_x = pt.x - new_x_centered - u * new_scale_x;
        current_image.pan_y = pt.y - new_y_centered - v * new_scale_y;

        update_title();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        if (wparam == 'O') {
            SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(BTN_OPEN_ID, 0), 0);
        } else if (wparam == 'R' || wparam == 'F') {
            if (current_image.texture_id) {
                current_image.zoom = 1.0f;
                current_image.pan_x = 0.0f;
                current_image.pan_y = 0.0f;
                update_title();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }
    case WM_SIZE: {
        if (hglrc) {
            InvalidateRect(hwnd, NULL, FALSE);
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
        
        
        if (is_dragging && hglrc) {
            render_frame();
            unsigned long long frame_time = GetTickCount64() % 16;
            if (frame_time < 16) Sleep(16 - frame_time);
        }
    }
    
    return (int)msg.wParam;
}
