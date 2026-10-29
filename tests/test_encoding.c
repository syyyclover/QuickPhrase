#include "storage.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

int main(void)
{
    wchar_t wide[8];
    char bytes[8];
    wchar_t pair[] = {0xD83D, 0xDE00, 0};
    wchar_t copy[2];
    wmemset(wide, 0x7777, 8);
    assert(qp_utf8_to_wide("ABC", wide, 3) == 2);
    assert(wcscmp(wide, L"AB") == 0);
    memset(bytes, '!', sizeof(bytes));
    assert(qp_wide_to_utf8(L"中文", bytes, 4) == 3);
    assert(strcmp(bytes, "中") == 0);
    qp_utf8_to_wide("\xC0\xAF", wide, 8);
    assert(wide[0] == 0xFFFD && wide[1] == 0xFFFD && wide[2] == 0);
    qp_utf8_to_wide("\xF4\x90\x80\x80", wide, 8);
    assert(wide[0] == 0xFFFD);
    qp_strlcpy(copy, pair, 2);
    assert(copy[0] == 0);
    qp_utf8_to_wide("\xF0\x9F\x98\x80", wide, 2);
    if (sizeof(wchar_t) == 2) assert(wide[0] == 0);
    else assert(wide[0] == 0x1F600 && wide[1] == 0);
    puts("encoding: bounded conversion, invalid UTF-8 and surrogate boundaries passed");
    return 0;
}
