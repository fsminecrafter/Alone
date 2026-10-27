#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matModel;

out vec3 fragWorld;
out vec3 fragNormal;
out vec4 fragColor;
out vec2 fragMat;

void main() {
    vec4 w = matModel * vec4(vertexPosition, 1.0);
    fragWorld = w.xyz;
    fragNormal = normalize(mat3(matModel) * vertexNormal);
    fragColor = vertexColor;
    fragMat = vertexTexCoord;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
