#version 450
// The overlay's colours, encoded for the swapchain they actually land on.
//
// ImGui's stock fragment shader writes the vertex colour times the texture,
// which is right when the swapchain is SRGB_NONLINEAR and wrong everywhere
// else: on an HDR10 swapchain those sRGB-encoded values are read as PQ, where
// 1.0 means ten thousand nits -- every white in the panel becomes a torch and
// every colour oversaturates. This shader is the same sample-and-multiply with
// one extra step chosen at pipeline creation (specialization constants, so the
// driver folds the branch away): decode sRGB, then encode for the target.
//
// kSdrNits is where the overlay's white lands in the HDR range: reference
// SDR white, 203 nits by default (ITU-R BT.2408's diffuse white), because the
// overlay must read like an SDR interface floating over the scene, not compete
// with the scene's own highlights.
//
// Blending still happens in the target's non-linear space, exactly as ImGui
// blends in sRGB space on an SDR swapchain: technically non-linear, visually
// the look every UI is tuned against.

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

// 0 = untouched, 1 = scRGB (extended sRGB linear), 2 = HDR10 PQ,
// 3 = an sRGB-format attachment, where the hardware does the encoding.
layout(constant_id = 0) const int kMode = 0;
layout(constant_id = 1) const float kSdrNits = 203.0;

vec3 srgb_to_linear(vec3 c)
{
    // The piecewise EOTF, not the pow(2.2) shortcut: the dark end is where a
    // translucent panel lives, and that is exactly where the shortcut is wrong.
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 bt709_to_bt2020(vec3 c)
{
    // Column-major, rows of the standard matrix: BT.2087's coefficients.
    const mat3 m = mat3(0.6274, 0.0691, 0.0164,
                        0.3293, 0.9195, 0.0880,
                        0.0433, 0.0114, 0.8956);
    return m * c;
}

vec3 pq_encode(vec3 nits)
{
    // SMPTE ST 2084 inverse EOTF, reference constants.
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 y = clamp(nits / 10000.0, vec3(0.0), vec3(1.0));
    vec3 ym = pow(y, vec3(m1));
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), vec3(m2));
}

void main()
{
    vec4 c = In.Color * texture(sTexture, In.UV.st);
    if (kMode == 1) {
        // scRGB: linear values where 1.0 is 80 nits.
        c.rgb = srgb_to_linear(c.rgb) * (kSdrNits / 80.0);
    } else if (kMode == 2) {
        // HDR10: linear light in BT.2020 primaries, PQ-encoded.
        c.rgb = pq_encode(bt709_to_bt2020(srgb_to_linear(c.rgb)) * kSdrNits);
    } else if (kMode == 3) {
        // An sRGB-format attachment. Nothing to do with HDR, and the most
        // common swapchain there is: the format itself carries the encoding, so
        // the hardware applies linear->sRGB to whatever this shader writes.
        // ImGui's colours are already sRGB, so writing them unchanged encodes
        // them twice -- measured on an R8G8B8A8_SRGB attachment, the panel's own
        // 79,84,92 came back 151,155,162, a light grey where the theme asks for
        // dark slate. Handing the hardware the linear values it expects makes
        // the round trip exact. Blending then happens in linear light rather
        // than in sRGB space, which is the one thing this cannot put back: it is
        // the attachment's own rule, and it is what the game's own draws get.
        c.rgb = srgb_to_linear(c.rgb);
    }
    fColor = c;
}
