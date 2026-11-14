#include "ui/View.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <oleacc.h>
#include <uiautomation.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ui/MainWindow.h"
#include "ui/UiCommon.h"

namespace ss {

namespace {
// Accent presets: a strong colour for the light theme (white text on it passes 4.5:1) and a bright one for the dark theme.
struct AccentPreset {
    const wchar_t* name;
    COLORREF light, dark;
};
const AccentPreset kAccents[] = {{L"Emerald", RGB(0, 108, 86), RGB(52, 211, 163)},   {L"Blue", RGB(0, 95, 184), RGB(96, 165, 250)},
                                 {L"Violet", RGB(103, 58, 183), RGB(167, 139, 250)}, {L"Rose", RGB(190, 24, 93), RGB(244, 114, 182)},
                                 {L"Orange", RGB(180, 83, 9), RGB(251, 146, 60)},    {L"Teal", RGB(14, 116, 144), RGB(34, 211, 238)},
                                 {L"Graphite", RGB(55, 65, 81), RGB(156, 163, 175)}};
constexpr int kAccentN = sizeof kAccents / sizeof kAccents[0];

// a + (b - a) * t
COLORREF Mix(COLORREF a, COLORREF b, double t) {
    auto m = [&](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

// Every accent-dependent colour (tints, bars, nav gradient) is derived from the one accent, so a new preset needs one line.
// Light: black text (21:1), secondary text >= 9:1, disabled >= 4.5:1. Dark: pure black (OLED style), only cards/hover lifted.
Theme BuildTheme(bool dark, int idx) {
    const AccentPreset& ap = kAccents[idx];
    Theme t{};
    t.dark = dark;
    const COLORREF W = RGB(255, 255, 255), K = RGB(0, 0, 0);
    if (!dark) {
        COLORREF ac = ap.light;
        t.bg = Mix(W, ac, 0.045);       t.panel = W;                    t.text = K;                     t.muted = RGB(52, 56, 56);
        t.accent = ac;                  t.accentSoft = Mix(W, ac, 0.19); t.danger = RGB(172, 24, 12);   t.success = RGB(8, 98, 8);
        t.border = Mix(RGB(178, 178, 178), ac, 0.07);                    t.barBg = Mix(W, ac, 0.12);
        t.hover = Mix(W, ac, 0.06);     t.pressed = Mix(W, ac, 0.14);   t.disabled = RGB(108, 112, 112);
        t.barFill = Mix(W, ac, 0.38);   t.barFillSel = Mix(W, ac, 0.6);
        t.cardTop = W;                  t.cardBot = Mix(W, ac, 0.08);   t.cardBorder = Mix(W, ac, 0.22);
        t.navTop = Mix(W, ac, 0.14);    t.navBot = Mix(W, ac, 0.03);
    } else {
        COLORREF ac = ap.dark;
        t.bg = K;                       t.panel = K;                    t.text = RGB(240, 240, 242);    t.muted = RGB(150, 152, 158);
        t.accent = ac;                  t.accentSoft = Mix(K, ac, 0.2); t.danger = RGB(255, 108, 96);   t.success = RGB(96, 205, 96);
        t.border = RGB(44, 44, 48);     t.barBg = RGB(34, 34, 36);
        t.hover = RGB(26, 26, 30);      t.pressed = RGB(44, 44, 50);    t.disabled = RGB(100, 102, 108);
        t.barFill = Mix(K, ac, 0.27);   t.barFillSel = Mix(K, ac, 0.6);
        t.cardTop = RGB(22, 22, 26);    t.cardBot = RGB(8, 8, 10);      t.cardBorder = RGB(52, 54, 62);
        t.navTop = K;                   t.navBot = K;
    }
    return t;
}
int g_accent = 0;
bool g_dark = false;
Theme g_cur = BuildTheme(false, 0);
HBRUSH g_bg = nullptr, g_panel = nullptr;
}  // namespace

const Theme& Colors() { return g_cur; }
bool IsDarkTheme() { return g_cur.dark; }
void SetDarkTheme(bool dark) {
    g_dark = dark;
    g_cur = BuildTheme(dark, g_accent);
    if (g_bg) DeleteObject(g_bg);
    if (g_panel) DeleteObject(g_panel);
    g_bg = CreateSolidBrush(g_cur.bg);
    g_panel = CreateSolidBrush(g_cur.panel);
}
void SetAccent(int idx) { g_accent = (idx >= 0 && idx < kAccentN) ? idx : 0; }  // takes effect with the next SetDarkTheme
int AccentIndex() { return g_accent; }
int AccentCount() { return kAccentN; }
const wchar_t* AccentName(int i) { return kAccents[(i >= 0 && i < kAccentN) ? i : 0].name; }
COLORREF AccentColor(int i, bool dark) { const AccentPreset& a = kAccents[(i >= 0 && i < kAccentN) ? i : 0]; return dark ? a.dark : a.light; }
COLORREF CategoryColor(FileCategory c) {
    static const COLORREF k[] = {RGB(225, 86, 86),  RGB(240, 160, 48), RGB(204, 190, 60), RGB(60, 170, 110),
                                 RGB(60, 160, 200), RGB(110, 120, 230), RGB(170, 100, 220), RGB(140, 146, 156)};
    return k[static_cast<int>(c)];
}

HBRUSH BgBrush() {
    if (!g_bg) SetDarkTheme(false);
    return g_bg;
}
HBRUSH PanelBrush() {
    if (!g_panel) SetDarkTheme(false);
    return g_panel;
}

// ---- SSCombo: rounded drop-down with the CB_* messages/CBN_SELCHANGE the views already use.
// The list is a native popup menu (rounded and themed by Windows 11).
namespace {
struct ComboData {
    std::vector<std::wstring> items;
    int sel = -1;
    bool hot = false;
    HFONT font = nullptr;
};

void ComboSelect(HWND h, ComboData* d, int i) {
    if (i < 0 || i >= static_cast<int>(d->items.size()) || i == d->sel) return;
    d->sel = i;
    InvalidateRect(h, nullptr, FALSE);
    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), CBN_SELCHANGE), reinterpret_cast<LPARAM>(h));
}

void ComboOpen(HWND h, ComboData* d) {
    if (!IsWindowEnabled(h) || d->items.empty()) return;
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < d->items.size(); ++i)
        AppendMenuW(m, MF_STRING | (static_cast<int>(i) == d->sel ? MF_CHECKED : 0), 1000 + i, d->items[i].c_str());
    RECT rc;
    GetWindowRect(h, &rc);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rc.left, rc.bottom + 2, 0, h, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000) ComboSelect(h, d, cmd - 1000);
}

