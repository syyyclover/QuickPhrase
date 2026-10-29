#include "table_key.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static int blank(wchar_t c)
{
    return c==L' ' || c==L'\t' || c==L'\r' || c==L'\n' || c==0x3000 || c==0x00A0;
}
static int digit(wchar_t c) { return c>=L'0' && c<=L'9'; }

wchar_t *qp_table_text_key(const wchar_t *src)
{
    if(!src)src=L"";
    while(*src && blank(*src))++src;
    size_t n=wcslen(src);while(n && blank(src[n-1]))--n;
    if(n>SIZE_MAX/sizeof(wchar_t)-1)return NULL;
    wchar_t *out=malloc((n+1)*sizeof(*out));if(!out)return NULL;
    for(size_t i=0;i<n;++i)out[i]=(wchar_t)towlower((wint_t)src[i]);
    out[n]=0;return out;
}

/* Decimal exponent + a bounded decimal-point offset. Never use floating point
   and never expand the exponent into a potentially enormous number of zeros. */
static wchar_t *exponent_add(const wchar_t *a,size_t na,int sign,ptrdiff_t offset)
{
    while(na>1 && *a==L'0'){++a;--na;}
    if(na==1 && *a==L'0')sign=1;
    wchar_t digits[3*sizeof(size_t)+1];size_t nb=0;
    size_t magnitude=(size_t)(offset<0?-offset:offset);
    do {digits[nb++]=(wchar_t)(L'0'+magnitude%10);magnitude/=10;}while(magnitude);
    for(size_t i=0;i<nb/2;++i){wchar_t t=digits[i];digits[i]=digits[nb-1-i];digits[nb-1-i]=t;}
    int other_sign=offset<0?-1:1;
    if(nb==1 && digits[0]==L'0')other_sign=1;
    size_t max=na>nb?na:nb;
    if(max>SIZE_MAX/sizeof(wchar_t)-3)return NULL;
    wchar_t *out=malloc((max+3)*sizeof(*out));if(!out)return NULL;
    size_t n=0;
    if(sign==other_sign) {
        int carry=0;
        for(size_t i=0;i<max || carry;++i) {
            int sum=carry+(i<na?a[na-1-i]-L'0':0)+(i<nb?digits[nb-1-i]-L'0':0);
            out[n++]=(wchar_t)(L'0'+sum%10);carry=sum/10;
        }
    } else {
        int compare=na==nb?wmemcmp(a,digits,na):na>nb?1:-1;
        const wchar_t *large=compare>=0?a:digits,*small=compare>=0?digits:a;
        size_t nl=compare>=0?na:nb,ns=compare>=0?nb:na;
        sign=compare>=0?sign:other_sign;
        int borrow=0;
        for(size_t i=0;i<nl;++i) {
            int value=large[nl-1-i]-L'0'-borrow-(i<ns?small[ns-1-i]-L'0':0);
            borrow=value<0;if(borrow)value+=10;out[n++]=(wchar_t)(L'0'+value);
        }
    }
    while(n>1 && out[n-1]==L'0')--n;
    for(size_t i=0;i<n/2;++i){wchar_t t=out[i];out[i]=out[n-1-i];out[n-1-i]=t;}
    if(sign<0 && !(n==1 && out[0]==L'0')) {memmove(out+1,out,n*sizeof(*out));out[0]=L'-';++n;}
    out[n]=0;return out;
}

wchar_t *qp_table_numeric_key_checked(const wchar_t *src,int *valid)
{
    int local=0;if(!valid)valid=&local;*valid=0;
    if(!src)return NULL;
    while(*src && blank(*src))++src;
    size_t length=wcslen(src);while(length && blank(src[length-1]))--length;
    if(!length || length>PTRDIFF_MAX)return NULL;
    const wchar_t *end=src+length,*p=src;
    int negative=0;
    if(*p==L'+' || *p==L'-'){negative=*p==L'-';++p;}
    const wchar_t *mantissa=p;
    size_t count=0,fraction=0;
    while(p<end && digit(*p)){++p;++count;}
    if(p<end && *p==L'.') {
        ++p;
        while(p<end && digit(*p)){++p;++count;++fraction;}
    }
    if(!count)return NULL;
    const wchar_t *mantissa_end=p,*exponent=L"0";size_t exponent_length=1;int exponent_sign=1;
    if(p<end && (*p==L'e' || *p==L'E')) {
        ++p;
        if(p<end && (*p==L'+' || *p==L'-')){exponent_sign=*p==L'-'?-1:1;++p;}
        exponent=p;while(p<end && digit(*p))++p;exponent_length=(size_t)(p-exponent);
        if(!exponent_length)return NULL;
    }
    if(p!=end)return NULL;
    *valid=1;
    if(count>SIZE_MAX/sizeof(wchar_t)-1)return NULL;
    wchar_t *coefficient=malloc((count+1)*sizeof(*coefficient));if(!coefficient)return NULL;
    size_t used=0;
    for(p=mantissa;p<mantissa_end;++p)if(digit(*p))coefficient[used++]=*p;
    size_t first=0,last=used;
    while(first<last && coefficient[first]==L'0')++first;
    if(first==last){coefficient[0]=L'0';coefficient[1]=0;return coefficient;}
    while(last>first && coefficient[last-1]==L'0')--last;
    ptrdiff_t offset=(ptrdiff_t)(used-last)-(ptrdiff_t)fraction;
    wchar_t *power=exponent_add(exponent,exponent_length,exponent_sign,offset);
    if(!power){free(coefficient);return NULL;}
    size_t digits=last-first,np=wcslen(power),n=digits+np+1+(size_t)negative;
    wchar_t *out=n<SIZE_MAX/sizeof(wchar_t)-1?malloc((n+1)*sizeof(*out)):NULL;
    if(out) {
        size_t at=0;if(negative)out[at++]=L'-';
        memcpy(out+at,coefficient+first,digits*sizeof(*out));at+=digits;out[at++]=L'e';
        memcpy(out+at,power,(np+1)*sizeof(*out));
    }
    free(power);free(coefficient);return out;
}
wchar_t *qp_table_numeric_key(const wchar_t *src)
{
    return qp_table_numeric_key_checked(src,NULL);
}
