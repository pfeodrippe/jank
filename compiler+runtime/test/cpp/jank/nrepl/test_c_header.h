/* Test header for C-style global functions.
 * Mimics headers like raylib.h where all functions are at global scope.
 */
#pragma once

/* Macro for compound literals (like raylib's CLITERAL) */
#ifndef CLITERAL
  #ifdef __cplusplus
    #define CLITERAL(type) type
  #else
    #define CLITERAL(type) (type)
  #endif
#endif

/* Color type for testing compound literal macros */
typedef struct Color
{
  unsigned char r;
  unsigned char g;
  unsigned char b;
  unsigned char a;
} Color;

/* Test macros - mimicking raylib color constants */
#define TEST_WHITE CLITERAL(Color){ 255, 255, 255, 255 } // White color
#define TEST_BLACK CLITERAL(Color){ 0, 0, 0, 255 } // Black color
#define TEST_RED CLITERAL(Color){ 255, 0, 0, 255 } // Red color
#define TEST_GREEN CLITERAL(Color){ 0, 255, 0, 255 } // Green color
#define TEST_BLUE CLITERAL(Color){ 0, 0, 255, 255 } // Blue color

/* Simple constant macros */
#define TEST_PI 3.14159f // Pi constant
#define TEST_MAX_VALUE 1000 // Maximum value
#define KEY_ESCAPE 256 // Escape key code

/* Function-like macros (like ecs_new_w_pair in flecs) */
#define TEST_ADD(a, b) ((a) + (b)) // Add two values
#define TEST_MUL(a, b) ((a) * (b)) // Multiply two values
#define TEST_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x))) // Clamp value

/* Simple inline functions for testing nested calls in macros */
static inline int test_get_five(void)
{
  return 5;
}

static inline int test_get_ten(void)
{
  return 10;
}

static inline int test_double(int x)
{
  return x * 2;
}

/* Functions that return pointers for testing cpp/box scenarios */
static inline int *test_get_int_ptr(void)
{
  static int value = 42;
  return &value;
}

/* Macro that takes a pointer argument (like ecs_new_w_pair) */
#define TEST_PTR_ADD(ptr, val) (*(ptr) + (val))

#ifdef __cplusplus
extern "C"
{
#endif

  /* Window management functions - using raylib-style inline comments */
  void InitWindow(int width, int height, char const *title); // Initialize window and OpenGL context
  void CloseWindow(void); // Close window and unload OpenGL context
  int WindowShouldClose(void); // Check if application should close

  /* Drawing functions - using raylib-style inline comments */
  void BeginDrawing(void); // Setup canvas to start drawing
  void EndDrawing(void); // End canvas drawing and swap buffers
  void ClearBackground(int color); // Set background color
  void DrawRectangle(int posX,
                     int posY,
                     int width,
                     int height,
                     int color); // Draw a color-filled rectangle
  void DrawCircle(int centerX, int centerY, float radius, int color); // Draw a color-filled circle
  void DrawText(char const *text,
                int posX,
                int posY,
                int fontSize,
                int color); // Draw text (using default font)

  /* Input functions */
  int IsKeyPressed(int key); // Check if a key has been pressed once
  int IsKeyDown(int key); // Check if a key is being pressed
  int GetMouseX(void); // Get mouse position X
  int GetMouseY(void); // Get mouse position Y

  /* Variadic functions (like printf, ImGui::Text) */
  void TraceLog(int logLevel, char const *text, ...); // Show trace log messages

#ifdef __cplusplus
  /* Functions with default parameters (C++ only) */
  void SetConfigFlags(unsigned int flags = 0); // Setup init configuration flags
  void DrawTextEx(char const *text,
                  int posX = 0,
                  int posY = 0,
                  int fontSize = 20,
                  int color = 0); // Draw text with defaults
#endif

  /* A struct type */
  typedef struct
  {
    float x;
    float y;
  } Vector2;

  typedef struct
  {
    float x;
    float y;
    float z;
  } Vector3;

  /* Functions using types */
  Vector2 GetMousePosition(void); // Get mouse position XY
  void DrawLineV(Vector2 start, Vector2 end, int color); // Draw a line (Vector version)

#ifdef __cplusplus
}
#endif
