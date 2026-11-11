
#include <stdint.h>
#include <stdbool.h>

#include "sdk_project_config.h"
#include "device_registers.h"
#include "interrupt_manager.h"
#include "ftm_pwm_driver.h"
#include "lpit_driver.h"
#include "peripherals_flash_1.h"

/* =========================================
 * FINAL PROJECT - MINI BODY CONTROL LIGHT
 * MCU: S32K144
 * ========================================= */

#define SW1_PIN              13U
#define SW1_MASK             (1UL << SW1_PIN)

#define PWM_CHANNEL          0U
#define MODE_COUNT           5U
#define DEBOUNCE_TICKS       20U

/* Last 4KB P-Flash sector.
 * Linker m_text must end at 0x0007EFFF.
 */
#define FLASH_DATA_ADDR      0x0007F000UL
#define FLASH_SECTOR_SIZE    0x1000UL
#define FLASH_RECORD_SIZE    8UL
#define FLASH_MAGIC          0x4C454431UL

/* =========================================
 * DEBUG VARIABLES
 * ========================================= */

volatile uint32_t g_tick_10ms = 0U;
volatile uint32_t g_button_count = 0U;
volatile uint32_t g_last_button_tick = 0U;

volatile uint8_t g_mode = 0U;
volatile uint8_t g_duty_percent = 0U;
volatile uint8_t g_fade_duty = 0U;

volatile uint8_t g_flash_loaded_duty = 0U;
volatile uint8_t g_flash_saved_duty = 0U;
volatile uint32_t g_flash_save_count = 0U;
volatile uint32_t g_flash_verify_ok = 0U;
volatile uint32_t g_flash_ready = 0U;

volatile status_t g_flash_status;

/* =========================================
 * DRIVER STATES
 * ========================================= */

static ftm_state_t g_ftm_state;
static flash_ssd_config_t g_flash_config;

/* Data source must be in RAM and aligned. */
static uint32_t g_flash_record[2]
    __attribute__((aligned(8)));

/* =========================================
 * FLASH LOAD
 * ========================================= */

static uint8_t Flash_LoadDuty(void)
{
    const volatile uint32_t *record =
        (const volatile uint32_t *)FLASH_DATA_ADDR;

    uint32_t magic = record[0];
    uint32_t value = record[1];

    uint8_t duty = (uint8_t)(value & 0xFFU);
    uint8_t inverted =
        (uint8_t)((value >> 8U) & 0xFFU);

    if ((magic == FLASH_MAGIC) &&
        ((value & 0xFFFF0000UL) == 0xA55A0000UL) &&
        (duty <= 100U) &&
        (((uint32_t)duty ^ (uint32_t)inverted) == 0xFFU))
    {
        return duty;
    }

    /* No valid saved configuration */
    return 0U;
}

/* =========================================
 * FLASH SAVE
 * ========================================= */

static status_t Flash_SaveDuty(uint8_t duty)
{
    status_t result;
    uint32_t primask;

    if ((g_flash_ready == 0U) || (duty > 100U))
    {
        return STATUS_ERROR;
    }

    /* Avoid unnecessary erase/write cycles */
    if (Flash_LoadDuty() == duty)
    {
        g_flash_saved_duty = duty;
        g_flash_verify_ok = 1U;
        return STATUS_SUCCESS;
    }

    g_flash_record[0] = FLASH_MAGIC;

    g_flash_record[1] =
        0xA55A0000UL |
        ((uint32_t)(duty ^ 0xFFU) << 8U) |
        (uint32_t)duty;

    /* Preserve previous global IRQ state */
    __asm volatile (
        "mrs %0, primask"
        : "=r"(primask)
    );

    /* No ISR may execute from P-Flash
     * during erase/program commands.
     */
    __asm volatile ("cpsid i" ::: "memory");

    /* Erase the reserved 4KB sector */
    result = FLASH_DRV_EraseSector(
        &g_flash_config,
        FLASH_DATA_ADDR,
        FLASH_SECTOR_SIZE
    );

    if (result == STATUS_SUCCESS)
    {
        /* Program one 8-byte phrase */
        result = FLASH_DRV_Program(
            &g_flash_config,
            FLASH_DATA_ADDR,
            FLASH_RECORD_SIZE,
            (const uint8_t *)g_flash_record
        );
    }

    /* Restore previous IRQ state */
    __asm volatile (
        "msr primask, %0"
        :
        : "r"(primask)
        : "memory"
    );

    if (result == STATUS_SUCCESS)
    {
        if (Flash_LoadDuty() == duty)
        {
            g_flash_saved_duty = duty;
            g_flash_save_count++;
            g_flash_verify_ok = 1U;
        }
        else
        {
            result = STATUS_ERROR;
            g_flash_verify_ok = 0U;
        }
    }
    else
    {
        g_flash_verify_ok = 0U;
    }

    return result;
}

