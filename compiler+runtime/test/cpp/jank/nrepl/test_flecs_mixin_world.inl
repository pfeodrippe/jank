/* Mixin file included inside world class - mimics flecs mixin pattern */

/* Template method - like flecs entity<T>() */
template <typename T>
T *get_component()
{
  return nullptr;
}

/* Template method with multiple params */
template <typename T, typename U>
void set_pair()
{
}
