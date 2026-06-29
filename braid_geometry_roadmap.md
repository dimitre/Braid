# Braid — Geometry, Camera & Mesh Loading Thoughts

*How camera, solid primitives, and mesh loading fit into Braid's two-tier design without breaking the "Surface is the one primitive" thesis.*

---

## Camera

### What is there now

`SketchApp` already has an immediate-mode camera:

```cpp
perspective(glm::radians(40.0f), 0.1f, 2450.0f);
camera({0, 0, 450}, {0, 0, 0}, {0, 1, 0});
```

This is fine for OF-style sketches. It maps directly to GLU-style expectations and keeps the API small.

### What is missing

A persistent `Camera` object would help for:

- **Orbit / arcball controls** (mouse drag rotates around a target).
- **Fly / first-person controls** (WASD + mouse look).
- **Smooth damping** (`camera.setTarget(eye, center)`, interpolated over frames).
- **Multiple cameras** (e.g., one for the main view, one for a picture-in-picture preview Surface).

### Proposed design

Keep the immediate-mode API as the default (it matches the sketch tier), but add an optional helper:

```cpp
namespace braid {

class Camera {
public:
    Camera();

    // projection
    void perspective(float fovY, float nearZ, float farZ, float aspect);
    void ortho(float left, float right, float bottom, float top,
               float nearZ = -1.0f, float farZ = 1.0f);

    // view
    void lookAt(glm::vec3 eye, glm::vec3 center, glm::vec3 up);

    // interactive controls
    void orbit(float deltaAzimuth, float deltaPolar, float zoom);  // mouse drag / scroll
    void fly(float forward, float right, float up);                // WASD

    // matrices
    glm::mat4 view() const;
    glm::mat4 projection() const;
    glm::mat4 viewProjection() const;

    glm::vec3 eye() const;
    glm::vec3 center() const;
    glm::vec3 up() const;
};

} // namespace braid
```

In `SketchApp`, add:

```cpp
void sketchCamera(const Camera& cam);  // replaces view_ + proj_ for the current frame
```

This keeps the imperative API intact while letting advanced users carry camera state across frames.

---

## Solid primitives

### The blocker: lighting

The current default shader only handles MVP + vertex color. Solid primitives without lighting look like flat colored silhouettes. Before adding solid `sphere()`, `cone()`, etc., the default shader needs a simple lit path.

Options:

1. **Single default shader with a mode uniform:** `u.mode` switches between flat/unlit and Lambert/Blinn-Phong.
2. **Separate shaders:** keep the flat shader for 2D, add a `lit` shader for 3D solids.
3. **Always lit:** make the default shader Blinn-Phong with a default light direction. 2D shapes just get `normal = (0,0,1)` and look the same as flat color if lighting is subtle.

Option 3 is the most opinionated and keeps the API small. A single default shader with one directional light + ambient is enough for 90% of creative coding.

Suggested default-light setup:

```cpp
struct DefaultLight {
    glm::vec3 direction = glm::normalize(glm::vec3(0.3f, -0.5f, -1.0f));
    glm::vec4 ambient = {0.3f, 0.3f, 0.3f, 1.0f};
    glm::vec4 diffuse = {0.7f, 0.7f, 0.7f, 1.0f};
    glm::vec4 specular = {0.2f, 0.2f, 0.2f, 1.0f};
};
```

Expose:

```cpp
void lightDirection(glm::vec3 dir);
void lightColor(glm::vec4 ambient, glm::vec4 diffuse);
```

### Tier-1 primitives: regular OF shapes

These should live on `Mesh` as static generators (returns `Result<Mesh>`), and have convenience methods on `SketchApp`.

```cpp
// Mesh generators
static Result<Mesh> plane(...);     // exists
static Result<Mesh> cube(...);      // exists, currently shared vertices per face
static Result<Mesh> sphere(Device, float radius, int stacks = 16, int slices = 32);
static Result<Mesh> cone(Device, float radius, float height, int slices = 32);
static Result<Mesh> cylinder(Device, float radius, float height, int slices = 32, int stacks = 1);
static Result<Mesh> torus(Device, float majorRadius, float minorRadius, int majorSlices = 32, int minorSlices = 16);
static Result<Mesh> icosahedron(Device, float radius);  // platonic
static Result<Mesh> dodecahedron(Device, float radius);
static Result<Mesh> octahedron(Device, float radius);
static Result<Mesh> tetrahedron(Device, float radius);
```

`SketchApp` wrappers:

```cpp
void sphere(float radius);
void cone(float radius, float height);
void cylinder(float radius, float height);
void torus(float majorRadius, float minorRadius);
void icosahedron(float radius);
// ... etc
```

### Wireframe vs solid

The current `box()` in `SketchApp` is wireframe. Need a mode toggle:

```cpp
void fillMode();      // solid, lit or unlit depending on fill()
void wireframeMode(); // line-list
```

Or, more OF-like:

```cpp
void noFill();        // wireframe
void fill(...);       // solid
```

with `noFill()` causing `box()` to render as lines and `fill()` causing solid triangles.

### Platonic solids

These are the five regular convex polyhedra:

- Tetrahedron (4 faces)
- Cube / Hexahedron (6 faces)
- Octahedron (8 faces)
- Dodecahedron (12 faces)
- Icosahedron (20 faces)

