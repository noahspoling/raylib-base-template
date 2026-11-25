#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float uTime;
uniform vec2  uResolution;
uniform vec2  uMouse;
uniform vec2  uTextureSize;

out vec4 finalColor;

void main() {
    finalColor = vec4(fragTexCoord, -1, 1);
}
