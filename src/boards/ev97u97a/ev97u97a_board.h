/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef EV97U97A_BOARD_H
#define EV97U97A_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "nora_spi_i2s_tdm.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile bool g_ev97u97a_clock_on_target;
extern volatile uint16_t g_ev97u97a_clock_diag;
extern volatile uint32_t g_ev97u97a_console_baud;
extern volatile bool g_ev97u97a_uart1_pins_init_ok, g_ev97u97a_user_io_pins_init_ok;
void ev97u97a_board_init(void);

bool ev97u97a_tdm_pins_init(nora_spi_i2s_tdm_clock_role_t role);

bool ev97u97a_i2c3_init(void);

#ifdef __cplusplus
}
#endif

#endif
