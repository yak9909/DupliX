// DupliX 2 (prototype): the effect's entry point and its parameters.
//
// The controls follow docs/design/複製プラグイン設計案_v2.md (simple at the top, details in collapsed groups)
// and the common layout of such effects (Cinema 4D's Cloner, Trapcode Form's Base Form, Motion's Replicator):
// docs/design/2026-11-27-v2-effect-controls.md. What is drawn so far: the chosen Sources in a plain grid.
#include "v2/dx2.h"

#include <cstdio>
#include <deque>
#include <string>

namespace dx2 {
namespace {

// Adds the parameters in order; the Disk ID of each is its index (dx2_params.h).
struct Builder {
    PF_InData* in;
    int next = 1;  // 0 is the input layer
    PF_Err err = PF_Err_NONE;
    std::deque<std::string> names;  // popup item strings of the slots (kept until PARAMS_SETUP returns)

    PF_ParamDef Def(const char* name, PF_ParamType type, PF_ParamFlags flags = PF_ParamFlag_NONE)
    {
        PF_ParamDef d;
        AEFX_CLR_STRUCT(d);
        d.param_type = type;
        PF_STRNNCPY(d.PF_DEF_NAME, name, sizeof(d.PF_DEF_NAME));
        d.flags = flags;
        return d;
    }
    void Add(PF_ParamDef& d, int expect)
    {
        if (err) return;
        if (expect != next) {
            Log("ParamsSetup: \"%s\" added at %d, expected %d", d.PF_DEF_NAME, next, expect);
            err = PF_Err_INTERNAL_STRUCT_DAMAGED;
            return;
        }
        d.uu.id = next;
        err = PF_ADD_PARAM(in, -1, &d);
        next++;
    }
    void Topic(int at, const char* name, bool collapsed = true)
    {
        PF_ParamDef d = Def(name, PF_Param_GROUP_START, collapsed ? PF_ParamFlag_START_COLLAPSED : PF_ParamFlag_NONE);
        Add(d, at);
    }
    void End(int at)
    {
        PF_ParamDef d = Def("", PF_Param_GROUP_END);
        Add(d, at);
    }
    void Slider(int at, const char* name, int vmin, int vmax, int smin, int smax, int dflt)
    {
        PF_ParamDef d = Def(name, PF_Param_SLIDER);
        d.u.sd.valid_min = vmin;
        d.u.sd.valid_max = vmax;
        d.u.sd.slider_min = smin;
        d.u.sd.slider_max = smax;
        d.u.sd.value = d.u.sd.dephault = dflt;
        Add(d, at);
    }
    void Float(int at, const char* name, double vmin, double vmax, double smin, double smax, double dflt,
               bool percent = false, PF_Precision prec = PF_Precision_TENTHS)
    {
        PF_ParamDef d = Def(name, PF_Param_FLOAT_SLIDER);
        d.u.fs_d.valid_min = (PF_FpShort)vmin;
        d.u.fs_d.valid_max = (PF_FpShort)vmax;
        d.u.fs_d.slider_min = (PF_FpShort)smin;
        d.u.fs_d.slider_max = (PF_FpShort)smax;
        d.u.fs_d.value = (PF_FpShort)dflt;
        d.u.fs_d.dephault = (PF_FpShort)dflt;
        d.u.fs_d.precision = prec;
        d.u.fs_d.display_flags = percent ? PF_ValueDisplayFlag_PERCENT : PF_ValueDisplayFlag_NONE;
        d.u.fs_d.curve_tolerance = AEFX_DEFAULT_CURVE_TOLERANCE;
        Add(d, at);
    }
    void Percent(int at, const char* name, double vmin, double vmax, double smin, double smax, double dflt)
    {
        Float(at, name, vmin, vmax, smin, smax, dflt, true);
    }
    void Angle(int at, const char* name, double dflt)
    {
        PF_ParamDef d = Def(name, PF_Param_ANGLE);
        d.u.ad.value = d.u.ad.dephault = FLOAT2FIX(dflt);
        Add(d, at);
    }
    // An angle in degrees as a plain slider, for rows that may be hidden: AE left an angle parameter's dial drawn
    // where its hidden row had been, with AEGP's HIDDEN flag and PF_PUI_INVISIBLE alike (user, 11/27). The rows
    // that are never hidden (Global Rotation) keep the real angle control.
    void Degrees(int at, const char* name, double dflt)
    {
        Float(at, name, -360000, 360000, -360, 360, dflt);
    }
    void Popup(int at, const char* name, const char* items, int dflt, bool supervise = false)
    {
        int n = 1;
        for (const char* p = items; *p; p++)
            if (*p == '|') n++;
        PF_ParamDef d = Def(name, PF_Param_POPUP, supervise ? PF_ParamFlag_SUPERVISE : PF_ParamFlag_NONE);
        d.u.pd.num_choices = (A_short)n;
        d.u.pd.value = d.u.pd.dephault = (A_short)dflt;
        d.u.pd.u.namesptr = items;
        Add(d, at);
    }
    void Check(int at, const char* name, bool dflt, bool supervise = false)
    {
        PF_ParamDef d = Def(name, PF_Param_CHECKBOX, supervise ? PF_ParamFlag_SUPERVISE : PF_ParamFlag_NONE);
        d.u.bd.u.nameptr = "";
        d.u.bd.value = d.u.bd.dephault = dflt ? TRUE : FALSE;
        Add(d, at);
    }
    void Layer(int at, const char* name, bool supervise = false)
    {
        PF_ParamDef d = Def(name, PF_Param_LAYER, supervise ? PF_ParamFlag_SUPERVISE : PF_ParamFlag_NONE);
        d.u.ld.dephault = PF_LayerDefault_NONE;
        Add(d, at);
    }
    void Point3(int at, const char* name, double x, double y, double z)
    {
        PF_ParamDef d = Def(name, PF_Param_POINT_3D);
        d.u.point3d_d.x_value = d.u.point3d_d.x_dephault = x;
        d.u.point3d_d.y_value = d.u.point3d_d.y_dephault = y;
        d.u.point3d_d.z_value = d.u.point3d_d.z_dephault = z;
        Add(d, at);
    }
    void Color(int at, const char* name, A_u_char r, A_u_char g, A_u_char b)
    {
        PF_ParamDef d = Def(name, PF_Param_COLOR);
        d.u.cd.value.red = r;
        d.u.cd.value.green = g;
        d.u.cd.value.blue = b;
        d.u.cd.value.alpha = 255;
        d.u.cd.dephault = d.u.cd.value;
        Add(d, at);
    }
    void Path(int at, const char* name)
    {
        PF_ParamDef d = Def(name, PF_Param_PATH);
        d.u.path_d.dephault = 0;  // no mask
        Add(d, at);
    }
    // a parameter with no value whose control the effect draws (custom UI)
    void Custom(int at, const char* name, int height)
    {
        PF_ParamDef d = Def(name, PF_Param_NO_DATA);
        d.ui_flags = PF_PUI_CONTROL;
        d.ui_width = 260;
        d.ui_height = (A_short)height;
        Add(d, at);
    }
};

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data)
{
    Builder b{in_data};

    // ---- the entrance: what to copy how many times in which shape ----
    b.Custom(P_HEADER, "Overview", kHeaderHeight);
    b.Popup(P_SHAPE, "Shape", kShapeItems, SHAPE_GRID, true);
    b.Slider(P_COUNT_A, "Count A", 1, 10000, 1, 50, 5);
    b.Slider(P_COUNT_B, "Count B", 1, 10000, 1, 50, 5);
    b.Slider(P_COUNT_C, "Count C", 1, 10000, 1, 20, 1);
    b.Popup(P_SAMPLING, "Sampling", "Regular|Scatter", 1);
    b.Slider(P_SEED, "Random Seed", 0, 1000000, 0, 1000, 0);
    b.Percent(P_COPY_SCALE, "Copy Scale", 0, 100000, 0, 200, 100);

    // ---- Shape Settings: the chosen Shape's rows only ----
    b.Topic(P_SHAPE_TOPIC, "Shape Settings", false);
    b.Float(P_GRID_SIZE_X, "Grid Size X", 0, 100000, 0, 4000, 800);
    b.Float(P_GRID_SIZE_Y, "Grid Size Y", 0, 100000, 0, 4000, 800);
    b.Float(P_GRID_SIZE_Z, "Grid Size Z", 0, 100000, 0, 4000, 0);
    b.Percent(P_GRID_BRICK, "Brick Offset", -100, 100, -100, 100, 0);
    b.Float(P_POLAR_INNER, "Inner Radius", 0, 100000, 0, 2000, 0);
    b.Float(P_POLAR_OUTER, "Outer Radius", 0, 100000, 0, 2000, 400);
    b.Degrees(P_POLAR_ANGLE, "Angle Range", 360);
    b.Slider(P_POLAR_SIDES, "Sides", 0, 64, 0, 12, 0);
    b.Degrees(P_POLAR_TWIST, "Twist", 0);
    b.Float(P_POLAR_HEIGHT, "Height", -100000, 100000, -2000, 2000, 0);
    b.Float(P_SPHERE_RADIUS, "Sphere Radius", 0, 100000, 0, 2000, 400);
    b.Float(P_SPHERE_SHELL, "Shell Spacing", 0, 100000, 0, 1000, 100);
    b.Degrees(P_SPHERE_LATITUDE, "Latitude Range", 180);
    b.Path(P_PATH_MASK, "Path");
    b.Float(P_PATH_ROW_SPACING, "Row Spacing", 0, 100000, 0, 500, 50);
    b.Check(P_PATH_EVEN, "Even Spacing", true);
    b.End(P_SHAPE_TOPIC_END);

    // ---- Flow: slide along A / B and loop ----
    b.Topic(P_FLOW_TOPIC, "Flow");
    b.Percent(P_FLOW_OFFSET_A, "Offset A", -100000, 100000, -100, 100, 0);
    b.Float(P_FLOW_SPEED_A, "Speed A", -1000, 1000, -2, 2, 0, false, PF_Precision_HUNDREDTHS);
    b.Percent(P_FLOW_VARIANCE_A, "Speed Variance A", 0, 100, 0, 100, 0);
    b.Check(P_FLOW_STEP_A, "Step A", false);
    b.Percent(P_FLOW_OFFSET_B, "Offset B", -100000, 100000, -100, 100, 0);
    b.Float(P_FLOW_SPEED_B, "Speed B", -1000, 1000, -2, 2, 0, false, PF_Precision_HUNDREDTHS);
    b.Percent(P_FLOW_VARIANCE_B, "Speed Variance B", 0, 100, 0, 100, 0);
    b.Check(P_FLOW_STEP_B, "Step B", false);
    b.Popup(P_FLOW_BOUNDARY, "Boundary", "Wrap|Ping Pong|Clamp", 1);
    b.Percent(P_FLOW_EDGE_FADE, "Edge Fade", 0, 50, 0, 50, 0);
    b.End(P_FLOW_TOPIC_END);

    // ---- Morph: towards a second shape, copy by copy ----
    b.Topic(P_MORPH_TOPIC, "Morph");
    b.Percent(P_MORPH_AMOUNT, "Morph Amount", 0, 100, 0, 100, 0);
    b.Popup(P_MORPH_TARGET, "Target Shape", "Grid|Polar|Sphere|Path|Emission", SHAPE_POLAR);
    b.Float(P_MORPH_TARGET_SIZE, "Target Size", 0, 100000, 0, 2000, 400);
    b.End(P_MORPH_TOPIC_END);

    // ---- Emission: birth, life and motion computed from the time alone ----
    b.Topic(P_EMIT_TOPIC, "Emission");
    b.Check(P_EMIT_ON, "Emission", false, true);
    b.Float(P_EMIT_START, "Emit Start", -100000, 100000, 0, 10, 0, false, PF_Precision_HUNDREDTHS);
    b.Float(P_EMIT_DURATION, "Emit Duration", 0, 100000, 0, 30, 10, false, PF_Precision_HUNDREDTHS);
    b.Float(P_EMIT_RATE, "Birth Rate", 0, 100000, 0, 500, 50);
    b.Percent(P_EMIT_JITTER, "Birth Jitter", 0, 100, 0, 100, 0);
    b.Float(P_EMIT_LIFE, "Life", 0, 100000, 0, 10, 3, false, PF_Precision_HUNDREDTHS);
    b.Percent(P_EMIT_LIFE_RANDOM, "Life Random", 0, 100, 0, 100, 0);
    b.Check(P_EMIT_LOOP, "Loop Births", false);
    b.Float(P_EMIT_VELOCITY, "Velocity", -100000, 100000, 0, 1000, 100);
    b.Percent(P_EMIT_VELOCITY_RANDOM, "Velocity Random", 0, 100, 0, 100, 0);
    b.Popup(P_EMIT_DIRECTION, "Direction", "Outward (N)|Along (T)|Up|Down", 1);
    b.Degrees(P_EMIT_SPREAD, "Spread", 0);
    b.Float(P_EMIT_GRAVITY, "Gravity", -100000, 100000, -500, 500, 0);
    b.Float(P_EMIT_WIND_X, "Wind X", -100000, 100000, -500, 500, 0);
    b.Float(P_EMIT_WIND_Y, "Wind Y", -100000, 100000, -500, 500, 0);
    b.Float(P_EMIT_WIND_Z, "Wind Z", -100000, 100000, -500, 500, 0);
    b.Float(P_EMIT_DRAG, "Air Resistance", 0, 100, 0, 10, 0, false, PF_Precision_HUNDREDTHS);
    b.Float(P_EMIT_TURB_AMOUNT, "Turbulence Amount", 0, 100000, 0, 500, 0);
    b.Float(P_EMIT_TURB_SCALE, "Turbulence Scale", 0, 100000, 0, 2000, 300);
    b.Slider(P_EMIT_TURB_COMPLEXITY, "Turbulence Complexity", 1, 8, 1, 8, 3);
    b.Degrees(P_EMIT_TURB_EVOLUTION, "Turbulence Evolution", 0);
    b.Float(P_EMIT_TURB_FLOW, "Turbulence Flow Speed", -1000, 1000, -10, 10, 0, false, PF_Precision_HUNDREDTHS);
    b.Float(P_EMIT_TURB_RAMP, "Turbulence Fade In", 0, 100, 0, 5, 1, false, PF_Precision_HUNDREDTHS);
    b.End(P_EMIT_TOPIC_END);

    // ---- Modifiers: one driver moves several properties at once ----
    b.Topic(P_MOD_TOPIC, "Modifiers", false);
    b.Custom(P_MOD_LIST, "Modifier List", kModListHeight);
    for (int k = 0; k < kModifierSlots; k++) {
        char pre[8], nm[40];
        snprintf(pre, sizeof(pre), "M%d ", k + 1);
        auto name = [&](const char* s) {
            snprintf(nm, sizeof(nm), "%s%s", pre, s);
            return (const char*)nm;
        };
        auto at = [&](int m) { return ModIndex(k, m); };
        snprintf(nm, sizeof(nm), "Modifier %d", k + 1);
        b.Topic(at(M_TOPIC), nm, false);
        b.Popup(at(M_TYPE), name("Type"), kModTypeItems, MT_NONE, true);
        b.Popup(at(M_ALONG), name("Along"), "A|B|C|Index|Distance|X|Y|Z|Age", 1);
        b.Float(at(M_FREQUENCY), name("Frequency"), 0, 1000, 0, 10, 1, false, PF_Precision_HUNDREDTHS);
        b.Degrees(at(M_PHASE), name("Phase"), 0);
        b.Float(at(M_SPEED), name("Speed"), -1000, 1000, -5, 5, 0, false, PF_Precision_HUNDREDTHS);
        b.Float(at(M_NOISE_SCALE), name("Noise Scale"), 0, 100000, 0, 2000, 300);
        b.Degrees(at(M_EVOLUTION), name("Evolution"), 0);
        b.Slider(at(M_SEED), name("Seed"), 0, 1000000, 0, 1000, k + 1);
        b.Percent(at(M_PROGRESS), name("Progress"), 0, 100, 0, 100, 0);
        b.Popup(at(M_ORDER), name("Order"), "Index|A|B|Random|Distance|Age", 1);
        b.Percent(at(M_SOFTNESS), name("Softness"), 0, 100, 0, 100, 50);
        b.Layer(at(M_MAP_LAYER), name("Map Layer"));
        b.Popup(at(M_MAP_SAMPLE), name("Sample At"), "UV|Screen", 1);
        b.Popup(at(M_MAP_CHANNEL), name("Channel"), "Luminance|Red|Green|Blue|Alpha", 1);
        b.Popup(at(M_MASK), name("Mask"), kModMaskItems, MM_ALL, true);
        b.Slider(at(M_INDEX_FROM), name("Index From"), 0, 1000000, 0, 100, 0);
        b.Slider(at(M_INDEX_TO), name("Index To"), 0, 1000000, 0, 100, 10);
        b.Slider(at(M_EVERY_N), name("Every N"), 1, 1000000, 1, 10, 2);
        b.Point3(at(M_FIELD_CENTER), name("Field Center"), 50, 50, 0);
        b.Float(at(M_FIELD_RADIUS), name("Field Radius"), 0, 100000, 0, 2000, 200);
        b.Percent(at(M_FIELD_SOFTNESS), name("Field Softness"), 0, 100, 0, 100, 50);
        b.Percent(at(M_AGE_FROM), name("Age From"), 0, 100, 0, 100, 0);
        b.Percent(at(M_AGE_TO), name("Age To"), 0, 100, 0, 100, 100);
        b.Float(at(M_POS_T), name("Position T"), -100000, 100000, -500, 500, 0);
        b.Float(at(M_POS_N), name("Position N"), -100000, 100000, -500, 500, 0);
        b.Float(at(M_POS_B), name("Position B"), -100000, 100000, -500, 500, 0);
        b.Percent(at(M_SHIFT_A), name("Shift A"), -100000, 100000, -100, 100, 0);
        b.Percent(at(M_SHIFT_B), name("Shift B"), -100000, 100000, -100, 100, 0);
        b.Degrees(at(M_ROTATION), name("Rotation"), 0);
        b.Percent(at(M_SCALE), name("Scale"), -100, 10000, -100, 200, 0);
        b.Percent(at(M_OPACITY), name("Opacity"), -100, 100, -100, 100, 0);
        b.Color(at(M_TINT), name("Tint"), 255, 255, 255);
        b.Percent(at(M_TINT_AMOUNT), name("Tint Amount"), 0, 100, 0, 100, 0);
        b.Float(at(M_TIME_OFFSET), name("Time Offset"), -10000, 10000, -5, 5, 0, false, PF_Precision_HUNDREDTHS);
        b.Slider(at(M_SOURCE_SHIFT), name("Source Shift"), -kSourceSlots, kSourceSlots, -kSourceSlots, kSourceSlots, 0);
        b.Percent(at(M_MORPH), name("Morph"), -100, 100, -100, 100, 0);
        b.Percent(at(M_STRENGTH), name("Strength"), 0, 1000, 0, 200, 100);
        b.End(at(M_TOPIC_END));
    }
    b.End(P_MOD_TOPIC_END);

    // ---- Sources: the layers to copy (none = this layer) ----
    b.Topic(P_SRC_TOPIC, "Sources", false);
    b.Custom(P_SRC_LIST, "Source List", kSrcListHeight);
    for (int k = 0; k < kSourceSlots; k++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "Source %d", k + 1);
        b.Layer(P_SRC_LAYER + k, nm, true);
    }
    b.Popup(P_SRC_ASSIGN, "Assign", kAssignItems, AS_SEQUENTIAL);
    b.Popup(P_SRC_TIME_MODE, "Time Mode", "Normal|Hold|Loop", 1);
    b.Float(P_SRC_TIME_OFFSET, "Source Time Offset", -10000, 10000, -5, 5, 0, false, PF_Precision_HUNDREDTHS);
    b.Slider(P_SRC_QUANTIZE, "Time Quantize", 0, 1000, 0, 24, 0);
    b.End(P_SRC_TOPIC_END);

