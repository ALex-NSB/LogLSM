#ifndef MYFUNC_H
#define MYFUNC_H

#include <stdint.h>

uint32_t swapBytes(uint32_t val, uint8_t size);
uint32_t bigEndianToInt(const uint8_t *bytes, uint8_t size);
uint8_t intToBigEndian(uint32_t value, uint8_t size, uint8_t *out);

#endif /* MYFUNC_H */
