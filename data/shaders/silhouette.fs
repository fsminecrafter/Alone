#version 330
// The player seen through whatever hides them: a flat translucent shape,
// drawn only where something is in front (depth test GREATER).
uniform vec4 colDiffuse;
out vec4 finalColor;
void main() { finalColor = colDiffuse; }
