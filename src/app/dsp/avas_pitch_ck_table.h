#ifndef AVAS_PITCH_CK_TABLE_H
#define AVAS_PITCH_CK_TABLE_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * cent -> ratio, Q14, 5 cent per entry. Unity (0 cent) is 16384, not 32767: the
 * +200 cent end needs ratio 2^(200/1200) = 1.1225, which does not fit a Q15
 * fraction (max representable value there is just under 1.0). Q14 leaves
 * exactly the headroom the +/-200 cent range needs (0.891..1.122) while
 * everything that multiplies by this table already widens to int32_t first,
 * so the two extra bits of scale cost nothing there.
 *
 * 5 CENT PER ENTRY, NOT 1 -- this table used to carry 401 entries (1 cent
 * each, 802 B) and that alone pushed the single-voice EV88G73A image over its
 * 64 KB ceiling (measured: this board has 145 B of flash to spare with this
 * feature and nothing else changed). 81 entries (162 B) is the arrow keys'
 * OWN step size, so nothing observable is lost there; avas_pitch_ck_cent_to_ratio_q14()
 * rounds *tc's cent argument to the nearest 5 before indexing, so the two input
 * paths (keys, console) land on the same finite set of ratios rather than one
 * being coarser than the other.
 *
 * The table is pure arithmetic (ratio = 2^(cent/1200)), not measured data, so
 * unlike the car_step/bb_step tables in avas_type_ty_ck_tables.h it carries no
 * wav->py generation chain of its own -- regenerating it is one Python
 * expression, not a re-analysis of a reference recording.
 * ------------------------------------------------------------------------- */
#define AVAS_PITCH_CK_CENT_MIN   (-200)
#define AVAS_PITCH_CK_CENT_MAX   (200)
#define AVAS_PITCH_CK_CENT_STEP  (5)
#define AVAS_PITCH_CK_RATIO_Q14_UNITY (16384)

static const int16_t s_avas_pitch_ck_ratio_q14[
    ((AVAS_PITCH_CK_CENT_MAX - AVAS_PITCH_CK_CENT_MIN) / AVAS_PITCH_CK_CENT_STEP) + 1] = {
    14596, 14639, 14681, 14724, 14766, 14809, 14852, 14895, 14938, 14981,
    15024, 15068, 15111, 15155, 15199, 15243, 15287, 15331, 15375, 15420,
    15464, 15509, 15554, 15599, 15644, 15689, 15735, 15780, 15826, 15872,
    15918, 15964, 16010, 16056, 16103, 16149, 16196, 16243, 16290, 16337,
    16384, 16431, 16479, 16527, 16574, 16622, 16670, 16719, 16767, 16815,
    16864, 16913, 16962, 17011, 17060, 17109, 17159, 17208, 17258, 17308,
    17358, 17408, 17459, 17509, 17560, 17611, 17662, 17713, 17764, 17815,
    17867, 17919, 17970, 18022, 18074, 18127, 18179, 18232, 18284, 18337,
    18390
};

/* Clamp to the table's domain and round to the nearest AVAS_PITCH_CK_CENT_STEP.
 * Callers snap BEFORE storing a cent value anywhere (avas_line_ck_request_pitch_cent()
 * does), so *tc/?tc report the cent that is actually sounding rather than the raw
 * argument -- reporting the unsnapped input would have the console claim a pitch
 * this table cannot produce. */
static inline int16_t avas_pitch_ck_snap_cent(int16_t cent)
{
    if (cent < (int16_t)AVAS_PITCH_CK_CENT_MIN) {
        cent = (int16_t)AVAS_PITCH_CK_CENT_MIN;
    } else if (cent > (int16_t)AVAS_PITCH_CK_CENT_MAX) {
        cent = (int16_t)AVAS_PITCH_CK_CENT_MAX;
    }

    /* Round-to-nearest on a signed offset: bias by half a step before the
     * truncating divide, then back to a signed cent value. */
    {
        const int16_t off  = (int16_t)(cent - (int16_t)AVAS_PITCH_CK_CENT_MIN);
        const int16_t half = (int16_t)(AVAS_PITCH_CK_CENT_STEP / 2);
        const int16_t idx  = (int16_t)((off + half) / (int16_t)AVAS_PITCH_CK_CENT_STEP);

        return (int16_t)((int16_t)AVAS_PITCH_CK_CENT_MIN
                          + (int16_t)(idx * (int16_t)AVAS_PITCH_CK_CENT_STEP));
    }
}

/* cent -> ratio. cent must already be on the table's grid (snap it first with
 * avas_pitch_ck_snap_cent()) -- this indexes directly rather than re-rounding, so
 * it stays a single load with no arithmetic on the hot rebuild path. */
static inline int16_t avas_pitch_ck_cent_to_ratio_q14(int16_t cent)
{
    const int16_t idx = (int16_t)((cent - (int16_t)AVAS_PITCH_CK_CENT_MIN)
                                   / (int16_t)AVAS_PITCH_CK_CENT_STEP);

    return s_avas_pitch_ck_ratio_q14[idx];
}

#endif /* AVAS_PITCH_CK_TABLE_H */
