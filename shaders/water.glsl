// Water's waves (scene/water.h has the C++ reference: the same sums): three Gerstner waves from the main one, each
// point of the surface going round in a circle. `wave`: height, length, direction (world, radians), choppiness.
void gerstner(vec2 xz, float t, vec4 wave, out vec3 offset, out vec3 normal) {
    const float turns[3] = float[](0.0, 0.6108652, -0.4363323);   // (0, 35, -25 degrees)
    const float lengths[3] = float[](1.0, 0.57, 0.31), heights[3] = float[](1.0, 0.45, 0.2);
    offset = vec3(0.0);
    vec3 tx = vec3(1.0, 0.0, 0.0), tz = vec3(0.0, 0.0, 1.0);   // (the moved surface's slopes: its normal from them)
    for (int i = 0; i < 3; ++i) {
        float amplitude = max(wave.x, 0.0) * heights[i];
        if (amplitude <= 0.0) continue;
        float a = wave.z + turns[i];
        vec2 d = vec2(cos(a), sin(a));
        float k = 6.2831853 / max(wave.y * lengths[i], 0.05);
        float speed = sqrt(9.81 / k);
        float steep = clamp(wave.w, 0.0, 1.0) / (k * amplitude * 3.0);
        float f = k * (dot(d, xz) - speed * t), c = cos(f), s = sin(f);
        float qa = steep * amplitude;
        offset += vec3(qa * d.x * c, amplitude * s, qa * d.y * c);
        float ka = k * amplitude, kqa = k * qa;
        tx += vec3(-kqa * d.x * d.x * s, ka * d.x * c, -kqa * d.x * d.y * s);
        tz += vec3(-kqa * d.x * d.y * s, ka * d.y * c, -kqa * d.y * d.y * s);
    }
    normal = normalize(cross(tz, tx));
}
