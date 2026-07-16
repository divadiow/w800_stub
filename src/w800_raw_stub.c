/* W800/W806 RAM flasher stub using the common OBK custom-stub protocol. */
#include <stdint.h>
#include <stddef.h>

#include "w800_deflate.h"

#define APB_CLK 40000000U

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define HR_APB_BASE_ADDR           (0x40010000U)
#define HR_UART0_BASE_ADDR         (HR_APB_BASE_ADDR + 0x600U)
#define HR_UART0_LINE_CTRL         (HR_UART0_BASE_ADDR + 0x00U)
#define HR_UART0_FLOW_CTRL         (HR_UART0_BASE_ADDR + 0x04U)
#define HR_UART0_DMA_CTRL          (HR_UART0_BASE_ADDR + 0x08U)
#define HR_UART0_FIFO_CTRL         (HR_UART0_BASE_ADDR + 0x0CU)
#define HR_UART0_BAUD_RATE_CTRL    (HR_UART0_BASE_ADDR + 0x10U)
#define HR_UART0_INT_MASK          (HR_UART0_BASE_ADDR + 0x14U)
#define HR_UART0_FIFO_STATUS       (HR_UART0_BASE_ADDR + 0x1CU)
#define HR_UART0_TX_WIN            (HR_UART0_BASE_ADDR + 0x20U)
#define HR_UART0_RX_WIN            (HR_UART0_BASE_ADDR + 0x30U)

#define ULCON_WL8                  0x03U
#define ULCON_TX_EN                0x40U
#define ULCON_RX_EN                0x80U
#define UFC_TX_FIFO_RESET          0x01U
#define UFC_RX_FIFO_RESET          0x02U
#define UFS_TX_FIFO_CNT_MASK       0x3FU
#define UFS_RX_FIFO_CNT_MASK       0x3C0U
#define UFS_RX_FIFO_CNT_SHIFT      6U

/* Common OBK/Easy-Flasher custom-stub protocol.
 * These values are shared with the existing OBK custom stubs.
 * Handlers below map them onto W800-specific UART/flash/memory operations.
 */
#define OBK_STUB_MAGIC              0xA5U
#define OBK_STUB_ACK_MAGIC          0x5AU

#define OBK_CMD_SYNC                0x00U
#define OBK_CMD_FLASH_ERASE         0x04U
#define OBK_CMD_FLASH_CHIP_ERASE    0x05U
#define OBK_CMD_BAUD_CHANGE         0x07U
#define OBK_CMD_FLASH_SHA256        0x09U
#define OBK_CMD_FLASH_CRC32         0x8FU
#define OBK_CMD_FLASH_ID            0x90U
#define OBK_CMD_FLASH_XMODEM_DL     0x91U  /* host -> target flash write */
#define OBK_CMD_FLASH_XMODEM_UL     0x92U  /* target -> host flash read */
#define OBK_CMD_KV_GET              0x93U  /* unsupported on W800/W806 */
#define OBK_CMD_KV_SET              0x94U  /* unsupported on W800/W806 */
#define OBK_CMD_GET_MAC             0x95U
#define OBK_CMD_FLASH_XMODEM_UL_Z   0x96U  /* compressed target -> host */
#define OBK_CMD_FLASH_XMODEM_DL_Z   0x97U  /* compressed host -> target */
#define OBK_CMD_RAW_XMODEM_UL       0x98U  /* target -> host absolute memory read */
#define OBK_CMD_EFUSE_READ          0x99U  /* unsupported: no silicon eFuse read contract is known */

#define OBK_STATUS_SUCCESS         0x00U
#define OBK_STATUS_ERROR           0x01U
#define OBK_STATUS_ADDR_ERROR      0x02U
#define OBK_STATUS_TYPE_ERROR      0x03U
#define OBK_STATUS_LEN_ERROR       0x04U
#define OBK_STATUS_CRC_ERROR       0x05U

#define SOH 0x01U
#define STX 0x02U
#define EOT 0x04U
#define ACK 0x06U
#define NAK 0x15U
#define CAN 0x18U
#define CRC_MODE 0x43U

#define FLASH_BASE                 0x08000000U
#define FLASH_SECTOR_SIZE          0x1000U
#define FLASH_PAGE_SIZE            0x100U
#define FLASH_WRITE_MIN_OFFSET     0x2000U
#define FT_PARAM_SIZE              132U
#define FT_PARAM_RUNTIME_OFFSET    0x1000U
#define FT_PARAM_MAGIC             0xA0FFFF9FU

#define W800_FLASH_CTRL_BASE       0x40002000U
#define W800_FLASH_CMD_ADDR        (W800_FLASH_CTRL_BASE + 0x000U)
#define W800_FLASH_CMD_START       (W800_FLASH_CTRL_BASE + 0x004U)
#define W800_FLASH_ADDR_REG        (W800_FLASH_CTRL_BASE + 0x010U)
#define W800_FLASH_CMD_START_BIT   0x00000100U
#define W800_RSA_SCRATCH_BASE      0x40000000U

#define PROMPT_IDLE_LOOPS          900000U
#define RX_WAIT_LOOPS              20000000U
#define XMODEM_WAIT_LOOPS          50000000U

