/*
 * hal.c — the only file that touches hardware registers.
 *
 *   clock      HSI16 -> PLL x10 / 2 = 80 MHz (no external crystal)
 *   SPI1       40 MHz, mode 0, software chip select
 *   DMA1 ch3   feeds SPI1_TX for the picture bands
 *   DWT        cycle counter used to measure frame times
 */
#include "hal.h"

#define MMIO32(addr) (*(volatile uint32_t *)(addr))

/* --- clocks ------------------------------------------------------- */
#define RCC_BASE     0x40021000UL
#define RCC_CR       MMIO32(RCC_BASE + 0x00)
#define RCC_CFGR     MMIO32(RCC_BASE + 0x08)
#define RCC_PLLCFGR  MMIO32(RCC_BASE + 0x0C)
#define RCC_AHB1ENR  MMIO32(RCC_BASE + 0x48)
#define RCC_AHB2ENR  MMIO32(RCC_BASE + 0x4C)
#define RCC_APB2ENR  MMIO32(RCC_BASE + 0x60)

#define FLASH_ACR    MMIO32(0x40022000UL)

/* --- GPIO --------------------------------------------------------- */
#define GPIO(port)        (0x48000000UL + 0x400UL * (port))
#define GPIO_MODER(p)     MMIO32(GPIO(p) + 0x00)
#define GPIO_OSPEEDR(p)   MMIO32(GPIO(p) + 0x08)
#define GPIO_PUPDR(p)     MMIO32(GPIO(p) + 0x0C)
#define GPIO_IDR(p)       MMIO32(GPIO(p) + 0x10)
#define GPIO_BSRR(p)      MMIO32(GPIO(p) + 0x18)
#define GPIO_BRR(p)       MMIO32(GPIO(p) + 0x28)
#define GPIO_AFRL(p)      MMIO32(GPIO(p) + 0x20)

/* --- SPI1 --------------------------------------------------------- */
#define SPI1_CR1   MMIO32(0x40013000UL + 0x00)
#define SPI1_CR2   MMIO32(0x40013000UL + 0x04)
#define SPI1_SR    MMIO32(0x40013000UL + 0x08)
#define SPI1_DR8   (*(volatile uint8_t *)(0x40013000UL + 0x0C))

/* --- DMA1 --------------------------------------------------------- */
#define DMA1_ISR    MMIO32(0x40020000UL + 0x00)
#define DMA1_IFCR   MMIO32(0x40020000UL + 0x04)
#define DMA1_CSELR  MMIO32(0x40020000UL + 0xA8)
/* DMA1 channel 3 (SPI1_TX): the channel block starts at 0x30 —
 * CCR, CNDTR, CPAR, CMAR in that order */
#define DMA1_C3_CCR   MMIO32(0x40020000UL + 0x30)
#define DMA1_C3_CNDTR MMIO32(0x40020000UL + 0x34)
#define DMA1_C3_CPAR  MMIO32(0x40020000UL + 0x38)
#define DMA1_C3_CMAR  MMIO32(0x40020000UL + 0x3C)
#define DMA_TCIF3     (1u << 9)
#define DMA_CSELR_SPI1_TX 1u     /* request 1 = SPI1_TX on STM32L4 */

/* --- DWT cycle counter -------------------------------------------- */
#define DWT_CTRL   MMIO32(0xE0001000UL)
#define DEMCR      MMIO32(0xE000EDFCUL)

/* ------------------------------------------------------------------ */

static void clock_init(void)
{
    /* 4 wait states at 80 MHz, plus the instruction/data/prefetch caches */
    FLASH_ACR = 4u | (1u << 8) | (1u << 9) | (1u << 10);

    RCC_CR |= (1u << 8);                       /* HSI16 on   */
    while (!(RCC_CR & (1u << 10))) {}
    RCC_PLLCFGR = (2u << 0) | (10u << 8) | (1u << 24);
    RCC_CR |= (1u << 24);                      /* PLL on     */
    while (!(RCC_CR & (1u << 25))) {}
    RCC_CFGR = (3u << 0);                      /* switch     */
    while ((RCC_CFGR & (3u << 2)) != (3u << 2)) {}
}

/* after enabling a peripheral clock, let the writes settle */
static void clk_sync(void)
{
    __asm volatile ("dsb sy" ::: "memory");
}

