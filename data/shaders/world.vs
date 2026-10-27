#version 330
// World shader: vertex colours, simple sun + point lights, fog, and a
// see-through "cutaway" around the player for things standing in front of them.
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matModel;

out vec3 fragWorld;
out vec3 fragNormal;
out vec4 fragColor;

void main() {
    vec4 w = matModel * vec4(vertexPosition, 1.0);
    fragWorld = w.xyz;
    fragNormal = normalize(mat3(matModel) * vertexNormal);
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
