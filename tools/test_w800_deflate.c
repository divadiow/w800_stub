#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../src/w800_miniz.h"

#define TEST_CAPACITY 131072U

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t pos;
    uint32_t capacity;
} buffer_t;

static uint8_t source[TEST_CAPACITY];
static uint8_t compressed[TEST_CAPACITY * 2U];
static uint8_t restored[TEST_CAPACITY];

static int buffer_get(void *ctx, uint8_t *value)
{
    buffer_t *buffer = (buffer_t *)ctx;
    if (buffer->pos >= buffer->size) return 0;
    *value = buffer->data[buffer->pos++];
    return 1;
}

static int buffer_put(void *ctx, uint8_t value)
{
    buffer_t *buffer = (buffer_t *)ctx;
    if (buffer->size >= buffer->capacity) return 0;
    buffer->data[buffer->size++] = value;
    return 1;
}

static void fill_source(uint32_t len, uint32_t pattern)
{
    uint32_t random = 0x12345678U;
    for (uint32_t i = 0U; i < len; i++) {
        if (pattern == 0U) source[i] = 0U;
        else if (pattern == 1U) source[i] = (uint8_t)i;
        else if (pattern == 2U) {
            random = random * 1664525U + 1013904223U;
            source[i] = (uint8_t)(random >> 24);
        } else {
            source[i] = (i & 0x3ffU) < 64U ? 0xffU : source[i & 0xffU];
        }
    }
}

static int zlib_deflate_raw(uint32_t len, int level, uint32_t *compressed_len)
{
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 0;
    stream.next_in = source;
    stream.avail_in = len;
    stream.next_out = compressed;
    stream.avail_out = sizeof(compressed);
    int status = deflate(&stream, Z_FINISH);
    *compressed_len = (uint32_t)stream.total_out;
    deflateEnd(&stream);
    return status == Z_STREAM_END;
}

static int zlib_inflate_raw(uint32_t compressed_len, uint32_t expected_len)
{
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, -15) != Z_OK) return 0;
    stream.next_in = compressed;
    stream.avail_in = compressed_len;
    stream.next_out = restored;
    stream.avail_out = sizeof(restored);
    int status = inflate(&stream, Z_FINISH);
    uint32_t restored_len = (uint32_t)stream.total_out;
    inflateEnd(&stream);
    return status == Z_STREAM_END && restored_len == expected_len;
}

static int test_case(uint32_t len, uint32_t pattern)
{
    fill_source(len, pattern);

    buffer_t encoded = { compressed, 0U, 0U, sizeof(compressed) };
    static const uint8_t levels[] = { 1U, 2U, 5U, 9U };
    for (uint32_t i = 0U; i < sizeof(levels); i++) {
        encoded.size = 0U;
        if (!w800_miniz_deflate_raw(source, len, levels[i], buffer_put, &encoded) ||
            !zlib_inflate_raw(encoded.size, len) || memcmp(source, restored, len)) {
            fprintf(stderr, "encoder failed: len=%u pattern=%u level=%u\n", len, pattern, levels[i]);
            return 0;
        }
    }

    for (int level = 0; level <= 9; level++) {
        uint32_t compressed_len;
        if (!zlib_deflate_raw(len, level, &compressed_len)) return 0;
        buffer_t input = { compressed, compressed_len, 0U, sizeof(compressed) };
        buffer_t output = { restored, 0U, 0U, sizeof(restored) };
        if (!w800_miniz_inflate_raw(buffer_get, &input, buffer_put, &output, len) ||
            output.size != len || memcmp(source, restored, len)) {
            fprintf(stderr, "decoder failed: len=%u pattern=%u level=%d\n", len, pattern, level);
            return 0;
        }
    }
    return 1;
}

static int test_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        fclose(file);
        return 0;
    }
    uint32_t len = (uint32_t)ftell(file);
    rewind(file);
    uint8_t *file_source = malloc(len ? len : 1U);
    uint8_t *file_compressed = malloc((size_t)len * 2U + 1024U);
    uint8_t *file_restored = malloc(len ? len : 1U);
    if (!file_source || !file_compressed || !file_restored || fread(file_source, 1, len, file) != len) {
        fclose(file);
        free(file_source);
        free(file_compressed);
        free(file_restored);
        return 0;
    }
    fclose(file);

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 0;
    stream.next_in = file_source;
    stream.avail_in = len;
    stream.next_out = file_compressed;
    stream.avail_out = len * 2U + 1024U;
    int status = deflate(&stream, Z_FINISH);
    uint32_t compressed_len = (uint32_t)stream.total_out;
    deflateEnd(&stream);
    if (status != Z_STREAM_END) return 0;

    buffer_t input = { file_compressed, compressed_len, 0U, compressed_len };
    buffer_t output = { file_restored, 0U, 0U, len };
    int ok = w800_miniz_inflate_raw(buffer_get, &input, buffer_put, &output, len) &&
             output.size == len && !memcmp(file_source, file_restored, len);
    if (!ok) fprintf(stderr, "decoder failed for file %s at input byte %u of %u\n", path, input.pos, compressed_len);

    buffer_t encoded = { file_compressed, 0U, 0U, len * 2U + 1024U };
    if (ok) ok = w800_miniz_deflate_raw(file_source, len, 5U, buffer_put, &encoded);
    if (ok) {
        memset(&stream, 0, sizeof(stream));
        if (inflateInit2(&stream, -15) != Z_OK) ok = 0;
        else {
            stream.next_in = file_compressed;
            stream.avail_in = encoded.size;
            stream.next_out = file_restored;
            stream.avail_out = len;
            status = inflate(&stream, Z_FINISH);
            ok = status == Z_STREAM_END && stream.total_out == len && !memcmp(file_source, file_restored, len);
            inflateEnd(&stream);
        }
    }
    free(file_source);
    free(file_compressed);
    free(file_restored);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !test_file(argv[1])) return 1;
    static const uint32_t lengths[] = { 0U, 1U, 2U, 3U, 257U, 4096U, 65535U };
    for (uint32_t pattern = 0U; pattern < 4U; pattern++) {
        for (uint32_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
            if (!test_case(lengths[i], pattern)) return 1;
        }
    }
    puts("w800 deflate tests passed");
    return 0;
}
