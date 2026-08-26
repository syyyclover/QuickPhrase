/*
 * resource.h —— 资源与控件 ID
 */
#ifndef QP_RESOURCE_H
#define QP_RESOURCE_H

/* 资源 */
#define IDI_APPICON             101
#define IDI_APPICON_SMALL       102

/* 主窗口控件 */
#define IDC_SEARCH              1001
#define IDC_TREE                1002
#define IDC_TITLE               1003
#define IDC_FIELDHOST           1004
#define IDC_PREVIEW             1005
#define IDC_BTN_COPY            1006
#define IDC_BTN_RESET           1007
#define IDC_BTN_EDIT            1008
#define IDC_HINT                1009
#define IDC_PREVIEW_LABEL       1010
#define IDC_SEARCH_LABEL        1011
#define IDC_TABS                1012   /* 并行填写的标签条（自绘） */

/* 动态字段控件 ID 基址（每个字段占 3 个 ID） */
#define IDC_FIELD_BASE          2000
#define IDC_FIELD_STRIDE        3

/* 菜单命令 */
#define IDM_FILE_IMPORT         40001
#define IDM_FILE_EXPORT         40002
#define IDM_FILE_HIDE           40003
#define IDM_FILE_EXIT           40004

#define IDM_TPL_NEW             40010
#define IDM_TPL_EDIT            40011
#define IDM_TPL_DUP             40012
#define IDM_TPL_DELETE          40013
#define IDM_GROUP_NEW           40014
#define IDM_GROUP_RENAME        40015
#define IDM_GROUP_DELETE        40016
#define IDM_TREE_EXPAND         40017
#define IDM_TREE_COLLAPSE       40018
#define IDM_TPL_MOVE_ROOT       40019

#define IDM_SETTINGS            40030
#define IDM_OPEN_DATA_DIR       40031
#define IDM_ABOUT               40032
#define IDM_TPL_DEMO            40033   /* 插入示例模板（复杂测试案例） */

#define IDM_COPY                40040   /* 加速键：Ctrl+Enter */
#define IDM_RESET               40041
#define IDM_FOCUS_SEARCH        40042
#define IDM_HIDE_WINDOW         40043

/* 托盘菜单 */
#define IDM_TRAY_OPEN           40050
#define IDM_TRAY_EXIT           40051
#define IDM_TRAY_AUTOSTART      40052
#define IDM_TRAY_SETTINGS       40053

/* 并行填写（右侧顶部标签条）。一个模板可以同时开多份互不干扰的填写。 */
#define IDM_SESS_NEW            40060   /* Ctrl+T   */
#define IDM_SESS_CLOSE          40061   /* Ctrl+W   */
#define IDM_SESS_RENAME         40062
#define IDM_SESS_DUP            40063
#define IDM_SESS_NEXT           40064   /* Ctrl+Tab       */
#define IDM_SESS_PREV           40065   /* Ctrl+Shift+Tab */
#define IDM_SESS_COLOR_BASE     40070   /* +0..8 = 调色板 */
#define IDM_SESS_SLOT_BASE      40080   /* +0..8 = Ctrl+1~9 */

/* 模板编辑器控件 */
#define IDC_ED_NAME             3001
#define IDC_ED_GROUP            3002
#define IDC_ED_SEGLIST          3003
#define IDC_ED_SEG_ADD          3004
#define IDC_ED_SEG_DEL          3005
#define IDC_ED_SEG_UP           3006
#define IDC_ED_SEG_DOWN         3007
#define IDC_ED_SEG_TYPE         3008
#define IDC_ED_SEG_LABEL        3009
#define IDC_ED_SEG_CONTENT      3010
#define IDC_ED_SEG_PLACE        3011
#define IDC_ED_SEG_DEFAULT      3012
#define IDC_ED_SEG_SEP          3013
#define IDC_ED_OPTLIST          3014
#define IDC_ED_OPT_ADD          3015
#define IDC_ED_OPT_EDIT         3016
#define IDC_ED_OPT_DEL          3017
#define IDC_ED_OPT_UP           3018
#define IDC_ED_OPT_DOWN         3019
#define IDC_ED_PARSELIST        3020
#define IDC_ED_PARSE_ADD        3021
#define IDC_ED_PARSE_EDIT       3022
#define IDC_ED_PARSE_DEL        3023
#define IDC_ED_PARSE_UP         3024
#define IDC_ED_PARSE_DOWN       3025
#define IDC_ED_CONDLIST         3026
#define IDC_ED_COND_ADD         3027
#define IDC_ED_COND_EDIT        3028
#define IDC_ED_COND_DEL         3029
#define IDC_ED_COND_UP          3030
#define IDC_ED_COND_DOWN        3031
#define IDC_ED_PREVIEW          3032
#define IDC_ED_OK               3033
#define IDC_ED_CANCEL           3034
#define IDC_ED_OPT_TEXT         3035
#define IDC_ED_SEG_ADD_STATIC   3036
#define IDC_ED_SEG_ADD_TEXT     3037
#define IDC_ED_SEG_ADD_SELECT   3038
#define IDC_ED_SEG_ADD_MULTI    3039
#define IDC_ED_SEG_HIDDEN       3040
#define IDC_ED_SEG_MULTILINE    3041   /* 文本输入：多行大文本框 */
#define IDC_ED_SEG_ADD_TIME     3042   /* 「+时间」按钮          */
#define IDC_ED_SEG_FORMAT       3043   /* 时间格式（可下拉可手敲）*/
#define IDC_ED_SEG_FMT_SAMPLE   3044   /* 格式的实时示例（灰字）  */
#define IDC_ED_SEG_REF          3045   /* 引用来源变量            */
#define IDC_ED_SEG_REF_MODE     3046   /* 取法：整体/分隔符/按字  */
#define IDC_ED_SEG_REF_A        3047   /* 取法参数一：分隔符 / 起始 */
#define IDC_ED_SEG_REF_B        3048   /* 取法参数二：第N段 / 取几个 */
#define IDC_ED_SEG_HINT         3049   /* 固定内容的占位符提示行    */

/* 规则编辑子对话框 */
#define IDC_RL_TYPE             3101
#define IDC_RL_DELIM            3102
#define IDC_RL_INDEX            3103
#define IDC_RL_KEYWORD          3104
#define IDC_RL_TARGET           3105
#define IDC_RL_FILL             3106
#define IDC_RL_TRIGGER          3107
#define IDC_RL_OP               3108
#define IDC_RL_MATCH            3109
#define IDC_RL_RESULT           3110
#define IDC_RL_OK               3111
#define IDC_RL_CANCEL           3112
#define IDC_RL_TIP              3113
#define IDC_RL_JOIN             3114   /* 全部满足 / 任意满足 */
#define IDC_RL_TERMLIST         3115   /* 条件项列表          */
#define IDC_RL_TERM_ADD         3116
#define IDC_RL_TERM_DEL         3117
#define IDC_RL_NEGATE           3118   /* 「不满足」取反勾选  */

/* 设置对话框 */
#define IDC_SET_HOTKEY          3201
#define IDC_SET_AUTOSTART       3202
#define IDC_SET_THEME           3203
#define IDC_SET_SEP             3204
#define IDC_SET_OK              3205
#define IDC_SET_CANCEL          3206
#define IDC_SET_DATADIR         3207

/* 简单输入对话框 */
#define IDC_INPUT_EDIT          3301
#define IDC_INPUT_OK            3302
#define IDC_INPUT_CANCEL        3303

/* 多选下拉弹窗 */
#define IDC_POPUP_LIST          3401

#endif /* QP_RESOURCE_H */
