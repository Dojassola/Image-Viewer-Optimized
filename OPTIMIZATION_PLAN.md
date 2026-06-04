# Image Viewer Optimization Plan

## Current Baseline

The current viewer is a lightweight Win32/OpenGL app. It now uses native WIC decoding, GPU texture rendering, mipmaps, event-driven repainting, toolbar clipping, GPU texture limit detection, and pre-upload downscaling for images that exceed `GL_MAX_TEXTURE_SIZE`.

This is a good baseline for common images, but it is not yet a full high-performance large-image engine.

## Completed Optimizations

- Render images as OpenGL textures instead of CPU/GDI blits.
- Use mipmaps for better zoomed-out quality and texture sampling performance.
- Keep rendering event-driven, avoiding a constant frame loop when nothing changes.
- Draw the toolbar inside OpenGL to avoid Win32 child-control/backbuffer conflicts.
- Clip image rendering below the toolbar with `glScissor`.
- Use a global WIC factory instead of recreating decoder infrastructure per load.
- Prefer WIC `32bppBGRA` plus `GL_EXT_bgra` when supported.
- Detect `GL_MAX_TEXTURE_SIZE`.
- Downscale oversized images through WIC before CPU allocation and GPU upload.
- Skip image draw calls when pan/zoom moves the image fully outside the viewport.

## Next Step 1: Async Loading

Goal: keep the UI responsive while opening large files.

Tasks:

- Move WIC decode work to a worker thread.
- Keep OpenGL texture creation on the UI/render thread.
- Add loading/cancel state in the toolbar.
- Ignore stale worker results when the user opens a different image before the previous decode finishes.

Expected impact:

- Large images no longer freeze the window during decode.
- The app feels much more responsive.

## Next Step 2: Texture Limit Handling With Better UX

Goal: make oversized-image handling explicit and predictable.

Tasks:

- Show original dimensions and uploaded texture dimensions clearly.
- Add a warning/status label when a file was downscaled for GPU compatibility.
- Preserve source dimensions separately from render dimensions.
- Consider a "fit to source pixels" mode once tiled rendering exists.

Expected impact:

- Users understand when the image is rendered from a reduced GPU texture.

## Next Step 3: Tiled Rendering

Goal: handle huge images without forcing the whole image into one CPU buffer and one GPU texture.

Tasks:

- Divide large images into fixed-size tiles, such as 512x512 or 1024x1024.
- Decode/upload only visible tiles.
- Keep a small LRU cache of recently visible tiles.
- Load lower-resolution tiles first for quick previews.

Expected impact:

- Much lower peak memory usage.
- Better handling of very large images.
- Faster initial display for massive files.

## Next Step 4: Modern OpenGL Path

Goal: replace legacy immediate mode while keeping fallback compatibility if needed.

Tasks:

- Use VBOs for quad geometry.
- Use shaders for textured rendering.
- Use an orthographic matrix uniform instead of fixed-function matrices.
- Keep the current simple path as a fallback if modern OpenGL setup fails.

Expected impact:

- Cleaner GPU pipeline.
- Better compatibility with modern drivers.
- Easier future effects, color transforms, and overlays.

## Next Step 5: Image Cache And Navigation

Goal: make folder browsing fast.

Tasks:

- Track images in the current folder.
- Preload metadata for next/previous images.
- Cache recently decoded or uploaded images.
- Add next/previous keyboard shortcuts.

Expected impact:

- Faster browsing through image folders.

## Priority Recommendation

Implement async loading first. It gives the biggest visible improvement without requiring the larger architectural work of tiled rendering.
