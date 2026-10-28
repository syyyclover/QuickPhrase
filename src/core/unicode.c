/* Bounded, platform-independent UTF-8 <-> wchar_t conversion.
 * Windows wchar_t is UTF-16; native Linux tests use UTF-32. */
#include "storage.h"
#include <stdint.h>
#include <string.h>

size_t qp_utf16_prefix(const wchar_t *src, size_t length, size_t max_units)
{
    size_t n=length<max_units?length:max_units;
    if(n && n<length && src[n-1]>=0xD800 && src[n-1]<=0xDBFF && src[n]>=0xDC00 && src[n]<=0xDFFF)--n;
    return n;
}

static size_t decode(const unsigned char *s, size_t remaining, uint32_t *cp)
{
    size_t n, i;
    uint32_t v, minimum;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) { n = 2; v = s[0] & 31; minimum = 0x80; }
    else if (s[0] >= 0xE0 && s[0] <= 0xEF) { n = 3; v = s[0] & 15; minimum = 0x800; }
    else if (s[0] >= 0xF0 && s[0] <= 0xF4) { n = 4; v = s[0] & 7; minimum = 0x10000; }
    else { *cp = 0xFFFD; return 1; }
    if (n > remaining) { *cp = 0xFFFD; return 1; }
    for (i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 63);
    }
    if (v < minimum || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        *cp = 0xFFFD; return 1;
    }
    *cp = v;
    return n;
}

size_t qp_utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    size_t n = 0, at = 0, length;
    if (!src) { if (dst && cap) dst[0] = 0; return 0; }
    if (!cap) dst = NULL;
    length = strlen(src);
    while (at < length) {
        uint32_t cp;
        size_t units;
        at += decode((const unsigned char *)src + at, length - at, &cp);
        units = sizeof(wchar_t) == 2 && cp > 0xFFFF ? 2 : 1;
        if (dst && n + units >= cap) break;
        if (dst) {
            if (units == 2) {
                uint32_t v = cp - 0x10000;
                dst[n] = (wchar_t)(0xD800 + (v >> 10));
                dst[n + 1] = (wchar_t)(0xDC00 + (v & 1023));
            } else dst[n] = (wchar_t)cp;
        }
        n += units;
    }
    if (dst) dst[n] = 0;
    return n;
}

size_t qp_wide_to_utf8(const wchar_t *src, char *dst, size_t cap)
{
    size_t n = 0;
    if (!src) { if (dst && cap) dst[0] = 0; return 0; }
    if (!cap) dst = NULL;
    while (*src) {
        uint32_t cp = (uint32_t)*src++;
        unsigned char bytes[4];
        size_t count;
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF &&
            *src >= 0xDC00 && *src <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)*src++ - 0xDC00);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) { count = 1; bytes[0] = (unsigned char)cp; }
        else if (cp < 0x800) {
            count = 2; bytes[0] = 0xC0 | (cp >> 6); bytes[1] = 0x80 | (cp & 63);
        } else if (cp < 0x10000) {
            count = 3; bytes[0] = 0xE0 | (cp >> 12); bytes[1] = 0x80 | ((cp >> 6) & 63);
            bytes[2] = 0x80 | (cp & 63);
        } else {
            count = 4; bytes[0] = 0xF0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 63);
            bytes[2] = 0x80 | ((cp >> 6) & 63); bytes[3] = 0x80 | (cp & 63);
        }
        if (dst && n + count >= cap) break;
        if (dst) memcpy(dst + n, bytes, count);
        n += count;
    }
    if (dst) dst[n] = 0;
    return n;
}