void system_init(void)
{
    clock_init();

    RCC_AHB2ENR |= (1u << 0) | (1u << 1) | (1u << 2);   /* GPIOA/B/C */
    clk_sync(); (void)RCC_AHB2ENR;

    RCC_APB2ENR |= (1u << 12);                          /* SPI1      */
    clk_sync(); (void)RCC_APB2ENR;

    /* SPI1: master, 8-bit, mode 0, fPCLK/2 = 40 MHz, software NSS */
    SPI1_CR1 = (1u << 2) | (1u << 8) | (1u << 9);
    SPI1_CR2 = (7u << 8);
    SPI1_CR1 |= (1u << 6);

    /* display pins: PA5/6/7 = SPI1 SCK/MISO/MOSI (AF5),
     *               PA9 = CS, PB10 = DC, PA1 = RST                  */
    GPIO_MODER(PORT_A) &= ~((3u << 10) | (3u << 12) | (3u << 14));
    GPIO_MODER(PORT_A) |= (2u << 10) | (2u << 12) | (2u << 14);
    GPIO_AFRL(PORT_A)  &= ~((0xFu << 20) | (0xFu << 24) | (0xFu << 28));
    GPIO_AFRL(PORT_A)  |= (5u << 20) | (5u << 24) | (5u << 28);
    GPIO_OSPEEDR(PORT_A) |= (3u << 10) | (3u << 12) | (3u << 14);

    gpio_output(PORT_A, 9);
    gpio_output(PORT_B, 10);
    gpio_output(PORT_A, 1);
    gpio_set(PORT_A, 9);
    gpio_set(PORT_A, 1);

    cycles_init();
}

void delay_ms(uint32_t ms)
{
    while (ms--)
        for (volatile uint32_t i = 0; i < 12000; i++) {}
}

void gpio_output(uint8_t port, uint8_t pin)
{
    GPIO_MODER(port) &= ~(3u << (pin * 2));
    GPIO_MODER(port) |= (1u << (pin * 2));
    GPIO_OSPEEDR(port) |= (3u << (pin * 2));
}

void gpio_set(uint8_t port, uint8_t pin)   { GPIO_BSRR(port) = (1u << pin); }
void gpio_clear(uint8_t port, uint8_t pin) { GPIO_BRR(port)  = (1u << pin); }

void gpio_input_pullup(uint8_t port, uint8_t pin)
{
    GPIO_MODER(port) &= ~(3u << (pin * 2));
    GPIO_PUPDR(port) &= ~(3u << (pin * 2));
    GPIO_PUPDR(port) |= (1u << (pin * 2));
}

bool gpio_read(uint8_t port, uint8_t pin)
{
    return (GPIO_IDR(port) >> pin) & 1u;
}

void spi_write(const uint8_t *buf, uint32_t n)
{
    while (n--) {
        while (!(SPI1_SR & (1u << 1))) {}     /* TXE */
        SPI1_DR8 = *buf++;
    }
    while (SPI1_SR & (1u << 7)) {}            /* drain (BSY) */
}

/* ---------------------------- SPI DMA ----------------------------- */

static bool dma_ok;

void spi_dma_init(void)
{
    RCC_AHB1ENR |= (1u << 0);                 /* DMA1 clock */
    clk_sync(); (void)RCC_AHB1ENR;

    DMA1_CSELR &= ~(0xFu << 8);               /* channel 3 request */
    DMA1_CSELR |= (DMA_CSELR_SPI1_TX << 8);
    DMA1_C3_CCR = 0;                          /* off while configuring */
    SPI1_CR2 |= (1u << 1);                    /* SPI1 TX DMA enable    */

    /* self test: push a few bytes and see whether the engine actually
     * moves them. The chip select is still high here, so the panel
     * ignores the traffic. If it fails we fall back to CPU-driven
     * transfers rather than hanging in spi_dma_wait(). */
    static uint8_t probe[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    spi_dma_start(probe, sizeof(probe));
    uint32_t t = 200000;
    while (spi_dma_busy() && --t) {}
    dma_ok = (t != 0);
    if (dma_ok) {
        DMA1_C3_CCR &= ~1u;
        while (SPI1_SR & (1u << 7)) {}
    }
}

bool spi_dma_available(void) { return dma_ok; }

void spi_dma_start(const uint8_t *buf, uint32_t n)
{
    DMA1_IFCR = DMA_TCIF3;                    /* clear the done flag   */
    DMA1_C3_CPAR  = (uint32_t)&SPI1_DR8;
    DMA1_C3_CMAR  = (uint32_t)buf;
    DMA1_C3_CNDTR = n;
    /* memory increment, 8-bit, memory -> peripheral, high priority */
    DMA1_C3_CCR = (1u << 7) | (1u << 4) | (1u << 0);
}

bool spi_dma_busy(void)
{
    return (DMA1_ISR & DMA_TCIF3) == 0;
}

void spi_dma_wait(void)
{
    while (spi_dma_busy()) {}
    DMA1_C3_CCR &= ~1u;                       /* channel off */
    while (SPI1_SR & (1u << 7)) {}            /* last byte on the wire */
}

/* ------------------------- cycle counter -------------------------- */

void cycles_init(void)
{
    DEMCR |= (1u << 24);                      /* TRCENA  */
    DWT_CYCCNT = 0;
    DWT_CTRL |= (1u << 0);                    /* CYCCNTENA */
}

/* cycles_now() is a static inline in hal.h (see the comment there) */
