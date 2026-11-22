#ifndef CELLSCOPE_MUSCLE_CODES_H
#define CELLSCOPE_MUSCLE_CODES_H

#include <stdint.h>

/* Draft color-to-code atlas derived from the painted diffuse atlas. This is
 * a first-pass mapping for review; unmapped colors return "MUS" so the UI
 * never invents a confident code. */
const char *muscle_code_for_color(uint8_t r, uint8_t g, uint8_t b);
const char *muscle_region_for_code(const char *code);

#endif
