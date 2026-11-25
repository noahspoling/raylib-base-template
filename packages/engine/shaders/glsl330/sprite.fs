#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

out vec4 finalColor;

void main() {
    vec4 c = texture(texture0, fragTexCoord) * fragColor * colDiffuse;
    if (c.a < 0.5) discard;
    finalColor = vec4(c.rgb, 1.0);
}
