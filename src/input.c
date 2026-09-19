/*
 * input.c — joystick + buttons as an NES controller.
 *
 * THE PINS (verified with the on-screen scanner in the mini-mario
 * project, where the board was held in PORTRAIT):
 *
 *   board UP = PC0    board DOWN  = PB4
 *   board LEFT = PB6  board RIGHT = PB0
 *   blue USER button B1 = PC13
 *
 * The emulator, however, shows the picture in LANDSCAPE, so the board
 * is held turned a quarter turn to the left compared with mini-mario.
 * The joystick turns with the board: the contact that used to be at the
 * top now sits on the player's LEFT, the old right contact is now up,
 * and so on. The table below is therefore the portrait map rotated 90
 * degrees counter-clockwise — one turn of the stick in the hand is one
 * turn of the D-pad in the game.
 *
 *   UP = PB0 (board right)     RIGHT = PB4 (board down)
 *   DOWN = PB6 (board left)    LEFT  = PC0 (board up)
 *
 * All switches are active-low. PA0 is permanently pulled low on this
 * shield and must never appear in the map.
 */
#include "input.h"
#include "hal.h"
#include "nes.h"

typedef struct { uint8_t port; uint8_t pin; uint8_t bit; } pin_t;

/* the table carries the NES bit itself, so the order of the rows is
 * cosmetic — reordering them can never silently rewire the pad */
static const pin_t map[] = {
    { PORT_C, 0,  PAD_LEFT  },   /* the board's "up" contact    */
    { PORT_B, 4,  PAD_RIGHT },   /* the board's "down" contact  */
    { PORT_B, 6,  PAD_DOWN  },   /* the board's "left" contact  */
    { PORT_B, 0,  PAD_UP    },   /* the board's "right" contact */
    { PORT_C, 13, PAD_A     },   /* the blue USER button (B1)   */
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

    for (unsigned i = 0; i < N_BUTTONS; i++)
        if (!gpio_read(map[i].port, map[i].pin))
            pad |= map[i].bit;

    /* five switches have to cover the NES's eight buttons, so the blue
     * button alone is A, and the joystick chooses what it means:
     * holding down as well makes it B (run / fire), holding up makes it
     * START (the menus) — pushing the stick while pressing the button is
     * something the games ask for anyway. */
    if (pad & PAD_A) {
        if (pad & PAD_DOWN) pad |= PAD_B;
        if (pad & PAD_UP)   pad |= PAD_START;
    }

    return pad;
}
