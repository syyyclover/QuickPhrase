#ifndef QP_EDITOR_INTERNAL_H
#define QP_EDITOR_INTERNAL_H
#include "dialogs_internal.h"
#include "editor_layout.h"
#define ED_MIN_W 880
#define ED_MIN_H 690
#define ED_MIN_TRACK_W 600
#define ED_MIN_TRACK_H 320
#define ED_PREVIEW_DEF 46
#define ED_SPLIT_HIT DPIX(8)
typedef struct {
    EvalContext *previewCtx;
    HWND hwnd;
    Template *t;
    int is_new;
    int result;
    int loading;                 /* 正在填充控件，忽略 EN_CHANGE */
    int cur;                     /* 当前片段索引 */

    HWND hName, hGroup, hSegLbl, hSegList;
    HWND hPageSegments, hPageProps;
    int activePage, narrow, propsForceScroll;
    HWND hBatch, hBatchSrc, hBatchSrcLbl;     /* 批量：源变量按行重复整模板输出 */
    HWND hAddStatic, hAddText, hAddSelect, hAddMulti, hAddTime;
    HWND hSegDel, hSegUp, hSegDown;

    HWND hPropsHost, hPropTitle;
    int propsScroll;             /* 属性区的垂直滚动位置 */
    int propsWheelDelta;
    int propsVisibilityKey;
    int lastClientW, lastClientH;
    int maximized, restore_cloaked;
    HWND hTableOutput;
    HWND hTableFmtLbl, hTableFmt, hTableInsert, hTableInsertBtn, hTableCellBtn;
    HWND hPreviewText, hPreviewTable;
    OutputMode previewMode;      /* 编辑器预览在文本 / 表格口径间切换 */
    HWND hTypeLbl, hType;
    HWND hLabelLbl, hLabel;
    HWND hContentLbl, hContent;
    HWND hHint;                       /* 固定内容下方的占位符提示（灰字一行） */
    HWND hPlaceLbl, hPlace;
    HWND hDefLbl, hDef;
    HWND hSepLbl, hSep;
    HWND hFmtLbl, hFmt, hFmtSample;   /* 时间格式 + 实时效果 */
    HWND hHidden;
    HWND hNoOutput;                   /* 参与匹配/引用/占位符，但不进入最终输出 */
    HWND hMultiline;
    HWND hRefLbl, hRef;                       /* 引用哪个变量 */
    HWND hRefModeLbl, hRefMode, hRefA, hRefB; /* 取整体 / 第 N 段 / 第 N 个字起 */

    HWND hOptLbl, hOptList, hOptAdd, hOptEdit, hOptDel, hOptUp, hOptDown;
    HWND hParseLbl, hParseList, hParseAdd, hParseEdit, hParseDel, hParseUp, hParseDown;
    HWND hCondLbl, hCondList, hCondAdd, hCondEdit, hCondDel, hCondUp, hCondDown;

    HWND hPreviewLbl, hPreview;
    int  splitDrag;              /* 正在拖预览分隔条 */
    int  splitGrabDy;            /* 按下点相对分隔条上沿的偏移 */
    int  splitLastY;             /* 上次画出的分隔条位置，-1 = 还没画过 */
} EditorCtx;
void ed_layout(EditorCtx *c);
Segment *ed_cur_seg(EditorCtx *c);
void ed_restore_size(HWND hwnd,HWND owner);
int ed_split_hit(HWND hwnd,int x,int y);
int ed_splitter_y(int height);
int ed_splitter_base(int height);
void ed_preview_bounds(int height,int *lo,int *hi);
LRESULT CALLBACK ed_props_proc(HWND,UINT,WPARAM,LPARAM);
BOOL CALLBACK ed_prop_hook_focus(HWND,LPARAM);
#endif
