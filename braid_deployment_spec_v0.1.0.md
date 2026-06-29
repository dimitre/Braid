# Braid v0.1.0 — Deployment Specification & Roadmap
## WebGPU Creative Coding Framework

**Date:** 2026-06-28  
**Status:** Ready for Claude implementation  
**Position:** 0.12 on microframework (0.0) to OF (0.5) scale  
**Target:** ~2,000 lines core, ~3s compile, ~1MB binary  

---

## 1. Philosophy

Braid is built from three primitives:

1. **Screen** — a renderable texture. Everything is a texture. The swapchain is just the final Screen.
2. **Command** — explicit GPU work submission. No implicit state machine.
3. **Channel** — pull-first event streams. Callbacks are opt-in, not required.

The API has two tiers:
- **Tier 1 (Explicit):** Direct WebGPU control. Multi-pass pipelines, compute shaders, explicit resource management.
- **Tier 2 (Sketch):** Processing-like facade. `background()`, `ellipse()`, `fill()`. Hides the encoder for simple sketches. You can drop to Tier 1 at any point.

**LLM-friendly design:**
- Single-header API (`braid.h`) — LLM ingests entire framework
- Explicit resource lifecycle — no hidden state
- Named parameter structs — self-documenting
- Error returns, not exceptions — LLM can branch on results
- No macro magic — AST-parseable

---

## 2. Resource Ownership & Lifetime

Every class holding a WebGPU handle follows these rules:

| Class | Copy | Move | Clone | Rationale |
|---|---|---|---|---|
| `Screen` | Deleted | Default | `clone()` | GPU texture handle, unique ownership |
| `Shader` | Deleted | Default | No | Pipeline cache tied to source WGSL |
| `Mesh` | Deleted | Default | `clone()` | GPU buffers, unique ownership |
| `Texture` | Deleted | Default | `clone()` | GPU texture, unique ownership |
| `Buffer` | Deleted | Default | `clone()` | GPU buffer, unique ownership |
| `App` | Deleted | Deleted | No | Window + device, singleton per process |
| `Channel<T>` | Deleted | Default | No | Queue state, unique |
| `Future<T>` | Deleted | Default | No | Async operation handle |
| `Timer` | Deleted | Default | No | Timing state |

**Move semantics:** All resources are move-only. Moving transfers GPU handle ownership. The moved-from object is empty (null handle, no-op destructor).

**Clone semantics:** Explicit `clone()` creates a new GPU resource with identical contents.

**RAII:** All resources release GPU memory on destruction. No manual `destroy()` calls.

---

## 3. Core Primitives

### 3.1 Screen

```cpp
namespace braid {

class Screen {
public:
    Screen(wgpu::Device device, int width, int height, 
           wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm);
    Screen(wgpu::Surface surface, int width, int height, 
           wgpu::TextureFormat format);

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    Screen(Screen&&) = default;
    Screen& operator=(Screen&&) = default;

    Screen clone() const;

    wgpu::RenderPassEncoder begin(wgpu::CommandEncoder& encoder, 
                                   glm::vec4 clearColor = {0,0,0,1});
    void end(wgpu::RenderPassEncoder& pass);

    wgpu::TextureView asTexture() const;
    void drawFullscreen(wgpu::RenderPassEncoder& pass, 
                        wgpu::BindGroup sourceBinding);

    int width() const;
    int height() const;
    wgpu::TextureFormat format() const;
    bool isValid() const;
};

} // namespace braid
```

### 3.2 Shader

```cpp
namespace braid {

class Shader {
public:
    struct LoadOptions {
        const char* wgsl = nullptr;
        const char* label = "shader";
        bool debug = false;
    };

    void load(const LoadOptions& opts);
    void loadFile(const char* path, bool debug = false);

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) = default;
    Shader& operator=(Shader&&) = default;

    wgpu::BindGroup bindTexture(int group, int binding, 
                                 wgpu::TextureView texture, 
                                 wgpu::Sampler sampler);
    wgpu::BindGroup bindBuffer(int group, int binding, 
                                wgpu::Buffer buffer, 
                                size_t offset = 0, 
                                size_t size = WGPU_WHOLE_SIZE);
    wgpu::BindGroup bindUniform(int group, int binding, 
                                 const void* data, 
                                 size_t size);

    wgpu::RenderPipeline getPipeline(
        wgpu::VertexBufferLayout vertexLayout,
        wgpu::BlendState blend = Blend::Alpha,
        wgpu::PrimitiveTopology topology = wgpu::PrimitiveTopology::TriangleList
    );

    wgpu::BindGroupLayout getLayout(uint32_t groupIndex = 0) const;
    bool isValid() const;
};

namespace Blend {
    extern wgpu::BlendState Alpha;
    extern wgpu::BlendState Additive;
    extern wgpu::BlendState None;
}

} // namespace braid
```

