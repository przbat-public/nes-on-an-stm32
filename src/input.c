/*
 * input.c — joystick + buttons as an NES controller.
 *
 * THE PIN MAP (single source of truth, verified with the on-screen
 * scanner in the mini-mario project):
 *
 *   LEFT  = PB6      RIGHT = PB0
 *   DOWN  = PB4      UP    = PC0
 *   A     = PC13 (the blue USER button) — jump / fire, like an NES A
 *   B     = PB4 (joystick DOWN)         — run / fire, like an NES B
 *   START = PC13 + DOWN together, or just tap the blue button on menus
 *
 * All switches are active-low. PA0 is permanently pulled low on this
 * shield and must never appear in the map.
 */
#include "input.h"
#include "hal.h"
#include "nes.h"

typedef struct { uint8_t port; uint8_t pin; } pin_t;

static const pin_t map[] = {
    /* 0 */ { PORT_B, 6  },   /* LEFT  */
    /* 1 */ { PORT_B, 0  },   /* RIGHT */
    /* 2 */ { PORT_B, 4  },   /* DOWN  */
    /* 3 */ { PORT_C, 0  },   /* UP    */
    /* 4 */ { PORT_C, 13 },   /* A     */
};
#define N_BUTTONS (sizeof(map) / sizeof(map[0]))

static void jtag_release(void)
{
    volatile uint32_t *rcc = (volatile uint32_t *)(0x40021000UL + 0x60);
    *rcc |= (1u << 0);                          /* SYSCFGEN */
    __asm volatile ("dsb sy" ::: "memory");
    volatile uint32_t *cfgr1 = (volatile uint32_t *)(0x40010000UL);
    *cfgr1 = (*cfgr1 & ~(7u << 24)) | (2u << 24);   /* JTAG off, SWD on */
}

void input_init(void)
{
    jtag_release();
    for (unsigned i = 0; i < N_BUTTONS; i++)
        gpio_input_pullup(map[i].port, map[i].pin);
}

uint8_t input_pad(void)
{
    uint8_t pad = 0;
    bool left  = !gpio_read(map[0].port, map[0].pin);
    bool right = !gpio_read(map[1].port, map[1].pin);
    bool down  = !gpio_read(map[2].port, map[2].pin);
    bool up    = !gpio_read(map[3].port, map[3].pin);
    bool a     = !gpio_read(map[4].port, map[4].pin);

    if (left)  pad |= PAD_LEFT;
    if (right) pad |= PAD_RIGHT;
    if (up)    pad |= PAD_UP;
    if (down)  pad |= PAD_DOWN;
    if (a)     pad |= PAD_A;

    /* B = DOWN while also holding A (so the joystick alone still walks
     * and the single button can do both jump and run) */
    if (a && down) pad |= PAD_B;

    /* START: joystick pushed up + button, or just UP held for menus */
    if (up && a)   pad |= PAD_START;

    return pad;
}
