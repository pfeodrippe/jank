#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*jank_entrypoint_fn)(int, const char **);
typedef double (*my_complex_fn)(int, double, int);
typedef int* (*my_array_ptr_fn)(int*, int);

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
  my_complex_fn complex = (my_complex_fn)dlsym(handle, "my_complex");
  if(complex == NULL)
  {
    fprintf(stderr, "dlsym my_complex failed: %s\n", dlerror());
    dlclose(handle);
    return 5;
  }

  double result = complex(10, 2.5, 20);
  if(result != 32.5) {
      fprintf(stderr, "Expected 32.5, got %f\n", result);
      dlclose(handle);
      return 1;
  }

  printf("Success: %f\n", result);

  // Call array ptr
  my_array_ptr_fn array_ptr = (my_array_ptr_fn)dlsym(handle, "my_array_ptr");
  if(array_ptr == NULL)
  {
    fprintf(stderr, "dlsym my_array_ptr failed: %s\n", dlerror());
    dlclose(handle);
    return 6;
  }

  int my_arr[] = {1, 2, 3};
  int* ret_ptr = array_ptr(my_arr, 3);
  if(ret_ptr != my_arr) {
      fprintf(stderr, "Expected ptr %p, got %p\n", (void*)my_arr, (void*)ret_ptr);
      dlclose(handle);
      return 1;
  }
  printf("Array ptr success\n");

  dlclose(handle);
  return 0;
}
