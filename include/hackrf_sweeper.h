/*
 * Copyright 2024 Subreption LLC <research@subreption.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __HACKRF_SWEEPER_H__
#define __HACKRF_SWEEPER_H__

#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h> // for bool
#include <hackrf.h>
#include <fftw3.h>

/* Until merge into libhackrf, if it happens, we take these for win32/64 support */
#ifdef _WIN32
	#define ADD_EXPORTS

	/* You should define ADD_EXPORTS *only* when building the DLL. */
	#ifdef ADD_EXPORTS
		#define ADDAPI __declspec(dllexport)
	#else
		#define ADDAPI __declspec(dllimport)
	#endif

	/* Define calling convention in one place, for convenience. */
	#define ADDCALL __cdecl

#else /* _WIN32 not defined. */

	#define ADDAPI
	#define ADDCALL

#endif

/**
 * @defgroup sweep Sweep related functions, structures and enums
 *
 * @brief Sweep initialization and configuration, exit, error handling, etc.
 *
 * # Library initialization & exit
 * The sweeping API needs an initialized state with frequency ranges and output mode.
 * A @ref hackrf_sweep_state must be previously allocated and passed to the @ref hackrf_sweep_init or optionally the @ref hackrf_sweep_easy_init function.
 * The output mode must be chosen first via @ref hackrf_sweep_set_output and the frequency ranges must be configured with @ref hackrf_sweep_set_range afterwards.
 *
 * # Sweep control
 * The sweep can be started via @ref hackrf_sweep_start and stopped with @ref hackrf_sweep_stop. A stopped sweep can run again with no additional configuration.
 * @ref hackrf_sweep_close must be used when the sweep state is no longer needed. The state will be released and invalidated.
 *
 */

/**
 * @defgroup sweep_callbacks Sweep callbacks
 *
 * @brief Configuration of user-supplied callbacks for FFT bins, raw transfer access, etc.
 *
 * # Raw access to transfers
 * The raw data returned from the device can be supplied to an user-supplied callback. This is the data as seen
 * from the USB transfer, with no alterations.
 *
 * ## Bypass mode
 * The bypass parameter in @ref hackrf_sweep_set_raw_sample_rx_callback can be used to disable all FFT
 * calculations, whenever the user desires raw transfer access with no additional processing.
 *
 * # FFT bins
 * An user-supplied callback can be set via @ref hackrf_sweep_set_fft_rx_callback to receive the FFT bins
 * once processed. The user must then read them from the FFT state @ref hackrf_sweep_fft_ctx.
 * This callback will be bypassed if the raw transfer callback is enabled in bypass mode.
 *
 * # Caveats
 * It isn't recommended to use both callbacks at the same time for obvious reasons. The suggested mode of
 * operation (whenever raw transfer processing is desirable) is to use @ref hackrf_sweep_set_raw_sample_rx_callback to enable the raw transfers with the bypass
 * parameter enabled. The internal RX callback then bypasses all the FFT processing and immediately calls
 * the user-supplied raw transfer sampling callback.
 *
 * # Callback implementation
 * The user-supplied callbacks must return zero (0) if they desire to be called again. Otherwise, a non-zero
 * return will lead to the sweep RX callback resetting them, effectively disabling them until the user configures
 * the callbacks via @ref hackrf_sweep_set_raw_sample_rx_callback or @ref hackrf_sweep_set_fft_rx_callback
 *
 */

#define FREQ_ONE_MHZ (1000000ull)

#define FREQ_MIN_MHZ (0)    /*    0 MHz */
#define FREQ_MAX_MHZ (7250) /* 7250 MHz */

#define DEFAULT_SAMPLE_RATE_HZ            (20000000) /* 20MHz default sample rate */
#define DEFAULT_BASEBAND_FILTER_BANDWIDTH (15000000) /* 15MHz default */

#define TUNE_STEP (DEFAULT_SAMPLE_RATE_HZ / FREQ_ONE_MHZ)
#define OFFSET    7500000

/* Verify these against libhackrf/firmware always */
#define BLOCKS_PER_TRANSFER 16
#define THROWAWAY_BLOCKS    2

