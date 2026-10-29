/* Private geometry queue: grouped native transactions, replay and damage. */
#include "ui.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    HWND hwnd, parent;
    RECT old;
    int x, y, w, h;
    UINT flags;
} QpLayoutEntry;
typedef struct {
    QpLayoutEntry *entries;
    int count, capacity;
} QpLayoutBatch;

static void layout_damage(const QpLayoutEntry *e)
{
    UINT flags = RDW_INVALIDATE | RDW_ERASE | RDW_FRAME;
    flags |= e->flags & SWP_NOCOPYBITS ? RDW_ALLCHILDREN : RDW_NOCHILDREN;
    RedrawWindow(e->hwnd, NULL, NULL, flags);
}

static void layout_parent_damage(HWND parent, const RECT *area)
{
    if (parent) RedrawWindow(parent, area, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN);
}

static void layout_commit(QpLayoutBatch *b)
{
    for (int first = 0; first < b->count; ) {
        int last = first + 1, ok = 0;
        HWND parent = b->entries[first].parent;
        RECT area = {0};
        while (last < b->count && b->entries[last].parent == parent) ++last;
        HDWP native = BeginDeferWindowPos(last - first);
        for (int i = first; native && i < last; ++i) {
            QpLayoutEntry *e = &b->entries[i];
            native = DeferWindowPos(native, e->hwnd, NULL, e->x, e->y, e->w, e->h, e->flags);
        }
        if (native) ok = EndDeferWindowPos(native);
        for (int i = first; i < last; ++i) {
            QpLayoutEntry *e = &b->entries[i];
            RECT wanted = {e->x, e->y, e->x + e->w, e->y + e->h};
            if (!ok) SetWindowPos(e->hwnd, NULL, e->x, e->y, e->w, e->h, e->flags);
            UnionRect(&area, &area, &e->old);
            UnionRect(&area, &area, &wanted);
            layout_damage(e);
        }
        layout_parent_damage(parent, &area);
        first = last;
    }
    b->count = 0;
}

HDWP qp_layout_begin(int capacity)
{
    QpLayoutBatch *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    if (capacity < 1) capacity = 1;
    b->entries = calloc((size_t)capacity, sizeof(*b->entries));
    if (!b->entries) { free(b); return NULL; }
    b->capacity = capacity;
    return (HDWP)b;
}

void qp_layout_end(HDWP batch)
{
    QpLayoutBatch *b = (QpLayoutBatch *)batch;
    if (!b) return;
    layout_commit(b);
    free(b->entries);
    free(b);
}

HDWP qp_defer(HDWP batch, HWND h, int x, int y, int w, int height)
{
    QpLayoutBatch *b = (QpLayoutBatch *)batch;
    QpLayoutEntry e = {0};
    if (!h) return batch;
    if (w < 0) w = 0;
    if (height < 0) height = 0;
    e.hwnd = h; e.parent = GetParent(h);
    if (!GetWindowRect(h, &e.old)) return batch;
    if (e.parent) MapWindowPoints(NULL, e.parent, (POINT *)&e.old, 2);
    /* Drop-down combos choose their closed height from the font. Reapplying
       the logical row height makes them resize themselves back on every frame.
       Their popup height is managed separately by qp_combo_dropdown_fix. */
    if (height > 0 && e.old.bottom > e.old.top && height != e.old.bottom-e.old.top) {
        wchar_t cls[16];
        if (GetClassNameW(h,cls,16) && lstrcmpiW(cls,L"ComboBox")==0 &&
            (GetWindowLongPtrW(h,GWL_STYLE)&3)>CBS_SIMPLE)
            height=e.old.bottom-e.old.top;
    }
    /* Last requested rectangle wins, including cancellation to the old one. */
    if (b) for (int i = 0; i < b->count; ++i) if (b->entries[i].hwnd == h) {
        e.old = b->entries[i].old;
        memmove(b->entries + i, b->entries + i + 1, (size_t)(b->count - i - 1) * sizeof(*b->entries));
        --b->count;
        break;
    }
    if (e.old.left == x && e.old.top == y && e.old.right - e.old.left == w &&
        e.old.bottom - e.old.top == height) return batch;
    e.x = x; e.y = y; e.w = w; e.h = height;
    /* Damage is queued explicitly below, including parent exposure and frames.
       Suppress native intermediate paints until all nested layouts are placed. */
    e.flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
    if (e.old.right - e.old.left != w || e.old.bottom - e.old.top != height)
        e.flags |= SWP_NOCOPYBITS;
    if (b && b->count == b->capacity) {
        int capacity = b->capacity <= INT_MAX / 2 ? b->capacity * 2 : 0;
        QpLayoutEntry *grown = capacity ? realloc(b->entries, (size_t)capacity * sizeof(*grown)) : NULL;
        if (grown) { b->entries = grown; b->capacity = capacity; }
        else layout_commit(b);
    }
    if (b) b->entries[b->count++] = e;
    else {
        SetWindowPos(h, NULL, x, y, w, height, e.flags);
        RECT wanted = {x, y, x + w, y + height}, area;
        UnionRect(&area, &e.old, &wanted);
        layout_damage(&e);
        layout_parent_damage(e.parent, &area);
    }
    return batch;
}
