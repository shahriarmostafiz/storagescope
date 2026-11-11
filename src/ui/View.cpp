#include "ui/View.h"

#include <commctrl.h>
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
// Accent: emerald / teal
const Theme kLight{RGB(243, 247, 246), RGB(255, 255, 255), RGB(32, 32, 32),    RGB(96, 96, 96),    RGB(0, 133, 107),
                   RGB(206, 241, 231), RGB(196, 43, 28),   RGB(15, 123, 15),    RGB(222, 230, 228), RGB(228, 234, 232),
                   RGB(246, 250, 249), RGB(226, 234, 232), RGB(165, 165, 165),  RGB(158, 222, 204), RGB(104, 200, 174),
                   RGB(255, 255, 255), RGB(234, 249, 244), RGB(200, 228, 220),  RGB(220, 244, 237), RGB(244, 251, 249), false};
// Pure black (OLED style): backgrounds are 0,0,0; only cards/hover states are lifted slightly for contrast.
const Theme kDark{RGB(0, 0, 0),       RGB(0, 0, 0),    RGB(240, 240, 242), RGB(150, 152, 158), RGB(52, 211, 163),
                  RGB(8, 50, 40),     RGB(255, 108, 96), RGB(96, 205, 96),   RGB(44, 44, 48),    RGB(34, 34, 36),
                  RGB(26, 26, 30),    RGB(44, 44, 50), RGB(100, 102, 108), RGB(14, 88, 70),    RGB(30, 128, 104),
                  RGB(22, 22, 26),    RGB(8, 8, 10),   RGB(52, 54, 62),    RGB(0, 0, 0),       RGB(0, 0, 0), true};
const Theme* g_theme = &kLight;
HBRUSH g_bg = nullptr, g_panel = nullptr;
}  // namespace

