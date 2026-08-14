// CK lab WM8904 codec driver.

// #include "chip.h"
// #include "app_console.h"
// #include "most_task.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "wm8904_port.h"            /* delay_ms/delay_us/GetTicks/TRACE + APP_* framing (CK lab shim) */
#include "nora_i2c_master.h"
#if RESOLVED_BOARD_USE_CMSIS_I2C
#include "Driver_I2C_dsPIC33AK.h"
#endif
#include "wm8904_def.h"


#include "wm8904.h"




//===========================================================
// Definition
//===========================================================

#define WM8904_SLV_ADDR                 (0x34)
#define WM8904_HPOUT_VOL_DEFAULT        (57u - 6u)    // -6dB, same as previous startup setting


/*
 * Startup policy:
 *
 * - Normal 48 kHz config and 96 kHz DAC-only config use the manual
 *   headphone/DC-servo start-up sequence.
 * - The WM8904 internal/default start-up sequence experiment is kept below
 *   as a reference, but it is not called from config.
 * - The integrated shutdown sequence is still used before init to bring a
 *   codec that survived dsPIC reset closer to a known state.
 * - Before manual start-up, wm8904_hpout_quench_before_startup() is used for
 *   HPOUT residual-state discharge control.
 */


/*
 * WM8904 96 kHz ADC/DAC modes:
 * - SAMPLE_RATE register must still select the 48 kHz setting.
 * - CLK_SYS_RATE must select SYSCLK/fs = 128.
 * - BCLK_DIV = 00010 and LRCLK_RATE = 040h.
 * - Simultaneous ADC + DAC operation is not allowed at 88.2/96 kHz.
 */
#define WM8904_SAMPLE_RATE_REG_48K       (0x5u)
#define WM8904_CLK_SYS_RATE_128FS        (0x1u)
#define WM8904_BCLK_DIV_96K              (0x2u)
#define WM8904_LRCLK_RATE_96K            (0x040u)


//----------------------------------------------------------
// Configurable sample rate -- FULL standard menu, TDM8 path (8 slots x 32 bit): BCLK = fs x 256,
// LRCLK_RATE = 256. So per fs: CLK_SYS_RATE = SYSCLK/fs, BCLK_DIV = SYSCLK/(fs x 256).
//   (Phase B) 48k FAMILY (8/12/16/24/32/48k): FLL-less, SYSCLK = MCLK = 12.288 MHz.
//   (Phase C) 44.1k FAMILY (11.025/22.05/44.1k): SYSCLK = FLL output = 11.2896 MHz (12.288 cannot
//             divide to these). use_fll=true rows share the one FLL setting below.
// Codes verified vs WM8904 datasheet Rev4.1: R21 CLK_SYS_RATE/SAMPLE_RATE (p.100), R26 BCLK_DIV (p.94,
// note ÷6 = 0b00111 = 0x07, NOT 0x06 = ÷5.5), FLL R116-R120 (pp.104-108).
// NOTE the 44.1k-family rows reuse the SAME SAMPLE_RATE/CLK_SYS_RATE/BCLK_DIV codes as the
// 48/24/12k rows; only the SYSCLK source differs (FLL 11.2896M vs MCLK 12.288M), which is what
// turns 48k->44.1k, 24k->22.05k, 12k->11.025k.
//----------------------------------------------------------

// (Phase C) One shared FLL setting for the whole 44.1k family: MCLK 12.288 MHz -> SYSCLK 11.2896 MHz.
// FVCO = FREF(12.288M) x N.K(7.35) x FRATIO(1) = 90.3168 MHz (in the required 90-100 MHz window);
// FOUT = FVCO / OUTDIV(8) = 11.2896 MHz. FRACN_ENA=1, GAIN/CTRL_RATE at datasheet defaults.
#define WM8904_FLL_R117_VAL   ( WM8904_FLL_OUTDIV(7u) | WM8904_FLL_FRATIO(0u) )        // OUTDIV field=div-1: 7=÷8; FRATIO ÷1
#define WM8904_FLL_R118_VAL   ( WM8904_FLL_K(0x599Au) )                                // K = round(0.35*65536) = 22938
#define WM8904_FLL_R119_VAL   ( WM8904_FLL_N(7u) | WM8904_FLL_GAIN(0u) )               // N = 7, GAIN x1
#define WM8904_FLL_R120_VAL   ( WM8904_FLL_CLK_REF_DIV(0u) | WM8904_FLL_CLK_REF_SRC(0u) ) // REF = MCLK / 1
#define WM8904_FLL_LOCK_MS    (10u)   // fixed settle after FLL_ENA before selecting FLL as SYSCLK

typedef struct {
    uint32_t fs_hz;              // target sample rate
    uint8_t  sample_rate_code;   // R21[2:0]   SAMPLE_RATE
    uint8_t  clk_sys_rate_code;  // R21[13:10] CLK_SYS_RATE (SYSCLK/fs)
    uint8_t  bclk_div_code;      // R26[4:0]   BCLK_DIV (SYSCLK/BCLK; BCLK = fs x 256 for TDM8)
    bool     use_fll;            // false: SYSCLK = MCLK 12.288M; true: SYSCLK = FLL 11.2896M (44.1k family)
} wm8904_rate_cfg_t;

static const wm8904_rate_cfg_t s_wm8904_rates[] = {
    //   fs        SAMPLE_RATE   CLK_SYS_RATE        BCLK_DIV        use_fll
    {  8000u,      0x0u,         0x9u /* 1536 */,    0x07u /* ÷6   */, false },
    { 11025u,      0x1u,         0x7u /* 1024 */,    0x04u /* ÷4   */, true  },  // FLL 11.2896M/1024
    { 12000u,      0x1u,         0x7u /* 1024 */,    0x04u /* ÷4   */, false },
    { 16000u,      0x2u,         0x6u /*  768 */,    0x03u /* ÷3   */, false },
    { 22050u,      0x3u,         0x5u /*  512 */,    0x02u /* ÷2   */, true  },  // FLL 11.2896M/512
    { 24000u,      0x3u,         0x5u /*  512 */,    0x02u /* ÷2   */, false },
    { 32000u,      0x4u,         0x4u /*  384 */,    0x01u /* ÷1.5 */, false },
    { 44100u,      0x5u,         0x3u /*  256 */,    0x00u /* ÷1   */, true  },  // FLL 11.2896M/256
    { 48000u,      0x5u,         0x3u /*  256 */,    0x00u /* ÷1   */, false },
};
#define WM8904_RATE_COUNT   (sizeof(s_wm8904_rates)/sizeof(s_wm8904_rates[0]))

// Per-I2C-instance selected sample rate (default 48 kHz). Indexed by the I2C instance number
// (I2C_INST_A=2, I2C_INST_B=3). Set per codec via wm8904_set_rate_hz(); default 48k until changed.
#define WM8904_INST_MAX     (4u)
static uint32_t s_fs_hz[WM8904_INST_MAX] = { 48000u, 48000u, 48000u, 48000u };
static bool s_io_ok[WM8904_INST_MAX] = { true, true, true, true };

// --- Declick research state ---
// One-shot restart-strategy bitmask consumed by the next (re)configure. 0 == baseline.
static uint8_t s_declick_pending = (uint8_t)WM8904_DECLICK_NONE;
// Retained DC-servo offset values captured after a full STARTUP servo run, per instance
// (R73..R76 = LINEOUTR, LINEOUTL, HPOUTR, HPOUTL), for the WARM_SERVO (DCS_TRIG_DAC_WR) restore.
static uint8_t s_dcs_val[WM8904_INST_MAX][4] = { { 0 } };
static bool    s_dcs_valid[WM8904_INST_MAX]  = { false, false, false, false };

static const wm8904_rate_cfg_t* wm8904_find_rate( uint32_t fs_hz );


// compatible -- upstream's default. wm8904_port.h (included above) decides first on this
// tree: it compiles TRACE out unless ENA_WM8904_TRACE=1, because printf was costing this
// 64 KB part ~8 KB and this driver was its only caller. See that header for the numbers.
#ifndef TRACE
#define TRACE                  printf
#endif






//===========================================================
// Enum & Struct typedef
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

static void     wm8904_write_reg(uint8_t inst, uint8_t uc_register_address, uint16_t us_data);
static uint16_t wm8904_read_reg(uint8_t inst, uint8_t uc_register_address);
static void     wm8904_verify_write_readback(uint8_t inst, uint8_t uc_register_address, uint16_t us_data, uint16_t read_dat);
static bool     wm8904_confirm_device_id(uint8_t inst);

static void     wm8904_config_digital_audio_interface_96k(uint8_t inst, bool master_cfg, bool dac_path);

static void     wm8904_config_96k_adc_only(uint8_t inst, bool master_cfg);
static void     wm8904_config_96k_dac_only(uint8_t inst, bool master_cfg);
static void     wm8904_config(uint8_t inst, bool master_cfg);

static void     wm8904_write_dac_digital_mute(uint8_t inst, bool mute, bool ena96k);
static void     wm8904_write_hpout_level_mute(uint8_t inst, bool mute);

static bool     wm8904_wait_dc_servo_startup_done(uint8_t inst, uint16_t mask, uint32_t timeout_ms);
static bool     wm8904_wait_write_sequencer_done(uint8_t inst, uint32_t timeout_ms);

//backup static bool     wm8904_integrated_startup_sequence(uint8_t inst);
//backup static bool     wm8904_integrated_shutdown_sequence(uint8_t inst);
static void     wm8904_hpout_quench_before_startup(uint8_t inst);

