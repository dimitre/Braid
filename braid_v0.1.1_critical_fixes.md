# Braid v0.1.1 — Critical Fixes (from Claude review)

## Fix 1: Unified Error Type

```cpp
namespace braid {

// One result type for everything fallible
template<typename T>
struct Result {
    bool ok;
    T value;           // valid only if ok == true
    std::string error; // valid only if ok == false

    operator bool() const { return ok; }
    T* operator->() { return &value; }
    T& operator*() { return value; }
};

template<>
struct Result<void> {
    bool ok;
    std::string error;
    operator bool() const { return ok; }
};

// Usage:
Result<Shader> Shader::load(const LoadOptions& opts);
Result<Texture> Texture::load(wgpu::Device device, const char* path);
Result<void> Mesh::setVertices(std::span<Vertex> v);

// In user code:
auto shader = Shader::load({.wgsl = source});
if (!shader) {
    std::cerr << "Shader failed: " << shader.error << "\n";
    return;
}
shader->use();  // operator-> works because ok == true

} // namespace braid
```

**Rule:** Every fallible function returns `Result<T>` or `Result<void>`. No exceptions. No void-with-side-effects. No ad-hoc structs.

---

## Fix 2: Channel Thread Safety + Callback Timing

```cpp
namespace braid {

template<typename T>
class Channel {
public:
    // IMMOBILE — fixes Subscription lifetime
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    // Thread-safe: any thread can push
    void push(T event);

    // Thread-safe: any thread can pop, but MAIN THREAD ONLY for GPU work
    std::optional<T> pop();
    bool empty() const;
    void clear();

    // Callbacks fire during pop() on the MAIN THREAD
    // NOT at push() time — prevents GPU calls from I/O threads
    Subscription subscribe(std::function<void(T)> callback);

private:
    std::queue<T> queue_;
    mutable std::mutex mtx_;
    std::vector<std::function<void(T)>> callbacks_;
    mutable std::mutex cbMtx_;
};

template<typename T>
void Channel<T>::push(T event) {
    {
        std::lock_guard lock(mtx_);
        queue_.push(std::move(event));
    }
    // Callbacks are NOT fired here — deferred to pop() on main thread
}

template<typename T>
std::optional<T> Channel<T>::pop() {
    std::optional<T> result;
    {
        std::lock_guard lock(mtx_);
        if (!queue_.empty()) {
            result = std::move(queue_.front());
            queue_.pop();
        }
    }

    // Fire callbacks on main thread, with popped value
    if (result) {
        std::lock_guard lock(cbMtx_);
        for (auto& cb : callbacks_) {
            cb(*result);
        }
    }

    return result;
}

} // namespace braid
```

**Thread safety table (restored):**

| Operation | Thread-safe? | Notes |
|---|---|---|
| `Channel::push()` | ✅ Yes | Lock-protected. Any thread. |
| `Channel::pop()` | ✅ Yes | Lock-protected. Main thread for GPU callbacks. |
| `Channel::subscribe()` | ✅ Yes | Main thread preferred. |
| `Screen::*` | ❌ No | Main thread only. |
| `Shader::*` | ❌ No | Main thread only. |
| `Mesh::setVertices()` | ❌ No | Main thread only. |
| `Texture::load()` | ❌ No | Main thread. Async variant uses I/O thread + Channel push. |
| `wgpu::Queue::Submit()` | ✅ Yes | WebGPU queue is thread-safe. |

**Callback timing rule:** Subscribed callbacks fire during `pop()` on the **main thread**, not at `push()` time. This prevents GPU calls from I/O threads.

---

## Fix 3: Sketch Tier Internal Model

