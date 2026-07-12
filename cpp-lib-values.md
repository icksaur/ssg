# C++ Library Values

Distilled from `vkobjects` and `code-quality.md`. These principles have proven effective across multiple programs.

---

## Core Philosophy

| Principle | Statement |
|---|---|
| Code is a liability | Less code = less to maintain, test, and break |
| Correct by design | Eliminate entire classes of bugs through structure, not discipline |
| Simple is best | Complexity is the greatest enemy |
| Strong typing | Catch errors at compile time, not runtime |

---

## Improving an Existing API from Consumer Evidence

These values are decision criteria, not a checklist to apply mechanically. When
an API is awkward, start with representative consumer code rather than the
library implementation. Headers show what an API permits; consumers show what
it costs to use correctly.

### Read the consumer as evidence

Look for friction that appears at call sites:

| Consumer evidence | Likely API problem | Question to ask |
|---|---|---|
| Long comments explaining required call order | Temporal or implicit coupling | Can one operation own the sequence? |
| Several calls that must always occur together | Missing domain operation | What single intent is the caller trying to express? |
| The same setup or policy assembly in multiple paths | Missing shared seam | Which part is invariant mechanism, and which part is application policy? |
| Raw integers, IDs, flags, sentinels, or parallel arrays | Representation leakage or primitive obsession | Can a type make invalid combinations unrepresentable? |
| Consumers inspect or mutate library internals | Broken encapsulation | What query or operation would expose intent instead? |
| Manual waits, invalidation, cleanup, or notification hooks | Leaked lifetime or synchronization protocol | Can RAII or an owning operation make this automatic? |
| Many booleans whose combinations change validity | State-space explosion | Should these be modes, variants, or separate operations? |
| Silent fallback, `null`, `0`, or empty results on misuse | Ambiguous failure | Can the boundary validate and fail loudly? |
| Different names for the same concept across layers | Missing shared vocabulary | What domain term should the public API establish? |
| Test setup is longer than the behavior under test | API requires too much incidental knowledge | What smaller coherent seam should tests and consumers use? |

Comments are not automatically a defect. Keep comments that explain rationale,
non-local constraints, units, or contracts the type system cannot express.
Treat comments that teach callers how to keep an object valid as evidence that
the contract may belong in the API.

### Recover the hidden contract

Before changing an API:

1. Write the consumer's intended operation in one sentence, without naming
   implementation steps.
2. List the ordering, lifetime, state, synchronization, and error assumptions
   currently required.
3. Identify what happens when each assumption is violated.
4. Decide which type or component should own the contract.
5. Describe the smallest public operation that can enforce it.

This avoids extracting arbitrary helpers that merely move complicated code
without reducing the knowledge required of callers.

### Put responsibility at the right boundary

The library should own:

- resource validity and lifetime rules;
- ordering required by its own implementation;
- synchronization and cache invalidation needed for correctness;
- representation invariants;
- validation of library inputs;
- coherent operations shared by consumers.

The application should own:

- product and presentation policy;
- user interaction and command-line behavior;
- workload-specific scheduling and quality tradeoffs;
- composition of genuinely independent library operations.

When policy must cross the boundary, pass descriptive configuration or a
strategy. The application chooses the policy; the library applies it without
requiring the application to reproduce internal invariants.

Do not move code into the library merely to make a consumer shorter. Move a
responsibility only when doing so gives the contract one owner, removes
duplication, prevents misuse, or creates a coherent domain operation.

### Attack problems in leverage order

1. **Capture behavior.** Establish an oracle for externally visible behavior
   before changing the boundary.
2. **Remove correctness hazards.** Fix invalid states, ambiguous failures,
   lifetime hazards, and order-dependent protocols first.
3. **Encapsulate hidden contracts.** Replace required call sequences with named
   operations owned by the component that knows why the sequence exists.
4. **Strengthen boundaries.** Introduce domain types, scoped enums, validated
   configuration, and actionable errors.
5. **Unify duplicated mechanism.** Route consumers through one implementation
   while leaving application policy explicit.
6. **Establish vocabulary.** Rename around domain intent after responsibilities
   are in the right place.
7. **Remove obsolete comments and glue.** Cosmetic cleanup comes last; otherwise
   it can hide rather than eliminate the underlying coupling.

Correctness and leverage matter more than line count. A small naming cleanup is
useful, but it should not displace a change that makes misuse impossible.

### Refactor through narrow, verifiable steps

For each API change:

- name the consumer evidence being addressed;
- state the contract and its new owner;
- preserve an integration or golden oracle for existing behavior;
- add focused tests for validation, state transitions, and failure paths;
- compile representative consumer code against the new happy path;
- converge on one supported path: delete private or internal alternatives;
  migrate public APIs through compatibility shims, deprecation, or a staged
  transition before removing the old path.

Prefer tests at the seam where consumer intent becomes library behavior. Unit
tests for internal helpers are useful, but they do not prove that the public
boundary is easy to use correctly.

