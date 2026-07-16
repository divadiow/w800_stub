#include "w800_deflate.h"

#define DEFLATE_WINDOW_SIZE 32768U
#define DEFLATE_WINDOW_MASK (DEFLATE_WINDOW_SIZE - 1U)
#define DEFLATE_HASH_SIZE   4096U
#define DEFLATE_HASH_WAYS   4U

typedef struct {
    w800_deflate_get_byte_fn get_byte;
    void *ctx;
    uint32_t bits;
    uint32_t bit_count;
} bit_input_t;

typedef struct {
    w800_deflate_put_byte_fn put_byte;
    void *ctx;
    uint32_t bits;
    uint32_t bit_count;
} bit_output_t;

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} huffman_t;

static uint8_t inflate_window[DEFLATE_WINDOW_SIZE];
static uint32_t deflate_candidates[DEFLATE_HASH_SIZE][DEFLATE_HASH_WAYS];

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};

static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int read_bits(bit_input_t *in, uint32_t count, uint32_t *value)
{
    while (in->bit_count < count) {
        uint8_t next;
        if (!in->get_byte(in->ctx, &next)) return 0;
        in->bits |= (uint32_t)next << in->bit_count;
        in->bit_count += 8U;
    }
    *value = in->bits & ((1U << count) - 1U);
    in->bits >>= count;
    in->bit_count -= count;
    return 1;
}

static int write_bits(bit_output_t *out, uint32_t value, uint32_t count)
{
    out->bits |= (value & ((1U << count) - 1U)) << out->bit_count;
    out->bit_count += count;
    while (out->bit_count >= 8U) {
        if (!out->put_byte(out->ctx, (uint8_t)out->bits)) return 0;
        out->bits >>= 8;
        out->bit_count -= 8U;
    }
    return 1;
}

static int flush_bits(bit_output_t *out)
{
    if (out->bit_count && !out->put_byte(out->ctx, (uint8_t)out->bits)) return 0;
    out->bits = 0U;
    out->bit_count = 0U;
    return 1;
}

static uint32_t reverse_bits(uint32_t value, uint32_t count)
{
    uint32_t reversed = 0U;
    while (count--) {
        reversed = (reversed << 1) | (value & 1U);
        value >>= 1;
    }
    return reversed;
}

static int build_huffman(huffman_t *table, const uint8_t *lengths, uint32_t symbols)
{
    uint16_t offsets[16];
    int32_t left = 1;

    for (uint32_t i = 0U; i < 16U; i++) table->count[i] = 0U;
    for (uint32_t i = 0U; i < symbols; i++) {
        if (lengths[i] > 15U) return 0;
        table->count[lengths[i]]++;
    }
    if (table->count[0] == symbols) return 0;
    for (uint32_t len = 1U; len <= 15U; len++) {
        left = (left << 1) - table->count[len];
        if (left < 0) return 0;
    }

    offsets[1] = 0U;
    for (uint32_t len = 1U; len < 15U; len++) {
        offsets[len + 1U] = offsets[len] + table->count[len];
    }
    for (uint32_t symbol = 0U; symbol < symbols; symbol++) {
        uint8_t len = lengths[symbol];
        if (len) table->symbol[offsets[len]++] = (uint16_t)symbol;
    }
    return 1;
}

