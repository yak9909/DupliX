// DupliX 2 (prototype): the parameter layout of docs/design/複製プラグイン設計案_v2.md as Effect Controls.
// Only the Sources (several layers) and a plain grid are drawn so far; the rest of the controls are a mock-up
// (docs/design/2026-11-27-v2-effect-controls.md lists what each will do).
//
// The index of a parameter is also its Disk ID (uu.id) and its AEGP effect stream index. Projects save them:
// once this effect ships, never insert, remove or reorder; only append before P_NUM.
#pragma once

namespace dx2 {

constexpr int kSourceSlots = 8;    // Sources: layer parameters (the count cannot change after PARAMS_SETUP)
constexpr int kModifierSlots = 8;  // Modifiers: slots shown one by one with "+ Add Modifier"

// one modifier slot (offsets from the slot's first parameter)
enum ModParam {
    M_TOPIC = 0,   // group "Modifier n"
    M_TYPE,        // None / Wave / Noise / Random / Stagger / Map / Constant (None = the slot is unused, hidden)
    // driver (shown by Type)
    M_ALONG,       // Wave
    M_FREQUENCY,   // Wave
    M_PHASE,       // Wave
    M_SPEED,       // Wave, Noise
    M_NOISE_SCALE, // Noise
    M_EVOLUTION,   // Noise
    M_SEED,        // Random
    M_PROGRESS,    // Stagger
    M_ORDER,       // Stagger
    M_SOFTNESS,    // Stagger
    M_MAP_LAYER,   // Map
    M_MAP_SAMPLE,  // Map
    M_MAP_CHANNEL, // Map
    // mask (shown by Mask)
    M_MASK,        // All / Index Range / Every N / Field / Age Range
    M_INDEX_FROM,
    M_INDEX_TO,
    M_EVERY_N,
    M_FIELD_CENTER,
    M_FIELD_RADIUS,
    M_FIELD_SOFTNESS,
    M_AGE_FROM,
    M_AGE_TO,
    // amounts (0 = not driven)
    M_POS_T,
    M_POS_N,
    M_POS_B,
    M_SHIFT_A,
    M_SHIFT_B,
    M_ROTATION,
    M_SCALE,
    M_OPACITY,
    M_TINT,
    M_TINT_AMOUNT,
    M_TIME_OFFSET,
    M_SOURCE_SHIFT,
    M_MORPH,
    M_STRENGTH,
    M_TOPIC_END,
    M_COUNT
};

enum Param {
    P_INPUT = 0,
    P_HEADER,          // custom UI: name, a picture of the layout, the summary
    P_SHAPE,           // Grid / Polar / Sphere / Path
    P_COUNT_A,
    P_COUNT_B,
    P_COUNT_C,
    P_SAMPLING,        // Regular / Scatter
    P_SEED,
    P_COPY_SCALE,      // the copies' size (% of the Source's own size); the Shape sets only where they go

    P_SHAPE_TOPIC,     // "Shape Settings" (the rows of the chosen Shape only)
    P_GRID_SIZE_X,
    P_GRID_SIZE_Y,
    P_GRID_SIZE_Z,
    P_GRID_BRICK,
    P_POLAR_INNER,
    P_POLAR_OUTER,
    P_POLAR_ANGLE,
    P_POLAR_SIDES,
    P_POLAR_TWIST,
    P_POLAR_HEIGHT,
    P_SPHERE_RADIUS,
    P_SPHERE_SHELL,
    P_SPHERE_LATITUDE,
    P_PATH_MASK,
    P_PATH_ROW_SPACING,
    P_PATH_EVEN,
    P_SHAPE_TOPIC_END,

    P_FLOW_TOPIC,      // "Flow"
    P_FLOW_OFFSET_A,
    P_FLOW_SPEED_A,
    P_FLOW_VARIANCE_A,
    P_FLOW_STEP_A,
    P_FLOW_OFFSET_B,
    P_FLOW_SPEED_B,
    P_FLOW_VARIANCE_B,
    P_FLOW_STEP_B,
    P_FLOW_BOUNDARY,
    P_FLOW_EDGE_FADE,
    P_FLOW_TOPIC_END,