**Design notes:**
- No WGSL reflection. Manual `@group(0) @binding(1)` in C++ bind calls.
- `bindUniform` copies data immediately into staging buffer. Safe to pass stack pointers.
- Pipelines cached internally by (vertexLayout, blend, topology) key.

### 3.3 Mesh

```cpp
namespace braid {

struct Vertex {
    glm::vec3 position;
    glm::vec2 texCoord;
    glm::vec3 normal;
    glm::vec4 color;
};

class Mesh {
public:
    Mesh(wgpu::Device device);

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) = default;
    Mesh& operator=(Mesh&&) = default;

    Mesh clone() const;

    void setVertices(std::span<Vertex> vertices);
    void setVertices(std::span<glm::vec3> positions,
                      std::span<glm::vec2> texCoords = {},
                      std::span<glm::vec3> normals = {},
                      std::span<glm::vec4> colors = {});
    void updateVertices(std::span<Vertex> vertices, size_t vertexOffset = 0);
    void setIndices(std::span<uint32_t> indices);

    void draw(wgpu::RenderPassEncoder& pass, uint32_t instanceCount = 1);

    static Mesh plane(wgpu::Device device, float w, float h, 
                      int cols = 1, int rows = 1);
    static Mesh cube(wgpu::Device device, float size);
    static Mesh cubeSphere(wgpu::Device device, float size, int subdivisions);
    static Mesh sphere(wgpu::Device device, float radius, 
                       int slices = 32, int stacks = 16);
    static Mesh icosahedron(wgpu::Device device, float radius, 
                            int subdivisions = 0);
    static Mesh torus(wgpu::Device device, float majorR, float minorR,
                       int majorSegs = 32, int minorSegs = 16);
    static Mesh cylinder(wgpu::Device device, float radius, float height,
                          int segments = 32);
    static Mesh cone(wgpu::Device device, float radius, float height,
                      int segments = 32);
    static Mesh fullscreenQuad(wgpu::Device device);
    static Mesh line(wgpu::Device device, std::span<glm::vec3> points);
    static Mesh line(std::span<glm::vec2> points);

    wgpu::VertexBufferLayout vertexLayout() const;
    size_t vertexCount() const;
    size_t indexCount() const;
    bool hasIndices() const;
    bool isValid() const;
};

} // namespace braid
```

### 3.4 Texture (with mango direct-to-GPU)

```cpp
namespace braid {

class Texture {
public:
    struct LoadResult {
        bool ok;
        std::string error;
    };

    // Sync load: mango decodes → direct GPU upload (zero-copy where possible)
    static LoadResult load(Texture& out, wgpu::Device device, const char* path);

    // Async load: returns Future, decodes on I/O thread, uploads on main
    static Future<Texture> loadAsync(wgpu::Device device, const char* path);

    // Create from raw data
    Texture(wgpu::Device device, int width, int height, 
            wgpu::TextureFormat format,
            const void* data = nullptr);

    Texture(wgpu::Texture texture);

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = default;
    Texture& operator=(Texture&&) = default;

    Texture clone(wgpu::CommandEncoder& encoder) const;

    wgpu::TextureView view() const;
    wgpu::Texture handle() const;

    int width() const;
    int height() const;
    wgpu::TextureFormat format() const;
    bool isValid() const;

    // CPU readback (async)
    Future<std::vector<uint8_t>> readPixelsAsync();

    // Mango-accelerated operations
    void generateMipmap(wgpu::CommandEncoder& encoder);
    void resize(int newWidth, int newHeight, wgpu::CommandEncoder& encoder);
};

} // namespace braid
```

**Mango integration:**
- `Texture::load()` uses mango's SIMD-accelerated decode (18× faster than libpng)
- Direct upload to GPU via `writeTexture()` — no intermediate CPU buffer where possible
- Color space conversion (sRGB → linear) done via mango's SIMD path during decode

### 3.5 Buffer