// --- Declick research helpers ---
static void     wm8904_hpout_ordered_disable(uint8_t inst);      // C: Table 42-ordered HP disable
static void     wm8904_capture_dc_servo(uint8_t inst);           // B: store R73..R76 after STARTUP
static bool     wm8904_apply_dc_servo_warm(uint8_t inst);        // B: restore via DCS_TRIG_DAC_WR
static void     wm8904_hpout_ramp_unmute(uint8_t inst);          // D: stepped HPOUT analog unmute (ramp up)
static void     wm8904_hpout_ramp_mute(uint8_t inst);            // E: stepped HPOUT gain ramp-down + mute
static bool     wm8904_wseq_shutdown(uint8_t inst);              // A: vendor Write Sequencer shutdown
static bool     wm8904_wseq_hp_enable(uint8_t inst);             // F: vendor WSEQ HP-enable block (idx 12)

/* Map legacy 1-based I2C instance to the HAL instance enum / CMSIS driver. */
#if RESOLVED_BOARD_USE_CMSIS_I2C
static ARM_DRIVER_I2C *wm8904_i2c_cmsis_driver(uint8_t inst);
#else
static nora_i2c_instance_t wm8904_i2c_hal_inst(uint8_t inst);
#endif




//===========================================================
// Variables
//===========================================================





//===========================================================
// Global Function
//===========================================================

bool wm8904_init( uint8_t inst, bool master_cfg )
{
    TRACE(" wm8904_init(I2C-%d): start. TDM master=%s @%ld\n", inst, (master_cfg)?"on":"off", GetTicks());

    if( inst >= WM8904_INST_MAX )
    {
        TRACE(" wm8904_init(%d): invalid instance\n", inst);
        return false;
    }
    s_io_ok[inst] = true;

    if( wm8904_confirm_device_id(inst) )
    {
        // CPU reset does not reset WM8904.
        // If WM8904 is still alive from the previous run,
        // discharge/quench the HPOUT residual state before software reset.
        wm8904_hpout_quench_before_startup(inst);

        // configure WM8904
        wm8904_config(inst, master_cfg);
    }
    else
    {
        TRACE(" Cancel starting up WM8904 I2C inst=%d master_cfg=%d\n", inst, master_cfg);
    }

    TRACE(" wm8904_init(%d): end. @%ld\n", inst, GetTicks());
    TRACE(" wm8904_init(%d): apply=%s\n", inst, s_io_ok[inst] ? "verified" : "FAILED" );
    TRACE("\n");
    return s_io_ok[inst];
}


bool wm8904_is_distinct_slave( uint8_t inst )
{
    // No WM8904 answers on this bus at all -> definitely not present here.
    if( !wm8904_confirm_device_id(inst) )
    {
        return false;
    }

    // A genuine slave codec is configured with BCLK_DIR clear. If the master bit
    // is already set, the device answering on this bus is the master codec (e.g.
    // MikroA) aliased here by a bridged A/B I2C -- not a separate slave. Read the
    // master/slave bit, which is currently the one register A and a real B differ
    // on (A=master, real B=slave). AUDIO_INTERFACE_1 is a normal R/W register, so
    // it reads back the configured value (unlike R0, which always reads the ID).
    uint16_t aif1 = wm8904_read_reg( inst, WM8904_AUDIO_INTERFACE_1 );
    bool is_master = (aif1 & WM8904_BCLK_DIR) != 0u;
    TRACE(" wm8904_is_distinct_slave(%d): AIF1=0x%04x master_bit=%d -> %s\n",
          inst, aif1, (int)is_master, is_master ? "alias(skip)" : "present");
    return !is_master;
}


void wm8904_init_96k_adc_only( uint8_t inst, bool master_cfg )
{
    TRACE(" wm8904_init_96k_adc_only(I2C-%d): start. TDM master=%s @%ld\n", inst, (master_cfg)?"on":"off", GetTicks());

    if( wm8904_confirm_device_id(inst) )
    {
        // CPU reset does not reset WM8904.
        // If WM8904 is still alive from the previous run,
        // discharge/quench the HPOUT residual state before software reset.
        // 96 kHz ADC-only does not use the HPOUT path.
        // Do not run HPOUT quench here.

        // configure WM8904 for 96 kHz ADC-only mode.
        wm8904_config_96k_adc_only(inst, master_cfg);
    }
    else
    {
        TRACE(" Cancel starting up WM8904 96k ADC-only I2C inst=%d master_cfg=%d\n", inst, master_cfg);
    }

    TRACE(" wm8904_init_96k_adc_only(%d): end. @%ld\n", inst, GetTicks());
    TRACE("\n");
}


void wm8904_init_96k_dac_only( uint8_t inst, bool master_cfg )
{
    TRACE(" wm8904_init_96k_dac_only(I2C-%d): start. TDM master=%s @%ld\n", inst, (master_cfg)?"on":"off", GetTicks());

    if( wm8904_confirm_device_id(inst) )
    {
        // CPU reset does not reset WM8904.
        // If WM8904 is still alive from the previous run,
        // discharge/quench the HPOUT residual state before software reset.
        wm8904_hpout_quench_before_startup(inst);

        // configure WM8904 for 96 kHz DAC-only mode.
        wm8904_config_96k_dac_only(inst, master_cfg);
    }
    else
    {
        TRACE(" Cancel starting up WM8904 96k DAC-only I2C inst=%d master_cfg=%d\n", inst, master_cfg);
    }

    TRACE(" wm8904_init_96k_dac_only(%d): end. @%ld\n", inst, GetTicks());
    TRACE("\n");
}


void wm8904_shutdown( uint8_t inst )
{
    // SHUTDOWN discharge policy. Measurement (research doc) showed the vendor Control Write Sequencer
    // shutdown is the only strategy that suppresses the pop, so it is now the DEFAULT (mask NONE). The
    // declick mask can still force the alternatives for A/B regression.
    const uint8_t declick = s_declick_pending;

    if( (declick & WM8904_DECLICK_LEGACY_QUENCH) != 0u )
    {
        // Regression: the pre-declick quench (RMV_SHORT=1 -> 0), for comparison vs the new default.
        wm8904_hpout_quench_before_startup(inst);
        return;
    }

    if( (declick & WM8904_DECLICK_ORDERED_SHUTDN) != 0u )
    {
        // C: Table 42-ordered headphone disable (RMV_SHORT=0 first, then all ENA bits off).
        wm8904_hpout_ordered_disable(inst);
        return;
    }

    // Default (NONE) and explicit WSEQ (A): vendor Control Write Sequencer shutdown (Table 89, ordered
    // RMV_SHORT->ENA->DCS->CP->DAC->CLK->PGA->BIAS->VMID with datasheet timing). Needs SYSCLK present;
    // falls back to the quench if the sequencer does not complete (e.g. no clock at cold entry).
    if( wm8904_wseq_shutdown(inst) )
    {
        return;
    }
    TRACE(" WM8904 WSEQ shutdown fell back to quench inst=%d\n", inst);
    wm8904_hpout_quench_before_startup(inst);
}




void wm8904_reg_write( uint8_t inst, uint8_t reg, uint16_t data )
{
    if( inst >= WM8904_INST_MAX ) { return; }
    wm8904_write_reg( inst, reg, data );
}

uint16_t wm8904_reg_read( uint8_t inst, uint8_t reg )
{
    if( inst >= WM8904_INST_MAX ) { return 0xFFFFu; }
    return wm8904_read_reg( inst, reg );
}


void wm8904_dump_reg( uint8_t inst )
{
    /*
     * This function is nothing BUT trace output -- it reads registers only in order to print
     * them. So it is compiled out with the trace (see wm8904_port.h): left in, it would issue
     * 249 I2C reads and discard every one, silently, which is worse than not being there.
     * The prototype stays either way, so a caller added later still links.
     */
#if ENA_WM8904_TRACE
    uint16_t reg_val = 0;

    TRACE("wm8904_dump_reg: inst=%d\n", inst);

    for( uint8_t reg=0x00; reg<=0xF8; reg++ )
    {
        reg_val = wm8904_read_reg(inst, reg);
        TRACE(" addr=0x%02x val=0x%04x\n", reg, reg_val);
    }
#else
    (void)inst;
#endif
}


// Returns whether the codec CONFIRMED the requested state -- see the header for why that is
// not cosmetic. The value is s_io_ok[inst], this driver's sticky per-instance I/O health,
// which every write path here already maintains: wm8904_write_reg() clears it on a failed
// transmit AND on a readback that disagrees with what was written. Returned from all three
// exits, the two declick ramps included, because a ramp is nothing but a sequence of those
// same writes.
bool wm8904_set_analog_output_mute( uint8_t inst, bool mute )
{
    if( inst >= WM8904_INST_MAX ) { return false; }

    // Declick research (D / SOFT_UNMUTE): when the one-shot mask is armed, unmute by ramping the HPOUT
    // volume in steps instead of the immediate mute-bit release. Mute (and every non-declick caller,
    // mask NONE) is unchanged.
    if( !mute && (s_declick_pending & WM8904_DECLICK_SOFT_UNMUTE) != 0u )
    {
        wm8904_hpout_ramp_unmute(inst);
        TRACE(" WM8904 analog output unmute(soft-ramp) inst=%d @%ld\n", inst, GetTicks());
        return s_io_ok[inst];
    }
    if( mute && (s_declick_pending & WM8904_DECLICK_SOFT_SHUTDOWN) != 0u )
    {
        wm8904_hpout_ramp_mute(inst);   // E: ramp HPOUT gain down BEFORE the hard mute/shutdown
        TRACE(" WM8904 analog output mute(soft-ramp-down) inst=%d @%ld\n", inst, GetTicks());
        return s_io_ok[inst];
    }

    wm8904_write_hpout_level_mute(inst, mute);

    TRACE(" WM8904 analog output %s inst=%d @%ld\n",
          (mute) ? "mute" : "unmute",
          inst,
          GetTicks());

    return s_io_ok[inst];
}


