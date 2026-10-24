/*
 * ADXL345 SPI surucusu (SPI1, mod 3, 4 telli, CS = PA4)
 */
#include "adxl345.h"

#define ADXL345_REG_DEVID        0x00
#define ADXL345_REG_BW_RATE      0x2C
#define ADXL345_REG_POWER_CTL    0x2D
#define ADXL345_REG_DATA_FORMAT  0x31
#define ADXL345_REG_DATAX0       0x32

#define ADXL345_SPI_READ         0x80
#define ADXL345_SPI_MB           0x40

#define ADXL345_DEVID_VALUE      0xE5

static SPI_HandleTypeDef *adxl_spi;

static inline void cs_low(void)  { HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin, GPIO_PIN_SET); }

static HAL_StatusTypeDef adxl_write(uint8_t reg, uint8_t val)
{
  uint8_t tx[2] = { reg & 0x3FU, val };
  HAL_StatusTypeDef st;

  cs_low();
  st = HAL_SPI_Transmit(adxl_spi, tx, 2, 10);
  cs_high();
  return st;
}

static HAL_StatusTypeDef adxl_read(uint8_t reg, uint8_t *buf, uint8_t len)
{
  uint8_t tx[7] = {0};
  uint8_t rx[7];
  HAL_StatusTypeDef st;

  if (len > 6)
  {
    return HAL_ERROR;
  }
  tx[0] = ADXL345_SPI_READ | (len > 1 ? ADXL345_SPI_MB : 0U) | (reg & 0x3FU);

  cs_low();
  st = HAL_SPI_TransmitReceive(adxl_spi, tx, rx, len + 1U, 10);
  cs_high();

  for (uint8_t i = 0; i < len; i++)
  {
    buf[i] = rx[i + 1U];
  }
  return st;
}

uint8_t ADXL345_Init(SPI_HandleTypeDef *hspi, uint8_t *devid)
{
  uint8_t id = 0;

  adxl_spi = hspi;
  cs_high();
  /* SPI'yi CS dusmeden once ac: mod 3'te SCK bosta HIGH olmali */
  __HAL_SPI_ENABLE(adxl_spi);
  HAL_Delay(5);

  HAL_StatusTypeDef st = adxl_read(ADXL345_REG_DEVID, &id, 1);
  if (devid)
  {
    *devid = id;
  }
  if (st != HAL_OK || id != ADXL345_DEVID_VALUE)
  {
    return 0;
  }

  adxl_write(ADXL345_REG_POWER_CTL, 0x00);   /* standby */
  adxl_write(ADXL345_REG_DATA_FORMAT, 0x0B); /* FULL_RES, +-16g, 4 telli SPI */
  adxl_write(ADXL345_REG_BW_RATE, 0x0D);     /* ODR 800 Hz */
  adxl_write(ADXL345_REG_POWER_CTL, 0x08);   /* olcum modu */
  return 1;
}

uint8_t ADXL345_ReadXYZ(int16_t *x, int16_t *y, int16_t *z)
{
  uint8_t d[6];

  if (adxl_read(ADXL345_REG_DATAX0, d, 6) != HAL_OK)
  {
    return 0;
  }
  *x = (int16_t)(((uint16_t)d[1] << 8) | d[0]);
  *y = (int16_t)(((uint16_t)d[3] << 8) | d[2]);
  *z = (int16_t)(((uint16_t)d[5] << 8) | d[4]);
  return 1;
}
