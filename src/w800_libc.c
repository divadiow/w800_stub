#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t len)
{
    unsigned char *out = (unsigned char *)dest;
    const unsigned char *in = (const unsigned char *)src;
    while (len--) *out++ = *in++;
    return dest;
}

void *memset(void *dest, int value, size_t len)
{
    unsigned char *out = (unsigned char *)dest;
    while (len--) *out++ = (unsigned char)value;
    return dest;
}
