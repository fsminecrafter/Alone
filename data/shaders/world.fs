#version 330
// World shader.
//  - Surface detail is procedural, chosen per vertex by a material id
//    (vertexTexCoord.x) and projected in world space, so meshes need no UVs.
//  - Sun/moon shadow map with soft PCF.
//  - Up to 32 coloured point lights plus a flashlight cone.
//  - See-through cutaway around the player, vision cone, snow, wetness, fog.
in vec3 fragWorld;
in vec3 fragNormal;
in vec4 fragColor;
in vec2 fragMat;

uniform vec4 colDiffuse;
uniform vec3 sunDir;          // towards the sun (or moon)
uniform vec3 sunColor;
uniform vec3 ambientColor;
uniform vec3 skyColor;        // for upward-facing ambient
uniform vec3 playerPos;
uniform vec3 viewDir;         // from the camera into the scene
uniform float cutRadius;
uniform float cutaway;
uniform vec4 lights[32];      // xyz position, w radius
uniform vec4 lightColors[32]; // rgb, a intensity
uniform int lightCount;
uniform vec3 spotPos;
uniform vec3 spotDir;
uniform float spotOn;
uniform float time;
uniform vec3 fogColor;
uniform float fogStart;
uniform float fogEnd;
uniform float unlit;
uniform float snow;           // 0..1 snow cover on upward faces
uniform float wet;            // 0..1 rain-darkened ground
uniform vec3 emissive;        // lit windows of the current building draw
uniform vec3 lampGlow;        // street lamp heads (while there is power, at night)
uniform float visionCone;     // 1 to darken what the player can't see
uniform vec2 facing;          // player facing (xz)
uniform mat4 lightVP;
uniform sampler2D shadowMap;
uniform float shadowOn;
uniform float shadowTexel;
uniform float brightness;    // player setting, 1 = neutral

out vec4 finalColor;

// ---------------------------------------------------------------------------
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { s += vnoise(p) * a; p *= 2.07; a *= 0.5; }
    return s;
}
// Thin dark line at integer crossings of x (antialiased)
float grout(float x, float width) {
    float d = abs(fract(x) - 0.5);
    float w = fwidth(x) * 1.5 + width;
    return smoothstep(0.5 - w, 0.5 - w * 0.4, d);
}
float bayer4(vec2 p) {
    int x = int(mod(p.x, 4.0)), y = int(mod(p.y, 4.0));
    int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    return float(m[y * 4 + x]) / 16.0;
}

