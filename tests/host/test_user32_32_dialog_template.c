/* Structural contract test for the PE32 normal-DLGTEMPLATE parser.
 *
 * This includes the implementation so the bounds-only validator can be
 * exercised without a window server.  Linker GC removes the windowing entry
 * points that need the freestanding syscall environment. */
#include <stdio.h>

#include "../../userland/libs/user32_32/user32_32_dlg.c"

/* Other exported functions in the included implementation retain references
 * even under section GC; these inert test-only definitions satisfy them. */
HWND user32_record_child_by_id(HWND parent, int ctrl_id)
{
    (void)parent;
    (void)ctrl_id;
    return 0;
}
int user32_record_ctrl_id(HWND hwnd)
{
    (void)hwnd;
    return 0;
}
void user32_w_to_ascii(const wchar_t16* src, char* dst, unsigned cap)
{
    (void)src;
    if (cap)
        dst[0] = 0;
}
int user32_record_enabled(HWND hwnd)
{
    (void)hwnd;
    return 0;
}
void user32_record_set_enabled(HWND hwnd, int enabled)
{
    (void)hwnd;
    (void)enabled;
}

static int expect(int condition, const char* name)
{
    if (condition)
        return 0;
    fprintf(stderr, "failed: %s\n", name);
    return 1;
}

int main(void)
{
    unsigned short count = 99;
    /* style, exstyle, cdit=0, x/y/cx/cy, empty menu/class/title. */
    static const unsigned char normal_empty[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    /* Normal header followed by one DWORD-aligned DLGITEMTEMPLATE.  Its id
     * is WORD 0x1234: the DIALOGEX DWORD-id format is rejected separately. */
    static const unsigned char normal_button[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,    0,    0,    0,    0,    0,    0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x34, 0x12, 0xff, 0xff, 0x80, 0x00, 0, 0, 0, 0,
    };
    static const unsigned char dialogex[] = {1, 0, 0xff, 0xff, 0, 0, 0, 0};
    static const unsigned char unknown_ordinal[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,    0,    0,    0,    0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0xff, 0xff, 0x99, 0x00, 0, 0, 0, 0,
    };
    int failed = 0;

    failed |= expect(dlg32_validate(normal_empty, sizeof(normal_empty), &count) && count == 0, "normal empty template");
    failed |= expect(dlg32_validate(normal_button, sizeof(normal_button), &count) && count == 1,
                     "normal WORD-id button template");
    failed |= expect(!dlg32_validate(normal_empty, sizeof(normal_empty) - 1, &count), "truncated template");
    failed |= expect(!dlg32_validate(dialogex, sizeof(dialogex), &count), "DIALOGEX discriminator");
    failed |= expect(!dlg32_validate(unknown_ordinal, sizeof(unknown_ordinal), &count), "unknown class ordinal");
    return failed ? 1 : 0;
}
