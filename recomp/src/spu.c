/* SPG2xx SPU. See spu.h. Integer widths and casts follow MAME on purpose. */
#include "spu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* per-channel registers (0x3000 + ch * 16) */
enum {
    WAVE_ADDR = 0x0, MODE = 0x1, LOOP_ADDR = 0x2, PAN_VOL = 0x3, ENVELOPE0 = 0x4,
    ENVELOPE_DATA = 0x5, ENVELOPE1 = 0x6, ENVELOPE_ADDR_HIGH = 0x7, ENVELOPE_ADDR = 0x8,
    WAVE_DATA_PREV = 0x9, ENVELOPE_LOOP_CTRL = 0xa, WAVE_DATA = 0xb, ADPCM_SEL = 0xd,
};
/* per-channel phase registers (0x3200 + ch * 16) */
enum {
    PHASE_HIGH = 0x0, PHASE_ACCUM_HIGH = 0x1, TARGET_PHASE_HIGH = 0x2, RAMP_DOWN_CLOCK = 0x3,
    PHASE = 0x4, PHASE_ACCUM = 0x5, TARGET_PHASE = 0x6, PHASE_CTRL = 0x7,
};
#define CHAN_OFFSET_MASK 0xf0f
/* control registers (0x3400) */
enum {
    CHANNEL_ENABLE = 0x00, MAIN_VOLUME = 0x01, CHANNEL_FIQ_ENABLE = 0x02, CHANNEL_FIQ_STATUS = 0x03,
    BEAT_BASE_COUNT = 0x04, BEAT_COUNT = 0x05, ENVCLK0 = 0x06, ENVCLK0_HIGH = 0x07, ENVCLK1 = 0x08,
    ENVCLK1_HIGH = 0x09, ENV_RAMP_DOWN = 0x0a, CHANNEL_STOP = 0x0b, CHANNEL_ZERO_CROSS = 0x0c,
    CONTROL = 0x0d, COMPRESS_CTRL = 0x0e, CHANNEL_STATUS = 0x0f, WAVE_IN_L = 0x10, WAVE_IN_R = 0x11,
    CHANNEL_REPEAT = 0x14, CHANNEL_ENV_MODE = 0x15, CHANNEL_TONE_RELEASE = 0x16,
    CHANNEL_ENV_IRQ = 0x17, CHANNEL_PITCH_BEND = 0x18, EQ_CUTOFF10 = 0x1b, EQ_CUTOFF32 = 0x1c,
    EQ_GAIN10 = 0x1d, EQ_GAIN32 = 0x1e,
};
#define BIS_MASK 0x4000
#define CONTROL_NOINT_MASK 0x0200
#define TONE_MODE_HW_ONESHOT 1

#define CREG(ch, r) s->regs[((ch) << 4) | (r)]
#define PREG(ch, r) s->phase_regs[((ch) << 4) | (r)]

static const uint32_t rampdown_frame_counts[8] = {
    13 * 4, 13 * 16, 13 * 64, 13 * 256, 13 * 1024, 13 * 4096, 13 * 8192, 13 * 8192
};
static const uint32_t envclk_frame_counts[16] = {
    4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 8192, 8192, 8192, 8192
};