void ComboPaint(HWND h, ComboData* d) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    const Theme& c = Colors();
    bool enabled = IsWindowEnabled(h) != 0, focus = GetFocus() == h;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(mem, bmp);
    FillRect(mem, &rc, BgBrush());
    int r = MulDiv(10, static_cast<int>(GetDpiForWindow(h)), 96);
    FillRoundRectAA(mem, rc, r / 2, d->hot && enabled ? c.hover : c.panel, focus ? c.accent : c.border, focus ? 2.f : 1.f);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, enabled ? c.text : c.disabled);
    HGDIOBJ of = SelectObject(mem, d->font ? d->font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    int pad = MulDiv(12, static_cast<int>(GetDpiForWindow(h)), 96);
    RECT tr{pad, 0, rc.right - 3 * pad, rc.bottom};
    if (d->sel >= 0) DrawTextW(mem, d->items[static_cast<size_t>(d->sel)].c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, of);
    // chevron
    int cx = rc.right - pad - 4, cy = rc.bottom / 2, s = MulDiv(4, static_cast<int>(GetDpiForWindow(h)), 96);
    DrawChevronAA(mem, cx, cy, s, enabled ? c.text : c.disabled, 1.8f * GetDpiForWindow(h) / 96.f);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

// MSAA wrapper base: the standard accessible object for the window; every call is forwarded, so a custom control
// overrides only the answers it needs to change (role, state, value, default action).
class DelegatingAcc : public IAccessible {
public:
    DelegatingAcc(HWND h, IAccessible* inner) : h_(h), in_(inner) {}
    ~DelegatingAcc() { in_->Release(); }
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == IID_IUnknown || r == IID_IDispatch || r == IID_IAccessible) {
            *p = static_cast<IAccessible*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP GetTypeInfoCount(UINT* p) override { return in_->GetTypeInfoCount(p); }
    STDMETHODIMP GetTypeInfo(UINT i, LCID l, ITypeInfo** t) override { return in_->GetTypeInfo(i, l, t); }
    STDMETHODIMP GetIDsOfNames(REFIID r, LPOLESTR* n, UINT c, LCID l, DISPID* d) override { return in_->GetIDsOfNames(r, n, c, l, d); }
    STDMETHODIMP Invoke(DISPID d, REFIID r, LCID l, WORD f, DISPPARAMS* p, VARIANT* v, EXCEPINFO* e, UINT* u) override {
        return in_->Invoke(d, r, l, f, p, v, e, u);
    }
    STDMETHODIMP get_accParent(IDispatch** p) override { return in_->get_accParent(p); }
    STDMETHODIMP get_accChildCount(long* n) override { return in_->get_accChildCount(n); }
    STDMETHODIMP get_accChild(VARIANT v, IDispatch** p) override { return in_->get_accChild(v, p); }
    STDMETHODIMP get_accName(VARIANT v, BSTR* s) override { return in_->get_accName(v, s); }
    STDMETHODIMP get_accValue(VARIANT v, BSTR* s) override { return in_->get_accValue(v, s); }
    STDMETHODIMP get_accDescription(VARIANT v, BSTR* s) override { return in_->get_accDescription(v, s); }
    STDMETHODIMP get_accRole(VARIANT v, VARIANT* r) override { return in_->get_accRole(v, r); }
    STDMETHODIMP get_accState(VARIANT v, VARIANT* s) override { return in_->get_accState(v, s); }
    STDMETHODIMP get_accHelp(VARIANT v, BSTR* s) override { return in_->get_accHelp(v, s); }
    STDMETHODIMP get_accHelpTopic(BSTR* f, VARIANT v, long* t) override { return in_->get_accHelpTopic(f, v, t); }
    STDMETHODIMP get_accKeyboardShortcut(VARIANT v, BSTR* s) override { return in_->get_accKeyboardShortcut(v, s); }
    STDMETHODIMP get_accFocus(VARIANT* v) override { return in_->get_accFocus(v); }
    STDMETHODIMP get_accSelection(VARIANT* v) override { return in_->get_accSelection(v); }
    STDMETHODIMP get_accDefaultAction(VARIANT v, BSTR* s) override { return in_->get_accDefaultAction(v, s); }
    STDMETHODIMP accSelect(long f, VARIANT v) override { return in_->accSelect(f, v); }
    STDMETHODIMP accLocation(long* x, long* y, long* w, long* hh, VARIANT v) override { return in_->accLocation(x, y, w, hh, v); }
    STDMETHODIMP accNavigate(long d, VARIANT v, VARIANT* e) override { return in_->accNavigate(d, v, e); }
    STDMETHODIMP accHitTest(long x, long y, VARIANT* v) override { return in_->accHitTest(x, y, v); }
    STDMETHODIMP accDoDefaultAction(VARIANT v) override { return in_->accDoDefaultAction(v); }
    STDMETHODIMP put_accName(VARIANT v, BSTR s) override { return in_->put_accName(v, s); }
    STDMETHODIMP put_accValue(VARIANT v, BSTR s) override { return in_->put_accValue(v, s); }

protected:
    // Helpers for the overrides: the answer applies to the control itself (CHILDID_SELF), anything else is forwarded.
    static bool Self(const VARIANT& v) { return v.lVal == CHILDID_SELF; }
    static HRESULT Str(const wchar_t* t, BSTR* s) {
        *s = SysAllocString(t);
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    HWND h_;
    IAccessible* in_;

private:
    LONG ref_ = 1;
};

// SSCombo: reports "combo box", the selected text as value and the default action "Open" (screen readers treat it like a native drop-down).
class ComboAcc : public DelegatingAcc {
public:
    using DelegatingAcc::DelegatingAcc;
    STDMETHODIMP get_accValue(VARIANT v, BSTR* s) override {
        auto* d = reinterpret_cast<ComboData*>(GetWindowLongPtrW(h_, GWLP_USERDATA));
        if (Self(v) && d && d->sel >= 0) return Str(d->items[static_cast<size_t>(d->sel)].c_str(), s);
        return in_->get_accValue(v, s);
    }
    STDMETHODIMP get_accRole(VARIANT v, VARIANT* r) override {
        if (!Self(v)) return in_->get_accRole(v, r);
        r->vt = VT_I4;
        r->lVal = ROLE_SYSTEM_COMBOBOX;
        return S_OK;
    }
    STDMETHODIMP get_accDefaultAction(VARIANT v, BSTR* s) override { return Self(v) ? Str(L"Open", s) : in_->get_accDefaultAction(v, s); }
    STDMETHODIMP accDoDefaultAction(VARIANT v) override {
        if (!Self(v)) return in_->accDoDefaultAction(v);
        PostMessageW(h_, WM_KEYDOWN, VK_F4, 0);  // opens the list on the UI thread
        return S_OK;
    }
};

// UI Automation provider (what NVDA / Narrator / test tools use): control type ComboBox, name, value, expand.
class ComboUia : public IRawElementProviderSimple, public IValueProvider, public IExpandCollapseProvider {
public:
    explicit ComboUia(HWND h) : h_(h) {}
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        *p = nullptr;
        if (r == IID_IUnknown || r == IID_IRawElementProviderSimple) *p = static_cast<IRawElementProviderSimple*>(this);
        else if (r == IID_IValueProvider) *p = static_cast<IValueProvider*>(this);
        else if (r == IID_IExpandCollapseProvider) *p = static_cast<IExpandCollapseProvider*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    // IRawElementProviderSimple
    STDMETHODIMP get_ProviderOptions(ProviderOptions* o) override {
        *o = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    STDMETHODIMP GetPatternProvider(PATTERNID id, IUnknown** p) override {
        *p = nullptr;
        if (id == UIA_ValuePatternId) *p = static_cast<IValueProvider*>(this);
        else if (id == UIA_ExpandCollapsePatternId) *p = static_cast<IExpandCollapseProvider*>(this);
        if (*p) AddRef();
        return S_OK;
    }
    STDMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* v) override {
        VariantInit(v);
        switch (id) {
            case UIA_ControlTypePropertyId:
                v->vt = VT_I4;
                v->lVal = UIA_ComboBoxControlTypeId;
                break;
            case UIA_NamePropertyId: {
                wchar_t t[128] = L"";
                GetWindowTextW(h_, t, 128);
                v->vt = VT_BSTR;
                v->bstrVal = SysAllocString(t);
                break;
            }
            case UIA_IsKeyboardFocusablePropertyId:
            case UIA_IsEnabledPropertyId:
                v->vt = VT_BOOL;
                v->boolVal = IsWindowEnabled(h_) ? VARIANT_TRUE : VARIANT_FALSE;
                break;
        }
        return S_OK;  // VT_EMPTY = "use the default" for everything else
    }
    STDMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** p) override { return UiaHostProviderFromHwnd(h_, p); }
    // IValueProvider (read-only: the choice is changed with the list or the arrow keys)
    STDMETHODIMP SetValue(LPCWSTR) override { return UIA_E_NOTSUPPORTED; }
    STDMETHODIMP get_Value(BSTR* s) override {
        auto* d = reinterpret_cast<ComboData*>(GetWindowLongPtrW(h_, GWLP_USERDATA));
        *s = SysAllocString(d && d->sel >= 0 ? d->items[static_cast<size_t>(d->sel)].c_str() : L"");
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP get_IsReadOnly(BOOL* b) override {
        *b = TRUE;
        return S_OK;
    }
    // IExpandCollapseProvider
    STDMETHODIMP Expand() override {
        PostMessageW(h_, WM_KEYDOWN, VK_F4, 0);  // opens the list on the UI thread
        return S_OK;
    }
    STDMETHODIMP Collapse() override { return S_OK; }
    STDMETHODIMP get_ExpandCollapseState(ExpandCollapseState* s) override {
        *s = ExpandCollapseState_Collapsed;
        return S_OK;
    }

private:
    LONG ref_ = 1;
    HWND h_;
};

LRESULT CALLBACK ComboProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<ComboData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new ComboData));
            return TRUE;
        case WM_NCDESTROY:
            delete d;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case WM_ERASEBKGND: return 1;
        case WM_GETOBJECT:
            if (static_cast<LONG>(l) == static_cast<LONG>(UiaRootObjectId)) {
                auto* prov = new ComboUia(h);
                LRESULT lr = UiaReturnRawElementProvider(h, w, l, prov);
                prov->Release();
                return lr;
            }
            if (static_cast<LONG>(l) == OBJID_CLIENT) {
                IAccessible* inner = nullptr;
                if (SUCCEEDED(CreateStdAccessibleObject(h, OBJID_CLIENT, IID_PPV_ARGS(&inner)))) {
                    auto* acc = new ComboAcc(h, inner);
                    LRESULT lr = LresultFromObject(IID_IAccessible, w, acc);
                    acc->Release();
                    return lr;
                }
            }
            break;
        case WM_PAINT:
            if (d) ComboPaint(h, d);
            return 0;
        case WM_WINDOWPOSCHANGING: {  // callers pass tall heights meant for native drop-downs: keep a fixed control height
            auto* p = reinterpret_cast<WINDOWPOS*>(l);
            if (!(p->flags & SWP_NOSIZE)) p->cy = MulDiv(34, static_cast<int>(GetDpiForWindow(h)), 96);
            break;
        }
        case WM_SETFONT:
            if (d) d->font = reinterpret_cast<HFONT>(w);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETFONT: return d ? reinterpret_cast<LRESULT>(d->font) : 0;
        case CB_ADDSTRING:
            if (!d) return CB_ERR;
            d->items.emplace_back(reinterpret_cast<const wchar_t*>(l));
            return static_cast<LRESULT>(d->items.size() - 1);
        case CB_SETCURSEL:
            if (!d) return CB_ERR;
            d->sel = (static_cast<int>(w) >= 0 && static_cast<size_t>(w) < d->items.size()) ? static_cast<int>(w) : -1;
            InvalidateRect(h, nullptr, FALSE);
            return d->sel;
        case CB_GETCURSEL: return d ? d->sel : CB_ERR;
        case CB_GETCOUNT: return d ? static_cast<LRESULT>(d->items.size()) : 0;
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_ENABLE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (d && !d->hot) {
                d->hot = true;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (d) d->hot = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(h);
            if (d) ComboOpen(h, d);
            return 0;
        case WM_KEYDOWN:
            if (!d) break;
            if (w == VK_F4 || w == VK_SPACE || w == VK_RETURN) ComboOpen(h, d);
            else if (w == VK_DOWN) ComboSelect(h, d, d->sel + 1);
            else if (w == VK_UP) ComboSelect(h, d, d->sel - 1);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- SSCheck: check box drawn with the same rounded, anti-aliased box as the list ticks. Speaks BM_GETCHECK/BM_SETCHECK and sends
// BN_CLICKED like a native BS_AUTOCHECKBOX, so the views keep their code.
struct CheckData {
    int state = BST_UNCHECKED;
    bool hot = false, down = false;
    HFONT font = nullptr;
};

void CheckToggle(HWND h, CheckData* d) {
    d->state = d->state == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED;
    InvalidateRect(h, nullptr, FALSE);
    NotifyWinEvent(EVENT_OBJECT_STATECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), reinterpret_cast<LPARAM>(h));
}