### Desired consumer experience

A successful API improvement leaves evidence in its consumers:

- the happy path reads in domain language;
- required ordering follows naturally from object structure or one operation;
- ownership and lifetime are visible;
- invalid inputs fail at the boundary with actionable errors;
- advanced control is available without burdening the common path;
- comments explain decisions, not operating instructions;
- representative consumers contain policy and orchestration, not library repair
  work.

Review the consumer again after the refactor. If it still needs to explain how
to hold the API correctly, the contract has not yet reached its proper owner.

---

## 1. RAII — Resource Acquisition Is Initialization

Every object that owns a GPU (or system) resource acquires it in the constructor and releases it in the destructor. No separate `init()`/`destroy()` pairs.

```cpp
Buffer buf(BufferBuilder(size).storage());   // acquires VkBuffer + VmaAllocation
// ...
// ~Buffer() frees automatically — no manual cleanup
```

**Why:** Eliminates entire classes of leaks. Scope controls lifetime.
**Applied to:** `Buffer`, `Image`, `ShaderModule`, `Pipeline`, `Commands`, `VulkanContext`, `Frame`, `TimestampQuery`.

---

## 2. Builder Passed to Constructor

Configuration is gathered in a Builder struct, then passed by reference to the object's constructor. The object itself is the valid, live resource.

```cpp
Buffer buf(BufferBuilder(size).storage().hostVisible());
Image  img(ImageBuilder().colorTarget(w, h), cmd);
```

- Stack allocation: `Buffer b(builder);`
- Heap allocation: `auto b = std::make_unique<Buffer>(builder);`
- In-place (vector): `vec.emplace_back(ImageBuilder().depth(), cmd);`

**Why:** Construction is a single expression. No two-phase init, no zombie objects. The caller decides storage class freely.

---

## 3. Fluent Builders

Builder methods return `*this` (by reference), enabling chains:

```cpp
BufferBuilder(size)
    .storage()
    .hostVisible()
    .transferDestination()
```

```cpp
VulkanContextOptions()
    .validation()
    .meshShaders()
    .throwOnValidationError()
```

**Why:** Configuration reads like English. Each option is opt-in; defaults are safe. Builder can be built inline or assigned to a local variable.

---

## 4. Objects Cannot Become Invalid — Throw in Builder/Constructor

If construction succeeds, the object is always valid. All validation happens before or during construction; failure throws.

```cpp
// Throws "buffer size must be greater than zero"
Buffer b(BufferBuilder(0).storage());

// Throws "failed to open shader file"
ShaderModule s(ShaderBuilder().mesh().fromFile("missing.spv"));

// Throws "invalid sample rate shading value"
VulkanContextOptions().sampleRateShading(3.0f);

// Throws "multiple frames in flight"
Frame f1; Frame f2;  // second construction throws
```

**Why:** No need to check `isValid()`, `bool operator`, or error codes on every use. Post-construction, the object can be used unconditionally.

---

## 5. Library User Owns Lifetime

No `shared_ptr`, no reference counting, no GC. The caller controls when objects are created and destroyed. Scope or container membership is the lifetime.

```cpp
std::vector<Image> depthImages;
for (size_t i = 0; i < context.swapchainImageCount; ++i)
    depthImages.emplace_back(ImageBuilder().depth(), setupCmd);
// depthImages.clear() → all ~Image() fire, all GPU memory freed
```

**Why:** Predictable performance, no hidden overhead, no ownership ambiguity. The user understands the program's resource profile completely.

---

## 6. Ownership and Hierarchical Lifetime Encoded in Constructor Parameters

Dependencies are required parameters — not setters, not globals (except the unavoidable context singleton). If an object needs a `Commands` or a `VulkanContext`, it takes one by reference, making the dependency and lifetime relationship visible at the call site.

```cpp
Image(ImageBuilder & builder, Commands & commands);   // needs active command buffer
Commands Frame::beginCommands();                       // Commands come from a Frame
Frame();                                               // Frame needs the context (via g_context)
```

The hierarchy is enforced structurally:
```
VulkanContext  (longest lifetime)
  └─ Frame     (one per render loop iteration, RAII guard prevents duplicates)
       └─ Commands  (tied to frame; can only submit through frame)
            └─ Buffer / Image  (created with commands, outlive the frame)
```

**Why:** Lifetime bugs become compile errors or immediate throws. You cannot accidentally use a `Commands` from a destroyed `Frame`.

---

## 7. Move Semantics, No Copy

Objects that own resources delete copy constructor and copy assignment. Move is allowed (transfers ownership, nulls the source).

```cpp
VulkanContext(const VulkanContext &) = delete;
VulkanContext & operator=(const VulkanContext &) = delete;
VulkanContext(VulkanContext &&) = delete;  // context: not even movable

Pipeline(Pipeline && other) : pipeline(other.pipeline) { other.pipeline = VK_NULL_HANDLE; }
Pipeline(const Pipeline &) = delete;
```