static uint8_t cmd_buf[4096];
static uint8_t xmodem_packet[3 + 1024 + 2];
static uint32_t w800_flash_jedec_id_cached;
static uint32_t w800_flash_size_cached;
static int prompt_enabled = 1;

static void delay_loops(volatile uint32_t loops)
{
    while (loops--) {
        __asm__ volatile ("nop");
    }
}

static void uart0_set_baud(uint32_t baud)
{
    if (baud == 0U) baud = 115200U;
    uint32_t div = (APB_CLK / (16U * baud) - 1U);
    uint32_t frac = ((APB_CLK % (baud * 16U)) * 16U / (baud * 16U));
    REG32(HR_UART0_BAUD_RATE_CTRL) = div | (frac << 16);
}

static void uart0_init(void)
{
    uart0_set_baud(115200U);
    REG32(HR_UART0_LINE_CTRL) = ULCON_WL8 | ULCON_TX_EN | ULCON_RX_EN;
    REG32(HR_UART0_FLOW_CTRL) = 0U;
    REG32(HR_UART0_DMA_CTRL) = 0U;
    REG32(HR_UART0_FIFO_CTRL) = 0U;
    REG32(HR_UART0_INT_MASK) = 0xFFFFFFFFU;
}

static int uart0_rx_count(void)
{
    return (int)((REG32(HR_UART0_FIFO_STATUS) & UFS_RX_FIFO_CNT_MASK) >> UFS_RX_FIFO_CNT_SHIFT);
}

static void uart0_putc(uint8_t b)
{
    /* Conservative: wait for TX FIFO to drain. This is slow but robust for early stub work. */
    while (REG32(HR_UART0_FIFO_STATUS) & UFS_TX_FIFO_CNT_MASK) { }
    REG32(HR_UART0_TX_WIN) = (uint32_t)b;
}

static void uart0_write(const void *ptr, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)ptr;
    while (len--) uart0_putc(*p++);
}

static int uart0_getc_timeout(uint8_t *out, uint32_t loops)
{
    while (loops--) {
        if (uart0_rx_count() > 0) {
            *out = (uint8_t)(REG32(HR_UART0_RX_WIN) & 0xFFU);
            return 1;
        }
    }
    return 0;
}

static int uart0_getc_block(uint8_t *out)
{
    while (1) {
        if (uart0_rx_count() > 0) {
            *out = (uint8_t)(REG32(HR_UART0_RX_WIN) & 0xFFU);
            return 1;
        }
    }
}

