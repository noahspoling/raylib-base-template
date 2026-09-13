#version 100
precision mediump float;

// GLSL ES 1.00 variant for Android/web. See glsl330/skin.fs for the
// commented desktop version — logic is identical.

varying vec2 fragTexCoord;
varying vec4 fragColor;

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform vec2 skinTexelSize;
uniform vec4 colDiffuse;

void main() {
    vec4 idx = texture2D(texture0, fragTexCoord);
    if (idx.a < 0.01) discard;

    vec2 skinPx = idx.rg * 255.0;
    vec2 uv = (skinPx + 0.5) * skinTexelSize;

    vec4 skinColor = texture2D(texture1, uv);
    gl_FragColor = vec4(skinColor.rgb, idx.a) * colDiffuse;
}
