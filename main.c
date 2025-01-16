#include "stm32f10x.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_adc.h"
#include "stm32f10x_tim.h"
#include "stm32f10x_dma.h"
#include "delay.h"
#include "lcd16x2.h"
#include "lookup.h"
#include <math.h>

#define N		128
#define ADC_BUFFER_SIZE 2
#define PI 3.14159265359

volatile uint16_t adc_buffer[ADC_BUFFER_SIZE];
volatile uint16_t adc_value = 0;
volatile uint16_t res_value = 0;
volatile uint16_t adc_value1 = 0;
volatile uint16_t n_count = 0;
volatile uint16_t n_done = 0;

volatile uint16_t pwm_buffer[N];
volatile uint16_t pwm_index = 0;
volatile uint16_t pwm_ready = 0;

int REX[N];
int IMX[N];
int REAL_OUT[N];
int IMAG_OUT[N];
uint16_t MAG[N];
uint16_t MAG_OUT[N];
uint16_t lcd_buf_top[N];
uint16_t lcd_buf_bot[N];
float fft_cos_lookup[N / 2];
float fft_sin_lookup[N / 2];

uint16_t low_pass(uint16_t input, uint8_t filter_index);
void init_timer(void);
void init_pwm(void);
void init_lcd(void);
void read_adc(uint16_t* adc_values);
void write_pwm(uint16_t val);
void lcd_update(void);
void fft();
void ifft(int *REX, int *IMX, int *REAL_OUT, int *IMAG_OUT);
void mag_to_buf(void);
void init_adc_dma(volatile uint16_t* adc_buffer, uint8_t buffer_size);
void init_pwm_dma(volatile uint16_t *pwm_buffer, uint16_t buffer_size);

/* Matlab code
>> fs = 35156;          % T?n s? l?y m?u
f1 = 20; f2 = 500;      % T?n s? d?i bass
fn1 = f1 / (fs/2);     fn0 = 0.0008; % T?n s? chu?n h�a
fn2 = f2 / (fs/2);     fn3 = 0.03;
% �?nh nghia t?n s? v� d�p ?ng
f = [0 fn0 fn1 fn2 fn3 1];
a = [1 1 2 2 1 1];           % Khu?ch d?i d?i bass (2), ch?n t?n s? kh�c (0)
% Thi?t k? b? l?c FIR
order = 8;              % B?c c?a b? l?c
b = firpm(order, f, a);   % Thi?t k? b? l?c v?i Parks-McClellan
% Ph�n t�ch d�p ?ng t?n s?
freqz(b, 1, 512, fs)
*/

uint16_t low_pass(uint16_t input, uint8_t filter_index) {
    int i;
    static float buffer[NUM_FILTERS][FILTER_BUF] = {0};
    float result = 0.0;

    // D?ch m?ng d? th�m gi� tr? m?i
    for (i = (FILTER_BUF - 1); i > 0; i--) {
        buffer[filter_index][i] = buffer[filter_index][i - 1];
    }
    buffer[filter_index][0] = input;

    // �p d?ng b? l?c FIR
    for (i = 0; i < FILTER_BUF; i++) {
        result += buffer[filter_index][i] * filter_coeffs[filter_index][i];
    }

    return (uint16_t)result;
}

// H�m kh?i t?o b?ng tra c?u
void generate_twiddle_factors(void) {
    for (uint16_t i = 0; i < N / 2; i++) {
        fft_cos_lookup[i] = cos(2 * PI * i / N);
        fft_sin_lookup[i] = sin(2 * PI * i / N);
    }
}

