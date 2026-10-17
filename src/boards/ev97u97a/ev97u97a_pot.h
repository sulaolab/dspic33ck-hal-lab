/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef EV97U97A_POT_H
#define EV97U97A_POT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ev97u97a_pot_init(void);

uint16_t ev97u97a_pot_read(void);

const char *ev97u97a_pot_fault(void);

#ifdef __cplusplus
}
#endif

#endif
