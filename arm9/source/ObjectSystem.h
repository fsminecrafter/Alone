#pragma once

// ---------------------------------------------------------------------------
// ObjectSystem.h  —  Tags, named objects, scripting, and audio emitters
//
// Hierarchy
// ---------
//   WorldObject         – any named, tagged thing in the world
//   ScriptComponent     – base class for C++ scripts attached to objects
//   AudioEmitter        – 3-D positional audio source attached to an object
//
// .world v2 binary additions (appended after ChunkEntry block)
// -----------------------------------------------------------
//   ObjectTableHeader
//   ObjectEntry[objectCount]
//   TagTableHeader
//   TagEntry[tagCount]          each followed by u16 indices[]
//   AudioTableHeader
//   AudioTrack[trackCount]      playlist (music)
//   AudioEmitterEntry[emitterCount]
// ---------------------------------------------------------------------------

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <functional>

// ---- forward declarations ----
class ScriptComponent;
class AudioSystem;
struct WorldObject;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define OBJ_MAX_OBJECTS     256
#define OBJ_MAX_TAGS        64
#define OBJ_MAX_NAME        32
#define OBJ_MAX_TAG_NAME    24
#define OBJ_MAX_SCRIPTS_PER_OBJ  4
#define OBJ_MAX_EMITTERS    32
#define OBJ_MAX_TRACKS      16
#define OBJ_CHUNK_TARGET    0xFFFF  // objectId sentinel: attached to chunk not individual obj

// ---------------------------------------------------------------------------
// .world v2 binary structs  (packed, little-endian)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

struct ObjectTableHeader {
    char  magic[4];    // "OBJS"
    u16   objectCount;
};

struct ObjectEntry {
    u16   id;
    char  name[OBJ_MAX_NAME];
    // World-space position baked at export (f32 fixed-point: value * 4096)
    s32   worldX, worldY, worldZ;
    u16   tagMask;     // bitfield — bit i set means tag[i] applies
    u8    flags;       // OBJ_FLAG_*
    u8    reserved;
};
#define OBJ_FLAG_ACTIVE      (1<<0)
#define OBJ_FLAG_FLOOR_CHUNK (1<<1)   // floor or chunk — scripts cannot attach
#define OBJ_FLAG_BILLBOARD   (1<<2)
#define OBJ_FLAG_MODEL       (1<<3)

struct TagTableHeader {
    char  magic[4];   // "TAGS"
    u16   tagCount;
};

struct TagEntry {
    u8    tagIndex;
    char  name[OBJ_MAX_TAG_NAME];
    u16   memberCount;
    // followed by memberCount * u16 (object IDs)
};

struct AudioTableHeader {
    char  magic[4];   // "AUDI"
    u8    trackCount;
    u8    emitterCount;
};

struct AudioSettingsHeader {
    char  magic[4];   // "AUSF"
    u8    fadeFrames;
    u8    delayFrames;
};

#define AUDIO_TRACK_NAME_LEN  48
struct AudioTrackEntry {
    char  filename[AUDIO_TRACK_NAME_LEN];  // e.g. "fat:/Alone/music/theme.dsnd"
    u8    volume;         // 0-127 (ARM7 mixer scale)
    u8    flags;          // TRACK_FLAG_LOOP
};
#define TRACK_FLAG_LOOP  (1<<0)

struct AudioEmitterEntry {
    u16   objectId;       // which WorldObject this is attached to (OBJ_CHUNK_TARGET = chunk)
    s16   chunkGridX, chunkGridZ;   // used when objectId == OBJ_CHUNK_TARGET
    s32   worldX, worldY, worldZ;   // absolute world-space position
    char  filename[AUDIO_TRACK_NAME_LEN];
    u8    volume;
    u8    flags;          // EMITTER_FLAG_LOOP
    u16   radiusInner;    // world units * 16 — full volume inside this
    u16   radiusOuter;    // world units * 16 — silent beyond this
};
#define EMITTER_FLAG_LOOP  (1<<0)

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Runtime WorldObject
// ---------------------------------------------------------------------------
struct WorldObject {
    u16            id;
    char           name[OBJ_MAX_NAME];
    float          x, y, z;     // world-space position (modifiable at runtime)
    float          rotY;         // Y rotation in degrees
    u32            tagMask;
    u8             flags;
    bool           active;

    ScriptComponent* scripts[OBJ_MAX_SCRIPTS_PER_OBJ];
    u8               scriptCount;
};

