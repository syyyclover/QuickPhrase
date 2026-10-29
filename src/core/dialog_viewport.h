#ifndef QP_DIALOG_VIEWPORT_H
#define QP_DIALOG_VIEWPORT_H
/* Physical coordinates. Fixed modal content is scrollable, never rescaled. */
int qp_dialog_scroll_limit(int content, int view);
int qp_dialog_focus_scroll(int content, int view, int current, int first, int last);
#endif