#define SWEEP_STATE_STOPPED      	(1 << 0)
#define SWEEP_STATE_EXITING      	(1 << 1)
#define SWEEP_STATE_RUNNING      	(1 << 2)
#define SWEEP_STATE_INITIALIZED  	(1 << 3)
#define SWEEP_STATE_RELEASED     	(1 << 4)
#define SWEEP_STATE_SWEEP_STARTED 	(1 << 5)
#define SWEEP_STATE_ONESHOT 		(1 << 6)
#define SWEEP_STATE_FINITE  		(1 << 7)
#define SWEEP_STATE_OUTPUT_SET      (1 << 8)
#define SWEEP_STATE_NORMALIZED_TIMESTAMP (1 << 9)
#define SWEEP_STATE_BYPASS_FFT 		(1 << 10)

/**
 * error enum, returned by many libhackrf_sweeper functions
 *
 * Many functions that are specified to return INT are actually returning this enum. This is made to be merge-compatible with libhackrf, reserving an unused range for our
 * error codes.
 * @ingroup sweep
 */
enum hackrf_sweep_error {
	/**
	 * Invalid frequency range supplied
	 */
	HACKRF_SWEEP_ERROR_INVALID_RANGE = -6000,

	/**
	 * Supplied parameter is incompatible with the active output mode
	 */
	HACKRF_SWEEP_ERROR_INCOMPATIBLE_MODE = -6001,

	/**
	 * Range count surpasses the supported limit.
	 */
	HACKRF_SWEEP_ERROR_INVALID_RANGE_COUNT = -6002,

	/**
	 * The state is not ready (ex. invalid order for API calls)
	 */
	HACKRF_SWEEP_ERROR_NOT_READY = -6003,

	/**
	 * The FFT size is invalid
	 */
	HACKRF_SWEEP_ERROR_INVALID_FFT_SIZE = -6004
};

/**
 * sweep data output mode enum
 *
 * Used by @ref hackrf_sweep_set_output, to set sweep output mode.
 *
 * @ingroup sweep
 */
typedef enum hackrf_sweep_output_mode {
	/**
	 * Text mode output (ASCII tabulated strings)
	 */
	HACKRF_SWEEP_OUTPUT_MODE_TEXT = 0,

	/**
	 * FFT binary packed data
	 */
	HACKRF_SWEEP_OUTPUT_MODE_BINARY = 1,

	/**
	 * Inverted FFT binary packed data
	 */
	HACKRF_SWEEP_OUTPUT_MODE_IFFT = 2,

	/**
	 * Limiter for validation
	 */
	HACKRF_SWEEP_OUTPUT_MODE_LAST
} hackrf_sweep_output_mode_t;

/**
 * sweep data output type enum
 *
 * Used by @ref hackrf_sweep_set_output, to set sweep output type (target).
 *
 * @ingroup sweep
 */
typedef enum hackrf_sweep_output_type {
	/**
	 * No-OPeration (literally passthrough, used for debugging, external callback-only and as benchmarking target)
	 */
	HACKRF_SWEEP_OUTPUT_TYPE_NOP = 0,

	/**
	 * File (as in stdlib) output
	 */
	HACKRF_SWEEP_OUTPUT_TYPE_FILE = 1,

	/**
	 * Limiter for validation
	 */
	HACKRF_SWEEP_OUTPUT_TYPE_LAST
} hackrf_sweep_output_type_t;

/**
 * Opaque struct for sweep FFT(W) configuration and data. Must be allocated and initialized via
 * @ref hackrf_sweep_setup_fft and be destroyed via @ref hackrf_sweep_free as part of the sweep state.
 * @ref hackrf_sweep_state
 * @ingroup sweep
 */
struct hackrf_sweep_fft_ctx {
	/**
	 * FFT size (calculated)
	 */
	int size;

	/**
	 * FFT bin width
	 */
	double bin_width;

	/**
	 * FFTW plan type
	 */
	int plan_type;

	/**
	 * Inverted FFT index (internal use)
	 */
	uint32_t ifft_idx;

	/**
	 * FFT and Inverted FFT complex (float) number sets (internal)
	 */
	fftwf_complex *fftw_in;
	fftwf_complex *fftw_out;
	fftwf_complex *ifftw_in;
	fftwf_complex *ifftw_out;

