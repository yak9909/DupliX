// DupliX 2 (design v5, 15): the graph editor's window (Win32, GDI, drawn by itself). Every change goes through
// dxg5/edit.h and ends in ed::Commit (one AE undo step); while a value is dragged only the window changes, and the
// release writes once (design v4 7).
//
// A node: its title, its outputs (a socket on the right each), its inputs (a socket on the left each, a name, the
// value when nothing is wired). Geometry wires are green, value wires grey (dashed when they carry a field).
//   right-click empty space      add a node
//   drag a node's title          move it            Delete: remove the selected node
//   drag from an output          to an input to wire (it snaps to the nearest input that fits, 24 px); right-click
//                                a wired input to unwire; drag from a wired input to lift its wire off (Blender)
//   click a value                type a number or a formula; drag it left / right to change a number
//   right-click a value          Expose to a macro, link to / unlink a macro, reset, expand a formula into nodes
//   drag a macro (right) onto a value   link it;  double-click a macro: rename it
//   Ctrl+Z / Ctrl+Shift+Z        AE's Undo / Redo
//   middle-drag                  pan;  wheel: zoom about the cursor;  Home: zoom to fit
//   the ▸ of a group's title     its line in the Code view
#include "v2/dx2_editor.h"

#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace dx2 {
namespace ed {
namespace {

constexpr int kNodeW = 200, kTitleH = 22, kRowH = 20, kPort = 5, kPanelW = 230, kTabH = 26, kStatusH = 20;
constexpr COLORREF kBg = RGB(30, 30, 32), kGridC = RGB(40, 40, 43), kNodeC = RGB(52, 52, 56), kNodeSel = RGB(70, 70, 78),
                   kTitleGen = RGB(48, 110, 90), kTitleSet = RGB(64, 92, 140), kTitleVal = RGB(90, 78, 120),
                   kTitleOut = RGB(120, 84, 50), kText = RGB(222, 222, 222), kDim = RGB(150, 150, 150),
                   kLinkC = RGB(110, 190, 240), kPanelC = RGB(38, 38, 41), kWarn = RGB(240, 160, 80), kTabC = RGB(44, 44, 48);

COLORREF TypeColor(dx5::Type t)
{
    switch (t) {
    case dx5::Type::Geometry: return RGB(90, 200, 140);
    case dx5::Type::Int: return RGB(60, 140, 90);
    case dx5::Type::Bool: return RGB(200, 120, 160);
    case dx5::Type::Vector: return RGB(120, 110, 220);
    case dx5::Type::Rotation: return RGB(150, 120, 230);
    default: return RGB(160, 160, 160);
    }
}

enum class Hit { None, Canvas, Title, GroupOpen, OutPort, InPort, Value, ExprText, Macro, Panel };
struct HitInfo {
    Hit what = Hit::None;
    uint32_t node = 0;
    int index = 0;  // the output, the input (row in Node::inputs), or the macro
    bool isInt = false;
};

enum class Drag { None, Move, Wire, Scrub, Pan, MacroLink };
enum class View { Nodes, Stack, Code };
constexpr int kApplyId = 9001;
constexpr double kZoomMin = 0.3, kZoomMax = 3, kSnap = 24;

struct MacroRowInfo {
    bool isInt;
    int k;
    bool header = false;  // the "Int Macros" / "Macros" heading
};

struct Win {
    Session* s = nullptr;
    HWND wnd = nullptr, edit = nullptr;
    HFONT font = nullptr, bold = nullptr;
    // the node view: screen = world * zoom + pan (+ the tabs); its fonts are made again for each zoom
    double panX = 30, panY = 30, zoom = 1;
    HFONT zFont = nullptr, zBold = nullptr;
    double fontZoom = 0;
    // a wire being dragged: the input it snaps to; a wire lifted off an input (restored if the drag is cut short)
    uint32_t snapNode = 0;
    int snapInput = -1;
    bool lifted = false;
    uint32_t liftNode = 0;
    std::string liftInput;
    dx5::Link liftLink{};
    dx5::Graph liftBefore;
    // a macro dragged from the panel
    bool macroDragInt = false;
    int macroDragK = -1;
    // the add menu's search
    bool editSearch = false, enterPressed = false;
    POINT searchAt{};
    uint32_t selected = 0;
    // a drag
    Drag drag = Drag::None;
    POINT start{}, now{};
    uint32_t dragNode = 0;
    int dragIndex = 0;
    double startX = 0, startY = 0, startValue = 0;
    bool moved = false;
    // the edit box: what it edits
    uint32_t editNode = 0;
    int editInput = -1, editMacro = -1;
    bool editMacroInt = false, editExprText = false;
    WNDPROC editProc = nullptr;
    std::vector<MacroRowInfo> macroRows;  // the macros listed in the panel, top to bottom
    std::string status;                   // the last refusal (shown at the bottom)
    // the views (design v5 8.4, 14)
    View view = View::Nodes;
    HWND code = nullptr, apply = nullptr, codeMsg = nullptr;
    HFONT mono = nullptr;
    WNDPROC codeProc = nullptr;
    bool codeError = false;
} g_win;

std::wstring W(const std::string& u8)
{
    if (u8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)std::max(n - 1, 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, &w[0], n);
    return w;
}
std::string U8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n - 1, 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string Num(double v, bool integer)
{
    char b[64];
    if (integer) snprintf(b, sizeof(b), "%lld", (long long)std::llround(v));
    else snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

const dx5::InputDef* InDef(const dx5::Node& n, const std::string& name)
{
    const dx5::NodeDef* d = dx5::FindDef(n.type);
    const int k = d ? d->Input(name) : -1;
    return k >= 0 ? &d->inputs[k] : nullptr;
}
int NumOutputs(const dx5::Node& n)
{
    const dx5::NodeDef* d = dx5::FindDef(n.type);
    return d ? (int)d->outputs.size() : 0;
}
bool HasTextRow(const dx5::Node& n) { return n.type == "expr"; }
bool IsMacroNode(const dx5::Node& n) { return n.type == "macro" || n.type == "int_macro"; }

// ---- layout ----
RECT CanvasRect()
{
    RECT c;
    GetClientRect(g_win.wnd, &c);
    c.top = kTabH;
    c.right = std::max(c.left, c.right - kPanelW);
    c.bottom = std::max(c.top, c.bottom - kStatusH);
    return c;
}
int FirstInputRow(const dx5::Node& n) { return NumOutputs(n) + (HasTextRow(n) ? 1 : 0); }
// a length of the node view on the screen
int Z(double v) { return (int)std::lround(v * g_win.zoom); }
int WorldH(const dx5::Node& n) { return kTitleH + (FirstInputRow(n) + (int)n.inputs.size()) * kRowH + 4; }
RECT NodeRect(const dx5::Node& n)
{
    const int x = (int)std::lround(n.x * g_win.zoom + g_win.panX), y = (int)std::lround(n.y * g_win.zoom + g_win.panY) + kTabH;
    return {x, y, x + Z(kNodeW), y + Z(WorldH(n))};
}
int RowTop(const dx5::Node& n, int row) { return NodeRect(n).top + Z(kTitleH + row * kRowH); }
int RowMid(const dx5::Node& n, int row) { return NodeRect(n).top + Z(kTitleH + row * kRowH + kRowH / 2); }
POINT OutPort(const dx5::Node& n, int out) { return {NodeRect(n).right, RowMid(n, out)}; }
POINT InPort(const dx5::Node& n, int in) { return {NodeRect(n).left, RowMid(n, FirstInputRow(n) + in)}; }
RECT ValueRect(const dx5::Node& n, int in)
{
    const RECT r = NodeRect(n);
    const int y = RowTop(n, FirstInputRow(n) + in);
    return {r.left + Z(kNodeW / 2), y + 1, r.right - Z(6), y + Z(kRowH) - 1};
}
RECT TextRect(const dx5::Node& n)
{
    const RECT r = NodeRect(n);
    const int y = RowTop(n, NumOutputs(n));
    return {r.left + Z(8), y + 1, r.right - Z(8), y + Z(kRowH) - 1};
}
// the ▸ at the right of a group's title (design v5 6.1: opens its code)
RECT GroupOpenRect(const dx5::Node& n)
{
    const RECT r = NodeRect(n);
    return {r.right - Z(22), r.top, r.right, r.top + Z(kTitleH)};
}
bool IsGroup(const dx5::Node& n)
{
    const dx5::NodeDef* d = dx5::FindDef(n.type);
    return d && d->group;
}
int PortR() { return std::max(2, Z(kPort)); }

void ZoomFonts()
{
    if (g_win.fontZoom == g_win.zoom && g_win.zFont) return;
    if (g_win.zFont) DeleteObject(g_win.zFont);
    if (g_win.zBold) DeleteObject(g_win.zBold);
    const int h = -std::max(4, Z(13));
    g_win.zFont = CreateFontW(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_win.zBold = CreateFontW(h, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_win.fontZoom = g_win.zoom;
}
RECT MacroRow(int row)
{
    const RECT c = CanvasRect();
    RECT r;
    GetClientRect(g_win.wnd, &r);
    const int y = kTabH + 34 + row * 22;
    return {c.right + 8, y, r.right - 8, y + 20};
}

bool In(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
bool Near(POINT a, POINT b, int d) { return std::abs(a.x - b.x) <= d && std::abs(a.y - b.y) <= d; }

HitInfo HitTest(POINT p)
{
    Session* s = g_win.s;
    HitInfo h;
    if (!s) return h;
    const RECT c = CanvasRect();
    if (!In(c, p)) {
        for (size_t row = 0; row < g_win.macroRows.size(); row++)
            if (!g_win.macroRows[row].header && In(MacroRow((int)row), p)) {
                HitInfo m{Hit::Macro, 0, g_win.macroRows[row].k};
                m.isInt = g_win.macroRows[row].isInt;
                return m;
            }
        return {Hit::Panel, 0, 0};
    }
    for (auto it = s->graph.nodes.rbegin(); it != s->graph.nodes.rend(); ++it) {
        const dx5::Node& n = *it;
        for (int k = 0; k < NumOutputs(n); k++)
            if (Near(OutPort(n, k), p, PortR() + 3)) return {Hit::OutPort, n.id, k};
        for (int k = 0; k < (int)n.inputs.size(); k++)
            if (Near(InPort(n, k), p, PortR() + 3)) return {Hit::InPort, n.id, k};
        const RECT r = NodeRect(n);
        if (!In(r, p)) continue;
        if (IsGroup(n) && In(GroupOpenRect(n), p)) return {Hit::GroupOpen, n.id, 0};
        if (p.y < r.top + Z(kTitleH)) return {Hit::Title, n.id, 0};
        if (HasTextRow(n) && In(TextRect(n), p)) return {Hit::ExprText, n.id, 0};
        for (int i = 0; i < (int)n.inputs.size(); i++)
            if (In(ValueRect(n, i), p)) return {Hit::Value, n.id, i};
        return {Hit::Title, n.id, 0};
    }
    return {Hit::Canvas, 0, 0};
}

// ---- what a value shows ----
std::string ConstText(const dx5::Node& n, const dx5::InputSlot& s)
{
    const dx5::InputDef* d = InDef(n, s.name);
    const dx5::Value& v = s.value;
    if (d && !d->choices.empty()) {
        const std::string& c = d->choices;
        int idx = (int)dx5::AsInt(v), i = 0;
        size_t a = 0;
        while (i < idx && a != std::string::npos) {
            a = c.find('|', a);
            if (a != std::string::npos) a++;
            i++;
        }
        if (a == std::string::npos) return Num((double)idx, true);
        const size_t b = c.find('|', a);
        return c.substr(a, b == std::string::npos ? std::string::npos : b - a);
    }
    switch (v.type) {
    case dx5::Type::Int: return Num((double)v.i, true);
    case dx5::Type::Bool: return v.i ? "true" : "false";
    case dx5::Type::Vector: return Num(v.v.x, false) + ", " + Num(v.v.y, false) + ", " + Num(v.v.z, false);
    case dx5::Type::Rotation: return "(rotation)";
    default: return Num(v.f, false);
    }
}

std::string LinkText(const dx5::Graph& g, const dx5::InputSlot& s)
{
    const dx5::Node* f = g.Find(s.link.from);
    if (!f) return "?";
    if (f->type == "macro") return dx5::MacroLabel(g, false, f->setting - 1);
    if (f->type == "int_macro") return dx5::MacroLabel(g, true, f->setting - 1);
    if (f->type == "expr") return dx5::ExprText(g, f->id, s.link.output);
    return "<- " + f->key;
}

// ---- drawing ----
void Text(HDC dc, const std::string& s, RECT r, COLORREF c, UINT fmt)
{
    SetTextColor(dc, c);
    const std::wstring w = W(s);
    DrawTextW(dc, w.c_str(), (int)w.size(), &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void FillRect(HDC dc, const RECT& r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    ::FillRect(dc, &r, b);
    DeleteObject(b);
}

void Wire(HDC dc, POINT a, POINT b, COLORREF c, bool dashed)
{
    LOGBRUSH lb{BS_SOLID, c, 0};
    HPEN pen = dashed ? ExtCreatePen(PS_GEOMETRIC | PS_DASH, 2, &lb, 0, nullptr) : CreatePen(PS_SOLID, 2, c);
    HGDIOBJ old = SelectObject(dc, pen);
    const int dx = std::max(Z(40), (int)std::abs(b.x - a.x) / 2);
    POINT pts[4] = {a, {a.x + dx, a.y}, {b.x - dx, b.y}, b};
    PolyBezier(dc, pts, 4);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void Port(HDC dc, POINT p, COLORREF c, bool filled)
{
    HBRUSH b = CreateSolidBrush(filled ? c : kNodeC);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    const int k = PortR();
    Ellipse(dc, p.x - k, p.y - k, p.x + k + 1, p.y + k + 1);
    SelectObject(dc, ob), SelectObject(dc, op);
    DeleteObject(b), DeleteObject(pen);
}

COLORREF TitleColor(const dx5::Node& n)
{
    const dx5::NodeDef* d = dx5::FindDef(n.type);
    if (!d) return kDim;
    if (d->category == "Output") return kTitleOut;
    if (d->category == "Generate") return kTitleGen;
    if (d->category == "Set" || d->category == "Geometry" || d->category == "Zone") return kTitleSet;
    return kTitleVal;
}

dx5::Type OutType(const dx5::Graph& g, uint32_t node, int out)
{
    const dx5::Node* n = g.Find(node);
    const dx5::NodeDef* d = n ? dx5::FindDef(n->type) : nullptr;
    return d && out < (int)d->outputs.size() ? d->outputs[out].type : dx5::Type::Float;
}

// The input nearest to p (within kSnap px on the screen) that the wire being dragged can feed (Connect would take it).
bool FindSnap(POINT p, uint32_t* node, int* input)
{
    Session* s = g_win.s;
    if (!s) return false;
    double best = kSnap * kSnap + 1;
    for (const dx5::Node& n : s->graph.nodes)
        for (int k = 0; k < (int)n.inputs.size(); k++) {
            const POINT q = InPort(n, k);
            const double d2 = double(q.x - p.x) * (q.x - p.x) + double(q.y - p.y) * (q.y - p.y);
            if (d2 > kSnap * kSnap || d2 >= best) continue;
            dx5::Graph probe = s->graph;
            if (!dx5::Connect(probe, g_win.dragNode, g_win.dragIndex, n.id, n.inputs[k].name, nullptr)) continue;
            best = d2, *node = n.id, *input = k;
        }
    return best <= kSnap * kSnap;
}

RECT TabRect(View v)
{
    const int i = (int)v;
    return {8 + i * 80, 0, 8 + i * 80 + 72, kTabH};
}

void PaintTabs(HDC dc, const RECT& client)
{
    RECT tabs = {client.left, client.top, client.right, client.top + kTabH};
    FillRect(dc, tabs, kTabC);
    const char* names[3] = {"Nodes", "Stack", "Code"};
    for (int i = 0; i < 3; i++) {
        RECT t = TabRect((View)i);
        const bool on = (int)g_win.view == i;
        if (on) FillRect(dc, {t.left, t.top + 3, t.right, t.bottom}, kBg);
        SelectObject(dc, on ? g_win.bold : g_win.font);
        Text(dc, names[i], t, on ? kText : kDim, DT_CENTER);
    }
    SelectObject(dc, g_win.font);
}

void PaintNodes(HDC dc, const RECT& client);
void PaintPanel(HDC dc, const RECT& client);

void Paint(HDC dc, const RECT& client)
{
    FillRect(dc, client, kBg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_win.font);
    if (g_win.view == View::Nodes) PaintNodes(dc, client);
    else if (g_win.view == View::Stack) {
        const RECT c = CanvasRect();
        RECT m = {c.left + 16, c.top + 16, c.right - 16, c.top + 40};
        Text(dc, "The stack view comes with the next build.", m, kDim, DT_LEFT);
    }
    PaintPanel(dc, client);
    PaintTabs(dc, client);
}

void PaintNodes(HDC dc, const RECT& client)
{
    Session* s = g_win.s;
    const RECT c = CanvasRect();
    // a grid that moves with the view
    HPEN gp = CreatePen(PS_SOLID, 1, kGridC);
    HGDIOBJ old = SelectObject(dc, gp);
    const double step = 40 * g_win.zoom;
    const double gx = std::fmod(g_win.panX, step), gy = std::fmod(g_win.panY, step);
    for (double x = gx < 0 ? gx + step : gx; x < c.right; x += step)
        MoveToEx(dc, (int)x, c.top, nullptr), LineTo(dc, (int)x, c.bottom);
    for (double y = c.top + (gy < 0 ? gy + step : gy); y < c.bottom; y += step)
        MoveToEx(dc, 0, (int)y, nullptr), LineTo(dc, c.right, (int)y);
    SelectObject(dc, old);
    DeleteObject(gp);
    SetBkMode(dc, TRANSPARENT);
    ZoomFonts();
    SelectObject(dc, g_win.zFont);
    if (!s) return;
    const dx5::Graph& g = s->graph;
    // Each zones: a frame around the nodes inside (design v5 9)
    for (const dx5::Node& z : g.nodes) {
        if (z.type != "each") continue;
        RECT f = NodeRect(z);
        for (const dx5::Node& n : g.nodes)
            if (n.zone == z.id) {
                const RECT r = NodeRect(n);
                f.left = std::min(f.left, r.left), f.top = std::min(f.top, r.top);
                f.right = std::max(f.right, r.right), f.bottom = std::max(f.bottom, r.bottom);
            }
        InflateRect(&f, Z(14), Z(14));
        HPEN zp = CreatePen(PS_SOLID, 1, RGB(110, 100, 150));
        HGDIOBJ op = SelectObject(dc, zp), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, f.left, f.top, f.right, f.bottom, Z(12), Z(12));
        SelectObject(dc, op), SelectObject(dc, ob);
        DeleteObject(zp);
    }
    for (const dx5::Node& n : g.nodes)
        for (int k = 0; k < (int)n.inputs.size(); k++) {
            const dx5::InputSlot& in = n.inputs[k];
            const dx5::Node* a = in.link.from ? g.Find(in.link.from) : nullptr;
            if (!a) continue;
            const dx5::Type t = OutType(g, a->id, in.link.output);
            Wire(dc, OutPort(*a, in.link.output), InPort(n, k), TypeColor(t),
                 t != dx5::Type::Geometry && dx5::IsField(g, a->id, in.link.output));
        }
    if (g_win.drag == Drag::Wire) {
        const dx5::Node* a = g.Find(g_win.dragNode);
        const dx5::Node* b = g_win.snapNode ? g.Find(g_win.snapNode) : nullptr;
        const POINT end = b && g_win.snapInput < (int)b->inputs.size() ? InPort(*b, g_win.snapInput) : g_win.now;
        if (a) Wire(dc, OutPort(*a, g_win.dragIndex), end, b ? TypeColor(OutType(g, a->id, g_win.dragIndex)) : kLinkC, false);
    }
    for (const dx5::Node& n : g.nodes) {
        const dx5::NodeDef* d = dx5::FindDef(n.type);
        const RECT r = NodeRect(n);
        if (r.right < c.left || r.left > c.right || r.bottom < c.top || r.top > c.bottom) continue;
        FillRect(dc, r, n.id == g_win.selected ? kNodeSel : kNodeC);
        RECT t = {r.left, r.top, r.right, r.top + Z(kTitleH)};
        FillRect(dc, t, TitleColor(n));
        SelectObject(dc, g_win.zBold);
        t.left += Z(10);
        if (IsGroup(n)) {
            t.right -= Z(22);
            Text(dc, "\xE2\x96\xB8", GroupOpenRect(n), kText, DT_CENTER);  // ▸
        }
        std::string title = d ? d->label : n.type + " (unknown)";
        if (IsMacroNode(n)) title = dx5::MacroLabel(g, n.type == "int_macro", n.setting - 1);
        Text(dc, title, t, kText, DT_LEFT);
        SelectObject(dc, g_win.zFont);
        for (int k = 0; d && k < (int)d->outputs.size(); k++) {
            RECT orow = {r.left + Z(10), RowTop(n, k), r.right - Z(12), RowTop(n, k) + Z(kRowH)};
            Text(dc, d->outputs[k].name, orow, kDim, DT_RIGHT);
            Port(dc, OutPort(n, k), TypeColor(d->outputs[k].type), false);
        }
        if (HasTextRow(n)) {
            RECT tr = TextRect(n);
            FillRect(dc, tr, RGB(40, 40, 44));
            tr.left += Z(5), tr.right -= Z(5);
            Text(dc, n.text, tr, kLinkC, DT_LEFT);
        }
        for (int i = 0; i < (int)n.inputs.size(); i++) {
            const dx5::InputSlot& in = n.inputs[i];
            const dx5::InputDef* id = InDef(n, in.name);
            RECT lr = {r.left + Z(10), RowTop(n, FirstInputRow(n) + i), r.left + Z(kNodeW / 2 - 4), 0};
            lr.bottom = lr.top + Z(kRowH);
            Text(dc, in.name, lr, kDim, DT_LEFT);
            if (!id || id->type != dx5::Type::Geometry) {
                RECT vr = ValueRect(n, i);
                if (!in.link.from) {
                    FillRect(dc, vr, RGB(40, 40, 44));
                    vr.left += Z(5), vr.right -= Z(5);
                    Text(dc, ConstText(n, in), vr, kText, DT_RIGHT);
                } else {
                    // a wired input has no value field; a formula or a macro shows its name, faint
                    const dx5::Node* f = g.Find(in.link.from);
                    vr.right -= Z(5);
                    if (f && (f->type == "expr" || IsMacroNode(*f))) Text(dc, LinkText(g, in), vr, RGB(120, 150, 175), DT_RIGHT);
                }
            }
            Port(dc, InPort(n, i), TypeColor(id ? id->type : dx5::Type::Float), in.link.from != 0);
        }
    }
    if (g_win.drag == Drag::MacroLink && g_win.moved && g_win.macroDragK >= 0) {
        // the macro being dragged, under the cursor
        const std::string chip = (g_win.macroDragInt ? "I" : "M") + std::to_string(g_win.macroDragK + 1) + "  " +
                                 dx5::MacroLabel(g, g_win.macroDragInt, g_win.macroDragK);
        RECT cr = {g_win.now.x + 12, g_win.now.y - 10, g_win.now.x + 172, g_win.now.y + 10};
        FillRect(dc, cr, RGB(60, 80, 100));
        SelectObject(dc, g_win.font);
        cr.left += 6;
        Text(dc, chip, cr, kText, DT_LEFT);
    }
    SelectObject(dc, g_win.font);
    if (!g.OutputNode()) {
        RECT m = {c.left + 10, c.bottom - 28, c.right - 10, c.bottom - 8};
        Text(dc, "No Output node: right-click to add one", m, kWarn, DT_LEFT);
    }

    // the status line
    RECT st = {client.left + 8, client.bottom - kStatusH, client.right - kPanelW, client.bottom};
    FillRect(dc, {client.left, client.bottom - kStatusH, client.right - kPanelW, client.bottom}, kTabC);
    Text(dc, g_win.status, st, kWarn, DT_LEFT);
}

// the macros panel (right; every view)
void PaintPanel(HDC dc, const RECT& client)
{
    Session* s = g_win.s;
    if (!s) return;
    const dx5::Graph& g = s->graph;
    const RECT c = CanvasRect();
    RECT p = {c.right, kTabH, client.right, client.bottom};
    FillRect(dc, p, kPanelC);
    SelectObject(dc, g_win.bold);
    RECT h = {c.right + 10, kTabH + 6, client.right - 8, kTabH + 28};
    Text(dc, "Macros", h, kText, DT_LEFT);
    SelectObject(dc, g_win.font);
    g_win.macroRows.clear();
    const uint32_t usedI = dx5::UsedIntMacros(g), usedM = dx5::UsedMacros(g);
    // two lists under their headings: I (Int Macro) and M (Macro)
    for (int pass = 0; pass < 2; pass++) {
        const bool isInt = pass == 0;
        const uint32_t used = isInt ? usedI : usedM;
        const size_t head = g_win.macroRows.size();
        for (int k = 0; k < 32; k++)
            if ((used >> k & 1) || !(isInt ? g.intMacros : g.macros)[k].name.empty()) {
                if (g_win.macroRows.size() == head) g_win.macroRows.push_back({isInt, -1, true});
                g_win.macroRows.push_back({isInt, k});
            }
    }
    for (size_t row = 0; row < g_win.macroRows.size(); row++) {
        const MacroRowInfo& m = g_win.macroRows[row];
        RECT r = MacroRow((int)row);
        if (m.header) {
            r.left += 2;
            Text(dc, m.isInt ? "I  Int Macros" : "M  Macros", r, kDim, DT_LEFT);
            continue;
        }
        FillRect(dc, r, RGB(46, 46, 50));
        RECT a = {r.left + 6, r.top, r.left + 34, r.bottom};
        Text(dc, (m.isInt ? "I" : "M") + std::to_string(m.k + 1), a, kDim, DT_LEFT);
        RECT b = {r.left + 34, r.top, r.right - 80, r.bottom};
        const bool used = ((m.isInt ? usedI : usedM) >> m.k) & 1;
        Text(dc, dx5::MacroLabel(g, m.isInt, m.k), b, used ? kLinkC : kDim, DT_LEFT);
        const dx5::MacroSetting& ms = m.isInt ? g.intMacros[m.k] : g.macros[m.k];
        RECT rr = {r.right - 80, r.top, r.right - 6, r.bottom};
        Text(dc, Num(ms.sliderMin, false) + ".." + Num(ms.sliderMax, false), rr, kDim, DT_RIGHT);
    }
    // the help below the list, wrapped to the panel and cut at its bottom (it was cut off at the side, v4 1.3)
    const int listBottom = g_win.macroRows.empty() ? kTabH + 34 : MacroRow((int)g_win.macroRows.size() - 1).bottom + 12;
    RECT help = {c.right + 10, std::max(listBottom, (int)client.bottom - 200), client.right - 10, client.bottom - 8};
    SetTextColor(dc, kDim);
    const wchar_t* hint =
        L"Right-click: add a node, or a value's menu (Expose to Macro).\n"
        L"Click a value to type a number or a formula; drag it to change it.\n"
        L"Drag from an output to an input to wire; drag from a wired input to lift its wire.\n"
        L"Wheel: zoom.  Middle-drag: pan.  Home: fit.\n"
        L"Drag a macro onto a value to link it; double-click it to rename.\n"
        L"Delete: remove the node.  Ctrl+Z: undo.\n"
        L"Keys go on Macro / Int Macro in the Effect Controls.";
    DrawTextW(dc, hint, -1, &help, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void Redraw() { InvalidateRect(g_win.wnd, nullptr, FALSE); }
void Refused(const std::string& why)
{
    g_win.status = why;
    Redraw();
}

// ---- the code view (design v5 8.4) ----
std::wstring ToEdit(const std::string& s)
{
    std::string t;
    for (char c : s) {
        if (c == '\n') t += '\r';
        t += c;
    }
    return W(t);
}

void CodeMessage(const std::string& m, bool error)
{
    g_win.codeError = error;
    if (g_win.codeMsg) SetWindowTextW(g_win.codeMsg, W(m).c_str());
}

void LayoutCode()
{
    if (!g_win.code) return;
    const RECT c = CanvasRect();
    const int barY = c.bottom - 30;
    MoveWindow(g_win.code, c.left + 4, c.top + 4, c.right - c.left - 8, barY - c.top - 8, TRUE);
    MoveWindow(g_win.apply, c.left + 4, barY, 150, 24, TRUE);
    MoveWindow(g_win.codeMsg, c.left + 162, barY + 4, c.right - c.left - 170, 20, TRUE);
}

void FillCode()
{
    if (!g_win.code || !g_win.s) return;
    SetWindowTextW(g_win.code, ToEdit(dx5::GraphToCode(g_win.s->graph)).c_str());
    SendMessageW(g_win.code, EM_SETMODIFY, FALSE, 0);
}

void SelectLine(int line)
{
    const LRESULT a = SendMessageW(g_win.code, EM_LINEINDEX, (WPARAM)std::max(0, line - 1), 0);
    if (a < 0) return;
    const LRESULT len = SendMessageW(g_win.code, EM_LINELENGTH, (WPARAM)a, 0);
    SendMessageW(g_win.code, EM_SETSEL, (WPARAM)a, (LPARAM)(a + len));
    SendMessageW(g_win.code, EM_SCROLLCARET, 0, 0);
    SetFocus(g_win.code);
}

void ApplyCodeNow()
{
    Session* s = g_win.s;
    if (!s || !g_win.code) return;
    const int n = GetWindowTextLengthW(g_win.code);
    std::wstring w((size_t)n + 1, L'\0');
    GetWindowTextW(g_win.code, &w[0], n + 1);
    w.resize((size_t)n);
    std::string code;
    for (char c : U8(w))
        if (c != '\r') code += c;
    std::vector<dx5::Diag> errs, notes;
    if (!dx5::ApplyCode(s->graph, code, &errs, &notes)) {
        const dx5::Diag d = errs.empty() ? dx5::Diag{} : errs[0];
        char b[64];
        snprintf(b, sizeof(b), "line %d col %d: ", d.pos.line, d.pos.col);
        const std::string m = std::string(b) + (errs.empty() ? "cannot read the code" : d.msg);
        Log("editor: code refused: %s", m.c_str());
        CodeMessage(m, true);
        SelectLine(d.pos.line);
        return;
    }
    Commit("apply code");
    std::string m = "Applied.";
    if (!notes.empty()) m = notes[0].msg;
    CodeMessage(m, false);
    FillCode();
}

LRESULT CALLBACK CodeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_RETURN && GetKeyState(VK_CONTROL) < 0) {
        ApplyCodeNow();
        return 0;
    }
    if (m == WM_CHAR && w == '\n') return 0;  // Ctrl+Enter's character
    if (m == WM_CHAR && w == '\t') {
        SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"    ");
        return 0;
    }
    if (m == WM_KEYDOWN && w == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_KEYDOWN && w == 'R' && GetKeyState(VK_CONTROL) < 0) {  // take the graph as it is now
        FillCode();
        CodeMessage("", false);
        return 0;
    }
    if (m == WM_CHAR && (w == 1 || w == 18)) return 0;  // Ctrl+A / Ctrl+R: no beep
    return CallWindowProcW(g_win.codeProc, h, m, w, l);
}

void SetView(View v)
{
    if (g_win.edit) SetFocus(g_win.wnd);  // ends a value's edit box
    g_win.view = v;
    if (v == View::Code) {
        if (!g_win.code) {
            g_win.code = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                             ES_WANTRETURN | ES_NOHIDESEL,
                                         0, 0, 10, 10, g_win.wnd, nullptr, nullptr, nullptr);
            g_win.apply = CreateWindowExW(0, L"BUTTON", L"Apply (Ctrl+Enter)", WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10, g_win.wnd,
                                          (HMENU)(INT_PTR)kApplyId, nullptr, nullptr);
            g_win.codeMsg = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFTNOWORDWRAP, 0, 0, 10, 10, g_win.wnd, nullptr, nullptr, nullptr);
            SendMessageW(g_win.code, WM_SETFONT, (WPARAM)g_win.mono, TRUE);
            SendMessageW(g_win.code, EM_SETLIMITTEXT, 0, 0);
            SendMessageW(g_win.apply, WM_SETFONT, (WPARAM)g_win.font, TRUE);
            SendMessageW(g_win.codeMsg, WM_SETFONT, (WPARAM)g_win.font, TRUE);
            g_win.codeProc = (WNDPROC)SetWindowLongPtrW(g_win.code, GWLP_WNDPROC, (LONG_PTR)CodeProc);
        }
        LayoutCode();
        FillCode();
        CodeMessage("", false);
        for (HWND c : {g_win.code, g_win.apply, g_win.codeMsg}) ShowWindow(c, SW_SHOW);
        SetFocus(g_win.code);
    } else {
        for (HWND c : {g_win.code, g_win.apply, g_win.codeMsg})
            if (c) ShowWindow(c, SW_HIDE);
        SetFocus(g_win.wnd);
    }
    Redraw();
}

// ---- the edit box ----
void EndEdit(bool commit);

LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
        g_win.enterPressed = w == VK_RETURN;
        EndEdit(w == VK_RETURN);
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;  // no beep
    if (m == WM_KILLFOCUS) {
        LRESULT r = CallWindowProcW(g_win.editProc, h, m, w, l);
        EndEdit(true);
        return r;
    }
    return CallWindowProcW(g_win.editProc, h, m, w, l);
}

void BeginEdit(const RECT& r, const std::string& text, HFONT font = nullptr)
{
    if (g_win.edit) EndEdit(false);
    g_win.enterPressed = false;
    g_win.edit = CreateWindowExW(0, L"EDIT", W(text).c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, r.left, r.top,
                                 r.right - r.left, r.bottom - r.top, g_win.wnd, nullptr, nullptr, nullptr);
    SendMessageW(g_win.edit, WM_SETFONT, (WPARAM)(font ? font : g_win.font), TRUE);
    g_win.editProc = (WNDPROC)SetWindowLongPtrW(g_win.edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    SendMessageW(g_win.edit, EM_SETSEL, 0, -1);
    SetFocus(g_win.edit);
}
void BeginValueEdit(const dx5::Node& n, int input)
{
    g_win.editNode = n.id, g_win.editInput = input, g_win.editMacro = -1, g_win.editExprText = false, g_win.editSearch = false;
    const dx5::InputSlot& s = n.inputs[input];
    std::string text = ConstText(n, s);
    if (s.link.from) {
        const dx5::Node* f = g_win.s->graph.Find(s.link.from);
        text = f && f->type == "expr" ? dx5::ExprText(g_win.s->graph, f->id, 0) : LinkText(g_win.s->graph, s);
        if (f && (f->type == "macro" || f->type == "int_macro")) text = std::string(f->type == "macro" ? "m" : "i") + std::to_string(f->setting);
    }
    ZoomFonts();
    BeginEdit(ValueRect(n, input), text, g_win.zFont);
}

void SearchMenu(POINT client, const std::string& text);

void EndEdit(bool commit)
{
    HWND e = g_win.edit;
    if (!e) return;
    g_win.edit = nullptr;  // first: DestroyWindow sends WM_KILLFOCUS again
    wchar_t buf[1024] = {0};
    GetWindowTextW(e, buf, 1023);
    DestroyWindow(e);
    SetFocus(g_win.wnd);
    Session* s = g_win.s;
    if (!commit || !s) return Redraw();
    const std::string text = U8(buf);
    if (g_win.editSearch) {
        g_win.editSearch = false;
        if (g_win.enterPressed) SearchMenu(g_win.searchAt, text);  // not when the box only lost the focus
        return Redraw();
    }
    if (g_win.editMacro >= 0) {
        if (dx5::RenameMacro(s->graph, g_win.editMacroInt, g_win.editMacro, text)) Commit("rename macro");
    } else if (g_win.editExprText) {
        dx5::Node* n = s->graph.Find(g_win.editNode);
        std::unique_ptr<dx5::Expr> ex;
        dx5::Diag d;
        if (!n) return Redraw();
        if (!dx5::ParseExpr(text, &ex, &d)) return Refused("formula: col " + std::to_string(d.pos.col) + ": " + d.msg);
        if (n->text != dx5::PrintExpr(*ex)) {
            n->text = dx5::PrintExpr(*ex);
            Commit("edit formula");
        }
    } else {
        dx5::Node* n = s->graph.Find(g_win.editNode);
        if (n && g_win.editInput >= 0 && g_win.editInput < (int)n->inputs.size()) {
            const std::string key = n->inputs[g_win.editInput].name;
            const dx5::Graph before = s->graph;
            std::string why;
            bool isInt = false;
            int k = -1;
            // "m3" / "i2": link that macro
            if (text.size() >= 2 && (text[0] == 'm' || text[0] == 'i') && std::all_of(text.begin() + 1, text.end(), ::isdigit)) {
                isInt = text[0] == 'i';
                k = std::atoi(text.c_str() + 1) - 1;
            }
            Log("editor: %s.%s = '%s'", n->key.c_str(), key.c_str(), text.c_str());
            if (k >= 0 && k < 32 ? dx5::LinkMacro(s->graph, n->id, key, isInt, k) : dx5::SetConstText(s->graph, n->id, key, text, &why)) {
                g_win.status.clear();
                Commit("set value");
            } else {
                s->graph = before;
                Refused(why.empty() ? "cannot set " + key : why);
            }
        }
    }
    Redraw();
}

// ---- menus ----
enum MenuId { M_ADD = 1000, M_SEARCH = 100, M_EXPOSE = 200, M_RESET, M_UNLINK, M_EXPAND, M_LINK = 300, M_LINKI = 400, M_CHOICE = 500, M_UNWIRE = 600 };

std::string MenuLabel(const dx5::NodeDef& d)
{
    return d.category == "Math" && d.name.rfind("math_", 0) == 0 ? d.name.substr(5) : d.label;
}
std::string Lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Adds a node of defs[index] where the menu was opened (client point).
void AddAt(POINT client, size_t index)
{
    const auto& defs = dx5::AllDefs();
    if (index >= defs.size()) return;
    const double x = (client.x - g_win.panX) / g_win.zoom, y = (client.y - kTabH - g_win.panY) / g_win.zoom;
    const uint32_t nid = dx5::AddNodeAt(g_win.s->graph, defs[index].name, x, y);
    if (nid) {
        g_win.selected = nid;
        Commit("add node");
    }
}

int Track(HMENU m, POINT client)
{
    POINT sp = client;
    ClientToScreen(g_win.wnd, &sp);
    return TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, g_win.wnd, nullptr);
}

void AddMenu(POINT client)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, M_SEARCH, L"Search...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::map<std::string, HMENU> sub;
    const char* order[] = {"Generate", "Set", "Geometry", "Input", "Math", "Utility", "Group", "Zone", "Output"};
    for (const char* cat : order) sub[cat] = CreatePopupMenu();
    const auto& defs = dx5::AllDefs();
    for (size_t i = 0; i < defs.size(); i++) {
        const dx5::NodeDef& d = defs[i];
        auto it = sub.find(d.category);
        if (it == sub.end() || d.name == "each") continue;
        const bool off = d.name == "output" && g_win.s->graph.OutputNode();
        AppendMenuW(it->second, MF_STRING | (off ? MF_GRAYED : 0), M_ADD + (UINT)i, W(MenuLabel(d)).c_str());
    }
    for (const char* cat : order) {
        if (GetMenuItemCount(sub[cat]) > 0) AppendMenuW(m, MF_POPUP, (UINT_PTR)sub[cat], W(cat).c_str());
        else DestroyMenu(sub[cat]);
    }
    const int id = Track(m, client);
    DestroyMenu(m);
    if (id == M_SEARCH) {
        // a box where the menu was: type part of a name, Enter shows the nodes that match
        g_win.editSearch = true, g_win.editMacro = -1, g_win.editExprText = false, g_win.editInput = -1;
        g_win.searchAt = client;
        BeginEdit({client.x, client.y, client.x + 180, client.y + 22}, "");
        return;
    }
    if (id >= M_ADD && id < M_ADD + (int)defs.size()) AddAt(client, (size_t)(id - M_ADD));
    Redraw();
}

// the nodes whose name or label holds `text` (one match is added at once)
void SearchMenu(POINT client, const std::string& text)
{
    const std::string q = Lower(text);
    const auto& defs = dx5::AllDefs();
    std::vector<size_t> hits;
    for (size_t i = 0; i < defs.size(); i++) {
        const dx5::NodeDef& d = defs[i];
        if (d.name == "each" || (d.name == "output" && g_win.s->graph.OutputNode())) continue;
        if (q.empty() || Lower(d.name).find(q) != std::string::npos || Lower(d.label).find(q) != std::string::npos) hits.push_back(i);
    }
    if (hits.empty()) return Refused("no node matches '" + text + "'");
    if (hits.size() == 1) return AddAt(client, hits[0]);
    HMENU m = CreatePopupMenu();
    for (size_t i : hits) AppendMenuW(m, MF_STRING, M_ADD + (UINT)i, W(MenuLabel(defs[i]) + "   (" + defs[i].category + ")").c_str());
    const int id = Track(m, client);
    DestroyMenu(m);
    if (id >= M_ADD && id < M_ADD + (int)defs.size()) AddAt(client, (size_t)(id - M_ADD));
}

void ValueMenu(POINT client, uint32_t node, int index)
{
    Session* s = g_win.s;
    dx5::Node* n = s->graph.Find(node);
    if (!n || index < 0 || index >= (int)n->inputs.size()) return;
    const dx5::InputSlot slot = n->inputs[index];
    const dx5::InputDef* d = InDef(*n, slot.name);
    if (d && d->type == dx5::Type::Geometry) return;
    HMENU m = CreatePopupMenu();
    if (d && !d->choices.empty()) {
        const std::string& c = d->choices;
        size_t a = 0;
        for (int i = 0; a != std::string::npos; i++) {
            const size_t b = c.find('|', a);
            const std::string item = c.substr(a, b == std::string::npos ? std::string::npos : b - a);
            AppendMenuW(m, MF_STRING | (dx5::AsInt(slot.value) == i ? MF_CHECKED : 0), M_CHOICE + i, W(item).c_str());
            a = b == std::string::npos ? b : b + 1;
        }
    } else {
        const bool isInt = d && (d->type == dx5::Type::Int || d->structural);
        AppendMenuW(m, MF_STRING | (dx5::FreeMacro(s->graph, isInt) < 0 || slot.link.from ? MF_GRAYED : 0), M_EXPOSE,
                    isInt ? L"Expose to Int Macro" : L"Expose to Macro");
        HMENU link = CreatePopupMenu(), linkM = CreatePopupMenu(), linkI = CreatePopupMenu();
        for (int k = 0; k < 32; k++) {
            AppendMenuW(linkM, MF_STRING, M_LINK + k, W("M" + std::to_string(k + 1) + "  " + dx5::MacroLabel(s->graph, false, k)).c_str());
            AppendMenuW(linkI, MF_STRING, M_LINKI + k, W("I" + std::to_string(k + 1) + "  " + dx5::MacroLabel(s->graph, true, k)).c_str());
        }
        AppendMenuW(link, MF_POPUP, (UINT_PTR)linkM, L"M  Macro");
        AppendMenuW(link, MF_POPUP, (UINT_PTR)linkI, L"I  Int Macro");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)link, L"Link to Macro");
        if (slot.link.from) AppendMenuW(m, MF_STRING, M_UNLINK, L"Unlink");
        const dx5::Node* f = slot.link.from ? s->graph.Find(slot.link.from) : nullptr;
        if (f && f->type == "expr") AppendMenuW(m, MF_STRING, M_EXPAND, L"Expand Formula into Nodes");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, M_RESET, L"Reset");
    }
    POINT sp = client;
    ClientToScreen(g_win.wnd, &sp);
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, g_win.wnd, nullptr);
    DestroyMenu(m);
    if (id == M_EXPOSE) {
        dx5::MacroWrite w;
        if (dx5::Expose(s->graph, node, slot.name, &w)) Commit("expose", {w});
    } else if (id == M_RESET) {
        if (d) {
            dx5::Node* q = s->graph.Find(node);
            dx5::InputSlot* qs = q ? q->In(slot.name) : nullptr;
            if (qs) qs->link = {}, qs->value = d->dflt;
            Commit("reset value");
        }
    } else if (id == M_UNLINK) {
        if (dx5::Disconnect(s->graph, node, slot.name)) Commit("unlink");
    } else if (id == M_EXPAND) {
        if (dx5::ExpandExpression(s->graph, slot.link.from)) Commit("expand formula");
    } else if (id >= M_CHOICE && id < M_CHOICE + 64) {
        if (dx5::SetConst(s->graph, node, slot.name, id - M_CHOICE)) Commit("set choice");
    } else if (id >= M_LINK && id < M_LINK + 32) {
        if (dx5::LinkMacro(s->graph, node, slot.name, false, id - M_LINK)) Commit("link macro");
    } else if (id >= M_LINKI && id < M_LINKI + 32) {
        if (dx5::LinkMacro(s->graph, node, slot.name, true, id - M_LINKI)) Commit("link int macro");
    }
    Redraw();
}