    // ---- Render ----
    b.Topic(P_RENDER_TOPIC, "Render");
    b.Popup(P_RENDER_BILLBOARD, "Billboard", "Off|Face Camera|Follow Orientation", 1);
    b.Check(P_RENDER_DEPTH_SORT, "Depth Sort", true);
    b.Percent(P_RENDER_DEPTH_FADE, "Depth Fade", 0, 100, 0, 100, 0);
    b.Point3(P_RENDER_POSITION, "Global Position", 50, 50, 0);
    b.Angle(P_RENDER_ROT_X, "Global Rotation X", 0);
    b.Angle(P_RENDER_ROT_Y, "Global Rotation Y", 0);
    b.Angle(P_RENDER_ROT_Z, "Global Rotation Z", 0);
    b.Percent(P_RENDER_SCALE, "Global Scale", 0, 100000, 0, 400, 100);
    b.End(P_RENDER_TOPIC_END);

    if (!b.err && b.next != P_NUM) {
        Log("ParamsSetup: %d parameters, expected %d", b.next, (int)P_NUM);
        b.err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    if (!b.err) {
        PF_CustomUIInfo ci;
        AEFX_CLR_STRUCT(ci);
        ci.events = PF_CustomEFlag_EFFECT;
        b.err = (*in_data->inter.register_ui)(in_data->effect_ref, &ci);
    }
    out_data->num_params = P_NUM;
    return b.err;
}

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data)
{
    out_data->my_version =
        PF_VERSION(kDupliX2Version[0], kDupliX2Version[1], kDupliX2Version[2], PF_Stage_DEVELOP, 1);
    // must match the PiPL (DupliX2PiPL.r)
    out_data->out_flags = PF_OutFlag_CUSTOM_UI | PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG | PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    Suite<AEGP_UtilitySuite6> util(in_data->pica_basicP, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (!util || util->AEGP_RegisterWithAEGP(nullptr, "DupliX2", &g_pluginId))
        Log("GlobalSetup: AEGP_RegisterWithAEGP failed (controls cannot be hidden, Sources not listed)");
    Log("GlobalSetup: %d parameters", (int)P_NUM);
    return PF_Err_NONE;
}

}  // namespace
}  // namespace dx2

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra)
{
    using namespace dx2;
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data);
            break;
        case PF_Cmd_ABOUT:
            PF_SPRINTF(out_data->return_msg, "DupliX 2 %d.%d.%d (prototype)\rCopies of layers in a shape.\rhttps://github.com/yak9909/DupliX",
                       kDupliX2Version[0], kDupliX2Version[1], kDupliX2Version[2]);
            break;
        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data);
            break;
        case PF_Cmd_SEQUENCE_SETUP:
        case PF_Cmd_SEQUENCE_RESETUP:
            err = SequenceSetup(in_data, out_data);
            break;
        case PF_Cmd_SEQUENCE_SETDOWN:
            SequenceSetdown(in_data, out_data);
            break;
        case PF_Cmd_UPDATE_PARAMS_UI:
            if (UpdateVisibility(in_data, params)) out_data->out_flags |= PF_OutFlag_REFRESH_UI;
            break;
        case PF_Cmd_USER_CHANGED_PARAM: {
            const int i = ((PF_UserChangedParamExtra*)extra)->param_index;
            if (i >= P_SRC_LAYER && i < P_SRC_LAYER + kSourceSlots) SetMinSourceRows(in_data, 0);
            UpdateVisibility(in_data, params);
            out_data->out_flags |= PF_OutFlag_REFRESH_UI;  // the custom UI shows the Shape and the Sources
            break;
        }
        case PF_Cmd_EVENT:
            err = HandleEvent(in_data, out_data, params, output, (PF_EventExtra*)extra);
            break;
        case PF_Cmd_SMART_PRE_RENDER:
            err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
            break;
        case PF_Cmd_SMART_RENDER:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra);
            break;
        default:
            break;
        }
    } catch (const std::bad_alloc&) {
        err = PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
