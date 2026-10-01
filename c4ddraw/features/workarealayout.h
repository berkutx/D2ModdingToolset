#pragma once

#include <windows.h>

/* chrome is AdjustWindowRectEx({0,0,0,0}, style, menu, exstyle).
 * Keep the complete outer window inside rcWork, including monitors at negative coordinates. */
static __inline BOOL c4_workarea_client_rect(const RECT* work, const RECT* chrome, RECT* client)
{
    RECT result;
    if (!work || !chrome || !client || work->right <= work->left || work->bottom <= work->top)
        return FALSE;
    result.left = work->left - chrome->left;
    result.top = work->top - chrome->top;
    result.right = work->right - chrome->right;
    result.bottom = work->bottom - chrome->bottom;
    if (result.right <= result.left || result.bottom <= result.top)
        return FALSE;
    *client = result;
    return TRUE;
}
