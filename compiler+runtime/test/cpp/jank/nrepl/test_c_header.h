/* Test header for C-style global functions.
 * Mimics headers like raylib.h where all functions are at global scope.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Window management functions - using raylib-style inline comments */
void InitWindow(int width, int height, const char* title);                       // Initialize window and OpenGL context
void CloseWindow(void);                                                          // Close window and unload OpenGL context
int WindowShouldClose(void);                                                     // Check if application should close

/* Drawing functions - using raylib-style inline comments */
void BeginDrawing(void);                                                         // Setup canvas to start drawing
void EndDrawing(void);                                                           // End canvas drawing and swap buffers
void ClearBackground(int color);                                                 // Set background color
void DrawRectangle(int posX, int posY, int width, int height, int color);        // Draw a color-filled rectangle
void DrawCircle(int centerX, int centerY, float radius, int color);              // Draw a color-filled circle
void DrawText(const char* text, int posX, int posY, int fontSize, int color);    // Draw text (using default font)

/* Input functions */
int IsKeyPressed(int key);                                                       // Check if a key has been pressed once
int IsKeyDown(int key);                                                          // Check if a key is being pressed
int GetMouseX(void);                                                             // Get mouse position X
int GetMouseY(void);                                                             // Get mouse position Y

/* A struct type */
typedef struct {
    float x;
    float y;
} Vector2;

typedef struct {
    float x;
    float y;
    float z;
} Vector3;

/* Functions using types */
Vector2 GetMousePosition(void);                                                  // Get mouse position XY
void DrawLineV(Vector2 start, Vector2 end, int color);                           // Draw a line (Vector version)

#ifdef __cplusplus
}
#endif
