#include "board.h"

#define REG(addr) (*(volatile uint32_t *)(addr))
#define GPIO_BASE 0x60004000u
#define GPIO_SET REG(GPIO_BASE + 0x08)
#define GPIO_CLEAR REG(GPIO_BASE + 0x0c)
#define GPIO_ENABLE REG(GPIO_BASE + 0x24)
#define USB_FIFO REG(0x60043000u)
#define USB_STATUS REG(0x60043004u)

enum {
    PIN_RESET = 3,
    PIN_CLOCK = 4,
    PIN_MOSI = 20,
    PIN_CS = 10,
    PIN_DC = 21,
};

void delay_ms(uint32_t ms)
{
    void (*const rom_delay_us)(uint32_t) = (void *)0x40000050u;
    rom_delay_us(ms * 1000);
}

void log_usb(const char *line)
{
    for (int retry = 0; retry < 40; ++retry) {
        if (USB_STATUS & 2u) {
            while (*line)
                USB_FIFO = (uint8_t)*line++;
            USB_STATUS = 1;
            return;
        }
        delay_ms(1);
    }
}

static void configure_output(unsigned pin)
{
    const uint32_t io_mux = 0x60009004u + 4 * pin;
    const uint32_t pin_config = GPIO_BASE + 0x74 + 4 * pin;
    const uint32_t out_select = GPIO_BASE + 0x554 + 4 * pin;
    REG(io_mux) = (REG(io_mux) & ~((7u << 12) | 2u)) | (1u << 12);
    REG(pin_config) &= ~(1u << 2);
    REG(out_select) = (1u << 9) | 128u;
    GPIO_ENABLE = 1u << pin;
}

static void spi_send(uint8_t byte)
{
    for (uint8_t bit = 0x80; bit != 0; bit >>= 1) {
        if (byte & bit)
            GPIO_SET = 1u << PIN_MOSI;
        else
            GPIO_CLEAR = 1u << PIN_MOSI;
        __asm__ volatile("nop; nop; nop; nop");
        GPIO_SET = 1u << PIN_CLOCK;
        __asm__ volatile("nop; nop; nop; nop");
        GPIO_CLEAR = 1u << PIN_CLOCK;
    }
}

static void command(uint8_t opcode, const uint8_t *params, unsigned count)
{
    GPIO_CLEAR = (1u << PIN_DC) | (1u << PIN_CS);
    spi_send(opcode);
    GPIO_SET = 1u << PIN_DC;
    for (unsigned i = 0; i < count; ++i)
        spi_send(params[i]);
    GPIO_SET = 1u << PIN_CS;
}

#define CONFIG(cmd, ...) do { \
    const uint8_t params[] = { __VA_ARGS__ }; \
    command((cmd), params, sizeof(params)); \
} while (0)

void init_display(void)
{
    GPIO_SET = (1u << PIN_RESET) | (1u << PIN_CS) | (1u << PIN_DC);
    GPIO_CLEAR = (1u << PIN_CLOCK) | (1u << PIN_MOSI);
    configure_output(PIN_RESET);
    configure_output(PIN_CLOCK);
    configure_output(PIN_MOSI);
    configure_output(PIN_CS);
    configure_output(PIN_DC);

    delay_ms(20);
    GPIO_CLEAR = 1u << PIN_RESET;
    delay_ms(20);
    GPIO_SET = 1u << PIN_RESET;
    delay_ms(150);

    command(0x01, 0, 0);  // SWRESET
    delay_ms(150);
    command(0x11, 0, 0);  // SLPOUT
    delay_ms(150);

    // ST7735R panel settings as used by Adafruit's 1.8-inch TFT initializer.
    CONFIG(0xb1, 0x01, 0x2c, 0x2d);
    CONFIG(0xb2, 0x01, 0x2c, 0x2d);
    CONFIG(0xb3, 0x01, 0x2c, 0x2d, 0x01, 0x2c, 0x2d);
    CONFIG(0xb4, 0x07);
    CONFIG(0xc0, 0xa2, 0x02, 0x84);
    CONFIG(0xc1, 0xc5);
    CONFIG(0xc2, 0x0a, 0x00);
    CONFIG(0xc3, 0x8a, 0x2a);
    CONFIG(0xc4, 0x8a, 0xee);
    CONFIG(0xc5, 0x0e);
    command(0x20, 0, 0);  // INVOFF
    CONFIG(0x36, 0xc0);   // MADCTL, portrait RGB
    CONFIG(0x3a, 0x05);   // COLMOD, RGB565
    command(0x13, 0, 0);  // NORON
    delay_ms(10);
    command(0x29, 0, 0);  // DISPON
    delay_ms(100);
}

#define SPI2_BASE 0x60024000u
#define SPI_COMMAND REG(SPI2_BASE)
#define SPI_UPDATE (1u << 23)
#define SPI_START (1u << 24)
#define SPI_BITS REG(SPI2_BASE + 0x1c)
#define SPI_WORD(i) REG(SPI2_BASE + 0x98 + 4u * (i))
#define SPI_MUX(pin) REG(GPIO_BASE + 0x554 + 4u * (pin))
#define SYSTEM_CLOCKS REG(0x600c0010u)
#define SYSTEM_RESETS REG(0x600c0018u)
#define TIMER_CONFIG REG(0x60023000u)
#define TIMER_LATCH REG(0x60023004u)
#define TIMER_VALUE REG(0x60023044u)

static void restart_original(void)
{
    REG(0x600080a8u) = 0x50d83aa1u;
    REG(0x60008094u) = 2000;
    REG(0x60008090u) = (1u << 31) | (5u << 28) | (1u << 8) | 2u;
    REG(0x600080a8u) = 0;
    for (;;)
        __asm__ volatile("nop");
}

