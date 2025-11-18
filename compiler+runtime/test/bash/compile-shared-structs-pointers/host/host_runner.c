#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct
{
  int x;
  double y;
} shared_point;

typedef int (*jank_entrypoint_fn)(int, const char **);
typedef shared_point (*make_point_fn)(int, double);
typedef shared_point (*translate_point_fn)(shared_point, int, double);
typedef shared_point *(*point_ptr_id_fn)(shared_point *);

int main(int argc, const char **argv)
{
  if(argc != 2)
  {
    fprintf(stderr, "usage: %s <path-to-shared-lib>\n", argv[0]);
    return 2;
  }

  void *handle = dlopen(argv[1], RTLD_NOW);
  if(handle == NULL)
  {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 3;
  }

  // Initialize runtime
  jank_entrypoint_fn entry = (jank_entrypoint_fn)dlsym(handle, "jank_entrypoint");
  if(entry == NULL)
  {
    fprintf(stderr, "dlsym jank_entrypoint failed: %s\n", dlerror());
    dlclose(handle);
    return 4;
  }

  const char *args[] = { "shared-host", NULL };
  // Run entrypoint to initialize
  entry(1, args);

  make_point_fn make_point = (make_point_fn)dlsym(handle, "make_point");
  if(make_point == NULL)
  {
    fprintf(stderr, "dlsym make_point failed: %s\n", dlerror());
    dlclose(handle);
    return 5;
  }

  shared_point origin = make_point(4, 1.5);
  if(origin.x != 4 || fabs(origin.y - 1.5) > 1e-9)
  {
    fprintf(stderr, "make_point returned unexpected values (%d, %.3f)\n", origin.x, origin.y);
    dlclose(handle);
    return 6;
  }
  printf("make_point => (%d, %.1f)\n", origin.x, origin.y);

  translate_point_fn translate_point = (translate_point_fn)dlsym(handle, "translate_point");
  if(translate_point == NULL)
  {
    fprintf(stderr, "dlsym translate_point failed: %s\n", dlerror());
    dlclose(handle);
    return 7;
  }

  shared_point moved = translate_point(origin, 3, 2.0);
  if(moved.x != 7 || fabs(moved.y - 3.5) > 1e-9)
  {
    fprintf(stderr, "translate_point returned unexpected values (%d, %.3f)\n", moved.x, moved.y);
    dlclose(handle);
    return 8;
  }
  printf("translate_point => (%d, %.1f)\n", moved.x, moved.y);

  point_ptr_id_fn point_ptr_id = (point_ptr_id_fn)dlsym(handle, "point_ptr_id");
  if(point_ptr_id == NULL)
  {
    fprintf(stderr, "dlsym point_ptr_id failed: %s\n", dlerror());
    dlclose(handle);
    return 9;
  }

  shared_point sample = { .x = 10, .y = 42.0 };
  shared_point *roundtrip = point_ptr_id(&sample);
  if(roundtrip != &sample)
  {
    fprintf(stderr, "Pointer roundtrip failed\n");
    dlclose(handle);
    return 10;
  }
  printf("pointer roundtrip ok\n");

  dlclose(handle);
  return 0;
}
