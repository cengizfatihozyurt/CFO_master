/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <math.h>
#include "ds18b20.h"
#include "adxl345.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define ACC_SAMPLE_PERIOD_MS   2U     /* ~500 Hz ivme ornekleme */
#define SEND_PERIOD_MS         1000U  /* ESP32'ye gonderim ve titresim penceresi */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
/* ---- STM32CubeMonitor ile izlenecek global degiskenler ---- */
volatile float    g_temperature_c = 0.0f;  /* DS18B20 sicaklik (C) */
volatile float    g_acc_x_g = 0.0f;        /* anlik ivme (g) */
volatile float    g_acc_y_g = 0.0f;
volatile float    g_acc_z_g = 0.0f;
volatile float    g_vib_rms_x_g = 0.0f;    /* 1 sn pencere titresim RMS (DC/yercekimi cikarilmis) */
volatile float    g_vib_rms_y_g = 0.0f;
volatile float    g_vib_rms_z_g = 0.0f;
volatile float    g_vib_rms_g = 0.0f;      /* toplam titresim RMS */
volatile uint8_t  g_ds18b20_ok = 0;        /* 1: son okuma basarili */
volatile uint8_t  g_adxl345_ok = 0;        /* 1: sensor bulundu */
volatile uint8_t  g_adxl345_devid = 0;     /* okunan DEVID (0xE5 olmali; 0x00/0xFF = baglanti sorunu) */
volatile uint32_t g_tx_count = 0;          /* ESP32'ye gonderilen paket sayisi */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART2_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static char *append_str(char *p, const char *s)
{
  while (*s)
  {
    *p++ = *s++;
  }
  return p;
}

/* nano.specs printf float desteklemedigi icin basit sabit noktali yazici */
static char *append_fixed(char *p, float v, uint8_t dec)
{
  uint32_t scale = 1;
  char tmp[12];
  uint8_t k = 0;

  for (uint8_t i = 0; i < dec; i++)
  {
    scale *= 10U;
  }
  if (v < 0.0f)
  {
    *p++ = '-';
    v = -v;
  }
  uint32_t n = (uint32_t)(v * (float)scale + 0.5f);
  uint32_t ip = n / scale;
  uint32_t fp = n % scale;

  do
  {
    tmp[k++] = (char)('0' + ip % 10U);
    ip /= 10U;
  } while (ip);
  while (k)
  {
    *p++ = tmp[--k];
  }
  if (dec)
  {
    *p++ = '.';
    for (uint8_t i = dec; i > 0; i--)
    {
      p[i - 1U] = (char)('0' + fp % 10U);
      fp /= 10U;
    }
    p += dec;
  }
  return p;
}

