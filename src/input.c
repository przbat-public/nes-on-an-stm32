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
 * The NES pad has eight inputs and this shield gives us five: four
 * directions and one button. Which of A and B the button means is decided
 * by the layout, and the layout is chosen at reset — hold the blue button
 * while the board boots for the second one:
 *
 *   gamepad (default)   A = blue            jump / act
 *                       B = blue + down     run / secondary
 *                       START = blue + up   held for a moment
 *
 *   shooter             B = blue            fire — the button you hold all
 *                                           the time while running
 *                       A = blue + up       jump
 *                       START = blue + down held for a moment
 *
 * The shooter layout exists because games like Contra put *fire* on B:
 * with it on a combination the player could not shoot while running.
 *
 * All switches are active-low. PA0 is permanently pulled low on this
 * shield and must never appear in the map.
 */
#include "input.h"
#include "hal.h"
#include "nes.h"

#define START_HOLD 12           /* ~0.7 s at this emulator's frame rate */

typedef struct { uint8_t port; uint8_t pin; uint8_t bit; } pin_t;

static bool    shooter_layout;      /* chosen in input_init()  */
static uint8_t start_hold;          /* frames the START combo is held */

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

    /* Which layout is active is decided here, before the game starts:
     * hold the blue button while the board comes out of reset. */
    unsigned held = 0;
    for (unsigned i = 0; i < 2000; i++)
        if (!gpio_read(PORT_C, 13)) held++;
    shooter_layout = (held > 1000);
}

uint8_t input_pad(void)
{
    uint8_t pad = 0;

    for (unsigned i = 0; i < N_BUTTONS; i++)
        if (!gpio_read(map[i].port, map[i].pin))
            pad |= map[i].bit;

    /* The blue button arrives on the A bit; what it means depends on the
     * layout (see the top of this file). START needs the combination to
     * be *held* for a moment: a quick tap of fire while ducking must not
     * pause the game. */
    bool blue = (pad & PAD_A) != 0;
    bool up   = (pad & PAD_UP) != 0;
    bool down = (pad & PAD_DOWN) != 0;

    if (shooter_layout) {
        pad &= (uint8_t)~PAD_A;              /* the button is B here */
        if (blue) pad |= PAD_B;              /* fire */
        if (blue && up) pad |= PAD_A;        /* jump, as shooters want it */
        if (blue && down) {
            if (++start_hold >= START_HOLD) pad |= PAD_START;
        } else {
            start_hold = 0;
        }
    } else {
        if (blue && down) pad |= PAD_B;      /* secondary action */
        if (blue && up) {
            if (++start_hold >= START_HOLD) pad |= PAD_START;
        } else {
            start_hold = 0;
        }
    }

    return pad;
}
