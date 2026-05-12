/**
 * \file    arrays.h
 *
 * \brief   Randomly generated arrays for testing purposes.
 *
 * \details Definitions live in arrays.c so each array's .rodata payload is
 *          emitted exactly once for the whole test binary instead of being
 *          duplicated by every translation unit that includes this header.
 *          Sizes are baked into the declaration so `sizeof(array_N)` still
 *          returns N at compile time.
 *
 * \author  Kamil Kielbasa
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef ARRAYS_H
#define ARRAYS_H

/* Include files -------------------------------------------------------------------------------- */

#include <stdint.h>

/* Module interface variables and constants ----------------------------------------------------- */

extern const uint8_t array_1[1];
extern const uint8_t array_2[2];
extern const uint8_t array_4[4];
extern const uint8_t array_5[5];
extern const uint8_t array_8[8];
extern const uint8_t array_16[16];
extern const uint8_t array_32[32];
extern const uint8_t array_64[64];
extern const uint8_t array_97[97];
extern const uint8_t array_128[128];
extern const uint8_t array_256[256];
extern const uint8_t array_271[271];
extern const uint8_t array_512[512];
extern const uint8_t array_1024[1024];
extern const uint8_t array_2048[2048];
extern const uint8_t array_3840[3840];
extern const uint8_t array_3907[3907];
extern const uint8_t array_4096[4096];
extern const uint8_t array_8000[8000];

#endif /* ARRAYS_H */