// --- Declick research one-shot strategy accessors (see wm8904.h / research doc) ---
void wm8904_set_pending_declick( uint8_t mask )
{
    s_declick_pending = mask;
}

uint8_t wm8904_get_pending_declick( void )
{
    return s_declick_pending;
}

bool wm8904_declick_servo_captured( uint8_t inst )
{
    return ( inst < WM8904_INST_MAX ) ? s_dcs_valid[inst] : false;
}






//===========================================================
// Local Function
//===========================================================

/*
 * \brief Write data to WM8904 Register.
 *
 * \param uc_register_address Register address to write
 * \param us_data Data to write.
 */
static void wm8904_write_reg(uint8_t inst, uint8_t uc_register_address, uint16_t us_data)
{
    uint16_t read_dat = 0;
    uint8_t  tx[3];


    tx[0] = uc_register_address;
    tx[1] = (uint8_t)((us_data >> 8) & 0xFFu);
    tx[2] = (uint8_t)(us_data & 0xFFu);

#if RESOLVED_BOARD_USE_CMSIS_I2C
    {
        ARM_DRIVER_I2C *drv = wm8904_i2c_cmsis_driver(inst);

        if( drv == NULL )
        {
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return;
        }

        if( drv->MasterTransmit((uint32_t)(WM8904_SLV_ADDR >> 1),
                                tx,
                                sizeof(tx),
                                false) != ARM_DRIVER_OK )
        {
            TRACE(" wm8904_write_reg(): CMSIS MasterTransmit failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return;
        }
    }
#else
    {
        nora_i2c_instance_t hal_inst = wm8904_i2c_hal_inst(inst);

        if( hal_inst == NORA_I2C_INST_COUNT )
        {
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return;
        }

        if( nora_i2c_write(hal_inst,
                                 (uint8_t)(WM8904_SLV_ADDR >> 1),
                                 tx,
                                 sizeof(tx)) != NORA_I2C_OK )
        {
            TRACE(" wm8904_write_reg(): I2C HAL write failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return;
        }
    }
#endif

    delay_us(100);  // keep same post-write settling as legacy path


    read_dat = wm8904_read_reg( inst, uc_register_address );

    wm8904_verify_write_readback(inst, uc_register_address, us_data, read_dat);
}




/*
 * \brief Verify WM8904 register readback after write.
 *
 * Some WM8904 registers contain trigger/update bits that do not read back as
 * the written command value. Keep those register-specific exceptions here so
 * wm8904_write_reg() stays focused on the I2C write sequence.
 */
static void wm8904_verify_write_readback(uint8_t inst, uint8_t uc_register_address, uint16_t us_data, uint16_t read_dat)
{
    switch( uc_register_address )
    {
    case 0x00:
        // R0 always reads back as the device ID 0x8904, not the written reset command.
        break;

    case WM8904_WRITE_SEQUENCER_3:
        // WSEQ_START is a trigger bit. Readback may not match the written value.
        break;

    case WM8904_DC_SERVO_1:
        /*
         * R68 / DC Servo 1 contains DC-servo trigger bits.
         *
         * DCS_TRIG_STARTUP_x and DCS_TRIG_DAC_WR_x are command/status bits:
         * writing 1 starts a correction, while readback 1 means the correction
         * is still in progress. After completion, readback can return 0 even
         * though the command write was accepted.
         *
         * Therefore full write/readback equality is not a valid check for this
         * register. Completion is checked separately by reading
         * WM8904_DC_SERVO_READBACK_0.
         */
        break;

    case WM8904_DAC_DIGITAL_VOLUME_LEFT:
    case WM8904_DAC_DIGITAL_VOLUME_RIGHT:
    case WM8904_ADC_DIGITAL_VOLUME_LEFT:
    case WM8904_ADC_DIGITAL_VOLUME_RIGHT:
        // WM8904_DAC_VU and WM8904_ADC_VU are same 0x0100.
        if( (us_data & ~(0x0100)) != read_dat )
        {
            TRACE(" wm8904_write_reg(): unmatch!! [reg:0x%x] w_data=0x%04x r_dat=0x%04x\n",
                                                   uc_register_address, us_data, read_dat);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        }
        break;

    case WM8904_ANALOGUE_OUT1_LEFT:
    case WM8904_ANALOGUE_OUT1_RIGHT:
        if( (us_data & ~WM8904_HPOUT_VU) != read_dat )
        {
            TRACE(" wm8904_write_reg(): unmatch!! [reg:0x%x] w_data=0x%04x r_dat=0x%04x\n",
                                                   uc_register_address, us_data, read_dat);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        }
        break;

    default:
        if( us_data != read_dat )
        {
            TRACE(" wm8904_write_reg(): unmatch!! [reg:0x%x] w_data=0x%04x r_dat=0x%04x\n",
                                                   uc_register_address, us_data, read_dat);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        }
        break;
    }
}




/*
 * \brief Read data from WM8904 Register.
 *
 * \param uc_register_address Register address to write
 * \return Register value.
 */
static uint16_t wm8904_read_reg(uint8_t inst, uint8_t uc_register_address)
{
#define RET_INVALID     (0xCECE)

    uint8_t tx[1];
    uint8_t rx[2];

    tx[0] = uc_register_address;

#if RESOLVED_BOARD_USE_CMSIS_I2C
    {
        ARM_DRIVER_I2C *drv = wm8904_i2c_cmsis_driver(inst);
        uint32_t addr7 = (uint32_t)(WM8904_SLV_ADDR >> 1);

        if( drv == NULL )
        {
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }

        if( drv->MasterTransmit(addr7, tx, sizeof(tx), true) != ARM_DRIVER_OK )
        {
            TRACE(" wm8904_read_reg(): CMSIS MasterTransmit failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }

        if( drv->MasterReceive(addr7, rx, sizeof(rx), false) != ARM_DRIVER_OK )
        {
            TRACE(" wm8904_read_reg(): CMSIS MasterReceive failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }
    }
#else
    {
        nora_i2c_instance_t hal_inst = wm8904_i2c_hal_inst(inst);

        if( hal_inst == NORA_I2C_INST_COUNT )
        {
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }

        if( nora_i2c_write_read(hal_inst,
                                     (uint8_t)(WM8904_SLV_ADDR >> 1),
                                     tx,
                                     sizeof(tx),
                                     rx,
                                     sizeof(rx)) != NORA_I2C_OK )
        {
            TRACE(" wm8904_read_reg(): I2C HAL read failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }
    }
#endif

    return ((((uint16_t)rx[0]) << 8) | rx[1]);
}


#if RESOLVED_BOARD_USE_CMSIS_I2C
/* Map legacy 1-based I2C instance to the CMSIS driver handle.
 * I2C peripheral init is done from main via Initialize()/PowerControl(). */
static ARM_DRIVER_I2C *wm8904_i2c_cmsis_driver(uint8_t inst)
{
    switch( inst )
    {
    case 1:
        return &Driver_I2C0;
    case 2:
        return &Driver_I2C1;
    case 3:
        return &Driver_I2C2;
    case 4:
        return &Driver_I2C3;
    default:
        return NULL;
    }
}
#else
/* Map legacy 1-based I2C instance to the I2C HAL instance enum.
 * I2C peripheral init is done from main via nora_i2c_init(). */
static nora_i2c_instance_t wm8904_i2c_hal_inst(uint8_t inst)
{
    switch( inst )
    {
    case 1:
        return NORA_I2C_INST_1;
    case 2:
        return NORA_I2C_INST_2;
    case 3:
        return NORA_I2C_INST_3;
    default:
        return NORA_I2C_INST_COUNT;
    }
}
#endif



static bool wm8904_confirm_device_id( uint8_t inst )
{
    uint16_t data = 0;

    data = wm8904_read_reg(inst, WM8904_SW_RESET_AND_ID);
    if(data != 0x8904)
    {
        if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        TRACE(" wm8904_confirm_device_id(%d): Error!! Failed to read WM8904 device ID\n", inst);
        return false;
    }
    else
    {
        TRACE(" wm8904_confirm_device_id(%d): WM8904 dev ID is 0x8904(good)\n", inst);
        return true;
    }
}



static void wm8904_config_digital_audio_interface_96k(uint8_t inst, bool master_cfg, bool dac_path)
{
    uint16_t data = 0;

    data  = 0x0050;

    if( dac_path )
    {
#if defined(WM8904_SWAP_DAC_LR)
        data &= ~(WM8904_AIFDACR_SRC);
        data |= WM8904_AIFDACL_SRC;   // Left DAC source = Right digital channel
#endif //defined(WM8904_SWAP_DAC_LR)
    }
    else
    {
#if defined(WM8904_SWAP_ADC_LR)
        data &= ~(WM8904_AIFADCR_SRC);
        data |= WM8904_AIFADCL_SRC;   // Left digital audio channel source = Right ADC
#endif //defined(WM8904_SWAP_ADC_LR)
    }

    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_0,   data );

    data =  WM8904_AIF_WL_32BIT;
#if APP_SLOTS_PER_FS == 2u
    data |= WM8904_AIF_FMT_I2S;
#else
    data |= WM8904_AIF_FMT_DSP;
  #if APP_USE_1_BIT_DELAY == 0u
    data |= WM8904_AIF_LRCLK_INV;   // set 1 means "no 1bit delay". it's tricky.
  #endif // APP_USE_1_BIT_DELAY == 0u
#endif // APP_SLOTS_PER_FS == 2u

    if( master_cfg )
    {
        data |= WM8904_BCLK_DIR;
    }
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_1,   data );

    data = WM8904_LRCLK_RATE(WM8904_LRCLK_RATE_96K);
    if( master_cfg )
    {
        data |= WM8904_LRCLK_DIR;
    }
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_3,   data );
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_2,   WM8904_BCLK_DIV(WM8904_BCLK_DIV_96K) );
}


static void wm8904_config_96k_adc_only( uint8_t inst, bool master_cfg )
{
    // --- Step 1: Software Reset ---
    wm8904_write_reg( inst, WM8904_SW_RESET_AND_ID,     0x0000 ); // R0 - SW Reset and ID
    delay_ms(50);

    wm8904_write_reg( inst, WM8904_BIAS_CONTROL_0,      WM8904_ISEL_HP_BIAS );
    wm8904_write_reg( inst, WM8904_VMID_CONTROL_0,      WM8904_VMID_BUF_ENA | WM8904_VMID_RES_LP | WM8904_VMID_ENA );
    delay_ms(5);
    wm8904_write_reg( inst, WM8904_BIAS_CONTROL_0,      WM8904_ISEL_HP_BIAS | WM8904_BIAS_ENA | 0x10 ); //0x10: secret? datasheet recommendation=0x19 (Low Power Playback Mode Disable)

    // 96 kHz ADC-only: enable input PGAs, keep headphone/lineout and DAC path disabled.
    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_0,  WM8904_INL_ENA | WM8904_INR_ENA );
    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_2,  0x0000 );
    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT12_ZC,   0x0000 );
    wm8904_write_reg( inst, WM8904_CHARGE_PUMP_0,       0x0000 );

    // Restore signal phase: WM8904 single-ended input PGA is internally inverting.
    // ADC_OSR128 must be 0 for 88.2/96 kHz ADC operation.
    // ADC_128_OSR_TST_MODE and ADC_BIASX1P5 remain 0 after software reset.
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_0,       WM8904_ADC_HPF | WM8904_ADCL_DATINV | WM8904_ADCR_DATINV );
    wm8904_write_reg( inst, WM8904_ANALOGUE_ADC_0,      0x0000 );
//    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_1,       WM8904_DEEMPH(0) ); // DAC_OSR128=0
    wm8904_write_dac_digital_mute( inst, true, true );

    // 96 kHz ADC-only: DACs must be disabled.
    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_6,  WM8904_ADCL_ENA | WM8904_ADCR_ENA );

    wm8904_write_reg( inst, WM8904_FLL_CONTROL_1,       0x0000);  // disabled FLL
    wm8904_write_reg( inst, WM8904_CLOCK_RATES_1,       WM8904_CLK_SYS_RATE(WM8904_CLK_SYS_RATE_128FS) | WM8904_SAMPLE_RATE(WM8904_SAMPLE_RATE_REG_48K) );
    wm8904_write_reg( inst, WM8904_CLOCK_RATES_0,       0x0000 );  // 0: SYSCLK = MCLK
    wm8904_write_reg( inst, WM8904_CLOCK_RATES_2,       WM8904_CLK_SYS_ENA | WM8904_CLK_DSP_ENA );

    wm8904_config_digital_audio_interface_96k(inst, master_cfg, false);

