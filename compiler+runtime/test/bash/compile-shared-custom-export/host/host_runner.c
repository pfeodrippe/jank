#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*jank_entrypoint_fn)(int, const char **);
typedef int (*my_inc_fn)(int);
typedef double (*my_add_double_fn)(double, double);
typedef int (*my_str_len_fn)(const char*);
typedef void (*my_void_fn)(void);
typedef long (*my_apply_cb_fn)(long);
typedef long (*my_apply_fn)(my_apply_cb_fn, long);
typedef my_apply_cb_fn (*my_return_fn_fn)(my_apply_cb_fn);

static long host_square(long x)
{
  return x * x;
}

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

  // Call custom export: int
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

  // Call custom export: double
  my_add_double_fn add_d = (my_add_double_fn)dlsym(handle, "my_add_double");
  if(add_d == NULL)
  {
    fprintf(stderr, "dlsym my_add_double failed: %s\n", dlerror());
    dlclose(handle);
    return 6;
  }
  double res_d = add_d(1.5, 2.5);
  if(res_d != 4.0) {
      fprintf(stderr, "Expected 4.0, got %f\n", res_d);
      dlclose(handle);
      return 1;
  }

  // Call custom export: string
  my_str_len_fn str_len = (my_str_len_fn)dlsym(handle, "my_str_len");
  if(str_len == NULL)
  {
    fprintf(stderr, "dlsym my_str_len failed: %s\n", dlerror());
    dlclose(handle);
    return 7;
  }
  int len = str_len("hello");
  if(len != 5) {
      fprintf(stderr, "Expected 5, got %d\n", len);
      dlclose(handle);
      return 1;
  }

  // Call custom export: void
  my_void_fn void_fn = (my_void_fn)dlsym(handle, "my_void");
  if(void_fn == NULL)
  {
    fprintf(stderr, "dlsym my_void failed: %s\n", dlerror());
    dlclose(handle);
    return 8;
  }
  void_fn();

  printf("Success: %d\n", result);
  printf("Void test passed\n");

  my_apply_fn my_apply = (my_apply_fn)dlsym(handle, "my_apply");
  if(my_apply == NULL)
  {
    fprintf(stderr, "dlsym my_apply failed: %s\n", dlerror());
    dlclose(handle);
    return 9;
  }

  long apply_result = my_apply(host_square, 3);
  if(apply_result != host_square(3))
  {
    fprintf(stderr, "Expected %ld, got %ld\n", host_square(3), apply_result);
    dlclose(handle);
    return 1;
  }

  printf("Callback test passed\n");

  my_return_fn_fn return_fn = (my_return_fn_fn)dlsym(handle, "my_return_fn");
  if(return_fn == NULL)
  {
    fprintf(stderr, "dlsym my_return_fn failed: %s\n", dlerror());
    dlclose(handle);
    return 10;
  }

  my_apply_cb_fn returned = return_fn(host_square);
  if(returned == NULL)
  {
    fprintf(stderr, "Returned callback was NULL\n");
    dlclose(handle);
    return 1;
  }

  if(returned(4) != host_square(4))
  {
    fprintf(stderr, "Returned callback produced unexpected result of (%ld)\n", returned(4));
    dlclose(handle);
    return 1;
  }

  printf("Return fn test passed\n");

  typedef long (*my_long_fn)(long);
  typedef my_long_fn (*my_return_jank_fn_fn)(void);

  my_return_jank_fn_fn return_jank_fn = (my_return_jank_fn_fn)dlsym(handle, "my_return_jank_fn");
  if(return_jank_fn == NULL)
  {
    fprintf(stderr, "dlsym my_return_jank_fn failed: %s\n", dlerror());
    dlclose(handle);
    return 11;
  }

  my_long_fn jank_fn = return_jank_fn();
  if(jank_fn == NULL)
  {
    fprintf(stderr, "Returned jank function was NULL\n");
    dlclose(handle);
    return 1;
  }

  long jank_res = jank_fn(5);
  if(jank_res != 20) // 5 * 4
  {
    fprintf(stderr, "Expected 20, got %ld\n", jank_res);
    dlclose(handle);
    return 1;
  }

  printf("Return jank fn test passed\n");

  dlclose(handle);
  return 0;
}
