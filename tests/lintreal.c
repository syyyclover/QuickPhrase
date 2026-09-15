/*
 * lintreal.c —— 开发期小工具（不参与 make test，也不产出交付物）
 * 把一份真实的模板 JSON 喂给 tmpl_lint，看体检会报出什么。
 *
 *   cc -std=gnu11 -O1 -Isrc -o build/lintreal tests/lintreal.c \
 *      src/engine.c src/storage.c src/cJSON.c src/regex.c
 *   build/lintreal QuickPhrase-模板.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include "engine.h"
#include "storage.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    char *text;
    AppData *d;
    Template *t;
    LintIssue *is;

    setlocale(LC_ALL, "");
    if (argc < 2) { printf("usage: lintreal <json>\n"); return 2; }
    text = slurp(argv[1]);
    if (!text) { printf("read failed\n"); return 2; }
    d = appdata_from_json(text);
    free(text);
    if (!d) { printf("parse failed\n"); return 2; }

    is = (LintIssue *)calloc(QP_LINT_MAX, sizeof(LintIssue));
    if (!is) { appdata_free(d); return 2; }
    for (t = d->templates; t; t = t->next) {
        int n = tmpl_lint(t, is, QP_LINT_MAX), i;
        printf("\n== %ls ==  %d segs, %d issues\n",
               t->name, seg_list_count(t->segments), n);
        for (i = 0; i < n; ++i)
            printf("  [%d] %ls\n", (int)is[i].kind, is[i].text);
    }
    free(is);
    appdata_free(d);
    return 0;
}
