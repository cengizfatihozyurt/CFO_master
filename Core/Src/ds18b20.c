/*
 * DS18B20 1-Wire surucusu (PA1, open-drain, harici 4.7k pull-up gerekli)
 * Mikrosaniye gecikmeleri icin DWT cycle counter kullanilir.
 */
#include "ds18b20.h"

#define OW_CMD_SKIP_ROM      0xCC
#define OW_CMD_CONVERT_T     0x44
#define OW_CMD_READ_SCRATCH  0xBE

static void delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * (SystemCoreClock / 1000000U);
  while ((DWT->CYCCNT - start) < ticks);
}

static inline void ow_low(void)     { DS18B20_GPIO_Port->BRR  = DS18B20_Pin; }
static inline void ow_release(void) { DS18B20_GPIO_Port->BSRR = DS18B20_Pin; }
static inline uint8_t ow_read(void) { return (DS18B20_GPIO_Port->IDR & DS18B20_Pin) ? 1U : 0U; }

static uint8_t ow_reset(void)
{
  uint8_t presence;
  uint32_t primask;

  /* Hat bosta yuksek olmali; dusukse pull-up yok ya da kisa devre var */
  if (ow_read() == 0U)
  {
    return 0;
  }

  ow_low();
  delay_us(480);
  primask = __get_PRIMASK();
  __disable_irq();
  ow_release();
  delay_us(70);
  presence = (ow_read() == 0U);
  __set_PRIMASK(primask);
  delay_us(410);
  return presence;
}

static void ow_write_bit(uint8_t bit)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  ow_low();
  if (bit)
  {
    delay_us(6);
    ow_release();
    delay_us(64);
  }
  else
  {
    delay_us(60);
    ow_release();
    delay_us(10);
  }
  __set_PRIMASK(primask);
}

static uint8_t ow_read_bit(void)
{
  uint8_t bit;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  ow_low();
  delay_us(3);
  ow_release();
  delay_us(10);
  bit = ow_read();
  __set_PRIMASK(primask);
  delay_us(53);
  return bit;
}

static void ow_write_byte(uint8_t data)
{
  for (uint8_t i = 0; i < 8; i++)
  {
    ow_write_bit(data & 0x01U);
    data >>= 1;
  }
}

static uint8_t ow_read_byte(void)
{
  uint8_t data = 0;
  for (uint8_t i = 0; i < 8; i++)
  {
    data >>= 1;
    if (ow_read_bit())
    {
      data |= 0x80U;
    }
  }
  return data;
}

static uint8_t crc8_maxim(const uint8_t *buf, uint8_t len)
{
  uint8_t crc = 0;
  while (len--)
  {
    uint8_t in = *buf++;
    for (uint8_t i = 0; i < 8; i++)
    {
      uint8_t mix = (crc ^ in) & 0x01U;
      crc >>= 1;
      if (mix)
      {
        crc ^= 0x8CU;
      }
      in >>= 1;
    }
  }
  return crc;
}

void DS18B20_Init(void)
{
  /* DWT cycle counter'i ac */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  ow_release();
}

uint8_t DS18B20_StartConversion(void)
{
  if (!ow_reset())
  {
    return 0;
  }
  ow_write_byte(OW_CMD_SKIP_ROM);
  ow_write_byte(OW_CMD_CONVERT_T);
  return 1;
}

uint8_t DS18B20_ReadTemperature(float *t)
{
  uint8_t sp[9];

  if (!ow_reset())
  {
    return 0;
  }
  ow_write_byte(OW_CMD_SKIP_ROM);
  ow_write_byte(OW_CMD_READ_SCRATCH);
  for (uint8_t i = 0; i < 9; i++)
  {
    sp[i] = ow_read_byte();
  }

  /* Hepsi 0x00 ise CRC de 0 cikar; sensor cevap vermiyor demektir */
  uint8_t all_zero = 1;
  for (uint8_t i = 0; i < 9; i++)
  {
    if (sp[i] != 0x00U)
    {
      all_zero = 0;
    }
  }
  if (all_zero || crc8_maxim(sp, 8) != sp[8])
  {
    return 0;
  }

  int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | sp[0]);
  *t = (float)raw / 16.0f;
  return 1;
}
