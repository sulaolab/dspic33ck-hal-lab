/*
 * nora_gpio_event_dspic33ck.c
 * ----------------------
 * Thin dsPIC33CK Change Notification (CN) event dispatcher for GPIO pins.
 * CK sibling of dspic33ak_gpio_event.c -- same structure and logic; the only
 * differences are silicon-forced: CK CN registers are 16-bit, and the per-port
 * CN interrupt flags live in IFS0 (A/B), IFS1 (C) and IFS4 (D/E) rather than
 * AK's IFS3/IFS9.
 *
 * The app/example owns the actual CN interrupt vector and forwards it here.
 * This keeps vector ownership out of the GPIO core while allowing small
 * validation apps to attach callbacks to selected pins.
 */

#include "nora_gpio_event.h"

#include <stddef.h>
#include <xc.h>


//===========================================================
// Definition
//===========================================================

#define NORA_GPIO_EVENT_PORT_COUNT    8u
#define NORA_GPIO_EVENT_PIN_COUNT     16u
#define NORA_GPIO_EVENT_CNCON_CNSTYLE 0x0800u
#define NORA_GPIO_EVENT_CNCON_ON      0x8000u

typedef struct
{
    volatile uint16_t *port;
    volatile uint16_t *cncon;
    volatile uint16_t *cnen0;
    volatile uint16_t *cnen1;
    volatile uint16_t *cnf;
    /*
     * The CN interrupt flag is deliberately NOT here.
     *
     * It used to be, as `{ ..., &IFS0, _IFS0_CNAIF_MASK }`, and a pointer plus a runtime
     * mask cannot be written one bit at a time: the compiler has to load the whole word,
     * mask, and store it back. IFSx and IECx are shared by every peripheral on the part --
     * on EV88G73A IFS0 alone carries the 1 ms tick, both audio DMA legs, the load-monitor
     * time base, SPI1 RX/TX and the console UART -- so that store puts back whatever
     * those bits were at the load, erasing anything hardware or another context set in
     * between.
     *
     * The flag is read and cleared through the DFP bit aliases (`_CNAIF`) in
     * nora_gpio_event_irq_clear_flag() / _irq_flag_is_set() instead, which is one
     * bclr.b. That also drops the per-part bank knowledge this table used to carry
     * (A/B in IFS0, C in IFS1, D/E in IFS4): the alias names the bit wherever it lives.
     */
} nora_gpio_event_regs_t;

typedef struct
{
    nora_gpio_pin_t            pin;
    nora_gpio_event_edge_t     trigger;
    nora_gpio_event_callback_t callback;
    void                           *user_data;
    bool                            previous_high;
} nora_gpio_event_slot_t;

