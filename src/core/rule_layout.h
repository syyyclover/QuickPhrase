#ifndef QP_RULE_LAYOUT_H
#define QP_RULE_LAYOUT_H

/* Pixel geometry only; caller supplies actual font/text measurements. */
typedef struct { int x, y, w, h; } QpRect;
typedef struct {
    int dpi, width, height, kind;
    int line_height, summary_height, paragraph_height, tip_height;
} QpRuleLayoutInput;
typedef struct {
    QpRect labels[7], fields[7];
    unsigned visible_rows;
    QpRect lookup_label, lookup_button, summary, paragraph, paragraph_help, tip;
    QpRect paragraph_mode_label, paragraph_mode, paragraph_separator_label, paragraph_separator;
    QpRect ok, cancel;
    int minimum_height;
} QpRuleLayout;

void qp_rule_layout(const QpRuleLayoutInput *input, QpRuleLayout *out);
#endif
