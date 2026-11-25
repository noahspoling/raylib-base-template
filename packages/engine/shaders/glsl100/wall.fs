#version 100
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
// Generated from glsl330 by dev/glsl100.py: edit that file and re-run.

#define MAX_TERRAINS 16
#define STEP_PX 4.0

varying vec3 fragPos;
varying vec2 fragSpan;
varying vec3 fragNormal;
varying vec4 fragColor;

uniform vec4  colDiffuse;
uniform vec4  cliffColor[MAX_TERRAINS];
uniform vec4  swatchColor[MAX_TERRAINS];
uniform float time;
uniform sampler2D cutMap;
uniform vec4  cutInfo;
uniform vec4  seeThrough;
uniform vec2  mapSize;
uniform float tileSize;
uniform float levelHeight;
uniform float waterPass;


float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float flag(float v, float b) {
    return mod(floor(v / b), 2.0);
}

float dither(vec2 frag) {
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

bool cutActive() {
    if (cutInfo.x < 0.5) return false;
    if (cutInfo.x < 1.5) return true;
    if (seeThrough.z <= 0.0 || gl_FragCoord.z >= seeThrough.w) return false;
    float d = distance(gl_FragCoord.xy, seeThrough.xy) / seeThrough.z;
    return d < 1.0 && dither(gl_FragCoord.xy) > smoothstep(0.7, 1.0, d);
}

void main() {
    float slot = floor(fragColor.r * 255.0 + 0.5);
    float style = floor(fragColor.g * 255.0 + 0.5);
    vec4 c = cliffColor[int(slot) - 1];
    vec4 sw = swatchColor[int(slot) - 1];
    vec2 n = fragNormal.xz;
    vec2 key = floor(fragPos.xz - n * 0.5);

    bool sectionFace = flag(style, 8.0) > 0.5;
    bool dim = false;
    if (cutInfo.x > 0.5) {
        vec2 cell = clamp(floor(key / tileSize), vec2(0.0), mapSize - 1.0);
        float cutLv = floor(texture2D(cutMap, (cell + 0.5) / mapSize).r * 255.0 + 0.5);
        bool cutOn = cutLv < 254.5 && cutActive();
        if (sectionFace) {
            if (!cutOn) discard;
            gl_FragColor = vec4(sw.rgb * 0.22 + 0.03, 1.0) * colDiffuse;
            return;
        }
        if (cutOn && fragPos.y > cutLv * levelHeight + 0.01) discard;
        dim = cutInfo.y > 0.5 && cutLv < 254.5 && fragPos.y < (cutLv - 2.0) * levelHeight;
    } else if (sectionFace) {
        discard;
    }
    float kh = hash12(key);
    float s = fragPos.y;
    float top = fragSpan.x, foot = fragSpan.y;
    float depth = top - s;
    float up = s - foot;
    float light = 1.0 + 0.1 * dot(n, vec2(-0.7071, -0.7071));

    vec3 col;
    if (flag(style, 4.0) > 0.5) {
        col = sw.rgb * 0.9;
    } else if (flag(style, 2.0) > 0.5) {
        col = c.rgb * (depth <= 1.5 ? 0.92 : 0.74);
    } else if (flag(style, 1.0) > 0.5) {
        float speed = 34.0 + 18.0 * kh;
        float streak = hash11(floor((depth - time * speed) / 5.0) * 7.13 + kh * 131.0);
        col = sw.rgb * (1.1 + 0.1 * hash11(kh * 91.0));
        if (streak > 0.72) col = mix(col, vec3(1.0), 0.35);
        if (depth < 1.0) col = mix(col, vec3(1.0), 0.3);
        if (up < 3.0) col = mix(col, vec3(1.0), 0.45 + 0.25 * sin(time * 9.0 + kh * 6.2832));
        light = 1.0;
    } else {
        float k;
        if (c.a > 0.5) {
            float r = mod(depth, STEP_PX);
            k = r < 1.0 ? 1.2 : 1.0 - 0.06 * r;
        } else {
            k = 0.7 + 0.3 * up / max(top - foot - 1.0, 1.0);
            float band = floor(s) + floor(hash12(floor(key / 6.0)) * 3.0);
            if (mod(band, 5.0) < 1.0) k *= 0.84;
            if (hash12(key * 1.7 + 3.1) < 0.07 && depth > 1.0) k *= 0.82;
            if (depth < 1.0) k = 1.18;
            else if (depth < 2.0) k *= 1.06;
            if (up < 1.0) k *= 0.85;
        }
        col = c.rgb * k;
    }
    if (dim) col *= 0.43;
    float alpha = waterPass > 0.5 ? (flag(style, 4.0) > 0.5 ? 0.7 : 0.86) : 1.0;
    gl_FragColor = vec4(col * light, alpha) * colDiffuse;
}
