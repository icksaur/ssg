# vkobjects C++ Style (simple project snapshot)

This captures the style already used across `include/`, `src/`, `demo/`, and `tests/`.

## 1) Naming and case

- **Types** (`class`, `struct`, `enum class`, aliases): **PascalCase**  
  - Examples: `VulkanContext`, `BufferBuilder`, `Blas`, `Tlas`, `ValidationSeverity`
- **Functions/methods**: **camelCase**  
  - Examples: `buildTlas`, `submitAndWait`, `deviceAddress`, `pipelineCache`
- **Variables/fields**: mostly **camelCase**
  - Examples: `windowWidth`, `swapchainImageViews`, `enableRayTracing`
- **Private/internal data members** often use **trailing underscore** (`name_`) in newer core types
  - Examples: `rid_`, `geometry_`, `handle_`, `scratchAddress_`
- **Constants**
  - Shared constants: `kCamelCase` (`kNullRid`, `kCacheMagic`)
  - Capacity-style constants: `UPPER_SNAKE_CASE` (`MAX_STORAGE_BUFFERS`)
- **Macros**: `UPPER_SNAKE_CASE` (mostly Vulkan/VMA or integration macros)

## 2) struct vs class

- Use **`struct`** for:
  - plain data/config holders,
  - lightweight builder/state carriers,
  - POD-ish interop-friendly types.
- Use **`class`** for:
  - resource-owning RAII wrappers,
  - types with private invariants/lifetime rules.
- Pattern in this codebase:
  - `struct`: `VulkanContextOptions`, `BindlessTable`, `BlasBuilder`, `TlasInstances`
  - `class`: `VulkanContext`, `Buffer`, `Blas`, `Tlas`, `Image`

## 3) Formatting

- **4-space indentation**, no tabs.
- **Brace style**: opening brace on same line (`K&R` style).
- Keep small methods/one-liners inline when clear.
- Prefer readable wrapping for long Vulkan setup calls and initializer lists.

## 4) Header/source organization

- Public API in `include/`, implementation in `src/`.
- `#pragma once` headers.
- `.cpp` files commonly start with project/internal includes first, then STL/system includes.
- Use anonymous namespace in `.cpp` for file-local helpers.

## 5) API style conventions

- Fluent builder methods return `*this` by reference.
- Strongly typed enums (`enum class`) are preferred.
- Move-only GPU/resource objects are explicit (`delete` copy, define move).
- Fail-fast for invalid usage (`assert` for invariants, `throw std::runtime_error` for runtime failures).

## 6) Practical consistency rule

Some details (for example `Type&` vs `Type &`) are mixed in the current tree.  
For new work: keep the existing style of the file you are editing, and preserve naming/RAII patterns above.