//
// Microchip WM8904 X32 Eval PCB equips RED and BLUE connectors
//
#if RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK
//////////////
// IN1(RED)
//////////////
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_1,  WM8904_L_MODE_SINGLE_ENDED | WM8904_L_IP_SEL_N_IN1L ); // in single-ended mode, use N only
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_1, WM8904_R_MODE_SINGLE_ENDED | WM8904_R_IP_SEL_N_IN1R ); // in single-ended mode, use N only
#else
//////////////
// IN2(BLUE)
//////////////
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_1,  WM8904_L_MODE_SINGLE_ENDED | WM8904_L_IP_SEL_N_IN2L ); // in single-ended mode, use N only
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_1, WM8904_R_MODE_SINGLE_ENDED | WM8904_R_IP_SEL_N_IN2R ); // in single-ended mode, use N only
#endif // RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK

//
// enabling / disabling MIC Bias voltage from WM8904
//
#if RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED
    // MIC related settings (BIAS voltage ON)
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_0,     WM8904_MICBIAS_ENA );
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_1,     WM8904_MICBIAS_SEL(0x1) ); // 001 = 10/9 x AVDD (2.0V)
    wm8904_write_reg( inst, WM8904_MIC_FILTER_CONTROL,     0x0000 );                  // disabled MICDET / Hook switch detection
    wm8904_write_reg( inst, WM8904_DIGITAL_MICROPHONE_0,   0x0000 );                  // disabled PDM mic
#else
    // MIC related settings (BIAS voltage OFF)
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_0,     0x0000 );
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_1,     0x0000 );
    wm8904_write_reg( inst, WM8904_MIC_FILTER_CONTROL,     0x0000 );
    wm8904_write_reg( inst, WM8904_DIGITAL_MICROPHONE_0,   0x0000 );
#endif // RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED


    ///////////////////////////
    // input gain settings
    ///////////////////////////
//org    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_0,    WM8904_LIN_VOL(0x5) ); // 00101 = +0.0 dB (default)
//org    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_0,   WM8904_RIN_VOL(0x5) ); // 00101 = +0.0 dB (default)
    // to reduce the hissing noise at A/D process
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_0,    WM8904_LIN_VOL(0x0) ); // 00000 = -1.5 dB
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_0,   WM8904_RIN_VOL(0x0) ); // 00000 = -1.5 dB

    // to reduce hissing noise on input side
    //  Gain[dB]=(VOL_CODE - 0xC0) * 0.375
    //  Gain[dB] / 0.375 + 0xC0(192) = VOL_CODE
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_LEFT,  WM8904_ADC_VU | WM8904_ADCL_VOL(0xA5) );     // 0xA5 = -10.125 dB
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_RIGHT, WM8904_ADC_VU | WM8904_ADCR_VOL(0xA5) );     // 0xA5 = -10.125 dB

    delay_ms(20);
}


static void wm8904_config_96k_dac_only( uint8_t inst, bool master_cfg )
{
    // --- Step 1: Software Reset ---
    wm8904_write_reg( inst, WM8904_SW_RESET_AND_ID,     0x0000 ); // R0 - SW Reset and ID
    delay_ms(50);

    wm8904_write_reg( inst, WM8904_BIAS_CONTROL_0,      WM8904_ISEL_HP_BIAS );
    wm8904_write_reg( inst, WM8904_VMID_CONTROL_0,      WM8904_VMID_BUF_ENA | WM8904_VMID_RES_LP | WM8904_VMID_ENA );
    delay_ms(5);
    wm8904_write_reg( inst, WM8904_BIAS_CONTROL_0,      WM8904_ISEL_HP_BIAS | WM8904_BIAS_ENA | 0x10 ); //0x10: secret? datasheet recommendation=0x19 (Low Power Playback Mode Disable)

    // 96 kHz DAC-only: ADC input PGAs are not used, headphone output path is used.
    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_0,  0x0000 );
    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_2,  WM8904_HPL_PGA_ENA | WM8904_HPR_PGA_ENA );
    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT12_ZC,   0x0000 );
    wm8904_write_reg( inst, WM8904_CHARGE_PUMP_0,       WM8904_CP_ENA );

    // 96 kHz DAC-only: ADCs must be disabled and DAC_OSR128 must be 0.
    // EQ_ENA remains 0 after software reset.
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_0,       0x0000 );
    wm8904_write_reg( inst, WM8904_ANALOGUE_ADC_0,      0x0000 );
//test    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_1,       WM8904_DEEMPH(0) ); // DAC_OSR128=0
    wm8904_write_dac_digital_mute( inst, true, true );

    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_6,  WM8904_DACL_ENA | WM8904_DACR_ENA );

    wm8904_write_reg( inst, WM8904_FLL_CONTROL_1,       0x0000);  // disabled FLL
    wm8904_write_reg( inst, WM8904_CLOCK_RATES_1,       WM8904_CLK_SYS_RATE(WM8904_CLK_SYS_RATE_128FS) | WM8904_SAMPLE_RATE(WM8904_SAMPLE_RATE_REG_48K) );
    wm8904_write_reg( inst, WM8904_CLOCK_RATES_0,       0x0000 );  // 0: SYSCLK = MCLK
    wm8904_write_reg( inst, WM8904_CLOCK_RATES_2,       WM8904_CLK_SYS_ENA | WM8904_CLK_DSP_ENA );

    wm8904_config_digital_audio_interface_96k(inst, master_cfg, true);

    // MIC related settings (BIAS voltage OFF)
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_0,     0x0000 );
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_1,     0x0000 );
    wm8904_write_reg( inst, WM8904_MIC_FILTER_CONTROL,     0x0000 );
    wm8904_write_reg( inst, WM8904_DIGITAL_MICROPHONE_0,   0x0000 );


    // Safety: preload OUT1 volume with analog MUTE before enabling the HP output path.
    // The application must explicitly call wm8904_set_analog_output_mute(inst, false) to output sound.
    wm8904_write_hpout_level_mute(inst, true);

    wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,          WM8904_HPL_ENA | WM8904_HPR_ENA );
    wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,          WM8904_HPL_ENA_DLY | WM8904_HPL_ENA | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );
    wm8904_write_reg( inst, WM8904_DC_SERVO_0,             WM8904_DCS_ENA_CHAN_3 | WM8904_DCS_ENA_CHAN_2 | WM8904_DCS_ENA_CHAN_1 | WM8904_DCS_ENA_CHAN_0 );
    wm8904_write_reg( inst, WM8904_DC_SERVO_1,             WM8904_DCS_TRIG_STARTUP_3 | WM8904_DCS_TRIG_STARTUP_2 | WM8904_DCS_TRIG_STARTUP_1 | WM8904_DCS_TRIG_STARTUP_0 );

    #define WM8904_DCS_STARTUP_COMPLETE_ALL_MASK_96K    (0x000F)
    #define WM8904_DCS_STARTUP_TIMEOUT_MS_96K           (500)
    if( !wm8904_wait_dc_servo_startup_done(inst,
                                           WM8904_DCS_STARTUP_COMPLETE_ALL_MASK_96K,
                                           WM8904_DCS_STARTUP_TIMEOUT_MS_96K) )
    {
        // For debug phase, continue the startup sequence even if timeout occurs.
        // The final register dump can show whether R4D is still incomplete.
    }


    wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,                                 WM8904_HPL_ENA_OUTP | WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                                                                                  WM8904_HPR_ENA_OUTP | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );
    wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,          WM8904_HPL_RMV_SHORT | WM8904_HPL_ENA_OUTP | WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                                                           WM8904_HPR_RMV_SHORT | WM8904_HPR_ENA_OUTP | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );


    ///////////////////////////
    // output gain settings
    ///////////////////////////
    // Startup default is analog MUTE. Keep muted until the application explicitly unmutes.
    wm8904_write_hpout_level_mute(inst, true);
    //  Gain[dB]=(VOL_CODE - 0xC0) * 0.375
    //  Gain[dB] / 0.375 + 0xC0(192) = VOL_CODE
    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_VOLUME_LEFT,  WM8904_DAC_VU | WM8904_DACL_VOL(0xC0) ); // 0dB
    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_VOLUME_RIGHT, WM8904_DAC_VU | WM8904_DACR_VOL(0xC0) ); // 0dB

    delay_ms(20);

    wm8904_write_dac_digital_mute( inst, false, true );  // digital unmute
}


