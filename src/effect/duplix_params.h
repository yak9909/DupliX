// DupliX: parameter indices. The index is also the AEGP effect stream index
// (AEGP_GetNewEffectStreamByIndex), so the order must never change once shipped.
// Layout (11/27, after common practice: AE's Repeater puts Copies first, then the per-copy transform; Cinema 4D's
// Random Effector groups Position / Rotation / Scale with a Seed; Trapcode Particular ends with World Transform
// and Rendering groups): Copies, Random Seed, Keep Original in Place, one group per randomized property, World
// Transform, Rendering. Names are English Title Case and unique within the effect (expressions find them by name).
#pragma once

enum DupliXParam {
    RP_INPUT = 0,
    RP_COUNT,
    RP_SEED,
    RP_KEEP_FIRST,
    RP_POS_TOPIC,      // group "Position"
    RP_POS_X,
    RP_POS_Y,
    RP_POS_Z,
    RP_POS_TOPIC_END,
    RP_ROT_TOPIC,      // group "Rotation"
    RP_ROT_X,
    RP_ROT_Y,
    RP_ROT_Z,
    RP_ROT_TOPIC_END,
    RP_SCALE_TOPIC,    // group "Scale"
    RP_SCALE_MIN,
    RP_SCALE_MAX,
    RP_SCALE_TOPIC_END,
    RP_OPACITY_TOPIC,  // group "Opacity"
    RP_OPACITY_MIN,
    RP_OPACITY_MAX,
    RP_OPACITY_TOPIC_END,
    RP_WORLD_TOPIC,    // group "World Transform" (both render modes): copies * G(world) * layer-to-world
    RP_WORLD_POS_X,
    RP_WORLD_POS_Y,
    RP_WORLD_POS_Z,
    RP_WORLD_ROT_X,    // angle
    RP_WORLD_ROT_Y,
    RP_WORLD_ROT_Z,
    RP_WORLD_SCALE_X,  // percent
    RP_WORLD_SCALE_Y,
    RP_WORLD_SCALE_Z,
    RP_WORLD_TOPIC_END,
    RP_RENDER_TOPIC,   // group "Rendering"
    RP_RENDER_MODE,    // popup: 3D Layers (the copies are 3D layers of the comp) / Fast (2D) (spec 12.13: drawn by
                       // the effect on a 2D layer)
    RP_MERGE,          // "Optimize" (3D Layers only): copies merged into groups
                       // (docs/superpowers/specs/2026-11-25-merge-optimization-design.md)
    RP_CANVAS,         // "Canvas" (Fast (2D) only): Comp Size / Fit Copies (the drawing grows to hold every copy)
    RP_RENDER_TOPIC_END,
    RP_NUM_PARAMS
};

// Render Mode's choices (popup values, 1-based)
enum DupliXRenderMode { RM_3D_LAYERS = 1, RM_FAST_2D = 2 };
// Canvas's choices: the drawing's extent in Fast (2D)
enum DupliXCanvas { CV_COMP = 1, CV_FIT = 2 };
inline bool IsFastMode(int popupValue) { return popupValue == RM_FAST_2D; }

// Disk IDs (persisted in projects). Never reuse or renumber. 1 (a status line) and 15 (Fast Mode, a checkbox
// before Render Mode) are retired.
enum DupliXDiskId {
    RD_STATUS_RETIRED = 1,
    RD_COUNT,
    RD_SEED,
    RD_POS_X,
    RD_POS_Y,
    RD_POS_Z,
    RD_ROT_X,
    RD_ROT_Y,
    RD_ROT_Z,
    RD_SCALE_MIN,
    RD_SCALE_MAX,
    RD_OPACITY_MIN,
    RD_OPACITY_MAX,
    RD_KEEP_FIRST,
    RD_FAST_RETIRED,
    RD_WORLD_TOPIC,
    RD_WORLD_POS_X,
    RD_WORLD_POS_Y,
    RD_WORLD_POS_Z,
    RD_WORLD_ROT_X,
    RD_WORLD_ROT_Y,
    RD_WORLD_ROT_Z,
    RD_WORLD_SCALE_X,
    RD_WORLD_SCALE_Y,
    RD_WORLD_SCALE_Z,
    RD_WORLD_TOPIC_END,
    RD_MERGE,
    RD_POS_TOPIC,
    RD_POS_TOPIC_END,
    RD_ROT_TOPIC,
    RD_ROT_TOPIC_END,
    RD_SCALE_TOPIC,
    RD_SCALE_TOPIC_END,
    RD_OPACITY_TOPIC,
    RD_OPACITY_TOPIC_END,
    RD_RENDER_TOPIC,
    RD_RENDER_MODE,
    RD_RENDER_TOPIC_END,
    RD_CANVAS,
};

#define DUPLIX_MATCH_NAME "DupliX"

// major, minor, bug fix (PF_VERSION; the PiPL's AE_Effect_Version must match: 0.1.0 develop 1 = 32769)
constexpr int kDupliXVersion[3] = {0, 1, 0};