const Theme& Colors() { return *g_theme; }
bool IsDarkTheme() { return g_theme->dark; }
void SetDarkTheme(bool dark) {
    g_theme = dark ? &kDark : &kLight;
    if (g_bg) DeleteObject(g_bg);
    if (g_panel) DeleteObject(g_panel);
    g_bg = CreateSolidBrush(g_theme->bg);
    g_panel = CreateSolidBrush(g_theme->panel);
}
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
    HBRUSH b = CreateSolidBrush(d->hot && enabled ? c.hover : c.panel);
    HPEN p = CreatePen(PS_SOLID, focus ? 2 : 1, focus ? c.accent : c.border);
    HGDIOBJ obr = SelectObject(mem, b), op = SelectObject(mem, p);
    RoundRect(mem, rc.left, rc.top, rc.right, rc.bottom, r, r);
    SelectObject(mem, obr);
    SelectObject(mem, op);
    DeleteObject(b);
    DeleteObject(p);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, enabled ? c.text : c.disabled);
    HGDIOBJ of = SelectObject(mem, d->font ? d->font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    int pad = MulDiv(12, static_cast<int>(GetDpiForWindow(h)), 96);
    RECT tr{pad, 0, rc.right - 3 * pad, rc.bottom};
    if (d->sel >= 0) DrawTextW(mem, d->items[static_cast<size_t>(d->sel)].c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, of);
    // chevron
    int cx = rc.right - pad - 4, cy = rc.bottom / 2, s = MulDiv(4, static_cast<int>(GetDpiForWindow(h)), 96);
    HPEN cp = CreatePen(PS_SOLID, 2, enabled ? c.muted : c.disabled);
    HGDIOBJ ocp = SelectObject(mem, cp);
    MoveToEx(mem, cx - s, cy - s / 2, nullptr);
    LineTo(mem, cx, cy + s / 2);
    LineTo(mem, cx + s + 1, cy - s / 2 - 1);
    SelectObject(mem, ocp);
    DeleteObject(cp);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

// MSAA wrapper: the standard object for the window, but reporting role "combo box", the selected text as value and a
// default action, so screen readers treat SSCombo like a native drop-down. Everything else is delegated.
class ComboAcc : public IAccessible {
public:
    ComboAcc(HWND h, IAccessible* inner) : h_(h), in_(inner) {}
    ~ComboAcc() { in_->Release(); }
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
    STDMETHODIMP get_accValue(VARIANT v, BSTR* s) override {
        auto* d = reinterpret_cast<ComboData*>(GetWindowLongPtrW(h_, GWLP_USERDATA));
        if (v.lVal == CHILDID_SELF && d && d->sel >= 0) {
            *s = SysAllocString(d->items[static_cast<size_t>(d->sel)].c_str());
            return *s ? S_OK : E_OUTOFMEMORY;
        }
        return in_->get_accValue(v, s);
    }
    STDMETHODIMP get_accDescription(VARIANT v, BSTR* s) override { return in_->get_accDescription(v, s); }
    STDMETHODIMP get_accRole(VARIANT v, VARIANT* r) override {
        if (v.lVal != CHILDID_SELF) return in_->get_accRole(v, r);
        r->vt = VT_I4;
        r->lVal = ROLE_SYSTEM_COMBOBOX;
        return S_OK;
    }
    STDMETHODIMP get_accState(VARIANT v, VARIANT* s) override { return in_->get_accState(v, s); }
    STDMETHODIMP get_accHelp(VARIANT v, BSTR* s) override { return in_->get_accHelp(v, s); }
    STDMETHODIMP get_accHelpTopic(BSTR* f, VARIANT v, long* t) override { return in_->get_accHelpTopic(f, v, t); }
    STDMETHODIMP get_accKeyboardShortcut(VARIANT v, BSTR* s) override { return in_->get_accKeyboardShortcut(v, s); }
    STDMETHODIMP get_accFocus(VARIANT* v) override { return in_->get_accFocus(v); }
    STDMETHODIMP get_accSelection(VARIANT* v) override { return in_->get_accSelection(v); }
    STDMETHODIMP get_accDefaultAction(VARIANT v, BSTR* s) override {
        if (v.lVal != CHILDID_SELF) return in_->get_accDefaultAction(v, s);
        *s = SysAllocString(L"Open");
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP accSelect(long f, VARIANT v) override { return in_->accSelect(f, v); }
    STDMETHODIMP accLocation(long* x, long* y, long* w, long* hh, VARIANT v) override { return in_->accLocation(x, y, w, hh, v); }
    STDMETHODIMP accNavigate(long d, VARIANT v, VARIANT* e) override { return in_->accNavigate(d, v, e); }
    STDMETHODIMP accHitTest(long x, long y, VARIANT* v) override { return in_->accHitTest(x, y, v); }
    STDMETHODIMP accDoDefaultAction(VARIANT v) override {
        if (v.lVal != CHILDID_SELF) return in_->accDoDefaultAction(v);
        PostMessageW(h_, WM_KEYDOWN, VK_F4, 0);  // opens the list on the UI thread
        return S_OK;
    }
    STDMETHODIMP put_accName(VARIANT v, BSTR s) override { return in_->put_accName(v, s); }
    STDMETHODIMP put_accValue(VARIANT v, BSTR s) override { return in_->put_accValue(v, s); }

private:
    LONG ref_ = 1;
    HWND h_;
    IAccessible* in_;
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
}  // namespace

LRESULT HeaderCustomDraw(LPARAM l) {
    auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(l);
    if (!Colors().dark) return CDRF_DODEFAULT;  // light theme: the system header is fine
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
        SelectObject(cd->hdc, old);
        DeleteObject(pen);
        SetBkMode(cd->hdc, TRANSPARENT);
        SetTextColor(cd->hdc, c.text);
        RECT tr = cd->rc;
        tr.left += 8;
        tr.right -= 8;
        bool right = (it.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT;
        std::wstring t = text;
        if (it.fmt & HDF_SORTUP) t += L" ▲";
        else if (it.fmt & HDF_SORTDOWN) t += L" ▼";
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

void DrawListTick(HWND list, HDC dc, int item, bool on) {
    RECT icon{};
    if (!ListView_GetItemRect(list, item, &icon, LVIR_ICON)) return;
    int dpi = GetDeviceCaps(dc, LOGPIXELSX), sz = MulDiv(14, dpi, 96), gap = MulDiv(3, dpi, 96);
    RECT b{icon.left - gap - sz, (icon.top + icon.bottom - sz) / 2, icon.left - gap, (icon.top + icon.bottom + sz) / 2};
    const Theme& c = Colors();
    HBRUSH br = CreateSolidBrush(on ? c.accent : c.panel);
    FillRect(dc, &b, br);
    DeleteObject(br);
    if (!on) {  // empty box
        HBRUSH fr = CreateSolidBrush(c.muted);
        FrameRect(dc, &b, fr);
        DeleteObject(fr);
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, std::max(2, sz / 7), c.dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, b.left + sz / 5, b.top + sz / 2, nullptr);
    LineTo(dc, b.left + sz * 2 / 5, b.bottom - sz / 4);
    LineTo(dc, b.right - sz / 5, b.top + sz / 4);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void DrawFlatButton(const DRAWITEMSTRUCT* d) {
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

    COLORREF bg = primary ? (pressed ? RGB(0, 96, 78) : hot ? RGB(0, 114, 92) : c.accent) : (pressed ? c.pressed : hot ? c.hover : c.panel);
    if (primary && c.dark) bg = pressed ? RGB(36, 168, 130) : hot ? RGB(100, 232, 190) : c.accent;
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
    HBRUSH b = CreateSolidBrush(bg);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, r, r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
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
