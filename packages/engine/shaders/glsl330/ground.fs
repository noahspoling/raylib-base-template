#version 330

#define MAX_TERRAINS 16
#define SADDLE 0.2
#define THIN_PX 2.0
#define EDGE_SLOPE 1.25
#define FLOW_PERIOD 3.0
#define STEP_PX 4.0
#define SHADOW_ALPHA 0.30
#define SHORE_FOAM 1.6

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform sampler2D terrainFills;
uniform vec4  colDiffuse;

uniform vec2  mapSize;
uniform float tileSize;
uniform vec2  fillSize;
uniform int   terrainCount;
uniform float time;
uniform vec4  terrainParams[MAX_TERRAINS];
uniform vec4  terrainFoam[MAX_TERRAINS];
uniform vec4  terrainExtra[MAX_TERRAINS];
uniform vec4  terrainSwatch[MAX_TERRAINS];
uniform float outputMode;
uniform vec2  viewDown;
uniform vec4  raisedInfo;
uniform sampler2D coverMask;
uniform vec4  coverRect;
uniform vec4  terrainSway[MAX_TERRAINS];
uniform float waterPass;
uniform float viewCos;
uniform sampler2D cutMap;
uniform vec4  cutInfo;
uniform vec4  seeThrough;

out vec4 finalColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float edgeNoise(vec2 wp, float seed) {
    vec2 o = vec2(seed * 37.0, seed * 91.0);
    float n = vnoise(wp / (tileSize * 0.75) + o) * 0.65
            + vnoise(wp / (tileSize * 0.25) + o.yx) * 0.35;
    return n * 2.0 - 1.0;
}

vec4 cellData(vec2 cell) {
    ivec2 c = ivec2(clamp(cell, vec2(0.0), mapSize - 1.0));
    return texelFetch(texture0, c, 0);
}

float cellId(vec2 cell) {
    return floor(cellData(cell).r * 255.0 + 0.5);
}

float flag(float v, float b) {
    return mod(floor(v / b), 2.0);
}

vec4 cellRow(vec2 c, float dy) {
    return vec4(cellId(c + vec2(-1.0, dy)), cellId(c + vec2(0.0, dy)),
                cellId(c + vec2(1.0, dy)),  cellId(c + vec2(2.0, dy)));
}

void cells(vec2 wp, out mat4 ids, out vec2 f, out vec2 c) {
    vec2 g = wp / tileSize - 0.5;
    c = floor(g);
    f = g - c;
    ids = mat4(cellRow(c, -1.0), cellRow(c, 0.0), cellRow(c, 1.0), cellRow(c, 2.0));
}

vec4 corners(mat4 ids) {
    return vec4(ids[1].y, ids[1].z, ids[2].y, ids[2].z);
}

vec4 crWeights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5 * vec4(-t + 2.0 * t2 - t3, 2.0 - 5.0 * t2 + 3.0 * t3,
                      t + 4.0 * t2 - 3.0 * t3, t3 - t2);
}

float blend(mat4 ids, float t, vec4 wx, vec4 wy) {
    vec4 tt = vec4(t);
    return dot(vec4(dot(step(tt, ids[0]), wx), dot(step(tt, ids[1]), wx),
                    dot(step(tt, ids[2]), wx), dot(step(tt, ids[3]), wx)), wy);
}

float holds(mat4 ids, vec2 f, float t) {
    float w = blend(ids, t, crWeights(f.x), crWeights(f.y));

    vec4 h = step(vec4(t), corners(ids));
    float d1 = h.x * h.w * (1.0 - h.y) * (1.0 - h.z);
    float d2 = h.y * h.z * (1.0 - h.x) * (1.0 - h.w);
    if (d1 + d2 > 0.5) {
        vec2 other = d1 > 0.5 ? vec2(ids[1].z, ids[2].y) : vec2(ids[1].y, ids[2].z);
        float mid = blend(ids, t, vec4(-0.0625, 0.5625, 0.5625, -0.0625),
                                  vec4(-0.0625, 0.5625, 0.5625, -0.0625));
        float side = (other.x == other.y && mid < 0.49) ? -1.0 : 1.0;
        if ((mid - 0.5) * side < SADDLE) {
            vec2 b = f * (1.0 - f);
            w += (0.5 + side * SADDLE - mid) * 16.0 * b.x * b.y;
        }
    }
    return w;
}

