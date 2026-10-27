#version 330
in vec3 fragWorld;
in vec3 fragNormal;
in vec4 fragColor;

uniform vec4 colDiffuse;
uniform vec3 sunDir;          // towards the sun
uniform vec3 sunColor;
uniform vec3 ambientColor;
uniform vec3 playerPos;
uniform vec3 viewDir;         // from the camera into the scene
uniform float cutRadius;      // see-through radius around the player (0 = off)
uniform float cutaway;        // 1 for meshes that may hide the player
uniform vec4 lights[16];      // xyz position, w radius
uniform vec4 lightColors[16]; // rgb colour, a intensity
uniform int lightCount;
uniform float time;
uniform vec3 fogColor;
uniform float fogStart;
uniform float fogEnd;
uniform float unlit;          // 1: skip lighting (fire, glow)

out vec4 finalColor;

float bayer4(vec2 p) {
    int x = int(mod(p.x, 4.0)), y = int(mod(p.y, 4.0));
    int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    return float(m[y * 4 + x]) / 16.0;
}

void main() {
    vec4 base = fragColor * colDiffuse;

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
    vec3 c;
    if (unlit > 0.5) {
        c = base.rgb;
    } else {
        float sun = max(dot(n, normalize(sunDir)), 0.0);
        vec3 light = ambientColor + sunColor * sun;
        for (int i = 0; i < lightCount; ++i) {
            vec3 lp = lights[i].xyz;
            float r = lights[i].w;
            vec3 dl = lp - fragWorld;
            float dist = length(dl);
            float att = clamp(1.0 - dist / r, 0.0, 1.0);
            att *= att;
            float facing = max(dot(n, dl / max(dist, 0.001)), 0.0) * 0.6 + 0.4;
            light += lightColors[i].rgb * lightColors[i].a * att * facing;
        }
        c = base.rgb * light;
    }

    // Water: vertex alpha below 1 marks water, give it a gentle shimmer
    if (base.a < 0.99) {
        float s = sin(fragWorld.x * 1.3 + time * 1.1) * sin(fragWorld.z * 1.7 - time * 0.8);
        c += vec3(0.03, 0.04, 0.05) * s * (ambientColor + sunColor);
    }

    // Distance fog from the player (orthographic camera: depth is meaningless)
    float fd = length(fragWorld.xz - playerPos.xz);
    float fog = clamp((fd - fogStart) / max(fogEnd - fogStart, 0.01), 0.0, 1.0);
    c = mix(c, fogColor, fog);
    finalColor = vec4(c, 1.0);
}