/* ---- IMA ADPCM (MAME sound/imaadpcm.cpp) ---- */
static const int8_t index_shift[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static int diff_lookup[89 * 16];
static int tables_done;

static void compute_tables(void)
{
    static const int8_t nbl2bit[16][4] = {
        { 1, 0, 0, 0 }, { 1, 0, 0, 1 }, { 1, 0, 1, 0 }, { 1, 0, 1, 1 },
        { 1, 1, 0, 0 }, { 1, 1, 0, 1 }, { 1, 1, 1, 0 }, { 1, 1, 1, 1 },
        { -1, 0, 0, 0 }, { -1, 0, 0, 1 }, { -1, 0, 1, 0 }, { -1, 0, 1, 1 },
        { -1, 1, 0, 0 }, { -1, 1, 0, 1 }, { -1, 1, 1, 0 }, { -1, 1, 1, 1 },
    };
    for (int step = -8; step <= 80; step++) {
        int stepval = (int)fmin(floor(16.0 * pow(11.0 / 10.0, (double)step)), 32767.);
        if (step == -5 || step == -4) stepval++;
        for (int nib = 0; nib < 16; nib++)
            diff_lookup[(step + 8) * 16 + nib] = nbl2bit[nib][0] *
                (stepval * nbl2bit[nib][1] + stepval / 2 * nbl2bit[nib][2] +
                 stepval / 4 * nbl2bit[nib][3] + stepval / 8);
    }
    tables_done = 1;
}

static int16_t adpcm_clock(spu_t *s, int ch, uint8_t nibble)
{
    int32_t *sig = &s->adpcm[ch].signal, *step = &s->adpcm[ch].step;
    *sig += diff_lookup[*step * 16 + (nibble & 15)];
    if (*sig > 32767) *sig = 32767;
    else if (*sig < -32768) *sig = -32768;
    *step += index_shift[nibble & 7];
    if (*step > 88) *step = 88;
    else if (*step < 0) *step = 0;
    return (int16_t)*sig;
}

/* ---- register field helpers ---- */
static uint16_t get_wave_addr_high(spu_t *s, int ch) { return CREG(ch, MODE) & 0x003f; }
static uint16_t get_loop_addr_high(spu_t *s, int ch) { return (CREG(ch, MODE) & 0x0fc0) >> 6; }
static uint16_t get_tone_mode(spu_t *s, int ch) { return (CREG(ch, MODE) & 0x3000) >> 12; }
static int get_16bit_bit(spu_t *s, int ch) { return (CREG(ch, MODE) & 0x4000) ? 1 : 0; }
static int get_adpcm_bit(spu_t *s, int ch) { return (CREG(ch, MODE) & 0x8000) ? 1 : 0; }
static uint16_t get_volume(spu_t *s, int ch) { return CREG(ch, PAN_VOL) & 0x007f; }
static uint16_t get_pan(spu_t *s, int ch) { return (CREG(ch, PAN_VOL) & 0x7f00) >> 8; }
static uint16_t get_envelope_inc(spu_t *s, int ch) { return CREG(ch, ENVELOPE0) & 0x007f; }
static int get_envelope_sign_bit(spu_t *s, int ch) { return (CREG(ch, ENVELOPE0) & 0x0080) ? 1 : 0; }
static uint16_t get_envelope_target(spu_t *s, int ch) { return (CREG(ch, ENVELOPE0) & 0x7f00) >> 8; }
static uint16_t get_edd(spu_t *s, int ch) { return CREG(ch, ENVELOPE_DATA) & 0x007f; }
static uint16_t get_envelope_count(spu_t *s, int ch) { return (CREG(ch, ENVELOPE_DATA) & 0xff00) >> 8; }
static void set_edd(spu_t *s, int ch, uint8_t edd) { CREG(ch, ENVELOPE_DATA) = (uint16_t)((CREG(ch, ENVELOPE_DATA) & ~0x007f) | edd); }
static void set_envelope_count(spu_t *s, int ch, uint16_t count) { CREG(ch, ENVELOPE_DATA) = (uint16_t)(get_edd(s, ch) | (count << 8)); }
static uint16_t get_envelope_load(spu_t *s, int ch) { return CREG(ch, ENVELOPE1) & 0x00ff; }
static int get_envelope_repeat_bit(spu_t *s, int ch) { return (CREG(ch, ENVELOPE1) & 0x0100) ? 1 : 0; }
static uint16_t get_envelope_repeat_count(spu_t *s, int ch) { return (CREG(ch, ENVELOPE1) & 0xfe00) >> 9; }
static void set_envelope_repeat_count(spu_t *s, int ch, uint16_t count)
{
    CREG(ch, ENVELOPE1) = (uint16_t)((CREG(ch, ENVELOPE1) & ~0xfe00) | ((count << 9) & 0xfe00));
}
static uint16_t get_envelope_addr_high(spu_t *s, int ch) { return CREG(ch, ENVELOPE_ADDR_HIGH) & 0x003f; }
static uint16_t get_envelope_eaoffset(spu_t *s, int ch) { return CREG(ch, ENVELOPE_LOOP_CTRL) & 0x01ff; }
static uint16_t get_rampdown_offset(spu_t *s, int ch) { return (CREG(ch, ENVELOPE_LOOP_CTRL) & 0xfe00) >> 9; }
static int get_adpcm36_bit(spu_t *s, int ch) { return (CREG(ch, ADPCM_SEL) & 0x8000) ? 1 : 0; }
static uint16_t get_rampdown_clock(spu_t *s, int ch) { return PREG(ch, RAMP_DOWN_CLOCK) & 0x0007; }
static uint32_t get_phase(spu_t *s, int ch) { return ((uint32_t)(PREG(ch, PHASE_HIGH) & 7) << 16) | PREG(ch, PHASE); }
static uint32_t get_wave_addr(spu_t *s, int ch) { return ((uint32_t)get_wave_addr_high(s, ch) << 16) | CREG(ch, WAVE_ADDR); }
static uint32_t get_loop_addr(spu_t *s, int ch) { return ((uint32_t)get_loop_addr_high(s, ch) << 16) | CREG(ch, LOOP_ADDR); }
static uint32_t get_envelope_addr(spu_t *s, int ch) { return ((uint32_t)get_envelope_addr_high(s, ch) << 16) | CREG(ch, ENVELOPE_ADDR); }
static void set_wave_addr(spu_t *s, int ch, uint32_t addr)
{
    CREG(ch, MODE) = (uint16_t)((CREG(ch, MODE) & ~0x003f) | ((addr >> 16) & 0x003f));
    CREG(ch, WAVE_ADDR) = addr & 0xffff;
}
static void inc_wave_addr(spu_t *s, int ch) { set_wave_addr(s, ch, get_wave_addr(s, ch) + 1); }

static uint32_t get_envelope_clock(spu_t *s, int ch)
{
    if (ch < 4) return (s->ctrl[ENVCLK0] >> (ch << 2)) & 0xf;
    if (ch < 8) return (s->ctrl[ENVCLK0_HIGH] >> ((ch - 4) << 2)) & 0xf;
    if (ch < 12) return (s->ctrl[ENVCLK1] >> ((ch - 8) << 2)) & 0xf;
    return (s->ctrl[ENVCLK1_HIGH] >> ((ch - 12) << 2)) & 0xf;
}
static uint32_t get_envclk_frame_count(spu_t *s, int ch) { return envclk_frame_counts[get_envelope_clock(s, ch)]; }
static uint32_t get_rampdown_frame_count(spu_t *s, int ch) { return rampdown_frame_counts[get_rampdown_clock(s, ch)]; }

static void update_rate(spu_t *s, int ch)
{
    s->channel_rate[ch] = ((double)get_phase(s, ch) * 140625.0 * 2.0) / (double)(1 << 19);
    s->channel_rate_accum[ch] = 0.0;
}

/* ---- channel control ---- */
static void start_channel(spu_t *s, int ch)
{
    if ((s->ctrl[CHANNEL_FIQ_ENABLE] >> ch) & 1)
        fprintf(stderr, "spu: channel %d FIQ enabled, not modelled\n", ch);
    s->ctrl[CHANNEL_STATUS] |= 1 << ch;
    s->envelope_addr[ch] = get_envelope_addr(s, ch);
    set_envelope_count(s, ch, get_envelope_load(s, ch));
    s->adpcm[ch].signal = s->adpcm[ch].step = 0;
    s->sample_shift[ch] = 0;
    s->sample_count[ch] = 0;
    if (get_adpcm36_bit(s, ch)) memset(&s->adpcm36[ch], 0, sizeof(s->adpcm36[ch]));
}

static void stop_channel(spu_t *s, int ch)
{
    s->ctrl[CHANNEL_STATUS] &= ~(1 << ch);
    CREG(ch, MODE) &= ~0x8000;
    s->ctrl[CHANNEL_TONE_RELEASE] &= ~(1 << ch);
    s->ctrl[ENV_RAMP_DOWN] &= ~(1 << ch);
}

static void loop_channel(spu_t *s, int ch)
{
    set_wave_addr(s, ch, get_loop_addr(s, ch));
    s->sample_shift[ch] = 0;
}

static uint16_t decode_adpcm36_nybble(spu_t *s, int ch, uint8_t data)
{
    int32_t shift = s->adpcm36[ch].header & 0xf;
    int16_t filter = (int16_t)((s->adpcm36[ch].header & 0x3f0) >> 4);
    int16_t f0 = (int16_t)(filter | ((filter & 0x20) ? ~0x3f : 0));
    int32_t f1 = 0;
    int16_t sdata = (int16_t)(data << 12);
    sdata = (int16_t)((sdata >> shift) + (((s->adpcm36[ch].prevsamp[0] * f0) + (s->adpcm36[ch].prevsamp[1] * f1) + 32) >> 12));
    s->adpcm36[ch].prevsamp[1] = s->adpcm36[ch].prevsamp[0];
    s->adpcm36[ch].prevsamp[0] = sdata;
    return (uint16_t)sdata ^ 0x8000;
}

static int fetch_sample(spu_t *s, int ch)
{
    CREG(ch, WAVE_DATA_PREV) = CREG(ch, WAVE_DATA);
    const uint16_t tone_mode = get_tone_mode(s, ch);

    if (get_adpcm36_bit(s, ch) && tone_mode != 0 && s->adpcm36[ch].remaining == 0) {
        s->adpcm36[ch].header = s->read(s->ctx, get_wave_addr(s, ch));
        s->adpcm36[ch].remaining = 8;
        inc_wave_addr(s, ch);
    }

    uint16_t raw = tone_mode ? s->read(s->ctx, get_wave_addr(s, ch)) : CREG(ch, WAVE_DATA);

    if (get_adpcm_bit(s, ch) || get_adpcm36_bit(s, ch)) {
        if (tone_mode != 0 && raw == 0xffff) {
            if (tone_mode == TONE_MODE_HW_ONESHOT) {
                s->sample_count[ch] = 0;
                s->ctrl[CHANNEL_STOP] |= 1 << ch;
                stop_channel(s, ch);
                return 0;
            }
            s->sample_count[ch] = 0;
            loop_channel(s, ch);
            CREG(ch, MODE) &= ~0x8000;
        } else {
            CREG(ch, WAVE_DATA) = raw;
            CREG(ch, WAVE_DATA) >>= s->sample_shift[ch];
            const uint8_t nib = (uint8_t)(CREG(ch, WAVE_DATA) & 0x000f);
            if (get_adpcm36_bit(s, ch)) CREG(ch, WAVE_DATA) = decode_adpcm36_nybble(s, ch, nib);
            else CREG(ch, WAVE_DATA) = (uint16_t)adpcm_clock(s, ch, nib) ^ 0x8000;
        }
        s->sample_count[ch]++;
    } else if (get_16bit_bit(s, ch)) {
        if (tone_mode != 0 && raw == 0xffff) {
            if (tone_mode == TONE_MODE_HW_ONESHOT) {
                /* unlike the other formats, MAME does not set the stop bit here */
                s->sample_count[ch] = 0;
                stop_channel(s, ch);
                return 0;
            }
            s->sample_count[ch] = 0;
            loop_channel(s, ch);
        } else {
            CREG(ch, WAVE_DATA) = raw;
        }
        s->sample_count[ch]++;
    } else { /* 8-bit */
        if (tone_mode != 0) {
            if (s->sample_shift[ch]) raw &= 0xff00;
            else raw = (uint16_t)(raw << 8);
            raw |= raw >> 8;
            if (raw == 0xffff) {
                if (tone_mode == TONE_MODE_HW_ONESHOT) {
                    s->sample_count[ch] = 0;
                    s->ctrl[CHANNEL_STOP] |= 1 << ch;
                    stop_channel(s, ch);
                    return 0;
                }
                s->sample_count[ch] = 0;
                loop_channel(s, ch);
            } else {
                CREG(ch, WAVE_DATA) = raw;
            }
        }
        s->sample_count[ch]++;
    }
    return 1;
}

static int advance_channel(spu_t *s, int ch)
{
    s->channel_rate_accum[ch] += s->channel_rate[ch];
    uint32_t n = 0;
    while (s->channel_rate_accum[ch] >= 70312.5) {
        s->channel_rate_accum[ch] -= 70312.5;
        n++;
    }
    if (!n) return 1;

    int playing = 1;
    for (uint32_t i = 0; i < n; i++) {
        playing = fetch_sample(s, ch);
        if (!playing) break;
        if (get_adpcm_bit(s, ch) || get_adpcm36_bit(s, ch)) {
            s->sample_shift[ch] += 4;
            if (s->sample_shift[ch] >= 16) {
                s->sample_shift[ch] = 0;
                inc_wave_addr(s, ch);
                if (get_adpcm36_bit(s, ch)) s->adpcm36[ch].remaining--;
            }
        } else if (get_16bit_bit(s, ch)) {
            inc_wave_addr(s, ch);
        } else {
            s->sample_shift[ch] += 8;
            if (s->sample_shift[ch] >= 16) {
                s->sample_shift[ch] = 0;
                inc_wave_addr(s, ch);
            }
        }
    }
    return playing;
}

static void rampdown_tick(spu_t *s, int ch)
{
    const uint8_t old_edd = (uint8_t)get_edd(s, ch);
    uint8_t new_edd = (uint8_t)(old_edd - get_rampdown_offset(s, ch));
    if (new_edd > old_edd) new_edd = 0;
    if (new_edd) {
        CREG(ch, ENVELOPE_DATA) &= ~0x007f;
        CREG(ch, ENVELOPE_DATA) |= new_edd & 0x007f;
        s->rampdown_frame[ch] = get_rampdown_frame_count(s, ch);
    } else {
        s->ctrl[CHANNEL_STOP] |= 1 << ch;
        stop_channel(s, ch);
    }
}

static void envelope_tick(spu_t *s, int ch)
{
    uint16_t new_count = get_envelope_count(s, ch);
    const uint16_t curr_edd = get_edd(s, ch);
    if (new_count > 0) {
        new_count--;
        set_envelope_count(s, ch, new_count);
    }
    if (new_count != 0) return;

    const uint16_t target = get_envelope_target(s, ch);
    uint16_t new_edd = curr_edd;
    const uint16_t inc = get_envelope_inc(s, ch);
    if (new_edd != target) {
        if (get_envelope_sign_bit(s, ch)) {
            new_edd -= inc;
            if (new_edd > curr_edd) new_edd = 0;
            else if (new_edd < target) new_edd = target;
            if (new_edd == 0) {
                s->ctrl[CHANNEL_STOP] |= 1 << ch;
                stop_channel(s, ch);
                return;
            }
        } else {
            new_edd += inc;
            if (new_edd >= target) new_edd = target;
        }
    }
    if (new_edd == target) {
        if (get_envelope_repeat_bit(s, ch)) {
            const uint16_t repeat_count = (uint16_t)(get_envelope_repeat_count(s, ch) - 1);
            if (repeat_count == 0) {
                CREG(ch, ENVELOPE0) = s->read(s->ctx, s->envelope_addr[ch]);
                CREG(ch, ENVELOPE1) = s->read(s->ctx, s->envelope_addr[ch] + 1);
                CREG(ch, ENVELOPE_LOOP_CTRL) = s->read(s->ctx, s->envelope_addr[ch] + 2);
                s->envelope_addr[ch] = get_envelope_addr(s, ch) + get_envelope_eaoffset(s, ch);
            } else {
                set_envelope_repeat_count(s, ch, repeat_count);
            }
        } else {
            CREG(ch, ENVELOPE0) = s->read(s->ctx, s->envelope_addr[ch]);
            CREG(ch, ENVELOPE1) = s->read(s->ctx, s->envelope_addr[ch] + 1);
            s->envelope_addr[ch] += 2;
        }
    }
    set_envelope_count(s, ch, get_envelope_load(s, ch));
    set_edd(s, ch, (uint8_t)new_edd);
}

/* ---- stream ---- */
void spu_generate(spu_t *s, uint32_t samples)
{
    for (uint32_t i = 0; i < samples; i++) {
        int32_t left_total = 0, right_total = 0;
        for (int ch = 0; ch < 16; ch++) {
            if (!(s->ctrl[CHANNEL_STATUS] & (1 << ch))) continue;
            if (!advance_channel(s, ch)) continue;

            int32_t sample = (int16_t)(CREG(ch, WAVE_DATA) ^ 0x8000);
            if (!(s->ctrl[CONTROL] & CONTROL_NOINT_MASK)) {
                int32_t prev = (int16_t)(CREG(ch, WAVE_DATA_PREV) ^ 0x8000);
                int16_t lerp = (int16_t)((s->channel_rate_accum[ch] / 70312.5) * 256.0);
                prev = (prev * (0x100 - lerp)) >> 8;
                sample = (sample * lerp) >> 8;
                sample += prev;
            }
            sample = (sample * (int32_t)get_edd(s, ch)) >> 7;

            const int32_t vol = get_volume(s, ch), pan = get_pan(s, ch);
            int32_t pan_left, pan_right;
            if (pan < 0x40) { pan_left = 0x7f * vol; pan_right = pan * 2 * vol; }
            else { pan_left = (0x7f - pan) * 2 * vol; pan_right = 0x7f * vol; }
            left_total += ((int16_t)sample * (int16_t)pan_left) >> 14;
            right_total += ((int16_t)sample * (int16_t)pan_right) >> 14;

            const uint16_t mask = (uint16_t)(1 << ch);
            if (s->ctrl[ENV_RAMP_DOWN] & mask) {
                if (s->rampdown_frame[ch] > 0) s->rampdown_frame[ch]--;
                if (s->rampdown_frame[ch] == 0) rampdown_tick(s, ch);
            } else if (!(s->ctrl[CHANNEL_ENV_MODE] & mask)) {
                if (s->envclk_frame[ch] > 0) s->envclk_frame[ch]--;
                if (s->envclk_frame[ch] == 0) {
                    envelope_tick(s, ch);
                    s->envclk_frame[ch] = get_envclk_frame_count(s, ch);
                }
            }
        }
        if (s->ctrl[WAVE_IN_L]) left_total += (int32_t)s->ctrl[WAVE_IN_L] - 0x8000;
        if (s->ctrl[WAVE_IN_R]) right_total += (int32_t)s->ctrl[WAVE_IN_R] - 0x8000;

        if (((s->ctrl[CONTROL] & 0x00c0) >> 6) == 0) { left_total >>= 4; right_total >>= 4; }
        else { left_total >>= 2; right_total >>= 2; } /* MAME: x1/x2 clip, treated as 1/4 */

        const int16_t l = (int16_t)((left_total * (int16_t)s->ctrl[MAIN_VOLUME]) >> 7);
        const int16_t r = (int16_t)((right_total * (int16_t)s->ctrl[MAIN_VOLUME]) >> 7);
        if (s->out) s->out(s->ctx, l, r);
    }
    s->samples_done += samples;
}

/* ---- registers ---- */
void spu_reset(spu_t *s)
{
    if (!tables_done) compute_tables();
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->phase_regs, 0, sizeof(s->phase_regs));
    memset(s->ctrl, 0, sizeof(s->ctrl));
    memset(s->sample_shift, 0, sizeof(s->sample_shift));
    memset(s->sample_count, 0, sizeof(s->sample_count));
    memset(s->channel_rate, 0, sizeof(s->channel_rate));
    memset(s->channel_rate_accum, 0, sizeof(s->channel_rate_accum));
    memset(s->rampdown_frame, 0, sizeof(s->rampdown_frame));
    memset(s->envclk_frame, 4, sizeof(s->envclk_frame)); /* MAME memsets bytes: 0x04040404 */
    memset(s->envelope_addr, 0, sizeof(s->envelope_addr));
    memset(s->adpcm, 0, sizeof(s->adpcm));
    memset(s->adpcm36, 0, sizeof(s->adpcm36));
    s->curr_beat_base_count = 0;
    s->ctrl[CHANNEL_REPEAT] = 0x3f;
    s->ctrl[CHANNEL_ENV_MODE] = 0x3f;
    s->samples_done = 0;
}

