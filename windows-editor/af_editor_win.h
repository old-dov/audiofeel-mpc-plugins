// af_editor_win.h - a Windows VST2 editor for AudioFeel plugins (prototype).
//
// The MPC SDK's VST2 wrapper has no editor: the host shows a plain parameter list. This header adds
// one without touching the SDK or the plugin's own code: `afgui::attach(effect, layout)` wraps the
// effect's dispatcher, sets effFlagsHasEditor and answers effEditGetRect / effEditOpen /
// effEditClose / effEditIdle. The window is a plain Win32 child window drawn with GDI+, so any
// Windows VST2 host can embed it. Layout comes from the plugin's ui.json (see gen_layout.py).
//
// Controls: knob and fader = vertical drag (Shift = fine), mouse wheel, double-click = default.
// Switch = click cycles the choices. Menu = click opens a list.

#pragma once

#if defined(_WIN32)

#include <windows.h>
#include <ole2.h>      // PROPID etc., which gdiplus.h needs (windows.h leaves them out when lean)
#include <gdiplus.h>
#undef min   // windows.h macros: the plugin's own code uses std::min / std::max
#undef max

#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace afgui {

using Param = mpc::Param;

enum class Widget { Knob, Fader, Switch, Menu };

struct Control
{
    const char* param;   // parameter name as declared by the plugin; nullptr = empty slot
    Widget widget;
    bool bipolar;
};

struct Page
{
    const char* name;
    std::vector<Control> controls;
};

struct Layout
{
    const char* title;
    const char* accent;  // ui.json "kit": cyan, amber...
    std::vector<Page> pages;
};

