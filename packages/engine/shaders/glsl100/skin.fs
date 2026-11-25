#version 100
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
// Generated from glsl330 by dev/glsl100.py: edit that file and re-run.

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
