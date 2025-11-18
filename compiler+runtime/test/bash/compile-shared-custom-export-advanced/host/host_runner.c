#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef int (*jank_entrypoint_fn)(int, const char **);
typedef bool (*my_is_true_fn)(bool);
typedef long (*my_add_three_fn)(long, long, long);
typedef double (*my_mixed_fn)(int, double, int);

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

  // Call custom export: bool
  my_is_true_fn is_true = (my_is_true_fn)dlsym(handle, "my_is_true");
  if(is_true == NULL)
  {
    fprintf(stderr, "dlsym my_is_true failed: %s\n", dlerror());
    dlclose(handle);
    return 5;
  }
  bool res_bool = is_true(true);
  if(res_bool != true) {
      fprintf(stderr, "Expected true, got %d\n", res_bool);
      dlclose(handle);
      return 1;
  }

  // Call custom export: long
  my_add_three_fn add_three = (my_add_three_fn)dlsym(handle, "my_add_three");
  if(add_three == NULL)
  {
    fprintf(stderr, "dlsym my_add_three failed: %s\n", dlerror());
    dlclose(handle);
    return 6;
  }
  long res_long = add_three(10L, 20L, 30L);
  if(res_long != 60L) {
      fprintf(stderr, "Expected 60, got %ld\n", res_long);
      dlclose(handle);
      return 1;
  }

  // Call custom export: mixed types
  my_mixed_fn mixed = (my_mixed_fn)dlsym(handle, "my_mixed");
  if(mixed == NULL)
  {
    fprintf(stderr, "dlsym my_mixed failed: %s\n", dlerror());
    dlclose(handle);
    return 7;
  }
  double res_mixed = mixed(5, 2.5, 3);
  if(res_mixed != 10.5) {
      fprintf(stderr, "Expected 10.5, got %f\n", res_mixed);
      dlclose(handle);
      return 1;
  }

  printf("Bool test: PASS\n");
  printf("Long test: PASS\n");
  printf("Mixed test: PASS\n");

  dlclose(handle);
  return 0;
}