static uint16_t crc16_xmodem(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0U;
    while (len--) {
        crc ^= ((uint16_t)(*data++)) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* WM wire CRC32: init 0xffffffff, reflected poly, no final xor. */
static uint32_t crc32_update_wire(uint32_t crc, uint8_t b)
{
    crc ^= (uint32_t)b;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
    return crc;
}

static uint8_t obk_crc8_sum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0U;
    while (len--) sum += *data++;
    return (uint8_t)sum;
}

static uint32_t load_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rotr32(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

typedef struct {
    uint32_t state[8];
    uint32_t total_len;
    uint32_t block_len;
    uint8_t block[64];
} sha256_ctx_t;

static const uint32_t sha256_k[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
};

static void sha256_transform(sha256_ctx_t *ctx)
{
    uint32_t w[64];
    for (uint32_t i = 0U; i < 16U; i++) {
        uint32_t j = i * 4U;
        w[i] = ((uint32_t)ctx->block[j] << 24) |
               ((uint32_t)ctx->block[j + 1U] << 16) |
               ((uint32_t)ctx->block[j + 2U] << 8) |
               (uint32_t)ctx->block[j + 3U];
    }
    for (uint32_t i = 16U; i < 64U; i++) {
        uint32_t s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
        uint32_t s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];
    for (uint32_t i = 0U; i < 64U; i++) {
        uint32_t s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    static const uint32_t initial[8] = {
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U
    };
    for (uint32_t i = 0U; i < 8U; i++) ctx->state[i] = initial[i];
    ctx->total_len = 0U;
    ctx->block_len = 0U;
}

static void sha256_update_byte(sha256_ctx_t *ctx, uint8_t value)
{
    ctx->block[ctx->block_len++] = value;
    ctx->total_len++;
    if (ctx->block_len == sizeof(ctx->block)) {
        sha256_transform(ctx);
        ctx->block_len = 0U;
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32])
{
    uint32_t message_len = ctx->total_len;
    sha256_update_byte(ctx, 0x80U);
    while (ctx->block_len != 56U) sha256_update_byte(ctx, 0U);
    uint32_t bit_len_high = message_len >> 29;
    uint32_t bit_len_low = message_len << 3;
    for (int shift = 24; shift >= 0; shift -= 8) sha256_update_byte(ctx, (uint8_t)(bit_len_high >> shift));
    for (int shift = 24; shift >= 0; shift -= 8) sha256_update_byte(ctx, (uint8_t)(bit_len_low >> shift));
    for (uint32_t i = 0U; i < 8U; i++) {
        digest[i * 4U] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4U + 2U] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4U + 3U] = (uint8_t)ctx->state[i];
    }
}

static void obk_ack(uint8_t type, uint8_t status)
{
    uint8_t a[6];
    a[0] = OBK_STUB_ACK_MAGIC;
    a[1] = type;
    a[2] = 0;
    a[3] = 0;
    a[4] = status;
    a[5] = obk_crc8_sum(a, 5);
    uart0_write(a, 6);
}

static void obk_data_reply(uint8_t type, const uint8_t *data, uint16_t len, uint8_t status)
{
    uint8_t h[4];
    h[0] = OBK_STUB_ACK_MAGIC;
    h[1] = type;
    h[2] = (uint8_t)(len & 0xFFU);
    h[3] = (uint8_t)((len >> 8) & 0xFFU);
    uart0_write(h, 4);
    if (len && data) uart0_write(data, len);
    uart0_putc(status);
    uint8_t sum = obk_crc8_sum(h, 4);
    for (uint16_t i = 0; i < len; i++) sum = (uint8_t)(sum + data[i]);
    sum = (uint8_t)(sum + status);
    uart0_putc(sum);
}

static uint32_t w800_read_flash_jedec_id(void)
{
    /* OpenW800 internal-flash driver uses command word 0x2c09F for JEDEC 0x9F,
     * then reads the three ID bytes from the RSA scratch window at 0x40000000.
     */
    REG32(W800_FLASH_CMD_ADDR) = 0x0002C09FU;
    REG32(W800_FLASH_CMD_START) = W800_FLASH_CMD_START_BIT;
    delay_loops(2000);
    return REG32(W800_RSA_SCRATCH_BASE) & 0x00FFFFFFU;
}

static void w800_flash_write_enable(void)
{
    REG32(W800_FLASH_CMD_ADDR) = 0x00000006U;
    REG32(W800_FLASH_CMD_START) = W800_FLASH_CMD_START_BIT;
}

static void w800_flash_erase_sector(uint32_t off)
{
    w800_flash_write_enable();
    REG32(W800_FLASH_CMD_ADDR) = 0x80000820U;
    REG32(W800_FLASH_ADDR_REG) = off & 0x01FFFFFFU;
    REG32(W800_FLASH_CMD_START) = W800_FLASH_CMD_START_BIT;
}

static void w800_flash_program_page(uint32_t off, const uint8_t *data)
{
    for (uint32_t i = 0U; i < FLASH_PAGE_SIZE; i += 4U) {
        uint32_t word = ((uint32_t)data[i]) |
                        ((uint32_t)data[i + 1U] << 8) |
                        ((uint32_t)data[i + 2U] << 16) |
                        ((uint32_t)data[i + 3U] << 24);
        REG32(W800_RSA_SCRATCH_BASE + i) = word;
    }
    w800_flash_write_enable();
    REG32(W800_FLASH_CMD_ADDR) = 0x80FF9002U;
    REG32(W800_FLASH_ADDR_REG) = off & 0x01FFFFFFU;
    REG32(W800_FLASH_CMD_START) = W800_FLASH_CMD_START_BIT;
}

static int w800_flash_range_is_erased(uint32_t off, uint32_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(FLASH_BASE + off);
    for (uint32_t i = 0U; i < len; i++) {
        if (p[i] != 0xFFU) return 0;
    }
    return 1;
}

static int w800_flash_range_matches(uint32_t off, const uint8_t *data, uint32_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(FLASH_BASE + off);
    for (uint32_t i = 0U; i < len; i++) {
        if (p[i] != data[i]) return 0;
    }
    return 1;
}

static uint32_t w800_flash_size_from_jedec(uint32_t id)
{
    uint8_t density = (uint8_t)((id >> 16) & 0xFFU);
    if (density < 0x13U || density > 0x1CU) {
        return 0U;
    }
    return 1UL << density;
}

static void w800_flash_probe(void)
{
    w800_flash_jedec_id_cached = w800_read_flash_jedec_id();
    w800_flash_size_cached = w800_flash_size_from_jedec(w800_flash_jedec_id_cached);
}

static void obk_send_flash_id_binary(uint8_t type)
{
    uint32_t id = w800_flash_jedec_id_cached ? w800_flash_jedec_id_cached : w800_read_flash_jedec_id();
    uint8_t payload[4];
    payload[0] = (uint8_t)(id & 0xFFU);
    payload[1] = (uint8_t)((id >> 8) & 0xFFU);
    payload[2] = (uint8_t)((id >> 16) & 0xFFU);
    payload[3] = 0U;
    obk_data_reply(type, payload, sizeof(payload), OBK_STATUS_SUCCESS);
}

static uint32_t w800_crc32_memory(uint32_t addr, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    for (uint32_t i = 0; i < len; i++) {
        crc = crc32_update_wire(crc, p[i]);
    }
    return crc;
}

static int w800_baud_is_supported(uint32_t baud)
{
    return baud == 115200U || baud == 460800U || baud == 921600U ||
           baud == 1000000U || baud == 2000000U;
}

static int w800_read_factory_mac_at(uint32_t flash_off, uint8_t mac[6])
{
    const volatile uint8_t *param = (const volatile uint8_t *)(uintptr_t)(FLASH_BASE + flash_off);
    uint8_t bytes[FT_PARAM_SIZE];
    for (uint32_t i = 0U; i < sizeof(bytes); i++) bytes[i] = param[i];
    if (load_le32(bytes) != FT_PARAM_MAGIC) return 0;

    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 8U; i < sizeof(bytes); i++) crc = crc32_update_wire(crc, bytes[i]);
    if (crc != load_le32(bytes + 4U)) return 0;

    uint8_t any = 0U;
    uint8_t all_ff = 0xFFU;
    for (uint32_t i = 0U; i < 6U; i++) {
        mac[i] = bytes[8U + i];
        any |= mac[i];
        all_ff &= mac[i];
    }
    return (mac[0] & 1U) == 0U && any != 0U && all_ff != 0xFFU;
}

