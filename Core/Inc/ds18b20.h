#ifndef __DS18B20_H
#define __DS18B20_H

#include "main.h"

/* 12 bit cozunurlukte donusum suresi (ms) */
#define DS18B20_CONV_TIME_MS  750U

void    DS18B20_Init(void);
uint8_t DS18B20_StartConversion(void);      /* 1: sensor cevap verdi */
uint8_t DS18B20_ReadTemperature(float *t);  /* 1: okuma + CRC basarili */

#endif /* __DS18B20_H */
