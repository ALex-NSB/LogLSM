#include "lsm6dso_temp.h"

#define LSM6DSO_TEMP_SENSOR_SENSITIVITY                 256

/*





*/
int32_t LSM6DSO_GetTemp(LSM6DSO_Object_t *pObj, float *Temp)
{
  int32_t ret = LSM6DSO_OK;
  int16_t val;
  if(lsm6dso_temperature_raw_get(&(pObj->Ctx), &val)  != LSM6DSO_OK)
  {
    return LSM6DSO_ERROR;
  }
  *Temp = 25 + (float)val / LSM6DSO_TEMP_SENSOR_SENSITIVITY;
  //*Temp = (float)val;
  
  return ret;
}