static int w800_read_factory_mac(uint8_t mac[6])
{
    if (w800_read_factory_mac_at(FT_PARAM_RUNTIME_OFFSET, mac)) return 1;
    return w800_read_factory_mac_at(0U, mac);
}

static int range_does_not_wrap(uint32_t addr, uint32_t len)
{
    return len != 0U && addr <= (UINT32_MAX - (len - 1U));
}

static void obk_send_flash_crc32(uint8_t type, uint32_t off, uint32_t len)
{
    if (!range_does_not_wrap(off, len) || !w800_flash_size_cached ||
        off > w800_flash_size_cached || len > (w800_flash_size_cached - off)) {
        obk_ack(type, OBK_STATUS_ADDR_ERROR);
        return;
    }
    uint32_t crc = w800_crc32_memory(FLASH_BASE + off, len);
    uint8_t payload[4];
    payload[0] = (uint8_t)(crc & 0xFFU);
    payload[1] = (uint8_t)((crc >> 8) & 0xFFU);
    payload[2] = (uint8_t)((crc >> 16) & 0xFFU);
    payload[3] = (uint8_t)((crc >> 24) & 0xFFU);
    obk_data_reply(type, payload, sizeof(payload), OBK_STATUS_SUCCESS);
}

static void obk_send_flash_sha256(uint8_t type, uint32_t off, uint32_t len)
{
    if (!range_does_not_wrap(off, len) || !w800_flash_size_cached ||
        off > w800_flash_size_cached || len > (w800_flash_size_cached - off)) {
        obk_ack(type, OBK_STATUS_ADDR_ERROR);
        return;
    }
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(FLASH_BASE + off);
    for (uint32_t i = 0U; i < len; i++) sha256_update_byte(&ctx, p[i]);
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    obk_data_reply(type, digest, sizeof(digest), OBK_STATUS_SUCCESS);
}

static void xmodem_send_memory(uint32_t addr, uint32_t len)
{
    uint8_t resp;
    int use_crc = 1;
    int use_1k = 1;
    uint8_t block = 1;

    /* Wait for receiver's C or NAK. */
    int receiver_ready = 0;
    for (uint32_t tries = 0U; tries < 100U; tries++) {
        if (uart0_getc_timeout(&resp, XMODEM_WAIT_LOOPS / 50U)) {
            if (resp == CRC_MODE) { use_crc = 1; use_1k = 1; receiver_ready = 1; break; }
            if (resp == NAK) { use_crc = 0; use_1k = 0; receiver_ready = 1; break; }
            if (resp == CAN) return;
        }
    }
    if (!receiver_ready) { uart0_putc(CAN); uart0_putc(CAN); return; }

    uint32_t off = 0U;
    while (off < len) {
        uint32_t block_size = use_1k ? 1024U : 128U;
        uint32_t chunk = (len - off > block_size) ? block_size : (len - off);
        xmodem_packet[0] = use_1k ? STX : SOH;
        xmodem_packet[1] = block;
        xmodem_packet[2] = (uint8_t)~block;
        const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)(addr + off);
        for (uint32_t i = 0; i < block_size; i++) {
            xmodem_packet[3 + i] = (i < chunk) ? src[i] : 0xFFU;
        }
        uint32_t pkt_len = 3U + block_size;
        if (use_crc) {
            uint16_t crc = crc16_xmodem(&xmodem_packet[3], block_size);
            xmodem_packet[pkt_len++] = (uint8_t)(crc >> 8);
            xmodem_packet[pkt_len++] = (uint8_t)(crc & 0xFFU);
        } else {
            uint8_t sum = 0;
            for (uint32_t i = 0; i < block_size; i++) sum = (uint8_t)(sum + xmodem_packet[3 + i]);
            xmodem_packet[pkt_len++] = sum;
        }

        int sent = 0;
        for (int retry = 0; retry < 10; retry++) {
            uart0_write(xmodem_packet, pkt_len);
            if (uart0_getc_timeout(&resp, XMODEM_WAIT_LOOPS)) {
                if (resp == ACK) { sent = 1; break; }
                if (resp == CAN) return;
            }
        }
        if (!sent) { uart0_putc(CAN); uart0_putc(CAN); return; }
        off += chunk;
        block++;
    }

    for (int retry = 0; retry < 10; retry++) {
        uart0_putc(EOT);
        if (uart0_getc_timeout(&resp, XMODEM_WAIT_LOOPS)) {
            if (resp == ACK) return;
        }
    }
    uart0_putc(CAN); uart0_putc(CAN);
}

typedef struct {
    uint32_t data_len;
    uint8_t block;
    uint8_t use_crc;
    uint8_t use_1k;
    uint8_t failed;
} xmodem_tx_stream_t;