namespace detail {

constexpr VstInt32 kBeginEdit = 43;  // audioMasterBeginEdit
constexpr VstInt32 kEndEdit = 44;    // audioMasterEndEdit

inline std::wstring wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

inline Gdiplus::Color accentFor(const char* kit)
{
    std::string k = kit ? kit : "";
    if (k.size() == 7 && k[0] == '#')
    {
        unsigned v = (unsigned)std::strtoul(k.c_str() + 1, nullptr, 16);
        return Gdiplus::Color(255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    }
    if (k == "amber") return Gdiplus::Color(255, 255, 176, 40);
    if (k == "green") return Gdiplus::Color(255, 90, 220, 120);
    if (k == "red")   return Gdiplus::Color(255, 255, 90, 80);
    if (k == "violet" || k == "purple") return Gdiplus::Color(255, 176, 120, 255);
    return Gdiplus::Color(255, 0, 200, 255);  // cyan
}

struct Cell
{
    int index;           // plugin parameter index, -1 = empty
    Widget widget;
    bool bipolar;
    int x, y, w, h;      // device pixels
};

class Editor
{
public:
    Editor(mpc::Plugin* plugin, const Layout* layout) : plugin_(plugin), layout_(layout) {}
    ~Editor() { close(); }

    ERect rect;          // handed to the host by effEditGetRect

    // Computes the window size (also needed before the window exists).
    void measure()
    {
        HDC dc = GetDC(nullptr);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        scale_ = dpi / 96.0f;
        if (scale_ < 1.0f) scale_ = 1.0f;
        if (scale_ > 3.0f) scale_ = 3.0f;

        // 8 columns at most; a plugin whose pages fit in 4 rows shows them all (stacked), the others
        // get a tab per page.
        size_t widest = 4;
        for (const Page& pg : layout_->pages) widest = pg.controls.size() > widest ? pg.controls.size() : widest;
        cols_ = widest > 8 ? 8 : (int)widest;
        int totalRows = 0, maxRows = 1;
        for (const Page& pg : layout_->pages)
        {
            int rows = ((int)pg.controls.size() + cols_ - 1) / cols_;
            if (rows < 1) rows = 1;
            totalRows += rows;
            maxRows = rows > maxRows ? rows : maxRows;
        }
        tabs_ = totalRows > 4 && layout_->pages.size() > 1;
        if (current_ >= (int)layout_->pages.size()) current_ = 0;
        width_ = px(2 * kMargin + cols_ * kCellW);
        height_ = tabs_ ? px(kHeader + kTabBar + maxRows * kCellH + kMargin)
                        : px(kHeader + totalRows * kCellH + (int)layout_->pages.size() * (kTitleH + 6) + kMargin);
        rect.top = 0; rect.left = 0;
        rect.bottom = (VstInt16)height_; rect.right = (VstInt16)width_;
        build();
    }

    bool open(HWND parent)
    {
        if (hwnd_ != nullptr) return true;
        measure();
        startGdiplus();
        registerClass();
        hwnd_ = CreateWindowExW(0, className(), L"AudioFeel", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0, 0, width_, height_, parent, nullptr, module(), this);
        if (hwnd_ == nullptr) { stopGdiplus(); return false; }
        SetTimer(hwnd_, 1, 33, nullptr);
        return true;
    }

    void close()
    {
        if (hwnd_ != nullptr)
        {
            HWND h = hwnd_;
            hwnd_ = nullptr;
            KillTimer(h, 1);
            DestroyWindow(h);
            stopGdiplus();
            unregisterClassIfLast();
        }
    }

private:
    static constexpr int kMargin = 16, kHeader = 40, kSection = 158, kCellW = 92, kCellH = 130, kTitleH = 22, kTabBar = 36;

    int px(int v) const { return (int)std::lround(v * scale_); }

    // ---- GDI+ lifetime (shared by every editor of this DLL) --------------------------------
    static int& gdipUsers() { static int n = 0; return n; }
    static ULONG_PTR& gdipToken() { static ULONG_PTR t = 0; return t; }
    static void startGdiplus()
    {
        if (gdipUsers()++ == 0)
        {
            Gdiplus::GdiplusStartupInput in;
            Gdiplus::GdiplusStartup(&gdipToken(), &in, nullptr);
        }
    }
    static void stopGdiplus()
    {
        if (--gdipUsers() == 0) Gdiplus::GdiplusShutdown(gdipToken());
    }

    // ---- window class ---------------------------------------------------------------------
    static HINSTANCE module()
    {
        static HMODULE h = nullptr;
        if (h == nullptr)
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&module, &h);
        return h;
    }
    static const wchar_t* className()
    {
        static wchar_t name[64] = {0};
        if (name[0] == 0) swprintf(name, 64, L"AudioFeelEditor_%p", (void*)&module);
        return name;
    }
    static int& classUsers() { static int n = 0; return n; }
    static void registerClass()
    {
        if (classUsers()++ > 0) return;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Editor::wndProc;
        wc.hInstance = module();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = className();
        RegisterClassExW(&wc);
    }
    static void unregisterClassIfLast()
    {
        if (--classUsers() == 0) UnregisterClassW(className(), module());
    }

    // ---- layout -----------------------------------------------------------------------------
    void build()
    {
        cells_.clear();
        titles_.clear();
        tabRects_.clear();
        int top = kHeader;
        if (tabs_)
        {
            int n = (int)layout_->pages.size();
            int tw = (width_ - 2 * px(kMargin)) / n;
            for (int i = 0; i < n; ++i)
                tabRects_.push_back({px(kMargin) + i * tw, px(kHeader), px(kMargin) + (i + 1) * tw, px(kHeader + kTabBar - 8)});
            top = kHeader + kTabBar;
        }
        for (size_t p = 0; p < layout_->pages.size(); ++p)
        {
            if (tabs_ && (int)p != current_) continue;
            const Page& page = layout_->pages[p];
            if (!tabs_) titles_.push_back({page.name, px(kMargin), px(top)});
            int rowsTop = tabs_ ? top : top + kTitleH;
            for (size_t c = 0; c < page.controls.size(); ++c)
            {
                const Control& ctl = page.controls[c];
                int col = (int)c % cols_, row = (int)c / cols_;
                Cell cell{-1, ctl.widget, ctl.bipolar,
                          px(kMargin + col * kCellW), px(rowsTop + row * kCellH), px(kCellW), px(kCellH)};
                if (ctl.param != nullptr)
                    for (int i = 0; i < plugin_->numParams(); ++i)
                        if (plugin_->param(i).name == ctl.param) { cell.index = i; break; }
                cells_.push_back(cell);
            }
            int rows = ((int)page.controls.size() + cols_ - 1) / cols_;
            top += (tabs_ ? 0 : kTitleH + (rows < 1 ? 1 : rows) * kCellH + 6);
        }
        shown_.assign((size_t)plugin_->numParams(), -1.0f);
    }

    // ---- painting -------------------------------------------------------------------------
    void paint(HDC target)
    {
        using namespace Gdiplus;
        HDC mem = CreateCompatibleDC(target);
        HBITMAP bmp = CreateCompatibleBitmap(target, width_, height_);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            const Color accent = accentFor(layout_->accent);
            SolidBrush bg(Color(255, 17, 20, 25));
            g.FillRectangle(&bg, 0, 0, width_, height_);

            FontFamily fam(L"Segoe UI");
            Font small(&fam, (REAL)px(11), FontStyleRegular, UnitPixel);
            Font label(&fam, (REAL)px(12), FontStyleRegular, UnitPixel);
            Font title(&fam, (REAL)px(18), FontStyleBold, UnitPixel);
            Font section(&fam, (REAL)px(11), FontStyleBold, UnitPixel);
            SolidBrush text(Color(255, 230, 237, 243));
            SolidBrush dim(Color(255, 125, 136, 150));
            SolidBrush acc(accent);
            StringFormat center; center.SetAlignment(StringAlignmentCenter); center.SetLineAlignment(StringAlignmentCenter);
            center.SetFormatFlags(StringFormatFlagsNoWrap); center.SetTrimming(StringTrimmingEllipsisCharacter);
            StringFormat left; left.SetAlignment(StringAlignmentNear); left.SetLineAlignment(StringAlignmentCenter);
            StringFormat right; right.SetAlignment(StringAlignmentFar); right.SetLineAlignment(StringAlignmentCenter);

            // header
            g.DrawString(wide(layout_->title).c_str(), -1, &title, RectF((REAL)px(kMargin), 0, (REAL)width_, (REAL)px(kHeader)), &left, &acc);
            g.DrawString(L"AudioFeel", -1, &small, RectF(0, 0, (REAL)(width_ - px(kMargin)), (REAL)px(kHeader)), &right, &dim);
            Pen rule(Color(255, 44, 52, 62), 1.0f);
            for (const auto& t : titles_)
            {
                g.DrawString(wide(t.name).c_str(), -1, &section, RectF((REAL)t.x, (REAL)t.y, (REAL)px(200), (REAL)px(kTitleH)), &left, &dim);
                g.DrawLine(&rule, t.x, t.y + px(kTitleH) - 2, width_ - px(kMargin), t.y + px(kTitleH) - 2);
            }

            for (size_t t = 0; t < tabRects_.size(); ++t)
            {
                const RECT& r = tabRects_[t];
                bool active = (int)t == current_;
                RectF box((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
                g.DrawString(wide(layout_->pages[t].name).c_str(), -1, &section, box, &center, active ? &acc : &dim);
                Pen underline(active ? accent : Color(255, 44, 52, 62), (REAL)px(active ? 3 : 1));
                g.DrawLine(&underline, (INT)(r.left + px(4)), (INT)r.bottom, (INT)(r.right - px(4)), (INT)r.bottom);
            }

            for (const Cell& c : cells_)
            {
                if (c.index < 0) continue;
                const Param& prm = plugin_->param(c.index);
                float v = plugin_->getNormalised(c.index);
                std::wstring name = wide(prm.name);
                std::string val = plugin_->displayText(c.index);
                if (!prm.unit.empty() && prm.kind == Param::Kind::Float) val += " " + prm.unit;
                float cx = (float)c.x + c.w / 2.0f;
                RectF nameBox((REAL)c.x, (REAL)(c.y + px(78)), (REAL)c.w, (REAL)px(16));
                RectF valBox((REAL)c.x, (REAL)(c.y + px(94)), (REAL)c.w, (REAL)px(16));

                if (c.widget == Widget::Knob)
                {
                    float r = (float)px(27), cy = (float)(c.y + px(42));
                    RectF arc(cx - r, cy - r, 2 * r, 2 * r);
                    Pen track(Color(255, 44, 52, 62), (REAL)px(5)); track.SetStartCap(LineCapRound); track.SetEndCap(LineCapRound);
                    g.DrawArc(&track, arc, 135.0f, 270.0f);
                    Pen active(accent, (REAL)px(5)); active.SetStartCap(LineCapRound); active.SetEndCap(LineCapRound);
                    if (c.bipolar)
                    {
                        float sweep = 135.0f * (2.0f * v - 1.0f);
                        if (std::fabs(sweep) > 0.5f) g.DrawArc(&active, arc, 270.0f, sweep);
                    }
                    else if (v > 0.002f) g.DrawArc(&active, arc, 135.0f, 270.0f * v);
                    float a = (135.0f + 270.0f * v) * 3.14159265f / 180.0f;
                    Pen hand(Color(255, 230, 237, 243), (REAL)px(2)); hand.SetStartCap(LineCapRound); hand.SetEndCap(LineCapRound);
                    g.DrawLine(&hand, cx + std::cos(a) * r * 0.35f, cy + std::sin(a) * r * 0.35f,
                               cx + std::cos(a) * r * 0.80f, cy + std::sin(a) * r * 0.80f);
                }
                else if (c.widget == Widget::Fader)
                {
                    float top = (float)(c.y + px(8)), h = (float)px(66);
                    SolidBrush trackBrush(Color(255, 44, 52, 62));
                    g.FillRectangle(&trackBrush, cx - px(3), top, (REAL)px(6), h);
                    SolidBrush fill(accent);
                    float ty = top + h * (1.0f - v);
                    g.FillRectangle(&fill, cx - px(3), ty, (REAL)px(6), top + h - ty);
                    SolidBrush thumb(Color(255, 230, 237, 243));
                    g.FillRectangle(&thumb, cx - px(11), ty - px(5), (REAL)px(22), (REAL)px(10));
                }
                else
                {
                    RectF box(cx - px(38), (REAL)(c.y + px(26)), (REAL)px(76), (REAL)px(34));
                    bool on = c.widget == Widget::Switch && prm.kind == Param::Kind::Bool && v >= 0.5f;
                    SolidBrush fill(on ? Color(255, accent.GetR(), accent.GetG(), accent.GetB()) : Color(255, 30, 36, 44));
                    Pen edge(Color(255, 70, 82, 96), 1.0f);
                    g.FillRectangle(&fill, box); g.DrawRectangle(&edge, box);
                    SolidBrush inside(on ? Color(255, 10, 14, 18) : Color(255, 230, 237, 243));
                    std::string shown = plugin_->displayText(c.index);
                    if (c.widget == Widget::Menu) shown += "  \xE2\x96\xBE";
                    g.DrawString(wide(shown).c_str(), -1, &label, box, &center, &inside);
                }
                g.DrawString(name.c_str(), -1, &label, nameBox, &center, &text);
                if (c.widget == Widget::Knob || c.widget == Widget::Fader)
                    g.DrawString(wide(val).c_str(), -1, &small, valBox, &center, &dim);
            }
        }
        BitBlt(target, 0, 0, width_, height_, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        for (int i = 0; i < plugin_->numParams(); ++i) shown_[(size_t)i] = plugin_->getNormalised(i);
    }

    // ---- interaction ---------------------------------------------------------------------
    const Cell* hit(int x, int y) const
    {
        for (const Cell& c : cells_)
            if (c.index >= 0 && x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) return &c;
        return nullptr;
    }

    void beginEdit(int i) { if (plugin_->host_ && plugin_->effect_) plugin_->host_(plugin_->effect_, kBeginEdit, i, 0, nullptr, 0.0f); }
    void endEdit(int i) { if (plugin_->host_ && plugin_->effect_) plugin_->host_(plugin_->effect_, kEndEdit, i, 0, nullptr, 0.0f); }
    void change(int i, float norm)
    {
        plugin_->setNormalised(i, norm);
        plugin_->notifyHostParameterChanged(i);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void stepChoice(int i, int dir)
    {
        const Param& p = plugin_->param(i);
        int n = p.kind == Param::Kind::Choice ? (int)p.choices.size() : 2;
        if (n < 2) return;
        int cur = (int)std::lround(p.toPlain(plugin_->getNormalised(i)));
        int next = ((cur + dir) % n + n) % n;
        beginEdit(i); change(i, p.toNormalised((float)next)); endEdit(i);
    }

    void openMenu(const Cell& c)
    {
        const Param& p = plugin_->param(c.index);
        if (p.kind != Param::Kind::Choice) { stepChoice(c.index, 1); return; }
        HMENU m = CreatePopupMenu();
        int cur = (int)std::lround(p.toPlain(plugin_->getNormalised(c.index)));
        for (size_t k = 0; k < p.choices.size(); ++k)
            AppendMenuW(m, MF_STRING | ((int)k == cur ? MF_CHECKED : 0), (UINT_PTR)(k + 1), wide(p.choices[k]).c_str());
        POINT pt = {c.x, c.y + c.h - px(40)};
        ClientToScreen(hwnd_, &pt);
        int sel = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(m);
        if (sel > 0) { beginEdit(c.index); change(c.index, p.toNormalised((float)(sel - 1))); endEdit(c.index); }
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                paint(dc);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_TIMER:
                for (int i = 0; i < plugin_->numParams(); ++i)
                    if (std::fabs(shown_[(size_t)i] - plugin_->getNormalised(i)) > 1e-5f) { InvalidateRect(hwnd_, nullptr, FALSE); break; }
                return 0;
            case WM_LBUTTONDOWN:
            {
                for (size_t t = 0; t < tabRects_.size(); ++t)
                {
                    const RECT& r = tabRects_[t];
                    int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
                    if (mx >= r.left && mx < r.right && my >= r.top && my <= r.bottom + px(6))
                    {
                        current_ = (int)t;
                        build();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                }
                const Cell* c = hit((short)LOWORD(lp), (short)HIWORD(lp));
                if (c == nullptr) return 0;
                if (c->widget == Widget::Switch) { stepChoice(c->index, 1); return 0; }
                if (c->widget == Widget::Menu) { openMenu(*c); return 0; }
                drag_ = c->index;
                dragY_ = (short)HIWORD(lp);
                dragValue_ = plugin_->getNormalised(c->index);
                dragRange_ = (float)px(c->widget == Widget::Fader ? 66 : 200);
                SetCapture(hwnd_);
                beginEdit(drag_);
                return 0;
            }
            case WM_MOUSEMOVE:
                if (drag_ >= 0)
                {
                    float fine = (GetKeyState(VK_SHIFT) & 0x8000) ? 0.2f : 1.0f;
                    float dy = (float)(dragY_ - (short)HIWORD(lp));
                    change(drag_, mpc::clamp(dragValue_ + fine * dy / dragRange_, 0.0f, 1.0f));
                }
                return 0;
            case WM_LBUTTONUP:
                if (drag_ >= 0) { endEdit(drag_); drag_ = -1; ReleaseCapture(); }
                return 0;
            case WM_CAPTURECHANGED:
                if (drag_ >= 0) { endEdit(drag_); drag_ = -1; }
                return 0;
            case WM_LBUTTONDBLCLK:
            {
                const Cell* c = hit((short)LOWORD(lp), (short)HIWORD(lp));
                if (c != nullptr && (c->widget == Widget::Knob || c->widget == Widget::Fader))
                {
                    const Param& p = plugin_->param(c->index);
                    beginEdit(c->index); change(c->index, p.toNormalised(p.def)); endEdit(c->index);
                }
                return 0;
            }
            case WM_MOUSEWHEEL:
            {
                POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
                ScreenToClient(hwnd_, &pt);
                const Cell* c = hit(pt.x, pt.y);
                if (c == nullptr) return 0;
                int dir = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
                if (c->widget == Widget::Switch || c->widget == Widget::Menu) { stepChoice(c->index, dir); return 0; }
                float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 0.005f : 0.025f;
                const Param& p = plugin_->param(c->index);
                if (p.kind == Param::Kind::Int || p.kind == Param::Kind::Choice) { stepChoice(c->index, dir); return 0; }
                beginEdit(c->index); change(c->index, mpc::clamp(plugin_->getNormalised(c->index) + dir * step, 0.0f, 1.0f)); endEdit(c->index);
                return 0;
            }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
        {
            Editor* e = (Editor*)((CREATESTRUCTW*)lp)->lpCreateParams;
            e->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)e);
        }
        Editor* e = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (e == nullptr || e->hwnd_ == nullptr) return DefWindowProcW(h, msg, wp, lp);
        return e->handle(msg, wp, lp);
    }

    struct Title { const char* name; int x, y; };

    mpc::Plugin* plugin_;
    const Layout* layout_;
    HWND hwnd_ = nullptr;
    float scale_ = 1.0f;
    int width_ = 0, height_ = 0;
    int cols_ = 8;
    bool tabs_ = false;
    int current_ = 0;
    std::vector<RECT> tabRects_;
    std::vector<Cell> cells_;
    std::vector<Title> titles_;
    std::vector<float> shown_;
    int drag_ = -1;
    int dragY_ = 0;
    float dragValue_ = 0.0f;
    float dragRange_ = 200.0f;
};

// ---- per-effect state and the dispatcher wrapper ---------------------------------------------
struct State
{
    AEffectDispatcherProc original;
    std::unique_ptr<Editor> editor;
};

inline std::map<AEffect*, State*>& states() { static std::map<AEffect*, State*> m; return m; }

inline VstIntPtr dispatch(AEffect* e, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    auto it = states().find(e);
    if (it == states().end()) return 0;
    State* st = it->second;
    switch (opcode)
    {
        case effEditGetRect:
            if (ptr == nullptr) return 0;
            st->editor->measure();
            *(ERect**)ptr = &st->editor->rect;
            return 1;
        case effEditOpen:
            return st->editor->open((HWND)ptr) ? 1 : 0;
        case effEditClose:
            st->editor->close();
            return 0;
        case effEditIdle:
            return 0;
        case effClose:
        {
            AEffectDispatcherProc original = st->original;
            st->editor.reset();
            states().erase(it);
            delete st;
            return original(e, opcode, index, value, ptr, opt);  // frees the plugin and the effect
        }
        default:
            return st->original(e, opcode, index, value, ptr, opt);
    }
}

} // namespace detail

// Gives `effect` (made by mpc::detail::createEffect) an editor drawn from `layout`.
inline void attach(AEffect* effect, const Layout& layout)
{
    if (effect == nullptr) return;
    mpc::Plugin* plugin = static_cast<mpc::Plugin*>(effect->object);
    if (plugin == nullptr) return;
    detail::State* st = new detail::State{effect->dispatcher, std::make_unique<detail::Editor>(plugin, &layout)};
    detail::states()[effect] = st;
    effect->dispatcher = &detail::dispatch;
    effect->flags |= effFlagsHasEditor;
}

} // namespace afgui

#endif // _WIN32
