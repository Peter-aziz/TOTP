/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>
#include "cmox_crypto.h"

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;
UART_HandleTypeDef huart2;

osThreadId_t RTC_TaskHandle;
const osThreadAttr_t RTC_Task_attributes = {
  .name = "RTC_Task", .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
osThreadId_t OTP_TaskHandle;
const osThreadAttr_t OTP_Task_attributes = {
  .name = "OTP_Task", .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
osThreadId_t Uart_TaskHandle;
const osThreadAttr_t Uart_Task_attributes = {
  .name = "Uart_Task", .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal1,
};
osThreadId_t LCD_TaskHandle;
const osThreadAttr_t LCD_Task_attributes = {
  .name = "LCD_Task", .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};

osMutexId_t OTP_MutexHandle;
const osMutexAttr_t OTP_Mutex_attributes = { .name = "OTP_Mutex" };
osMutexId_t Time_MutexHandle;
const osMutexAttr_t Time_Mutex_attributes = { .name = "Time_Mutex" };
osSemaphoreId_t UartSemHandle;
const osSemaphoreAttr_t UartSem_attributes = { .name = "UartSem" };

const uint8_t secret_key[16] = {
  0x4D, 0x79, 0x53, 0x65, 0x63, 0x72, 0x65, 0x74,
  0x4B, 0x65, 0x79, 0x31, 0x32, 0x33, 0x34, 0x35
};

uint8_t secbuffer[2]  = {0x00, 0x00};
uint8_t minbuffer[2]  = {0x01, 0x47};
uint8_t hourbuffer[2] = {0x02, 0x21};
volatile uint32_t current_otp = 0;
volatile uint8_t sync_done = 0;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_USART2_UART_Init(void);
void StartDefaultTask(void *argument);
void StartTask02(void *argument);
void StartTask03(void *argument);
void StartLCDTask(void *argument);

/* -- LCD pin definitions --------------------------------------------- */
#define LCD_RS_PORT   GPIOB
#define LCD_RS_PIN    GPIO_PIN_0
#define LCD_E_PORT    GPIOB
#define LCD_E_PIN     GPIO_PIN_1
#define LCD_DB4_PORT  GPIOA
#define LCD_DB4_PIN   GPIO_PIN_3
#define LCD_DB5_PORT  GPIOA
#define LCD_DB5_PIN   GPIO_PIN_4
#define LCD_DB6_PORT  GPIOA
#define LCD_DB6_PIN   GPIO_PIN_5
#define LCD_DB7_PORT  GPIOA
#define LCD_DB7_PIN   GPIO_PIN_6

/* -- NOP busy-wait: works before AND after scheduler starts ----------- */
/* STM32L432 runs at 32 MHz ? ~3200 NOPs per ms                         */
static void LCD_DelayMs(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++)
        for (volatile uint32_t j = 0; j < 3200; j++)
            __NOP();
}

static void LCD_DelayUs(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++)
        for (volatile uint32_t j = 0; j < 3; j++)
            __NOP();
}

/* -- LCD low-level ---------------------------------------------------- */
static void LCD_PulseEnable(void)
{
    HAL_GPIO_WritePin(LCD_E_PORT, LCD_E_PIN, GPIO_PIN_SET);
    LCD_DelayUs(500);   /* E high: min 450 ns per HD44780 datasheet     */
    HAL_GPIO_WritePin(LCD_E_PORT, LCD_E_PIN, GPIO_PIN_RESET);
    LCD_DelayUs(500);   /* E low hold                                   */
}

static void LCD_WriteNibble(uint8_t nibble)
{
    HAL_GPIO_WritePin(LCD_DB4_PORT, LCD_DB4_PIN, (nibble >> 0) & 1);
    HAL_GPIO_WritePin(LCD_DB5_PORT, LCD_DB5_PIN, (nibble >> 1) & 1);
    HAL_GPIO_WritePin(LCD_DB6_PORT, LCD_DB6_PIN, (nibble >> 2) & 1);
    HAL_GPIO_WritePin(LCD_DB7_PORT, LCD_DB7_PIN, (nibble >> 3) & 1);
    LCD_DelayUs(10);    /* data setup time before pulse                 */
    LCD_PulseEnable();
}

static void LCD_SendByte(uint8_t value, uint8_t isData)
{
    HAL_GPIO_WritePin(LCD_RS_PORT, LCD_RS_PIN,
                      isData ? GPIO_PIN_SET : GPIO_PIN_RESET);
    LCD_DelayUs(10);
    LCD_WriteNibble(value >> 4);    /* high nibble first */
    LCD_WriteNibble(value & 0x0F);  /* then low nibble   */
    LCD_DelayMs(2);                 /* command execution time           */
}

