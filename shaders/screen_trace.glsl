// Screen-space rays through the last frame's picture (prevColor_, binding 18: colour, and distance along its view
// in a; mipped), for reflections and global illumination. scene.frag traces them per pixel (MSAA, see-through
// surfaces); without MSAA, ssr_trace.frag and gi_trace.frag trace them at half size beforehand (renderer_trace.cpp).

layout(set = 0, binding = 18) uniform sampler2D lastFrame;

// The last frame's picture's level (it's mipped) as blurred as a cone `width` metres across, `dist` metres away
float pictureLevel(float width, float dist) {
    float pixels = width / max(dist * 2.0 * frame.camForward.w, 1e-3) * frame.prevViewport.w;
    return clamp(log2(max(pixels, 1.0)), 0.0, float(textureQueryLevels(lastFrame) - 1));
}

// Screen-space reflection from `pos` along r: marches through the last frame's picture (each point placed with its
// camera) until it passes just behind what was drawn there; rgb = what it hit (as blurred as a surface this rough
// spreads it over the distance), a = how much to trust it (0: nothing). Hierarchical: where nothing in the ray's
// 8 x 8 tile of the picture is as near as the ray (the near pyramid, binding 39), it jumps to where the ray leaves
// the tile on screen; elsewhere it steps a couple of pixels at a time. Long rays (to 120 m) in few samples.
layout(set = 0, binding = 39) uniform sampler2D lastFrameNear;   // (the nearest distance per 2 x 2 up to 32 x 32)
vec4 traceReflection(vec3 pos, vec3 r, float rough, vec2 pixel) {
    vec2 size = vec2(textureSize(lastFrame, 0));
    vec3 start = pos + r * 0.05;
    // A point t along the ray, in the last frame's clip space and along its view: both straight lines in t, so
    // projected once here and stepped with additions below
    vec4 clip0 = frame.prevViewProj * vec4(start, 1.0), clipDir = frame.prevViewProj * vec4(r, 0.0);
    float z0 = dot(start - frame.prevCamPos.xyz, frame.prevCamForward.xyz), zDir = dot(r, frame.prevCamForward.xyz);
    vec2 scale = frame.prevViewport.zw * 0.5 / size, offset = (frame.prevViewport.xy + frame.prevViewport.zw * 0.5) / size;
    const float maxDistance = 200.0;
    // (medium quality: fewer samples, longer steps where it steps, less refining)
    bool fine = frame.shadowParams.z > 1.5;
    int samples = fine ? 56 : 36, refine = fine ? 5 : 3;
    float stepPixels = fine ? 2.0 : 3.5, grow = fine ? 1.12 : 1.21;
    float t = 0.1, last = 0.0, step = 0.12;
    int level = 2;   // (the pyramid's level the ray steps over: 8 x 8 tiles to start; up to 32 x 32 over open space)
    for (int i = 0; i < samples && t < maxDistance; ++i) {
        vec4 clip = clip0 + clipDir * t;
        if (clip.w <= 1e-4) break;
        vec2 ndc = clip.xy / clip.w;
        if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) break;   // (off the picture)
        vec2 uv = ndc * scale + offset;
        float rayZ = z0 + zDir * t;
        // (how fast the ray crosses the picture here: pixels per metre along it)
        vec4 ahead = clip0 + clipDir * (t + 0.25);
        vec2 screenDir = ahead.w > 1e-4 ? ((ahead.xy / ahead.w) * scale + offset - uv) * size / 0.25 : vec2(0.0);
        float speed = max(length(screenDir), 1e-3);
        // Where the ray leaves this tile of the pyramid's level on screen; if nothing in the tile is as near as the
        // ray anywhere in it (its farthest there: where it leaves, going away; or here, coming nearer), straight to
        // that edge, a level up (bigger tiles) for the next; else a level down, down to the 8 x 8 tiles, then finer
        float kTile = exp2(float(level + 1));
        vec2 at = uv * size, inTile = mod(at, kTile);
        vec2 toEdge = vec2(screenDir.x > 0.0 ? kTile - inTile.x : inTile.x, screenDir.y > 0.0 ? kTile - inTile.y : inTile.y);
        vec2 times = toEdge / max(abs(screenDir), vec2(1e-3));
        float exitT = min(times.x, times.y) + 0.5 / speed;
        float deepest = max(rayZ, z0 + zDir * (t + exitT));
        // (the step grows along the ray, as before: long rays in few samples)
        step *= grow;
        if (deepest < textureLod(lastFrameNear, uv, float(level)).r - 0.1) {   // (clear: its edge or a step, the further)
            last = t;
            t += max(exitT, step);
            level = min(level + 1, 4);
            continue;
        }
        if (level > 2) {   // (something as near in this big tile: look again at the smaller ones round here)
            --level;
            step /= grow;   // (no further along: the step doesn't grow)
            continue;
        }
        // Something in this tile as near: finer, a couple of pixels at a time (thin things aren't stepped over)
        float fineStep = min(step, max(stepPixels / speed, 0.04));
        float sceneZ = textureLod(lastFrame, uv, 0.0).a;
        // (behind it by less than a step, or a little more: thin things aren't missed between two samples)
        if (rayZ > sceneZ && rayZ < sceneZ + max(0.25 + t * 0.04, (t - last) * 1.5)) {
            float lo = last, hi = t;   // (the crossing, closer)
            for (int k = 0; k < refine; ++k) {
                float mid = 0.5 * (lo + hi);
                vec4 c = clip0 + clipDir * mid;
                if (c.w <= 1e-4) break;   // (behind the last frame's camera: no point on its picture)
                vec2 u = c.xy / c.w * scale + offset;
                if (z0 + zDir * mid > textureLod(lastFrame, u, 0.0).a) hi = mid;
                else lo = mid;
                uv = u;
            }
            vec2 edge = smoothstep(vec2(1.0), vec2(0.85), abs(ndc));   // (fades out at the picture's edges)
            vec3 seen = textureLod(lastFrame, uv, pictureLevel(t * rough * rough * 1.6, max(rayZ, 0.1))).rgb;
            if (any(isnan(seen)) || any(isinf(seen))) return vec4(0.0);
            return vec4(min(seen, vec3(64.0)), edge.x * edge.y * (1.0 - t / maxDistance));
        }
        last = t;
        t += fineStep;
    }
    return vec4(0.0);
}