// ---------------------------------------------------------------------------
// ScriptComponent — base class
// User defines subclasses in C++, registers via REGISTER_SCRIPT macro.
// ---------------------------------------------------------------------------
class ScriptComponent {
public:
    WorldObject* object = nullptr;   // set by ObjectSystem on attach

    virtual ~ScriptComponent() {}

    // Lifecycle — all optional
    virtual void start()       {}   // called once after world load
    virtual void update()      {}   // called 60 fps
    virtual void lowerUpdate() {}   // called 30 fps (every other frame)

    // ---- Move helpers ----
    void moveTo(float tx, float ty, float tz);
    void moveBy(float dx, float dy, float dz);
    void moveForward(float dist);
    void moveBackward(float dist) { moveForward(-dist); }
    void moveLeft(float dist)     { moveForward(dist); rotateY(-90); moveForward(0); rotateY(90); }

    // ---- Rotate helpers ----
    void rotateY(float deg);
    void rotateTo(float deg);

    // ---- Texture helpers  (editor assigns tex IDs; use runtime IDs) ----
    void setObjectColor(u8 r, u8 g, u8 b);

    // ---- Time helpers ----
    bool  isDay()   const;
    bool  isNight() const { return !isDay(); }
    int   getHour() const;
    int   getSeason() const;

    // ---- Audio helpers (calls into AudioSystem) ----
    void  playSound(const char* filename, u8 volume = 100);
    void  stopSound();
    float distToCamera() const;

protected:
    // Internal: resolved by ObjectSystem
    float _soundTimer = 0.0f;
    int   _channelId  = -1;
};

// ---------------------------------------------------------------------------
// Script factory  — maps string names → factory functions
// ---------------------------------------------------------------------------
typedef ScriptComponent* (*ScriptFactory)();

struct ScriptRegistration {
    const char*   name;
    ScriptFactory factory;
    ScriptRegistration* next;
};

// Declare + auto-register a script class in a .cpp file:
//   REGISTER_SCRIPT(MyScript)
#define REGISTER_SCRIPT(ClassName) \
    static ScriptComponent* _factory_##ClassName() { return new ClassName(); } \
    static ScriptRegistration _reg_##ClassName = { \
        #ClassName, _factory_##ClassName, nullptr \
    }; \
    __attribute__((constructor)) \
    static void _init_##ClassName() { \
        ObjectSystem::registerScript(&_reg_##ClassName); \
    }

// ---------------------------------------------------------------------------
// Runtime tag handle
// ---------------------------------------------------------------------------
struct RuntimeTag {
    char  name[OBJ_MAX_TAG_NAME];
    u8    index;
    u16   members[OBJ_MAX_OBJECTS];
    u16   memberCount;
};

// ---------------------------------------------------------------------------
// ObjectSystem
// ---------------------------------------------------------------------------
class ObjectSystem {
public:
    ObjectSystem();
    ~ObjectSystem();

    // Load objects/tags from an open .world file (seek position: right after chunk data)
    bool loadFromWorld(FILE* fd);
    void unload();

    // Per-frame updates (call from main loop)
    void update(int frame, float camX, float camY, float camZ);

    // Script registration (called via REGISTER_SCRIPT constructor)
    static void registerScript(ScriptRegistration* reg);

    // Lookup
    WorldObject* findByName(const char* name);
    WorldObject* findById(u16 id);

    // Tag queries — fills outIds[], returns count found (≤ maxOut)
    int  findByTag(const char* tagName, u16* outIds, int maxOut) const;
    bool objectHasTag(u16 objectId, const char* tagName) const;

    // Script attach (can be done at runtime, or auto-wired from .world v2)
    bool attachScript(u16 objectId, const char* scriptName);

    // Called from main to let scripts run start() after world finishes loading
    void fireStart();

    u16  objectCount()  const { return objCount; }
    WorldObject* getObject(u16 i) { return (i < objCount) ? &objects[i] : nullptr; }

    // --- internal friend access ---
    float getCamX() const { return _camX; }
    float getCamY() const { return _camY; }
    float getCamZ() const { return _camZ; }

private:
    WorldObject objects[OBJ_MAX_OBJECTS];
    u16         objCount;
    RuntimeTag  tags[OBJ_MAX_TAGS];
    u8          tagCount;
    int         _frame;
    float       _camX, _camY, _camZ;
    bool        _startFired;

    static ScriptRegistration* s_scriptRegistry;

    ScriptComponent* createScript(const char* name);
    int  tagIndexByName(const char* name) const;
};

// Global singleton
extern ObjectSystem g_objects;
