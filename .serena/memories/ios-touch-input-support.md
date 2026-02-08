# iOS Touch Input Support for SDF Viewer

## Problem
The sdf_engine.hpp only handled mouse events (SDL_EVENT_MOUSE_*), not touch events. On iOS:
- UI buttons worked via SDL's touch-to-mouse hint
- Camera rotation didn't work because it requires `mousePressed=true` during drag

## Solution

### 1. Enable touch-to-mouse conversion (sdf_viewer_ios.mm)
```cpp
// Before calling sdfx::init()
SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
```
This helps ImGui buttons receive tap events.

### 2. Add explicit touch event handling (sdf_engine.hpp)
In the `poll_events_to_buffer()` function's switch statement:

```cpp
// Touch/finger events for iOS - treat as mouse drag for camera rotation
// SDL provides dx/dy in normalized coordinates (-1 to 1)
case SDL_EVENT_FINGER_DOWN:
    e->mousePressed = true;
    e->lastMouseX = event.tfinger.x;
    e->lastMouseY = event.tfinger.y;
    ed.mouse_button = SDL_BUTTON_LEFT;
    ed.mouse_x = event.tfinger.x * g_framebufferWidth;
    ed.mouse_y = event.tfinger.y * g_framebufferHeight;
    break;

case SDL_EVENT_FINGER_UP:
    e->mousePressed = false;
    ed.mouse_button = SDL_BUTTON_LEFT;
    ed.mouse_x = event.tfinger.x * g_framebufferWidth;
    ed.mouse_y = event.tfinger.y * g_framebufferHeight;
    break;

case SDL_EVENT_FINGER_MOTION:
    {
        // dx/dy from SDL are normalized deltas - scale for natural rotation
        float dx = event.tfinger.dx * 500.0f;  // Horizontal rotation
        float dy = event.tfinger.dy * 500.0f;  // Vertical tilt
        e->camera.update(dx, dy, 0);
        e->dirty = true;
        e->lastMouseX = event.tfinger.x;
        e->lastMouseY = event.tfinger.y;
        ed.mouse_x = event.tfinger.x * g_framebufferWidth;
        ed.mouse_y = event.tfinger.y * g_framebufferHeight;
        ed.mouse_xrel = dx;
        ed.mouse_yrel = dy;
    }
    break;
```

## Key Notes
- Touch coordinates (tfinger.x/y) are normalized 0-1, must multiply by framebuffer size
- Use `g_framebufferWidth` / `g_framebufferHeight` global variables
- Camera rotation is handled directly in FINGER_MOTION for immediate response
