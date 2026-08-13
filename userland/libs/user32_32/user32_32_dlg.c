/*
 * userland/libs/user32_32/user32_32_dlg.c
 *
 * The dialog-item surface, the enabled-state pair, and the odds and
 * ends of the i386 (PE32) user32 companion that did not belong with
 * the window lifecycle (user32_32.c), the WM shims
 * (user32_32_misc.c), or the computational helpers
 * (user32_32_util.c).
 *
 * WHAT IS AND IS NOT REAL HERE — read before extending.
 *
 * A Windows dialog has two halves. The first is a resource-driven
 * one: DialogBoxParam / CreateDialogParam take a template out of the
 * PE's `.rsrc` section, instantiate every control the template
 * describes, and run a modal loop against them. DuetOS has NO PE
 * resource parser — nothing in kernel/loader or userland/libs walks
 * IMAGE_DIRECTORY_ENTRY_RESOURCE — so that half cannot be
 * implemented honestly and every entry point belonging to it carries
 * a STUB marker and returns the documented failure.
 *
 * The second half is the item accessors: GetDlgItem, SetDlgItemText,
 * SendDlgItemMessage, CheckDlgButton and friends. Those are defined
 * purely in terms of "find the child window with this control id,
 * then do a normal window operation on it" — no resource data
 * involved. That works today, because CreateWindowEx records a
 * child's parent and its control id (the `hMenu` argument, per the
 * WS_CHILD convention) in the per-window record table. So an
 * application that builds its dialog by calling CreateWindowEx for
 * each control — which is exactly what code written without a
 * resource compiler does — gets a REAL item surface.
 *
 * The dividing line, stated once so a future reader does not have to
 * re-derive it: anything that needs a TEMPLATE is stubbed; anything
 * that needs only a CONTROL ID is real.
 */

#include "user32_32_internal.h"

/* Defined in the sibling TUs; declared here rather than exported
 * through the internal header because these are the public Win32
 * entry points, not internal plumbing. */
LRESULT __stdcall SendMessageA(HWND h, UINT msg, WPARAM w, LPARAM l);
BOOL __stdcall SetWindowTextA(HWND h, const char* text);
BOOL __stdcall PostMessageA(HWND h, UINT msg, WPARAM w, LPARAM l);
int __stdcall GetWindowTextA(HWND h, char* buf, int len);
int __stdcall GetWindowTextW(HWND h, wchar_t16* buf, int len);
INT __stdcall GetSystemMetrics(int index);

/* Button control messages / states (winuser.h). */
#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1

/* ------------------------------------------------------------------
 * Dialog items — real, keyed on control id
 * ------------------------------------------------------------------ */

__declspec(dllexport) HWND __stdcall GetDlgItem(HWND dlg, int id)
{
    return user32_record_child_by_id(dlg, id);
}

__declspec(dllexport) int __stdcall GetDlgCtrlID(HWND h)
{
    return user32_record_ctrl_id(h);
}

__declspec(dllexport) BOOL __stdcall SetDlgItemTextA(HWND dlg, int id, const char* text)
{
    HWND item = GetDlgItem(dlg, id);
    if (!item)
        return 0;
    return SetWindowTextA(item, text);
}

__declspec(dllexport) BOOL __stdcall SetDlgItemTextW(HWND dlg, int id, const wchar_t16* text)
{
    char flat[WIN_TITLE_MAX];
    user32_w_to_ascii(text, flat, WIN_TITLE_MAX);
    return SetDlgItemTextA(dlg, id, flat);
}

__declspec(dllexport) UINT __stdcall GetDlgItemTextA(HWND dlg, int id, char* buf, int cap)
{
    HWND item = GetDlgItem(dlg, id);
    if (!item)
    {
        if (buf && cap > 0)
            buf[0] = '\0';
        return 0;
    }
    return (UINT)GetWindowTextA(item, buf, cap);
}