void TIM3_IRQHandler()
{
    static uint8_t s = 0;
    static int pwm_delay = 0;

    if (TIM_GetITStatus(TIM3, TIM_IT_Update))
    {
        uint16_t adc_value_ch1 = adc_buffer[0]; // Gi� tr? k�nh 1 (PA1)
        uint16_t adc_value_ch2 = adc_buffer[1]; // Gi� tr? k�nh 2 (PA2)

        // X? l� k�nh 1 (PA1) cho FFT
        adc_value = adc_value_ch1 >> 2;
        res_value = adc_value_ch2 >> 2;
/*
        uint8_t filter_index = 0;

        if (res_value <= 84) {
            adc_value1 = adc_value; // Bypass low_pass filter
        } else {
            if (res_value <= 169) {
                filter_index = 0;
            } else if (res_value <= 254) {
                filter_index = 1;
            } else if (res_value <= 339) {
                filter_index = 2;
            } else if (res_value <= 424) {
                filter_index = 3;
            } else if (res_value <= 509) {
                filter_index = 4;
            } else if (res_value <= 594) {
                filter_index = 5;
            } else if (res_value <= 679) {
                filter_index = 6;
            } else if (res_value <= 764) {
                filter_index = 7;
            } else if (res_value <= 849) {
                filter_index = 8;
            } else if (res_value <= 934) {
                filter_index = 9;
            } else {
                filter_index = 10;
            }

            adc_value1 = low_pass(adc_value, filter_index);
        }
        write_pwm(adc_value1);
*/
        s++;
        if (s >= 2)
        {
            if (n_done == 0)
            {
                REX[n_count++] = adc_value;
                if (pwm_ready) {
                write_pwm(pwm_buffer[pwm_index]);
                pwm_index++;
                if (pwm_index >= N) {
                pwm_index = 0;   // Quay l?i d?u b? d?m
                pwm_ready = 0;
                }
                } else {
                TIM2->CCR1 = 0; // T?t PWM khi kh�ng c� t�n hi?u
                } 
                if (n_count >= N)
                {
                    n_done = 1;
                    n_count = 0;
                }
            }
            s = 0;
        }
        // Clears the TIM3 interrupt pending bit
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
    }
}

int main(void)
{
    init_adc_dma(adc_buffer, ADC_BUFFER_SIZE);
    init_timer();
    init_pwm();
    init_lcd();
    generate_twiddle_factors();

    while (1)
    { 
			/*	
		// Wait until sampling is done
		while (!n_done);
		fft();
		mag_to_buf();
		lcd_update();
		n_done = 0;
			*/
        // Wait until sampling is done
        while (!n_done);

        // Ki?m tra xem ADC c� t�n hi?u d?u v�o hay kh�ng
        uint8_t adc_has_input = 0;
        for (uint8_t i = 0; i < ADC_BUFFER_SIZE; i++) {
            if (adc_buffer[i] > 5) { // Ngu?ng gi� tr? d? x�c d?nh c� t�n hi?u d?u v�o
                adc_has_input = 1;
                break;
            }
        }

        if (adc_has_input) {
            fft();
            mag_to_buf();
            ifft(REX, IMX, REAL_OUT, IMAG_OUT);
            lcd_update();
        } else {
            // X�a b? d?m PWM d? kh�ng ph�t t�n hi?u kh�ng mong mu?n
            for (uint16_t i = 0; i < N; i++) {
                pwm_buffer[i] = 0;
            }
            pwm_ready = 0;
        }

        n_done = 0; 
    }
}


void init_adc_dma(volatile uint16_t* adc_buffer, uint8_t buffer_size)
{
    ADC_InitTypeDef ADC_InitStruct;
    GPIO_InitTypeDef GPIO_InitStruct;
    DMA_InitTypeDef DMA_InitStruct;

    // Step 1: Initialize GPIO for ADC (PA1, PA2)
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_1 | GPIO_Pin_2; // PA1 v� PA2
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStruct);

    // Step 2: Enable clocks for ADC1 and DMA1
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    // Step 3: Configure DMA
    DMA_DeInit(DMA1_Channel1);
    DMA_InitStruct.DMA_PeripheralBaseAddr = (uint32_t)&ADC1->DR; // �?a ch? c?a thanh ghi d? li?u ADC
    DMA_InitStruct.DMA_MemoryBaseAddr = (uint32_t)adc_buffer;    // �?a ch? b? nh? d?m
    DMA_InitStruct.DMA_DIR = DMA_DIR_PeripheralSRC;              // Chuy?n t? ADC d?n b? nh?
    DMA_InitStruct.DMA_BufferSize = buffer_size;                 // K�ch thu?c b? d?m
    DMA_InitStruct.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStruct.DMA_MemoryInc = DMA_MemoryInc_Enable;         // B? nh? tang d?n
    DMA_InitStruct.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord; // D? li?u 16-bit
    DMA_InitStruct.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    DMA_InitStruct.DMA_Mode = DMA_Mode_Circular;                 // Ch? d? v�ng l?p
    DMA_InitStruct.DMA_Priority = DMA_Priority_High;
    DMA_InitStruct.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel1, &DMA_InitStruct);

    // Enable DMA1 Channel1
    DMA_Cmd(DMA1_Channel1, ENABLE);

    // Step 4: Configure ADC
    ADC_InitStruct.ADC_ContinuousConvMode = ENABLE;              // Ch? d? chuy?n d?i li�n t?c
    ADC_InitStruct.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_InitStruct.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_InitStruct.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStruct.ADC_NbrOfChannel = 2;                        // S? lu?ng k�nh
    ADC_InitStruct.ADC_ScanConvMode = ENABLE;                   // B?t ch? d? qu�t da k�nh
    ADC_Init(ADC1, &ADC_InitStruct);

    // Configure ADC channels
    ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 1, ADC_SampleTime_7Cycles5); // PA1, uu ti�n 1
    ADC_RegularChannelConfig(ADC1, ADC_Channel_2, 2, ADC_SampleTime_7Cycles5); // PA2, uu ti�n 2

    // Enable ADC DMA
    ADC_DMACmd(ADC1, ENABLE);

    // Enable ADC1
    ADC_Cmd(ADC1, ENABLE);

    // Calibrate ADC (n?u c?n)
    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1));
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1));

    // Start ADC conversion
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
}

