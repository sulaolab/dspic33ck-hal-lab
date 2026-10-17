/* Included by the DFP-derived host harness; never part of board firmware. */
#include <assert.h>
#include <stdio.h>

static unsigned callback_count;
static nora_gpio_event_edge_t callback_edge;
static void on_edge(nora_gpio_pin_t pin, nora_gpio_event_edge_t edge, void *data)
{
    assert(pin == NORA_GPIO_PIN(NORA_GPIO_PORT_F, 2u));
    assert(data == &callback_count);
    ++callback_count;
    callback_edge = edge;
}

int main(void)
{
    nora_gpio_pin_t pin = 0u;
    nora_gpio_rp_t rp = 0u;
    size_t i;
    assert(!nora_gpio_pin_from_rp(32u, NULL));
    assert(!nora_gpio_rp_from_pin(0u, NULL));
    assert(!nora_gpio_pin_from_rp(31u, &pin));
    assert(!nora_gpio_pin_from_rp(100u, &pin));
    assert(!nora_gpio_pin_from_rp(176u, &pin));
    assert(!nora_gpio_rp_from_pin(NORA_GPIO_PIN(NORA_GPIO_PORT_E, 0u), &rp));
    assert(!nora_gpio_rp_from_pin(NORA_GPIO_PIN(NORA_GPIO_PORT_A, 4u), &rp));
    for (i = 0; i < sizeof(rp_expected) / sizeof(rp_expected[0]); ++i) {
        const struct rp_expectation *e = &rp_expected[i];
        unsigned port = e->rp < 48u ? NORA_GPIO_PORT_B :
                        e->rp < 64u ? NORA_GPIO_PORT_C :
                        e->rp < 80u ? NORA_GPIO_PORT_D :
                        e->rp < 96u ? NORA_GPIO_PORT_F : NORA_GPIO_PORT_A;
        unsigned bit = e->rp < 80u ? e->rp % 16u :
                       e->rp < 96u ? e->rp - 80u : e->rp - 91u;
        assert(nora_gpio_pin_from_rp(e->rp, &pin));
        assert(pin == NORA_GPIO_PIN(port, bit));
        assert(nora_gpio_rp_from_pin(pin, &rp) && rp == e->rp);
        memset((void *)mock_rpor, 0, sizeof(mock_rpor));
        assert(nora_pps_route_output(NORA_PPS_OUTPUT_U1TX, e->rp));
        assert(mock_rpor[e->bank] == (uint16_t)(_RPOUT_U1TX << e->shift));
        assert(nora_pps_find_output_rp(NORA_PPS_OUTPUT_U1TX, &rp) && rp == e->rp);
        assert(mock_lock == 0x0800u);
        assert(nora_pps_route_input(NORA_PPS_INPUT_U1RX, e->rp) && _U1RXR == e->rp);
        assert(nora_pps_clear_output(e->rp));
        assert((mock_rpor[e->bank] & e->mask) == 0u);
    }
    assert(!nora_pps_route_input(NORA_PPS_INPUT_U1RX, 100u));
    assert(!nora_pps_route_input(NORA_PPS_INPUT_U1RX, 175u));
    assert(!nora_pps_route_input(NORA_PPS_INPUT_U1RX, 182u));
    memset((void *)mock_rpor, 0, sizeof(mock_rpor));
    assert(nora_pps_route_output(NORA_PPS_OUTPUT_U1TX, NORA_PPS_RPV0));
    assert(nora_pps_route_input(NORA_PPS_INPUT_U1RX, NORA_PPS_RPV0));
    assert(!nora_pps_find_output_rp(NORA_PPS_OUTPUT_U1TX, &rp));
#ifdef _RP99R
    {
        const nora_pps_output_t outputs[] = {NORA_PPS_OUTPUT_CLC5, NORA_PPS_OUTPUT_CLC6,
                                            NORA_PPS_OUTPUT_CLC7, NORA_PPS_OUTPUT_CLC8};
        const unsigned pins[] = {80u, 95u, 96u, 99u};
        for (i = 0; i < 4; ++i) {
            memset((void *)mock_rpor, 0, sizeof(mock_rpor));
            assert(nora_pps_route_output(outputs[i], pins[i]));
            assert(nora_pps_find_output_rp(outputs[i], &rp) && rp == pins[i]);
        }
        assert(nora_gpio_rp_config_digital_output(82u, true));
        assert((LATF & 4u) && !(TRISF & 4u) && !(ANSELF & 4u));
        assert(nora_gpio_rp_write(82u, false) && !(LATF & 4u));
        assert(nora_gpio_rp_set_pull(82u, NORA_GPIO_PULL_UP) && (CNPUF & 4u));
        assert(nora_gpio_rp_set_open_drain(82u, true) && (ODCF & 4u));
        PORTF = 4u;
        assert(nora_gpio_rp_read(82u) == NORA_GPIO_LEVEL_HIGH);
        PORTF = 0u;
        assert(nora_gpio_event_rp_attach(82u, NORA_GPIO_EVENT_EDGE_EITHER,
                                         on_edge, &callback_count));
        assert((CNEN0F & 4u) && (CNEN1F & 4u) && (CNCONF & 0x8800u) == 0x8800u);
        assert(!nora_gpio_event_rp_irq_enable(82u, 8u));
        assert(nora_gpio_event_rp_irq_enable(82u, 3u) && _CNFIE && _CNFIP == 3u);
        _CNFIF = 1u;
        _CNAIF = 1u;
        assert(nora_gpio_event_rp_irq_set_enabled(82u, false) && _CNFIF);
        assert(nora_gpio_event_rp_irq_set_enabled(82u, true));
        { bool enabled; assert(nora_gpio_event_rp_irq_is_enabled(82u, &enabled) && enabled); }
        PORTF = 4u;
        CNFF = 4u;
        nora_gpio_event_process_isr();
        assert(callback_count == 1u && callback_edge == NORA_GPIO_EVENT_EDGE_RISING);
        assert(!CNFF && !_CNFIF && _CNAIF);
        PORTF = 0u;
        CNFF = 4u;
        _CNFIF = 1u;
        nora_gpio_event_process_isr();
        assert(callback_count == 2u && callback_edge == NORA_GPIO_EVENT_EDGE_FALLING);
        _CNFIF = 1u;
        assert(nora_gpio_event_rp_irq_disable(82u) && !_CNFIE && !_CNFIF && _CNAIF);
        assert(nora_gpio_event_rp_detach(82u) && !(CNEN0F & 4u) && !(CNEN1F & 4u));
    }
#else
    (void)on_edge;
    for (i = 80u; i <= 99u; ++i) {
        assert(!nora_gpio_pin_from_rp((nora_gpio_rp_t)i, &pin));
        assert(!nora_pps_route_input(NORA_PPS_INPUT_U1RX, (nora_gpio_rp_t)i));
        assert(!nora_pps_route_output(NORA_PPS_OUTPUT_U1TX, (nora_gpio_rp_t)i));
    }
    assert(!nora_gpio_rp_from_pin(NORA_GPIO_PIN(NORA_GPIO_PORT_A, 5u), &rp));
    assert(!nora_gpio_rp_from_pin(NORA_GPIO_PIN(NORA_GPIO_PORT_F, 0u), &rp));
    assert(!nora_gpio_config_digital_output(NORA_GPIO_PIN(NORA_GPIO_PORT_F, 0u), true));
    assert(!nora_gpio_event_attach(NORA_GPIO_PIN(NORA_GPIO_PORT_F, 2u),
                                  NORA_GPIO_EVENT_EDGE_EITHER, on_edge, &callback_count));
#endif
    puts("PASS: RP conversion, DFP output slots, PPS routes, virtual exclusion and Port F CN");
    return 0;
}
