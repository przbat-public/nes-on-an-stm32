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

#define START_HOLD 12           /* ~0.25 s at the speeds this reaches now,
                                 * ~0.7 s back when it ran at 17 fps */

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

void input_init(void)
{
    hal_release_jtag_pins();
    for (unsigned i = 0; i < N_BUTTONS; i++)
        gpio_input_pullup(map[i].port, map[i].pin);

    /* Which layout is active is decided here, before the game starts:
     * hold the blue button while the board comes out of reset. */
    unsigned held = 0;
    for (unsigned i = 0; i < 2000; i++)
        if (!gpio_read(PORT_C, 13)) held++;
    shooter_layout = (held > 1000);
}

/* ------------------------- driven from the debugger --------------- *
 * A five-switch shield cannot produce the pad sequences a game's state
 * machine needs at the frame it needs them (Castlevania III wants a
 * *newly pressed* START while its title screen sits in sub-state 4, and
 * the host/board frame comparison has to drive both sides with the same
 * script). These two words let SWD take the pad over:
 *
 *   dbg_pad_override != 0   the pad is dbg_pad_value, every frame
 *   dbg_pad_auto   != 0     press START for 3 frames every N emulated
 *                           frames, starting at frame 20 of each period
 *                           — the script tools/host runners use
 *
 * Both are 0 on the bench, so the joystick is untouched until somebody
 * writes them; the cost is 16 bytes of .bss.
 */
volatile uint32_t dbg_pad_override;
volatile uint32_t dbg_pad_value;
volatile uint32_t dbg_pad_auto;

/* main.c counts completed frames, so at the moment input_pad() is asked
 * for frame N it holds N — the phase is absolute and setting dbg_pad_auto
 * halfway through a run still reproduces the script from frame 0. */
extern volatile uint32_t dbg_frames;

uint8_t input_pad(void)
{
    uint8_t pad = 0;

    if (dbg_pad_override)
        return (uint8_t)dbg_pad_value;
    if (dbg_pad_auto) {
        uint32_t f = dbg_frames % dbg_pad_auto;
        return (f >= 20 && f < 23) ? PAD_START : 0;
    }

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

    /* Which direction joins the blue button to mean START, and what the
     * button means on its own, is the only thing the layouts disagree on. */
    bool start_combo;
    if (shooter_layout) {
        pad &= (uint8_t)~PAD_A;              /* the button is B here */
        if (blue)       pad |= PAD_B;        /* fire */
        if (blue && up) pad |= PAD_A;        /* jump, as shooters want it */
        start_combo = blue && down;
    } else {
        if (blue && down) pad |= PAD_B;      /* secondary action */
        start_combo = blue && up;
    }

    /* START has to be *held*: a tap of fire while ducking, or a jump with
     * the button down, must not pause the game by accident. */
    if (start_combo) {
        if (++start_hold >= START_HOLD) pad |= PAD_START;
    } else {
        start_hold = 0;
    }

    return pad;
}
