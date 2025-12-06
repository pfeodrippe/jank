// Simulates jank-generated code for: (defn ggg [v] (+ v 48))
extern "C"
{
  __attribute__((visibility("default"))) int jank_ggg(int v)
  {
    return v + 48;
  }
}
