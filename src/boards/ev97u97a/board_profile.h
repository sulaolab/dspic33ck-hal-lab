/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef BOARD_PROFILE_H
#define BOARD_PROFILE_H

#if !defined(DSPIC33CK_BOARD_EV97U97A)
#error "boards/ev97u97a/board_profile.h was included without DSPIC33CK_BOARD_EV97U97A -- the configuration's extra-include-directories and its preprocessor-macros disagree (firmware.X/nbproject/configurations.xml)."
#endif

#ifndef DEMO_ENABLE_WM8904_AUDIO
#define DEMO_ENABLE_WM8904_AUDIO 1
#endif

#ifndef DEMO_ENABLE_TDM_MASTER_LOOPBACK
#define DEMO_ENABLE_TDM_MASTER_LOOPBACK 0
#endif

#ifndef DEMO_ENABLE_I2C_PROBE
#define DEMO_ENABLE_I2C_PROBE 0
#endif

#ifndef PRE_GAIN_CODEC_DB_X10
#define PRE_GAIN_CODEC_DB_X10   0
#endif
#ifndef POST_GAIN_CODEC_DB_X10
#define POST_GAIN_CODEC_DB_X10  0
#endif

#define BOARD_CONSOLE_HAS_RX 1

#define BOARD_CONSOLE_USE_RX_ISR 0

#define DEMO_ENABLE_CONSOLE_COMMANDS 1

#endif
