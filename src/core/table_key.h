#ifndef QP_TABLE_KEY_H
#define QP_TABLE_KEY_H
#include <wchar.h>
/* Owning normalized keys. Numeric invalid syntax yields NULL with valid=0;
   allocation failure after valid syntax yields NULL with valid=1. */
wchar_t *qp_table_text_key(const wchar_t *src);
wchar_t *qp_table_numeric_key(const wchar_t *src);
wchar_t *qp_table_numeric_key_checked(const wchar_t *src, int *valid);
#endif
