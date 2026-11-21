// Adapted from Open Engine Simulator 1e226ee7 and Delta Studio b7d0a046.
// MIT, Ange Yaghi; full grant and origins in LICENSE.txt.
// SDL vertex uniforms use set 1; fragment sampler uses set 2 and uniforms set 3.
cbuffer VertexUniforms : register(b0, space1) {
    float4x4 transform;
    float4x4 cameraView;
    float4x4 projection;
};
struct VertexInput {
    float4 position : TEXCOORD0;
    float4 normal : TEXCOORD1;
    float2 texcoord : TEXCOORD2;
};
struct VertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};
VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    // Delta's standard vertex shader extends generated positions to homogeneous points.
    output.position = mul(float4(input.position.xyz, 1.0), transform);
    output.position = mul(output.position, cameraView);
    output.position = mul(output.position, projection);
    output.texcoord = input.texcoord;
    return output;
}
cbuffer FragmentUniforms : register(b0, space3) {
    float4 baseColor;
    uint glyph;
    uint padding0;
    uint padding1;
    uint padding2;
};
Texture2D<float4> glyphAtlas : register(t0, space2);
SamplerState glyphSampler : register(s0, space2);
float srgb(float value) {
    return value < 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}
float4 PSMain(VertexOutput input) : SV_Target0 {
    if (glyph != 0) return float4(baseColor.rgb, baseColor.a * glyphAtlas.Sample(glyphSampler, input.texcoord).r);
    return float4(srgb(baseColor.r), srgb(baseColor.g), srgb(baseColor.b), baseColor.a);
}