static void fail(const char *reason)
{
    log_usb(reason);
    delay_ms(100);
    restart_original();
}

uint32_t timer_ticks(void)
{
    TIMER_LATCH = 1u << 30;
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
        if (TIMER_LATCH & (1u << 29))
            return TIMER_VALUE;
    }
    fail("PET ERROR: timer latch timeout\r\n");
    return 0;
}

static void spi_wait(uint32_t request)
{
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
        if (!(SPI_COMMAND & request))
            return;
    }
    fail("PET ERROR: SPI transaction timeout\r\n");
}

static void spi_trigger(uint32_t request)
{
    SPI_COMMAND = request;
    spi_wait(request);
}

void configure_hardware_spi(unsigned divider)
{
    const uint32_t clock_bit = 1u << 6;
    SYSTEM_CLOCKS |= clock_bit;
    SYSTEM_RESETS |= clock_bit;
    SYSTEM_RESETS &= ~clock_bit;

    // SPI2 master, mode 0, MOSI only, with a GPIO for CS.
    REG(SPI2_BASE + 0xe8) = 3;  // Enable and select the 40 MHz crystal.
    REG(SPI2_BASE + 0xe0) = 0;
    REG(SPI2_BASE + 0x30) = 0;
    REG(SPI2_BASE + 0x20) = 0x3f;
    REG(SPI2_BASE + 0x10) = 1u << 27;
    REG(SPI2_BASE + 0x08) = 0;
    REG(SPI2_BASE + 0x0c) = ((divider - 1u) << 12) |
        ((divider / 2u - 1u) << 6) | (divider - 1u);
    spi_trigger(SPI_UPDATE);

    // Route SCK and MOSI through the GPIO matrix to the existing wires.
    SPI_MUX(PIN_CLOCK) = (1u << 9) | 63u;
    SPI_MUX(PIN_MOSI) = (1u << 9) | 65u;
}

static void hardware_send(uint8_t byte)
{
    SPI_WORD(0) = byte;
    SPI_BITS = 7;
    spi_trigger(SPI_UPDATE);
    spi_trigger(SPI_START);
}

#define TRIG (1u << 7)
#define ECHO (1u << 6)
#define GPIO_INPUT REG(GPIO_BASE + 0x3c)
void init_sonar(void)
{
    REG(0x600c0010u) |= 1u << 29;
    REG(0x600c0018u) &= ~(1u << 29);
    TIMER_CONFIG = (TIMER_CONFIG | (3u << 30)) & ~(1u << 28);
    GPIO_CLEAR = TRIG;
    REG(GPIO_BASE + 0x28) = ECHO;
    REG(0x60009020u) = 1u << 12;
    REG(0x6000901cu) = (1u << 12) | (1u << 9);
    REG(GPIO_BASE + 0x570) = (1u << 9) | 128u;
    GPIO_ENABLE = TRIG;
}

static int wait_echo(int high, uint32_t *when)
{
    const uint32_t start = timer_ticks();
    do {
        *when = timer_ticks();
        if (!!(GPIO_INPUT & ECHO) == high)
            return 1;
    } while (*when - start < 30u * 1000u * 16u);
    return 0;
}

unsigned distance_mm(void)
{
    if (GPIO_INPUT & ECHO)
        return 0;
    GPIO_SET = TRIG;
    ((void (*)(uint32_t))0x40000050u)(12);
    GPIO_CLEAR = TRIG;
    uint32_t rise, fall;
    if (!wait_echo(1, &rise) || !wait_echo(0, &fall))
        return 0;
    return ((fall - rise) * 10u + 464u) / 928u;
}

static void address_range(uint8_t command, uint8_t first, uint8_t last)
{
    GPIO_CLEAR = (1u << PIN_DC) | (1u << PIN_CS);
    hardware_send(command);
    GPIO_SET = 1u << PIN_DC;
    hardware_send(0);
    hardware_send(first);
    hardware_send(0);
    hardware_send(last);
    GPIO_SET = 1u << PIN_CS;
}

void begin_rectangle(unsigned x, unsigned y, unsigned w, unsigned h)
{
    // The display is mounted 180 degrees from its default orientation.
    address_range(0x2a, WIDTH - x - w, WIDTH - x - 1);
    address_range(0x2b, HEIGHT - y - h, HEIGHT - y - 1);
    GPIO_CLEAR = (1u << PIN_DC) | (1u << PIN_CS);
    hardware_send(0x2c);
    GPIO_SET = 1u << PIN_DC;
}

void write_pixels(const uint8_t *bytes, unsigned size)
{
    // LCD rows and packed assets are 32-bit aligned, with 64-byte transactions.
    const uint8_t *aligned = __builtin_assume_aligned(bytes, 4);
    if (SPI_BITS != 511) {
        spi_wait(SPI_START);
        SPI_BITS = 511;
        spi_trigger(SPI_UPDATE);
    }
    for (unsigned offset = 0; offset < size; offset += 64) {
        spi_wait(SPI_START);
        for (unsigned i = 0; i < 64; i += 4) {
            uint32_t word;
            __builtin_memcpy(&word, aligned + offset + i, sizeof(word));
            SPI_WORD(i / 4) = word;
        }
        // Let the last block keep shifting while the next row is composed.
        SPI_COMMAND = SPI_START;
    }
}


void end_rectangle(void)
{
    spi_wait(SPI_START);
    GPIO_SET = 1u << PIN_CS;
}

int usb_read_byte(void)
{
    return USB_STATUS & (1u << 2) ? (int)(USB_FIFO & 0xffu) : -1;
}