static int xmodem_tx_send_packet(xmodem_tx_stream_t *stream)
{
    uint8_t resp;
    uint32_t block_size = stream->use_1k ? 1024U : 128U;
    for (uint32_t i = stream->data_len; i < block_size; i++) xmodem_packet[3U + i] = 0xFFU;
    xmodem_packet[0] = stream->use_1k ? STX : SOH;
    xmodem_packet[1] = stream->block;
    xmodem_packet[2] = (uint8_t)~stream->block;
    uint32_t packet_len = 3U + block_size;
    if (stream->use_crc) {
        uint16_t crc = crc16_xmodem(&xmodem_packet[3], block_size);
        xmodem_packet[packet_len++] = (uint8_t)(crc >> 8);
        xmodem_packet[packet_len++] = (uint8_t)crc;
    } else {
        uint8_t sum = 0U;
        for (uint32_t i = 0U; i < block_size; i++) sum = (uint8_t)(sum + xmodem_packet[3U + i]);
        xmodem_packet[packet_len++] = sum;
    }
    for (uint32_t retry = 0U; retry < 10U; retry++) {
        uart0_write(xmodem_packet, packet_len);
        if (uart0_getc_timeout(&resp, XMODEM_WAIT_LOOPS)) {
            if (resp == ACK) {
                stream->block++;
                stream->data_len = 0U;
                return 1;
            }
            if (resp == CAN) break;
        }
    }
    stream->failed = 1U;
    uart0_putc(CAN);
    uart0_putc(CAN);
    return 0;
}

static int xmodem_tx_start(xmodem_tx_stream_t *stream)
{
    uint8_t resp;
    stream->data_len = 0U;
    stream->block = 1U;
    stream->use_crc = 1U;
    stream->use_1k = 1U;
    stream->failed = 0U;
    for (uint32_t tries = 0U; tries < 100U; tries++) {
        if (uart0_getc_timeout(&resp, XMODEM_WAIT_LOOPS / 50U)) {
            if (resp == CRC_MODE) return 1;
            if (resp == NAK) {
                stream->use_crc = 0U;
                stream->use_1k = 0U;
                return 1;
            }
            if (resp == CAN) break;
        }
    }
    stream->failed = 1U;
    uart0_putc(CAN);
    uart0_putc(CAN);
    return 0;
}

static int xmodem_tx_put_byte(void *ctx, uint8_t value)
{
    xmodem_tx_stream_t *stream = (xmodem_tx_stream_t *)ctx;
    if (stream->failed) return 0;
    xmodem_packet[3U + stream->data_len++] = value;
    uint32_t block_size = stream->use_1k ? 1024U : 128U;
    return stream->data_len < block_size || xmodem_tx_send_packet(stream);
}

static int xmodem_tx_finish(xmodem_tx_stream_t *stream)
{
    uint8_t resp;
    if (stream->failed) return 0;
    if (stream->data_len && !xmodem_tx_send_packet(stream)) return 0;
    for (uint32_t retry = 0U; retry < 10U; retry++) {
        uart0_putc(EOT);
        if (uart0_getc_timeout(&resp, XMODEM_WAIT_LOOPS) && resp == ACK) return 1;
    }
    uart0_putc(CAN);
    uart0_putc(CAN);
    return 0;
}

static int xmodem_send_compressed_memory(uint32_t addr, uint32_t len, uint8_t level)
{
    xmodem_tx_stream_t stream;
    if (!xmodem_tx_start(&stream)) return 0;
    if (!w800_deflate_fixed((const volatile uint8_t *)(uintptr_t)addr, len, level,
                            xmodem_tx_put_byte, &stream)) {
        if (!stream.failed) {
            uart0_putc(CAN);
            uart0_putc(CAN);
        }
        return 0;
    }
    return xmodem_tx_finish(&stream);
}

typedef struct {
    uint32_t data_pos;
    uint32_t data_len;
    uint8_t expected_block;
    uint8_t started;
    uint8_t pending_ack;
    uint8_t failed;
} xmodem_rx_stream_t;

static int xmodem_rx_next_packet(xmodem_rx_stream_t *stream)
{
    uint8_t marker;
    if (stream->pending_ack) {
        uart0_putc(ACK);
        stream->pending_ack = 0U;
    }
    for (;;) {
        if (!stream->started) {
            uint32_t tries;
            for (tries = 0U; tries < 100U; tries++) {
                uart0_putc(CRC_MODE);
                if (uart0_getc_timeout(&marker, XMODEM_WAIT_LOOPS / 50U)) break;
            }
            if (tries == 100U) goto fail;
            stream->started = 1U;
        } else if (!uart0_getc_timeout(&marker, XMODEM_WAIT_LOOPS)) {
            goto fail;
        }
        if (marker == CAN || marker == EOT) goto fail;
        if (marker != SOH && marker != STX) {
            uart0_putc(NAK);
            continue;
        }

        uint32_t block_size = marker == STX ? 1024U : 128U;
        uint8_t block_no, block_inv, crc_hi, crc_lo;
        if (!uart0_getc_timeout(&block_no, XMODEM_WAIT_LOOPS) ||
            !uart0_getc_timeout(&block_inv, XMODEM_WAIT_LOOPS)) goto fail;
        for (uint32_t i = 0U; i < block_size; i++) {
            if (!uart0_getc_timeout(&xmodem_packet[i], XMODEM_WAIT_LOOPS)) goto fail;
        }
        if (!uart0_getc_timeout(&crc_hi, XMODEM_WAIT_LOOPS) ||
            !uart0_getc_timeout(&crc_lo, XMODEM_WAIT_LOOPS)) goto fail;
        uint16_t received_crc = ((uint16_t)crc_hi << 8) | crc_lo;
        if (block_inv != (uint8_t)~block_no ||
            received_crc != crc16_xmodem(xmodem_packet, block_size)) {
            uart0_putc(NAK);
            continue;
        }
        if (block_no == (uint8_t)(stream->expected_block - 1U)) {
            uart0_putc(ACK);
            continue;
        }
        if (block_no != stream->expected_block) goto fail;
        stream->expected_block++;
        stream->data_pos = 0U;
        stream->data_len = block_size;
        stream->pending_ack = 1U;
        return 1;
    }

fail:
    stream->failed = 1U;
    uart0_putc(CAN);
    uart0_putc(CAN);
    return 0;
}

