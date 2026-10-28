#pragma once
// Mouse input that doesn't drop clicks, and the mouse pointer.
//
// raylib looks at the mouse buttons once per frame, so a click that goes
// down and up again between two frames (easy when the game runs slowly) is
// never seen. Here the window's button events are caught as they happen and
// remembered until the end of the next frame.
//
// The pointer is the system's own (drawn by the OS or browser, so it moves
// smoothly however slow the game is) with the game's arrow and crosshair
// pictures on it.

namespace alone::input {

void init();       // after InitWindow
void shutdown();
void endFrame();   // call right before EndDrawing()

bool pressed(int button);    // went down since the last frame (even if it's already up again)
bool released(int button);   // went up since the last frame

enum class Pointer { Arrow, ArrowWorld, Crosshair, Hidden };
void setPointer(Pointer p);  // cheap to call every frame

} // namespace alone::input