// a number that a drag can change (unwired, not a choice, a Float or Int)
bool Scrubbable(const dx5::Node& n, int input)
{
    const dx5::InputSlot& s = n.inputs[input];
    const dx5::InputDef* d = InDef(n, s.name);
    if (s.link.from || (d && !d->choices.empty())) return false;
    return s.value.type == dx5::Type::Float || s.value.type == dx5::Type::Int;
}

// ---- the view: zoom, fit, a group's code ----
// The zoom set to z (kept in range), the world point under `about` staying under it.
void ZoomAt(POINT about, double z)
{
    z = std::clamp(z, kZoomMin, kZoomMax);
    if (z == g_win.zoom) return;
    if (g_win.edit) SetFocus(g_win.wnd);  // a value's box would stay where the value was
    const double wx = (about.x - g_win.panX) / g_win.zoom, wy = (about.y - kTabH - g_win.panY) / g_win.zoom;
    g_win.zoom = z;
    g_win.panX = about.x - wx * z, g_win.panY = about.y - kTabH - wy * z;
    Log("editor: zoom %.3f", z);
    Redraw();
}

// Home: every node in view (not larger than 1:1)
void FitAll()
{
    Session* s = g_win.s;
    if (!s || s->graph.nodes.empty()) return;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const dx5::Node& n : s->graph.nodes) {
        x0 = std::min(x0, n.x), y0 = std::min(y0, n.y);
        x1 = std::max(x1, n.x + kNodeW), y1 = std::max(y1, n.y + WorldH(n));
    }
    const RECT c = CanvasRect();
    const double cw = std::max(1L, c.right - c.left - 60), ch = std::max(1L, c.bottom - c.top - 60);
    const double z = std::clamp(std::min({cw / std::max(1.0, x1 - x0), ch / std::max(1.0, y1 - y0), 1.0}), kZoomMin, kZoomMax);
    if (g_win.edit) SetFocus(g_win.wnd);
    g_win.zoom = z;
    g_win.panX = c.left + (c.right - c.left - (x1 - x0) * z) / 2 - x0 * z;
    g_win.panY = (c.bottom - c.top - (y1 - y0) * z) / 2 - y0 * z;
    Log("editor: zoom %.3f", z);
    Redraw();
}