static int xmodem_rx_get_byte(void *ctx, uint8_t *value)
{
    xmodem_rx_stream_t *stream = (xmodem_rx_stream_t *)ctx;
    if (stream->failed) return 0;
    if (stream->data_pos == stream->data_len && !xmodem_rx_next_packet(stream)) return 0;
    *value = xmodem_packet[stream->data_pos++];
    return 1;
}

static int xmodem_rx_finish(xmodem_rx_stream_t *stream)
{
    uint8_t marker;
    if (stream->failed) return 0;
    stream->data_pos = stream->data_len;
    if (stream->pending_ack) {
        uart0_putc(ACK);
        stream->pending_ack = 0U;
    }
    if (uart0_getc_timeout(&marker, XMODEM_WAIT_LOOPS) && marker == EOT) {
        uart0_putc(ACK);
        return 1;
    }
    uart0_putc(CAN);
    uart0_putc(CAN);
    return 0;
}

typedef struct {
    uint32_t off;
    uint32_t expected_len;
    uint32_t written;
    uint32_t page_start;
    uint32_t page_len;
    uint8_t page[FLASH_PAGE_SIZE];
} inflate_flash_t;

static int inflate_flash_flush(inflate_flash_t *flash)
{
    if (!flash->page_len) return 1;
    int all_erased = 1;
    for (uint32_t i = 0U; i < flash->page_len; i++) {
        if (flash->page[i] != 0xFFU) {
            all_erased = 0;
            break;
        }
    }
    if (all_erased) {
        flash->page_len = 0U;
        return 1;
    }
    for (uint32_t i = flash->page_len; i < FLASH_PAGE_SIZE; i++) flash->page[i] = 0xFFU;
    w800_flash_program_page(flash->page_start, flash->page);
    if (!w800_flash_range_matches(flash->page_start, flash->page, flash->page_len)) return 0;
    flash->page_len = 0U;
    return 1;
}

static int inflate_flash_put_byte(void *ctx, uint8_t value)
{
    inflate_flash_t *flash = (inflate_flash_t *)ctx;
    if (flash->written >= flash->expected_len) return 0;
    if (!flash->page_len) {
        flash->page_start = flash->off + flash->written;
        if ((flash->page_start & (FLASH_SECTOR_SIZE - 1U)) == 0U) {
            if (!w800_flash_range_is_erased(flash->page_start, FLASH_SECTOR_SIZE)) {
                w800_flash_erase_sector(flash->page_start);
                if (!w800_flash_range_is_erased(flash->page_start, FLASH_SECTOR_SIZE)) return 0;
            }
        }
    }
    flash->page[flash->page_len++] = value;
    flash->written++;
    return flash->page_len < FLASH_PAGE_SIZE || inflate_flash_flush(flash);
}

static int xmodem_receive_compressed_flash(uint32_t off, uint32_t len)
{
    xmodem_rx_stream_t stream = { 0U, 0U, 1U, 0U, 0U, 0U };
    inflate_flash_t flash;
    flash.off = off;
    flash.expected_len = len;
    flash.written = 0U;
    flash.page_start = 0U;
    flash.page_len = 0U;
    if (!w800_inflate_raw(xmodem_rx_get_byte, &stream, inflate_flash_put_byte, &flash, len) ||
        !inflate_flash_flush(&flash) || flash.written != len) {
        if (!stream.failed) {
            uart0_putc(CAN);
            uart0_putc(CAN);
        }
        return 0;
    }
    return xmodem_rx_finish(&stream);
}