/* =========================================
 * PWM CONTROL
 * ========================================= */

static void SetDuty(uint8_t percent)
{
    uint16_t duty_value;

    if (percent > 100U)
    {
        percent = 100U;
    }

    g_duty_percent = percent;

    duty_value = (uint16_t)
        (((uint32_t)percent * 32768U) / 100U);

    FTM_DRV_UpdatePwmChannel(
        INST_FLEXTIMER_PWM_1,
        PWM_CHANNEL,
        FTM_PWM_UPDATE_IN_DUTY_CYCLE,
        duty_value,
        0U,
        true
    );
}

/* =========================================
 * LPIT INTERRUPT - 10MS
 * ========================================= */

void LPIT0_Ch0_IRQHandler(void)
{
    if (LPIT_DRV_GetInterruptFlagTimerChannels(
            INST_LPIT_CONFIG_1, 1UL))
    {
        LPIT_DRV_ClearInterruptFlagTimerChannels(
            INST_LPIT_CONFIG_1, 1UL);

        g_tick_10ms++;
    }
}

/* =========================================
 * SW1 INTERRUPT
 * ========================================= */

void PORTC_IRQHandler(void)
{
    if ((PORTC->ISFR & SW1_MASK) != 0U)
    {
        uint32_t now = g_tick_10ms;

        PORTC->ISFR = SW1_MASK;

        if ((PTC->PDIR & SW1_MASK) == 0U)
        {
            if ((g_button_count == 0U) ||
                ((uint32_t)(now - g_last_button_tick)
                 >= DEBOUNCE_TICKS))
            {
                g_last_button_tick = now;
                g_button_count++;
            }
        }
    }
}

/* =========================================
 * RESTORE MODE FROM SAVED DUTY
 * ========================================= */

static uint8_t ModeFromDuty(uint8_t duty)
{
    switch (duty)
    {
        case 25U: return 1U;
        case 50U: return 2U;
        case 75U: return 3U;
        default:  return 0U;
    }
}

/* =========================================
 * MAIN
 * ========================================= */

