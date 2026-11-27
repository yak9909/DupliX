// DupliX 2 (prototype): the custom UI in the Effect Controls.
//   Overview      a picture of the layout (dots coloured by the Source each copy shows) and a summary
//   Modifier List the slots in use as chips (x removes one), "+ Add Modifier" shows the next free slot
//   Source List   the chosen layers (x removes one, the rest move up), "+ Add Source" shows the next row
// Every value stays an ordinary AE parameter (keyframes, expressions); the lists only show / hide rows.
#include "v2/dx2.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "AEFX_SuiteHelper.h"

namespace dx2 {
namespace {

struct RGBA {
    float r, g, b, a;
};

// one colour per Source slot (the same in the Overview's dots and the Source List)
const RGBA kSourceColors[kSourceSlots] = {
    {0.93f, 0.42f, 0.40f, 1}, {0.96f, 0.69f, 0.30f, 1}, {0.93f, 0.86f, 0.38f, 1}, {0.48f, 0.80f, 0.42f, 1},
    {0.36f, 0.78f, 0.86f, 1}, {0.42f, 0.58f, 0.96f, 1}, {0.66f, 0.48f, 0.92f, 1}, {0.92f, 0.52f, 0.78f, 1},
};
const RGBA kTypeColors[] = {
    {0.5f, 0.5f, 0.5f, 1},    // None
    {0.36f, 0.78f, 0.86f, 1}, // Wave
    {0.66f, 0.48f, 0.92f, 1}, // Noise
    {0.96f, 0.69f, 0.30f, 1}, // Random
    {0.48f, 0.80f, 0.42f, 1}, // Stagger
    {0.92f, 0.52f, 0.78f, 1}, // Map
    {0.80f, 0.80f, 0.80f, 1}, // Constant
};
const wchar_t* const kTypeNames[] = {L"None", L"Wave", L"Noise", L"Random", L"Stagger", L"Map", L"Constant"};
const wchar_t* const kShapeNames[] = {L"Grid", L"Polar", L"Sphere", L"Path"};
// what Count A / B / C mean in each Shape
const wchar_t* const kAxisNames[4][3] = {
    {L"Columns", L"Rows", L"Layers"},
    {L"Around", L"Rings", L"Levels"},
    {L"Longitude", L"Latitude", L"Shells"},
    {L"Along", L"Rows", L"Depth"},
};
const wchar_t* const kAssignNames[] = {L"Sequential", L"Random", L"By A", L"By B"};

struct Rect {
    float x, y, w, h;
    bool Has(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// ---- drawing (Drawbot) ----
class Canvas {
public:
    Canvas(PF_InData* in_data, PF_OutData* out_data, PF_EventExtra* ev) : in_(in_data), out_(out_data)
    {
        if (AEFX_AcquireDrawbotSuites(in_data, out_data, &s_)) return;
        acquired_ = true;
        PF_EffectCustomUISuite1* cui = nullptr;
        if (AEFX_AcquireSuite(in_data, out_data, kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion1, nullptr,
                              (void**)&cui) == PF_Err_NONE &&
            cui) {
            cui->PF_GetDrawingReference(ev->contextH, &draw_);
            AEFX_ReleaseSuite(in_data, out_data, kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion1, nullptr);
        }
        if (!draw_) return;
        s_.drawbot_suiteP->GetSupplier(draw_, &sup_);
        s_.drawbot_suiteP->GetSurface(draw_, &surf_);
        s_.supplier_suiteP->GetDefaultFontSize(sup_, &fontSize_);
        s_.surface_suiteP->SetAntiAliasPolicy(surf_, kDRAWBOT_AntiAliasPolicy_High);
        PFAppSuite6* app = nullptr;
        if (in_data->pica_basicP->AcquireSuite(kPFAppSuite, kPFAppSuiteVersion6, (const void**)&app) == kSPNoError && app) {
            PF_App_Color c;
            if (!app->PF_AppGetColor(PF_App_Color_TEXT, &c)) text_ = {c.red / 65535.f, c.green / 65535.f, c.blue / 65535.f, 1};
            if (!app->PF_AppGetColor(PF_App_Color_PANEL_BACKGROUND, &c))
                panel_ = {c.red / 65535.f, c.green / 65535.f, c.blue / 65535.f, 1};
            in_data->pica_basicP->ReleaseSuite(kPFAppSuite, kPFAppSuiteVersion6);
        }
    }
    ~Canvas()
    {
        if (acquired_) AEFX_ReleaseDrawbotSuites(in_, out_);
    }
    bool Ok() const { return surf_ != nullptr; }
    float FontSize() const { return fontSize_; }
    RGBA Text() const { return text_; }
    RGBA Dim() const { return Mix(text_, panel_, 0.45f); }
    // a shade between the panel and the text colour (t = 0 panel .. 1 text)
    RGBA Shade(float t) const { return Mix(panel_, text_, t); }
    static RGBA Mix(RGBA a, RGBA b, float t)
    {
        return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
    }

    void FillRound(const Rect& r, float radius, RGBA c)
    {
        Path([&](DRAWBOT_PathRef p) { RoundPath(p, r, radius); }, c, true);
    }
    void StrokeRound(const Rect& r, float radius, RGBA c, float width = 1)
    {
        Path([&](DRAWBOT_PathRef p) { RoundPath(p, {r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}, radius); }, c, false, width);
    }
    void Circle(float cx, float cy, float radius, RGBA c, bool fill = true, float width = 1)
    {
        Path(
            [&](DRAWBOT_PathRef p) {
                DRAWBOT_PointF32 ctr = {cx, cy};
                s_.path_suiteP->AddArc(p, &ctr, radius, 0, 360);
                s_.path_suiteP->Close(p);
            },
            c, fill, width);
    }
    void Line(float x0, float y0, float x1, float y1, RGBA c, float width = 1)
    {
        Path(
            [&](DRAWBOT_PathRef p) {
                s_.path_suiteP->MoveTo(p, x0, y0);
                s_.path_suiteP->LineTo(p, x1, y1);
            },
            c, false, width);
    }
    void Polyline(const std::vector<DRAWBOT_PointF32>& pts, RGBA c, float width = 1)
    {
        if (pts.size() < 2) return;
        Path(
            [&](DRAWBOT_PathRef p) {
                s_.path_suiteP->MoveTo(p, pts[0].x, pts[0].y);
                for (size_t i = 1; i < pts.size(); i++) s_.path_suiteP->LineTo(p, pts[i].x, pts[i].y);
            },
            c, false, width);
    }
    // text with its baseline at y; cut with "..." past maxWidth (0 = no limit)
    void Text(float x, float y, const std::wstring& s, RGBA c, float size = 0, float maxWidth = 0,
              DRAWBOT_TextAlignment align = kDRAWBOT_TextAlignment_Left)
    {
        if (!Ok() || s.empty()) return;
        DRAWBOT_FontRef font = nullptr;
        DRAWBOT_BrushRef brush = nullptr;
        DRAWBOT_ColorRGBA col = {c.r, c.g, c.b, c.a};
        if (!s_.supplier_suiteP->NewDefaultFont(sup_, size > 0 ? size : fontSize_, &font) &&
            !s_.supplier_suiteP->NewBrush(sup_, &col, &brush)) {
            DRAWBOT_PointF32 o = {x, y};
            s_.surface_suiteP->DrawString(surf_, brush, font, (const DRAWBOT_UTF16Char*)s.c_str(), &o, align,
                                          maxWidth > 0 ? kDRAWBOT_TextTruncation_EndEllipsis : kDRAWBOT_TextTruncation_None,
                                          maxWidth);
        }
        if (brush) s_.supplier_suiteP->ReleaseObject((DRAWBOT_ObjectRef)brush);
        if (font) s_.supplier_suiteP->ReleaseObject((DRAWBOT_ObjectRef)font);
    }
    // a small "x" (remove) mark centred at (cx, cy)
    void Cross(float cx, float cy, float h, RGBA c)
    {
        Line(cx - h, cy - h, cx + h, cy + h, c, 1.4f);
        Line(cx - h, cy + h, cx + h, cy - h, c, 1.4f);
    }

private:
    template <class F>
    void Path(F build, RGBA c, bool fill, float width = 1)
    {
        if (!Ok()) return;
        DRAWBOT_PathRef path = nullptr;
        if (s_.supplier_suiteP->NewPath(sup_, &path) || !path) return;
        build(path);
        DRAWBOT_ColorRGBA col = {c.r, c.g, c.b, c.a};
        if (fill) {
            DRAWBOT_BrushRef brush = nullptr;
            if (!s_.supplier_suiteP->NewBrush(sup_, &col, &brush) && brush) {
                s_.surface_suiteP->FillPath(surf_, brush, path, kDRAWBOT_FillType_Default);
                s_.supplier_suiteP->ReleaseObject((DRAWBOT_ObjectRef)brush);
            }
        } else {
            DRAWBOT_PenRef pen = nullptr;
            if (!s_.supplier_suiteP->NewPen(sup_, &col, width, &pen) && pen) {
                s_.surface_suiteP->StrokePath(surf_, pen, path);
                s_.supplier_suiteP->ReleaseObject((DRAWBOT_ObjectRef)pen);
            }
        }
        s_.supplier_suiteP->ReleaseObject((DRAWBOT_ObjectRef)path);
    }
    void RoundPath(DRAWBOT_PathRef p, const Rect& r, float rad)
    {
        rad = std::min(rad, std::min(r.w, r.h) * 0.5f);
        auto arc = [&](float cx, float cy, float start) {
            DRAWBOT_PointF32 c = {cx, cy};
            s_.path_suiteP->AddArc(p, &c, rad, start, 90);
        };
        s_.path_suiteP->MoveTo(p, r.x + rad, r.y);
        s_.path_suiteP->LineTo(p, r.x + r.w - rad, r.y);
        arc(r.x + r.w - rad, r.y + rad, 270);
        s_.path_suiteP->LineTo(p, r.x + r.w, r.y + r.h - rad);
        arc(r.x + r.w - rad, r.y + r.h - rad, 0);
        s_.path_suiteP->LineTo(p, r.x + rad, r.y + r.h);
        arc(r.x + rad, r.y + r.h - rad, 90);
        s_.path_suiteP->LineTo(p, r.x, r.y + rad);
        arc(r.x + rad, r.y + rad, 180);
        s_.path_suiteP->Close(p);
    }

    PF_InData* in_;
    PF_OutData* out_;
    DRAWBOT_Suites s_ = {};
    bool acquired_ = false;
    DRAWBOT_DrawRef draw_ = nullptr;
    DRAWBOT_SupplierRef sup_ = nullptr;
    DRAWBOT_SurfaceRef surf_ = nullptr;
    float fontSize_ = 11;
    RGBA text_ = {0.85f, 0.85f, 0.85f, 1};
    RGBA panel_ = {0.14f, 0.14f, 0.14f, 1};
};

Rect Frame(const PF_EventExtra* ev)
{
    const PF_UnionableRect& f = ev->effect_win.current_frame;
    return {(float)f.left, (float)f.top, (float)(f.right - f.left), (float)(f.bottom - f.top)};
}

// ---- the lists' items (the same layout for drawing and for clicks) ----
enum class Act { None, AddModifier, RemoveModifier, AddSource, RemoveSource };
struct Item {
    Rect r;
    Act act;
    int slot;     // modifier / source slot
    Rect remove;  // the "x" (RemoveModifier / RemoveSource)
};

constexpr float kPad = 5, kChipH = 20, kGap = 3;
float g_lastFontSize = 11;  // UI thread only

// chips in equal columns (at least 3: 8 chips + the button fit in 3 rows); the button takes the rest of its row
std::vector<Item> ModifierItems(const Rect& f, PF_ParamDef* params[], float fontSize)
{
    std::vector<Item> items;
    const float inner = f.w - 2 * kPad;
    const int cols = std::max(3, (int)((inner + kGap) / (fontSize * 7 + kGap)));
    const float cw = (inner - (cols - 1) * kGap) / cols;
    int n = 0;
    auto cell = [&](int span) {
        const int c = n % cols, r = n / cols;
        n += span;
        return Rect{f.x + kPad + c * (cw + kGap), f.y + kPad + r * (kChipH + kGap), span * cw + (span - 1) * kGap, kChipH};
    };
    bool free = false;
    for (int k = 0; k < kModifierSlots; k++) {
        if (params[ModIndex(k, M_TYPE)]->u.pd.value == MT_NONE) {
            free = true;
            continue;
        }
        const Rect r = cell(1);
        items.push_back({r, Act::RemoveModifier, k, {r.x + r.w - 16, r.y, 16, r.h}});
    }
    if (free) items.push_back({cell(cols - n % cols), Act::AddModifier, -1, {}});
    return items;
}

std::vector<Item> SourceItems(const Rect& f, int rows, const SourceInfo src[kSourceSlots])
{
    std::vector<Item> items;
    const float colW = (f.w - 2 * kPad - kGap) * 0.5f;
    int n = 0;
    auto place = [&]() {
        Rect r = {f.x + kPad + (n % 2) * (colW + kGap), f.y + kPad + (n / 2) * (kChipH + 1), colW, kChipH - 2};
        n++;
        return r;
    };
    for (int k = 0; k < kSourceSlots; k++)
        if (src[k].used) {
            Rect r = place();
            items.push_back({r, Act::RemoveSource, k, {r.x + r.w - 18, r.y, 18, r.h}});
        }
    if (rows < kSourceSlots) items.push_back({place(), Act::AddSource, -1, {}});
    return items;
}

std::wstring Wide(const std::string& u8)
{
    if (u8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)std::max(n - 1, 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, &w[0], n);
    return w;
}

// ---- the three controls ----

void DrawOverview(Canvas& c, const Rect& f, PF_ParamDef* params[], const SourceInfo src[kSourceSlots])
{
    const int shape = std::clamp((int)params[P_SHAPE]->u.pd.value, 1, 4);
    const int A = std::max(1, (int)params[P_COUNT_A]->u.sd.value);
    const int B = std::max(1, (int)params[P_COUNT_B]->u.sd.value);
    const int C = std::max(1, (int)params[P_COUNT_C]->u.sd.value);
    const int assign = std::clamp((int)params[P_SRC_ASSIGN]->u.pd.value, 1, 4);
    const int seed = params[P_SEED]->u.sd.value;
    std::vector<int> used;
    for (int k = 0; k < kSourceSlots; k++)
        if (src[k].used) used.push_back(k);
    int mods = 0;
    for (int k = 0; k < kModifierSlots; k++)
        if (params[ModIndex(k, M_TYPE)]->u.pd.value != MT_NONE) mods++;

    // the picture: one dot per copy of the first C layer (at most 16 x 16 shown)
    const float box = std::min(f.h - 2 * kPad, 96.0f);
    const Rect pic = {f.x + kPad, f.y + (f.h - box) * 0.5f, box, box};
    c.FillRound(pic, 6, c.Shade(0.08f));
    const int sa = std::min(A, 16), sb = std::min(B, 16);
    const float cx = pic.x + box * 0.5f, cy = pic.y + box * 0.5f, R = box * 0.40f;
    const float dot = std::clamp(box / (std::max(sa, sb) * 3.2f), 1.2f, 4.5f);
    auto colorOf = [&](int i, int a, int b) {
        if (used.empty()) return c.Shade(0.75f);
        return kSourceColors[used[AssignSource(assign, seed, i, a, b, (int)used.size())]];
    };
    if (shape == SHAPE_PATH) {  // a gentle S curve with the copies along it
        std::vector<DRAWBOT_PointF32> pts;
        for (int s = 0; s <= 40; s++) {
            const float t = s / 40.0f;
            pts.push_back({pic.x + box * (0.1f + 0.8f * t), cy + R * 0.6f * std::sin(t * 6.2832f)});
        }
        c.Polyline(pts, c.Shade(0.3f), 1.2f);
    } else if (shape == SHAPE_SPHERE) {
        c.Circle(cx, cy, R, c.Shade(0.3f), false, 1.0f);
    }
    for (int b = 0; b < sb; b++)
        for (int a = 0; a < sa; a++) {
            const int i = b * A + a;
            float x = cx, y = cy;
            const float u = sa > 1 ? (float)a / (sa - 1) : 0.5f, v = sb > 1 ? (float)b / (sb - 1) : 0.5f;
            switch (shape) {
            case SHAPE_POLAR: {
                const float ang = 6.2832f * a / sa - 1.5708f, rr = R * (sb > 1 ? 0.25f + 0.75f * v : 1.0f);
                x = cx + rr * std::cos(ang), y = cy + rr * std::sin(ang);
                break;
            }
            case SHAPE_SPHERE: {  // longitude a, latitude b, seen from a little above
                const float lon = 6.2832f * a / sa, lat = 3.1416f * (sb > 1 ? v : 0.5f) - 1.5708f;
                const float px = std::cos(lat) * std::sin(lon), py = std::sin(lat), pz = std::cos(lat) * std::cos(lon);
                if (pz < -0.05f) continue;  // the far side
                x = cx + R * px, y = cy + R * (py * 0.94f - pz * 0.2f);
                break;
            }
            case SHAPE_PATH: {
                const float t = (sa > 1 ? u : 0.5f);
                x = pic.x + box * (0.1f + 0.8f * t) + (v - 0.5f) * box * 0.08f;
                y = cy + R * 0.6f * std::sin(t * 6.2832f) + (v - 0.5f) * box * 0.25f;
                break;
            }
            default:
                x = cx + (u - 0.5f) * 2 * R, y = cy + (v - 0.5f) * 2 * R;
                break;
            }
            c.Circle(x, y, dot, colorOf(i, a, b));
        }

    // the summary, in short lines (the narrowest panel leaves about 120 px beside the picture):
    //   DupliX 2 / Grid · 24 copies / A 6 Columns / B 4 Rows / C 1 Layers / Sources 3 · Random / Modifiers 2
    const float tx = pic.x + box + 10, tw = f.x + f.w - tx - kPad, fs = c.FontSize(), lh = fs * 1.3f;
    float y = f.y + kPad + fs * 1.3f;
    c.Text(tx, y, L"DupliX 2", c.Text(), fs * 1.35f, tw);
    y += lh + 3;
    wchar_t line[160];
    const long long total = (long long)A * B * C;
    swprintf(line, 160, L"%ls · %lld %ls", kShapeNames[shape - 1], total, total == 1 ? L"copy" : L"copies");
    c.Text(tx, y, line, c.Text(), 0, tw);
    const int counts[3] = {A, B, C};
    const float numW = fs * 3.2f;  // the counts in a column, the meanings after them
    for (int k = 0; k < 3; k++) {
        y += lh;
        c.Text(tx, y, std::wstring(1, (wchar_t)(L'A' + k)), c.Dim());
        c.Text(tx + fs * 1.1f + numW, y, std::to_wstring(counts[k]), c.Text(), 0, 0, kDRAWBOT_TextAlignment_Right);
        c.Text(tx + fs * 1.6f + numW, y, kAxisNames[shape - 1][k], c.Dim(), 0, tw - fs * 1.6f - numW);
    }
    y += lh;
    if (used.empty())
        swprintf(line, 160, L"Sources: this layer");
    else
        swprintf(line, 160, L"Sources %d · %ls", (int)used.size(), kAssignNames[assign - 1]);
    c.Text(tx, y, line, c.Dim(), 0, tw);
    y += lh;
    swprintf(line, 160, L"Modifiers %d", mods);
    c.Text(tx, y, line, c.Dim(), 0, tw);
}

void DrawButton(Canvas& c, const Rect& r, const wchar_t* label)
{
    c.StrokeRound(r, 4, c.Shade(0.45f), 1);
    c.Text(r.x + r.w * 0.5f, r.y + r.h * 0.5f + c.FontSize() * 0.36f, label, c.Text(), 0, r.w - 4,
           kDRAWBOT_TextAlignment_Center);
}

void DrawModifierList(Canvas& c, const Rect& f, PF_ParamDef* params[])
{
    const std::vector<Item> items = ModifierItems(f, params, c.FontSize());
    const float base = c.FontSize() * 0.36f;
    bool any = false;
    for (const Item& it : items) {
        if (it.act == Act::AddModifier) {
            DrawButton(c, it.r, it.r.w >= c.FontSize() * 8 ? L"+  Add Modifier" : L"+  Add");
            continue;
        }
        any = true;
        const int type = std::clamp((int)params[ModIndex(it.slot, M_TYPE)]->u.pd.value - 1, 0, 6);
        // [n| Type   x]: the slot number on a tab in the Type's colour
        c.FillRound(it.r, 4, c.Shade(0.14f));
        const float tab = 14;
        c.FillRound({it.r.x, it.r.y, tab, it.r.h}, 4, kTypeColors[type]);
        c.Text(it.r.x + tab * 0.5f, it.r.y + it.r.h * 0.5f + base, std::to_wstring(it.slot + 1), {0.1f, 0.1f, 0.1f, 1},
               0, 0, kDRAWBOT_TextAlignment_Center);
        c.Text(it.r.x + tab + 4, it.r.y + it.r.h * 0.5f + base, kTypeNames[type], c.Text(), 0,
               it.r.w - tab - 4 - it.remove.w);
        c.Cross(it.remove.x + it.remove.w * 0.5f, it.remove.y + it.remove.h * 0.5f, 3, c.Dim());
    }
    if (!any) {
        const Rect& last = items.empty() ? f : items.back().r;
        c.Text(f.x + kPad, last.y + last.h + c.FontSize() * 1.5f, L"Wave, Noise, Random... drive position, scale, opacity",
               c.Dim(), 0, f.w - 2 * kPad);
    }
}

void DrawSourceList(Canvas& c, const Rect& f, int rows, const SourceInfo src[kSourceSlots])
{
    const std::vector<Item> items = SourceItems(f, rows, src);
    const float base = c.FontSize() * 0.36f;
    bool any = false;
    for (const Item& it : items) {
        if (it.act == Act::AddSource) {
            DrawButton(c, it.r, L"+  Add Source");
            continue;
        }
        any = true;
        c.FillRound(it.r, 4, c.Shade(0.14f));
        c.FillRound({it.r.x + 5, it.r.y + it.r.h * 0.5f - 4, 8, 8}, 2, kSourceColors[it.slot]);
        std::wstring s = std::to_wstring(it.slot + 1) + L"  " + Wide(src[it.slot].name);
        c.Text(it.r.x + 18, it.r.y + it.r.h * 0.5f + base, s, c.Text(), 0, it.r.w - 38);
        c.Cross(it.remove.x + it.remove.w * 0.5f, it.remove.y + it.remove.h * 0.5f, 3, c.Dim());
    }
    if (!any) {
        const Rect& last = items.empty() ? f : items.back().r;
        c.Text(f.x + kPad, last.y + last.h + c.FontSize() * 1.5f, L"No Source: the copies show this layer.", c.Dim(), 0,
               f.w - 2 * kPad);
    }
}

PF_Err Draw(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* ev)
{
    if (ev->effect_win.area != PF_EA_CONTROL) return PF_Err_NONE;
    Canvas c(in_data, out_data, ev);
    if (!c.Ok()) return PF_Err_NONE;
    g_lastFontSize = c.FontSize();
    const Rect f = Frame(ev);
    static int logged = 0;
    if (logged < 30) {
        logged++;
        Log("draw: param %d, frame %.0f,%.0f %.0f x %.0f, font %.1f", (int)ev->effect_win.index, f.x, f.y, f.w, f.h,
            c.FontSize());
    }
    SourceInfo src[kSourceSlots];
    switch (ev->effect_win.index) {
    case P_HEADER:
        ReadSources(in_data, src);
        DrawOverview(c, f, params, src);
        break;
    case P_MOD_LIST:
        DrawModifierList(c, f, params);
        break;
    case P_SRC_LIST:
        ReadSources(in_data, src);
        DrawSourceList(c, f, SourceRowsShown(in_data, src), src);
        break;
    default:
        return PF_Err_NONE;
    }
    ev->evt_out_flags = PF_EO_HANDLED_EVENT;
    return PF_Err_NONE;
}

// ---- clicks ----

void UndoGroup(PF_InData* in_data, const char* name, bool start)
{
    Suite<AEGP_UtilitySuite6> u(in_data->pica_basicP, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (!u) return;
    if (start) u->AEGP_StartUndoGroup(name);
    else u->AEGP_EndUndoGroup();
}

PF_Err Click(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* ev)
{
    if (ev->effect_win.area != PF_EA_CONTROL) return PF_Err_NONE;
    const Rect f = Frame(ev);
    const float px = (float)ev->u.do_click.screen_point.h, py = (float)ev->u.do_click.screen_point.v;
    const float fontSize = g_lastFontSize;  // the chips' widths as last drawn
    std::vector<Item> items;
    SourceInfo src[kSourceSlots];
    int rows = 1;
    if (ev->effect_win.index == P_MOD_LIST) {
        items = ModifierItems(f, params, fontSize);
    } else if (ev->effect_win.index == P_SRC_LIST) {
        ReadSources(in_data, src);
        rows = SourceRowsShown(in_data, src);
        items = SourceItems(f, rows, src);
    } else {
        return PF_Err_NONE;
    }
    for (const Item& it : items) {
        if (!it.r.Has(px, py)) continue;
        const bool onX = it.remove.Has(px, py);
        switch (it.act) {
        case Act::AddModifier:
            for (int k = 0; k < kModifierSlots; k++) {
                PF_ParamDef* t = params[ModIndex(k, M_TYPE)];
                if (t->u.pd.value != MT_NONE) continue;
                // shown first: AE ignores a new value of a hidden parameter
                SetHiddenRange(in_data, ModIndex(k, M_TOPIC), ModIndex(k, M_TOPIC_END), false);
                t->u.pd.value = MT_WAVE;
                t->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
                UpdateVisibility(in_data, params);  // the rows Wave does not use
                break;
            }
            break;
        case Act::RemoveModifier:
            if (!onX) break;
            params[ModIndex(it.slot, M_TYPE)]->u.pd.value = MT_NONE;  // hidden by UPDATE_PARAMS_UI afterwards
            params[ModIndex(it.slot, M_TYPE)]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
            break;
        case Act::AddSource:
            SetMinSourceRows(in_data, rows + 1);
            UpdateVisibility(in_data, params);
            break;
        case Act::RemoveSource: {
            if (!onX) break;
            // the later Sources move up one row
            A_long ids[kSourceSlots] = {};
            int last = -1;
            for (int k = 0; k < kSourceSlots; k++)
                if ((ids[k] = SourceLayerId(in_data, k)) != 0) last = k;
            UndoGroup(in_data, "Remove Source", true);
            for (int k = it.slot; k <= last; k++) SetSourceLayer(in_data, k, k + 1 < kSourceSlots ? ids[k + 1] : 0);
            UndoGroup(in_data, nullptr, false);
            SetMinSourceRows(in_data, 0);
            UpdateVisibility(in_data, params);
            break;
        }
        default:
            break;
        }
        out_data->out_flags |= PF_OutFlag_REFRESH_UI;
        ev->evt_out_flags = PF_EO_HANDLED_EVENT;
        break;
    }
    return PF_Err_NONE;
}

}  // namespace

PF_Err HandleEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output,
                   PF_EventExtra* extra)
{
    (void)output;
    switch (extra->e_type) {
    case PF_Event_DRAW:
        return Draw(in_data, out_data, params, extra);
    case PF_Event_DO_CLICK:
        return Click(in_data, out_data, params, extra);
    default:
        return PF_Err_NONE;
    }
}

}  // namespace dx2
