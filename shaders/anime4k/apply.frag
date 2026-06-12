#version 450

// Fused Anime4K_Upscale_DoG_x2. Copyright (c) 2019-2021 bloc97, MIT.
// This is algebraically equivalent to the release's luma, Gaussian X/Y and
// apply passes, fused to avoid additional-sampler crashes in SDL 3.4's Vulkan
// renderer.
layout(set = 2, binding = 0) uniform sampler2D source_image;
layout(location = 0) in vec4 color;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec4 output_color;

float luma(vec2 position)
{
    return dot(texture(source_image, position).rgb, vec3(0.299, 0.587, 0.114));
}

void main()
{
    const float strength = 0.8;
    const float w0 = 0.38774;
    const float w1 = 0.24477;
    const float w2 = 0.06136;
    vec2 pixel = 1.0 / textureSize(source_image, 0);
    float center = luma(uv);
    float minimum = 1.0;
    float maximum = 0.0;
    float gaussian = 0.0;
    for (int y = -2; y <= 2; ++y) {
        float wy = y == 0 ? w0 : abs(y) == 1 ? w1 : w2;
        for (int x = -2; x <= 2; ++x) {
            float wx = x == 0 ? w0 : abs(x) == 1 ? w1 : w2;
            float sample_luma = luma(uv + vec2(x, y) * pixel);
            gaussian += sample_luma * wx * wy;
            if (abs(x) <= 1 && abs(y) <= 1) {
                minimum = min(minimum, sample_luma);
                maximum = max(maximum, sample_luma);
            }
        }
    }
    float correction = (center - gaussian) * strength;
    correction = clamp(correction + center, minimum, maximum) - center;
    output_color = texture(source_image, uv) * color
        + vec4(vec3(correction), 0.0);
}
