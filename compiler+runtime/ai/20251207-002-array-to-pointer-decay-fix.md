# Array-to-Pointer Decay Fix for Binary Operators

## Problem

When assigning a string literal to a `const char*` struct field using `cpp/=`, the operation failed:

```clojure
(let [s (cpp/new fl/ecs_entity_desc_t)]
  (cpp/= (cpp/.-name s) #cpp "ddd")  ; FAILED
  nil)
```

Error: `Binary operator = is not supported for 'const char *&' and 'const char (&)[4]'`

The issue was that the `no_binary_incompat_ptrs` validator in processor.cpp rejected the assignment because:
- `args[0]` is `const char*` (pointer type)
- `args[1]` is `const char[4]` (array type)
- The validator checked if both args are pointers, but arrays are not pointers in C++

## Root Cause

In `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/analyze/processor.cpp`, the `no_binary_incompat_ptrs` validator (lines 360-397) only checked `Cpp::IsPointerType()` for both arguments. C/C++ arrays are not pointers but should decay to pointers when assigned.

Original code:
```cpp
auto const is_arg0_ptr{ Cpp::IsPointerType(Cpp::GetNonReferenceType(args[0].m_Type)) };
if((is_arg0_ptr
    && is_arg0_ptr != Cpp::IsPointerType(Cpp::GetNonReferenceType(args[1].m_Type)))
   || !Cpp::IsImplicitlyConvertible(...))
```

## Solution

Added explicit handling for array-to-pointer decay. When the destination is a pointer and the source is an array, check if the array element type is compatible with the pointer's pointee type.

Fixed code:
```cpp
auto const arg0_type{ Cpp::GetNonReferenceType(args[0].m_Type) };
auto const arg1_type{ Cpp::GetNonReferenceType(args[1].m_Type) };
auto const is_arg0_ptr{ Cpp::IsPointerType(arg0_type) };
auto const is_arg1_ptr{ Cpp::IsPointerType(arg1_type) };
auto const is_arg1_arr{ Cpp::IsArrayType(arg1_type) };

/* Handle array-to-pointer decay: arrays are compatible with pointers
 * when the array element type matches the pointer's pointee type.
 * This handles cases like: const char* = "string literal" */
if(is_arg0_ptr && is_arg1_arr)
{
  auto const pointee_type{ Cpp::GetPointeeType(arg0_type) };
  auto const elem_type{ Cpp::GetArrayElementType(arg1_type) };
  if(pointee_type && elem_type
     && Cpp::IsImplicitlyConvertible(elem_type, pointee_type))
  {
    return ok();
  }
}
```

## Test

Added test at `/Users/pfeodrippe/dev/jank/compiler+runtime/test/jank/cpp/operator/equal/pass-array-to-pointer-decay.jank`:

```clojure
(cpp/raw "struct TestCharStruct { const char* name; };")
(let [s (cpp/new cpp/TestCharStruct)]
  (cpp/= (cpp/.-name s) #cpp "hello")
  :success)
```

## Result

Before: `Binary operator = is not supported for 'const char *&' and 'const char (&)[4]'`
After: Assignment works correctly

Tests: 212 total, 211 passed, 1 failed (pre-existing unrelated issue)

## Use Case

This enables pure jank syntax for setting C struct string fields:

```clojure
(let [desc (cpp/new fl/ecs_entity_desc_t)]
  (cpp/= (cpp/.-name desc) #cpp "my_entity_name")
  desc)
```