    P_MORPH_TOPIC,     // "Morph"
    P_MORPH_AMOUNT,
    P_MORPH_TARGET,
    P_MORPH_TARGET_SIZE,
    P_MORPH_TOPIC_END,

    P_EMIT_TOPIC,      // "Emission"
    P_EMIT_ON,
    P_EMIT_START,
    P_EMIT_DURATION,
    P_EMIT_RATE,
    P_EMIT_JITTER,
    P_EMIT_LIFE,
    P_EMIT_LIFE_RANDOM,
    P_EMIT_LOOP,
    P_EMIT_VELOCITY,
    P_EMIT_VELOCITY_RANDOM,
    P_EMIT_DIRECTION,
    P_EMIT_SPREAD,
    P_EMIT_GRAVITY,
    P_EMIT_WIND_X,
    P_EMIT_WIND_Y,
    P_EMIT_WIND_Z,
    P_EMIT_DRAG,
    P_EMIT_TURB_AMOUNT,
    P_EMIT_TURB_SCALE,
    P_EMIT_TURB_COMPLEXITY,
    P_EMIT_TURB_EVOLUTION,
    P_EMIT_TURB_FLOW,
    P_EMIT_TURB_RAMP,
    P_EMIT_TOPIC_END,

    P_MOD_TOPIC,       // "Modifiers"
    P_MOD_LIST,        // custom UI: the slots in use, "+ Add Modifier"
    P_MOD_BASE,
    P_MOD_TOPIC_END = P_MOD_BASE + kModifierSlots * M_COUNT,

    P_SRC_TOPIC,       // "Sources"
    P_SRC_LIST,        // custom UI: the layers in use, "+ Add Source"
    P_SRC_LAYER,       // Source 1 .. kSourceSlots
    P_SRC_ASSIGN = P_SRC_LAYER + kSourceSlots,
    P_SRC_TIME_MODE,
    P_SRC_TIME_OFFSET,
    P_SRC_QUANTIZE,
    P_SRC_TOPIC_END,

    P_RENDER_TOPIC,    // "Render"
    P_RENDER_BILLBOARD,
    P_RENDER_DEPTH_SORT,
    P_RENDER_DEPTH_FADE,
    P_RENDER_POSITION,
    P_RENDER_ROT_X,
    P_RENDER_ROT_Y,
    P_RENDER_ROT_Z,
    P_RENDER_SCALE,
    P_RENDER_TOPIC_END,

    P_NUM
};

inline int ModIndex(int slot, int m) { return P_MOD_BASE + slot * M_COUNT + m; }

// popup values (1-based)
enum Shape { SHAPE_GRID = 1, SHAPE_POLAR, SHAPE_SPHERE, SHAPE_PATH };
enum ModType { MT_NONE = 1, MT_WAVE, MT_NOISE, MT_RANDOM, MT_STAGGER, MT_MAP, MT_CONSTANT };
enum ModMask { MM_ALL = 1, MM_INDEX_RANGE, MM_EVERY_N, MM_FIELD, MM_AGE_RANGE };
enum Assign { AS_SEQUENTIAL = 1, AS_RANDOM, AS_BY_A, AS_BY_B };

constexpr const char* kShapeItems = "Grid|Polar|Sphere|Path";
constexpr const char* kModTypeItems = "None|Wave|Noise|Random|Stagger|Map|Constant";
constexpr const char* kModMaskItems = "All|Index Range|Every N|Field|Age Range";
constexpr const char* kAssignItems = "Sequential|Random|By A|By B";

}  // namespace dx2

#define DUPLIX2_MATCH_NAME "DupliX2"
constexpr int kDupliX2Version[3] = {0, 1, 0};  // PF_VERSION(0,1,0,DEVELOP,1) = 32769 (the PiPL's AE_Effect_Version)
