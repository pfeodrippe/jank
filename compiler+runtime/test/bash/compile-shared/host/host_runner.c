#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*jank_entrypoint_fn)(int, const char **);

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

  jank_entrypoint_fn entry = (jank_entrypoint_fn)dlsym(handle, "jank_entrypoint");
  if(entry == NULL)
  {
    fprintf(stderr, "dlsym failed: %s\n", dlerror());
    dlclose(handle);
    return 4;
  }

  const char *args[] = {
    "shared-host", // program name placeholder, dropped by the runtime
    "alpha",
    "beta",
    NULL
  };
  int exit_code = entry(3, args);
  dlclose(handle);

  return exit_code;
}
