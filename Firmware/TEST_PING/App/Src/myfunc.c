#include "myfunc.h"

uint32_t swapBytes(uint32_t val, uint8_t size)
{
  uint8_t *pval = (uint8_t*)&val;
  uint8_t t;
  for(uint8_t i=0; i < size/2; i++)
  {
    t = pval[i];
    pval[i] = pval[size-i-1];
    pval[size-i-1] = t;
  }
  return val;
}


uint32_t bigEndianToInt(const uint8_t *bytes, uint8_t size)
{
  uint32_t result = 0;
  for(uint8_t i = 0; i < size; i++) {
    result = (result << 8) | bytes[i];
  }
  return result;
}


uint8_t intToBigEndian(uint32_t value, uint8_t size, uint8_t *out)
{
  uint8_t *pvalue = (uint8_t*)&value;
  
}

/*void StrToHex(char* dst, uint8_t* src, uint16_t size)
{
  char ss[10];
  dst[0]=0;
  for(uint16_t i=0; i<size; i++)
  {
    sprintf(ss," _%02X", src[i]);
    strcat(dst,ss);
  }
  strcat(dst,"\n");
}*/