/*
 * hal.h — Hardware Abstraction Layer.
 *
 * The only layer that knows about registers. Everything above it (lcd.c,
 * input.c, the emulator) speaks this API, so porting to another board
 * means rewriting hal.c and nothing else.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* GPIO ports */
#define PORT_A 0
#define PORT_B 1
#define PORT_C 2

void system_init(void);          /* clocks (80 MHz), SPI1, display pins */
void delay_ms(uint32_t ms);

void gpio_output(uint8_t port, uint8_t pin);
void gpio_set(uint8_t port, uint8_t pin);
void gpio_clear(uint8_t port, uint8_t pin);
void gpio_input_pullup(uint8_t port, uint8_t pin);
bool gpio_read(uint8_t port, uint8_t pin);

/* CPU-polled SPI (used for the small command/data transfers) */
void spi_write(const uint8_t *buf, uint32_t n);
/* CPU-polled SPI read (panel RAMRD readback): clocks 0xFF out, captures MISO */
void spi_read(uint8_t *buf, uint32_t n);
void spi_rx_flush(void);
void spi_set_baud(uint32_t br);  /* CR1 BR field: 0 = 40 MHz, 2 = 10 MHz */
uint32_t hal_miso_probe(uint32_t pull);  /* sample PA6 as a GPIO input (0/1/2 = none/up/down) */

/* SPI via DMA (used for the picture bands) */
void spi_dma_init(void);
void spi_dma_start(const uint8_t *buf, uint32_t n);
void spi_dma_wait(void);
bool spi_dma_busy(void);
bool spi_dma_available(void);   /* did the boot self test pass? */

/* cycle counter (DWT) for timing measurements.
 * Inline: the frame accounting calls it a few thousand times per frame, and
 * a call to hal.c costs more than the counter it reads. */
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004UL)
void     cycles_init(void);
static inline uint32_t cycles_now(void) { return DWT_CYCCNT; }

/* Free the pins the joystick shares with the JTAG port (SWD is kept). Call
 * before configuring any joystick pin, or PB4 reads as permanently low. */
void hal_release_jtag_pins(void);