bool present(vec4 ids, float t) {
    return any(equal(ids, vec4(t)));
}

bool nearby(mat4 ids, float t) {
    return present(ids[0], t) || present(ids[1], t) || present(ids[2], t) || present(ids[3], t);
}

vec2 cornerCell(vec2 c, int k) {
    return c + vec2(k == 1 || k == 3 ? 1.0 : 0.0, k >= 2 ? 1.0 : 0.0);
}

vec2 flowAt(vec2 c, vec2 f, float t) {
    vec2 sum = vec2(0.0);
    for (int k = 0; k < 4; k++) {
        vec2 o = cornerCell(c, k) - c;
        vec4 d = cellData(c + o);
        if (floor(d.r * 255.0 + 0.5) != t || flag(floor(d.b * 255.0 + 0.5), 1.0) < 0.5) continue;
        float a = floor(d.g * 255.0 + 0.5) * (6.2831853 / 256.0);
        vec2 w = mix(1.0 - f, f, o);
        sum += w.x * w.y * vec2(sin(a), -cos(a));
    }
    return sum;
}

vec4 fillAt(float slot, vec2 p) {
    vec2 local = mod(floor(p), fillSize) + 0.5;
    return texture(terrainFills, vec2(((slot - 1.0) * fillSize.x + local.x) / (fillSize.x * float(terrainCount)),
                                      local.y / fillSize.y));
}

float slopeDist(vec2 p) {
    vec2 g = p / tileSize - 0.5;
    vec2 c = floor(g);
    vec2 f = g - c;
    float d = mix(mix(cellData(c).a, cellData(c + vec2(1.0, 0.0)).a, f.x),
                  mix(cellData(c + vec2(0.0, 1.0)).a, cellData(c + vec2(1.0, 1.0)).a, f.x), f.y) * 255.0;
    return (d / 32.0 - 0.5) * tileSize;
}

float slopeShade(vec2 p) {
    vec2 gr = vec2(slopeDist(p + vec2(2.0, 0.0)) - slopeDist(p - vec2(2.0, 0.0)),
                   slopeDist(p + vec2(0.0, 2.0)) - slopeDist(p - vec2(0.0, 2.0)));
    return dot(gr, gr) > 1e-4 ? 1.0 + 0.16 * dot(-normalize(gr), vec2(-0.7071, -0.7071)) : 1.0;
}

bool stairsNose(vec2 p, out float nose) {
    nose = 0.0;
    vec2 cell = floor(p / tileSize);
    vec4 d = cellData(cell);
    float drop = floor(floor(d.b * 255.0 + 0.5) / 32.0);
    if (drop < 0.5) return false;
    float g = floor(d.g * 255.0 + 0.5);
    float facing = mod(g, 4.0);
    float index = mod(floor(g / 4.0), 8.0);
    float depth = floor(g / 32.0) + 1.0;
    vec2 f = p / tileSize - cell;
    float u = facing < 0.5 ? f.y : facing < 1.5 ? 1.0 - f.x : facing < 2.5 ? 1.0 - f.y : f.x;
    float t = (index + u) / depth;
    float steps = max(1.0, floor(drop * raisedInfo.y / STEP_PX + 0.5));
    nose = (t * steps - floor(t * steps)) * depth * tileSize / steps;
    return true;
}

vec3 coverAt(vec2 p) {
    if (coverRect.z <= 0.0) return vec3(0.0);
    float v = floor(texture(coverMask, (p - coverRect.xy) / coverRect.zw).r * 255.0 + 0.5);
    return vec3(step(128.0, v), mod(floor(v / 64.0), 2.0), mod(v, 64.0));
}

bool coveredAt(vec2 p) {
    return coverAt(p).x > 0.5;
}

bool wetBankNear(vec2 p, float r) {
    return coverAt(p + vec2(r, 0.0)).z > 31.5 || coverAt(p - vec2(r, 0.0)).z > 31.5 ||
           coverAt(p + vec2(0.0, r)).z > 31.5 || coverAt(p - vec2(0.0, r)).z > 31.5;
}

