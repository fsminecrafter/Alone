#pragma once
#include <math.h>

#define LIGHT_DIR_X   ( 0.4f)
#define LIGHT_DIR_Y   ( 1.0f)
#define LIGHT_DIR_Z   (-0.3f)
#define LIGHT_AMBIENT  0.3f
#define LIGHT_DIFFUSE  0.7f

extern float g_lightX, g_lightY, g_lightZ;

inline void initLight()
{
    float mag = sqrtf(LIGHT_DIR_X*LIGHT_DIR_X +
                      LIGHT_DIR_Y*LIGHT_DIR_Y +
                      LIGHT_DIR_Z*LIGHT_DIR_Z);
    if (mag < 0.0001f) mag = 1.0f;
    g_lightX = LIGHT_DIR_X / mag;
    g_lightY = LIGHT_DIR_Y / mag;
    g_lightZ = LIGHT_DIR_Z / mag;
}

inline float lightScale(float nx, float ny, float nz)
{
    float dot = nx*g_lightX + ny*g_lightY + nz*g_lightZ;
    if (dot < 0.0f) dot = 0.0f;
    float scale = LIGHT_AMBIENT + LIGHT_DIFFUSE * dot;
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