__declspec(dllexport) UINT __stdcall GetDlgItemTextW(HWND dlg, int id, wchar_t16* buf, int cap)
{
    HWND item = GetDlgItem(dlg, id);
    if (!item)
    {
        if (buf && cap > 0)
            buf[0] = 0;
        return 0;
    }
    return (UINT)GetWindowTextW(item, buf, cap);
}

__declspec(dllexport) LRESULT __stdcall SendDlgItemMessageA(HWND dlg, int id, UINT msg, WPARAM w, LPARAM l)
{
    HWND item = GetDlgItem(dlg, id);
    return item ? SendMessageA(item, msg, w, l) : 0;
}

__declspec(dllexport) LRESULT __stdcall SendDlgItemMessageW(HWND dlg, int id, UINT msg, WPARAM w, LPARAM l)
{
    return SendDlgItemMessageA(dlg, id, msg, w, l);
}

__declspec(dllexport) BOOL __stdcall CheckDlgButton(HWND dlg, int id, UINT check)
{
    HWND item = GetDlgItem(dlg, id);
    if (!item)
        return 0;
    (void)SendMessageA(item, BM_SETCHECK, (WPARAM)check, 0);
    return 1;
}

__declspec(dllexport) UINT __stdcall IsDlgButtonChecked(HWND dlg, int id)
{
    HWND item = GetDlgItem(dlg, id);
    if (!item)
        return 0;
    return (UINT)SendMessageA(item, BM_GETCHECK, 0, 0);
}

__declspec(dllexport) BOOL __stdcall CheckRadioButton(HWND dlg, int first, int last, int check)
{
    if (first > last)
        return 0;
    for (int id = first; id <= last; ++id)
        (void)CheckDlgButton(dlg, id, (UINT)((id == check) ? 1 : 0));
    return 1;
}

__declspec(dllexport) BOOL __stdcall SetDlgItemInt(HWND dlg, int id, UINT value, BOOL is_signed)
{
    /* Format in place — no CRT dependency, and the buffer is sized
     * for the widest 32-bit decimal plus sign and terminator. */
    char buf[12];
    unsigned n = value;
    int neg = 0;
    if (is_signed && (int)value < 0)
    {
        neg = 1;
        n = (unsigned)(-(int)value);
    }
    unsigned i = sizeof(buf);
    buf[--i] = '\0';
    do
    {
        buf[--i] = (char)('0' + (n % 10u));
        n /= 10u;
    } while (n && i > 1);
    if (neg && i > 0)
        buf[--i] = '-';
    return SetDlgItemTextA(dlg, id, &buf[i]);
}

__declspec(dllexport) UINT __stdcall GetDlgItemInt(HWND dlg, int id, BOOL* translated, BOOL is_signed)
{
    char buf[16];
    const UINT len = GetDlgItemTextA(dlg, id, buf, (int)sizeof(buf));
    if (translated)
        *translated = 0;
    if (len == 0)
        return 0;
    unsigned i = 0;
    int neg = 0;
    if (is_signed && buf[0] == '-')
    {
        neg = 1;
        i = 1;
    }
    unsigned value = 0;
    unsigned digits = 0;
    for (; buf[i]; ++i)
    {
        if (buf[i] < '0' || buf[i] > '9')
            return 0;
        value = value * 10u + (unsigned)(buf[i] - '0');
        ++digits;
    }
    if (digits == 0)
        return 0;
    if (translated)
        *translated = 1;
    return neg ? (UINT)(-(int)value) : value;
}

/* ------------------------------------------------------------------
 * Dialog templates — blocked on an RT_DIALOG template decoder
 * ------------------------------------------------------------------ */

// STUB: a dialog template cannot be read, so no controls can be
// instantiated. The rationale that used to sit here -- "no PE resource
// (.rsrc) parser exists anywhere in the tree" -- is stale: the walker
// landed in userland/libs/common/pe_resources.h and this DLL already
// uses it for LoadString / LoadIcon / LoadCursor / LoadBitmap. What is
// still missing is the layer above it: a DLGTEMPLATE / DLGITEMTEMPLATE
// decoder and the control instantiation it drives. Returning -1 is the
/* Resource-template implementation intentionally accepts only resource IDs.
 * The indirect APIs have no byte length, so treating a caller pointer as a
 * bounded template would not meet the malformed-input contract. */