float dither(vec2 frag) {
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

float cutLevelAt(vec2 p) {
    ivec2 c = ivec2(clamp(floor(p / tileSize), vec2(0.0), mapSize - 1.0));
    return floor(texelFetch(cutMap, c, 0).r * 255.0 + 0.5);
}

bool cutActive() {
    if (cutInfo.x < 0.5) return false;
    if (cutInfo.x < 1.5) return true;
    if (seeThrough.z <= 0.0 || gl_FragCoord.z >= seeThrough.w) return false;
    float d = distance(gl_FragCoord.xy, seeThrough.xy) / seeThrough.z;
    return d < 1.0 && dither(gl_FragCoord.xy) > smoothstep(0.7, 1.0, d);
}

float wind(vec2 p, float speed) {
    return sin(time * speed * 1.7 + dot(p, vec2(0.045, 0.03)) + vnoise(p / 48.0) * 4.0);
}

void main() {
    vec2 wraw = fragTexCoord * mapSize * tileSize;
    vec2 wp = floor(wraw) + 0.5;
    float level = raisedInfo.w > 0.5 ? floor(raisedInfo.x / raisedInfo.y + 0.5) : 0.0;
    float cutLv = 255.0;
    bool cutHere = false;
    if (outputMode < 0.5 && cutInfo.x > 0.5) {
        cutLv = cutLevelAt(wp);
        cutHere = cutLv < 254.5 && cutActive();
        if (cutHere && level > cutLv + 0.5) discard;
    }
    bool shore = false;
    if (outputMode < 0.5 && raisedInfo.w > 0.5) {
        vec3 cv0 = coverAt(wp);
        bool cap0 = cutHere && abs(level - cutLv) < 0.5;
        bool water0 = cv0.z > 0.5 && cv0.z < 31.5;
        shore = waterPass > 0.5 && cv0.z > 31.5;
        if (waterPass < 0.5 && (water0 || (cv0.x > 0.5 && !cap0))) discard;
        if (waterPass > 0.5 && (!(water0 || shore) || ((cv0.x > 0.5 || cv0.y > 0.5) && !cap0))) discard;
    }
    float pxw = max(length(dFdx(wraw)), length(dFdy(wraw)));
    float far = outputMode < 0.5 ? smoothstep(1.25, 2.5, pxw) : 0.0;

    mat4 ids;
    vec2 f, c;
    cells(wp, ids, f, c);
    vec4 cs = corners(ids);
    float lo = min(min(cs.x, cs.y), min(cs.z, cs.w));
    vec2 side = step(vec2(0.5), f);
    float own = mix(mix(cs.x, cs.y, side.x), mix(cs.z, cs.w, side.x), side.y);
    bool crisp = false;
    for (int i = 0; i < MAX_TERRAINS; i++) {
        if (i >= terrainCount) break;
        if (terrainExtra[i].x > 0.5 && present(cs, float(i + 1))) crisp = true;
    }

    float result = lo;
    float win = 0.0;
    bool overhang = false;
    vec4 rp = vec4(0.0);
    vec4 rfoam = vec4(0.0);
    vec4 rx = vec4(0.0);
    vec4 rsw = vec4(0.0);
    vec4 rsway = vec4(0.0);
    float edge = -1.0;
    float edgeT = 0.0;
    vec4 ep = vec4(0.0);
    vec4 efoam = vec4(0.0);
    vec4 ex = vec4(0.0);
    vec4 esw = vec4(0.0);
    vec4 esway = vec4(0.0);
    vec2 right = vec2(viewDown.y, -viewDown.x);
    vec2 colAxis = abs(right.x) >= abs(right.y) ? vec2(sign(right.x), 0.0) : vec2(0.0, sign(right.y));

    for (int i = 1; i <= MAX_TERRAINS; i++) {
        if (i > terrainCount) break;
        float t = float(i);
        vec4 p = terrainParams[i - 1];
        if (t <= lo) {
            if (t == lo) { rp = p; rfoam = terrainFoam[i - 1]; rx = terrainExtra[i - 1]; rsw = terrainSwatch[i - 1]; rsway = terrainSway[i - 1]; }
            continue;
        }
        bool square = crisp || terrainExtra[i - 1].x > 0.5;
        float base = -1.0;
        if (present(cs, t)) base = square ? step(t, own) : holds(ids, f, t) + edgeNoise(wp, t) * p.x;
        float score = base;
        if (p.y > 0.0 && !square && far < 0.5 && nearby(ids, t)) {
            float len = floor(hash12(vec2(floor(dot(wp, colAxis)), t * 13.0)) * (p.y + 1.0));
            if (len > 0.0) {
                vec2 up = wp - viewDown * len;
                vec4 sw = terrainSway[i - 1];
                if (sw.x > 0.0) up += right * floor(sw.x * len / (p.y + 1.0) * wind(wp, sw.y) + 0.5);
                mat4 ids2 = ids;
                vec2 g2 = up / tileSize - 0.5;
                vec2 c2 = floor(g2);
                vec2 f2 = g2 - c2;
                if (c2 != c) cells(up, ids2, f2, c2);
                if (present(corners(ids2), t) && !coveredAt(up)) score = max(score, holds(ids2, f2, t) + edgeNoise(up, t) * p.x);
            }
        }
        if (score > 0.5) {
            overhang = base <= 0.5 && result < 0.5;
            result = t;
            win = score;
            rp = p;
            rfoam = terrainFoam[i - 1];
            rx = terrainExtra[i - 1];
            rsw = terrainSwatch[i - 1];
            rsway = terrainSway[i - 1];
            edge = -1.0;
        } else if (base > edge) {
            edge = base;
            edgeT = t;
            ep = p;
            efoam = terrainFoam[i - 1];
            ex = terrainExtra[i - 1];
            esw = terrainSwatch[i - 1];
            esway = terrainSway[i - 1];
        }
    }
    if (result < 0.5) discard;

    if (shore) {
        float prio = 0.0;
        for (int i = 1; i <= MAX_TERRAINS; i++) {
            if (i > terrainCount) break;
            if (terrainExtra[i - 1].w < 0.5) continue;
            float t = float(i);
            float pr = present(cs, t) ? 3.0 : nearby(ids, t) ? 2.0 : 1.0;
            if (pr > prio) {
                prio = pr;
                result = t;
                rp = terrainParams[i - 1];
                rfoam = terrainFoam[i - 1];
                rx = terrainExtra[i - 1];
                rsw = terrainSwatch[i - 1];
                rsway = terrainSway[i - 1];
            }
            if (prio > 2.5) break;
        }
        if (prio < 0.5) discard;
        edge = -1.0;
        overhang = false;
    }

    if (!shore && result > lo && edge > -0.5 && (win - edge) * tileSize / EDGE_SLOPE < THIN_PX) {
        result = edgeT;
        rp = ep;
        rfoam = efoam;
        rx = ex;
        rsw = esw;
        rsway = esway;
        edge = -1.0;
        overhang = false;
    }

    vec2 fp = wp;
    float gust = 0.0;
    if (rsway.x > 0.0 && far < 0.5) {
        fp.x += floor(rsway.x * 0.5 * sin(time * rsway.y * 1.3 + wp.y * 0.18 + vnoise(wp / 24.0) * 5.0) + 0.5);
        gust = smoothstep(0.55, 1.0, wind(wp * 0.6, rsway.y * 0.7));
    }
    vec4 color = fillAt(result, fp);
    color.rgb *= 1.0 + 0.07 * gust;
    if (rx.z > 0.0) {
        vec2 fl = flowAt(c, f, result);
        if (dot(fl, fl) > 1e-4) {
            vec2 dir = normalize(fl);
            vec2 shift = dir * rx.z * FLOW_PERIOD;
            float ph = fract(time / FLOW_PERIOD);
            color = mix(fillAt(result, wp - shift * ph), fillAt(result, wp - shift * fract(ph + 0.5)),
                        abs(1.0 - 2.0 * ph));
            vec2 q = vec2(dot(wp, dir) - time * rx.z, dot(wp, vec2(-dir.y, dir.x)));
            if (far < 0.5 && hash12(floor(q / vec2(9.0, 3.0)) + result * 17.0) > 0.86 &&
                mod(q.x, 9.0) < 4.0 && mod(q.y, 3.0) < 1.2) {
                color.rgb = mix(color.rgb, vec3(1.0), 0.2);
            }
        }
    }

    color.rgb = mix(color.rgb, rsw.rgb, far);

    if (rp.z > 0.0 && edge > -0.5) {
        float dist = (0.5 - edge) * tileSize / EDGE_SLOPE;
        float wash = 1.0 - rx.y * 0.45 * (1.0 - sin(time * rp.w + vnoise(wp / 24.0) * 6.2832));
        float width = rp.z * wash;
        if (dist < width && !(waterPass > 0.5 && raisedInfo.w > 0.5 && wetBankNear(wp, rp.z + 1.0))) {
            float a = rfoam.a * (dist < width * 0.6 ? 1.0 : 0.5) * (1.0 - far);
            color.rgb = mix(color.rgb, rfoam.rgb, a);
        }
    }
    bool falls = false;
    if (outputMode > 0.5) {
        for (int k = 0; k < 4; k++) {
            vec2 cc = cornerCell(c, k);
            vec4 d = cellData(cc);
            if (floor(d.r * 255.0 + 0.5) != result) continue;
            float fb = floor(d.b * 255.0 + 0.5);
            vec2 o = wp / tileSize - (cc + 0.5);
            if ((flag(fb, 2.0) > 0.5 && o.y < 0.0) || (flag(fb, 4.0) > 0.5 && o.x > 0.0) ||
                (flag(fb, 8.0) > 0.5 && o.y > 0.0) || (flag(fb, 16.0) > 0.5 && o.x < 0.0)) falls = true;
        }
    }
    if (outputMode > 0.5 && outputMode < 1.5) {
        finalColor = vec4((result + (falls ? 64.0 : 0.0) + (overhang ? 128.0 : 0.0)) / 255.0, 0.0, 0.0, 1.0);
        return;
    }
    bool water = rx.w > 0.5;
    float waterDepth = 0.0;
    if (outputMode < 0.5 && raisedInfo.w > 0.5) {
        vec3 cv = coverAt(wp);
        bool atCut = cutHere && abs(level - cutLv) < 0.5;
        if (waterPass < 0.5) {
            if (water) discard;
            if (cv.x > 0.5) {
                if (!atCut) discard;
                finalColor = vec4(rsw.rgb * 0.22 + 0.03, 1.0) * fragColor * colDiffuse;
                return;
            }
        } else {
            if (!water) discard;
            if ((cv.x > 0.5 || cv.y > 0.5) && !atCut) discard;
            waterDepth = shore ? (cv.z - 32.0) / 31.0 : cv.z;
        }
    }

    float nose = 0.0;
    bool raised = raisedInfo.w > 0.5;
    bool stairs = raised && stairsNose(wp, nose);
    bool sloped = raised && !stairs && raisedInfo.z > 0.0 && slopeDist(wp) < raisedInfo.z;
    if (outputMode > 1.5) {
        if (overhang || stairs || (raisedInfo.z > 0.0 && slopeDist(wp) < raisedInfo.z + 2.0)) discard;
        if (water && !falls) discard;
        if (falls) {
            float churn = 0.55 + 0.2 * sin(time * 7.0 + hash12(floor(wp / 2.0)) * 6.2832);
            finalColor = vec4(mix(rsw.rgb, vec3(1.0), churn), 0.9 * fragColor.a * colDiffuse.a);
        } else {
            finalColor = vec4(0.0, 0.0, 0.0, SHADOW_ALPHA * fragColor.a * colDiffuse.a);
        }
        return;
    }
    if (raised) {
        if (stairs) {
            color.rgb *= nose < 1.0 ? 1.2 : 1.0;
        } else if (rx.x > 0.5) {
            float r = mod(floor(wp.y), STEP_PX);
            color.rgb *= r < 1.0 ? 1.15 : 1.0 - 0.05 * r;
        }
        if (sloped) {
            if (!water) color.rgb *= slopeShade(wp);
        } else if (lo < 0.5 && !overhang && (win - 0.5) * tileSize / EDGE_SLOPE < 1.0) {
            color.rgb = mix(color.rgb, vec3(1.0), 0.2);
        }
    }
    if (cutInfo.y > 0.5 && cutLv < 254.5 && level < cutLv - 1.5) color.rgb *= 0.43;
    finalColor = color * fragColor * colDiffuse;
    if (waterPass > 0.5) {
        float a = waterDepth > 30.5 ? 0.97 : clamp(1.0 - exp(-waterDepth * 0.45), shore ? 0.0 : 0.38, 0.95);
        a += (1.0 - a) * (1.0 - viewCos) * 0.55 * (shore ? smoothstep(0.0, 0.3, waterDepth) : 1.0);
        if (shore && far < 0.5 && waterDepth * raisedInfo.y < SHORE_FOAM) {
            float wash = 1.0 - rx.y * 0.45 * (1.0 - sin(time * rp.w + vnoise(wp / 24.0) * 6.2832));
            float fa = rfoam.a * (0.55 + 0.45 * wash);
            finalColor.rgb = mix(finalColor.rgb, rfoam.rgb, fa);
            a = max(a, 0.6 * fa);
        }
        finalColor.a *= a;
    }
}