#define LCD_Cmd(c)  LCD_SendByte((c), 0)
#define LCD_Data(c) LCD_SendByte((c), 1)

static void LCD_Init(void)
{
    /* All pins low to start */
    HAL_GPIO_WritePin(LCD_RS_PORT, LCD_RS_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LCD_E_PORT,  LCD_E_PIN,  GPIO_PIN_RESET);

    LCD_DelayMs(50);    /* >40ms power-on delay                         */

    /* Force 8-bit mode 3 times then switch to 4-bit */
    LCD_WriteNibble(0x3); LCD_DelayMs(5);
    LCD_WriteNibble(0x3); LCD_DelayMs(1);
    LCD_WriteNibble(0x3); LCD_DelayMs(1);
    LCD_WriteNibble(0x2); LCD_DelayMs(1);

    LCD_Cmd(0x28);  /* 4-bit, 2 lines, 5x8 font */
    LCD_Cmd(0x0C);  /* Display ON, cursor OFF, blink OFF */
    LCD_Cmd(0x06);  /* Entry mode: increment, no shift */
    LCD_Cmd(0x01);  /* Clear display */
    LCD_DelayMs(2); /* Clear needs >1.52ms */
}

static void LCD_SetCursor(uint8_t row, uint8_t col)
{
    uint8_t addr = (row == 0 ? 0x00 : 0x40) + col;
    LCD_Cmd(0x80 | addr);
}

static void LCD_Print(const char *str)
{
    while (*str)
        LCD_Data((uint8_t)*str++);
}

/* -- main ------------------------------------------------------------- */
int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART2_UART_Init();

  // Remove Harcoded time - comment next 3 lines
  //HAL_I2C_Master_Transmit(&hi2c1, 0xD0, secbuffer,  2, 10);
  //HAL_I2C_Master_Transmit(&hi2c1, 0xD0, minbuffer,  2, 10);
  //HAL_I2C_Master_Transmit(&hi2c1, 0xD0, hourbuffer, 2, 10);
	// Instead, read existing values from RTC:
	uint8_t read_reg[1] = {0x00};
	HAL_I2C_Master_Transmit(&hi2c1, 0xD0, read_reg, 1, 10);
	HAL_I2C_Master_Receive(&hi2c1, 0xD1, &secbuffer[1], 1, 10);

	read_reg[0] = 0x01;
	HAL_I2C_Master_Transmit(&hi2c1, 0xD0, read_reg, 1, 10);
	HAL_I2C_Master_Receive(&hi2c1, 0xD1, &minbuffer[1], 1, 10);

	read_reg[0] = 0x02;
	HAL_I2C_Master_Transmit(&hi2c1, 0xD0, read_reg, 1, 10);
	HAL_I2C_Master_Receive(&hi2c1, 0xD1, &hourbuffer[1], 1, 10);
	
	
  __HAL_RCC_CRC_CLK_ENABLE();
  cmox_initialize(NULL);

  /* LCD init before scheduler — NOP delays work fine here */
  LCD_Init();
  LCD_SetCursor(0, 0); LCD_Print("  OTP System    ");
  LCD_SetCursor(1, 0); LCD_Print("  Starting...   ");
  HAL_Delay(2000);
  LCD_Cmd(0x01);  /* clear */
  LCD_DelayMs(2);

  osKernelInitialize();
  OTP_MutexHandle  = osMutexNew(&OTP_Mutex_attributes);
  Time_MutexHandle = osMutexNew(&Time_Mutex_attributes);
  UartSemHandle    = osSemaphoreNew(1, 1, &UartSem_attributes);

  RTC_TaskHandle  = osThreadNew(StartDefaultTask, NULL, &RTC_Task_attributes);
  OTP_TaskHandle  = osThreadNew(StartTask02,      NULL, &OTP_Task_attributes);
  Uart_TaskHandle = osThreadNew(StartTask03,      NULL, &Uart_Task_attributes);
  LCD_TaskHandle  = osThreadNew(StartLCDTask,     NULL, &LCD_Task_attributes);

  osKernelStart();
  while (1) {}
}