/* Format: T:25.06,VX:0.012,VY:0.010,VZ:0.015,V:0.022\r\n */
static void Send_To_ESP32(void)
{
  char buf[96];
  char *p = buf;

  p = append_str(p, "T:");
  if (g_ds18b20_ok)
  {
    p = append_fixed(p, g_temperature_c, 2);
  }
  else
  {
    p = append_str(p, "ERR");
  }
  p = append_str(p, ",VX:");
  p = append_fixed(p, g_vib_rms_x_g, 3);
  p = append_str(p, ",VY:");
  p = append_fixed(p, g_vib_rms_y_g, 3);
  p = append_str(p, ",VZ:");
  p = append_fixed(p, g_vib_rms_z_g, 3);
  p = append_str(p, ",V:");
  p = append_fixed(p, g_vib_rms_g, 3);
  p = append_str(p, "\r\n");

  HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)(p - buf), 50);
  g_tx_count++;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI1_Init();
  MX_USART2_Init();
  /* USER CODE BEGIN 2 */
  DS18B20_Init();
  uint8_t devid = 0;
  g_adxl345_ok = ADXL345_Init(&hspi1, &devid);
  g_adxl345_devid = devid;

  uint8_t  conv_pending = DS18B20_StartConversion();
  uint32_t t_conv = HAL_GetTick();
  uint32_t t_acc  = HAL_GetTick();
  uint32_t t_send = HAL_GetTick();

  /* titresim penceresi istatistikleri (ham LSB) */
  int32_t  sum_x = 0, sum_y = 0, sum_z = 0;
  int64_t  sq_x = 0, sq_y = 0, sq_z = 0;
  uint32_t n_samples = 0;

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t now = HAL_GetTick();

    /* --- ADXL345 ornekleme --- */
    if (g_adxl345_ok && (now - t_acc) >= ACC_SAMPLE_PERIOD_MS)
    {
      int16_t x, y, z;
      t_acc = now;
      if (ADXL345_ReadXYZ(&x, &y, &z))
      {
        g_acc_x_g = x * ADXL345_LSB_TO_G;
        g_acc_y_g = y * ADXL345_LSB_TO_G;
        g_acc_z_g = z * ADXL345_LSB_TO_G;

        sum_x += x;  sq_x += (int32_t)x * x;
        sum_y += y;  sq_y += (int32_t)y * y;
        sum_z += z;  sq_z += (int32_t)z * z;
        n_samples++;
      }
    }

    /* --- DS18B20 (bloklamayan donusum) --- */
    if (!conv_pending)
    {
      conv_pending = DS18B20_StartConversion();
      t_conv = now;
      if (!conv_pending)
      {
        g_ds18b20_ok = 0;
      }
    }
    else if ((now - t_conv) >= DS18B20_CONV_TIME_MS)
    {
      float t;
      if (DS18B20_ReadTemperature(&t))
      {
        g_temperature_c = t;
        g_ds18b20_ok = 1;
      }
      else
      {
        g_ds18b20_ok = 0;
      }
      conv_pending = 0;
    }

    /* --- Titresim hesapla + ESP32'ye gonder --- */
    if ((now - t_send) >= SEND_PERIOD_MS)
    {
      t_send = now;
      if (n_samples > 1U)
      {
        float n  = (float)n_samples;
        float mx = sum_x / n, my = sum_y / n, mz = sum_z / n;
        float vx = (float)sq_x / n - mx * mx;
        float vy = (float)sq_y / n - my * my;
        float vz = (float)sq_z / n - mz * mz;
        if (vx < 0.0f) vx = 0.0f;
        if (vy < 0.0f) vy = 0.0f;
        if (vz < 0.0f) vz = 0.0f;

        g_vib_rms_x_g = sqrtf(vx) * ADXL345_LSB_TO_G;
        g_vib_rms_y_g = sqrtf(vy) * ADXL345_LSB_TO_G;
        g_vib_rms_z_g = sqrtf(vz) * ADXL345_LSB_TO_G;
        g_vib_rms_g   = sqrtf(vx + vy + vz) * ADXL345_LSB_TO_G;
      }
      sum_x = sum_y = sum_z = 0;
      sq_x = sq_y = sq_z = 0;
      n_samples = 0;

      /* Sensor bulunamadiysa her saniye tekrar dene */
      if (!g_adxl345_ok)
      {
        g_adxl345_ok = ADXL345_Init(&hspi1, &devid);
        g_adxl345_devid = devid;
      }

      Send_To_ESP32();
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI1 Initialization Function (ADXL345: mod 3, 1 MHz)
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_HIGH;
  hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART2 Initialization Function (ESP32, 115200 8N1)
  * @param None
  * @retval None
  */
static void MX_USART2_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* Cikis seviyeleri: 1-Wire serbest, CS pasif (yuksek) */
  HAL_GPIO_WritePin(GPIOA, DS18B20_Pin|ADXL345_CS_Pin, GPIO_PIN_SET);

  /* PA1: DS18B20 1-Wire (open-drain, harici 4.7k pull-up) */
  GPIO_InitStruct.Pin = DS18B20_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DS18B20_GPIO_Port, &GPIO_InitStruct);

  /* PA4: ADXL345 CS */
  GPIO_InitStruct.Pin = ADXL345_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(ADXL345_CS_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