```cpp
namespace braid {

class Buffer {
public:
    Buffer(wgpu::Device device, size_t size, wgpu::BufferUsage usage);

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) = default;
    Buffer& operator=(Buffer&&) = default;

    void write(const void* data, size_t size, size_t offset = 0);
    Future<std::vector<uint8_t>> readAsync(size_t size, size_t offset = 0);

    wgpu::Buffer handle() const;
    size_t size() const;
    bool isValid() const;
};

} // namespace braid
```

---

## 4. Event System: Channels (Pull-First)

```cpp
namespace braid {

template<typename T>
class Channel {
public:
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = default;
    Channel& operator=(Channel&&) = default;

    void push(T event);
    std::optional<T> pop();
    bool empty() const;
    void clear();

    Subscription subscribe(std::function<void(T)> callback);

    template<typename Filter>
    Subscription subscribe(Filter filter, std::function<void(T)> callback);
};

class Subscription {
public:
    ~Subscription();
    Subscription(Subscription&&);
    Subscription(const Subscription&) = delete;
};

struct KeyEvent {
    int key;
    bool pressed;
    bool repeat;
    bool shift, ctrl, alt, super;
};

struct MouseEvent {
    std::optional<enum Button { Left, Right, Middle }> button;
    glm::vec2 pos;
    glm::vec2 delta;
    bool pressed;
    int clickCount;
};

struct ScrollEvent {
    glm::vec2 delta;
};

struct WindowEvent {
    enum Type { Resized, Moved, Focused, Unfocused, Closed } type;
    glm::vec2 size;
    glm::vec2 pos;
};

struct DropEvent {
    std::vector<std::string> paths;
};

} // namespace braid
```

---

## 5. Timer (ofTimerFps Logic)

```cpp
namespace braid {

class Timer {
public:
    Timer(int targetFps = 60);

    void setFps(int fps);
    void reset();
    void waitNext();  // hybrid sleep+yield from ofTimerFps

    float deltaTime() const;
    float elapsedTime() const;
    int frameCount() const;
    int currentFps() const;

private:
    using nanos = std::chrono::duration<long long, std::nano>;
    nanos interval;
    std::chrono::time_point<std::chrono::steady_clock> wakeTime;
    std::chrono::time_point<std::chrono::steady_clock> lastWakeTime;
    int targetFps;
    int frames = 0;
    float delta = 0.0f;
    float elapsed = 0.0f;
};

} // namespace braid
```

**Implementation:** Direct port of `ofTimerFps`:
1. `sleep_until(wakeTime - 3ms)` — lazy sleep
2. `yield()` spin — tight loop for last 3ms
3. `wakeTime += interval` — advance target

---

## 6. App Lifecycle

### 6.1 Explicit Tier (App)

```cpp
namespace braid {

class App {
public:
    struct Settings {
        int width = 1280;
        int height = 720;
        const char* title = "Braid";
        bool resizable = true;
        bool fullscreen = false;
        bool vsync = true;
        int msaaSamples = 1;
        wgpu::TextureFormat format = wgpu::TextureFormat::BGRA8Unorm;
        int ioThreads = 2;
        bool enableValidation = true;
        int targetFps = 60;
    };

    App(const Settings& settings = {});
    virtual ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    virtual void setup() {}
    virtual void update() {}
    virtual void draw() {}
    virtual void exit() {}

    virtual void keyPressed(KeyEvent e) {}
    virtual void keyReleased(KeyEvent e) {}
    virtual void mousePressed(MouseEvent e) {}
    virtual void mouseReleased(MouseEvent e) {}
    virtual void mouseMoved(MouseEvent e) {}
    virtual void mouseDragged(MouseEvent e) {}
    virtual void windowResized(WindowEvent e) {}

    virtual void deviceLost() {}
    virtual void surfaceResized(int w, int h) {}

    void run();
    void present();
    void close();

    wgpu::Device device() const;
    wgpu::Queue queue() const;
    Screen& screen();
    Timer& timer();

    int width() const;
    int height() const;
    glm::vec2 mousePos() const;
    float mouseX() const;
    float mouseY() const;
    float deltaTime() const;
    float elapsedTime() const;
    int frameCount() const;

    Channel<KeyEvent>& keyEvents;
    Channel<MouseEvent>& mouseEvents;
    Channel<ScrollEvent>& scrollEvents;
    Channel<WindowEvent>& windowEvents;
    Channel<DropEvent>& dropEvents;

    void setWindowTitle(const char* title);
    void setWindowSize(int w, int h);
    void setFullscreen(bool fullscreen);
    void setCursorVisible(bool visible);
    void setCursorLocked(bool locked);
};

} // namespace braid
```

### 6.2 Sketch Tier (SketchApp)