// Look up one supported standard-menu rate row; NULL for values outside the
// 8/11.025/12/16/22.05/24/32/44.1/48 kHz menu. The row records whether the FLL is required.
static const wm8904_rate_cfg_t* wm8904_find_rate( uint32_t fs_hz )
{
    for( unsigned i = 0u; i < WM8904_RATE_COUNT; i++ )
    {
        if( s_wm8904_rates[i].fs_hz == fs_hz )
        {
            return &s_wm8904_rates[i];
        }
    }
    return NULL;
}

// (Phase B) Select the sample rate applied to `inst` on its NEXT (re)configuration. Stores only;
// the caller must re-init the codec (e.g. audio_transport_restart()) for it to take effect. Rejects any
// rate not in the standard menu (8/11.025/12/16/22.05/24/32/44.1/48k). In the independent
// codec-master topology either endpoint may be selected before a mute-bounded restart.
bool wm8904_set_rate_hz( uint8_t inst, uint32_t fs_hz )
{
    if( inst >= WM8904_INST_MAX )            { return false; }
    if( wm8904_find_rate( fs_hz ) == NULL )  { return false; }   // must be a table rate (48k or 44.1k family)
    s_fs_hz[inst] = fs_hz;
    return true;
}

// Currently-selected sample rate for `inst` (the value the next wm8904_config applies). Default 48k.
// Lets the app read the codec-domain rate (e.g. to re-tune the A-side DSP to A's rate).
uint32_t wm8904_get_rate_hz( uint8_t inst )
{
    return ( inst < WM8904_INST_MAX ) ? s_fs_hz[inst] : 48000u;
}


static void wm8904_config( uint8_t inst, bool master_cfg )
{
    uint16_t data = 0;

    // (Phase B) Resolve the target sample rate for this instance (default 48 kHz). Fail-safe to 48k
    // if somehow unset/out of range, so a bad value never leaves the codec unclocked.
    const wm8904_rate_cfg_t* rate =
        wm8904_find_rate( (inst < WM8904_INST_MAX) ? s_fs_hz[inst] : 48000u );
    if( rate == NULL )
    {
        if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        rate = wm8904_find_rate( 48000u );
    }
    TRACE(" wm8904_config(%d): fs=%luHz FLL=%s (SAMPLE_RATE=0x%x CLK_SYS_RATE=0x%x BCLK_DIV=0x%x)\n",
          inst, (unsigned long)rate->fs_hz, rate->use_fll ? "on(SYSCLK=11.2896M)" : "off(SYSCLK=MCLK)",
          (unsigned)rate->sample_rate_code, (unsigned)rate->clk_sys_rate_code,
          (unsigned)rate->bclk_div_code );

    // Declick research (B / WARM_SERVO): a warm restart skips the R0 software reset and restores the DC
    // servo from captured offsets via DCS_TRIG_DAC_WR (below), instead of collapsing VMID/charge-pump and
    // re-running the full STARTUP servo. Only taken when the one-shot mask asks AND a prior STARTUP run has
    // captured offsets for this instance; otherwise we fall through to the unchanged cold path. Baseline
    // (mask NONE / shipping / recovery) always resets.
    const bool warm = ( (s_declick_pending & WM8904_DECLICK_WARM_SERVO) != 0u )
                      && ( inst < WM8904_INST_MAX ) && s_dcs_valid[inst];

    // --- Step 1: Software Reset ---
    if( !warm )
    {
        wm8904_write_reg( inst, WM8904_SW_RESET_AND_ID,     0x0000 ); // R0 - SW Reset and ID
        delay_ms(50);
    }
    else
    {
        TRACE(" wm8904_config(%d): WARM restart -- skip SW-reset, DCS_TRIG_DAC_WR servo restore\n", inst);
    }

    wm8904_write_reg( inst, WM8904_BIAS_CONTROL_0,      WM8904_ISEL_HP_BIAS );
    wm8904_write_reg( inst, WM8904_VMID_CONTROL_0,      WM8904_VMID_BUF_ENA | WM8904_VMID_RES_LP | WM8904_VMID_ENA );
    delay_ms(5);

    wm8904_write_reg( inst, WM8904_BIAS_CONTROL_0,      WM8904_ISEL_HP_BIAS | WM8904_BIAS_ENA | 0x10 ); //0x10: secret? datasheet recommendation=0x19 (Low Power Playback Mode Disable)

    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_0,  WM8904_INL_ENA | WM8904_INR_ENA );
    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_2,  WM8904_HPL_PGA_ENA | WM8904_HPR_PGA_ENA );
    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT12_ZC,   0x0000 );
    wm8904_write_reg( inst, WM8904_CHARGE_PUMP_0,       WM8904_CP_ENA );

// test class G(regular)   wm8904_write_reg( inst, WM8904_CLASS_W_0,           WM8904_CP_DYN_PWR );

    // note:
    // WM8904 datasheet said "The input to the ADC is phase inverted with respect to the selected input pin."
    // Restore signal phase: WM8904 single-ended input PGA is internally inverting (ref: Datasheet p.38).
    // Enabling digital inversion at the ADC output stage to align with the input source phase.
    //
    // WM8904_ADC_HPF is keeping the default setting at the datasheet. There is no special meaning.
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_0,       WM8904_ADC_HPF | WM8904_ADCL_DATINV | WM8904_ADCR_DATINV );

    wm8904_write_reg( inst, WM8904_ANALOGUE_ADC_0,      WM8904_ADC_OSR128 );  //default

    wm8904_write_dac_digital_mute( inst, true, false );

    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_6,  WM8904_DACL_ENA | WM8904_DACR_ENA | WM8904_ADCL_ENA | WM8904_ADCR_ENA );


    // (Phase C) Clocking: 48k family runs SYSCLK = MCLK (FLL off); 44.1k family runs SYSCLK from the
    // FLL (12.288M -> 11.2896M). CLK_SYS is still disabled here (post SW-reset), so it is safe to set
    // up the source. FLL order (datasheet pp.104-108): program R117-R120, enable FLL (FLL_ENA after
    // FRACN_ENA), wait for lock, THEN select FLL as SYSCLK (SYSCLK_SRC) and enable CLK_SYS/DSP.
    if( rate->use_fll )
    {
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_2, WM8904_FLL_R117_VAL );
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_3, WM8904_FLL_R118_VAL );
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_4, WM8904_FLL_R119_VAL );
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_5, WM8904_FLL_R120_VAL );
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_1, WM8904_FLL_FRACN_ENA );                     // configure, FLL_ENA=0
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_1, WM8904_FLL_FRACN_ENA | WM8904_FLL_ENA );    // enable FLL
        delay_ms( WM8904_FLL_LOCK_MS );                                                           // let the FLL lock
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_1, WM8904_CLK_SYS_RATE(rate->clk_sys_rate_code) | WM8904_SAMPLE_RATE(rate->sample_rate_code) );
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_0, 0x0000 );
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_2, WM8904_SYSCLK_SRC | WM8904_CLK_SYS_ENA | WM8904_CLK_DSP_ENA );  // SYSCLK = FLL
    }
    else
    {
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_1, 0x0000 );  // FLL disabled
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_1, WM8904_CLK_SYS_RATE(rate->clk_sys_rate_code) | WM8904_SAMPLE_RATE(rate->sample_rate_code) );
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_0, 0x0000 );  // 0: SYSCLK = MCLK
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_2, WM8904_CLK_SYS_ENA | WM8904_CLK_DSP_ENA );  // SYSCLK_SRC=0 (MCLK)
    }


    data  = 0x0050;
#if defined(WM8904_SWAP_ADC_LR)
    data &= ~(WM8904_AIFADCR_SRC);
    data |= WM8904_AIFADCL_SRC;   // Left digital audio channel source = Right ADC