void init_timer()
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStruct;
    NVIC_InitTypeDef NVIC_InitStruct;

    // Step 1: Initialize TIM3 for timer interrupt
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    // Timer freq = timer_clock / ((TIM_Prescaler+1) * (TIM_Period+1))
    // Timer freq = 72MHz / ((1+1) * (1023+1) = 35.15kHz
    TIM_TimeBaseInitStruct.TIM_Prescaler = 1;
    TIM_TimeBaseInitStruct.TIM_Period = 1023;
    TIM_TimeBaseInitStruct.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInitStruct.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &TIM_TimeBaseInitStruct);
    // Enable TIM3 interrupt
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM3, ENABLE);

    // Step 2: Initialize NVIC for timer interrupt
    NVIC_InitStruct.NVIC_IRQChannel = TIM3_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 0x00;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0x00;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);
}

void init_pwm()
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStruct;
    TIM_OCInitTypeDef TIM_OCInitStruct;
    GPIO_InitTypeDef GPIO_InitStruct;

    // Step 1: Initialize TIM2 for PWM
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    // Timer freq = timer_clock / ((TIM_Prescaler+1) * (TIM_Period+1))
    // Timer freq = 72MHz / ((1+1) * (1023+1) = 35.15kHz
    TIM_TimeBaseInitStruct.TIM_Prescaler = 1;
    TIM_TimeBaseInitStruct.TIM_Period = 1023;
    TIM_TimeBaseInitStruct.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInitStruct.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseInitStruct);
    TIM_Cmd(TIM2, ENABLE);

    // Step 2: Initialize PWM
    TIM_OCInitStruct.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStruct.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStruct.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OCInitStruct.TIM_Pulse = 0;
    TIM_OC1Init(TIM2, &TIM_OCInitStruct);
    TIM_OC1PreloadConfig(TIM2, TIM_OCPreload_Enable);

    // Step 3: Initialize GPIOA (PA0) for PWM output
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void init_lcd()
{
    uint8_t i;

    // Initialize LCD
    lcd16x2_init(LCD16X2_DISPLAY_ON_CURSOR_OFF_BLINK_OFF);

    // Fill custom char
    for (i = 0; i < 8; i++)
    {
        lcd16x2_create_custom_char(i, bar_graph[i]);
    }
}

void read_adc(uint16_t* adc_values)
{
    uint8_t i;

    // Start ADC conversion
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);

    for (i = 0; i < 2; i++) // 2 k�nh
    {
        // Wait until ADC conversion finished
        while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC));
        adc_values[i] = ADC_GetConversionValue(ADC1);
    }
}

void write_pwm(uint16_t val)
{
    // Write PWM value
    TIM2->CCR1 = val;
}

void lcd_update()
{
    uint8_t i;

    // Write 16 point frequency spectrum (index 1 to 16)
    // Frequency spectrum at index 0 (DC value) is not used
    for (i = 1; i <= 16; i++)
    {
        // Write first row
        if (lcd_buf_top[i] == ' ')
        {
            lcd16x2_gotoxy((i-1), 0);
            lcd16x2_putc(' ');
        }
        else
        {
            lcd16x2_put_custom_char((i-1), 0, lcd_buf_top[i]);
        }
        // Write second row
        lcd16x2_put_custom_char((i-1), 1, lcd_buf_bot[i]);
    }
}