```cpp
namespace braid {

class SketchApp : public App {
    // === Internal state (hidden from user) ===
    struct State {
        glm::vec4 fill = {1,1,1,1};
        glm::vec4 stroke = {1,1,1,1};
        bool fillEnabled = true;
        bool strokeEnabled = true;
        float strokeWeight = 1.0f;
    } state_;

    struct TransformStack {
        std::vector<glm::mat4> stack;
        glm::mat4 current = glm::identity<glm::mat4>();

        void push() { stack.push_back(current); }
        void pop() { if (!stack.empty()) { current = stack.back(); stack.pop_back(); } }
        void translate(glm::vec3 t) { current = glm::translate(current, t); }
        void rotate(float angle, glm::vec3 axis) { current = glm::rotate(current, angle, axis); }
        void scale(glm::vec3 s) { current = glm::scale(current, s); }
    } transform_;

    // Default shader: handles MVP + color + basic lit/unlit
    Shader defaultShader_;

    // Current render pass encoder (managed by App::run)
    wgpu::RenderPassEncoder* currentPass_ = nullptr;

    // Batch accumulator: merge consecutive primitives of same type
    // v0.1.1: no batching — one draw call per primitive. Flagged for v0.2.

public:
    // === User API ===
    void background(float r, float g, float b, float a = 1.0f);
    void fill(float r, float g, float b, float a = 1.0f);
    void noFill();
    void stroke(float r, float g, float b, float a = 1.0f);
    void noStroke();
    void strokeWeight(float w);

    void pushMatrix();
    void popMatrix();
    void translate(float x, float y, float z = 0);
    void rotate(float angle);  // 2D, radians
    void rotate(float angle, float x, float y, float z);  // 3D axis-angle
    void scale(float s);

    void rect(float x, float y, float w, float h);
    void ellipse(float x, float y, float w, float h);
    void circle(float x, float y, float r);
    void line(float x1, float y1, float x2, float y2);
    void triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c);
    void quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d);
    void point(float x, float y);

    void box(float size);
    void sphere(float radius);

    // === Camera ===
    void camera(glm::vec3 eye, glm::vec3 center, glm::vec3 up);
    void perspective(float fov, float near, float far);
    void ortho(float left, float right, float bottom, float top, 
               float near = -1, float far = 1);

    // === Escape hatch ===
    wgpu::CommandEncoder& encoder();
    wgpu::RenderPassEncoder& pass();
    void flush();  // submit current encoder, start new one
};

} // namespace braid
```

**Sketch tier rules:**
1. **Default shader** handles MVP matrix + vertex color. User doesn't write WGSL for basic shapes.
2. **Transform stack** is separate from WebGPU pipeline state. `pushMatrix()`/`popMatrix()` are client-side only.
3. **No batching in v0.1.1** — each primitive is one draw call. Flagged for v0.2 optimization.
4. **State changes** (fill, stroke) are client-side until a primitive is drawn. No pipeline rebind mid-frame.
5. **Camera** sets projection + view matrices on the default shader. `camera()` replaces both `perspective()` and `lookAt()`.

---

## Fix 4: MouseEvent Syntax

```cpp
namespace braid {

enum class MouseButton { Left, Right, Middle };

struct MouseEvent {
    std::optional<MouseButton> button;  // nullopt for move events
    glm::vec2 pos;
    glm::vec2 delta;
    bool pressed;  // only meaningful if button.has_value()
    int clickCount;
};

} // namespace braid
```

---

## Fix 5: Mesh::line Device Parameter

```cpp
// WRONG (missing device)
static Mesh line(std::span<glm::vec2> points);

// RIGHT
static Mesh line(wgpu::Device device, std::span<glm::vec2> points);
```

---

## Fix 6: clone() Signatures

| Class | clone() signature | Notes |
|---|---|---|
| `Screen` | `Screen clone() const` | Offscreen only. Swapchain Screen asserts/fails. |
| `Mesh` | `Mesh clone() const` | Copies GPU buffers. |
| `Texture` | `Texture clone(wgpu::CommandEncoder& encoder) const` | GPU→GPU copy via encoder. |
| `Buffer` | `Buffer clone() const` | Copies GPU buffer. |

**Ownership table fix:**

| Class | Copy | Move | Clone | Rationale |
|---|---|---|---|---|
| `Screen` | Deleted | Default | `clone()` — offscreen only | Swapchain Screen can't clone |
| `Texture` | Deleted | Default | `clone(encoder)` | Needs encoder for GPU copy |
| `Mesh` | Deleted | Default | `clone()` | Copies GPU buffers |
| `Buffer` | Deleted | Default | `clone()` | Copies GPU buffer |