#endif //defined(WM8904_SWAP_ADC_LR)
#if defined(WM8904_SWAP_DAC_LR)
    data &= ~(WM8904_AIFDACR_SRC);
    data |= WM8904_AIFDACL_SRC;   // Left DAC source = Right digital channel
#endif //defined(WM8904_SWAP_DAC_LR)
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_0,   data );

// note!! regarding WM8904_AIF_LRCLK_INV
//
// I2S modes:
//  LRC polarity
//   0 = Not Inverted
//   1 = Inverted
//
//  DSP Mode Mode A-B:
//   0 = [   1-bit delay] MSB is available on 2nd BCLK rising edge after LRC rising edge (mode A)
//   1 = [no 1-bit delay] MSB is available on 1st BCLK rising edge after LRC rising edge (mode B)
//
    // WM8904_AUDIO_INTERFACE_1 settings
    data =  WM8904_AIF_WL_32BIT;
#if APP_SLOTS_PER_FS == 2u
    data |= WM8904_AIF_FMT_I2S;
#else
    data |= WM8904_AIF_FMT_DSP;
  #if APP_USE_1_BIT_DELAY == 0u
    data |= WM8904_AIF_LRCLK_INV;   // set 1 means "no 1bit delay". it's tricky. see above comment.
  #endif // APP_USE_1_BIT_DELAY == 0u
#endif // APP_SLOTS_PER_FS == 2u

    if( master_cfg )
    {
        data |= WM8904_BCLK_DIR;
    }
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_1,   data );


    // WM8904_AUDIO_INTERFACE_3 settings
#if APP_SLOTS_PER_FS == 2u
    COMPILEASSERT(APP_SLOTS_PER_FS == 2u);
           // 3) LRCLK output and rate: 64Fs (= 48 kHz at BCLK 3.072 MHz).
    data = WM8904_LRCLK_RATE(64);
#else
    COMPILEASSERT(APP_SLOTS_PER_FS == 8u);

    data = WM8904_LRCLK_RATE(256);
#endif // APP_SLOTS_PER_FS == 2u
    if( master_cfg )
    {
        data |= WM8904_LRCLK_DIR;
    }
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_3,   data );

#if APP_SLOTS_PER_FS == 2u
    // 2) BCLK divider: MCLK (12.288 MHz) -> BCLK = 3.072 MHz (= /4).
    //    Use the datasheet table to select the BCLK_DIV code for division by four.
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_2,   WM8904_BCLK_DIV(0x4) ); // 0x4:Divide by 4
#else
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_2,   WM8904_BCLK_DIV(rate->bclk_div_code) ); // (Phase B) TDM8: BCLK = fs x 256
#endif // APP_SLOTS_PER_FS == 2u


//
// Microchip WM8904 X32 Eval PCB equips RED and BLUE connectors
//
#if RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK
//////////////
// IN1(RED)
//////////////
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_1,  WM8904_L_MODE_SINGLE_ENDED | WM8904_L_IP_SEL_N_IN1L ); // in single-ended mode, use N only
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_1, WM8904_R_MODE_SINGLE_ENDED | WM8904_R_IP_SEL_N_IN1R ); // in single-ended mode, use N only
#else
//////////////
// IN2(BLUE)
//////////////
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_1,  WM8904_L_MODE_SINGLE_ENDED | WM8904_L_IP_SEL_N_IN2L ); // in single-ended mode, use N only
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_1, WM8904_R_MODE_SINGLE_ENDED | WM8904_R_IP_SEL_N_IN2R ); // in single-ended mode, use N only
#endif // RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK

//
// enabling / disabling MIC Bias voltage from WM8904
//
#if RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED
    // MIC related settings (BIAS voltage ON)
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_0,     WM8904_MICBIAS_ENA );
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_1,     WM8904_MICBIAS_SEL(0x1) ); // 001 = 10/9 x AVDD (2.0V)
    wm8904_write_reg( inst, WM8904_MIC_FILTER_CONTROL,     0x0000 );                  // disabled MICDET / Hook switch detection
    wm8904_write_reg( inst, WM8904_DIGITAL_MICROPHONE_0,   0x0000 );                  // disabled PDM mic
#else
    // MIC related settings (BIAS voltage OFF)
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_0,     0x0000 );
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_1,     0x0000 );
    wm8904_write_reg( inst, WM8904_MIC_FILTER_CONTROL,     0x0000 );
    wm8904_write_reg( inst, WM8904_DIGITAL_MICROPHONE_0,   0x0000 );
#endif // RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED


    /////////////////////////////////////////////////
    // DC servo manual startup sequence (start)
    ////////////////////////////////////////////////
    // Safety: preload OUT1 volume with analog MUTE before enabling the HP output path.
    // The application must explicitly call wm8904_set_analog_output_mute(inst, false) to output sound.
    wm8904_write_hpout_level_mute(inst, true);

    #define WM8904_DCS_STARTUP_COMPLETE_ALL_MASK    (0x000F)
    #define WM8904_DCS_STARTUP_TIMEOUT_MS           (500)

    // Declick research (F / WSEQ_STARTUP): run the pop-critical HP output-enable via the vendor Write
    // Sequencer (Table 88 idx 12..22) with datasheet timing, instead of the manual sequence below. Clocks/
    // interface/BIAS/VMID/CP/PGA were already brought up manually, so TDM/ADC config is preserved. On WSEQ
    // timeout we fall back to the manual sequence.
    bool hp_enable_done = false;
    if( (s_declick_pending & WM8904_DECLICK_WSEQ_STARTUP) != 0u )
    {
        if( wm8904_wseq_hp_enable(inst) )
        {
            wm8904_capture_dc_servo(inst);   // WSEQ ran the STARTUP servo -> capture for WARM_SERVO
            hp_enable_done = true;
        }
        else
        {
            TRACE(" WM8904 WSEQ HP-enable fell back to manual inst=%d\n", inst);
        }
    }

    if( !hp_enable_done )
    {
        wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,                               WM8904_HPL_ENA |
                                                                                    WM8904_HPR_ENA );
        delay_us(20);

        wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,          WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                                                               WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );
        wm8904_write_reg( inst, WM8904_DC_SERVO_0,             WM8904_DCS_ENA_CHAN_3 | WM8904_DCS_ENA_CHAN_2 | WM8904_DCS_ENA_CHAN_1 | WM8904_DCS_ENA_CHAN_0 );

        // Declick research (B): a warm restart restores the retained DC-servo offsets with DCS_TRIG_DAC_WR
        // (~2ms/ch) instead of a full STARTUP measurement (~86ms/ch). The cold path additionally CAPTURES the
        // measured offsets so a subsequent warm restart has values to restore. wm8904_apply_dc_servo_warm()
        // returns false only if the (guarded) offsets went stale, in which case we run the full STARTUP.
        if( !warm || !wm8904_apply_dc_servo_warm(inst) )
        {
            wm8904_write_reg( inst, WM8904_DC_SERVO_1,         WM8904_DCS_TRIG_STARTUP_3 | WM8904_DCS_TRIG_STARTUP_2 | WM8904_DCS_TRIG_STARTUP_1 | WM8904_DCS_TRIG_STARTUP_0 );
            if( !wm8904_wait_dc_servo_startup_done(inst,
                                                   WM8904_DCS_STARTUP_COMPLETE_ALL_MASK,
                                                   WM8904_DCS_STARTUP_TIMEOUT_MS) )
            {
                // For debug phase, continue the startup sequence even if timeout occurs.
                // The final register dump can show whether R4D is still incomplete.
            }
            else
            {
                wm8904_capture_dc_servo(inst);   // store measured offsets for future WARM_SERVO restores
            }
        }

        wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,                                 WM8904_HPL_ENA_OUTP | WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                                                                                      WM8904_HPR_ENA_OUTP | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );
        wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,          WM8904_HPL_RMV_SHORT | WM8904_HPL_ENA_OUTP | WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                                                               WM8904_HPR_RMV_SHORT | WM8904_HPR_ENA_OUTP | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );
    }
    /////////////////////////////////////////////////
    // DC servo manual startup sequence (end)
    ////////////////////////////////////////////////


    ///////////////////////////
    // input gain settings
    ///////////////////////////
//org    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_0,    WM8904_LIN_VOL(0x5) ); // 00101 = +0.0 dB (default)
//org    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_0,   WM8904_RIN_VOL(0x5) ); // 00101 = +0.0 dB (default)
    // to reduce the hissing noise at A/D process
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_0,    WM8904_LIN_VOL(0x0) ); // 00000 = -1.5 dB
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_0,   WM8904_RIN_VOL(0x0) ); // 00000 = -1.5 dB

    // to reduce hissing noise on input side
    //  Gain[dB]=(VOL_CODE - 0xC0) * 0.375
    //  Gain[dB] / 0.375 + 0xC0(192) = VOL_CODE
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_LEFT,  WM8904_ADC_VU | WM8904_ADCL_VOL(0xA5) );     // 0xA5 = -10.125 dB
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_RIGHT, WM8904_ADC_VU | WM8904_ADCR_VOL(0xA5) );     // 0xA5 = -10.125 dB
//    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_LEFT,  WM8904_ADC_VU | WM8904_ADCL_VOL(0xC0) );     // 0xC0 = 0 dB
//    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_RIGHT, WM8904_ADC_VU | WM8904_ADCR_VOL(0xC0) );     // 0xC0 = 0 dB


    ///////////////////////////
    // output gain settings
    ///////////////////////////
    // -6dB to reduce the hissing noise on output side.
