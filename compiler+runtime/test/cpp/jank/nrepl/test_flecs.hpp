/* Simplified flecs-like header for testing member autocompletion.
 * Mimics the real flecs::world structure with:
 * - Template methods
 * - Non-template methods
 *
 * Note: The real flecs::world uses #include inside the class body for mixins,
 * which creates complex declaration chains. That case is tested separately
 * with inline cpp/raw since include paths are tricky in tests.
 */
#pragma once

namespace flecs {

struct world {
    /* Non-template methods - like flecs progress(), defer_begin() */
    bool progress(float delta_time = 0.0f) { return true; }
    void defer_begin() {}
    void defer_end() {}
    void quit() {}

    /* Template method inline - like flecs entity<T>() */
    template<typename T>
    T* entity() { return nullptr; }

    /* Mixin content (normally #include "mixins/...") */
    template<typename T>
    T* get_component() { return nullptr; }

    template<typename T, typename U>
    void set_pair() {}

    /* More non-template methods */
    int get_count() { return 0; }
    void* get_world_ptr() { return nullptr; }
};

/* Another type in the flecs namespace */
struct entity {
    void add() {}
    void remove() {}

    template<typename T>
    T* get() { return nullptr; }
};

} /* namespace flecs */
