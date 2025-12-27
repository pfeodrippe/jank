# iOS Compile Server Namespace Conflict Fix

## Problem
When iOS apps connect to the jank compile server, the namespace path from the iOS bundle path may contain a `jank` directory, e.g.:
```
.Users.pfeodrippe.Library.Developer.CoreSimulator.Devices.57653CE6-DF09-4724-8B28-7CB6BA90E0E3.data.Containers.Bundle.Application.2186EFED-B0E0-41F3-AEE6-6F34D47194CB.SdfViewerMobile-JIT-Only.app.src.jank.vybe.sdf.ios
```

This creates a C++ namespace hierarchy:
```cpp
namespace _Users::pfeodrippe::...::SdfViewerMobile_JIT_Only::app::src::jank::vybe::sdf::ios
```

When code inside this namespace references `jank::runtime`, C++ first looks in the **local** `jank` namespace (from the hierarchy above), not the global `::jank::runtime`.

## Solution
Changed all generated code in `codegen/processor.cpp` to use **fully-qualified namespace references**:
- `jank::runtime` → `::jank::runtime`
- `jank::analyze` → `::jank::analyze`
- etc.

The `::` prefix tells C++ to start from the global namespace.

## Important Notes
1. Do NOT add `::` prefix to **namespace definitions** (`namespace jank::codegen { ... }`), only to **references** 
2. The `using namespace ::jank::analyze;` directive CAN use the `::` prefix
3. Case labels like `case ::jank::runtime::object_type::nil:` correctly use `::` prefix

## Related Fixes
1. **GC Thread Registration**: Changed `pthread_create` to `GC_pthread_create` for the compile server thread to fix "Collecting from unknown thread" crash
2. **Digit Prefix**: Modified `munge()` to prefix identifiers starting with digits with `_` (UUIDs in bundle paths)
3. **NS Binding Scope**: Added binding scope for target namespace during analysis phase to fix unresolved native aliases like `sdfx/init`
