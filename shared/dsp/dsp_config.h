/**
 * ESP-32 Audio DSP Configuration Structures
 *
 * These structs match the binary format produced by the web configurator.
 * The binary is directly memory-mappable:
 *   const dsp_config_t* cfg = (dsp_config_t*)ptr;
 */

#ifndef DSP_CONFIG_H
#define DSP_CONFIG_H

#include <stdint.h>

#define DSP_MAGIC          0x44535043  // 'DSPC'
#define DSP_VERSION        4
#define DSP_MAX_PEQ_BANDS  10
#define DSP_MAX_ROOM_EQ_BANDS 10
#define DSP_MAX_XO_STAGES  4
#define DSP_NUM_INPUTS     2
#define DSP_NUM_OUTPUTS    4
#define MAX_DELAY_SAMPLES  960   /* 10ms at 96kHz */

#pragma pack(push, 4)

/**
 * Biquad filter coefficients (negated-a convention).
 * Difference equation: y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] + a1*y[n-1] + a2*y[n-2]
 * Note: a1 and a2 are already negated, so inner loop uses all additions.
 */
typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;  // = -a1/a0 from transfer function
    float a2;  // = -a2/a0 from transfer function
} biquad_coeffs_t;

/**
 * Biquad filter state (for real-time processing).
 * Direct Form II Transposed.
 */
typedef struct {
    float z1;  // delay element 1
    float z2;  // delay element 2
} biquad_state_t;

/** Shadow EQ band parameters (UI-friendly, stored alongside biquad coefficients) */
typedef struct {
    float    frequency;
    float    gain_db;
    float    q;
    uint8_t  filter_type;    // BLE_FILTER_*
    uint8_t  enabled;
    uint8_t  _pad[2];
} eq_band_params_t;          // 16 bytes, 4-byte aligned

/** Shadow crossover parameters */
typedef struct {
    float    frequency;
    uint8_t  filter_type;    // BLE_XO_BUTTERWORTH / BLE_XO_LINKWITZ_RILEY
    uint8_t  slope;          // BLE_XO_SLOPE_* (number of biquad stages)
    uint8_t  enabled;
    uint8_t  _pad;
} xo_params_t;               // 8 bytes, 4-byte aligned

/** Input channel configuration */
typedef struct {
    float    gain;           // linear gain
    uint8_t  mute;
    uint8_t  phase_invert;
    uint8_t  num_eq_bands;   // number of active EQ bands
    uint8_t  num_room_eq_bands;
    biquad_coeffs_t eq_bands[DSP_MAX_PEQ_BANDS];
    eq_band_params_t eq_params[DSP_MAX_PEQ_BANDS];
    biquad_coeffs_t  room_eq_bands[DSP_MAX_ROOM_EQ_BANDS];
    eq_band_params_t room_eq_params[DSP_MAX_ROOM_EQ_BANDS];
} dsp_input_t;

/** Routing crosspoint */
typedef struct {
    uint8_t  enabled;
    uint8_t  _pad[3];
    float    gain;           // linear gain (0..1)
} dsp_crosspoint_t;

/** Output channel configuration */
typedef struct {
    float    gain;           // linear gain
    uint32_t delay_samples;
    uint8_t  mute;
    uint8_t  phase_invert;
    uint8_t  num_eq_bands;
    uint8_t  num_hp_stages;
    uint8_t  num_lp_stages;
    uint8_t  _reserved;
    uint8_t  _pad[2];
    biquad_coeffs_t eq_bands[DSP_MAX_PEQ_BANDS];
    biquad_coeffs_t hp_stages[DSP_MAX_XO_STAGES];
    biquad_coeffs_t lp_stages[DSP_MAX_XO_STAGES];
    eq_band_params_t eq_params[DSP_MAX_PEQ_BANDS];
    xo_params_t      hp_params;
    xo_params_t      lp_params;
} dsp_output_t;

/** Global configuration */
typedef struct {
    float    master_volume;  // linear gain
    uint32_t sample_rate;
    uint32_t reserved;
} dsp_global_t;

/** Binary file header */
typedef struct {
    uint32_t magic;          // DSP_MAGIC
    uint16_t version;
    uint16_t preset_index;
    uint32_t sample_rate;
    uint32_t crc32;
} dsp_header_t;

/** System-level tuning (adaptive USB feedback; binary layout unchanged) */
typedef struct {
    float    drift_kp;          // feedback P gain, ppm/ms (default 25)
    float    drift_ki;          // feedback I gain, ppm/(ms*s) (default 0.5)
    float    drift_target_fill; // legacy ABI field; firmware uses a fixed 24 ms cushion
    float    drift_max_ppm;     // max host feedback ±ppm (clamped to 20..200)
} dsp_system_t;

/** Complete DSP configuration (memory-mappable from binary) */
typedef struct {
    dsp_header_t     header;
    dsp_input_t      inputs[DSP_NUM_INPUTS];
    dsp_crosspoint_t routing[DSP_NUM_INPUTS][DSP_NUM_OUTPUTS];
    dsp_output_t     outputs[DSP_NUM_OUTPUTS];
    dsp_global_t     global;
    dsp_system_t     system;
} dsp_config_t;

#pragma pack(pop)

#endif // DSP_CONFIG_H
