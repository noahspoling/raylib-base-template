#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform vec2 skinTexelSize;
uniform vec4 colDiffuse;

out vec4 finalColor;

void main() {
    vec4 idx = texture(texture0, fragTexCoord);
    if (idx.a < 0.01) discard;

    vec2 skinPx = idx.rg * 255.0;
    vec2 uv = (skinPx + 0.5) * skinTexelSize;

    vec4 skinColor = texture(texture1, uv);
    finalColor = vec4(skinColor.rgb, idx.a) * colDiffuse;
}