// Returns a brightness multiplier (and may tint) for a material.
vec3 material(int id, float param, vec3 p, vec3 n, vec3 base) {
    vec2 uv;
    if (abs(n.y) > 0.6) uv = p.xz;
    else if (abs(n.x) > abs(n.z)) uv = vec2(p.z, p.y);
    else uv = vec2(p.x, p.y);
    float m = 1.0;
    if (id == 1) {            // grass
        float g = fbm(uv * 1.3);
        float blades = vnoise(uv * vec2(18.0, 22.0));
        m = 0.82 + 0.3 * g + 0.12 * (blades - 0.5);
        base = mix(base, base * vec3(1.12, 1.05, 0.75), smoothstep(0.55, 0.8, fbm(uv * 0.21)));
    } else if (id == 2) {     // forest floor: needles, moss patches
        float g = fbm(uv * 2.2);
        m = 0.75 + 0.45 * g + 0.15 * (vnoise(uv * 25.0) - 0.5);
        base = mix(base, vec3(0.30, 0.38, 0.18), smoothstep(0.55, 0.75, fbm(uv * 0.35)) * 0.6);
    } else if (id == 3) {     // dirt / gravel
        m = 0.8 + 0.3 * fbm(uv * 3.0) + 0.25 * (vnoise(uv * 40.0) - 0.5);
    } else if (id == 4) {     // asphalt
        m = 0.9 + 0.12 * fbm(uv * 0.6) + 0.18 * (vnoise(uv * 60.0) - 0.5);
        float crack = smoothstep(0.03, 0.0, abs(fbm(uv * 0.45) - 0.5)) * step(0.6, vnoise(uv * 0.2));
        m *= 1.0 - 0.35 * crack;
    } else if (id == 5) {     // paving slabs 0.6 m
        vec2 q = uv / 0.6;
        q.x += step(1.0, mod(floor(q.y), 2.0)) * 0.5;
        m = 0.92 + 0.12 * hash(floor(q)) + 0.06 * fbm(uv * 4.0);
        m *= mix(0.68, 1.0, grout(q.x, 0.03) * grout(q.y, 0.03));
    } else if (id == 6) {     // cobbles
        vec2 q = uv / 0.22;
        q.x += step(1.0, mod(floor(q.y), 2.0)) * 0.5;
        vec2 f = fract(q) - 0.5;
        float r = length(f * vec2(1.0, 1.15));
        m = (0.78 + 0.3 * hash(floor(q))) * mix(1.0, 0.55, smoothstep(0.32, 0.5, r));
    } else if (id == 7) {     // wooden siding: horizontal (param 0) or vertical boards (param 1)
        float c = param > 0.5 ? (abs(n.x) > abs(n.z) ? p.z : p.x) / 0.2 : p.y / 0.19;
        float plank = floor(c);
        float edge = smoothstep(0.0, 0.12 + fwidth(c), fract(c));
        m = (0.92 + 0.08 * hash(vec2(plank, 3.0))) * mix(0.62, 1.0, edge);
        m *= 0.95 + 0.08 * vnoise(vec2(uv.x * 0.7, c * 3.0));
    } else if (id == 8) {     // brick
        vec2 q = vec2(uv.x / 0.25, uv.y / 0.075);
        q.x += step(1.0, mod(floor(q.y), 2.0)) * 0.5;
        float h = hash(floor(q));
        m = 0.85 + 0.25 * h;
        float mortar = grout(q.x, 0.04) * grout(q.y, 0.1);
        base = mix(vec3(0.78, 0.76, 0.72), base * m, mortar);
        m = 1.0;
    } else if (id == 9) {     // plaster / render
        m = 0.92 + 0.12 * fbm(uv * 1.7) + 0.04 * (vnoise(uv * 30.0) - 0.5);
    } else if (id == 10 || id == 11) {   // roof tiles (10) / shingles (11), rows across the slope
        float across = param > 0.5 ? p.z : p.x;
        float along = param > 0.5 ? p.x : p.z;
        float rowW = id == 10 ? 0.32 : 0.25;
        float c = across / rowW;
        float row = floor(c);
        float off = mod(row, 2.0) * 0.5;
        float col = along / (id == 10 ? 0.24 : 0.4) + off;
        float shadeRow = mix(0.62, 1.0, smoothstep(0.0, 0.3 + fwidth(c), fract(c)));
        float gaps = id == 11 ? grout(col, 0.04) : mix(0.8, 1.0, abs(sin(fract(col) * 3.1416)));
        m = shadeRow * gaps * (0.9 + 0.15 * hash(vec2(floor(col), row)));
    } else if (id == 12) {    // wooden floor planks along x
        float c = p.z / 0.16;
        float plank = floor(c);
        float joint = grout(p.x / 1.8 + hash(vec2(plank, 1.0)), 0.004);
        m = (0.85 + 0.2 * hash(vec2(plank, 7.0))) * mix(0.7, 1.0, grout(c, 0.03)) * mix(0.75, 1.0, joint);
        m *= 0.95 + 0.1 * vnoise(vec2(p.x * 1.5, c * 6.0));
    } else if (id == 13) {    // floor tiles 0.3 m
        vec2 q = uv / 0.3;
        m = (0.95 + 0.06 * hash(floor(q))) * mix(0.75, 1.0, grout(q.x, 0.03) * grout(q.y, 0.03));
    } else if (id == 14) {    // carpet / fabric
        m = 0.9 + 0.12 * fbm(uv * 6.0) + 0.1 * (vnoise(uv * 90.0) - 0.5);
    } else if (id == 15) {    // bark
        m = 0.75 + 0.35 * vnoise(vec2((p.x + p.z) * 9.0, p.y * 1.2)) + 0.1 * (vnoise(vec2(p.x * 30.0, p.y * 8.0)) - 0.5);
    } else if (id == 16) {    // birch bark: white with dark marks
        float marks = step(0.78, vnoise(vec2((p.x + p.z) * 6.0, p.y * 4.0)));
        m = (0.95 + 0.08 * vnoise(vec2(p.y * 5.0, p.x * 20.0))) * (1.0 - 0.75 * marks);
    } else if (id == 17) {    // foliage
        float g = vnoise(uv * 3.1 + vec2(p.y * 2.3));
        m = 0.72 + 0.5 * g;
        base = mix(base, base * vec3(1.2, 1.15, 0.85), step(0.72, vnoise(uv * 5.7 + p.y)));
    } else if (id == 18) {    // glass
        float streak = smoothstep(0.2, 0.0, abs(fract((uv.x + uv.y) * 0.8) - 0.5) - 0.3);
        m = 0.9 + 0.35 * streak;
    } else if (id == 19) {    // water
        float t = time;
        float w = vnoise(uv * 0.9 + vec2(t * 0.15, t * 0.1)) * 0.6 + vnoise(uv * 2.7 - vec2(t * 0.2, -t * 0.12)) * 0.4;
        m = 0.85 + 0.3 * w;
    } else if (id == 20) {    // concrete
        vec2 q = uv / 2.0;
        m = (0.9 + 0.1 * fbm(uv * 2.5) + 0.06 * hash(floor(q))) * mix(0.8, 1.0, grout(q.x, 0.004) * grout(q.y, 0.004));
    } else if (id == 21) {    // metal / painted surfaces
        m = 0.96 + 0.06 * vnoise(uv * 8.0);
    }
    return base * m;
}