static int xmodem_receive_flash(uint32_t off, uint32_t len)
{
    uint8_t expected_block = 1U;
    uint32_t written = 0U;
    uint8_t marker = 0U;

    for (uint32_t tries = 0U; tries < 100U; tries++) {
        uart0_putc(CRC_MODE);
        if (uart0_getc_timeout(&marker, XMODEM_WAIT_LOOPS / 50U)) break;
    }
    if (marker != SOH && marker != STX) {
        uart0_putc(CAN);
        uart0_putc(CAN);
        return 0;
    }

    while (1) {
        if (marker == EOT) {
            if (written == len) {
                uart0_putc(ACK);
                return 1;
            }
            uart0_putc(CAN);
            uart0_putc(CAN);
            return 0;
        }
        if (marker == CAN) return 0;
        if (marker != SOH && marker != STX) {
            uart0_putc(NAK);
        } else {
            uint32_t block_size = marker == STX ? 1024U : 128U;
            uint8_t block_no, block_inv, crc_hi, crc_lo;
            if (!uart0_getc_timeout(&block_no, XMODEM_WAIT_LOOPS) ||
                !uart0_getc_timeout(&block_inv, XMODEM_WAIT_LOOPS)) return 0;
            for (uint32_t i = 0U; i < block_size; i++) {
                if (!uart0_getc_timeout(&xmodem_packet[i], XMODEM_WAIT_LOOPS)) return 0;
            }
            if (!uart0_getc_timeout(&crc_hi, XMODEM_WAIT_LOOPS) ||
                !uart0_getc_timeout(&crc_lo, XMODEM_WAIT_LOOPS)) return 0;
            uint16_t received_crc = ((uint16_t)crc_hi << 8) | crc_lo;
            uint16_t calculated_crc = crc16_xmodem(xmodem_packet, block_size);
            if (block_inv != (uint8_t)~block_no || received_crc != calculated_crc) {
                uart0_putc(NAK);
            } else if (block_no == (uint8_t)(expected_block - 1U)) {
                uart0_putc(ACK);
            } else if (block_no != expected_block) {
                uart0_putc(CAN);
                uart0_putc(CAN);
                return 0;
            } else {
                if (written >= len) {
                    uart0_putc(CAN);
                    uart0_putc(CAN);
                    return 0;
                }
                uint32_t chunk = (len - written > block_size) ? block_size : (len - written);
                uint32_t page_off = 0U;
                while (page_off < chunk) {
                    uint32_t current_off = off + written + page_off;
                    if ((current_off & (FLASH_SECTOR_SIZE - 1U)) == 0U) {
                        w800_flash_erase_sector(current_off);
                        if (!w800_flash_range_is_erased(current_off, FLASH_SECTOR_SIZE)) {
                            uart0_putc(CAN);
                            uart0_putc(CAN);
                            return 0;
                        }
                    }
                    uint32_t page_len = chunk - page_off;
                    if (page_len > FLASH_PAGE_SIZE) page_len = FLASH_PAGE_SIZE;
                    for (uint32_t i = page_len; i < FLASH_PAGE_SIZE; i++) xmodem_packet[page_off + i] = 0xFFU;
                    w800_flash_program_page(current_off, &xmodem_packet[page_off]);
                    if (!w800_flash_range_matches(current_off, &xmodem_packet[page_off], page_len)) {
                        uart0_putc(CAN);
                        uart0_putc(CAN);
                        return 0;
                    }
                    page_off += page_len;
                }
                written += chunk;
                expected_block++;
                uart0_putc(ACK);
            }
        }
        if (!uart0_getc_timeout(&marker, XMODEM_WAIT_LOOPS)) return 0;
    }
}

