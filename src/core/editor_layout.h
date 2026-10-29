#ifndef QP_EDITOR_LAYOUT_H
#define QP_EDITOR_LAYOUT_H
#include "rule_layout.h"
typedef struct { int dpi,width,height,preview_height,active_page; } QpEditorLayoutInput;
typedef struct {
    QpRect name_label,name,group_label,group,batch,batch_label,batch_source;
    QpRect segment_label,toolbar[8],segment,properties,page_tabs[2];
    QpRect preview_label,preview,ok,cancel;
    int narrow,body_bottom;
} QpEditorLayout;
void qp_editor_layout(const QpEditorLayoutInput *in,QpEditorLayout *out);
void qp_editor_preview_bounds(int dpi,int height,int *low,int *high);
int qp_editor_preview_height(int dpi,int height,int wanted);
int qp_editor_splitter_y(int dpi,int height,int wanted);
#endif
