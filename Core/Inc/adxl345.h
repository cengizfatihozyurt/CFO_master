#ifndef __ADXL345_H
#define __ADXL345_H

#include "main.h"

/* FULL_RES modunda olcek: 3.9 mg/LSB (tum araliklarda) */
#define ADXL345_LSB_TO_G   0.0039f

uint8_t ADXL345_Init(SPI_HandleTypeDef *hspi, uint8_t *devid);  /* 1: DEVID dogru (0xE5) */
uint8_t ADXL345_ReadXYZ(int16_t *x, int16_t *y, int16_t *z);

#endif /* __ADXL345_H */