void CheckPaint(HWND h, CheckData* d) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    const Theme& c = Colors();
    bool enabled = IsWindowEnabled(h) != 0, focus = GetFocus() == h, on = d->state == BST_CHECKED;
    int dpi = static_cast<int>(GetDpiForWindow(h)), sz = MulDiv(18, dpi, 96);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(mem, bmp);
    FillRect(mem, &rc, BgBrush());
    RECT b{0, (rc.bottom - sz) / 2, sz, (rc.bottom - sz) / 2 + sz};
    COLORREF edge = enabled ? (d->hot ? c.accent : c.dark ? c.muted : c.text) : c.disabled;
    if (on) FillRoundRectAA(mem, b, MulDiv(5, dpi, 96), enabled ? c.accent : c.disabled);
    else FillRoundRectAA(mem, b, MulDiv(5, dpi, 96), c.panel, edge, 1.6f * dpi / 96.f);
    if (on) DrawCheckMarkAA(mem, b, c.dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
    wchar_t text[256] = L"";
    GetWindowTextW(h, text, 256);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, enabled ? c.text : c.disabled);
    HGDIOBJ of = SelectObject(mem, d->font ? d->font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    RECT tr{sz + MulDiv(10, dpi, 96), 0, rc.right, rc.bottom};
    if (focus) {  // keyboard focus: accent outline around the label
        RECT f = tr;
        DrawTextW(mem, text, -1, &f, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        int fh = f.bottom - f.top;
        RECT fr{tr.left - 5, (rc.bottom - fh) / 2 - 3, std::min<LONG>(tr.left + (f.right - f.left) + 5, rc.right - 1), (rc.bottom + fh) / 2 + 3};
        FillRoundRectAA(mem, fr, MulDiv(6, dpi, 96), c.bg, c.accent, 1.5f);
    }
    DrawTextW(mem, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, of);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

// SSCheck: reports "check box", the checked state and the default action "Toggle".
class CheckAcc : public DelegatingAcc {
public:
    using DelegatingAcc::DelegatingAcc;
    STDMETHODIMP get_accName(VARIANT v, BSTR* s) override {
        wchar_t t[256] = L"";
        GetWindowTextW(h_, t, 256);
        return Self(v) && *t ? Str(t, s) : in_->get_accName(v, s);
    }
    STDMETHODIMP get_accRole(VARIANT v, VARIANT* r) override {
        if (!Self(v)) return in_->get_accRole(v, r);
        r->vt = VT_I4;
        r->lVal = ROLE_SYSTEM_CHECKBUTTON;
        return S_OK;
    }
    STDMETHODIMP get_accState(VARIANT v, VARIANT* s) override {
        HRESULT hr = in_->get_accState(v, s);
        auto* d = reinterpret_cast<CheckData*>(GetWindowLongPtrW(h_, GWLP_USERDATA));
        if (SUCCEEDED(hr) && Self(v) && s->vt == VT_I4 && d && d->state == BST_CHECKED) s->lVal |= STATE_SYSTEM_CHECKED;
        return hr;
    }
    STDMETHODIMP get_accDefaultAction(VARIANT v, BSTR* s) override { return Self(v) ? Str(L"Toggle", s) : in_->get_accDefaultAction(v, s); }
    STDMETHODIMP accDoDefaultAction(VARIANT v) override {
        if (!Self(v)) return in_->accDoDefaultAction(v);
        PostMessageW(h_, WM_KEYUP, VK_SPACE, 0);  // toggles on the UI thread
        return S_OK;
    }
};

LRESULT CALLBACK CheckProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<CheckData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_NCCREATE:  // DefWindowProc (below) stores the window text, which is the label
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new CheckData));
            break;
        case WM_NCDESTROY:
            delete d;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            if (d) CheckPaint(h, d);
            return 0;
        case WM_GETOBJECT:
            if (static_cast<LONG>(l) == OBJID_CLIENT) {
                IAccessible* inner = nullptr;
                if (SUCCEEDED(CreateStdAccessibleObject(h, OBJID_CLIENT, IID_PPV_ARGS(&inner)))) {
                    auto* acc = new CheckAcc(h, inner);
                    LRESULT lr = LresultFromObject(IID_IAccessible, w, acc);
                    acc->Release();
                    return lr;
                }
            }
            break;
        case WM_SETFONT:
            if (d) d->font = reinterpret_cast<HFONT>(w);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETFONT: return d ? reinterpret_cast<LRESULT>(d->font) : 0;
        case BM_SETCHECK:
            if (d) d->state = w == BST_CHECKED ? BST_CHECKED : BST_UNCHECKED;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case BM_GETCHECK: return d ? d->state : BST_UNCHECKED;
        case WM_GETDLGCODE: return DLGC_BUTTON;
        case WM_ENABLE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (d && !d->hot) {
                d->hot = true;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (d) d->hot = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(h);
            if (d) d->down = true;
            SetCapture(h);
            return 0;
        case WM_LBUTTONUP: {
            bool fire = d && d->down;
            if (d) d->down = false;
            if (GetCapture() == h) ReleaseCapture();
            RECT rc;
            GetClientRect(h, &rc);
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (fire && PtInRect(&rc, p) && IsWindowEnabled(h)) CheckToggle(h, d);
            return 0;
        }
        case WM_KEYUP:
            if (d && w == VK_SPACE && IsWindowEnabled(h)) CheckToggle(h, d);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- SSInfo: a small round "i" icon whose window text is shown as a tooltip on hover, click or keyboard focus. Used instead of
// long hint sentences. Change the tip with SetWindowText.
struct InfoData {
    HWND tip = nullptr;
    bool hot = false;
    HFONT font = nullptr;
    std::wstring text;  // what the tooltip shows right now (kept alive for TTN_GETDISPINFO)
};

void InfoColors(HWND tip) {
    const Theme& c = Colors();
    SetWindowTheme(tip, L"", L"");  // a themed tooltip ignores custom colours
    SendMessageW(tip, TTM_SETTIPBKCOLOR, c.dark ? RGB(44, 44, 50) : RGB(252, 252, 252), 0);
    SendMessageW(tip, TTM_SETTIPTEXTCOLOR, c.text, 0);
}

class InfoAcc : public DelegatingAcc {
public:
    using DelegatingAcc::DelegatingAcc;
    STDMETHODIMP get_accName(VARIANT v, BSTR* s) override {
        wchar_t t[512] = L"";
        GetWindowTextW(h_, t, 512);
        return Self(v) && *t ? Str(t, s) : in_->get_accName(v, s);
    }
    STDMETHODIMP get_accRole(VARIANT v, VARIANT* r) override {
        if (!Self(v)) return in_->get_accRole(v, r);
        r->vt = VT_I4;
        r->lVal = ROLE_SYSTEM_GRAPHIC;
        return S_OK;
    }
};

LRESULT CALLBACK InfoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<InfoData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_NCCREATE:  // DefWindowProc (below) stores the window text
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new InfoData));
            break;
        case WM_CREATE: {
            d = reinterpret_cast<InfoData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            d->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, h, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
            TOOLINFOW ti{sizeof ti};
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = h;
            ti.uId = reinterpret_cast<UINT_PTR>(h);
            ti.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(d->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            SendMessageW(d->tip, TTM_SETMAXTIPWIDTH, 0, MulDiv(420, static_cast<int>(GetDpiForWindow(h)), 96));
            SendMessageW(d->tip, TTM_SETDELAYTIME, TTDT_INITIAL, 250);
            SendMessageW(d->tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
            InfoColors(d->tip);
            return 0;
        }
        case WM_NCDESTROY:
            if (d) {
                if (d->tip) DestroyWindow(d->tip);
                delete d;
            }
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case WM_APP_INFO_THEME:  // the app theme changed
            if (d && d->tip) InfoColors(d->tip);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_NOTIFY:
            if (d && reinterpret_cast<NMHDR*>(l)->code == TTN_GETDISPINFOW) {
                wchar_t buf[1024] = L"";
                GetWindowTextW(h, buf, 1024);
                d->text = buf;
                reinterpret_cast<NMTTDISPINFOW*>(l)->lpszText = d->text.data();
                return 0;
            }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            const Theme& c = Colors();
            bool focus = GetFocus() == h, on = d && d->hot;
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ ob = SelectObject(mem, bmp);
            FillRect(mem, &rc, BgBrush());
            RECT ring = rc;
            InflateRect(&ring, -1, -1);
            int rad = (ring.right - ring.left) / 2;
            FillRoundRectAA(mem, ring, rad, on || focus ? c.accentSoft : c.bg, c.accent, focus ? 2.f : 1.4f);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, c.accent);
            HGDIOBJ of = SelectObject(mem, d && d->font ? d->font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
            DrawTextW(mem, L"i", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(mem, of);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_GETOBJECT:
            if (static_cast<LONG>(l) == OBJID_CLIENT) {
                IAccessible* inner = nullptr;
                if (SUCCEEDED(CreateStdAccessibleObject(h, OBJID_CLIENT, IID_PPV_ARGS(&inner)))) {
                    auto* acc = new InfoAcc(h, inner);
                    LRESULT lr = LresultFromObject(IID_IAccessible, w, acc);
                    acc->Release();
                    return lr;
                }
            }
            break;
        case WM_SETFONT:
            if (d) d->font = reinterpret_cast<HFONT>(w);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETFONT: return d ? reinterpret_cast<LRESULT>(d->font) : 0;
        case WM_GETDLGCODE: return DLGC_STATIC;
        case WM_MOUSEMOVE:
            if (d && !d->hot) {
                d->hot = true;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (d) d->hot = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:  // click or tap also opens the tip (no hover on touch)
            SetFocus(h);
            if (d && d->tip) SendMessageW(d->tip, TTM_POPUP, 0, 0);
            return 0;
        case WM_SETFOCUS:  // keyboard users get the tip when they tab to the icon
            if (d && d->tip && GetKeyState(VK_LBUTTON) >= 0) SendMessageW(d->tip, TTM_POPUP, 0, 0);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            if (d && d->tip) SendMessageW(d->tip, TTM_POP, 0, 0);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

int FlowButtons(const FlowItem* items, int count, int margin, int width, int top, int btnH, int gap, bool place, bool show, POINT* end) {
    int x = margin, y = top, rows = 1;
    for (int i = 0; i < count; ++i) {
        if (x + items[i].w > width - margin && x > margin) {
            x = margin;
            y += btnH + gap;
            ++rows;
        }
        if (place) {
            MoveWindow(items[i].h, x, y, items[i].w, btnH, TRUE);
            ShowWindow(items[i].h, show ? SW_SHOW : SW_HIDE);
        }
        x += items[i].w + gap;
    }
    if (end) *end = {x, y};
    return rows;
}

LRESULT HeaderCustomDraw(LPARAM l) {
    auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(l);
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        wchar_t text[64] = L"";
        HDITEMW it{HDI_TEXT | HDI_FORMAT};
        it.pszText = text;
        it.cchTextMax = 64;
        Header_GetItem(cd->hdr.hwndFrom, static_cast<int>(cd->dwItemSpec), &it);
        const Theme& c = Colors();
        FillRect(cd->hdc, &cd->rc, PanelBrush());
        HPEN pen = CreatePen(PS_SOLID, 1, c.border);
        HGDIOBJ old = SelectObject(cd->hdc, pen);
        MoveToEx(cd->hdc, cd->rc.right - 1, cd->rc.top + 3, nullptr);
        LineTo(cd->hdc, cd->rc.right - 1, cd->rc.bottom - 3);
        MoveToEx(cd->hdc, cd->rc.left, cd->rc.bottom - 1, nullptr);
        LineTo(cd->hdc, cd->rc.right, cd->rc.bottom - 1);
        SelectObject(cd->hdc, old);
        DeleteObject(pen);
        SetBkMode(cd->hdc, TRANSPARENT);
        SetTextColor(cd->hdc, c.text);
        RECT tr = cd->rc;
        tr.left += 8;
        tr.right -= 8;
        bool right = (it.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT;
        std::wstring t = text;
        if (it.fmt & HDF_SORTUP) t += L" ▴";
        else if (it.fmt & HDF_SORTDOWN) t += L" ▾";
        DrawTextW(cd->hdc, t.c_str(), -1, &tr, (right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

// A virtual list view does not draw its own check boxes reliably, so the tick is painted over the (empty) box: accent square + check.
int HitListTick(HWND list, POINT ptClient) {
    LVHITTESTINFO ht{};
    ht.pt = ptClient;
    int i = ListView_HitTest(list, &ht);
    RECT icon{};
    if (i < 0 || !ListView_GetItemRect(list, i, &icon, LVIR_ICON)) return -1;
    return ptClient.x < icon.left ? i : -1;  // the box sits left of the file icon
}

namespace {
void PaintTick(HDC dc, const RECT& b, int sz, bool on) {
    const Theme& c = Colors();
    int rad = std::max(2, sz / 5);
    if (on) {
        FillRoundRectAA(dc, b, rad, c.accent);
        DrawCheckMarkAA(dc, b, c.dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
    } else {
        FillRoundRectAA(dc, b, rad, c.panel, c.dark ? c.muted : c.text, std::max(1.5f, sz / 10.f));
    }
}
}  // namespace

void DrawListTick(HWND list, HDC dc, int item, bool on) {
    RECT icon{};
    if (!ListView_GetItemRect(list, item, &icon, LVIR_ICON)) return;
    int dpi = GetDeviceCaps(dc, LOGPIXELSX), sz = MulDiv(14, dpi, 96), gap = MulDiv(3, dpi, 96);
    RECT b{icon.left - gap - sz, (icon.top + icon.bottom - sz) / 2, icon.left - gap, (icon.top + icon.bottom + sz) / 2};
    PaintTick(dc, b, sz, on);
}

// For lists that keep the native check box (no icon column): the box is painted over the native one at the left edge of the row.
void DrawListTickNative(HWND list, HDC dc, int item, bool on) {
    RECT row{};
    if (!ListView_GetItemRect(list, item, &row, LVIR_BOUNDS)) return;
    int dpi = GetDeviceCaps(dc, LOGPIXELSX), sz = MulDiv(14, dpi, 96);
    int left = row.left + MulDiv(2, dpi, 96);
    RECT b{left, (row.top + row.bottom - sz) / 2, left + sz, (row.top + row.bottom + sz) / 2};
    RECT cover = b;
    InflateRect(&cover, MulDiv(2, dpi, 96), MulDiv(2, dpi, 96));
    FillRect(dc, &cover, PanelBrush());  // hides the native box (drawn slightly larger than ours)
    PaintTick(dc, b, sz, on);
}

// One accent colour choice in Settings: a filled circle, with a ring and a check mark on the current one.
void DrawAccentSwatch(const DRAWITEMSTRUCT* d) {
    int i = static_cast<int>(d->CtlID) - kAccentSwatchBase;
    const Theme& c = Colors();
    HDC dc = d->hDC;
    FillRect(dc, &d->rcItem, BgBrush());
    RECT r = d->rcItem;
    int pad = std::max<int>(3, (r.right - r.left) / 8);
    bool cur = i == AccentIndex(), focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    if (focus) FillRoundRectAA(dc, r, (r.right - r.left) / 2, c.bg, c.accent, 1.5f);  // keyboard focus ring
    InflateRect(&r, -pad, -pad);
    FillRoundRectAA(dc, r, (r.right - r.left) / 2, AccentColor(i, c.dark), cur ? c.text : kNoColor, 2.5f);
    if (cur) {
        InflateRect(&r, -(r.right - r.left) / 5, -(r.right - r.left) / 5);
        DrawCheckMarkAA(dc, r, c.dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
    }
}

void DrawFlatButton(const DRAWITEMSTRUCT* d) {
    if (d->CtlID >= static_cast<UINT>(kAccentSwatchBase) && d->CtlID < static_cast<UINT>(kAccentSwatchBase + AccentCount())) {
        DrawAccentSwatch(d);
        return;
    }
    wchar_t text[96] = L"";
    GetWindowTextW(d->hwndItem, text, 96);
    std::wstring t = text;
    bool disabled = (d->itemState & ODS_DISABLED) != 0, pressed = (d->itemState & ODS_SELECTED) != 0;
    bool primary = t.rfind(L"Scan", 0) == 0;
    bool danger = t.find(L"Delete") != std::wstring::npos || t.find(L"Permanent") != std::wstring::npos;
    POINT cp;
    GetCursorPos(&cp);
    ScreenToClient(d->hwndItem, &cp);
    bool hot = !disabled && PtInRect(&d->rcItem, cp);
    const Theme& c = Colors();

    COLORREF bg = primary ? (pressed ? Mix(c.accent, RGB(0, 0, 0), c.dark ? 0.25 : 0.4) : hot ? (c.dark ? Mix(c.accent, RGB(255, 255, 255), 0.25) : Mix(c.accent, RGB(0, 0, 0), 0.2)) : c.accent)
                          : (pressed ? c.pressed : hot ? c.hover : c.panel);
    COLORREF fg = primary ? (c.dark ? RGB(0, 28, 20) : RGB(255, 255, 255)) : danger ? c.danger : c.text;
    COLORREF border = primary ? bg : c.border;
    if (t == L"Clean Selected" || t == L"Empty Recycle Bin") {  // mild red accent for cleanup actions
        bg = c.dark ? (pressed ? RGB(78, 28, 26) : hot ? RGB(62, 22, 20) : RGB(44, 16, 14))
                    : (pressed ? RGB(247, 205, 200) : hot ? RGB(251, 222, 218) : RGB(253, 236, 234));
        border = c.dark ? RGB(130, 52, 48) : RGB(232, 168, 162);
        fg = c.dark ? RGB(255, 128, 116) : RGB(176, 48, 38);
    }
    if (disabled) {
        bg = c.bg;
        fg = c.disabled;
        border = c.border;
    }
    HDC dc = d->hDC;
    FillRect(dc, &d->rcItem, BgBrush());
    UINT dpi = GetDpiForWindow(d->hwndItem);
    int r = MulDiv(8, static_cast<int>(dpi), 96);
    FillRoundRectAA(dc, d->rcItem, r / 2, bg, border);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    HGDIOBJ of = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(d->hwndItem, WM_GETFONT, 0, 0)));
    RECT tr = d->rcItem;
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = d->rcItem;
        InflateRect(&f, -3, -3);
        DrawFocusRect(dc, &f);
    }
}

bool View::Create(HWND parent, MainWindow* mw) {
    mw_ = mw;
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DBLCLKS;  // the treemap opens folders on double-click
        wc.lpfnWndProc = Proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SSView";
        RegisterClassExW(&wc);
        WNDCLASSEXW cc{sizeof cc};
        cc.lpfnWndProc = ComboProc;
        cc.hInstance = hi;
        cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cc.lpszClassName = L"SSCombo";
        RegisterClassExW(&cc);
        WNDCLASSEXW kc{sizeof kc};
        kc.lpfnWndProc = CheckProc;
        kc.hInstance = hi;
        kc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        kc.lpszClassName = L"SSCheck";
        RegisterClassExW(&kc);
        WNDCLASSEXW ic{sizeof ic};
        ic.lpfnWndProc = InfoProc;
        ic.hInstance = hi;
        ic.hCursor = LoadCursorW(nullptr, IDC_HAND);
        ic.lpszClassName = L"SSInfo";
        RegisterClassExW(&ic);
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"SSView", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                            parent, nullptr, hi, this);
    if (!hwnd_) return false;
    OnCreate();
    mw_->ApplyFont(hwnd_);
    return true;
}

HWND View::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK View::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    View* v = reinterpret_cast<View*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        v = static_cast<View*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
        v->hwnd_ = h;
    }
    return v ? v->OnMessage(m, w, l) : DefWindowProcW(h, m, w, l);
}

void View::EnableScroll() {
    scrollable_ = true;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, GetWindowLongPtrW(hwnd_, GWL_STYLE) | WS_VSCROLL);  // shown only when the page does not fit
}

bool View::BeginLayout(int w, int h) {
    if (inLayout_) return false;  // SetScrollInfo can resize the client area and re-enter Layout
    inLayout_ = true;
    viewW_ = w;
    viewH_ = h;
    return true;
}

void View::EndLayout(int contentBottom) {
    if (!scrollable_) {
        inLayout_ = false;
        return;
    }
    contentH_ = contentBottom + scrollY_ + mw_->S(20);
    int maxY = std::max(0, contentH_ - viewH_);
    if (scrollY_ > maxY) {  // the window grew: clamp and lay out again
        scrollY_ = maxY;
        inLayout_ = false;
        Layout(viewW_, viewH_);
        return;
    }
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMin = 0;
    si.nMax = contentH_ - 1;
    si.nPage = static_cast<UINT>(viewH_);
    si.nPos = scrollY_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    inLayout_ = false;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right != viewW_ && retry_++ < 2) Layout(rc.right, rc.bottom);  // the scrollbar appeared/disappeared
    retry_ = 0;
}

void View::ScrollTo(int y) {
    y = std::clamp(y, 0, std::max(0, contentH_ - viewH_));
    if (y == scrollY_) return;
    scrollY_ = y;
    Layout(viewW_, viewH_);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

LRESULT View::OnMessage(UINT m, WPARAM w, LPARAM l) {
    if (scrollable_ && m == WM_VSCROLL) {
        SCROLLINFO si{sizeof si, SIF_ALL};
        GetScrollInfo(hwnd_, SB_VERT, &si);
        int y = si.nPos;
        switch (LOWORD(w)) {
            case SB_LINEUP: y -= mw_->S(30); break;
            case SB_LINEDOWN: y += mw_->S(30); break;
            case SB_PAGEUP: y -= static_cast<int>(si.nPage); break;
            case SB_PAGEDOWN: y += static_cast<int>(si.nPage); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: y = si.nTrackPos; break;
            case SB_TOP: y = 0; break;
            case SB_BOTTOM: y = contentH_; break;
        }
        ScrollTo(y);
        return 0;
    }
    if (scrollable_ && m == WM_MOUSEWHEEL) {
        ScrollTo(scrollY_ - GET_WHEEL_DELTA_WPARAM(w) * mw_->S(60) / WHEEL_DELTA);
        return 0;
    }
    switch (m) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) Layout(LOWORD(l), HIWORD(l));
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, BgBrush());
            return 1;
        }
        case WM_DRAWITEM:
            if (l && reinterpret_cast<DRAWITEMSTRUCT*>(l)->CtlType == ODT_BUTTON) {
                DrawFlatButton(reinterpret_cast<DRAWITEMSTRUCT*>(l));
                return TRUE;
            }
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, Colors().text);
            SetBkColor(dc, Colors().panel);
            return reinterpret_cast<LRESULT>(PanelBrush());
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, Colors().text);
            SetBkColor(dc, Colors().bg);
            return reinterpret_cast<LRESULT>(BgBrush());
        }
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

}  // namespace ss

namespace ss {
void FitColumns(HWND list, const int* base, int n, int avail) {
    int total = 0;
    for (int i = 0; i < n; ++i) total += base[i];
    // Leave room for the vertical scrollbar so the last column is not clipped.
    avail -= GetSystemMetrics(SM_CXVSCROLL);
    if (total <= 0 || avail <= 0) return;
    for (int i = 0; i < n; ++i) ListView_SetColumnWidth(list, i, base[i] * avail / total);
}
}  // namespace ss