static void handle_obk_frame(void)
{
    uint8_t type, l0, l1, crc;
    if (!uart0_getc_timeout(&type, RX_WAIT_LOOPS)) return;
    if (!uart0_getc_timeout(&l0, RX_WAIT_LOOPS)) return;
    if (!uart0_getc_timeout(&l1, RX_WAIT_LOOPS)) return;
    uint16_t data_len = (uint16_t)l0 | ((uint16_t)l1 << 8);
    if (data_len > sizeof(cmd_buf)) {
        obk_ack(type, OBK_STATUS_LEN_ERROR);
        return;
    }
    for (uint32_t i = 0; i < data_len; i++) {
        if (!uart0_getc_timeout(&cmd_buf[i], RX_WAIT_LOOPS)) return;
    }
    if (!uart0_getc_timeout(&crc, RX_WAIT_LOOPS)) return;

    uint8_t sum = OBK_STUB_MAGIC + type + l0 + l1;
    for (uint32_t i = 0; i < data_len; i++) sum = (uint8_t)(sum + cmd_buf[i]);
    if (sum != crc) {
        obk_ack(type, OBK_STATUS_CRC_ERROR);
        return;
    }

    if (type == OBK_CMD_SYNC) {
        obk_ack(type, OBK_STATUS_SUCCESS);
    } else if (type == OBK_CMD_BAUD_CHANGE) {
        if (data_len != 4U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t baud = load_le32(cmd_buf);
        if (!w800_baud_is_supported(baud)) { obk_ack(type, OBK_STATUS_TYPE_ERROR); return; }
        obk_ack(type, OBK_STATUS_SUCCESS);
        delay_loops(60000);
        uart0_set_baud(baud);
    } else if (type == OBK_CMD_FLASH_ID) {
        obk_send_flash_id_binary(type);
    } else if (type == OBK_CMD_FLASH_SHA256) {
        if (data_len != 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t off = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        obk_send_flash_sha256(type, off, len);
    } else if (type == OBK_CMD_FLASH_CRC32) {
        if (data_len != 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t off = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        obk_send_flash_crc32(type, off, len);
    } else if (type == OBK_CMD_GET_MAC) {
        if (data_len != 0U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint8_t mac[6];
        if (!w800_read_factory_mac(mac)) {
            obk_ack(type, OBK_STATUS_ERROR);
            return;
        }
        obk_data_reply(type, mac, sizeof(mac), OBK_STATUS_SUCCESS);
    } else if (type == OBK_CMD_FLASH_ERASE) {
        if (data_len < 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t off = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        if (!range_does_not_wrap(off, len) || !w800_flash_size_cached ||
            off < FLASH_WRITE_MIN_OFFSET ||
            (off & (FLASH_SECTOR_SIZE - 1U)) != 0U ||
            (len & (FLASH_SECTOR_SIZE - 1U)) != 0U ||
            off > w800_flash_size_cached || len > (w800_flash_size_cached - off)) {
            obk_ack(type, OBK_STATUS_ADDR_ERROR);
            return;
        }
        for (uint32_t current = off; current < off + len; current += FLASH_SECTOR_SIZE) {
            w800_flash_erase_sector(current);
            if (!w800_flash_range_is_erased(current, FLASH_SECTOR_SIZE)) {
                obk_ack(type, OBK_STATUS_ERROR);
                return;
            }
        }
        obk_ack(type, OBK_STATUS_SUCCESS);
    } else if (type == OBK_CMD_FLASH_XMODEM_DL) {
        if (data_len < 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t off = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        if (!range_does_not_wrap(off, len) || !w800_flash_size_cached ||
            off < FLASH_WRITE_MIN_OFFSET ||
            (off & (FLASH_SECTOR_SIZE - 1U)) != 0U ||
            off > w800_flash_size_cached || len > (w800_flash_size_cached - off)) {
            obk_ack(type, OBK_STATUS_ADDR_ERROR);
            return;
        }
        obk_ack(type, OBK_STATUS_SUCCESS);
        xmodem_receive_flash(off, len);
    } else if (type == OBK_CMD_FLASH_XMODEM_UL) {
        if (data_len < 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t off = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        if (!range_does_not_wrap(off, len) || !w800_flash_size_cached ||
            off > w800_flash_size_cached || len > (w800_flash_size_cached - off)) {
            obk_ack(type, OBK_STATUS_ADDR_ERROR);
            return;
        }
        obk_ack(type, OBK_STATUS_SUCCESS);
        xmodem_send_memory(FLASH_BASE + off, len);
    } else if (type == OBK_CMD_FLASH_XMODEM_UL_Z) {
        if (data_len < 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t off = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        uint8_t level = data_len >= 9U ? cmd_buf[8] : 5U;
        if (!range_does_not_wrap(off, len) || !w800_flash_size_cached ||
            off > w800_flash_size_cached || len > (w800_flash_size_cached - off)) {
            obk_ack(type, OBK_STATUS_ADDR_ERROR);
            return;
        }
        obk_ack(type, OBK_STATUS_SUCCESS);
        xmodem_send_compressed_memory(FLASH_BASE + off, len, level);
    } else if (type == OBK_CMD_FLASH_XMODEM_DL_Z) {
        if (data_len < 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t off = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        if (!range_does_not_wrap(off, len) || !w800_flash_size_cached ||
            off < FLASH_WRITE_MIN_OFFSET ||
            off > w800_flash_size_cached || len > (w800_flash_size_cached - off)) {
            obk_ack(type, OBK_STATUS_ADDR_ERROR);
            return;
        }
        obk_ack(type, OBK_STATUS_SUCCESS);
        xmodem_receive_compressed_flash(off, len);
    } else if (type == OBK_CMD_RAW_XMODEM_UL) {
        if (data_len < 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t addr = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        if (!range_does_not_wrap(addr, len)) {
            obk_ack(type, OBK_STATUS_ADDR_ERROR);
            return;
        }
        obk_ack(type, OBK_STATUS_SUCCESS);
        xmodem_send_memory(addr, len);
    } else if (type == OBK_CMD_EFUSE_READ) {
        /* No silicon eFuse payload contract has been established for W800. */
        obk_ack(type, OBK_STATUS_TYPE_ERROR);
    } else if (type == OBK_CMD_FLASH_CHIP_ERASE) {
        if (!w800_flash_size_cached) {
            obk_ack(type, OBK_STATUS_ERROR);
            return;
        }
        for (uint32_t current = FLASH_WRITE_MIN_OFFSET;
             current < w800_flash_size_cached;
             current += FLASH_SECTOR_SIZE) {
            w800_flash_erase_sector(current);
            if (!w800_flash_range_is_erased(current, FLASH_SECTOR_SIZE)) {
                obk_ack(type, OBK_STATUS_ERROR);
                return;
            }
        }
        obk_ack(type, OBK_STATUS_SUCCESS);
    } else if (type == OBK_CMD_KV_GET || type == OBK_CMD_KV_SET) {
        obk_ack(type, OBK_STATUS_TYPE_ERROR);
    } else {
        obk_ack(type, OBK_STATUS_TYPE_ERROR);
    }
}

int main(void)
{
    uart0_init();
    w800_flash_probe();

    /* The W800 ROM upload backend waits for this prompt before using 0xA5 commands. */
    for (int i = 0; i < 4; i++) {
        uart0_putc('C');
        delay_loops(80000);
    }

    while (1) {
        uint8_t b;
        if (!uart0_getc_timeout(&b, PROMPT_IDLE_LOOPS)) {
            if (prompt_enabled) uart0_putc('C');
            continue;
        }
        if (b == OBK_STUB_MAGIC) {
            prompt_enabled = 0;
            handle_obk_frame();
        }
    }
    return 0;
}