	/**
	 * FFT plan for FFTW
	 */
	fftwf_plan plan;

	/**
	 * Inverted FFT plan for FFTW
	 */
	fftwf_plan plan_inverted;

	/**
	 * FFT power readings and window (arrays)
	 */
	float *pwr;
	float *window;
};

typedef struct hackrf_sweep_fft_ctx hackrf_sweep_fft_ctx_t;

/**
 * External sample block callback with passthrough samples from the sweep RX callback.
 * Receives multiple "blocks" of data, each with a 10-byte header containing the tuned frequency followed by the samples. See @ref hackrf_init_sweep for more info.
 * Read the `hackrf_sample_block_cb_fn` documentation form for details.
 * @ingroup sweep_callbacks
 */
typedef int (*hackrf_sweep_sample_block_cb_fn)(void *sweep_state, hackrf_transfer *transfer);

/**
 * External callback for FFT data. Receives the binary FFT packed data. The fft @ref hackrf_sweep_fft_ctx context contains the FFT bins.
 * The format can be handled the same way the internal `fft_output_record_binary()` function uses, as an example.
 * @ingroup sweep_callbacks
 */
typedef int (*hackrf_sweep_rx_cb_fn)(void *sweep_state, uint64_t frequency, hackrf_transfer *transfer);

/**
 * Opaque mutex lock callback
 * It should call OS-specific library functions to lock a mutex.
 * @ingroup sweep_callbacks
 */
typedef int (*hackrf_sweep_mutex_lock_fn)(void *mutex);

/**
 * Opaque mutex unlock callback
 * It should call OS-specific library functions to unlock a mutex.
 * @ingroup sweep_callbacks
 */
typedef int (*hackrf_sweep_mutex_unlock_fn)(void *mutex);

/**
 * Opaque struct for sweep state control. Must be allocated via malloc and initialized via
 * @ref hackrf_sweep_init or @ref hackrf_sweep_easy_init, closed with @ref hackrf_sweep_close
 * and be destroyed via @ref hackrf_sweep_free.
 * @ingroup sweep
 */

struct hackrf_sweep_state
{
	/**
	 * Maximum number of sweeps (if non-zero) for a finite run
	 */
	uint32_t max_sweeps;

	/**
	 * Used internally for tracking USB transfer delays
	 */
	struct timeval usb_transfer_time;

	/**
	 * Frequency list configured via @ref hackrf_sweep_set_range
	 */
	uint16_t frequencies[MAX_SWEEP_RANGES * 2];

	/**
	 * length of array @p frequencies (in pairs, so total array length / 2!). Must be less than @ref MAX_SWEEP_RANGES
	 */
	int num_ranges;

	/**
	 * width of each tuning step in Hz
	 */
	uint32_t tune_step;

	/**
	 * Used internally for FFT size and optimization of frequency ranges
	 */
	int step_count;

	/**
	 * Reference to the HackRF device associated with this sweep state
	 */
	hackrf_device *device;

	/**
	 * Output mode @ref hackrf_sweep_output_mode, set via @ref hackrf_sweep_set_output
	 */
	hackrf_sweep_output_mode_t output_mode;

	/**
	 * Output type (target) @ref hackrf_sweep_output_type, set via @ref hackrf_sweep_set_output
	 */
	hackrf_sweep_output_type_t output_type;

	/**
	 * File pointer managed by the user and set via @ref hackrf_sweep_set_output
	 */
	FILE *output;

	/**
	 * Callback for raw sample handling @ref hackrf_sweep_sample_block_cb_fn
	 */
	hackrf_sweep_sample_block_cb_fn ext_cb_sample_block;

	/**
	 * Callback for FFT bins handling @ref hackrf_sweep_rx_cb_fn
	 */
	hackrf_sweep_rx_cb_fn ext_cb_fft_ready;

	/**
	 * FFT state and FFTW configuration @ref hackrf_sweep_fft_ctx
	 */
	hackrf_sweep_fft_ctx_t fft;

	/**
	 * Flags for running state and other conditions.
	 */
	unsigned int flags;

	/**
	 * Current run sweep count.
	 */
	uint64_t sweep_count;

	/**
	 * Total amount of bytes transferred during the current or last active run
	 */
	uint32_t byte_count;

