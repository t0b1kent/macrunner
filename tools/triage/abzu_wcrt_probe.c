#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int main(void)
{
    static const wchar_t text[] = L"3";
    wchar_t *end_int = NULL;
    wchar_t *end_double = NULL;
    long i = wcstol(text, &end_int, 10);
    double d = wcstod(text, &end_double);
    union
    {
        double d;
        uint64_t u;
    } bits;

    bits.d = d;
    printf("abzu-wcrt-probe: wcstol=%ld end_i=%td wcstod_bits=0x%016llx end_d=%td\n",
           i, end_int ? end_int - text : -1,
           (unsigned long long)bits.u,
           end_double ? end_double - text : -1);
    fflush(stdout);

    if (i != 3) return 10;
    if (bits.u != 0x4008000000000000ULL) return 1;
    return 0;
}