uint16_t spu_read(spu_t *s, uint32_t off)
{
    if (off < 0x200) return s->regs[off];
    if (off < 0x400) return s->phase_regs[off - 0x200];
    return s->ctrl[off - 0x400];
}

static void channel_w(spu_t *s, uint32_t off, uint16_t data)
{
    switch (off & CHAN_OFFSET_MASK) {
    case PAN_VOL: s->regs[off] = data & 0x7f7f; break;
    case ENVELOPE_DATA: s->regs[off] = data & 0xff7f; break;
    case ADPCM_SEL: s->regs[off] = data & 0xfe00; break;
    default: s->regs[off] = data; break;
    }
}

static void phase_w(spu_t *s, uint32_t off, uint16_t data)
{
    const int ch = (off & 0x01f0) >> 4;
    switch (off & CHAN_OFFSET_MASK) {
    case PHASE_HIGH: s->phase_regs[off] = data & 7; update_rate(s, ch); break;
    case PHASE_ACCUM_HIGH: case TARGET_PHASE_HIGH: case RAMP_DOWN_CLOCK:
        s->phase_regs[off] = data & 7; break;
    case PHASE: s->phase_regs[off] = data; update_rate(s, ch); break;
    default: s->phase_regs[off] = data; break;
    }
}

static void ctrl_w(spu_t *s, uint32_t off, uint16_t data)
{
    uint16_t old, changed;
    switch (off) {
    case CHANNEL_ENABLE:
        old = s->ctrl[off];
        s->ctrl[off] = data;
        changed = old ^ data;
        for (int ch = 0; ch < 16; ch++) {
            const uint16_t mask = (uint16_t)(1 << ch);
            if (!(changed & mask)) continue;
            if (data & mask) {
                if (s->ctrl[CHANNEL_STOP] & mask) continue;
                if (!(s->ctrl[CHANNEL_STATUS] & mask)) start_channel(s, ch);
            } else if (s->ctrl[CHANNEL_STATUS] & mask) {
                stop_channel(s, ch);
            }
        }
        break;
    case MAIN_VOLUME: s->ctrl[off] = data & 0x007f; break;
    case CHANNEL_FIQ_ENABLE: s->ctrl[off] = data; break;
    case CHANNEL_FIQ_STATUS: s->ctrl[off] &= ~data; break;
    case BEAT_BASE_COUNT:
        s->ctrl[off] = data & 0x07ff;
        s->curr_beat_base_count = s->ctrl[off];
        break;
    case BEAT_COUNT:
        s->ctrl[off] &= ~(data & BIS_MASK);
        s->ctrl[off] &= BIS_MASK;
        s->ctrl[off] |= data & ~BIS_MASK;
        if (s->ctrl[off] & 0x8000) fprintf(stderr, "spu: beat IRQ enabled, not modelled\n");
        break;
    case ENVCLK0: case ENVCLK1: {
        old = s->ctrl[off];
        s->ctrl[off] = data;
        changed = old ^ data;
        const int base = off == ENVCLK0 ? 0 : 8;
        for (int b = 0; b < 4; b++)
            if (changed & (0x0f << (b << 2))) s->envclk_frame[b + base] = get_envclk_frame_count(s, b + base);
        break;
    }
    case ENVCLK0_HIGH: case ENVCLK1_HIGH: {
        old = s->ctrl[off];
        s->ctrl[off] = data;
        changed = old ^ data;
        const int base = off == ENVCLK0_HIGH ? 0 : 8;
        /* MAME computes the count from channel b + base, not b + base + 4 */
        for (int b = 0; b < 4; b++)
            if (changed & (0x0f << (b << 2))) s->envclk_frame[b + base + 4] = get_envclk_frame_count(s, b + base);
        break;
    }
    case ENV_RAMP_DOWN:
        old = s->ctrl[off];
        s->ctrl[off] = data & s->ctrl[CHANNEL_STATUS];
        changed = old ^ s->ctrl[off];
        for (int ch = 0; ch < 16; ch++)
            if ((changed & (1 << ch)) && (data & (1 << ch))) s->rampdown_frame[ch] = get_rampdown_frame_count(s, ch);
        break;
    case CHANNEL_STOP:
        old = s->ctrl[off];
        s->ctrl[off] &= ~data;
        changed = old ^ s->ctrl[off];
        for (int ch = 0; ch < 16; ch++) {
            const uint16_t mask = (uint16_t)(1 << ch);
            if (!(changed & mask)) continue;
            if (!(s->ctrl[CHANNEL_ENABLE] & mask)) continue;
            if (!(s->ctrl[CHANNEL_STATUS] & mask)) start_channel(s, ch);
        }
        break;
    case CONTROL: s->ctrl[off] = data & 0x9fe8; break;
    case CHANNEL_STATUS: break; /* read-only */
    case WAVE_IN_L: case WAVE_IN_R:
        /* MAME brings the stream up to date first; not modelled (Batman never writes these after init) */
        s->ctrl[off] = data;
        break;
    case CHANNEL_ENV_IRQ: s->ctrl[off] &= ~data; break;
    case EQ_CUTOFF10: case EQ_CUTOFF32: case EQ_GAIN10: case EQ_GAIN32: s->ctrl[off] = data & 0x7f7f; break;
    default: s->ctrl[off] = data; break;
    }
}

void spu_write(spu_t *s, uint32_t off, uint16_t data)
{
    if (off < 0x200) channel_w(s, off, data);
    else if (off < 0x400) phase_w(s, off - 0x200, data);
    else ctrl_w(s, off - 0x400, data);
}