	/**
	 * Internal use for sweep RX callback, determines the number of blocks to process per transfer
	 */
	int blocks_per_xfer;

	/**
	 * Sample rate in Hz, used in FFT calculations and tuning the device
	 */
	uint64_t sample_rate_hz;

	/**
	 * Opaque write mutex used to wrap sensitive write accesses to the sweep state
	 */
	void *write_mutex;

	/**
	 * @ref hackrf_sweep_mutex_lock_fn
	 */
	hackrf_sweep_mutex_lock_fn mutex_lock;

	/**
	 * @ref hackrf_sweep_mutex_unlock_fn
	 */
	hackrf_sweep_mutex_unlock_fn mutex_unlock;
};

typedef struct hackrf_sweep_state hackrf_sweep_state_t;

/**
 * @ref hackrf_sweep_mutex_lock_fn
 */
static inline void mutex_write_lock(hackrf_sweep_state_t *state)
{
	if (state->write_mutex != NULL && state->mutex_lock != NULL) {
		state->mutex_lock(state->write_mutex);
	}
}

/**
 * @ref hackrf_sweep_mutex_unlock_fn
 */
static inline void mutex_write_unlock(hackrf_sweep_state_t *state)
{
	if (state->write_mutex != NULL && state->mutex_unlock != NULL) {
		state->mutex_unlock(state->write_mutex);
	}
}

/**
 * @ref hackrf_sweep_state
 */
static inline void hackrf_sweep_state_set(hackrf_sweep_state_t* state, unsigned int flag) {
    if (flag == SWEEP_STATE_STOPPED || flag == SWEEP_STATE_RUNNING) {
        state->flags &= ~(SWEEP_STATE_STOPPED | SWEEP_STATE_RUNNING);
        state->flags |= flag;
    } else if (flag == SWEEP_STATE_EXITING) {
        state->flags |= SWEEP_STATE_EXITING;
    }
}

static inline void set_state_condition(hackrf_sweep_state_t* state, unsigned int flag) {
    state->flags &= ~(SWEEP_STATE_INITIALIZED | SWEEP_STATE_RELEASED);
    state->flags |= flag;
}

static inline void set_sweep_started(hackrf_sweep_state_t* state) {
    state->flags |= SWEEP_STATE_SWEEP_STARTED;
}

static inline  void clear_sweep_started(hackrf_sweep_state_t* state) {
    state->flags &= ~SWEEP_STATE_SWEEP_STARTED;
}

static inline void set_sweep_finiteness(hackrf_sweep_state_t* state, unsigned int flag) {
    state->flags &= ~(SWEEP_STATE_ONESHOT | SWEEP_STATE_FINITE);
    state->flags |= flag;
}

static inline void set_sweep_flag(hackrf_sweep_state_t* state, unsigned int flag) {
    state->flags |= flag;
}

static inline void clear_sweep_flag(hackrf_sweep_state_t* state, unsigned int flag) {
    state->flags &= ~flag;
}

static inline int is_flag_set(const hackrf_sweep_state_t* state, unsigned int flag) {
    return state->flags & flag;
}

static inline const uint64_t hackrf_sweep_count(hackrf_sweep_state_t *state) {
	return state->sweep_count;
}

/**
 * Initialize sweep state
 *
 * @param device device to use for the sweep
 * @param state allocated state to initialize
 * @param sample_rate_hz sample rate in Hz
 * @param tune_step width of each tuning step in Hz
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 *
 */
extern ADDAPI int ADDCALL hackrf_sweep_init(hackrf_device *device,
	hackrf_sweep_state_t *state, uint64_t sample_rate_hz, uint32_t tune_step);