// A group's ▸: the Code view, with the line that makes the node selected (opening a group in place comes later)
void OpenGroupCode(uint32_t node)
{
    const dx5::Node* n = g_win.s ? g_win.s->graph.Find(node) : nullptr;
    if (!n) return;
    const std::string key = n->key;
    SetView(View::Code);
    const std::string code = dx5::GraphToCode(g_win.s->graph);
    int line = 1;
    for (size_t a = 0; a < code.size(); line++) {
        size_t b = code.find('\n', a);
        if (b == std::string::npos) b = code.size();
        size_t t = code.find_first_not_of(" \t", a);
        if (t != std::string::npos && t < b && code.compare(t, key.size(), key) == 0) {
            const size_t e = code.find_first_not_of(' ', t + key.size());
            if (e < b && code[e] == '=') return SelectLine(line);
        }
        a = b + 1;
    }
}

// ---- the window ----
LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Session* s = g_win.s;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, r.right), std::max(1L, r.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, r);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        LayoutCode();
        Redraw();
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == kApplyId && HIWORD(w) == BN_CLICKED) ApplyCodeNow();
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_win.codeMsg) {
            static HBRUSH bg = CreateSolidBrush(kBg);
            SetTextColor((HDC)w, g_win.codeError ? RGB(250, 110, 90) : kDim);
            SetBkColor((HDC)w, kBg);
            return (LRESULT)bg;
        }
        break;
    case WM_LBUTTONDOWN: {
        if (!s) return 0;
        const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (p.y < kTabH) {
            for (int i = 0; i < 3; i++)
                if (In(TabRect((View)i), p) && g_win.view != (View)i) SetView((View)i);
            return 0;
        }
        if (g_win.view != View::Nodes) return 0;
        SetFocus(h);
        const HitInfo hi = HitTest(p);
        g_win.start = g_win.now = p;
        g_win.moved = false;
        g_win.dragNode = hi.node;
        g_win.dragIndex = hi.index;
        g_win.lifted = false;
        g_win.snapNode = 0, g_win.snapInput = -1;
        if (hi.what == Hit::Title) {
            const dx5::Node* n = s->graph.Find(hi.node);
            g_win.selected = hi.node;
            g_win.drag = Drag::Move;
            g_win.startX = n->x, g_win.startY = n->y;
        } else if (hi.what == Hit::GroupOpen) {
            g_win.selected = hi.node;
            OpenGroupCode(hi.node);
            return 0;
        } else if (hi.what == Hit::OutPort) {
            g_win.drag = Drag::Wire;
        } else if (hi.what == Hit::InPort) {
            // a wired input: its wire is lifted off and dragged from where it comes (Blender)
            const dx5::Node* n = s->graph.Find(hi.node);
            if (n && n->inputs[hi.index].link.from) {
                g_win.lifted = true;
                g_win.liftBefore = s->graph;
                g_win.liftNode = hi.node;
                g_win.liftInput = n->inputs[hi.index].name;
                g_win.liftLink = n->inputs[hi.index].link;
                g_win.dragNode = g_win.liftLink.from;
                g_win.dragIndex = g_win.liftLink.output;
                dx5::Disconnect(s->graph, hi.node, g_win.liftInput);
                g_win.drag = Drag::Wire;
            }
        } else if (hi.what == Hit::Macro) {
            g_win.drag = Drag::MacroLink;
            g_win.macroDragInt = hi.isInt, g_win.macroDragK = hi.index;
        } else if (hi.what == Hit::Value) {
            const dx5::Node* n = s->graph.Find(hi.node);
            g_win.selected = hi.node;
            g_win.drag = Drag::Scrub;
            g_win.startValue = dx5::AsFloat(n->inputs[hi.index].value);
        } else if (hi.what == Hit::ExprText) {
            const dx5::Node* n = s->graph.Find(hi.node);
            g_win.selected = hi.node;
            g_win.editNode = hi.node, g_win.editExprText = true, g_win.editMacro = -1, g_win.editInput = -1, g_win.editSearch = false;
            ZoomFonts();
            BeginEdit(TextRect(*n), n->text, g_win.zFont);
        } else if (hi.what == Hit::Canvas) {
            g_win.selected = 0;
        }
        if (g_win.drag != Drag::None) SetCapture(h);
        Redraw();
        return 0;
    }
    case WM_MBUTTONDOWN:
        g_win.drag = Drag::Pan;
        g_win.start = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        g_win.startX = g_win.panX, g_win.startY = g_win.panY;
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE: {
        if (!s || g_win.drag == Drag::None) return 0;
        const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        g_win.now = p;
        const int dx = p.x - g_win.start.x, dy = p.y - g_win.start.y;
        if (std::abs(dx) > 3 || std::abs(dy) > 3) g_win.moved = true;
        if (g_win.drag == Drag::Move) {
            dx5::MoveNode(s->graph, g_win.dragNode, g_win.startX + dx / g_win.zoom, g_win.startY + dy / g_win.zoom);
        } else if (g_win.drag == Drag::Wire) {
            g_win.snapNode = 0, g_win.snapInput = -1;
            FindSnap(p, &g_win.snapNode, &g_win.snapInput);
        } else if (g_win.drag == Drag::Pan) {
            g_win.panX = g_win.startX + dx, g_win.panY = g_win.startY + dy;
        } else if (g_win.drag == Drag::Scrub && g_win.moved) {
            // only the window changes while dragging; the release writes once (design v4 7)
            dx5::Node* n = s->graph.Find(g_win.dragNode);
            if (n && g_win.dragIndex < (int)n->inputs.size() && Scrubbable(*n, g_win.dragIndex)) {
                const dx5::InputDef* d = InDef(*n, n->inputs[g_win.dragIndex].name);
                double step = d ? (d->softMax - d->softMin) / 300.0 : 0.5;
                if (GetKeyState(VK_SHIFT) < 0) step *= 10;
                if (GetKeyState(VK_CONTROL) < 0) step *= 0.1;
                dx5::SetConst(s->graph, n->id, n->inputs[g_win.dragIndex].name, g_win.startValue + dx * step);
            }
        }
        Redraw();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!s) return 0;
        // the drag is taken and ended before ReleaseCapture: it sends WM_CAPTURECHANGED at once, which would read
        // a drag still going as one cut short and end it (11/28: no wire ever connected, a click opened no edit box)
        const Drag d = g_win.drag;
        g_win.drag = Drag::None;
        ReleaseCapture();
        const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (d == Drag::Move && g_win.moved) {
            Commit("move node");
        } else if (d == Drag::Wire) {
            // where it was let go: the input it snaps to, else an input under the cursor (refused with why)
            uint32_t toNode = 0;
            int toInput = -1;
            if (!FindSnap(p, &toNode, &toInput)) {
                const HitInfo hi = HitTest(p);
                if (hi.what == Hit::InPort) toNode = hi.node, toInput = hi.index;
            }
            g_win.snapNode = 0, g_win.snapInput = -1;
            const dx5::Node* to = toNode ? s->graph.Find(toNode) : nullptr;
            std::string why;
            bool wired = false;
            if (to && toInput >= 0) {
                const std::string name = to->inputs[toInput].name;
                wired = dx5::Connect(s->graph, g_win.dragNode, g_win.dragIndex, toNode, name, &why);
                if (wired && g_win.lifted && toNode == g_win.liftNode && name == g_win.liftInput) {
                    s->graph = g_win.liftBefore;  // put back where it was: nothing changed
                    g_win.lifted = false;
                    Redraw();
                    return 0;
                }
                if (wired) {
                    g_win.status.clear();
                    Commit("wire");  // with a lifted wire: its move, one step
                } else {
                    Refused(why);
                }
            }
            if (!wired && g_win.lifted) Commit("unwire");  // a lifted wire let go elsewhere comes off
            g_win.lifted = false;
        } else if (d == Drag::MacroLink) {
            const HitInfo hi = HitTest(p);
            const dx5::Node* n = hi.what == Hit::Value ? s->graph.Find(hi.node) : nullptr;
            if (g_win.moved && n && hi.index < (int)n->inputs.size()) {
                if (dx5::LinkMacro(s->graph, n->id, n->inputs[hi.index].name, g_win.macroDragInt, g_win.macroDragK)) {
                    g_win.status.clear();
                    Commit("link macro");
                } else {
                    Refused("a macro cannot feed " + n->inputs[hi.index].name);
                }
            }
            g_win.macroDragK = -1;
        } else if (d == Drag::Scrub) {
            dx5::Node* n = s->graph.Find(g_win.dragNode);
            if (n && g_win.dragIndex < (int)n->inputs.size()) {
                if (g_win.moved) {
                    if (Scrubbable(*n, g_win.dragIndex) && dx5::AsFloat(n->inputs[g_win.dragIndex].value) != g_win.startValue)
                        Commit("drag value");
                } else {
                    const dx5::InputDef* id = InDef(*n, n->inputs[g_win.dragIndex].name);
                    if (id && !id->choices.empty()) ValueMenu(p, n->id, g_win.dragIndex);
                    else BeginValueEdit(*n, g_win.dragIndex);
                }
            }
        }
        Redraw();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!s || g_win.view != View::Nodes || g_win.drag != Drag::None) return 0;
        POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};  // screen coordinates
        ScreenToClient(h, &p);
        if (!In(CanvasRect(), p)) return 0;
        ZoomAt(p, g_win.zoom * std::pow(1.1, GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA));
        return 0;
    }
    case WM_MBUTTONUP:
        g_win.drag = Drag::None;  // first (see WM_LBUTTONUP)
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (g_win.drag != Drag::None && (HWND)l != h) {
            // a drag cut short (another window took the mouse): keep what it did as one step
            if ((g_win.drag == Drag::Move || g_win.drag == Drag::Scrub) && g_win.moved) Commit("drag");
            if (g_win.drag == Drag::Wire && g_win.lifted && s) s->graph = g_win.liftBefore;  // the lifted wire goes back
            g_win.lifted = false, g_win.snapNode = 0, g_win.snapInput = -1, g_win.macroDragK = -1;
            g_win.drag = Drag::None;
            Redraw();
        }
        return 0;
    case WM_RBUTTONUP: {
        if (!s || g_win.view != View::Nodes) return 0;
        const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const HitInfo hi = HitTest(p);
        if (hi.what == Hit::Canvas) AddMenu(p);
        else if (hi.what == Hit::Value) ValueMenu(p, hi.node, hi.index);
        else if (hi.what == Hit::InPort) {
            const dx5::Node* n = s->graph.Find(hi.node);
            if (n && n->inputs[hi.index].link.from) {
                HMENU mm = CreatePopupMenu();
                AppendMenuW(mm, MF_STRING, M_UNWIRE, L"Unwire");
                POINT sp = p;
                ClientToScreen(h, &sp);
                const int id = TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, h, nullptr);
                DestroyMenu(mm);
                if (id == M_UNWIRE && dx5::Disconnect(s->graph, hi.node, n->inputs[hi.index].name)) Commit("unwire");
                Redraw();
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        if (!s) return 0;
        const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const HitInfo hi = HitTest(p);
        if (hi.what == Hit::Macro) {
            for (size_t row = 0; row < g_win.macroRows.size(); row++)
                if (g_win.macroRows[row].k == hi.index && g_win.macroRows[row].isInt == hi.isInt) {
                    RECT r = MacroRow((int)row);
                    r.left += 30, r.right -= 80;
                    g_win.editMacro = hi.index, g_win.editMacroInt = hi.isInt, g_win.editExprText = false, g_win.editInput = -1,
                    g_win.editSearch = false;
                    BeginEdit(r, (hi.isInt ? s->graph.intMacros : s->graph.macros)[hi.index].name);
                }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (!s) return 0;
        if (w == VK_DELETE && g_win.selected && g_win.view == View::Nodes) {
            if (dx5::RemoveNode(s->graph, g_win.selected)) Commit("remove node");
            g_win.selected = 0;
            Redraw();
        } else if (w == VK_HOME && g_win.view == View::Nodes) {
            FitAll();
        } else if (w == 'Z' && GetKeyState(VK_CONTROL) < 0) {
            RequestUndo(GetKeyState(VK_SHIFT) < 0);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: {
        Session* gone = g_win.s;
        if (g_win.edit) g_win.edit = nullptr;
        if (g_win.zFont) DeleteObject(g_win.zFont);
        if (g_win.zBold) DeleteObject(g_win.zBold);
        g_win.zFont = g_win.zBold = nullptr, g_win.fontZoom = 0;
        g_win.s = nullptr;
        g_win.wnd = nullptr;
        WindowGone(gone);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

HWND CreateEditorWindow(Session* s)
{
    static bool registered = false;
    HINSTANCE inst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&CreateEditorWindow, &inst);
    if (!registered) {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"DupliX2Editor";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        registered = RegisterClassW(&wc) != 0;
    }
    static HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    static HFONT bold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    static HFONT mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    // owned by AE's main window: it stays above it and goes with it
    void* owner = nullptr;
    Suite<AEGP_UtilitySuite6> util(g_pica, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (util) util->AEGP_GetMainHWND(&owner);
    g_win = Win{};
    g_win.s = s;
    g_win.font = font;
    g_win.bold = bold;
    g_win.mono = mono;
    HWND wnd = CreateWindowExW(0, L"DupliX2Editor", L"DupliX 2 Editor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               960, 600, (HWND)owner, nullptr, inst, nullptr);
    g_win.wnd = wnd;
    if (wnd) ShowWindow(wnd, SW_SHOWNORMAL);
    return wnd;
}

void GraphReloaded(HWND wnd)
{
    if (wnd != g_win.wnd) return;
    if (g_win.selected && g_win.s && !g_win.s->graph.Find(g_win.selected)) g_win.selected = 0;
    if (g_win.view == View::Code && g_win.code) {
        // the code as the user left it stays; untouched code follows the graph
        if (SendMessageW(g_win.code, EM_GETMODIFY, 0, 0)) CodeMessage("The graph changed in After Effects: Ctrl+R takes it (your edit is lost), Apply replaces it.", true);
        else FillCode();
    }
    Redraw();
}

}  // namespace ed
}  // namespace dx2