// Screen-space global illumination at `pos`: `rays` rays over the hemisphere around n (cosine-weighted, turned
// per pixel and, with temporal AA or the half-size passes' history, every frame), marched through the last frame's
// picture, up to 10 m: where the near pyramid's 16 x 16 tiles where the ray is and where it would be three steps on
// hold nothing as near as the ray gets, it takes those three steps at once (a long jump doesn't look at the tiles
// it crosses between: a thin thing there may let a little light past, which a bounce's blur hides) (open space crossed quickly, the steps kept for near things). rgb = the average of what they met (read two levels down: blurred, less noise), a = how much they met
// (0..1: the rest is the probes' and the sky's light). The picture holds last frame's bounces: light bounces again.
vec4 traceIrradiance(vec3 pos, vec3 n, vec2 pixel, int rays, int steps, float spin) {
    float grow = steps >= 8 ? 1.4 : 1.55;
    const float maxDistance = 10.0;
    vec2 size = vec2(textureSize(lastFrame, 0));
    vec2 scale = frame.prevViewport.zw * 0.5 / size, offset = (frame.prevViewport.xy + frame.prevViewport.zw * 0.5) / size;
    float noise = fract(52.9829189 * fract(dot(pixel + spin, vec2(0.06711056, 0.00583715))));
    vec3 t1 = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), t2 = cross(n, t1);
    vec3 start = pos + n * 0.05;
    vec4 clip0 = frame.prevViewProj * vec4(start, 1.0);
    float z0 = dot(start - frame.prevCamPos.xyz, frame.prevCamForward.xyz);
    vec3 sum = vec3(0.0);
    float met = 0.0;
    for (int i = 0; i < rays; ++i) {
        float u = fract(noise + float(i) / float(rays)), phi = (float(i) + noise) * 2.39996 * 2.0 + noise * 6.2832;
        vec3 d = normalize(t1 * (cos(phi) * sqrt(u)) + t2 * (sin(phi) * sqrt(u)) + n * sqrt(1.0 - u));
        vec4 clipDir = frame.prevViewProj * vec4(d, 0.0);
        float zDir = dot(d, frame.prevCamForward.xyz);
        float t = 0.08 + 0.1 * noise, step = 0.12;
        for (int k = 0; k < steps && t < maxDistance; ++k) {
            vec4 clip = clip0 + clipDir * t;
            if (clip.w <= 1e-4) break;
            vec2 ndc = clip.xy / clip.w;
            if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) break;
            vec2 uv = ndc * scale + offset;
            float rayZ = z0 + zDir * t;
            float jump = step * (1.0 + grow + grow * grow);   // (three steps on: clear where it starts and where it lands)
            vec4 landing = clip0 + clipDir * (t + jump);
            vec2 landUv = landing.w > 1e-4 ? landing.xy / landing.w * scale + offset : uv;
            float deepest = max(rayZ, z0 + zDir * (t + jump));
            if (deepest < min(textureLod(lastFrameNear, uv, 3.0).r, textureLod(lastFrameNear, landUv, 3.0).r) - 0.1) {
                t += jump;
                step *= grow * grow * grow;
                continue;
            }
            float sceneZ = textureLod(lastFrame, uv, 1.0).a;   // (half size: enough, and quicker)
            // (behind it by no more than its thickness might be: growing with the distance, as the steps do, up to
            // 4 m's; far hits fade out faster, being the less sure: a long ray's sporadic hits are grain)
            if (rayZ > sceneZ && rayZ < sceneZ + 0.3 + min(t, 4.0) * 0.15) {
                vec2 edge = smoothstep(vec2(1.0), vec2(0.8), abs(ndc));
                vec3 seen = textureLod(lastFrame, uv, 2.0).rgb;
                if (!any(isnan(seen)) && !any(isinf(seen))) {
                    float fade = 1.0 - t / maxDistance;
                    float w = edge.x * edge.y * fade * fade;
                    sum += min(seen, vec3(16.0)) * w;
                    met += w;
                }
                break;
            }
            t += step;
            step *= grow;
        }
    }
    return vec4(met > 0.0 ? sum / met : vec3(0.0), met / float(rays));
}
