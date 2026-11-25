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
uniform vec4 colDiffuse;


void main() {
    vec4 c = texture2D(texture0, fragTexCoord) * fragColor * colDiffuse;
    if (c.a < 0.5) discard;
    gl_FragColor = vec4(c.rgb, 1.0);
}
