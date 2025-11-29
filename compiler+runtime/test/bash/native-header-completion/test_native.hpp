/**
 * @file test_native.hpp
 * @brief Test header for native header completion integration test.
 *
 * This header simulates a library like flecs.h with:
 * - A namespace containing types, structs, and functions
 * - Used to test that jank's native header completion works correctly
 */

#pragma once

#include <string>

namespace test_native
{

/** A simple position struct */
struct position
{
  float x{};
  float y{};
  float z{};
};

/** A simple velocity struct */
struct velocity
{
  float dx{};
  float dy{};
  float dz{};
};

/** Entity ID type */
using entity_t = unsigned long long;

/** Create a new entity */
inline entity_t create_entity()
{
  static entity_t next_id = 1;
  return next_id++;
}

/** Initialize the system */
inline bool init_system()
{
  return true;
}

/** Shutdown the system */
inline void shutdown_system()
{
}

/** Get the library version */
inline std::string get_version()
{
  return "1.0.0";
}

/** Update position by velocity */
inline void update_position(position& pos, velocity const& vel, float dt)
{
  pos.x += vel.dx * dt;
  pos.y += vel.dy * dt;
  pos.z += vel.dz * dt;
}

} // namespace test_native
