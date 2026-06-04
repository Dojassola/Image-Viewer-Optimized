# Image-Viewer-Optimized

A small Win32 image viewer rendered with OpenGL.

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
