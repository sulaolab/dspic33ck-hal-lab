/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef DSPIC33CK_BOARD_EV97U97A
#error "boards/ev97u97a/ev97u97a_board.c is EV97U97A-owned. Build it only in the CK1024MP710_EV97U97A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev97u97a_board.h"
#include "ev97u97a_led3_rgb.h"
#include "ev97u97a_pins.h"

#include "app_traps.h"
#include "uart_platform_stdio.h"

#include "nora_clock.h"
#include "nora_dma.h"
#include "nora_gpio.h"
#include "nora_i2c_master.h"
#include "nora_pps.h"
#include "nora_gpio_table.h"
#include "nora_clock_dspic33ck_bringup.h"
#include "nora_reset.h"
#include "nora_spi_i2s_tdm_dspic33ck_pins.h"
#include <xc.h>

#define EV97U97A_CLOCK_FRC_HZ          8000000UL
#ifndef EV97U97A_CLOCK_TARGET_FOSC_HZ
#define EV97U97A_CLOCK_TARGET_FOSC_HZ  200000000UL
#endif

#define EV97U97A_CONSOLE_BAUD_FAST     230400UL
#define EV97U97A_CONSOLE_BAUD_SAFE     9600UL

#define EV97U97A_CONSOLE_FAST_MIN_DIV  25UL

volatile nora_clock_status_t g_ev97u97a_clock_init_status =
    NORA_CLOCK_ERR_NOT_SUPPORTED;
volatile bool     g_ev97u97a_clock_on_target = false;
volatile uint16_t g_ev97u97a_osccon_after_switch = 0u;

volatile uint16_t g_ev97u97a_clock_diag = 0u;

volatile uint32_t g_ev97u97a_console_baud = 0u;

volatile bool g_ev97u97a_console_fast_ok = false;

volatile bool g_ev97u97a_uart1_pins_init_ok = false;

volatile bool g_ev97u97a_user_io_pins_init_ok = false;

static void ev97u97a_clock_init(void)
{
    static const nora_clock_dspic33ck_bringup_t req = {
        .source         = NORA_CLOCK_SOURCE_FRC,
        .input_hz       = EV97U97A_CLOCK_FRC_HZ,
        .target_fosc_hz = EV97U97A_CLOCK_TARGET_FOSC_HZ,
    };
    nora_clock_dspic33ck_bringup_result_t res;

    nora_clock_dspic33ck_bringup(&req, &res);

    g_ev97u97a_clock_init_status   = res.status;
    g_ev97u97a_osccon_after_switch = res.osccon_after_switch;
    g_ev97u97a_clock_on_target     = res.on_target;
    g_ev97u97a_clock_diag          = res.diag;
}

static bool ev97u97a_uart1_pins_init(void);
static bool ev97u97a_user_io_pins_init(void);

void ev97u97a_board_init(void)
{
    ev97u97a_clock_init();
    g_ev97u97a_uart1_pins_init_ok = ev97u97a_uart1_pins_init();

    g_ev97u97a_console_fast_ok =
        ((nora_clock_get_fcy_hz() / (4UL * EV97U97A_CONSOLE_BAUD_FAST)) >=
         EV97U97A_CONSOLE_FAST_MIN_DIV);
    g_ev97u97a_console_baud = g_ev97u97a_console_fast_ok ? EV97U97A_CONSOLE_BAUD_FAST
                                                        : EV97U97A_CONSOLE_BAUD_SAFE;
    uart_platform_stdio_init(g_ev97u97a_console_baud);

    g_ev97u97a_user_io_pins_init_ok = ev97u97a_user_io_pins_init();

    nora_dma_global_init();
}

static bool ev97u97a_uart1_pins_init(void)
{
    if (!nora_pinmux_route_output(NORA_PPS_OUTPUT_U1TX,
                                       EV97U97A_UART1_TX_RP,
                                       true)) {
        return false;
    }

    if (!nora_pinmux_route_input(NORA_PPS_INPUT_U1RX,
                                      EV97U97A_UART1_RX_RP)) {
        return false;
    }

    return true;
}

/* The RGB cathodes connect directly to PWM2H/3H/4H (no DM330030 bridge). */
static const nora_gpio_table_entry_t ev97u97a_user_io_pins[] = {
    {EV97U97A_LED1_PIN, &nora_gpio_cfg_output_low},
    {EV97U97A_LED2_PIN, &nora_gpio_cfg_output_low},
    {EV97U97A_RGB_RED_PIN, &nora_gpio_cfg_output_low},
    {EV97U97A_RGB_GREEN_PIN, &nora_gpio_cfg_output_low},
    {EV97U97A_RGB_BLUE_PIN, &nora_gpio_cfg_output_low},
    {EV97U97A_SW1_PIN, &nora_gpio_cfg_input_pullup},
    {EV97U97A_SW2_PIN, &nora_gpio_cfg_input_pullup},
    {EV97U97A_SW3_PIN, &nora_gpio_cfg_input_pullup},
    {EV97U97A_POT_PIN, &nora_gpio_cfg_analog_input},
};
static bool ev97u97a_user_io_pins_init(void)
{
    /* Release PPS on the RGB pads before enabling their dedicated PWM outputs. */
    return nora_gpio_table_apply(ev97u97a_user_io_pins,
        sizeof ev97u97a_user_io_pins / sizeof ev97u97a_user_io_pins[0]) &&
        nora_pps_clear_output(44u) && nora_pps_clear_output(42u) && nora_pps_clear_output(65u);
}

static const nora_spi_i2s_tdm_pinmap_t ev97u97a_tdm_pinmap = {
    .bclk = EV97U97A_TDM_RP_BCLK,
    .fs   = EV97U97A_TDM_RP_FS,
    .sdo  = EV97U97A_TDM_RP_SDO,
    .sdi  = EV97U97A_TDM_RP_SDI,
};

bool ev97u97a_tdm_pins_init(nora_spi_i2s_tdm_clock_role_t role)
{
    return nora_spi_i2s_tdm_pins_configure(&ev97u97a_tdm_pinmap, role);
}

static bool ev97u97a_i2c3_pins_init(void)
{
    /* ALTI2C3=ON selects this fixed-function pair; no PPS route is involved. */
    if (!nora_gpio_rp_set_analog(EV97U97A_I2C3_RP_ASCL, false)) return false;
    if (!nora_gpio_rp_set_analog(EV97U97A_I2C3_RP_ASDA, false)) return false;

    return true;
}

#define EV97U97A_I2C3_BUS_HZ  400000UL

bool ev97u97a_i2c3_init(void)
{
    nora_i2c_config_t cfg;

    if (!ev97u97a_i2c3_pins_init()) {
        return false;
    }

    cfg.fcy_hz             = nora_clock_get_fcy_hz();
    cfg.bus_hz             = EV97U97A_I2C3_BUS_HZ;
    cfg.timeout_ms         = 0u;
    cfg.get_ms             = NULL;
    cfg.pending_timeout_ms = 0u;

    return nora_i2c_init(NORA_I2C_INST_3, &cfg) == NORA_I2C_OK;
}

uint16_t board_reset_cause_raw(void)
{
    return RCON;
}

const char *board_reset_cause_str(void)
{

    return nora_reset_cause_str(RCON, false);
}

volatile uint16_t *board_trap_bad_addr(void)
{
    return (volatile uint16_t *)0x0000u;
}

uint16_t board_trap_stack_beyond_limit(void)
{
    return 0xFFF0u;
}
