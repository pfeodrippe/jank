#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*jank_entrypoint_fn)(int, const char **);
typedef int (*my_inc_fn)(int);

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

  // Call custom export
  my_inc_fn inc = (my_inc_fn)dlsym(handle, "my_inc");
  if(inc == NULL)
  {
    fprintf(stderr, "dlsym my_inc failed: %s\n", dlerror());
    dlclose(handle);
    return 5;
  }

  int result = inc(5);
  if(result != 6) {
      fprintf(stderr, "Expected 6, got %d\n", result);
      dlclose(handle);
      return 1;
  }

  printf("Success: %d\n", result);

  dlclose(handle);
  return 0;
}