#include "../common/pe_resources.h"
LRESULT __stdcall DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
HWND __stdcall CreateWindowExA(DWORD, const char*, const char*, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
                               void*);
BOOL __stdcall DestroyWindow(HWND);
BOOL __stdcall ShowWindow(HWND, INT);
BOOL __stdcall GetMessageA(struct user32_msg32*, HWND, UINT, UINT);
LRESULT __stdcall DispatchMessageA(const struct user32_msg32*);
typedef INT(__stdcall* DLGPROC32)(HWND, UINT, WPARAM, LPARAM);
#define DLG32_MAX_ITEMS 64u
#define DLG32_WS_CHILD 0x40000000u
#define DLG32_WS_VISIBLE 0x10000000u
#define DLG32_WS_POPUP 0x80000000u
#define DLG32_DS_SETFONT 0x40u
#define DLG32_DS_CENTER 0x0800u
#define DLG32_WM_INITDIALOG 0x0110u
#define DLG32_WM_CLOSE 0x0010u
typedef struct
{
    const unsigned char* p;
    unsigned size, off;
    int ok;
} DLG32_CURSOR;
typedef struct
{
    HWND hwnd;
    INT result;
    int modal, ended, used;
} DLG32_STATE;
static DLG32_STATE s_dlg32_states[4];
static int dlg32_need(DLG32_CURSOR* c, unsigned n)
{
    if (!c->ok || c->off > c->size || n > c->size - c->off)
    {
        c->ok = 0;
        return 0;
    }
    return 1;
}
static unsigned short dlg32_u16(DLG32_CURSOR* c)
{
    unsigned short x;
    if (!dlg32_need(c, 2))
        return 0;
    x = (unsigned short)(c->p[c->off] | ((unsigned short)c->p[c->off + 1] << 8));
    c->off += 2;
    return x;
}
static unsigned dlg32_u32(DLG32_CURSOR* c)
{
    unsigned x;
    if (!dlg32_need(c, 4))
        return 0;
    x = (unsigned)c->p[c->off] | ((unsigned)c->p[c->off + 1] << 8) | ((unsigned)c->p[c->off + 2] << 16) |
        ((unsigned)c->p[c->off + 3] << 24);
    c->off += 4;
    return x;
}
static void dlg32_align(DLG32_CURSOR* c)
{
    unsigned x = (c->off + 3u) & ~3u;
    if (!c->ok || x < c->off || x > c->size)
        c->ok = 0;
    else
        c->off = x;
}
static void dlg32_skip(DLG32_CURSOR* c)
{
    unsigned short x = dlg32_u16(c);
    if (!c->ok || !x)
        return;
    if (x == 0xffffu)
    {
        (void)dlg32_u16(c);
        return;
    }
    while (c->ok && x)
        x = dlg32_u16(c);
}
static void dlg32_text(DLG32_CURSOR* c, char* s, unsigned cap)
{
    unsigned short x;
    unsigned n = 0;
    if (cap)
        s[0] = 0;
    x = dlg32_u16(c);
    if (!c->ok || !x)
        return;
    if (x == 0xffffu)
    {
        (void)dlg32_u16(c);
        return;
    }
    while (c->ok && x)
    {
        if (cap && n + 1 < cap)
            s[n++] = (x < 128) ? (char)x : '?';
        x = dlg32_u16(c);
    }
    if (cap)
        s[n] = 0;
}
/* Normal DLGITEMTEMPLATE has a 16-bit id. DIALOGEX promotes the id to
 * DWORD, but that wire format is rejected at the header discriminator. */