Implementation: generate vertices from well-known coordinates, compute face indices, then optionally subdivide + normalize for spherical variants.

```cpp
static Result<Mesh> icosahedron(Device, float radius, int subdivisions = 0);
```

Subdivisions turn an icosahedron into a geodesic sphere — useful for planet/sphere approximations.

### Archimedean solids

These are semi-regular convex polyhedra: regular but not identical faces. Useful for generative art and crystalline forms.

Prioritized by usefulness:

1. **Truncated cube**
2. **Cuboctahedron**
3. **Rhombicuboctahedron**
4. **Snub cube**
5. **Icosidodecahedron**
6. **Truncated icosahedron** (soccer ball)
7. **Rhombicosidodecahedron**

Implementation strategy: rather than hardcoding coordinates for all 13 solids, provide:

- A small set of canonical generators for the most useful ones.
- A general **Wythoff construction** or **Conway polyhedron notation** parser for the rest.

Conway notation is compact and powerful:

```cpp
static Result<Mesh> conway(Device, const char* ops, float radius);
// examples:
// conway("tI")  -> truncated icosahedron
// conway("aC")  -> amplified cube
// conway("kD")  -> triangulated dodecahedron
```

This is more work upfront but gives infinite primitives from a small core.

### Composite solids

Operations that build new meshes from existing ones:

```cpp
// Extrude a 2D polygon into a 3D mesh
static Result<Mesh> extrude(Device, std::span<glm::vec2> polygon, float depth);

// Lathe / revolve a profile around an axis
static Result<Mesh> revolve(Device, std::span<glm::vec2> profile, int slices = 32);

// Subdivide all triangles (Loop-style or simple midpoint)
static Result<Mesh> subdivide(const Mesh& src, int iterations = 1);

// Normalize all vertices to a sphere (spherify)
static Result<Mesh> spherify(const Mesh& src, float radius = 1.0f);

// Invert face winding / compute convex hull (later)
```

These fit the "model the artist's mind" north star: extrude and revolve are mental operations, not GPU operations.

---

## Mesh loading

### Format choice

| Format | Pros | Cons |
|--------|------|------|
| **PLY** | Simple, supports colors/normals/UVs, ASCII and binary | Less common for assets |
| **OBJ** | Ubiquitous, tiny parsers exist | No vertex colors, limited material |
| **glTF** | Modern, full PBR, animations | Complex, needs a dedicated library |
| **STL** | Common in 3D printing / fabrication | No UVs, no colors, triangles only |

For Braid's niche (creative coding, generative art, installations), the right ladder is:

1. **PLY first** — custom parser, ~100 lines, covers vertex colors and point clouds.
2. **OBJ second** — `tinyobjloader` is a single header, perfect for this codebase.
3. **glTF later** — only when animation / skinning is needed.

### Proposed API

```cpp
namespace braid {

class Mesh {
    // existing generators ...

    static Result<Mesh> loadPLY(wgpu::Device device, const char* path);
    static Result<Mesh> loadOBJ(wgpu::Device device, const char* path);
    static Result<Mesh> loadSTL(wgpu::Device device, const char* path);

    // async variant via Channel
    static void loadPLYAsync(wgpu::Device device, const char* path,
                             Channel<Result<Mesh>>& out);
};

} // namespace braid
```

Keep `mango` for images; do not use it for meshes. A single-header loader (`tinyobjloader`) keeps the dependency story clean.

### Point clouds

PLY is often used for point clouds. Add:

```cpp
static Result<Mesh> pointCloud(wgpu::Device device, std::span<glm::vec3> points,
                               std::span<glm::vec4> colors = {});
```

and render with `PointList` topology. This connects directly to the Kinect/depth-camera / LiDAR workflow common in installations.

---

## Design principles to keep

1. **Primitives return `Result<Mesh>`.** Failures (bad parameters, empty spans) are explicit.
2. **Generators produce interleaved `Vertex` data.** No special formats — everything flows into the existing `Mesh` class.
3. **SketchApp stays optional.** All geometry should be usable from Tier 1 directly.
4. **Normals and UVs are always generated.** Even if unused today, they make the mesh future-proof for texturing and lighting.
5. **No heavy dependencies.** Prefer procedural generation and single-header parsers over Assimp.

---

## Suggested implementation order

1. **Lit default shader** — ambient + one directional light. This unlocks all solid primitives.
2. **Solid OF primitives** — `sphere`, `cone`, `cylinder`, `torus` on `Mesh` and `SketchApp`.
3. **Wireframe/solid mode toggle** — decide whether `noFill()` means wireframe globally.
4. **Platonic solids** — tetrahedron, octahedron, dodecahedron, icosahedron.
5. **PLY loader** — point clouds and colored meshes.
6. **OBJ loader** via `tinyobjloader`.
7. **Archimedean solids** — start with truncated cube, cuboctahedron, soccer ball.
8. **Composite operations** — `extrude`, `revolve`, `subdivide`.
9. **Camera object** — orbit + fly helpers, once 3D examples need it.
10. **glTF** — only when someone asks for animation.

---

## Summary

Camera is mostly there; a helper object would complete it. Solid primitives need a lit shader first, then the OF basics, then Platonic/Archimedean solids. PLY and OBJ loading should come before any heavy library like Assimp. Keep everything generating the same `Vertex` layout and returning `Result<Mesh>` so the Surface thesis stays intact.