/* -- GPIO ------------------------------------------------------------- */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* PA8 — button input with pull-up */
  GPIO_InitStruct.Pin  = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* LED */
  HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin   = LD3_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD3_GPIO_Port, &GPIO_InitStruct);

  /* LCD data: PA3=DB4, PA4=DB5, PA5=DB6, PA6=DB7 */
  GPIO_InitStruct.Pin  = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* LCD control: PB0=RS, PB1=E */
  GPIO_InitStruct.Pin  = GPIO_PIN_0 | GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/* -- Tasks ------------------------------------------------------------ */
void StartDefaultTask(void *argument)
{
	// Comment these lines that hardocde a time
  //osMutexAcquire(Time_MutexHandle, osWaitForever);
  //hourbuffer[1] = 0x21;
  //minbuffer[1]  = 0x47;
  //secbuffer[1]  = 0x00;
  //osMutexRelease(Time_MutexHandle);

  //HAL_I2C_Master_Transmit(&hi2c1, 0xD0, secbuffer,  2, 10);
  //HAL_I2C_Master_Transmit(&hi2c1, 0xD0, minbuffer,  2, 10);
  //HAL_I2C_Master_Transmit(&hi2c1, 0xD0, hourbuffer, 2, 10);

  for (;;)
  {
    uint8_t loc_sec[2]  = {0x00, 0};
    uint8_t loc_min[2]  = {0x01, 0};
    uint8_t loc_hour[2] = {0x02, 0};

    HAL_I2C_Master_Transmit(&hi2c1, 0xD0, loc_sec,  1, 10);
    HAL_I2C_Master_Receive (&hi2c1, 0xD1, loc_sec  + 1, 1, 10);
    HAL_I2C_Master_Transmit(&hi2c1, 0xD0, loc_min,  1, 10);
    HAL_I2C_Master_Receive (&hi2c1, 0xD1, loc_min  + 1, 1, 10);
    HAL_I2C_Master_Transmit(&hi2c1, 0xD0, loc_hour, 1, 10);
    HAL_I2C_Master_Receive (&hi2c1, 0xD1, loc_hour + 1, 1, 10);
    loc_hour[1] &= 0x3F;

    if (sync_done != 2)
    {
      osMutexAcquire(Time_MutexHandle, osWaitForever);
      hourbuffer[1] = loc_hour[1];
      minbuffer[1]  = loc_min[1];
      secbuffer[1]  = loc_sec[1];
      osMutexRelease(Time_MutexHandle);
    }
    osDelay(1000);
  }
}