void fft() {
    uint16_t i, j, k, stage, stage_size, half_size;
    float ur, ui, tr, ti;

    // 1. Reset ph?n ?o
    for (i = 0; i < N; i++) {
        IMX[i] = 0;
    }

    // 2. Bit-reversal reordering
    j = 0;
    for (i = 1; i < N; i++) {
        k = N >> 1;
        while (j >= k) {
            j -= k;
            k >>= 1;
        }
        j += k;
        if (i < j) {
            // Ho�n d?i ph?n th?c
            tr = REX[i];
            REX[i] = REX[j];
            REX[j] = tr;

            ti = IMX[i];
            IMX[i] = IMX[j];
            IMX[j] = ti;
        }
    }

    // 3. FFT computation (s? d?ng b?ng tra c?u)
    for (stage = 1; stage <= log2(N); stage++) {
        stage_size = 1 << stage;      // 2^stage
        half_size = stage_size >> 1;  // stage_size / 2

        for (j = 0; j < half_size; j++) {
            // L?y gi� tr? cos v� sin t? b?ng tra c?u
            ur = fft_cos_lookup[(N / stage_size) * j];
            ui = -fft_sin_lookup[(N / stage_size) * j];

            for (i = j; i < N; i += stage_size) {
                k = i + half_size;
                tr = ur * REX[k] - ui * IMX[k];
                ti = ur * IMX[k] + ui * REX[k];
                REX[k] = REX[i] - tr;
                IMX[k] = IMX[i] - ti;
                REX[i] = REX[i] + tr;
                IMX[i] = IMX[i] + ti;
            }
        }
    }
}

void ifft(int *REX, int *IMX, int *REAL_OUT, int *IMAG_OUT) {
    uint16_t i, j, k, stage, stage_size, half_size;
    float ur, ui, tr, ti;
    float norm_factor = 1.0 / N;

    // 1. Bit-reversal reordering
    j = 0;
    for (i = 1; i < N; i++) {
        k = N >> 1;
        while (j >= k) {
            j -= k;
            k >>= 1;
        }
        j += k;
        if (i < j) {
            tr = REX[i];
            REX[i] = REX[j];
            REX[j] = tr;

            ti = IMX[i];
            IMX[i] = IMX[j];
            IMX[j] = ti;
        }
    }

    // 2. IFFT computation (s? d?ng b?ng tra c?u)
    for (stage = 1; stage <= log2(N); stage++) {
        stage_size = 1 << stage;
        half_size = stage_size >> 1;

        for (j = 0; j < half_size; j++) {
            ur = fft_cos_lookup[(N / stage_size) * j];
            ui = fft_sin_lookup[(N / stage_size) * j]; // D?u "+" cho IFFT

            for (i = j; i < N; i += stage_size) {
                k = i + half_size;
                tr = ur * REX[k] - ui * IMX[k];
                ti = ur * IMX[k] + ui * REX[k];
                REX[k] = REX[i] - tr;
                IMX[k] = IMX[i] - ti;
                REX[i] = REX[i] + tr;
                IMX[i] = IMX[i] + ti;
            }
        }
    }

    // 3. Chu?n h�a k?t qu?
    for (i = 0; i < N; i++) {
        REAL_OUT[i] = REX[i] * norm_factor;
        IMAG_OUT[i] = IMX[i] * norm_factor;
        MAG_OUT[i] = sqrt(REAL_OUT[i] * REAL_OUT[i] + IMAG_OUT[i] * IMAG_OUT[i]);
        if (MAG_OUT[i] > 1023) MAG_OUT[i] = 1023;
        pwm_buffer[i] = (MAG_OUT[i] > 0) ? (uint16_t)round(MAG_OUT[i]) : 0;
    }
    pwm_ready = 1;
}

void mag_to_buf()
{
    uint8_t i;

    // Convert magnitude to bar graph display on LCD
    for(i = 1; i <=16; i++)
    {
        MAG[i] = sqrt(REX[i]*REX[i] + IMX[i]*IMX[i]);
        // Scaling magnitude to fit the LCD bar graph maximum value
        MAG[i] /= 128;

        // Fill LCD row buffer
        if (MAG[i] > 15)
        {
            lcd_buf_top[i] = 7;
            lcd_buf_bot[i] = 7;
        }
        else if (MAG[i] > 7)
        {
            lcd_buf_top[i] = MAG[i] - 7 - 1;
            lcd_buf_bot[i] = 7;
        }
        else
        {
            lcd_buf_top[i] = ' ';
            lcd_buf_bot[i] = MAG[i];
        }
    }
}