```cpp
namespace braid {

class SketchApp : public App {
public:
    SketchApp(const Settings& settings = {});

    // Drawing commands (implicit encoder)
    void background(float r, float g, float b, float a = 1.0f);
    void background(glm::vec4 color);
    void fill(float r, float g, float b, float a = 1.0f);
    void fill(glm::vec4 color);
    void noFill();
    void stroke(float r, float g, float b, float a = 1.0f);
    void stroke(glm::vec4 color);
    void noStroke();
    void strokeWeight(float weight);

    // Primitives
    void ellipse(float x, float y, float w, float h);
    void circle(float x, float y, float r);
    void rect(float x, float y, float w, float h);
    void line(float x1, float y1, float x2, float y2);
    void line(glm::vec2 a, glm::vec2 b);
    void line(glm::vec3 a, glm::vec3 b);
    void triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c);
    void triangle(glm::vec3 a, glm::vec3 b, glm::vec3 c);
    void quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d);
    void point(float x, float y);
    void point(glm::vec3 p);

    // 3D
    void box(float size);
    void box(float w, float h, float d);
    void sphere(float radius);
    void sphere(float radius, int slices, int stacks);

    // Transformations
    void pushMatrix();
    void popMatrix();
    void translate(float x, float y, float z = 0);
    void translate(glm::vec3 t);
    void rotate(float angle);
    void rotate(float angle, float x, float y, float z);
    void rotate(glm::quat q);
    void scale(float s);
    void scale(float x, float y, float z);
    void scale(glm::vec3 s);

    // Camera
    void camera(glm::vec3 eye, glm::vec3 center, glm::vec3 up);
    void perspective(float fov, float near, float far);
    void ortho(float left, float right, float bottom, float top, 
               float near = -1, float far = 1);

    // Image
    void image(Texture& tex, float x, float y, float w = 0, float h = 0);
    void image(Screen& screen, float x, float y, float w = 0, float h = 0);

    // Escape hatch
    wgpu::CommandEncoder& encoder();
    wgpu::RenderPassEncoder& pass();
    void flush();
};

} // namespace braid
```

---

## 7. Compute Shaders

```cpp
namespace braid {

class ComputePass {
public:
    ComputePass(wgpu::Device device);

    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;
    ComputePass(ComputePass&&) = default;
    ComputePass& operator=(ComputePass&&) = default;

    void setShader(Shader& shader);
    void bindBuffer(int group, int binding, wgpu::Buffer buffer, 
                    wgpu::BufferUsage usage = wgpu::BufferUsage::Storage);
    void bindTexture(int group, int binding, wgpu::TextureView texture);
    void setUniforms(int group, int binding, const void* data, size_t size);

    void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1);
    void dispatchIndirect(wgpu::Buffer indirectBuffer, size_t offset);

    void submit();
    void encode(wgpu::CommandEncoder& encoder);
};

} // namespace braid
```

---

## 8. Error Handling

```cpp
namespace braid {

class Error {
public:
    const char* message() const;
    const char* file() const;
    int line() const;
    bool isFatal() const;
};

class ShaderCompileError : public Error {};
class DeviceLostError : public Error {};
class TextureLoadError : public Error {};

} // namespace braid
```

**Behavior:**
- `Shader::load()` returns `Error` on failure, not throws
- `App::deviceLost()` called on GPU reset
- Validation errors logged in debug, ignored in release

---

## 9. Project Structure

```
braid/
├── braid.h              # Single include — LLM ingests one file
├── braid.cpp            # Implementation
├── chalet.yaml          # Build config
├── .zed/
│   ├── settings.json
│   ├── tasks.json
│   └── keymap.json
├── examples/
│   ├── hello.cpp        # Minimal: clear screen
│   ├── primitives.cpp   # Your microframework port
│   └── particles.cpp    # Compute + render
├── libs/
│   ├── RGFW/            # Git-fetched by Chalet
│   ├── mango/           # Git-fetched by Chalet
│   └── dawn/            # Git-fetched by Chalet or system
└── README.md
```

### 9.1 Chalet Configuration

```yaml
# chalet.yaml
name: braid
version: 0.1.0
default-run: hello

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
      - webgpu_dawn
      - mango

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

  dawn:
    git: https://dawn.googlesource.com/dawn
    commit: abc123...
```

### 9.2 Zed Configuration