void StartTask02(void *argument)
{
  uint8_t mac_out[CMOX_SHA1_SIZE];
  size_t  computed_size;

  for (;;)
  {
    uint8_t time_seed[2];
    osMutexAcquire(Time_MutexHandle, osWaitForever);
    time_seed[0] = hourbuffer[1];
    time_seed[1] = minbuffer[1];
    osMutexRelease(Time_MutexHandle);

    if (cmox_mac_compute(CMOX_HMAC_SHA1_ALGO,
                         time_seed, sizeof(time_seed),
                         secret_key, sizeof(secret_key),
                         NULL, 0,
                         mac_out, CMOX_SHA1_SIZE,
                         &computed_size) == CMOX_MAC_SUCCESS)
    {
      uint8_t  offset = mac_out[19] & 0x0F;
      uint32_t binary = ((mac_out[offset]     & 0x7F) << 24) |
                        ((mac_out[offset + 1] & 0xFF) << 16) |
                        ((mac_out[offset + 2] & 0xFF) <<  8) |
                        ( mac_out[offset + 3] & 0xFF);

      osMutexAcquire(OTP_MutexHandle, osWaitForever);
      current_otp = binary % 1000000;
      osMutexRelease(OTP_MutexHandle);
    }
    osDelay(1000);
  }
}
void StartTask03(void *argument)
{
  char    uartBuf[60];
  char    line1[17];
  char    line2[17];
  uint8_t sync_buf[16] = {0};

  for (;;)
  {
    /* ---- SYNC button handling (unchanged) ---- */
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) == GPIO_PIN_RESET)
    {
      sync_done = 2;
      
      HAL_UART_Transmit(&huart2, (uint8_t*)"SYNC_REQ\r\n", 10, 100);
			__HAL_UART_FLUSH_DRREGISTER(&huart2);
			osDelay(50);                             
      memset(sync_buf, 0, sizeof(sync_buf));
      HAL_StatusTypeDef rx_status = HAL_UART_Receive(&huart2, sync_buf, 13, 8000);

      if (rx_status == HAL_OK && strncmp((char*)sync_buf, "SYNC:", 5) == 0)
      {
        uint8_t h = ((sync_buf[5] - '0') << 4) | (sync_buf[6] - '0');
        uint8_t m = ((sync_buf[7] - '0') << 4) | (sync_buf[8] - '0');
        uint8_t s = ((sync_buf[9] - '0') << 4) | (sync_buf[10] - '0');

        osMutexAcquire(Time_MutexHandle, osWaitForever);
        hourbuffer[1] = h;
        minbuffer[1]  = m;
        secbuffer[1]  = s;
        osMutexRelease(Time_MutexHandle);

        HAL_I2C_Master_Transmit(&hi2c1, 0xD0, secbuffer,  2, 10);
        HAL_I2C_Master_Transmit(&hi2c1, 0xD0, minbuffer,  2, 10);
        HAL_I2C_Master_Transmit(&hi2c1, 0xD0, hourbuffer, 2, 10);
        HAL_UART_Transmit(&huart2, (uint8_t*)"SYNC OK\r\n", 9, 100);
      }
      else if (rx_status != HAL_OK)
        HAL_UART_Transmit(&huart2, (uint8_t*)"SYNC TIMEOUT\r\n", 14, 100);
      else
        HAL_UART_Transmit(&huart2, (uint8_t*)"SYNC INVALID\r\n", 14, 100);

      sync_done = 0;
      osDelay(300);
    }

    /* ---- Read shared state ---- */
    uint8_t h, m, s;
    osMutexAcquire(Time_MutexHandle, osWaitForever);
    h = hourbuffer[1]; m = minbuffer[1]; s = secbuffer[1];
    osMutexRelease(Time_MutexHandle);

    uint32_t otp;
    osMutexAcquire(OTP_MutexHandle, osWaitForever);
    otp = current_otp;
    osMutexRelease(OTP_MutexHandle);

    /* ---- UART (unchanged) ---- */
    int len = snprintf(uartBuf, sizeof(uartBuf),
                       "Time: %02x:%02x:%02x | OTP: %06lu\r\n",
                       h, m, s, (unsigned long)otp);
    HAL_UART_Transmit(&huart2, (uint8_t*)uartBuf, (uint16_t)len, 100);

    /* ---- LCD (merged from StartLCDTask) ---- */
    snprintf(line1, sizeof(line1), "T:%02x:%02x:%02x       ", h, m, s);
    snprintf(line2, sizeof(line2), "O:%06lu    ", (unsigned long)otp);
    line1[16] = '\0';
    line2[16] = '\0';

    LCD_SetCursor(0, 0); LCD_Print(line1);
    LCD_SetCursor(1, 0); LCD_Print(line2);

    osDelay(1000);
  }
}

void StartLCDTask(void *argument)
{
    char line1[17];
    char line2[17];

    for (;;)
    {
        uint8_t  h, m, s;
        uint32_t otp;

        /* Read shared data */
        osMutexAcquire(Time_MutexHandle, osWaitForever);
        h = hourbuffer[1]; m = minbuffer[1]; s = secbuffer[1];
        osMutexRelease(Time_MutexHandle);

        osMutexAcquire(OTP_MutexHandle, osWaitForever);
        otp = current_otp;
        osMutexRelease(OTP_MutexHandle);

        /* Format — exactly 16 chars each, space-padded */
        snprintf(line1, sizeof(line1), "T:%02x:%02x:%02x        ", h, m, s);
        snprintf(line2, sizeof(line2), "O:%09lu     ", (unsigned long)otp);
        line1[16] = '\0';
        line2[16] = '\0';

        /* Write to LCD — NOP delays inside LCD driver are safe here */
        LCD_SetCursor(0, 0);
        LCD_Print(line1);
        LCD_SetCursor(1, 0);
        LCD_Print(line2);

        osDelay(1000);  /* yield to other tasks for 1 second */
    }
}

/* -- Boilerplate ------------------------------------------------------ */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState  = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 16;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1);
  HAL_RCCEx_EnableMSIPLLMode();
}

static void MX_I2C1_Init(void)
{
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x00707CBB;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  HAL_I2C_Init(&hi2c1);
  HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE);
  HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0);
}

static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 9600;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  HAL_UART_Init(&huart2);
}

cmox_init_retval_t cmox_ll_init(void *pArg)   { return CMOX_INIT_SUCCESS; }
cmox_init_retval_t cmox_ll_deInit(void *pArg) { return CMOX_INIT_SUCCESS; }

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM1) HAL_IncTick();
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif

