#version 330

// UV-remap skinning fragment shader. texture0 (the map-animation frame, bound
// automatically by DrawTexturePro) holds baked coordinates instead of color:
// RG = skin-space pixel position / 255, A = silhouette mask. See
// tools/skin_baker for how frames get baked and
// include/components/skinned_sprite.h for the component this drives.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;      // baked UV-animation frame
uniform sampler2D texture1;      // skin texture (full detail art)
uniform vec2 skinTexelSize;      // 1.0 / skin texture dimensions, in pixels
uniform vec4 colDiffuse;         // sprite tint, from DrawTexturePro

out vec4 finalColor;

void main() {
    vec4 idx = texture(texture0, fragTexCoord);
    if (idx.a < 0.01) discard;

    // idx.rg were baked as exact skin-space pixel coords, stored /255.
    vec2 skinPx = idx.rg * 255.0;
    vec2 uv = (skinPx + 0.5) * skinTexelSize;

    vec4 skinColor = texture(texture1, uv);
    finalColor = vec4(skinColor.rgb, idx.a) * colDiffuse;
}
