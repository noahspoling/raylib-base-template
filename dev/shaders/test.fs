#version 330

in vec2 fragTexCoord;        // 0..1 across the canvas, origin top-left
in vec4 fragColor;

uniform sampler2D texture0;  // starter.png if it exists next to this file, else 1x1 white
uniform float uTime;         // seconds
uniform vec2  uResolution;   // canvas size in pixels
uniform vec2  uMouse;        // 0..1 over the canvas
uniform vec2  uTextureSize;  // texture0 size in pixels

out vec4 finalColor;

void main() {
    finalColor = vec4(fragTexCoord, 1, 1);
}