**Why:** Single ownership is enforced by the type system. No accidental double-free, no silent shallow copy of a GPU handle.

---

## 8. Scoped Enums Replace Raw Flags

Vulkan's raw `VkPipelineStageFlagBits2` integers are wrapped in `enum class`. Bitwise combination via `operator|` is provided explicitly.

```cpp
cmd.imageBarrier(img,
    Stage::Compute, Access::ShaderWrite, Layout::General,
    Stage::Fragment, Access::ShaderRead,  Layout::ShaderReadOnly);
```

**Why:** Passing `Stage` where `Access` is expected is a compile error. Intent is self-documenting. The underlying integer is still zero-cost.

---

## 9. Compile-Time Validation via CRTP and Templates

Push constant size is enforced at compile time, not discovered at driver crash:

```cpp
struct MyPush : PushConstantBase<MyPush> {
    uint32_t verticesRID;
    uint32_t textureRID;
};  // static_assert fires here if > 128 bytes

template<typename T>
void pushConstants(const T & data) {
    static_assert(sizeof(T) <= 128, "Push constants exceed 128-byte Vulkan guaranteed minimum");
    ...
}
```

**Why:** Shifts correctness left. The constraint is checked once at compile time for every instantiation, not at every call site.

---

## 10. Implicit Conversion Operators for Interop

Objects convert implicitly to their underlying Vulkan handles where needed, so they integrate with raw Vulkan APIs without boilerplate unwrapping.

```cpp
operator VkBuffer()        const;  // Buffer
operator VkImage()         const;  // Image
operator VkShaderModule()  const;  // ShaderModule
operator VkCommandBuffer()        ;  // Commands
operator VkPipeline()      const;  // Pipeline
operator SDL_Window*()           ;  // SDLWindow (demo)
```

**Why:** The wrapper object is a drop-in wherever the raw handle is expected. No `.handle()` or `.get()` noise.

---

## 11. Deferred GPU Destruction (Transparent to User)

GPU resources cannot be freed while in-flight. `DestroyGeneration` collects resources and frees them once the corresponding fence signals, completely transparently.

```cpp
// User just destroys a buffer normally:
{
    Buffer temp(...);
} // ~Buffer() schedules the VkBuffer for deferred release
// Actual vmaDestroyBuffer happens next time that frame's fence signals
```

**Why:** Correct-by-design GPU resource safety without forcing the user to track fences or delay deletion manually.

---

## 12. Runtime Invariant Guards

`Frame::currentGuard` is a static pointer that asserts only one `Frame` exists at a time. Construction sets it; the RAII destructor clears it.

```cpp
if (Frame::currentGuard != nullptr)
    throw std::runtime_error("multiple frames in flight");
Frame::currentGuard = this;
// ... ~Frame() { Frame::currentGuard = nullptr; }
```

**Why:** Misuse of the frame lifecycle becomes an immediate, descriptive exception — not undefined behavior or a Vulkan validation error buried in a log.

---

## 13. Separation of Configuration and Operation

Builders carry configuration. Objects carry state and behavior. Never mixed.

- `BufferBuilder` — no Vulkan calls, just accumulates flags
- `Buffer` — makes Vulkan calls, owns the allocation, exposes `upload`, `download`, `rid()`
- `VulkanContextOptions` — pure configuration struct with fluent setters
- `VulkanContext` — the live GPU context, not configurable after construction

**Why:** Configuration is easy to compose, copy, and inspect. Operational objects have a clear single responsibility.

---

## 14. Descriptive, Self-Documenting Names

Names communicate what and why, not how.

| Name | Intent |
|---|---|
| `Commands::submitAndWait()` | synchronous one-shot GPU work |
| `Commands::oneShot()` | creates a temporary command buffer for setup |
| `ImageBuilder::depthSampled()` | depth image that is also shader-readable |
| `BufferBuilder::readback()` | host-cached allocation for fast CPU reads |
| `Frame::beginCommands()` | commands are scoped to the frame |
| `VulkanContextOptions::throwOnValidationError()` | turns validation warnings into exceptions |

---

## 15. Immutable-After-Construction Objects

Objects do not expose setters. Once constructed, the GPU resource is what it is. Configuration happens before construction; re-configuration means constructing a new object.

**Why:** No invalid intermediate states. Thread-safe by default. Reasoning about object state is trivial: it is always the state it was constructed with.

---

## Summary Table

| Value | Mechanism |
|---|---|
| No leaks | RAII constructors/destructors |
| No invalid objects | Throw in builder/constructor |
| No lifetime ambiguity | Move-only, no copy |
| No misuse of API | Scoped enums, typed wrappers |
| No size bugs at runtime | `static_assert` in CRTP / templates |
| No ownership confusion | User owns lifetime; dependencies via constructor params |
| No two-phase init | Builder → constructor in one expression |
| No manual GPU sync for destroy | Deferred `DestroyGeneration` |
| No redundant unwrapping | Implicit conversion operators |
| No config/state tangling | Builders separate from objects |
