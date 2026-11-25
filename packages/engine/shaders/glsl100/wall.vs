#version 100
// Generated from glsl330 by dev/glsl100.py: edit that file and re-run.

attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec3 vertexNormal;
attribute vec4 vertexColor;

uniform mat4 mvp;

varying vec3 fragPos;
varying vec2 fragSpan;
varying vec3 fragNormal;
varying vec4 fragColor;

void main() {
    fragPos = vertexPosition;
    fragSpan = vertexTexCoord;
    fragNormal = vertexNormal;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