/**
 * Initialize sweep state with sane defaults
 *
 * @param device device to use for the sweep
 * @param state allocated state to initialize
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_easy_init(hackrf_device *device,
	hackrf_sweep_state_t *state);

/**
 * Configure the frequency range for sweeping
 * @ref hackrf_sweep_set_output must be called before, to account for output mode limitations
 * such as inverted FFT (limits operation to one single frequency range).
 *
 * @param state sweep state
 * @param frequency_list list of start-stop frequency pairs in MHz
 * @param range_count length of array @p frequency_list (in pairs, so total array length / 2!). Must be less than MAX_SWEEP_RANGES
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_set_range(hackrf_sweep_state_t *state,
	uint16_t frequency_list[],
	size_t range_count);

/**
 * Configure FFTW in the sweep state. This has no effect in transfer bypass mode
 *
 * @param state sweep state
 * @param plan_type FFTW plan type (ex. MEASURE, default)
 * @param requested_bin_width requested width of each FFT bin
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_setup_fft(hackrf_sweep_state_t *state,
int plan_type, uint32_t requested_bin_width);


/**
 * Stop any running sweeps
 *
 * @param state sweep state
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_stop(hackrf_sweep_state_t *state);

/**
 * Start sweeping
 *
 * @param state sweep state
 * @param max_sweeps zero for continuous sweep, any other number for a finite run of N sweeps
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_start(hackrf_sweep_state_t *state, int max_sweeps);

/**
 * Configure the output mode
 *
 * @param state sweep state
 * @param output_mode output mode @ref hackrf_sweep_output_mode_t (ex. text, or FFT binary, or inverted FFT binary)
 * @param output_type output target (ex. file)
 * @param arg argument for output handler (ex. FILE * pointer)
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_set_output(hackrf_sweep_state_t *state,
	hackrf_sweep_output_mode_t output_mode,
	hackrf_sweep_output_type_t output_type,
	void *arg);

/**
 * Configure the FFTW wisdom file
 *
 * @param state sweep state
 * @param path path to Wisdom file
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_import_wisdom(hackrf_sweep_state_t *state,
	const char *path);

/**
 * Export the FFTW wisdom file
 *
 * @param state sweep state
 * @param path path to Wisdom file (file or parent directory must be writable)
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_export_wisdom(hackrf_sweep_state_t *state,
	const char *path);

/**
 * Configure the user callback for the FFT bins
 *
 * @param state sweep state
 * @param fft_ready_cb callback for the FFT bins @ref hackrf_sweep_rx_cb_fn
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep_callbacks
 */
extern ADDAPI int ADDCALL hackrf_sweep_set_fft_rx_callback(hackrf_sweep_state_t *state,
	hackrf_sweep_rx_cb_fn fft_ready_cb);

/**
 * Configure the user callback for the raw transfer data
 *
 * @param state sweep state
 * @param transfer_ready_cb callback for the raw transfers @ref hackrf_sweep_sample_block_cb_fn
 * @param bypass if true, the FFT processing will be bypassed
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep_callbacks
 */
extern ADDAPI int ADDCALL hackrf_sweep_set_raw_sample_rx_callback(hackrf_sweep_state_t *state,
	hackrf_sweep_sample_block_cb_fn transfer_ready_cb, bool bypass);

/**
 * Set the sample rate in Hz for the sweep
 *
 * @param state sweep state
 * @param sample_rate_hz sample rate in Hz
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_set_sample_rate(hackrf_sweep_state_t *state,
	uint64_t sample_rate_hz);

/**
 * Set the number of blocks per transfer
 *
 * @param state sweep state
 * @param blocks_per_xfer number of blocks per transfer
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep_internal
 */
extern ADDAPI int ADDCALL hackrf_sweep_set_blocks_per_xfer(hackrf_sweep_state_t *state,
	int blocks_per_xfer);

/**
 * Configure the opaque write access mutex and its callbacks.
 *
 * @param state sweep state
 * @param mutex opaque mutex object
 * @param lock opaque locking API from user thread library
 * @param unlock opaque unlocking API from user thread library
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_set_write_mutex(hackrf_sweep_state_t *state,
	void *mutex,
	hackrf_sweep_mutex_lock_fn lock,
	hackrf_sweep_mutex_unlock_fn unlock);

/**
 * Close and release the sweep state
 * Any running sweeps will be stopped. The state will not be usable anympore.
 * If re-runs are intended, use @ref hackrf_sweep_stop.
 *
 * @param state sweep state
 * @return @ref HACKRF_SUCCESS on success or @ref hackrf_error variant
 * @ingroup sweep
 */
extern ADDAPI int ADDCALL hackrf_sweep_close(hackrf_sweep_state_t *state);

#endif /*__HACKRF_SWEEPER_H__*/
