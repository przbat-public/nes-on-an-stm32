/*
 * input.h — the physical controls, mapped to an NES pad.
 */
#pragma once
#include <stdint.h>

void    input_init(void);
uint8_t input_pad(void);     /* NES pad bits (PAD_* from nes.h) */