#define NORA_GPIO_EVENT_PORT_ROW(L) \
    { &PORT##L, &CNCON##L, &CNEN0##L, &CNEN1##L, &CNF##L }
#define NORA_GPIO_EVENT_PORT_NONE \
    { 0, 0, 0, 0, 0 }


//===========================================================
// Function Prototype
//===========================================================

static const nora_gpio_event_regs_t *nora_gpio_event_regs_for(nora_gpio_pin_t pin);
static unsigned                            nora_gpio_event_port_index(nora_gpio_pin_t pin);
static uint8_t                            nora_gpio_event_bit_index(nora_gpio_pin_t pin);
static uint16_t                           nora_gpio_event_mask(nora_gpio_pin_t pin);
static bool                               nora_gpio_event_trigger_valid(nora_gpio_event_edge_t trigger);
static bool                               nora_gpio_event_trigger_matches(nora_gpio_event_edge_t trigger,
                                                                              nora_gpio_event_edge_t actual_edge);
static bool                               nora_gpio_event_irq_set_priority(unsigned port_index, uint8_t priority);
static bool                               nora_gpio_event_irq_clear_flag(unsigned port_index);
static bool                               nora_gpio_event_irq_flag_is_set(unsigned port_index);
static bool                               nora_gpio_event_irq_get_enable(unsigned port_index, bool *enabled);
static bool                               nora_gpio_event_irq_set_enable(unsigned port_index, bool enable);


//===========================================================
// Variables
//===========================================================

/*
 * A port qualifies when its CN registers exist AND the DFP names its interrupt bits. The
 * probe is written out per port rather than built by a macro: `defined` produced by macro
 * expansion is undefined behaviour, and getting a silent "port absent" out of it would
 * disable change notification on a part that has it.
 *
 * A port whose CN registers are present but whose interrupt bits the DFP does not name is
 * an error, not a port to skip -- attaching to it would arm a notification nothing can
 * clear, and the ISR would re-enter forever.
 */
#if defined(PORTA) && defined(CNCONA) && !(defined(_CNAIF) && defined(_CNAIE))
#error "PORTA change notification is present but the DFP names no _CNAIF/_CNAIE bit alias"
#endif
#if defined(PORTB) && defined(CNCONB) && !(defined(_CNBIF) && defined(_CNBIE))
#error "PORTB change notification is present but the DFP names no _CNBIF/_CNBIE bit alias"
#endif
#if defined(PORTC) && defined(CNCONC) && !(defined(_CNCIF) && defined(_CNCIE))
#error "PORTC change notification is present but the DFP names no _CNCIF/_CNCIE bit alias"
#endif
#if defined(PORTD) && defined(CNCOND) && !(defined(_CNDIF) && defined(_CNDIE))
#error "PORTD change notification is present but the DFP names no _CNDIF/_CNDIE bit alias"
#endif
#if defined(PORTE) && defined(CNCONE) && !(defined(_CNEIF) && defined(_CNEIE))
#error "PORTE change notification is present but the DFP names no _CNEIF/_CNEIE bit alias"
#endif

static const nora_gpio_event_regs_t s_event_regs[NORA_GPIO_EVENT_PORT_COUNT] =
{
#if defined(PORTA) && defined(CNCONA) && defined(CNEN0A) && defined(CNEN1A) && defined(CNFA) && defined(_CNAIF)
    NORA_GPIO_EVENT_PORT_ROW(A),                             /* [0] PORTA */
#else
    NORA_GPIO_EVENT_PORT_NONE,
#endif
#if defined(PORTB) && defined(CNCONB) && defined(CNEN0B) && defined(CNEN1B) && defined(CNFB) && defined(_CNBIF)
    NORA_GPIO_EVENT_PORT_ROW(B),                             /* [1] PORTB */
#else
    NORA_GPIO_EVENT_PORT_NONE,
#endif
#if defined(PORTC) && defined(CNCONC) && defined(CNEN0C) && defined(CNEN1C) && defined(CNFC) && defined(_CNCIF)
    NORA_GPIO_EVENT_PORT_ROW(C),                             /* [2] PORTC */
#else
    NORA_GPIO_EVENT_PORT_NONE,
#endif
#if defined(PORTD) && defined(CNCOND) && defined(CNEN0D) && defined(CNEN1D) && defined(CNFD) && defined(_CNDIF)
    NORA_GPIO_EVENT_PORT_ROW(D),                             /* [3] PORTD */
#else
    NORA_GPIO_EVENT_PORT_NONE,
#endif
#if defined(PORTE) && defined(CNCONE) && defined(CNEN0E) && defined(CNEN1E) && defined(CNFE) && defined(_CNEIF)
    NORA_GPIO_EVENT_PORT_ROW(E),                             /* [4] PORTE */
#else
    NORA_GPIO_EVENT_PORT_NONE,
#endif
    NORA_GPIO_EVENT_PORT_NONE,                               /* [5] PORTF (absent) */
    NORA_GPIO_EVENT_PORT_NONE,                               /* [6] PORTG (absent) */
    NORA_GPIO_EVENT_PORT_NONE,                               /* [7] PORTH (absent) */
};

static nora_gpio_event_slot_t s_event_slots[NORA_GPIO_EVENT_PORT_COUNT][NORA_GPIO_EVENT_PIN_COUNT];
static volatile uint16_t s_event_owned_masks[NORA_GPIO_EVENT_PORT_COUNT];


//===========================================================
// Global Function
//===========================================================

bool nora_gpio_event_attach(nora_gpio_pin_t pin,
                                 nora_gpio_event_edge_t trigger,
                                 nora_gpio_event_callback_t callback,
                                 void *user_data)
{
    const nora_gpio_event_regs_t *regs = nora_gpio_event_regs_for(pin);
    unsigned port_index = nora_gpio_event_port_index(pin);
    uint8_t bit_index = nora_gpio_event_bit_index(pin);
    uint16_t mask = nora_gpio_event_mask(pin);

    if ((regs == 0) || (callback == 0) || !nora_gpio_event_trigger_valid(trigger))
    {
        return false;
    }

    *regs->cnen0 &= (uint16_t)~mask;
    *regs->cnen1 &= (uint16_t)~mask;
    *regs->cnf   &= (uint16_t)~mask;
    (void)nora_gpio_event_irq_clear_flag(port_index);

    s_event_slots[port_index][bit_index].pin       = pin;
    s_event_slots[port_index][bit_index].trigger   = trigger;
    s_event_slots[port_index][bit_index].callback  = callback;
    s_event_slots[port_index][bit_index].user_data = user_data;
    s_event_slots[port_index][bit_index].previous_high = ((*regs->port & mask) != 0u);
    s_event_owned_masks[port_index] |= mask;

    *regs->cncon |= (NORA_GPIO_EVENT_CNCON_CNSTYLE | NORA_GPIO_EVENT_CNCON_ON);

    *regs->cnen0 |= mask;
    *regs->cnen1 |= mask;

    return true;
}

bool nora_gpio_event_detach(nora_gpio_pin_t pin)
{
    const nora_gpio_event_regs_t *regs = nora_gpio_event_regs_for(pin);
    unsigned port_index = nora_gpio_event_port_index(pin);
    uint8_t bit_index = nora_gpio_event_bit_index(pin);
    uint16_t mask = nora_gpio_event_mask(pin);

    if (regs == 0)
    {
        return false;
    }

    *regs->cnen0 &= (uint16_t)~mask;
    *regs->cnen1 &= (uint16_t)~mask;
    *regs->cnf   &= (uint16_t)~mask;
    (void)nora_gpio_event_irq_clear_flag(port_index);

    s_event_slots[port_index][bit_index].pin       = 0u;
    s_event_slots[port_index][bit_index].trigger   = NORA_GPIO_EVENT_EDGE_NONE;
    s_event_slots[port_index][bit_index].callback  = 0;
    s_event_slots[port_index][bit_index].user_data = 0;
    s_event_slots[port_index][bit_index].previous_high = false;
    s_event_owned_masks[port_index] &= (uint16_t)~mask;

    return true;
}

bool nora_gpio_event_irq_enable(nora_gpio_pin_t pin, uint8_t priority)
{
    const nora_gpio_event_regs_t *regs = nora_gpio_event_regs_for(pin);
    unsigned port_index = nora_gpio_event_port_index(pin);

    if ((regs == 0) || (priority > 7u))
    {
        return false;
    }
    if (!nora_gpio_event_irq_set_priority(port_index, priority))
    {
        return false;
    }
    if (!nora_gpio_event_irq_clear_flag(port_index))
    {
        return false;
    }
    return nora_gpio_event_irq_set_enable(port_index, true);
}

bool nora_gpio_event_irq_disable(nora_gpio_pin_t pin)
{
    const nora_gpio_event_regs_t *regs = nora_gpio_event_regs_for(pin);
    unsigned port_index = nora_gpio_event_port_index(pin);

    if (regs == 0)
    {
        return false;
    }
    if (!nora_gpio_event_irq_set_enable(port_index, false))
    {
        return false;
    }
    return nora_gpio_event_irq_clear_flag(port_index);
}

bool nora_gpio_event_irq_is_enabled(nora_gpio_pin_t pin, bool *enabled)
{
    const nora_gpio_event_regs_t *regs = nora_gpio_event_regs_for(pin);
    unsigned port_index = nora_gpio_event_port_index(pin);

    if ((regs == 0) || (enabled == 0))
    {
        return false;
    }
    return nora_gpio_event_irq_get_enable(port_index, enabled);
}

bool nora_gpio_event_irq_set_enabled(nora_gpio_pin_t pin, bool enable)
{
    const nora_gpio_event_regs_t *regs = nora_gpio_event_regs_for(pin);
    unsigned port_index = nora_gpio_event_port_index(pin);

    if (regs == 0)
    {
        return false;
    }
    return nora_gpio_event_irq_set_enable(port_index, enable);
}

bool nora_gpio_event_rp_attach(nora_gpio_rp_t rp,
                                    nora_gpio_event_edge_t trigger,
                                    nora_gpio_event_callback_t callback,
                                    void *user_data)
{
    nora_gpio_pin_t pin;

    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_event_attach(pin, trigger, callback, user_data);
}

bool nora_gpio_event_rp_detach(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;

    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_event_detach(pin);
}

bool nora_gpio_event_rp_irq_enable(nora_gpio_rp_t rp, uint8_t priority)
{
    nora_gpio_pin_t pin;

    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_event_irq_enable(pin, priority);
}

bool nora_gpio_event_rp_irq_disable(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;

    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_event_irq_disable(pin);
}

bool nora_gpio_event_rp_irq_is_enabled(nora_gpio_rp_t rp, bool *enabled)
{
    nora_gpio_pin_t pin;

    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_event_irq_is_enabled(pin, enabled);
}

bool nora_gpio_event_rp_irq_set_enabled(nora_gpio_rp_t rp, bool enable)
{
    nora_gpio_pin_t pin;

    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_event_irq_set_enabled(pin, enable);
}

void nora_gpio_event_process_isr(void)
{
    uint8_t port_index;

    for (port_index = 0u; port_index < NORA_GPIO_EVENT_PORT_COUNT; port_index++)
    {
        const nora_gpio_event_regs_t *regs = &s_event_regs[port_index];
        uint16_t pending_mask;
        uint16_t port_level;
        uint8_t bit_index;

        if (regs->port == 0)
        {
            continue;
        }

        pending_mask = (uint16_t)(*regs->cnf & s_event_owned_masks[port_index]);
        if (pending_mask == 0u)
        {
            if ((s_event_owned_masks[port_index] != 0u) &&
                nora_gpio_event_irq_flag_is_set(port_index))
            {
                (void)nora_gpio_event_irq_clear_flag(port_index);
            }
            continue;
        }

        port_level = *regs->port;

        for (bit_index = 0u; bit_index < NORA_GPIO_EVENT_PIN_COUNT; bit_index++)
        {
            uint16_t bit_mask = (uint16_t)((uint16_t)1u << bit_index);
            nora_gpio_event_slot_t *slot;
            nora_gpio_event_edge_t actual_edge;
            bool current_high;

            if ((pending_mask & bit_mask) == 0u)
            {
                continue;
            }

            slot = &s_event_slots[port_index][bit_index];
            current_high = ((port_level & bit_mask) != 0u);
            if (current_high == slot->previous_high)
            {
                continue;
            }

            actual_edge = current_high ?
                          NORA_GPIO_EVENT_EDGE_RISING :
                          NORA_GPIO_EVENT_EDGE_FALLING;
            slot->previous_high = current_high;

            if ((slot->callback != 0) &&
                nora_gpio_event_trigger_matches(slot->trigger, actual_edge))
            {
                slot->callback(slot->pin, actual_edge, slot->user_data);
            }
        }

        *regs->cnf &= (uint16_t)~pending_mask;
        (void)nora_gpio_event_irq_clear_flag(port_index);
    }
}


//===========================================================
// Local Function
//===========================================================

static const nora_gpio_event_regs_t *nora_gpio_event_regs_for(nora_gpio_pin_t pin)
{
    unsigned port_index = nora_gpio_event_port_index(pin);

    if (port_index >= NORA_GPIO_EVENT_PORT_COUNT)
    {
        return 0;
    }
    if (s_event_regs[port_index].port == 0)
    {
        return 0;
    }
    return &s_event_regs[port_index];
}

static unsigned nora_gpio_event_port_index(nora_gpio_pin_t pin)
{
    return (unsigned)(pin >> 4);
}

static uint8_t nora_gpio_event_bit_index(nora_gpio_pin_t pin)
{
    return (uint8_t)(pin & 0x0Fu);
}

static uint16_t nora_gpio_event_mask(nora_gpio_pin_t pin)
{
    return (uint16_t)((uint16_t)1u << nora_gpio_event_bit_index(pin));
}

static bool nora_gpio_event_trigger_valid(nora_gpio_event_edge_t trigger)
{
    return (trigger == NORA_GPIO_EVENT_EDGE_EITHER);
}

static bool nora_gpio_event_trigger_matches(nora_gpio_event_edge_t trigger,
                                                 nora_gpio_event_edge_t actual_edge)
{
    return ((trigger == NORA_GPIO_EVENT_EDGE_EITHER) ||
            (trigger == actual_edge));
}

static bool nora_gpio_event_irq_set_priority(unsigned port_index, uint8_t priority)
{
    switch (port_index)
    {
#if defined(_CNAIP)
    case NORA_GPIO_PORT_A: _CNAIP = priority; return true;
#endif
#if defined(_CNBIP)
    case NORA_GPIO_PORT_B: _CNBIP = priority; return true;
#endif
#if defined(_CNCIP)
    case NORA_GPIO_PORT_C: _CNCIP = priority; return true;
#endif
#if defined(_CNDIP)
    case NORA_GPIO_PORT_D: _CNDIP = priority; return true;
#endif
#if defined(_CNEIP)
    case NORA_GPIO_PORT_E: _CNEIP = priority; return true;
#endif
    default: return false;
    }
}

static bool nora_gpio_event_irq_clear_flag(unsigned port_index)
{
    switch (port_index)
    {
#if defined(_CNAIF)
    case NORA_GPIO_PORT_A: _CNAIF = 0u; return true;
#endif
#if defined(_CNBIF)
    case NORA_GPIO_PORT_B: _CNBIF = 0u; return true;
#endif
#if defined(_CNCIF)
    case NORA_GPIO_PORT_C: _CNCIF = 0u; return true;
#endif
#if defined(_CNDIF)
    case NORA_GPIO_PORT_D: _CNDIF = 0u; return true;
#endif
#if defined(_CNEIF)
    case NORA_GPIO_PORT_E: _CNEIF = 0u; return true;
#endif
    default: return false;
    }
}

/*
 * Reading the flag is never a hazard -- a read cannot put another peripheral's bit back --
 * but it goes through the alias anyway so the bank stays out of this file. An unknown port
 * reports "not pending", which is what the caller then does nothing about.
 */
static bool nora_gpio_event_irq_flag_is_set(unsigned port_index)
{
    switch (port_index)
    {
#if defined(_CNAIF)
    case NORA_GPIO_PORT_A: return (_CNAIF != 0u);
#endif
#if defined(_CNBIF)
    case NORA_GPIO_PORT_B: return (_CNBIF != 0u);
#endif
#if defined(_CNCIF)
    case NORA_GPIO_PORT_C: return (_CNCIF != 0u);
#endif
#if defined(_CNDIF)
    case NORA_GPIO_PORT_D: return (_CNDIF != 0u);
#endif
#if defined(_CNEIF)
    case NORA_GPIO_PORT_E: return (_CNEIF != 0u);
#endif
    default: return false;
    }
}

static bool nora_gpio_event_irq_get_enable(unsigned port_index, bool *enabled)
{
    switch (port_index)
    {
#if defined(_CNAIE)
    case NORA_GPIO_PORT_A: *enabled = (_CNAIE != 0u); return true;
#endif
#if defined(_CNBIE)
    case NORA_GPIO_PORT_B: *enabled = (_CNBIE != 0u); return true;
#endif
#if defined(_CNCIE)
    case NORA_GPIO_PORT_C: *enabled = (_CNCIE != 0u); return true;
#endif
#if defined(_CNDIE)
    case NORA_GPIO_PORT_D: *enabled = (_CNDIE != 0u); return true;
#endif
#if defined(_CNEIE)
    case NORA_GPIO_PORT_E: *enabled = (_CNEIE != 0u); return true;
#endif
    default: return false;
    }
}

/*
 * The if/else is load-bearing, not style: `_CNAIE = v` hands the compiler a runtime value,
 * and while dsPIC33C folds that into a single BFINS today, nothing in the C requires it --
 * the identical line is a whole-word read-modify-write on dsPIC33A, which has no BFINS.
 * Written this way both arms store a literal, so each is one bset.b / bclr.b whatever
 * the compiler decides.
 */
static bool nora_gpio_event_irq_set_enable(unsigned port_index, bool enable)
{
    if (enable)
    {
        switch (port_index)
        {
#if defined(_CNAIE)
        case NORA_GPIO_PORT_A: _CNAIE = 1u; return true;
#endif
#if defined(_CNBIE)
        case NORA_GPIO_PORT_B: _CNBIE = 1u; return true;
#endif
#if defined(_CNCIE)
        case NORA_GPIO_PORT_C: _CNCIE = 1u; return true;
#endif
#if defined(_CNDIE)
        case NORA_GPIO_PORT_D: _CNDIE = 1u; return true;
#endif
#if defined(_CNEIE)
        case NORA_GPIO_PORT_E: _CNEIE = 1u; return true;
#endif
        default: return false;
        }
    }

    switch (port_index)
    {
#if defined(_CNAIE)
    case NORA_GPIO_PORT_A: _CNAIE = 0u; return true;
#endif
#if defined(_CNBIE)
    case NORA_GPIO_PORT_B: _CNBIE = 0u; return true;
#endif
#if defined(_CNCIE)
    case NORA_GPIO_PORT_C: _CNCIE = 0u; return true;
#endif
#if defined(_CNDIE)
    case NORA_GPIO_PORT_D: _CNDIE = 0u; return true;
#endif
#if defined(_CNEIE)
    case NORA_GPIO_PORT_E: _CNEIE = 0u; return true;
#endif
    default: return false;
    }
}