---

## Fix 7: Dawn Linkage

```yaml
# chalet.yaml — macOS only, Dawn as prebuilt shared lib
name: braid
version: 0.1.0

targets:
  braid-core:
    kind: staticLib
    language: C++
    cppStandard: c++20
    sources: src/braid/*.cpp
    includes: 
      - src
      - libs/RGFW
      - libs/mango/include
      - libs/dawn/include
    links:
      - dawn  # libdawn.dylib, prebuilt or system
      - mango
      - glm
      - fmt

  hello:
    kind: executable
    sources: examples/hello.cpp
    links: braid-core

dependencies:
  RGFW:
    git: https://github.com/ColleagueRiley/RGFW
    tag: v1.0.0

  mango:
    git: https://github.com/t0rakka/mango
    tag: v1.0.0

  glm:
    git: https://github.com/g-truc/glm
    tag: 0.9.9.8

  fmt:
    git: https://github.com/fmtlib/fmt
    tag: 10.0.0

  # Dawn: prebuilt for macOS, not compiled from source
  # User downloads libdawn.dylib + headers separately
  # Or: system dependency if available via Homebrew
```

**Note:** Dawn compilation from source is 15-40MB and minutes to build. For v0.1, require prebuilt `libdawn.dylib`. Document download link.

---

## Fix 8: Validation Default

```cpp
struct Settings {
    // ...
    #ifdef NDEBUG
        bool enableValidation = false;  // Release: off
    #else
        bool enableValidation = true;   // Debug: on
    #endif
};
```

---

## Fix 9: bindUniform Buffer Lifetime

```cpp
class Shader {
    // Ring buffer: 3 frames, rotating
    // Each frame gets one uniform buffer, reused after 3 frames
    // Safe because GPU is pipelined (max 3 frames in flight)
    struct UniformRing {
        std::array<wgpu::Buffer, 3> buffers;
        std::array<size_t, 3> capacities;
        int index = 0;

        wgpu::Buffer allocate(wgpu::Device device, size_t size) {
            auto& buf = buffers[index];
            auto& cap = capacities[index];
            if (!buf || cap < size) {
                buf = device.CreateBuffer({size, wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst});
                cap = size;
            }
            index = (index + 1) % 3;
            return buf;
        }
    };
    UniformRing uniformRing_;

public:
    wgpu::BindGroup bindUniform(int group, int binding, 
                                 const void* data, size_t size) {
        auto buf = uniformRing_.allocate(device_, size);
        device_.GetQueue().WriteBuffer(buf, 0, data, size);
        // Create bind group referencing buf
        // buf remains alive for 3 frames — safe for GPU use
    }
};
```

---

## Fix 10: "Single-Header" Rephrase

**Old:** "Single-header API (`braid.h`) — LLM ingests entire framework"

**New:** "Single-include API — import `braid.h` for the full interface. Implementation is in `braid.cpp` (or `src/braid/*.cpp`). Both fit comfortably in LLM context windows."

---

## Summary of Blockers

| # | Issue | Fix | Status |
|---|---|---|---|
| 1 | Inline enum syntax | Separate `enum class MouseButton` | ✅ Fixed |
| 2 | `Mesh::line` missing device | Add `wgpu::Device device` param | ✅ Fixed |
| 3 | clone() signatures | Table + code aligned | ✅ Fixed |
| 4 | Error handling inconsistency | Unified `Result<T>` | ✅ Fixed |
| 5 | Channel thread safety | Restored table + callback timing | ✅ Fixed |
| 6 | Sketch tier internals | Specified state + transform + shader | ✅ Fixed |
| 7 | Dawn binary size | Prebuilt shared lib, documented | ✅ Fixed |
| 8 | Validation default | Debug-only | ✅ Fixed |
| 9 | bindUniform lifetime | Ring buffer, 3 frames | ✅ Fixed |
| 10 | "Single-header" claim | Rephrased to "single-include" | ✅ Fixed |

All blockers resolved. Ready for code generation.
