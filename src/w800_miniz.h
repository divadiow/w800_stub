#ifndef W800_MINIZ_H
#define W800_MINIZ_H

#include <stdint.h>

typedef int (*w800_miniz_get_byte_fn)(void *ctx, uint8_t *value);
typedef int (*w800_miniz_put_byte_fn)(void *ctx, uint8_t value);

int w800_miniz_inflate_raw(w800_miniz_get_byte_fn get_byte, void *get_ctx,
                           w800_miniz_put_byte_fn put_byte, void *put_ctx,
                           uint32_t expected_len);

int w800_miniz_deflate_raw(const volatile uint8_t *src, uint32_t len, uint8_t level,
                           w800_miniz_put_byte_fn put_byte, void *put_ctx);

#endif