static int dlg32_known_class_ordinal(unsigned short ordinal)
{
    return ordinal >= 0x0080u && ordinal <= 0x0085u;
}
static void dlg32_class_field(DLG32_CURSOR* c)
{
    unsigned short first = dlg32_u16(c);
    if (!c->ok)
        return;
    if (first == 0xffffu)
    {
        if (!dlg32_known_class_ordinal(dlg32_u16(c)))
            c->ok = 0;
        return;
    }
    while (c->ok && first)
        first = dlg32_u16(c);
}
static int dlg32_validate(const void* raw, unsigned size, unsigned short* out)
{
    DLG32_CURSOR c;
    unsigned style, i;
    unsigned short count, extra;
    if (!raw || size < 18)
        return 0;
    if (((const unsigned char*)raw)[0] == 1 && ((const unsigned char*)raw)[1] == 0 &&
        ((const unsigned char*)raw)[2] == 0xff && ((const unsigned char*)raw)[3] == 0xff)
        return 0;
    c.p = raw;
    c.size = size;
    c.off = 0;
    c.ok = 1;
    style = dlg32_u32(&c);
    (void)dlg32_u32(&c);
    count = dlg32_u16(&c);
    if (count > DLG32_MAX_ITEMS)
        return 0;
    (void)dlg32_u16(&c);
    (void)dlg32_u16(&c);
    (void)dlg32_u16(&c);
    (void)dlg32_u16(&c);
    dlg32_skip(&c);
    dlg32_skip(&c);
    dlg32_skip(&c);
    if (style & DLG32_DS_SETFONT)
    {
        (void)dlg32_u16(&c);
        dlg32_skip(&c);
    }
    for (i = 0; c.ok && i < count; i++)
    {
        dlg32_align(&c);
        (void)dlg32_u32(&c);
        (void)dlg32_u32(&c);
        (void)dlg32_u16(&c);
        (void)dlg32_u16(&c);
        (void)dlg32_u16(&c);
        (void)dlg32_u16(&c);
        (void)dlg32_u16(&c);
        dlg32_class_field(&c);
        dlg32_skip(&c);
        extra = dlg32_u16(&c);
        if (dlg32_need(&c, extra))
            c.off += extra;
    }
    if (!c.ok)
        return 0;
    *out = count;
    return 1;
}
static DLG32_STATE* dlg32_state(HWND h)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        if (s_dlg32_states[i].used && s_dlg32_states[i].hwnd == h)
            return &s_dlg32_states[i];
    return 0;
}
void user32_dialog_on_destroy(HWND h)
{
    DLG32_STATE* s = dlg32_state(h);
    if (s)
        s->used = 0;
}
static DLG32_STATE* dlg32_alloc(int modal)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        if (!s_dlg32_states[i].used)
        {
            s_dlg32_states[i].used = 1;
            s_dlg32_states[i].modal = modal;
            s_dlg32_states[i].ended = 0;
            s_dlg32_states[i].result = 0;
            return &s_dlg32_states[i];
        }
    return 0;
}
static const char* dlg32_class(unsigned short x, const char* s)
{
    if (x == 0x80)
        return "BUTTON";
    if (x == 0x81)
        return "EDIT";
    if (x == 0x82)
        return "STATIC";
    if (x == 0x83)
        return "LISTBOX";
    if (x == 0x84)
        return "SCROLLBAR";
    if (x == 0x85)
        return "COMBOBOX";
    return s[0] ? s : (const char*)0;
}
static HWND dlg32_create(const void* raw, unsigned size, HWND parent, DLGPROC32 proc, LPARAM param, int modal,
                         INT* result)
{
    DLG32_CURSOR c;
    unsigned style, ex;
    unsigned short count, i;
    short x, y, w, h;
    char title[WIN_TITLE_MAX];
    HWND hwnd;
    DLG32_STATE* state;
    if (!dlg32_validate(raw, size, &count))
        return 0;
    c.p = raw;
    c.size = size;
    c.off = 0;
    c.ok = 1;
    style = dlg32_u32(&c);
    ex = dlg32_u32(&c);
    (void)dlg32_u16(&c);
    x = (short)dlg32_u16(&c);
    y = (short)dlg32_u16(&c);
    w = (short)dlg32_u16(&c);
    h = (short)dlg32_u16(&c);
    dlg32_skip(&c);
    dlg32_skip(&c);
    dlg32_text(&c, title, sizeof(title));
    if (style & DLG32_DS_SETFONT)
    {
        (void)dlg32_u16(&c);
        dlg32_skip(&c);
    }
    if (style & DLG32_DS_CENTER)
    {
        x = (short)((GetSystemMetrics(0) - w * 2) / 2);
        y = (short)((GetSystemMetrics(1) - h * 2) / 2);
    }
    hwnd = CreateWindowExA(ex, "", title, style | DLG32_WS_POPUP, x * 2, y * 2, w * 2, h * 2, parent, 0, 0, 0);
    if (!hwnd)
        return 0;
    state = dlg32_alloc(modal);
    if (!state)
    {
        (void)DestroyWindow(hwnd);
        return 0;
    }
    state->hwnd = hwnd;
    for (i = 0; i < count; i++)
    {
        unsigned is, ie;
        unsigned short id, ord = 0, extra, first;
        char cls[64], text[WIN_TITLE_MAX];

        cls[0] = '\0';
        dlg32_align(&c);
        is = dlg32_u32(&c);
        ie = dlg32_u32(&c);
        x = (short)dlg32_u16(&c);
        y = (short)dlg32_u16(&c);
        w = (short)dlg32_u16(&c);
        h = (short)dlg32_u16(&c);
        id = dlg32_u16(&c);
        first = dlg32_u16(&c);
        if (first == 0xffffu)
        {
            ord = dlg32_u16(&c);
            if (!dlg32_known_class_ordinal(ord))
            {
                (void)DestroyWindow(hwnd);
                return 0;
            }
        }
        else
        {
            c.off -= 2;
            dlg32_text(&c, cls, sizeof(cls));
        }
        dlg32_text(&c, text, sizeof(text));
        extra = dlg32_u16(&c);
        if (!dlg32_need(&c, extra))
        {
            (void)DestroyWindow(hwnd);
            return 0;
        }
        c.off += extra;
        if (!dlg32_class(ord, cls) ||
            !CreateWindowExA(ie, dlg32_class(ord, cls), text, is | DLG32_WS_CHILD | DLG32_WS_VISIBLE, x * 2, y * 2,
                             w * 2, h * 2, hwnd, (HMENU)(unsigned long)id, 0, 0))
        {
            (void)DestroyWindow(hwnd);
            return 0;
        }
    }
    (void)ShowWindow(hwnd, 1);
    if (proc)
        (void)proc(hwnd, DLG32_WM_INITDIALOG, (WPARAM)(unsigned long)GetDlgItem(hwnd, 1), param);
    if (!modal)
        return hwnd;
    while (!state->ended)
    {
        struct user32_msg32 m;
        if (!GetMessageA(&m, 0, 0, 0))
        {
            (void)DestroyWindow(hwnd);
            return 0;
        }
        if (m.hwnd == hwnd && proc)
        {
            if (!proc(hwnd, m.message, m.wParam, m.lParam))
                (void)DefWindowProcA(hwnd, m.message, m.wParam, m.lParam);
        }
        else
            (void)DispatchMessageA(&m);
    }
    if (!state->ended)
    {
        (void)DestroyWindow(hwnd);
        return 0;
    }
    if (result)
        *result = state->result;
    (void)DestroyWindow(hwnd);
    return hwnd;
}
static const void* dlg32_resource(HINSTANCE inst, unsigned id, unsigned* size)
{
    DUET_RES_VIEW v;
    DUET_RES_KEY t, n;
    unsigned rva;
    if (!inst || !duet_res_init(inst, &v))
        return 0;
    t.by_name = 0;
    t.id = DUET_RES_TYPE_DIALOG;
    t.name = 0;
    t.name_len = 0;
    n.by_name = 0;
    n.id = id;
    n.name = 0;
    n.name_len = 0;
    if (!duet_res_find(&v, &t, &n, 0, 0, &rva, size))
        return 0;
    return duet_res_at(&v, rva, *size);
}
__declspec(dllexport) INT __stdcall DialogBoxParamA(HINSTANCE i, const char* t, HWND p, void* f, LPARAM a)
{
    const void* r;
    unsigned s;
    INT o = -1;
    unsigned long id = (unsigned long)t;
    if (!id || id > 0xffffu)
        return -1;
    r = dlg32_resource(i, (unsigned)id, &s);
    return r && dlg32_create(r, s, p, (DLGPROC32)f, a, 1, &o) ? o : -1;
}
__declspec(dllexport) INT __stdcall DialogBoxParamW(HINSTANCE i, const wchar_t16* t, HWND p, void* f, LPARAM a)
{
    return DialogBoxParamA(i, (const char*)t, p, f, a);
}
__declspec(dllexport) HWND __stdcall CreateDialogParamA(HINSTANCE i, const char* t, HWND p, void* f, LPARAM a)
{
    const void* r;
    unsigned s;
    unsigned long id = (unsigned long)t;
    if (!id || id > 0xffffu)
        return 0;
    r = dlg32_resource(i, (unsigned)id, &s);
    return r ? dlg32_create(r, s, p, (DLGPROC32)f, a, 0, 0) : 0;
}
__declspec(dllexport) HWND __stdcall CreateDialogParamW(HINSTANCE i, const wchar_t16* t, HWND p, void* f, LPARAM a)
{
    return CreateDialogParamA(i, (const char*)t, p, f, a);
}
__declspec(dllexport) BOOL __stdcall EndDialog(HWND h, INT r)
{
    DLG32_STATE* s = dlg32_state(h);
    if (!s)
        return 0;
    s->result = r;
    if (s->modal)
    {
        s->ended = 1;
        (void)PostMessageA(h, DLG32_WM_CLOSE, 0, 0);
        return 1;
    }
    return DestroyWindow(h);
}
/* ------------------------------------------------------------------
 * Enabled state
 * ------------------------------------------------------------------ */