int main(void)
{
    uint32_t handled_buttons = 0U;
    uint32_t last_fade_tick = 0U;

    uint8_t fade_up = 1U;
    uint8_t loaded_duty = 0U;

    /* 1. Clock */
    CLOCK_SYS_Init(
        g_clockManConfigsArr,
        CLOCK_MANAGER_CONFIG_CNT,
        g_clockManCallbacksArr,
        CLOCK_MANAGER_CALLBACK_CNT
    );

    CLOCK_SYS_UpdateConfiguration(
        0U,
        CLOCK_MANAGER_POLICY_AGREEMENT
    );

    /* 2. Pins */
    PINS_DRV_Init(
        NUM_OF_CONFIGURED_PINS0,
        g_pin_mux_InitConfigArr0
    );

    PORTC->PCR[SW1_PIN] =
        (PORTC->PCR[SW1_PIN] &
         ~(PORT_PCR_MUX_MASK |
           PORT_PCR_PE_MASK |
           PORT_PCR_PS_MASK |
           PORT_PCR_IRQC_MASK))
        | PORT_PCR_MUX(1U)
        | PORT_PCR_PE_MASK
        | PORT_PCR_PS_MASK
        | PORT_PCR_IRQC(0xAU);

    PTC->PDDR &= ~SW1_MASK;
    PORTC->ISFR = SW1_MASK;

    /* 3. Flash initialization */
    g_flash_status = FLASH_DRV_Init(
        &Flash_InitConfig0,
        &g_flash_config
    );

    if (g_flash_status == STATUS_SUCCESS)
    {
        g_flash_ready = 1U;

        loaded_duty = Flash_LoadDuty();
        g_flash_loaded_duty = loaded_duty;
        g_flash_saved_duty = loaded_duty;
    }

    /* 4. FTM PWM */
    FTM_DRV_Init(
        INST_FLEXTIMER_PWM_1,
        &flexTimer_pwm_1_InitConfig,
        &g_ftm_state
    );

    FTM_DRV_InitPwm(
        INST_FLEXTIMER_PWM_1,
        &flexTimer_pwm_1_PwmConfig
    );

    g_mode = ModeFromDuty(loaded_duty);
    SetDuty(50U);

    /* 5. LPIT */
    LPIT_DRV_Init(
        INST_LPIT_CONFIG_1,
        &lpit1_InitConfig
    );

    LPIT_DRV_InitChannel(
        INST_LPIT_CONFIG_1,
        0U,
        &lpit1_ChnConfig0
    );

    /* 6. Interrupts */
    INT_SYS_InstallHandler(
        PORTC_IRQn,
        PORTC_IRQHandler,
        (isr_t *)0
    );

    INT_SYS_ClearPending(PORTC_IRQn);
    INT_SYS_SetPriority(PORTC_IRQn, 2U);
    INT_SYS_EnableIRQ(PORTC_IRQn);

    INT_SYS_InstallHandler(
        LPIT0_Ch0_IRQn,
        LPIT0_Ch0_IRQHandler,
        (isr_t *)0
    );

    INT_SYS_ClearPending(LPIT0_Ch0_IRQn);
    INT_SYS_SetPriority(LPIT0_Ch0_IRQn, 3U);
    INT_SYS_EnableIRQ(LPIT0_Ch0_IRQn);

    LPIT_DRV_StartTimerChannels(
        INST_LPIT_CONFIG_1,
        1UL
    );

    /* 7. Main loop */
    for (;;)
    {
        uint32_t buttons = g_button_count;
        uint32_t tick = g_tick_10ms;

        if (buttons != handled_buttons)
        {
            handled_buttons++;

            g_mode = (uint8_t)
                ((g_mode + 1U) % MODE_COUNT);

            switch (g_mode)
            {
                case 0U:
                    SetDuty(0U);
                    break;

                case 1U:
                    SetDuty(25U);
                    break;

                case 2U:
                    SetDuty(50U);
                    break;

                case 3U:
                    SetDuty(75U);
                    break;

                case 4U:
                    g_fade_duty = 0U;
                    fade_up = 1U;
                    last_fade_tick = tick;
                    SetDuty(0U);
                    break;

                default:
                    g_mode = 0U;
                    SetDuty(0U);
                    break;
            }

            /* Save fixed brightness only.
             * Never save every fading tick.
             */
            if (g_mode != 4U)
            {
                g_flash_status =
                    Flash_SaveDuty(g_duty_percent);
            }
        }

        /* Fading every 30ms */
        if (g_mode == 4U)
        {
            if ((uint32_t)(tick - last_fade_tick) >= 3U)
            {
                last_fade_tick = tick;

                if (fade_up != 0U)
                {
                    if (g_fade_duty < 100U)
                    {
                        g_fade_duty++;
                    }
                    else
                    {
                        fade_up = 0U;
                    }
                }
                else
                {
                    if (g_fade_duty > 0U)
                    {
                        g_fade_duty--;
                    }
                    else
                    {
                        fade_up = 1U;
                    }
                }

                SetDuty(g_fade_duty);
            }
        }
    }
}