//    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT1_LEFT,       WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(57-6) ); // -6dB
//    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT1_RIGHT,      WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(57-6) ); // -6dB
    // Startup default is analog MUTE. Keep muted until the application explicitly unmutes.
    wm8904_write_hpout_level_mute(inst, true);
    //  Gain[dB]=(VOL_CODE - 0xC0) * 0.375
    //  Gain[dB] / 0.375 + 0xC0(192) = VOL_CODE
    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_VOLUME_LEFT,  WM8904_DAC_VU | WM8904_DACL_VOL(0xC0) ); // 0dB
    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_VOLUME_RIGHT, WM8904_DAC_VU | WM8904_DACR_VOL(0xC0) ); // 0dB


    delay_ms(20);


    wm8904_write_dac_digital_mute( inst, false, false );  // digital unmute
}


static void wm8904_write_dac_digital_mute(uint8_t inst, bool mute, bool ena96k)
{
    uint16_t data = 0;

    data = WM8904_DAC_MUTERATE    |
           WM8904_DAC_UNMUTE_RAMP |
           WM8904_DEEMPH(0);
    if( !ena96k )
    {
        data |= WM8904_DAC_OSR128;   // 96K config must be disabled.
    }

    if( mute )
    {
        data |= WM8904_DAC_MUTE;
    }

    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_1, data );
}


static void wm8904_write_hpout_level_mute(uint8_t inst, bool mute)
{
    uint16_t data_l = 0;
    uint16_t data_r = 0;

    data_l = WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(WM8904_HPOUT_VOL_DEFAULT);
    data_r = WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(WM8904_HPOUT_VOL_DEFAULT);

    if( mute )
    {
        data_l |= WM8904_HPOUTL_MUTE;
        data_r |= WM8904_HPOUTR_MUTE;
    }

    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT1_LEFT,  data_l );
    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT1_RIGHT, data_r );
}


static bool wm8904_wait_dc_servo_startup_done(uint8_t inst, uint16_t mask, uint32_t timeout_ms)
{
    uint16_t data  = 0;
    /* Elapsed-time stamp read only by the TRACE below, hence unused when the trace is
     * compiled out (ENA_WM8904_TRACE=0). Said with the attribute rather than left for
     * -Wall to report, because a warning that is expected trains people to ignore warnings. */
    uint32_t start __attribute__((unused)) = GetTicks();

    while( timeout_ms-- )
    {
        data = wm8904_read_reg(inst, WM8904_DC_SERVO_READBACK_0);

        if( (data & mask) == mask )
        {
            TRACE(" WM8904 DC servo startup done inst=%d R4D=0x%04x %ld(ms)\n", inst, data, GetTicks()-start);
            return true;
        }

        delay_ms(1);
    }

    TRACE(" WM8904 DC servo startup timeout!! inst=%d R4D=0x%04x\n", inst, data);

    return false;
}


//backup static bool wm8904_integrated_startup_sequence(uint8_t inst)
//backup {
//backup #define WM8904_WSEQ_STARTUP_INDEX       (0u)
//backup #define WM8904_WSEQ_STARTUP_TIMEOUT_MS  (500u)
//backup 
//backup     bool result = false;
//backup 
//backup     TRACE(" WM8904 default startup sequence start inst=%d @%ld\n",
//backup           inst,
//backup           GetTicks());
//backup 
//backup     /*
//backup      * Default Start-Up sequence:
//backup      * - Requires MCLK/SYSCLK to be available.
//backup      * - Intended for DAC playback via headphone/line output.
//backup      * - Datasheet default assumes 12.288 MHz MCLK and configures 48 kHz playback.
//backup      * - Runs DC servo sequence for pop/click reduction.
//backup      */
//backup 
//backup     // Enable the Control Write Sequencer.
//backup     wm8904_write_reg( inst,
//backup                       WM8904_WRITE_SEQUENCER_0,
//backup                       WM8904_WSEQ_ENA );
//backup 
//backup     // Start default startup sequence from index 0.
//backup     wm8904_write_reg( inst,
//backup                       WM8904_WRITE_SEQUENCER_3,
//backup                       WM8904_WSEQ_START |
//backup                       WM8904_WSEQ_START_INDEX(WM8904_WSEQ_STARTUP_INDEX) );
//backup 
//backup     result = wm8904_wait_write_sequencer_done(inst,
//backup                                               WM8904_WSEQ_STARTUP_TIMEOUT_MS);
//backup 
//backup     // Datasheet quick start-up sequence unmutes DAC after WSEQ completion.
//backup     // For your current analog-mute policy, I would NOT unmute here automatically.
//backup     // Keep analog mute and let the application call wm8904_set_analog_output_mute().
//backup     //
//backup     // wm8904_write_reg( inst, WM8904_DAC_DIGITAL_1, 0x0000 );
//backup 
//backup     TRACE(" WM8904 default startup sequence %s inst=%d @%ld\n",
//backup           (result) ? "done" : "timeout",
//backup           inst,
//backup           GetTicks());
//backup 
//backup     return result;
//backup }

//backup static bool wm8904_integrated_shutdown_sequence(uint8_t inst)
//backup {
//backup #define WM8904_WSEQ_SHUTDOWN_INDEX      (25u)
//backup #define WM8904_WSEQ_SHUTDOWN_TIMEOUT_MS (500u)
//backup 
//backup     bool result = false;
//backup 
//backup     TRACE(" WM8904 pre-reset shutdown sequence start inst=%d @%ld\n", inst, GetTicks());
//backup 
//backup     /*
//backup      * CPU reset does not reset WM8904.
//backup      * The codec may still be alive from the previous run.
//backup      *
//backup      * Do not directly touch OUT1 volume/mute registers here.
//backup      * Use the WM8904 default shutdown sequence instead, so the codec can
//backup      * shut down HP/charge pump/DC servo blocks in its intended order.
//backup      *
//backup      * Note:
//backup      * The write sequencer requires WM8904 SYSCLK/MCLK to be available.
//backup      * If it times out, continue to the normal software reset/config flow.
//backup      */
//backup 
//backup     // Enable the Control Write Sequencer clock, then start the default shutdown sequence.
//backup     wm8904_write_reg( inst, WM8904_WRITE_SEQUENCER_0, WM8904_WSEQ_ENA );
//backup 
//backup     wm8904_write_reg( inst,
//backup                       WM8904_WRITE_SEQUENCER_3,
//backup                       WM8904_WSEQ_START | WM8904_WSEQ_START_INDEX(WM8904_WSEQ_SHUTDOWN_INDEX) );
//backup 
//backup     result = wm8904_wait_write_sequencer_done(inst, WM8904_WSEQ_SHUTDOWN_TIMEOUT_MS);
//backup 
//backup     TRACE(" WM8904 pre-reset shutdown sequence %s inst=%d @%ld\n",
//backup           (result) ? "done" : "timeout",
//backup           inst,
//backup           GetTicks());
//backup 
//backup     return result;
//backup }


static bool wm8904_wait_write_sequencer_done(uint8_t inst, uint32_t timeout_ms)
{
    uint16_t data  = 0;
    uint32_t start __attribute__((unused)) = GetTicks();   /* TRACE-only, as above */

    while( timeout_ms-- )
    {
        data = wm8904_read_reg(inst, WM8904_WRITE_SEQUENCER_4);

        // A failed I2C read returns RET_INVALID (0xCECE) and clears s_io_ok. Its bit0 is 0, so
        // the WSEQ_BUSY test below would otherwise misread an unreachable codec as "sequencer
        // idle / done" -- a bogus success. Treat a failed read as a sequencer failure so the
        // caller (wm8904_shutdown) falls back to the quench discharge instead of trusting it.
        // Bound the s_io_ok[] read the same way as every write below; an out-of-range inst
        // is treated as not-ok (safe fallback) rather than indexing past the array.
        const bool io_ok = ( inst < WM8904_INST_MAX ) ? s_io_ok[inst] : false;
        if( ( !io_ok ) || ( data == RET_INVALID ) )
        {
            TRACE(" WM8904 WSEQ wait: I2C read failed inst=%d data=0x%04x\n", inst, data);
            return false;
        }

        if( (data & WM8904_WSEQ_BUSY) == 0u )
        {
            TRACE(" WM8904 write sequencer done inst=%d R70=0x%04x %ld(ms)\n",
                  inst,
                  data,
                  GetTicks()-start);
            return true;
        }

        delay_ms(1);
    }

    TRACE(" WM8904 write sequencer timeout!! inst=%d R70=0x%04x\n", inst, data);

    return false;
}


static void wm8904_hpout_quench_before_startup(uint8_t inst)
{
    TRACE(" WM8904 HPOUT quench before startup inst=%d @%ld\n",
          inst,
          GetTicks());

    wm8904_write_reg(inst,
                     WM8904_ANALOGUE_HP_0,
                     WM8904_HPL_RMV_SHORT |
                     WM8904_HPR_RMV_SHORT);   // 0x0088

    delay_ms(100);

    wm8904_write_reg(inst, WM8904_ANALOGUE_HP_0, 0x0000);

    delay_ms(100);

    TRACE(" WM8904 HPOUT quench before startup done inst=%d @%ld\n",
          inst,
          GetTicks());
}


//===========================================================
// Declick research helpers (one-shot)
//===========================================================