/* GAP: the flag round-trips exactly, and EnableWindow fires the
 * WM_ENABLE the Win32 contract promises, but the compositor does not
 * consult it — a disabled window still receives mouse and keyboard
 * messages. Dialog code that greys a control and later re-reads its
 * own state works; code that relies on the input block does not.
 * Closing this needs an enabled bit in the kernel's window record and
 * a check in the input router. */
__declspec(dllexport) BOOL __stdcall EnableWindow(HWND h, BOOL enable)
{
    const int was = user32_record_enabled(h);
    user32_record_set_enabled(h, enable ? 1 : 0);
    if (was != (enable ? 1 : 0))
    {
        /* WM_ENABLE = 0x000A, wParam = new enabled state. */
        (void)SendMessageA(h, 0x000A, (WPARAM)(enable ? 1u : 0u), 0);
    }
    /* Win32 returns non-zero iff the window was PREVIOUSLY disabled. */
    return was ? 0 : 1;
}

__declspec(dllexport) BOOL __stdcall IsWindowEnabled(HWND h)
{
    return user32_record_enabled(h) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * Thread-targeted messages
 * ------------------------------------------------------------------ */

#define DUETOS_THREAD_MESSAGE_TAG 0x80000000u

/* The high-bit target is an internal SYS_WIN_POST_MSG transport tag. Public
 * PE32 HWND values reserve the high byte as zero, so it cannot alias a window. */
__declspec(dllexport) BOOL __stdcall PostThreadMessageA(DWORD tid, UINT msg, WPARAM w, LPARAM l)
{
    if (tid == 0 || (tid & DUETOS_THREAD_MESSAGE_TAG) != 0)
        return 0;
    return PostMessageA((HWND)(unsigned long)(DUETOS_THREAD_MESSAGE_TAG | tid), msg, w, l);
}

__declspec(dllexport) BOOL __stdcall PostThreadMessageW(DWORD tid, UINT msg, WPARAM w, LPARAM l)
{
    return PostThreadMessageA(tid, msg, w, l);
}

/* ------------------------------------------------------------------
 * System parameters
 * ------------------------------------------------------------------ */

#define SPI_GETWORKAREA 0x0030
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1

/* SPI_GETWORKAREA is answerable: the compositor has no reserved
 * taskbar strip, so the work area IS the screen, and both extents
 * come from the same SYS_WIN_GET_METRIC the caller could have asked
 * GetSystemMetrics for.
 *
 * GAP: every other SPI_* action reports failure. The ones callers ask
 * for most (SPI_GETNONCLIENTMETRICS, SPI_GETICONTITLELOGFONT) want
 * LOGFONT data, which needs a font pipeline that does not exist; the
 * rest describe desktop preferences DuetOS does not model. Reporting
 * failure lets a caller fall back to its own defaults, which is
 * strictly better than handing it a zeroed LOGFONT it would then try
 * to render with. */
__declspec(dllexport) BOOL __stdcall SystemParametersInfoA(UINT action, UINT param, void* data, UINT winini)
{
    (void)param;
    (void)winini;
    if (action == SPI_GETWORKAREA)
    {
        if (!data)
            return 0;
        struct user32_rect* r = (struct user32_rect*)data;
        r->left = 0;
        r->top = 0;
        r->right = GetSystemMetrics(SM_CXSCREEN);
        r->bottom = GetSystemMetrics(SM_CYSCREEN);
        return 1;
    }
    return 0;
}

__declspec(dllexport) BOOL __stdcall SystemParametersInfoW(UINT action, UINT param, void* data, UINT winini)
{
    return SystemParametersInfoA(action, param, data, winini);
}

/* ------------------------------------------------------------------
 * Icon / cursor lifetime + monitors
 * ------------------------------------------------------------------ */

// STUB: these report success without releasing anything. The rationale
// that used to sit here -- "LoadIcon / LoadCursor hand back non-null
// sentinels, so there is nothing allocated" -- is stale: LoadIcon and
// LoadCursor now decode .rsrc and return a real GDI bitmap / cursor
// handle for a PE hInstance (see user32_32_misc.c), so a caller that
// loads in a loop leaks one kernel object per call. Retiring this needs
// a GDI object-delete syscall these DLLs do not import yet. TRUE keeps
// a caller's cleanup path quiet; it is not evidence anything was freed.
__declspec(dllexport) BOOL __stdcall DestroyIcon(HICON icon)
{
    (void)icon;
    return 1;
}

__declspec(dllexport) BOOL __stdcall DestroyCursor(HCURSOR cursor)
{
    (void)cursor;
    return 1;
}

// STUB: single-monitor sentinel. The compositor drives one
// framebuffer and has no monitor objects, so every query resolves to
// the same pseudo-handle. 0x9001 matches the PE32+ sibling so the two
// surfaces do not diverge on the value a caller might compare.
__declspec(dllexport) HANDLE __stdcall MonitorFromWindow(HWND h, DWORD flags)
{
    (void)h;
    (void)flags;
    return (HANDLE)(unsigned long)0x9001u;
}

__declspec(dllexport) HANDLE __stdcall MonitorFromPoint(struct user32_point pt, DWORD flags)
{
    (void)pt;
    (void)flags;
    return (HANDLE)(unsigned long)0x9001u;
}

__declspec(dllexport) HANDLE __stdcall MonitorFromRect(const struct user32_rect* r, DWORD flags)
{
    (void)r;
    (void)flags;
    return (HANDLE)(unsigned long)0x9001u;
}
