/*
 * W800 custom RAM flasher stub for OpenBeken/Easy Flasher.
 *
 * This file is W800-specific. It implements two host-facing protocols:
 *   1. WinnerMicro 0x21 frames used by the stock W800 flasher path.
 *   2. The common OBK custom-stub 0xA5 protocol used by the existing custom stubs.
 *
 * Platform-specific work stays behind w800_* helpers. The OBK command numbers are
 * shared protocol values and are deliberately not renamed to a chip family.
 *
 * v0.6 remains read-focused: flash erase/write/download commands are rejected until
 * read-side integration is stable.
 */
#include <stdint.h>
#include <stddef.h>

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

#define W800_FRAME_MAGIC           0x21U

/* WinnerMicro command-frame commands supported by this W800 stub. */
#define W800_CMD_BAUD_CHANGE        0x31U
#define W800_CMD_FLASH_ERASE        0x32U
#define W800_CMD_FLASH_ID           0x3CU
#define W800_CMD_VERSION            0x3EU
#define W800_CMD_RESET              0x3FU
#define W800_CMD_RAW_READ           0x4AU

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
#define OBK_CMD_FLASH_SHA256        0x09U  /* optional in some stubs, not enabled */
#define OBK_CMD_FLASH_CRC32         0x8FU
#define OBK_CMD_FLASH_ID            0x90U
#define OBK_CMD_FLASH_XMODEM_DL     0x91U  /* host -> target flash write */
#define OBK_CMD_FLASH_XMODEM_UL     0x92U  /* target -> host flash read */
#define OBK_CMD_FLASH_XMODEM_UL_Z   0x96U  /* compressed target -> host, not enabled */
#define OBK_CMD_FLASH_XMODEM_DL_Z   0x97U  /* compressed host -> target, not enabled */
#define OBK_CMD_RAW_XMODEM_UL       0x98U  /* target -> host absolute memory read */
#define OBK_CMD_EFUSE_READ          0x99U  /* W800 key-parameter mirror, not proven silicon OTP */

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

#define W800_FLASH_CTRL_BASE       0x40002000U
#define W800_FLASH_CMD_ADDR        (W800_FLASH_CTRL_BASE + 0x000U)
#define W800_FLASH_CMD_START       (W800_FLASH_CTRL_BASE + 0x004U)
#define W800_FLASH_ADDR_REG        (W800_FLASH_CTRL_BASE + 0x010U)
#define W800_FLASH_CMD_START_BIT   0x00000100U
#define W800_RSA_SCRATCH_BASE      0x40000000U

#define W800_WDG_BASE              (HR_APB_BASE_ADDR + 0x1600U)
#define W800_WDG_LOAD_VALUE        (W800_WDG_BASE + 0x00U)
#define W800_WDG_CTRL              (W800_WDG_BASE + 0x08U)
#define W800_WDG_LOCK              (W800_WDG_BASE + 0x40U)

#define PROMPT_IDLE_LOOPS          900000U
#define RX_WAIT_LOOPS              20000000U
#define XMODEM_WAIT_LOOPS          50000000U

static uint8_t cmd_buf[4096];
static uint8_t xmodem_packet[3 + 1024 + 2];
static uint8_t flash_page[FLASH_PAGE_SIZE];
static uint32_t w800_flash_jedec_id_cached;
static uint32_t w800_flash_size_cached;
static int native_xmodem_armed;
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

static void put_le16(uint16_t v)
{
    uart0_putc((uint8_t)(v & 0xFFU));
    uart0_putc((uint8_t)((v >> 8) & 0xFFU));
}

static void put_le32(uint32_t v)
{
    uart0_putc((uint8_t)(v & 0xFFU));
    uart0_putc((uint8_t)((v >> 8) & 0xFFU));
    uart0_putc((uint8_t)((v >> 16) & 0xFFU));
    uart0_putc((uint8_t)((v >> 24) & 0xFFU));
}

