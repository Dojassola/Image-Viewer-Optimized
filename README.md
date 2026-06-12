# Tiny-Image-Viewer

A small Win32 image viewer rendered with OpenGL.

## Current optimization level

The viewer is optimized for lightweight local viewing:

- GPU accelerated pan/zoom through OpenGL texture rendering.
- Event-driven repainting instead of a continuous render loop.
- Mipmapped textures for faster and cleaner zoomed-out rendering.
- WIC-based native Windows decoding with one global decoder factory.
- BGRA upload path when `GL_EXT_bgra` is available, reducing channel conversion work.
- GPU texture-size detection through `GL_MAX_TEXTURE_SIZE`.
- Automatic WIC downscale before upload when an image is larger than the GPU texture limit.
- Scissor clipping keeps image rendering out of the toolbar area.
- Simple offscreen culling skips texture draw calls when the image is fully outside the viewport.

The app is still intentionally simple: it does not yet use tiled rendering, async loading, persistent caches, or a modern shader/VBO pipeline. See [OPTIMIZATION_PLAN.md](OPTIMIZATION_PLAN.md).

## Build

```powershell
g++ image_viewer_opengl.cpp -o ImageViewer.exe -municode -mwindows -lopengl32 -lshell32 -lole32 -lwindowscodecs -lgdi32 -lcomdlg32
```

## Controls

- Open an image with the toolbar button, `O`, or drag and drop.
- Mouse wheel zooms around the cursor.
- Drag the image to pan.
- `Fit`, `F`, or `R` resets the view.
- `100%` or `1` shows the image at actual pixel size.
