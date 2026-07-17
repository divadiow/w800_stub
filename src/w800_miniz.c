#include "w800_miniz.h"

#include "miniz.h"

#if !TDEFL_LESS_MEMORY
#error "The W800 stub requires TDEFL_LESS_MEMORY=1"
#endif

#define W800_MINIZ_INPUT_SIZE 1024U

typedef struct {
    w800_miniz_put_byte_fn put_byte;
    void *ctx;
} miniz_output_t;

static tdefl_compressor deflator;
static tinfl_decompressor inflator;
static uint8_t inflate_input[W800_MINIZ_INPUT_SIZE];
static uint8_t inflate_dictionary[TINFL_LZ_DICT_SIZE];

static mz_bool deflate_output(const void *buffer, int len, void *user)
{
    miniz_output_t *output = (miniz_output_t *)user;
    const uint8_t *bytes = (const uint8_t *)buffer;
    for (int i = 0; i < len; i++) {
        if (!output->put_byte(output->ctx, bytes[i])) return MZ_FALSE;
    }
    return MZ_TRUE;
}

int w800_miniz_deflate_raw(const volatile uint8_t *src, uint32_t len, uint8_t level,
                           w800_miniz_put_byte_fn put_byte, void *put_ctx)
{
    if (!put_byte) return 0;
    if (level < 1U || level > 10U) level = 5U;
    miniz_output_t output = { put_byte, put_ctx };
    int flags = (int)tdefl_create_comp_flags_from_zip_params(level, -15, MZ_DEFAULT_STRATEGY);
    if (tdefl_init(&deflator, deflate_output, &output, flags) != TDEFL_STATUS_OKAY) return 0;
    return tdefl_compress_buffer(&deflator, (const void *)(uintptr_t)src, len,
                                 TDEFL_FINISH) == TDEFL_STATUS_DONE;
}

static int write_inflate_output(w800_miniz_put_byte_fn put_byte, void *put_ctx,
                                const uint8_t *buffer, uint32_t len,
                                uint32_t *total, uint32_t expected_len)
{
    if (len > expected_len - *total) return 0;
    for (uint32_t i = 0U; i < len; i++) {
        if (!put_byte(put_ctx, buffer[i])) return 0;
    }
    *total += len;
    return 1;
}

int w800_miniz_inflate_raw(w800_miniz_get_byte_fn get_byte, void *get_ctx,
                           w800_miniz_put_byte_fn put_byte, void *put_ctx,
                           uint32_t expected_len)
{
    if (!get_byte || !put_byte) return 0;
    tinfl_init(&inflator);
    uint32_t total = 0U;
    uint32_t dictionary_pos = 0U;

    for (;;) {
        uint32_t input_len = 0U;
        while (input_len < sizeof(inflate_input) &&
               get_byte(get_ctx, &inflate_input[input_len])) {
            input_len++;
        }
        if (!input_len) return 0;

        uint32_t input_pos = 0U;
        while (input_pos < input_len) {
            size_t input_bytes = input_len - input_pos;
            size_t output_bytes = sizeof(inflate_dictionary) - dictionary_pos;
            tinfl_status status = tinfl_decompress(&inflator,
                inflate_input + input_pos, &input_bytes,
                inflate_dictionary, inflate_dictionary + dictionary_pos, &output_bytes,
                TINFL_FLAG_HAS_MORE_INPUT);
            input_pos += (uint32_t)input_bytes;
            if (!write_inflate_output(put_byte, put_ctx,
                    inflate_dictionary + dictionary_pos, (uint32_t)output_bytes,
                    &total, expected_len)) return 0;
            dictionary_pos += (uint32_t)output_bytes;
            if (dictionary_pos == sizeof(inflate_dictionary)) dictionary_pos = 0U;

            if (status == TINFL_STATUS_DONE) return total == expected_len;
            if (status < TINFL_STATUS_DONE) return 0;
            if (status == TINFL_STATUS_NEEDS_MORE_INPUT) break;
            if (!input_bytes && !output_bytes) return 0;
        }
    }
}
