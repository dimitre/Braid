struct VertexInput {
    @location(0) position: vec2<f32>,
    @location(1) side: f32,
    @location(2) tangent: vec2<f32>,
    @location(3) distortion: vec2<f32>,
};

struct Uniforms {
    transform: mat4x4<f32>,
    color: vec4<f32>,
    thickness: f32,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec4<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var pos = in.position + in.distortion;
    let perp = vec2<f32>(-in.tangent.y, in.tangent.x);
    pos = pos + in.side * u.thickness * perp;

    var out: VertexOutput;
    out.position = u.transform * vec4<f32>(pos, 0.0, 1.0);
    out.color = u.color;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    return in.color;
}
