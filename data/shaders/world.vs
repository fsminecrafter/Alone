#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
// Wall meshes only: the wall piece this vertex belongs to
// (centre x, centre z, lowest point, kind: 0 wall, 1 fitting, 2 never cut)
in vec4 vertexTangent;

uniform mat4 mvp;
uniform mat4 matModel;
// Cutting walls down, like Project Zomboid: walls standing between you and the
// camera drop to a low stub so you can see into the rooms.
//   0 off, 1 every wall in front of you (the building you're in),
//   2 only walls near your line of sight (a building that hides you)
uniform float wallCut;
uniform float wallFloor;   // height of the floor of the building being drawn
uniform vec3 playerPos;
uniform vec3 viewDir;

out vec3 fragWorld;
out vec3 fragNormal;
out vec4 fragColor;
out vec2 fragMat;

const float kCutHeight = 0.55;

void main() {
    vec3 p = vertexPosition;
    if (wallCut > 0.5 && vertexTangent.w < 1.5) {
        vec2 toCam = -normalize(viewDir.xz);
        // (the piece's centre goes through the model matrix too: an upper
        // floor is kept off the map and drawn moved into place)
        vec2 rel = (matModel * vec4(vertexTangent.x, 0.0, vertexTangent.y, 1.0)).xz - playerPos.xz;
        float ahead = dot(rel, toCam);
        float aside = abs(rel.x * toCam.y - rel.y * toCam.x);
        if (ahead > 0.05 && (wallCut < 1.5 || (aside < 4.5 && ahead < 30.0))) {
            float top = kCutHeight - vertexTangent.w * 0.02;
            // a piece that starts above the cut (glass, lintels) disappears;
            // the rest is cut down to the stub's height
            if (vertexTangent.z >= top) p = vec3(vertexTangent.x, wallFloor - 3.0, vertexTangent.y);
            else p.y = min(p.y, wallFloor + top);
        }
    }
    vec4 w = matModel * vec4(p, 1.0);
    fragWorld = w.xyz;
    fragNormal = normalize(mat3(matModel) * vertexNormal);
    fragColor = vertexColor;
    fragMat = vertexTexCoord;
    gl_Position = mvp * vec4(p, 1.0);
}