```json
// .zed/settings.json
{
  "lsp": {
    "clangd": {
      "binary": {
        "path": "/usr/bin/clangd",
        "arguments": ["--background-index", "--clang-tidy"]
      }
    }
  },
  "languages": {
    "C++": {
      "tab_size": 4,
      "hard_tabs": false,
      "preferred_line_length": 100
    }
  }
}
```

```json
// .zed/tasks.json
[
  {
    "label": "chalet build debug",
    "command": "chalet build --configuration Debug",
    "use_new_terminal": false,
    "reveal": "always"
  },
  {
    "label": "chalet run",
    "command": "chalet run hello",
    "use_new_terminal": false,
    "reveal": "always"
  }
]
```

```json
// .zed/keymap.json
[
  {
    "context": "Workspace",
    "bindings": {
      "cmd-shift-b": ["task::Spawn", { "task_name": "chalet build debug" }],
      "cmd-shift-r": ["task::Spawn", { "task_name": "chalet run" }]
    }
  }
]
```

---

## 10. Roadmap

### v0.1.0 — Bootstrap (Week 1-2)
- [ ] RGFW + WebGPU bootstrap (window, surface, device)
- [ ] `Screen` class (begin/end, clear, fullscreen quad)
- [ ] `Shader` class (load WGSL, compile, basic pipeline)
- [ ] `Mesh` class (vertex buffer, draw, plane + cube primitives)
- [ ] `App` class (run loop, event pump, virtual hooks)
- [ ] `Timer` class (ofTimerFps port)
- [ ] `hello.cpp` example (clear screen, colored triangle)
- [ ] Chalet build config
- [ ] Zed IDE config

**Target:** Compiles, runs, draws a triangle. ~1,500 lines.

### v0.1.1 — Sketch Tier (Week 3-4)
- [ ] `SketchApp` class (background, fill, rect, ellipse, line)
- [ ] `pushMatrix`/`popMatrix`, translate, rotate, scale
- [ ] `camera`, `perspective`, `ortho`
- [ ] `box`, `sphere` primitives
- [ ] `image()` for Texture and Screen
- [ ] Port your microframework `program.h` sketch
- [ ] `Channel<T>` event system (key, mouse, window)

**Target:** Your old sketch runs. ~2,000 lines.

### v0.1.2 — Mango Integration (Week 5-6)
- [ ] `Texture::load()` with mango SIMD decode
- [ ] Direct GPU upload path (zero-copy where possible)
- [ ] `Texture::loadAsync()` with I/O thread pool
- [ ] `generateMipmap()`, `resize()` via mango
- [ ] `Buffer` class (GPU buffer, upload/download)

**Target:** Image loading is fast. ~2,500 lines.

### v0.2.0 — Compute & Async (Week 7-8)
- [ ] `ComputePass` class
- [ ] Compute + render buffer sharing
- [ ] Particle system example
- [ ] `Future<T>` for async operations
- [ ] `readPixelsAsync()`

**Target:** GPU particles. ~3,000 lines.

### v0.2.1 — Text & Audio (Week 9-10)
- [ ] SDF font rendering (atlas generation)
- [ ] `Font` class, `text()` on SketchApp
- [ ] miniaudio integration (or rtaudio)
- [ ] `Sound` class, basic playback

**Target:** Text + audio working. ~3,500 lines.

### v0.3.0 — Polish (Week 11-12)
- [ ] Shader hot-reload (file watcher)
- [ ] Frame budget profiling
- [ ] Multi-window support (research)
- [ ] Emscripten target (research)
- [ ] Documentation + examples

**Target:** Production-ready for installations. ~4,000 lines.

---

## 11. Claude Implementation Brief

**Context:** You are implementing Braid v0.1.0, a WebGPU creative coding framework. The spec above defines the API. Key constraints:

1. **Single-header API** (`braid.h`) where possible
2. **Move-only resources** — no copies of GPU handles
3. **Explicit over implicit** — no hidden state
4. **No WGSL reflection** — manual bind group bindings
5. **ofTimerFps logic** for frame pacing
6. **mango for image loading** — SIMD-accelerated
7. **Chalet build** — JSON/YAML config
8. **Zed IDE** — tasks and keybindings included

**First deliverable:** `braid.h` + `braid.cpp` + `chalet.yaml` + `examples/hello.cpp` that compiles and draws a colored triangle.

**Dependencies:** RGFW (windowing), Dawn (WebGPU), mango (image), glm (math), fmt (strings).

**Do not implement:** Shader reflection, exception-based errors, multi-window, audio, text rendering, compute shaders. These are v0.2+.

**Generate the core files now.**