static uint16_t crc16_ccitt_false(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;
    while (len--) {
        crc ^= ((uint16_t)(*data++)) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
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

static void w800_send_text(const char *s)
{
    while (*s) uart0_putc((uint8_t)*s++);
}


static char hex_digit(uint8_t v)
{
    v &= 0x0FU;
    return (char)(v < 10U ? ('0' + v) : ('A' + (v - 10U)));
}

static void w800_send_hex8(uint8_t v)
{
    uart0_putc((uint8_t)hex_digit((uint8_t)(v >> 4)));
    uart0_putc((uint8_t)hex_digit(v));
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

static void w800_send_flash_id_text(void)
{
    uint32_t id = w800_flash_jedec_id_cached ? w800_flash_jedec_id_cached : w800_read_flash_jedec_id();
    uint8_t manufacturer = (uint8_t)(id & 0xFFU);
    uint8_t density = (uint8_t)((id >> 16) & 0xFFU);

    w800_send_text("FID:");
    w800_send_hex8(manufacturer);
    uart0_putc(',');
    w800_send_hex8(density);
    uart0_putc('\n');
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

static void w800_watchdog_reset(void)
{
    REG32(W800_WDG_LOCK) = 0x1ACCE551U;
    REG32(W800_WDG_LOAD_VALUE) = 0x100U;
    REG32(W800_WDG_CTRL) = 0x3U;
    REG32(W800_WDG_LOCK) = 1U;
    while (1) { __asm__ volatile ("nop"); }
}

static void w800_read_raw(uint32_t addr, uint32_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = p[i];
        crc = crc32_update_wire(crc, b);
        uart0_putc(b);
    }
    put_le32(crc);
}

static void handle_w800_frame(void)
{
    uint8_t lenb[2], crcb[2];
    if (!uart0_getc_timeout(&lenb[0], RX_WAIT_LOOPS)) return;
    if (!uart0_getc_timeout(&lenb[1], RX_WAIT_LOOPS)) return;
    if (!uart0_getc_timeout(&crcb[0], RX_WAIT_LOOPS)) return;
    if (!uart0_getc_timeout(&crcb[1], RX_WAIT_LOOPS)) return;
    uint16_t frame_len = (uint16_t)lenb[0] | ((uint16_t)lenb[1] << 8);
    uint16_t rx_crc = (uint16_t)crcb[0] | ((uint16_t)crcb[1] << 8);
    if (frame_len < 6U || frame_len > sizeof(cmd_buf)) {
        uart0_putc('S');
        return;
    }
    uint32_t body_len = (uint32_t)frame_len - 2U;
    for (uint32_t i = 0; i < body_len; i++) {
        if (!uart0_getc_timeout(&cmd_buf[i], RX_WAIT_LOOPS)) return;
    }
    if (crc16_ccitt_false(cmd_buf, body_len) != rx_crc) {
        uart0_putc('R');
        return;
    }
    if (body_len < 4U) {
        uart0_putc('S');
        return;
    }
    uint32_t cmd = load_le32(cmd_buf);
    if (cmd == W800_CMD_BAUD_CHANGE) {
        if (body_len >= 8U) {
            uint32_t baud = load_le32(cmd_buf + 4);
            delay_loops(30000);
            uart0_set_baud(baud);
            delay_loops(60000);
            uart0_putc('C');
            delay_loops(5000000);
            native_xmodem_armed = 1;
            prompt_enabled = 1;
            for (int i = 0; i < 4; i++) {
                uart0_putc('C');
                if (i != 3) delay_loops(80000);
            }
        } else {
            uart0_putc('S');
        }
    } else if (cmd == W800_CMD_FLASH_ERASE) {
        if (body_len < 8U || !w800_flash_size_cached) {
            uart0_putc('S');
            return;
        }
        uint32_t start = (uint32_t)cmd_buf[4] | ((uint32_t)cmd_buf[5] << 8);
        uint32_t count = (uint32_t)cmd_buf[6] | ((uint32_t)cmd_buf[7] << 8);
        uint32_t off;
        uint32_t erase_len;
        if ((start & 0x8000U) != 0U) {
            off = (start & 0x7FFFU) * 0x10000U;
            erase_len = count * 0x10000U;
        } else {
            off = start * FLASH_SECTOR_SIZE;
            erase_len = count * FLASH_SECTOR_SIZE;
        }
        if (!range_does_not_wrap(off, erase_len) ||
            off < FLASH_WRITE_MIN_OFFSET || off > w800_flash_size_cached ||
            erase_len > (w800_flash_size_cached - off)) {
            uart0_putc('S');
            return;
        }
        for (uint32_t current = off; current < off + erase_len; current += FLASH_SECTOR_SIZE) {
            w800_flash_erase_sector(current);
            if (!w800_flash_range_is_erased(current, FLASH_SECTOR_SIZE)) {
                uart0_putc('R');
                return;
            }
        }
        for (int i = 0; i < 4; i++) uart0_putc('C');
    } else if (cmd == W800_CMD_FLASH_ID) {
        w800_send_flash_id_text();
    } else if (cmd == W800_CMD_VERSION) {
        w800_send_text("R:W800RAW6");
    } else if (cmd == W800_CMD_RESET) {
        uart0_putc('C');
        delay_loops(300000);
        w800_watchdog_reset();
    } else if (cmd == W800_CMD_RAW_READ) {
        if (body_len >= 12U) {
            uint32_t addr = load_le32(cmd_buf + 4);
            uint32_t len = load_le32(cmd_buf + 8);
            if (range_does_not_wrap(addr, len)) {
                w800_read_raw(addr, len);
            } else {
                uart0_putc('S');
            }
        } else {
            uart0_putc('S');
        }
    } else {
        uart0_putc('S');
    }
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

static int native_program_page(uint32_t off, uint32_t len)
{
    for (uint32_t i = len; i < FLASH_PAGE_SIZE; i++) flash_page[i] = 0xFFU;
    if ((off & (FLASH_SECTOR_SIZE - 1U)) == 0U) {
        w800_flash_erase_sector(off);
        if (!w800_flash_range_is_erased(off, FLASH_SECTOR_SIZE)) return 0;
    }
    w800_flash_program_page(off, flash_page);
    return w800_flash_range_matches(off, flash_page, len);
}

static int xmodem_receive_w800_fls(uint8_t marker)
{
    uint8_t expected_block = 1U;
    uint32_t flash_off = 0U;
    uint32_t image_len = 0U;
    uint32_t image_received = 0U;
    uint32_t image_crc_expected = 0U;
    uint32_t image_crc = 0xFFFFFFFFU;
    uint32_t page_used = 0U;
    int header_valid = 0;

    while (1) {
        if (marker == EOT) {
            if (header_valid && image_received == image_len) {
                if (page_used != 0U && !native_program_page(flash_off + image_received - page_used, page_used)) {
                    uart0_putc(CAN); uart0_putc(CAN); return 0;
                }
                if (image_crc != image_crc_expected) {
                    uart0_putc(CAN); uart0_putc(CAN); return 0;
                }
                uart0_putc(ACK);
                return 1;
            }
            uart0_putc(CAN); uart0_putc(CAN); return 0;
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
                uart0_putc(CAN); uart0_putc(CAN); return 0;
            } else {
                uint32_t data_start = 0U;
                if (!header_valid) {
                    if (block_size < 64U || load_le32(xmodem_packet) != 0xA0FFFF9FU) {
                        uart0_putc(CAN); uart0_putc(CAN); return 0;
                    }
                    uint32_t header_crc = 0xFFFFFFFFU;
                    for (uint32_t i = 0U; i < 60U; i++) header_crc = crc32_update_wire(header_crc, xmodem_packet[i]);
                    if (header_crc != load_le32(&xmodem_packet[60])) {
                        uart0_putc(CAN); uart0_putc(CAN); return 0;
                    }
                    uint32_t image_addr = load_le32(&xmodem_packet[8]);
                    image_len = load_le32(&xmodem_packet[12]);
                    image_crc_expected = load_le32(&xmodem_packet[24]);
                    if (image_addr < FLASH_BASE || !range_does_not_wrap(image_addr, image_len)) {
                        uart0_putc(CAN); uart0_putc(CAN); return 0;
                    }
                    flash_off = image_addr - FLASH_BASE;
                    if (!image_len || !w800_flash_size_cached || flash_off < FLASH_WRITE_MIN_OFFSET ||
                        (flash_off & (FLASH_SECTOR_SIZE - 1U)) != 0U ||
                        flash_off > w800_flash_size_cached || image_len > (w800_flash_size_cached - flash_off)) {
                        uart0_putc(CAN); uart0_putc(CAN); return 0;
                    }
                    header_valid = 1;
                    data_start = 64U;
                }
                for (uint32_t i = data_start; i < block_size && image_received < image_len; i++) {
                    uint8_t value = xmodem_packet[i];
                    flash_page[page_used++] = value;
                    image_crc = crc32_update_wire(image_crc, value);
                    image_received++;
                    if (page_used == FLASH_PAGE_SIZE) {
                        if (!native_program_page(flash_off + image_received - FLASH_PAGE_SIZE, FLASH_PAGE_SIZE)) {
                            uart0_putc(CAN); uart0_putc(CAN); return 0;
                        }
                        page_used = 0U;
                    }
                }
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
        uint32_t baud = 115200U;
        if (data_len >= 4U) baud = load_le32(cmd_buf);
        obk_ack(type, OBK_STATUS_SUCCESS);
        delay_loops(60000);
        uart0_set_baud(baud);
    } else if (type == OBK_CMD_FLASH_ID) {
        obk_send_flash_id_binary(type);
    } else if (type == OBK_CMD_FLASH_CRC32) {
        if (data_len < 8U) { obk_ack(type, OBK_STATUS_LEN_ERROR); return; }
        uint32_t off = load_le32(cmd_buf);
        uint32_t len = load_le32(cmd_buf + 4);
        obk_send_flash_crc32(type, off, len);
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
    } else if (type == OBK_CMD_FLASH_XMODEM_UL_Z ||
               type == OBK_CMD_FLASH_XMODEM_DL_Z || type == OBK_CMD_FLASH_SHA256) {
        obk_ack(type, OBK_STATUS_TYPE_ERROR);
    } else {
        obk_ack(type, OBK_STATUS_TYPE_ERROR);
    }
}

int main(void)
{
    uart0_init();
    w800_flash_probe();

    w800_send_text("W800RAW6\n");

    /* Initial prompt for the W800 upload path, matching the stock stub's post-upload sync shape. */
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
        if (b == W800_FRAME_MAGIC) {
            if (!native_xmodem_armed) prompt_enabled = 0;
            handle_w800_frame();
        } else if (b == OBK_STUB_MAGIC) {
            native_xmodem_armed = 0;
            prompt_enabled = 0;
            handle_obk_frame();
        } else if (native_xmodem_armed && (b == SOH || b == STX)) {
            xmodem_receive_w800_fls(b);
            native_xmodem_armed = 1;
        }
    }
    return 0;
}