static int decode_symbol(bit_input_t *in, const huffman_t *table, uint32_t *symbol)
{
    uint32_t code = 0U;
    uint32_t first = 0U;
    uint32_t index = 0U;

    for (uint32_t len = 1U; len <= 15U; len++) {
        uint32_t bit;
        if (!read_bits(in, 1U, &bit)) return 0;
        code |= bit;
        uint32_t count = table->count[len];
        if (code >= first && code - first < count) {
            *symbol = table->symbol[index + code - first];
            return 1;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return 0;
}

static void fixed_tables(huffman_t *literal, huffman_t *distance)
{
    uint8_t lengths[288];
    for (uint32_t i = 0U; i <= 143U; i++) lengths[i] = 8U;
    for (uint32_t i = 144U; i <= 255U; i++) lengths[i] = 9U;
    for (uint32_t i = 256U; i <= 279U; i++) lengths[i] = 7U;
    for (uint32_t i = 280U; i <= 287U; i++) lengths[i] = 8U;
    (void)build_huffman(literal, lengths, 288U);
    for (uint32_t i = 0U; i < 32U; i++) lengths[i] = 5U;
    (void)build_huffman(distance, lengths, 32U);
}

static int dynamic_tables(bit_input_t *in, huffman_t *literal, huffman_t *distance)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t code_lengths[19];
    uint8_t lengths[320];
    huffman_t code_table;
    uint32_t value;

    if (!read_bits(in, 5U, &value)) return 0;
    uint32_t literal_count = value + 257U;
    if (!read_bits(in, 5U, &value)) return 0;
    uint32_t distance_count = value + 1U;
    if (!read_bits(in, 4U, &value)) return 0;
    uint32_t code_count = value + 4U;
    if (literal_count > 286U || distance_count > 32U) return 0;

    for (uint32_t i = 0U; i < 19U; i++) code_lengths[i] = 0U;
    for (uint32_t i = 0U; i < code_count; i++) {
        if (!read_bits(in, 3U, &value)) return 0;
        code_lengths[order[i]] = (uint8_t)value;
    }
    if (!build_huffman(&code_table, code_lengths, 19U)) return 0;

    uint32_t total = literal_count + distance_count;
    uint32_t used = 0U;
    while (used < total) {
        uint32_t symbol;
        if (!decode_symbol(in, &code_table, &symbol)) return 0;
        if (symbol < 16U) {
            lengths[used++] = (uint8_t)symbol;
        } else {
            uint32_t repeat;
            uint8_t repeated_length;
            if (symbol == 16U) {
                if (!used || !read_bits(in, 2U, &repeat)) return 0;
                repeat += 3U;
                repeated_length = lengths[used - 1U];
            } else if (symbol == 17U) {
                if (!read_bits(in, 3U, &repeat)) return 0;
                repeat += 3U;
                repeated_length = 0U;
            } else if (symbol == 18U) {
                if (!read_bits(in, 7U, &repeat)) return 0;
                repeat += 11U;
                repeated_length = 0U;
            } else {
                return 0;
            }
            if (repeat > total - used) return 0;
            while (repeat--) lengths[used++] = repeated_length;
        }
    }
    if (!lengths[256] || !build_huffman(literal, lengths, literal_count)) return 0;
    return build_huffman(distance, lengths + literal_count, distance_count);
}

static int inflate_output_byte(w800_deflate_put_byte_fn put_byte, void *put_ctx,
                               uint32_t expected_len, uint32_t *written, uint8_t value)
{
    if (*written >= expected_len) return 0;
    inflate_window[*written & DEFLATE_WINDOW_MASK] = value;
    if (!put_byte(put_ctx, value)) return 0;
    (*written)++;
    return 1;
}

int w800_inflate_raw(w800_deflate_get_byte_fn get_byte, void *get_ctx,
                     w800_deflate_put_byte_fn put_byte, void *put_ctx,
                     uint32_t expected_len)
{
    bit_input_t in = { get_byte, get_ctx, 0U, 0U };
    huffman_t literal;
    huffman_t distance;
    uint32_t written = 0U;
    uint32_t final_block = 0U;

    do {
        uint32_t block_type;
        if (!read_bits(&in, 1U, &final_block) || !read_bits(&in, 2U, &block_type)) return 0;
        if (block_type == 0U) {
            uint8_t lo, hi, inv_lo, inv_hi;
            in.bits = 0U;
            in.bit_count = 0U;
            if (!get_byte(get_ctx, &lo) || !get_byte(get_ctx, &hi) ||
                !get_byte(get_ctx, &inv_lo) || !get_byte(get_ctx, &inv_hi)) return 0;
            uint32_t stored_len = (uint32_t)lo | ((uint32_t)hi << 8);
            uint32_t stored_inv = (uint32_t)inv_lo | ((uint32_t)inv_hi << 8);
            if ((stored_len ^ 0xffffU) != stored_inv) return 0;
            while (stored_len--) {
                uint8_t value;
                if (!get_byte(get_ctx, &value) ||
                    !inflate_output_byte(put_byte, put_ctx, expected_len, &written, value)) return 0;
            }
            continue;
        }
        if (block_type == 1U) fixed_tables(&literal, &distance);
        else if (block_type == 2U) {
            if (!dynamic_tables(&in, &literal, &distance)) return 0;
        } else {
            return 0;
        }

        for (;;) {
            uint32_t symbol;
            if (!decode_symbol(&in, &literal, &symbol)) return 0;
            if (symbol < 256U) {
                if (!inflate_output_byte(put_byte, put_ctx, expected_len, &written, (uint8_t)symbol)) return 0;
            } else if (symbol == 256U) {
                break;
            } else {
                if (symbol < 257U || symbol > 285U) return 0;
                uint32_t length_index = symbol - 257U;
                uint32_t extra = 0U;
                if (length_extra[length_index] &&
                    !read_bits(&in, length_extra[length_index], &extra)) return 0;
                uint32_t match_len = length_base[length_index] + extra;

                uint32_t distance_symbol;
                if (!decode_symbol(&in, &distance, &distance_symbol) || distance_symbol >= 30U) return 0;
                extra = 0U;
                if (distance_extra[distance_symbol] &&
                    !read_bits(&in, distance_extra[distance_symbol], &extra)) return 0;
                uint32_t match_distance = distance_base[distance_symbol] + extra;
                if (!match_distance || match_distance > written || match_len > expected_len - written) return 0;
                while (match_len--) {
                    uint8_t value = inflate_window[(written - match_distance) & DEFLATE_WINDOW_MASK];
                    if (!inflate_output_byte(put_byte, put_ctx, expected_len, &written, value)) return 0;
                }
            }
        }
    } while (!final_block);

    return written == expected_len;
}

static int write_fixed_symbol(bit_output_t *out, uint32_t symbol)
{
    uint32_t code;
    uint32_t count;
    if (symbol <= 143U) {
        code = 0x30U + symbol;
        count = 8U;
    } else if (symbol <= 255U) {
        code = 0x190U + symbol - 144U;
        count = 9U;
    } else if (symbol <= 279U) {
        code = symbol - 256U;
        count = 7U;
    } else if (symbol <= 287U) {
        code = 0xc0U + symbol - 280U;
        count = 8U;
    } else {
        return 0;
    }
    return write_bits(out, reverse_bits(code, count), count);
}

static int write_match(bit_output_t *out, uint32_t length, uint32_t distance)
{
    uint32_t length_index = 0U;
    while (length_index < 28U && length >= length_base[length_index + 1U]) length_index++;
    if (!write_fixed_symbol(out, 257U + length_index)) return 0;
    uint32_t extra_count = length_extra[length_index];
    if (extra_count && !write_bits(out, length - length_base[length_index], extra_count)) return 0;

    uint32_t distance_symbol = 0U;
    while (distance_symbol < 29U && distance >= distance_base[distance_symbol + 1U]) distance_symbol++;
    if (!write_bits(out, reverse_bits(distance_symbol, 5U), 5U)) return 0;
    extra_count = distance_extra[distance_symbol];
    return !extra_count || write_bits(out, distance - distance_base[distance_symbol], extra_count);
}

static uint32_t hash_at(const volatile uint8_t *src, uint32_t pos)
{
    uint32_t hash = src[pos];
    hash = (hash * 251U) ^ src[pos + 1U];
    hash = (hash * 251U) ^ src[pos + 2U];
    return hash & (DEFLATE_HASH_SIZE - 1U);
}

static void remember_position(const volatile uint8_t *src, uint32_t pos, uint32_t len)
{
    if (len - pos < 3U) return;
    uint32_t hash = hash_at(src, pos);
    for (uint32_t i = DEFLATE_HASH_WAYS - 1U; i > 0U; i--) {
        deflate_candidates[hash][i] = deflate_candidates[hash][i - 1U];
    }
    deflate_candidates[hash][0] = pos;
}

int w800_deflate_fixed(const volatile uint8_t *src, uint32_t len, uint8_t level,
                       w800_deflate_put_byte_fn put_byte, void *put_ctx)
{
    bit_output_t out = { put_byte, put_ctx, 0U, 0U };
    uint32_t probes = ((uint32_t)level + 1U) / 2U;
    if (probes < 1U) probes = 1U;
    if (probes > DEFLATE_HASH_WAYS) probes = DEFLATE_HASH_WAYS;

    for (uint32_t hash = 0U; hash < DEFLATE_HASH_SIZE; hash++) {
        for (uint32_t way = 0U; way < DEFLATE_HASH_WAYS; way++) {
            deflate_candidates[hash][way] = 0xffffffffU;
        }
    }
    if (!write_bits(&out, 1U, 1U) || !write_bits(&out, 1U, 2U)) return 0;

    uint32_t pos = 0U;
    while (pos < len) {
        uint32_t best_len = 0U;
        uint32_t best_distance = 0U;
        uint32_t hash = 0U;
        if (len - pos >= 3U) {
            hash = hash_at(src, pos);
            for (uint32_t way = 0U; way < probes; way++) {
                uint32_t candidate = deflate_candidates[hash][way];
                if (candidate == 0xffffffffU || candidate >= pos || pos - candidate > DEFLATE_WINDOW_SIZE) continue;
                uint32_t match_len = 0U;
                uint32_t max_len = len - pos;
                if (max_len > 258U) max_len = 258U;
                while (match_len < max_len && src[candidate + match_len] == src[pos + match_len]) match_len++;
                if (match_len >= 3U && match_len > best_len) {
                    best_len = match_len;
                    best_distance = pos - candidate;
                }
            }
        }
        remember_position(src, pos, len);
        if (best_len >= 3U) {
            if (!write_match(&out, best_len, best_distance)) return 0;
            for (uint32_t i = 1U; i < best_len; i++) remember_position(src, pos + i, len);
            pos += best_len;
        } else {
            if (!write_fixed_symbol(&out, src[pos])) return 0;
            pos++;
        }
    }
    return write_fixed_symbol(&out, 256U) && flush_bits(&out);
}