float shadowAt(vec3 world, vec3 n) {
    if (shadowOn < 0.5) return 1.0;
    vec4 ls = lightVP * vec4(world + n * 0.06, 1.0);
    vec3 pc = ls.xyz / ls.w * 0.5 + 0.5;
    if (pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0 || pc.z > 1.0) return 1.0;
    float bias = 0.0015;
    float lit = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float d = texture(shadowMap, pc.xy + vec2(x, y) * shadowTexel).r;
            lit += pc.z - bias > d ? 0.0 : 1.0;
        }
    return lit / 9.0;
}

void main() {
    vec4 base = fragColor * colDiffuse;
    int mat = int(fragMat.x + 0.5);

    if (cutaway > 0.5 && cutRadius > 0.0) {
        vec3 rel = fragWorld - playerPos;
        float towardsCamera = -dot(rel, viewDir);
        vec3 perp = rel + viewDir * towardsCamera;
        float d = length(perp);
        if (towardsCamera > 0.25 && rel.y > -0.1) {
            float edge = smoothstep(cutRadius, cutRadius * 0.6, d);
            if (edge > bayer4(gl_FragCoord.xy) * 0.999) discard;
        }
    }

    vec3 n = normalize(fragNormal);
    if (!gl_FrontFacing) n = -n;
    vec3 albedo = material(mat, fragMat.y, fragWorld, n, base.rgb);

    // Snow settles on upward faces; rain darkens the ground
    float up = smoothstep(0.55, 0.9, n.y);
    if (snow > 0.0 && mat != 19 && mat != 18) {
        float patchy = smoothstep(0.25, 0.75, snow + (vnoise(fragWorld.xz * 0.7) - 0.5) * 0.5);
        albedo = mix(albedo, vec3(0.86, 0.88, 0.92), up * patchy);
    }
    if (wet > 0.0 && up > 0.5 && mat != 19) albedo *= 1.0 - 0.28 * wet;

    vec3 c;
    if (unlit > 0.5) {
        c = albedo;
    } else {
        float sun = max(dot(n, normalize(sunDir)), 0.0);
        float sh = sun > 0.0 ? shadowAt(fragWorld, n) : 1.0;
        // Sky from above, ground bounce from below, darker near the ground (cheap AO)
        float ao = mix(0.72, 1.0, smoothstep(0.0, 0.9, fragWorld.y + 0.05));
        vec3 amb = mix(ambientColor * 0.75, ambientColor * 0.6 + skyColor * 0.25, n.y * 0.5 + 0.5) * ao;
        vec3 light = amb + sunColor * sun * sh;
        for (int i = 0; i < lightCount; ++i) {
            vec3 dl = lights[i].xyz - fragWorld;
            float dist = length(dl);
            float r = lights[i].w;
            float att = clamp(1.0 - dist / r, 0.0, 1.0);
            att = att * att * (3.0 - 2.0 * att);
            float facingL = max(dot(n, dl / max(dist, 0.001)), 0.0) * 0.7 + 0.3;
            light += lightColors[i].rgb * lightColors[i].a * att * facingL;
        }
        if (spotOn > 0.5) {
            vec3 dl = fragWorld - spotPos;
            float dist = length(dl);
            float cone = smoothstep(0.82, 0.95, dot(dl / max(dist, 0.001), spotDir));
            float att = clamp(1.0 - dist / 20.0, 0.0, 1.0);
            light += vec3(1.0, 0.95, 0.82) * 1.6 * cone * att * att * (max(dot(n, -dl / max(dist, 0.001)), 0.0) * 0.7 + 0.3);
        }
        c = albedo * light;
    }
    // Lit windows glow at night
    if (mat == 18) c += emissive;
    if (mat == 22) c = albedo * 0.4 + lampGlow;

    // What the player can't see is darker (behind them, far away)
    if (visionCone > 0.5) {
        vec2 d = fragWorld.xz - playerPos.xz;
        float dist = length(d);
        float ang = dot(d / max(dist, 0.001), facing);
        float seen = max(smoothstep(-0.55, 0.0, ang), 1.0 - smoothstep(3.0, 6.0, dist));
        c *= mix(0.72, 1.0, seen);
    }

    float fd = length(fragWorld.xz - playerPos.xz);
    float fog = clamp((fd - fogStart) / max(fogEnd - fogStart, 0.01), 0.0, 1.0);
    c = mix(c, fogColor, fog);
    finalColor = vec4(c * brightness, 1.0);
}
