#pragma once
#include <math.h>

// Light direction (normalised, toward the light source).
// Set by applyTimeOfDay() in main.cpp each time the hour changes.
extern float g_lightX, g_lightY, g_lightZ;

// Ambient and diffuse strengths [0..1].
// Also set by applyTimeOfDay() — do NOT use hardcoded defines here.
extern float g_ambient, g_diffuse;

inline void initLight()
{
    // Default to morning sun until applyTimeOfDay() runs.
    g_lightX =  0.33f;
    g_lightY =  0.88f;
    g_lightZ = -0.18f;
    g_ambient = 0.30f;
    g_diffuse = 0.70f;
}

inline float lightScale(float nx, float ny, float nz)
{
    float dot = nx*g_lightX + ny*g_lightY + nz*g_lightZ;
    if (dot < 0.0f) dot = 0.0f;
    float scale = g_ambient + g_diffuse * dot;
    if (scale > 1.0f) scale = 1.0f;
    return scale;
}

// Apply lighting scale to a u8 RGB and submit via glColor3b.
// glColor3b in libnds takes uint8 (0-255) and shifts >>3 internally.
inline void glColorLit(u8 r, u8 g, u8 b, float scale)
{
    glColor3b(
        (u8)(r * scale),
        (u8)(g * scale),
        (u8)(b * scale)
    );
}
