// Simulates jank-generated code for: (defn ggg [v] (+ v 49))
extern "C"
{
  __attribute__((visibility("default"))) int jank_ggg(int v)
  {
    return v + 49; // Changed from 48 to 49!
  }
}
