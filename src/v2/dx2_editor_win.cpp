// DupliX 2 (design v4, 1.2): the graph editor's window (Win32, GDI, drawn by itself). Every change goes through
// graph/edit.h and ends in ed::Commit (one AE undo step); while a value is dragged only the window changes, and the
// release writes once (design v4 7).
//
//   right-click empty space      add a node
//   drag a node's title          move it            Delete: remove the selected node
//   drag from an output (right)  to an input (left) to wire; right-click an input to unwire
//   click a value                type a number (a choice: pick from a menu); drag it left / right to change it
//   right-click a value          Expose to a macro, link to / unlink a macro, reset
//   double-click a macro (right) rename it
//   Ctrl+Z / Ctrl+Shift+Z        AE's Undo / Redo
//   middle-drag                  pan
#include "v2/dx2_editor.h"

#include <windowsx.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dx2 {
namespace ed {
namespace {

constexpr int kNodeW = 190, kTitleH = 22, kRowH = 19, kPort = 5, kPanelW = 230;
constexpr COLORREF kBg = RGB(30, 30, 32), kGridC = RGB(40, 40, 43), kNodeC = RGB(52, 52, 56), kNodeSel = RGB(70, 70, 78),
                   kTitleC = RGB(64, 92, 140), kTitleOut = RGB(120, 84, 50), kText = RGB(222, 222, 222),
                   kDim = RGB(150, 150, 150), kWire = RGB(170, 170, 120), kLinkC = RGB(110, 190, 240),
                   kPanelC = RGB(38, 38, 41), kWarn = RGB(240, 160, 80);

enum class Hit { None, Canvas, Title, OutPort, InPort, Value, Macro, Panel };
struct HitInfo {
    Hit what = Hit::None;
    uint32_t node = 0;
    int index = 0;  // the port, the param, or the macro
};

enum class Drag { None, Move, Wire, Scrub, Pan };

struct Win {
    Session* s = nullptr;
    HWND wnd = nullptr, edit = nullptr;
    HFONT font = nullptr, bold = nullptr;
    double panX = 30, panY = 30;
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
    int editParam = -1, editMacro = -1;
    WNDPROC editProc = nullptr;
    std::vector<int> macroRows;  // the macros listed in the panel, top to bottom
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

// ---- layout ----
RECT NodeRect(const dxg::Node& n)
{
    const int rows = (int)n.params.size();
    const int x = (int)std::lround(n.x + g_win.panX), y = (int)std::lround(n.y + g_win.panY);
    return {x, y, x + kNodeW, y + kTitleH + rows * kRowH + 4};
}
POINT InPort(const dxg::Node& n, int port)
{
    const RECT r = NodeRect(n);
    return {r.left, r.top + kTitleH / 2 + port * kRowH};
}
POINT OutPort(const dxg::Node& n)
{
    const RECT r = NodeRect(n);
    return {r.right, r.top + kTitleH / 2};
}
RECT ValueRect(const dxg::Node& n, int i)
{
    const RECT r = NodeRect(n);
    const int y = r.top + kTitleH + i * kRowH + 2;
    return {r.left + kNodeW / 2 - 4, y, r.right - 6, y + kRowH - 2};
}
RECT CanvasRect()
{
    RECT c;
    GetClientRect(g_win.wnd, &c);
    c.right = std::max(c.left, c.right - kPanelW);
    return c;
}
RECT MacroRow(int row)
{
    const RECT c = CanvasRect();
    RECT r;
    GetClientRect(g_win.wnd, &r);
    const int y = 34 + row * 22;
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
            if (In(MacroRow((int)row), p)) return {Hit::Macro, 0, g_win.macroRows[row]};
        return {Hit::Panel, 0, 0};
    }
    // the top node first (drawn last)
    for (auto it = s->graph.nodes.rbegin(); it != s->graph.nodes.rend(); ++it) {
        const dxg::Node& n = *it;
        const dxg::NodeSpec* spec = dxg::Spec(n.type);
        if (spec && spec->hasOutput && Near(OutPort(n), p, kPort + 3)) return {Hit::OutPort, n.id, 0};
        for (int k = 0; spec && k < spec->numInputs; k++)
            if (Near(InPort(n, k), p, kPort + 3)) return {Hit::InPort, n.id, k};
        const RECT r = NodeRect(n);
        if (!In(r, p)) continue;
        if (p.y < r.top + kTitleH) return {Hit::Title, n.id, 0};
        for (int i = 0; i < (int)n.params.size(); i++)
            if (In(ValueRect(n, i), p)) return {Hit::Value, n.id, i};
        return {Hit::Title, n.id, 0};  // the rest of the box moves it too
    }
    return {Hit::Canvas, 0, 0};
}

// ---- what a value shows ----
std::string ValueText(const dxg::Graph& g, const dxg::Node& n, const dxg::Param& p)
{
    const dxg::ParamSpec* ps = dxg::FindParam(dxg::Spec(n.type), p.key);
    if (ps && ps->choices) {
        std::string c = ps->choices;
        int idx = (int)std::lround(p.in.base), i = 0;
        size_t a = 0;
        while (i < idx && a != std::string::npos) {
            a = c.find('|', a);
            if (a != std::string::npos) a++;
            i++;
        }
        if (a == std::string::npos) return Num(p.in.base, true);
        const size_t b = c.find('|', a);
        return c.substr(a, b == std::string::npos ? std::string::npos : b - a);
    }
    const bool integer = ps && ps->structural;
    std::string s;
    const bool plain = p.in.links.size() == 1 && p.in.base == 0 && p.in.links[0].depth == 1;
    if (!plain || p.in.links.empty()) s = Num(p.in.base, integer);
    for (const dxg::Link& l : p.in.links) {
        const int k = dxg::MacroIndex(l.src);
        const std::string name = k >= 0 ? dxg::MacroLabel(g, k) : l.src;
        if (plain) return name;
        s += l.depth < 0 ? " - " : " + ";
        if (std::fabs(l.depth) != 1) s += Num(std::fabs(l.depth), false) + "x";
        s += name;
    }
    return s;
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

void Wire(HDC dc, POINT a, POINT b, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 2, c);
    HGDIOBJ old = SelectObject(dc, pen);
    const int dx = std::max(40, (int)std::abs(b.x - a.x) / 2);
    POINT pts[4] = {a, {a.x + dx, a.y}, {b.x - dx, b.y}, b};
    PolyBezier(dc, pts, 4);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void Port(HDC dc, POINT p, bool filled)
{
    HBRUSH b = CreateSolidBrush(filled ? kWire : kNodeC);
    HPEN pen = CreatePen(PS_SOLID, 1, kWire);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    Ellipse(dc, p.x - kPort, p.y - kPort, p.x + kPort + 1, p.y + kPort + 1);
    SelectObject(dc, ob), SelectObject(dc, op);
    DeleteObject(b), DeleteObject(pen);
}

void Paint(HDC dc, const RECT& client)
{
    Session* s = g_win.s;
    FillRect(dc, client, kBg);
    const RECT c = CanvasRect();
    // a grid that moves with the view
    HPEN gp = CreatePen(PS_SOLID, 1, kGridC);
    HGDIOBJ old = SelectObject(dc, gp);
    const int step = 40;
    for (int x = (int)std::fmod(g_win.panX, step); x < c.right; x += step) MoveToEx(dc, x, 0, nullptr), LineTo(dc, x, c.bottom);
    for (int y = (int)std::fmod(g_win.panY, step); y < c.bottom; y += step) MoveToEx(dc, 0, y, nullptr), LineTo(dc, c.right, y);
    SelectObject(dc, old);
    DeleteObject(gp);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_win.font);
    if (!s) return;
    const dxg::Graph& g = s->graph;
    for (const dxg::Wire& w : g.wires) {
        const dxg::Node* a = g.FindNode(w.from);
        const dxg::Node* b = g.FindNode(w.to);
        if (a && b) Wire(dc, OutPort(*a), InPort(*b, w.port), kWire);
    }
    if (g_win.drag == Drag::Wire) {
        const dxg::Node* a = g.FindNode(g_win.dragNode);
        if (a) Wire(dc, OutPort(*a), g_win.now, kLinkC);
    }
    for (const dxg::Node& n : g.nodes) {
        const dxg::NodeSpec* spec = dxg::Spec(n.type);
        const RECT r = NodeRect(n);
        FillRect(dc, r, n.id == g_win.selected ? kNodeSel : kNodeC);
        RECT t = {r.left, r.top, r.right, r.top + kTitleH};
        FillRect(dc, t, n.type == dxg::NodeType::Output ? kTitleOut : kTitleC);
        SelectObject(dc, g_win.bold);
        t.left += 10;
        Text(dc, spec ? spec->name : n.typeName + " (unknown)", t, kText, DT_LEFT);
        SelectObject(dc, g_win.font);
        for (int i = 0; i < (int)n.params.size(); i++) {
            const dxg::Param& p = n.params[i];
            const dxg::ParamSpec* ps = dxg::FindParam(spec, p.key);
            RECT lr = {r.left + 10, r.top + kTitleH + i * kRowH + 2, r.left + kNodeW / 2 - 8, 0};
            lr.bottom = lr.top + kRowH - 2;
            Text(dc, ps ? ps->label : p.key, lr, kDim, DT_LEFT);
            RECT vr = ValueRect(n, i);
            FillRect(dc, vr, RGB(40, 40, 44));
            vr.left += 5, vr.right -= 5;
            Text(dc, ValueText(g, n, p), vr, p.in.links.empty() ? kText : kLinkC, DT_RIGHT);
        }
        for (int k = 0; spec && k < spec->numInputs; k++) Port(dc, InPort(n, k), g.WireInto(n.id, k) != nullptr);
        if (spec && spec->hasOutput) Port(dc, OutPort(n), false);
    }
    if (!g.OutputNode()) {
        RECT m = {c.left + 10, c.bottom - 28, c.right - 10, c.bottom - 8};
        Text(dc, "No Output node: right-click to add one", m, kWarn, DT_LEFT);
    }

    // the macros panel
    RECT p = {c.right, 0, client.right, client.bottom};
    FillRect(dc, p, kPanelC);
    SelectObject(dc, g_win.bold);
    RECT h = {c.right + 10, 6, client.right - 8, 28};
    Text(dc, "Macros", h, kText, DT_LEFT);
    SelectObject(dc, g_win.font);
    g_win.macroRows.clear();
    const uint32_t used = dxg::UsedMacros(g);
    for (int k = 0; k < kMacros; k++)
        if ((used >> k & 1) || !g.macros[k].name.empty()) g_win.macroRows.push_back(k);
    for (size_t row = 0; row < g_win.macroRows.size(); row++) {
        const int k = g_win.macroRows[row];
        RECT r = MacroRow((int)row);
        FillRect(dc, r, RGB(46, 46, 50));
        RECT a = {r.left + 6, r.top, r.left + 34, r.bottom};
        Text(dc, std::to_string(k + 1), a, kDim, DT_LEFT);
        RECT b = {r.left + 34, r.top, r.right - 80, r.bottom};
        Text(dc, dxg::MacroLabel(g, k), b, (used >> k & 1) ? kLinkC : kDim, DT_LEFT);
        RECT rr = {r.right - 80, r.top, r.right - 6, r.bottom};
        Text(dc, Num(g.macros[k].sliderMin, false) + ".." + Num(g.macros[k].sliderMax, false), rr, kDim, DT_RIGHT);
    }
    RECT help = {c.right + 10, client.bottom - 150, client.right - 8, client.bottom - 8};
    SetTextColor(dc, kDim);
    const wchar_t* hint =
        L"Right-click: add a node, or a value's menu (Expose to Macro).\n"
        L"Drag a value left / right, or click it to type.\n"
        L"Drag from an output to an input to wire.\n"
        L"Delete: remove the node.  Ctrl+Z: undo.\n"
        L"Double-click a macro to rename it.\n"
        L"Keys and expressions go on Macro 1-32 in the Effect Controls.";
    DrawTextW(dc, hint, -1, &help, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
}

void Redraw() { InvalidateRect(g_win.wnd, nullptr, FALSE); }

// ---- the edit box ----
void EndEdit(bool commit);

LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
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

void BeginEdit(const RECT& r, const std::string& text, uint32_t node, int param, int macro)
{
    if (g_win.edit) EndEdit(false);
    g_win.editNode = node, g_win.editParam = param, g_win.editMacro = macro;
    g_win.edit = CreateWindowExW(0, L"EDIT", W(text).c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 r.left, r.top, r.right - r.left, r.bottom - r.top, g_win.wnd, nullptr, nullptr, nullptr);
    SendMessageW(g_win.edit, WM_SETFONT, (WPARAM)g_win.font, TRUE);
    g_win.editProc = (WNDPROC)SetWindowLongPtrW(g_win.edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    SendMessageW(g_win.edit, EM_SETSEL, 0, -1);
    SetFocus(g_win.edit);
}

void EndEdit(bool commit)
{
    HWND e = g_win.edit;
    if (!e) return;
    g_win.edit = nullptr;  // first: DestroyWindow sends WM_KILLFOCUS again
    wchar_t buf[256] = {0};
    GetWindowTextW(e, buf, 255);
    DestroyWindow(e);
    SetFocus(g_win.wnd);
    Session* s = g_win.s;
    if (!commit || !s) return Redraw();
    const std::string text = U8(buf);
    if (g_win.editMacro >= 0) {
        if (dxg::RenameMacro(s->graph, g_win.editMacro, text)) Commit("rename macro");
    } else {
        dxg::Node* n = s->graph.FindNode(g_win.editNode);
        double v = 0;
        auto r = std::from_chars(text.data(), text.data() + text.size(), v);
        if (n && g_win.editParam >= 0 && g_win.editParam < (int)n->params.size() && r.ec == std::errc() &&
            v != n->params[g_win.editParam].in.base && dxg::SetValue(s->graph, n->id, n->params[g_win.editParam].key, v))
            Commit("set value");
    }
    Redraw();
}

// ---- menus ----
enum MenuId { M_ADD = 100, M_EXPOSE = 200, M_RESET, M_UNLINK = 300, M_LINK = 400, M_CHOICE = 500, M_UNWIRE = 600 };

void AddMenu(POINT client)
{
    HMENU m = CreatePopupMenu();
    for (int t = 0; t < (int)dxg::NodeType::Count_; t++) {
        const dxg::NodeSpec* s = dxg::Spec((dxg::NodeType)t);
        if (!s) continue;
        const bool off = s->type == dxg::NodeType::Output && g_win.s->graph.OutputNode();
        AppendMenuW(m, MF_STRING | (off ? MF_GRAYED : 0), M_ADD + t, (L"Add " + W(s->name)).c_str());
    }
    POINT sp = client;
    ClientToScreen(g_win.wnd, &sp);
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, g_win.wnd, nullptr);
    DestroyMenu(m);
    if (id >= M_ADD && id < M_ADD + (int)dxg::NodeType::Count_) {
        const uint32_t nid = dxg::AddNode(g_win.s->graph, (dxg::NodeType)(id - M_ADD), client.x - g_win.panX, client.y - g_win.panY);
        if (nid) {
            g_win.selected = nid;
            Commit("add node");
        }
    }
    Redraw();
}

void ValueMenu(POINT client, uint32_t node, int index)
{
    Session* s = g_win.s;
    dxg::Node* n = s->graph.FindNode(node);
    if (!n || index < 0 || index >= (int)n->params.size()) return;
    const dxg::Param p = n->params[index];
    const dxg::ParamSpec* ps = dxg::FindParam(dxg::Spec(n->type), p.key);
    HMENU m = CreatePopupMenu();
    if (ps && ps->choices) {
        std::string c = ps->choices;
        size_t a = 0;
        for (int i = 0; a != std::string::npos; i++) {
            const size_t b = c.find('|', a);
            const std::string item = c.substr(a, b == std::string::npos ? std::string::npos : b - a);
            AppendMenuW(m, MF_STRING | (std::lround(p.in.base) == i ? MF_CHECKED : 0), M_CHOICE + i, W(item).c_str());
            a = b == std::string::npos ? b : b + 1;
        }
    } else {
        AppendMenuW(m, MF_STRING | (dxg::FreeMacro(s->graph) < 0 ? MF_GRAYED : 0), M_EXPOSE, L"Expose to Macro");
        HMENU link = CreatePopupMenu();
        for (int k = 0; k < kMacros; k++)
            AppendMenuW(link, MF_STRING, M_LINK + k, W(std::to_string(k + 1) + "  " + dxg::MacroLabel(s->graph, k)).c_str());
        AppendMenuW(m, MF_POPUP, (UINT_PTR)link, L"Link to Macro");
        for (const dxg::Link& l : p.in.links) {
            const int k = dxg::MacroIndex(l.src);
            if (k >= 0) AppendMenuW(m, MF_STRING, M_UNLINK + k, (L"Unlink " + W(dxg::MacroLabel(s->graph, k))).c_str());
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, M_RESET, L"Reset");
    }
    POINT sp = client;
    ClientToScreen(g_win.wnd, &sp);
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, g_win.wnd, nullptr);
    DestroyMenu(m);
    if (id == M_EXPOSE) {
        dxg::MacroWrite w;
        if (dxg::Expose(s->graph, node, p.key, "", &w)) Commit("expose", {w});
    } else if (id == M_RESET) {
        if (ps) {
            dxg::Param* q = n->Find(p.key);
            q->in.links.clear();
            dxg::SetValue(s->graph, node, p.key, ps->dflt);
            Commit("reset value");
        }
    } else if (id >= M_CHOICE && id < M_CHOICE + 64) {
        if (dxg::SetValue(s->graph, node, p.key, id - M_CHOICE)) Commit("set choice");
    } else if (id >= M_LINK && id < M_LINK + kMacros) {
        if (dxg::LinkMacro(s->graph, node, p.key, id - M_LINK, 1)) Commit("link macro");
    } else if (id >= M_UNLINK && id < M_UNLINK + kMacros) {
        if (dxg::UnlinkMacro(s->graph, node, p.key, id - M_UNLINK)) Commit("unlink macro");
    }
    Redraw();
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
    case WM_SIZE: Redraw(); return 0;
    case WM_LBUTTONDOWN: {
        if (!s) return 0;
        SetFocus(h);
        const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const HitInfo hi = HitTest(p);
        g_win.start = g_win.now = p;
        g_win.moved = false;
        g_win.dragNode = hi.node;
        g_win.dragIndex = hi.index;
        if (hi.what == Hit::Title) {
            const dxg::Node* n = s->graph.FindNode(hi.node);
            g_win.selected = hi.node;
            g_win.drag = Drag::Move;
            g_win.startX = n->x, g_win.startY = n->y;
        } else if (hi.what == Hit::OutPort) {
            g_win.drag = Drag::Wire;
        } else if (hi.what == Hit::Value) {
            const dxg::Node* n = s->graph.FindNode(hi.node);
            g_win.selected = hi.node;
            g_win.drag = Drag::Scrub;
            g_win.startValue = n->params[hi.index].in.base;
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
            dxg::MoveNode(s->graph, g_win.dragNode, g_win.startX + dx, g_win.startY + dy);
        } else if (g_win.drag == Drag::Pan) {
            g_win.panX = g_win.startX + dx, g_win.panY = g_win.startY + dy;
        } else if (g_win.drag == Drag::Scrub && g_win.moved) {
            // only the window changes while dragging; the release writes once (design v4 7)
            dxg::Node* n = s->graph.FindNode(g_win.dragNode);
            if (n && g_win.dragIndex < (int)n->params.size()) {
                const dxg::ParamSpec* ps = dxg::FindParam(dxg::Spec(n->type), n->params[g_win.dragIndex].key);
                if (ps && !ps->choices) {
                    double step = (ps->softMax - ps->softMin) / 300.0;
                    if (GetKeyState(VK_SHIFT) < 0) step *= 10;
                    if (GetKeyState(VK_CONTROL) < 0) step *= 0.1;
                    dxg::SetValue(s->graph, n->id, ps->key, g_win.startValue + dx * step);
                }
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
            const HitInfo hi = HitTest(p);
            if (hi.what == Hit::InPort && dxg::Connect(s->graph, g_win.dragNode, hi.node, hi.index)) Commit("wire");
        } else if (d == Drag::Scrub) {
            dxg::Node* n = s->graph.FindNode(g_win.dragNode);
            if (n && g_win.dragIndex < (int)n->params.size()) {
                if (g_win.moved) {
                    if (n->params[g_win.dragIndex].in.base != g_win.startValue) Commit("drag value");
                } else {
                    const dxg::ParamSpec* ps = dxg::FindParam(dxg::Spec(n->type), n->params[g_win.dragIndex].key);
                    if (ps && ps->choices) ValueMenu(p, n->id, g_win.dragIndex);
                    else
                        BeginEdit(ValueRect(*n, g_win.dragIndex), Num(n->params[g_win.dragIndex].in.base, false), n->id,
                                  g_win.dragIndex, -1);
                }
            }
        }
        Redraw();
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
            g_win.drag = Drag::None;
            Redraw();
        }
        return 0;
    case WM_RBUTTONUP: {
        if (!s) return 0;
        const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const HitInfo hi = HitTest(p);
        if (hi.what == Hit::Canvas) AddMenu(p);
        else if (hi.what == Hit::Value) ValueMenu(p, hi.node, hi.index);
        else if (hi.what == Hit::InPort && s->graph.WireInto(hi.node, hi.index)) {
            HMENU mm = CreatePopupMenu();
            AppendMenuW(mm, MF_STRING, M_UNWIRE, L"Unwire");
            POINT sp = p;
            ClientToScreen(h, &sp);
            const int id = TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, h, nullptr);
            DestroyMenu(mm);
            if (id == M_UNWIRE && dxg::Disconnect(s->graph, hi.node, hi.index)) Commit("unwire");
            Redraw();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        if (!s) return 0;
        const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const HitInfo hi = HitTest(p);
        if (hi.what == Hit::Macro) {
            for (size_t row = 0; row < g_win.macroRows.size(); row++)
                if (g_win.macroRows[row] == hi.index) {
                    RECT r = MacroRow((int)row);
                    r.left += 30, r.right -= 80;
                    BeginEdit(r, s->graph.macros[hi.index].name, 0, -1, hi.index);
                }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (!s) return 0;
        if (w == VK_DELETE && g_win.selected) {
            if (dxg::RemoveNode(s->graph, g_win.selected)) Commit("remove node");
            g_win.selected = 0;
            Redraw();
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
    // owned by AE's main window: it stays above it and goes with it
    void* owner = nullptr;
    Suite<AEGP_UtilitySuite6> util(g_pica, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (util) util->AEGP_GetMainHWND(&owner);
    g_win = Win{};
    g_win.s = s;
    g_win.font = font;
    g_win.bold = bold;
    HWND wnd = CreateWindowExW(0, L"DupliX2Editor", L"DupliX 2 Editor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               900, 560, (HWND)owner, nullptr, inst, nullptr);
    g_win.wnd = wnd;
    if (wnd) ShowWindow(wnd, SW_SHOWNORMAL);
    return wnd;
}

void GraphReloaded(HWND wnd)
{
    if (wnd != g_win.wnd) return;
    if (g_win.selected && g_win.s && !g_win.s->graph.FindNode(g_win.selected)) g_win.selected = 0;
    Redraw();
}

}  // namespace ed
}  // namespace dx2