// C: datasheet Table 42-ordered headphone disable. Step 1 clears RMV_SHORT (re-shorts the outputs to
// ground) while the output stages stay enabled; Step 2 clears all ENA/ENA_DLY/ENA_OUTP bits. This is the
// vendor pop-suppressed disable, versus the baseline quench which sets RMV_SHORT=1 first. VMID and the
// charge pump are intentionally left up here (same as the quench) -- the following config re-uses them.
static void wm8904_hpout_ordered_disable(uint8_t inst)
{
    TRACE(" WM8904 ordered HP disable (Table42) inst=%d @%ld\n", inst, GetTicks());

    // Step 1: RMV_SHORT = 0, keep ENA / ENA_DLY / ENA_OUTP set (assumes a running/enabled output).
    wm8904_write_reg(inst, WM8904_ANALOGUE_HP_0,
                     WM8904_HPL_ENA_OUTP | WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                     WM8904_HPR_ENA_OUTP | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA);   // 0x0077
    delay_ms(1);

    // Step 2: all ENA bits off.
    wm8904_write_reg(inst, WM8904_ANALOGUE_HP_0, 0x0000);
    delay_ms(1);
}

// B (capture): store the measured DC-servo offsets (R73..R76 readback = current offset) so a later
// WARM_SERVO restart can reload them with DCS_TRIG_DAC_WR. Called after a completed STARTUP servo.
static void wm8904_capture_dc_servo(uint8_t inst)
{
    if( inst >= WM8904_INST_MAX ) { return; }

    s_dcs_val[inst][0] = (uint8_t)( wm8904_read_reg(inst, WM8904_DC_SERVO_6) & 0x00FFu );  // LINEOUTR
    s_dcs_val[inst][1] = (uint8_t)( wm8904_read_reg(inst, WM8904_DC_SERVO_7) & 0x00FFu );  // LINEOUTL
    s_dcs_val[inst][2] = (uint8_t)( wm8904_read_reg(inst, WM8904_DC_SERVO_8) & 0x00FFu );  // HPOUTR
    s_dcs_val[inst][3] = (uint8_t)( wm8904_read_reg(inst, WM8904_DC_SERVO_9) & 0x00FFu );  // HPOUTL
    s_dcs_valid[inst]  = true;

    TRACE(" WM8904 DC servo captured inst=%d [%02x %02x %02x %02x]\n",
          inst, s_dcs_val[inst][0], s_dcs_val[inst][1], s_dcs_val[inst][2], s_dcs_val[inst][3]);
}

// B (restore): reload the captured offsets and trigger the fast DAC-write servo mode (~2ms/ch) instead
// of a full STARTUP measurement (~86ms/ch). Returns false if no capture is available (caller then runs
// the STARTUP path); true once applied, even if the completion poll times out (still on the warm path).
static bool wm8904_apply_dc_servo_warm(uint8_t inst)
{
    if( inst >= WM8904_INST_MAX || !s_dcs_valid[inst] ) { return false; }

    wm8904_write_reg(inst, WM8904_DC_SERVO_6, s_dcs_val[inst][0]);
    wm8904_write_reg(inst, WM8904_DC_SERVO_7, s_dcs_val[inst][1]);
    wm8904_write_reg(inst, WM8904_DC_SERVO_8, s_dcs_val[inst][2]);
    wm8904_write_reg(inst, WM8904_DC_SERVO_9, s_dcs_val[inst][3]);

    wm8904_write_reg(inst, WM8904_DC_SERVO_1,
                     WM8904_DCS_TRIG_DAC_WR_3 | WM8904_DCS_TRIG_DAC_WR_2 |
                     WM8904_DCS_TRIG_DAC_WR_1 | WM8904_DCS_TRIG_DAC_WR_0);

    // DAC-write completion is reported in R77[7:4] (DCS_DAC_WR_COMPLETE); reuse the readback waiter.
    /* The CALL is load-bearing -- it is the wait. Only its result is TRACE-only, so the
     * attribute goes on the variable and the call stays unconditional. */
    const bool done __attribute__((unused)) =
        wm8904_wait_dc_servo_startup_done(inst, WM8904_DCS_DAC_WR_COMPLETE_Msk, 100u);
    TRACE(" WM8904 DC servo WARM restore inst=%d done=%d\n", inst, (int)done);
    return true;
}

// D: stepped (soft) HPOUT analog unmute. Clears the mute by climbing the OUT1 volume from a lower level
// to the default in small steps, so the mute release is a short ramp instead of an instantaneous jump.
// HPOUT_VU commits the L/R volume update. The DAC digital path is already soft-unmuted (DAC_UNMUTE_RAMP).
static void wm8904_hpout_ramp_unmute(uint8_t inst)
{
    const uint8_t target = (uint8_t)WM8904_HPOUT_VOL_DEFAULT;                 // -6 dB (51)
    const uint8_t start  = ( target > 24u ) ? (uint8_t)( target - 24u ) : 0u; // ~ -15 dB below target
    const uint8_t step   = 3u;

    for( uint8_t v = start; v < target; v = (uint8_t)( v + step ) )
    {
        wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_LEFT,  WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(v));
        wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_RIGHT, WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(v));
        delay_ms(6);
    }
    wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_LEFT,  WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(target));
    wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_RIGHT, WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(target));
}

// E: stepped HPOUT gain ramp-DOWN before the hard mute/shutdown. Lowers OUT1 volume from the default
// operating level toward minimum in small steps, then latches the HPOUT mute bit, so the output is
// already near-silent when the teardown/discharge follows. Mirror of wm8904_hpout_ramp_unmute().
static void wm8904_hpout_ramp_mute(uint8_t inst)
{
    const uint8_t target = (uint8_t)WM8904_HPOUT_VOL_DEFAULT;                 // -6 dB (51) operating level
    const uint8_t floor   = ( target > 24u ) ? (uint8_t)( target - 24u ) : 0u; // ~ -15 dB below target
    const uint8_t step    = 3u;

    for( uint8_t v = target; v > floor; v = ( v > step ) ? (uint8_t)( v - step ) : 0u )
    {
        wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_LEFT,  WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(v));
        wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_RIGHT, WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(v));
        delay_ms(6);
    }
    // Final hard mute at the floor level (sets HPOUTx_MUTE via the shared writer).
    wm8904_write_hpout_level_mute(inst, true);
}

// A: run the WM8904 vendor Control Write Sequencer default shutdown (index 0x19, Table 89) -- the ordered
// RMV_SHORT -> ENA -> DCS -> CP -> DAC -> CLK -> PGA -> BIAS -> VMID power-down with datasheet timing.
// Requires SYSCLK to be present. Returns false on sequencer timeout so the caller can fall back to quench.
// NOTE: only the WSEQ *shutdown* is wired; startup stays on the manual Table 41 sequence (which already
// matches the vendor enable order and, unlike the fixed-48k WSEQ startup, honours the TDM/rate/ADC config).
static bool wm8904_wseq_shutdown(uint8_t inst)
{
    TRACE(" WM8904 WSEQ shutdown start inst=%d @%ld\n", inst, GetTicks());

    // The vendor shutdown sequence steps VMID/CP/CLK/PGA/BIAS power-down under SYSCLK. If SYSCLK is
    // sourced from the FLL (44.1k family) the sequence disturbs/powers down the FLL and kills its own
    // clock mid-run -> WSEQ_BUSY never clears -> 600 ms timeout, and the analog HP block is left wedged
    // (subsequent ANALOGUE_HP_0 writes then fail their read-back verify). Coming from 48k the source is
    // MCLK, which is always present on the codec-master XTAL, so the sequence completes (~290 ms) -- that
    // is the observed 44.1k->48k-only failure. Point SYSCLK at MCLK before starting the sequencer so it
    // has a stable clock for the whole power-down regardless of the current rate. Datasheet p.100: set
    // CLK_SYS_ENA=0 before changing SYSCLK_SRC. R22 (CLOCK_RATES_2) is always accessible; no readback
    // needed. wm8904_config reprograms the clock source fully on the following (re)configure.
    wm8904_write_reg(inst, WM8904_CLOCK_RATES_2, 0x0000);                              // CLK_SYS_ENA=0 (allow source switch)
    wm8904_write_reg(inst, WM8904_CLOCK_RATES_2,
                     WM8904_CLK_SYS_ENA | WM8904_CLK_DSP_ENA);                         // SYSCLK_SRC=0 (MCLK), re-enable

    wm8904_write_reg(inst, WM8904_WRITE_SEQUENCER_0, WM8904_WSEQ_ENA);                 // enable sequencer clock
    wm8904_write_reg(inst, WM8904_WRITE_SEQUENCER_3,
                     WM8904_WSEQ_START | WM8904_WSEQ_START_INDEX(0x19u));              // start at shutdown index
    delay_ms(1);                                                                       // let WSEQ_BUSY assert
    return wm8904_wait_write_sequencer_done(inst, 600u);
}

// F: run only the HP/LINEOUT output-enable portion of the vendor startup sequence (Table 88 indices
// 12..22): HP_ENA -> ENA_DLY -> DCS_ENA -> DCS_TRIG_STARTUP -> ENA_OUTP -> RMV_SHORT, with the datasheet
// inter-step timing and the ~256ms DC-servo wait. The caller has already brought up clocks/interface/
// BIAS/VMID/CP/PGA manually (so TDM/ADC config is preserved); this only replaces the pop-critical output
// bring-up. Needs SYSCLK. Returns false on sequencer timeout so the caller can fall back to the manual seq.
static bool wm8904_wseq_hp_enable(uint8_t inst)
{
    TRACE(" WM8904 WSEQ HP-enable (idx12) start inst=%d @%ld\n", inst, GetTicks());
    wm8904_write_reg(inst, WM8904_WRITE_SEQUENCER_0, WM8904_WSEQ_ENA);
    wm8904_write_reg(inst, WM8904_WRITE_SEQUENCER_3,
                     WM8904_WSEQ_START | WM8904_WSEQ_START_INDEX(0x0Cu));   // start at HP_ENA (index 12)
    delay_ms(1);
    return wm8904_wait_write_sequencer_done(inst, 600u);
}
