/*
 * Copyright 2024 Subreption LLC <research@subreption.com>
 * Copyright 2016-2022 Great Scott Gadgets <info@greatscottgadgets.com>
 * Copyright 2016 Dominic Spill <dominicgs@gmail.com>
 * Copyright 2016 Mike Walters <mike@flomp.net>
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

/*
 * This is a complete refactoring and re-implementation of hackrf_sweep.c as
 * a library.
 */

#include <hackrf.h>
#include "hackrf_sweeper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <fftw3.h>
#include <inttypes.h>
#include <math.h>

#define _FILE_OFFSET_BITS 64

#ifndef bool
typedef int bool;
	#define true 1
	#define false 0
#endif

#ifdef _WIN32
	#define _USE_MATH_DEFINES
	#include <windows.h>
	#include <io.h>
	#ifdef _MSC_VER

		#ifdef _WIN64
typedef int64_t ssize_t;
		#else
typedef int32_t ssize_t;
		#endif

		#define strtoull _strtoui64
		#define snprintf _snprintf

static int gettimeofday(struct timeval* tv, void* ignored)
{
	FILETIME ft;
	unsigned __int64 tmp = 0;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmp |= ft.dwHighDateTime;
		tmp <<= 32;
		tmp |= ft.dwLowDateTime;
		tmp /= 10;
		tmp -= 11644473600000000Ui64;
		tv->tv_sec = (long) (tmp / 1000000UL);
		tv->tv_usec = (long) (tmp % 1000000UL);
	}
	return 0;
}

	#endif
#endif

#if defined(__GNUC__)
	#include <unistd.h>
	#include <sys/time.h>
#endif

#include <signal.h>
#include <math.h>

#define FD_BUFFER_SIZE (8 * 1024)

#if defined _WIN32
	#define m_sleep(a) Sleep((a))
#else
	#define m_sleep(a) usleep((a * 1000))
#endif


static const uint32_t hackrf_sweep_freq_min = FREQ_MIN_MHZ;
static const uint32_t hackrf_sweep_freq_max = FREQ_MAX_MHZ;

static inline float logPower(fftwf_complex in, float scale)
{
	float re = in[0] * scale;
	float im = in[1] * scale;
	float magsq = re * re + im * im;

	return (float) (log2(magsq) * 10.0f / log2(10.0f));
}

static void fft_output_record_string(hackrf_sweep_state_t *state, uint64_t frequency, FILE *fpout)
{
	int i;
	struct tm* fft_time;
	char time_str[50];
	hackrf_sweep_fft_ctx_t *fft = &state->fft;

	time_t time_stamp_seconds = state->usb_transfer_time.tv_sec;
	fft_time = localtime(&time_stamp_seconds);

	strftime(time_str, 50, "%Y-%m-%d, %H:%M:%S", fft_time);
	fprintf(fpout,
		"%s.%06ld, %" PRIu64 ", %" PRIu64 ", %.2f, %u",
		time_str,
		(long int) state->usb_transfer_time.tv_usec,
		(uint64_t) (frequency),
		(uint64_t) (frequency + state->sample_rate_hz  / 4),
		fft->bin_width,
		fft->size);

	for (i = 0; (fft->size / 4) > i; i++) {
		fprintf(fpout,
			", %.2f",
			fft->pwr[i + 1 + (fft->size * 5) / 8]);
	}

	fprintf(fpout, "\n");
	fprintf(fpout,
		"%s.%06ld, %" PRIu64 ", %" PRIu64 ", %.2f, %u",
		time_str,
		(long int) state->usb_transfer_time.tv_usec,
		(uint64_t) (frequency + (state->sample_rate_hz  / 2)),
		(uint64_t) (frequency + ((state->sample_rate_hz  * 3) / 4)),
		fft->bin_width,
		fft->size);

	for (i = 0; (fft->size / 4) > i; i++) {
		fprintf(fpout, ", %.2f", fft->pwr[i + 1 + (fft->size / 8)]);
	}

	fprintf(fpout, "\n");
}

static void fft_output_record_binary(hackrf_sweep_state_t *state, uint64_t frequency, FILE *fpout)
{
	uint64_t band_edge;
	uint32_t record_length;
	hackrf_sweep_fft_ctx_t *fft = &state->fft;

	record_length =
		2 * sizeof(band_edge) + (fft->size / 4) * sizeof(float);

	fwrite(&record_length, sizeof(record_length), 1, fpout);
	band_edge = frequency;
	fwrite(&band_edge, sizeof(band_edge), 1, fpout);
	band_edge = frequency + state->sample_rate_hz / 4;
	fwrite(&band_edge, sizeof(band_edge), 1, fpout);
	fwrite(&fft->pwr[1 + (fft->size * 5) / 8],
			sizeof(float),
			fft->size / 4,
			fpout);

	fwrite(&record_length, sizeof(record_length), 1, fpout);

	band_edge = frequency + state->sample_rate_hz  / 2;
	fwrite(&band_edge, sizeof(band_edge), 1, fpout);

	band_edge = frequency + (state->sample_rate_hz  * 3) / 4;
	fwrite(&band_edge, sizeof(band_edge), 1, fpout);

	fwrite(&fft->pwr[1 + fft->size / 8], sizeof(float), fft->size / 4, fpout);
}

int rx_callback(hackrf_transfer* transfer)
{
	int8_t* buf;
	uint8_t* ubuf;
	uint64_t frequency; /* in Hz */
	hackrf_sweep_state_t *state = (hackrf_sweep_state_t *) transfer->rx_ctx;
	hackrf_sweep_fft_ctx_t *fft = &state->fft;
	int i, j, ifft_bins, ret;

	if (state->ext_cb_sample_block != NULL) {
		ret = state->ext_cb_sample_block(state, transfer);
		/*
		 * Replicate the expected behavior: non-zero returns means the callback is not
		 * to be called again
		 */
		if (ret) {
			mutex_write_lock(state);
			state->ext_cb_sample_block = NULL;
			mutex_write_unlock(state);
		}
	}

	if ((state->output_type == HACKRF_SWEEP_OUTPUT_TYPE_FILE) &&
		state->output == NULL) {
		return -1;
	}

	if (state->flags & SWEEP_STATE_EXITING) {
		return 0;
	}

	// happens only once with timestamp normalized
	if ((state->usb_transfer_time.tv_sec == 0 && state->usb_transfer_time.tv_usec == 0) ||
	    !is_flag_set(state, SWEEP_STATE_NORMALIZED_TIMESTAMP)) {
		// set the timestamp for the first sweep
		gettimeofday(&state->usb_transfer_time, NULL);
	}

	state->byte_count += transfer->valid_length;

	buf = (int8_t*) transfer->buffer;
	ifft_bins = fft->size * state->step_count;

	for (j = 0; j < state->blocks_per_xfer; j++)
	{
		ubuf = (uint8_t*) buf;
		if (ubuf[0] == 0x7F && ubuf[1] == 0x7F) {
			frequency = ((uint64_t) (ubuf[9]) << 56) |
				((uint64_t) (ubuf[8]) << 48) |
				((uint64_t) (ubuf[7]) << 40) |
				((uint64_t) (ubuf[6]) << 32) |
				((uint64_t) (ubuf[5]) << 24) |
				((uint64_t) (ubuf[4]) << 16) |
				((uint64_t) (ubuf[3]) << 8) | ubuf[2];
		} else {
			buf += BYTES_PER_BLOCK;
			continue;
		}

		if (frequency == (uint64_t) (FREQ_ONE_MHZ * state->frequencies[0]))
		{
			if (is_flag_set(state, SWEEP_STATE_SWEEP_STARTED))
			{
				if (!is_flag_set(state, SWEEP_STATE_BYPASS_FFT) &&
					state->output_mode == HACKRF_SWEEP_OUTPUT_MODE_IFFT)
				{
					fftwf_execute(fft->plan_inverted);

					for (i = 0; i < ifft_bins; i++)
					{
						fft->ifftw_out[i][0] *= 1.0f / ifft_bins;
						fft->ifftw_out[i][1] *= 1.0f / ifft_bins;

						if (state->output_type == HACKRF_SWEEP_OUTPUT_TYPE_FILE) {
							fwrite(&fft->ifftw_out[i][0],
								sizeof(float),
								1,
								state->output);
							fwrite(&fft->ifftw_out[i][1],
								sizeof(float),
								1,
								state->output);
						}
					}
				}

				state->sweep_count++;

				if (is_flag_set(state, SWEEP_STATE_NORMALIZED_TIMESTAMP)) {
					// set the timestamp of the next sweep
					gettimeofday(&state->usb_transfer_time, NULL);
				}

				if (is_flag_set(state, SWEEP_STATE_ONESHOT)) {
					mutex_write_lock(state);
					hackrf_sweep_state_set(state, SWEEP_STATE_EXITING);
					mutex_write_unlock(state);
				} else if (is_flag_set(state, SWEEP_STATE_FINITE) &&
					state->sweep_count == state->max_sweeps) {
					mutex_write_lock(state);
					hackrf_sweep_state_set(state, SWEEP_STATE_EXITING);
					mutex_write_unlock(state);
				}
			}

			set_sweep_started(state);
		}

		if ((state->flags & SWEEP_STATE_EXITING) ||
			(state->flags & SWEEP_STATE_STOPPED)) {
			return 0;
		}

		if (!is_flag_set(state, SWEEP_STATE_SWEEP_STARTED)) {
			buf += BYTES_PER_BLOCK;
			continue;
		}
		if ((FREQ_MAX_MHZ * FREQ_ONE_MHZ) < frequency) {
			buf += BYTES_PER_BLOCK;
			continue;
		}

		/* Bypass FFT binning if enabled by user  */
		if (is_flag_set(state, SWEEP_STATE_BYPASS_FFT)) {
			continue;
		}

		/* copy to fftw_in as floats */
		buf += BYTES_PER_BLOCK - (fft->size * 2);

		for (i = 0; i < fft->size; i++) {
			fft->fftw_in[i][0] = buf[i * 2] * fft->window[i] * 1.0f / 128.0f;
			fft->fftw_in[i][1] = buf[i * 2 + 1] * fft->window[i] * 1.0f / 128.0f;
		}

		buf += fft->size * 2;
		fftwf_execute(fft->plan);

		for (i = 0; i < fft->size; i++) {
			fft->pwr[i] = logPower(fft->fftw_out[i], 1.0f / fft->size);
		}

		/* If the callback is set, pass the FFT bins to the user */
		if (state->ext_cb_fft_ready != NULL) {
			ret = state->ext_cb_fft_ready(state, frequency, transfer);
			if (ret) {
				/* callback doesn't want to be called again */
				mutex_write_lock(state);
				state->ext_cb_fft_ready = NULL;
				mutex_write_unlock(state);
			}
		}

		if (state->output_mode == HACKRF_SWEEP_OUTPUT_MODE_BINARY)
		{
			if (state->output_type == HACKRF_SWEEP_OUTPUT_TYPE_FILE) {
				fft_output_record_binary(state, frequency, state->output);
			}
		}
		else if (state->output_mode == HACKRF_SWEEP_OUTPUT_MODE_IFFT)
		{
			fft->ifft_idx = (uint32_t) round(
				(frequency - (uint64_t) (FREQ_ONE_MHZ * state->frequencies[0])) /
				fft->bin_width);

			fft->ifft_idx = (fft->ifft_idx + ifft_bins / 2) % ifft_bins;

			for (i = 0; (fft->size / 4) > i; i++)
			{
				fft->ifftw_in[fft->ifft_idx + i][0] =
					fft->fftw_out[i + 1 + (fft->size * 5) / 8][0];

				fft->ifftw_in[fft->ifft_idx + i][1] =
					fft->fftw_out[i + 1 + (fft->size * 5) / 8][1];
			}

			fft->ifft_idx += fft->size / 2;
			fft->ifft_idx %= ifft_bins;

			for (i = 0; (fft->size / 4) > i; i++)
			{
				fft->ifftw_in[fft->ifft_idx + i][0] =
					fft->fftw_out[i + 1 + (fft->size / 8)][0];

				fft->ifftw_in[fft->ifft_idx + i][1] =
					fft->fftw_out[i + 1 + (fft->size / 8)][1];
			}
		} else {
			if (state->output_type == HACKRF_SWEEP_OUTPUT_TYPE_FILE) {
				fft_output_record_string(state, frequency, state->output);
			}
		}
	}

	return 0;
}


int hackrf_sweep_setup_fft(hackrf_sweep_state_t *state, int plan_type, uint32_t requested_bin_width)
{
	int i;
	int err = HACKRF_SUCCESS;
	hackrf_sweep_fft_ctx_t *fft = (hackrf_sweep_fft_ctx_t *) &state->fft;

	if (requested_bin_width) {
		fft->size = state->sample_rate_hz  / requested_bin_width;
	} else {
		fft->size = 20;
	}

	/*
	 * The FFT bin width must be no more than a quarter of the sample rate
	 * for interleaved mode. With our fixed sample rate of 20 Msps, that
	 * results in a maximum bin width of 5000000 Hz.
	 */
	if (4 > fft->size) {
		return HACKRF_SWEEP_ERROR_INVALID_FFT_SIZE;
	}

	/*
	 * The maximum number of FFT bins we support is equal to the number of
	 * samples in a block. Each block consists of 16384 bytes minus 10
	 * bytes for the frequency header, leaving room for 8187 two-byte
	 * samples. As we pad fftSize up to the next odd multiple of four, this
	 * makes our maximum supported fftSize 8180.  With our fixed sample
	 * rate of 20 Msps, that results in a minimum bin width of 2445 Hz.
	 */
	if (8180 < fft->size) {
		return HACKRF_SWEEP_ERROR_INVALID_FFT_SIZE;
	}

	/* Account for default FFTW_MEASURE initialization */
	if (fft->plan_type != plan_type)
		fft->plan_type = plan_type;

	/* In interleaved mode, the FFT bin selection works best if the total
	 * number of FFT bins is equal to an odd multiple of four.
	 * (e.g. 4, 12, 20, 28, 36, . . .)
	 */
	while ((fft->size + 4) % 8) {
		fft->size++;
	}

	fft->bin_width = (double) state->sample_rate_hz  / fft->size;
	fft->fftw_in = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * fft->size);
	fft->fftw_out = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * fft->size);

	fft->plan =
		fftwf_plan_dft_1d(fft->size, fft->fftw_in, fft->fftw_out, FFTW_FORWARD, fft->plan_type);

	fft->pwr = (float*) fftwf_malloc(sizeof(float) * fft->size);
	fft->window = (float*) fftwf_malloc(sizeof(float) * fft->size);

	for (i = 0; i < fft->size; i++) {
		fft->window[i] = (float) (0.5f * (1.0f - cos(2 * M_PI * i / (fft->size - 1))));
	}

	/* Execute the plan once to make sure it's ready to go when real
	 * data starts to flow.  See issue #1366
	*/
	fftwf_execute(fft->plan);

	if (state->output_mode == HACKRF_SWEEP_OUTPUT_MODE_IFFT) {
		fft->ifftw_in = (fftwf_complex*) fftwf_malloc(
			sizeof(fftwf_complex) * fft->size * state->step_count);
		fft->ifftw_out = (fftwf_complex*) fftwf_malloc(
			sizeof(fftwf_complex) * fft->size * state->step_count);
		fft->plan_inverted = fftwf_plan_dft_1d(
			fft->size * state->step_count,
			fft->ifftw_in,
			fft->ifftw_out,
			FFTW_BACKWARD,
			fft->plan_type);

		/* Execute the plan once to make sure it's ready to go when real
	 	 * data starts to flow.  See issue hackrf #1366
		*/
		fftwf_execute(fft->plan_inverted);
	}

	return err;
}

static inline void set_default_range(hackrf_sweep_state_t *state)
{
	mutex_write_lock(state);

	state->num_ranges = 0;

	/* Sane defaults */
	state->frequencies[0] = (uint16_t) hackrf_sweep_freq_min;
	state->frequencies[1] = (uint16_t) hackrf_sweep_freq_max;
	state->num_ranges++;

	mutex_write_unlock(state);
}

int hackrf_sweep_init(hackrf_device *device,
	hackrf_sweep_state_t *state, uint64_t sample_rate_hz, uint32_t tune_step)
{
	if ((state->flags & SWEEP_STATE_INITIALIZED) != 0) {
		return HACKRF_ERROR_INVALID_PARAM;
	}

	memset(state->frequencies, 0, sizeof(state->frequencies));
	set_default_range(state);

	state->device = device;
	state->tune_step = TUNE_STEP;
	state->fft.plan_type = FFTW_MEASURE;

	/* Set internal transfer settings */
	state->blocks_per_xfer = BLOCKS_PER_TRANSFER;

	if (sample_rate_hz) {
		state->sample_rate_hz = sample_rate_hz;
	} else {
		state->sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
	}

	set_state_condition(state, SWEEP_STATE_INITIALIZED);
	clear_sweep_started(state);

	return HACKRF_SUCCESS;
}

int hackrf_sweep_easy_init(hackrf_device *device,
	hackrf_sweep_state_t *state)
{
	return hackrf_sweep_init(device, state, DEFAULT_SAMPLE_RATE_HZ, TUNE_STEP);
}

int hackrf_sweep_set_write_mutex(hackrf_sweep_state_t *state,
	void *mutex, hackrf_sweep_mutex_lock_fn lock,
	hackrf_sweep_mutex_unlock_fn unlock)
{
	if (state->write_mutex == NULL) {
		state->write_mutex = mutex;
		state->mutex_lock = lock;
		state->mutex_unlock = unlock;

		return HACKRF_SUCCESS;
	}

	return HACKRF_ERROR_INVALID_PARAM;
}

int hackrf_sweep_set_sample_rate(hackrf_sweep_state_t *state, uint64_t sample_rate_hz)
{
	/* TODO: changing sample rate needs recalculating fft size etc */
	return HACKRF_ERROR_OTHER;
}

int hackrf_sweep_set_blocks_per_xfer(hackrf_sweep_state_t *state, int blocks_per_xfer)
{
	/* XXX: changing this might not be worth exposing to the user */
	state->blocks_per_xfer = blocks_per_xfer;
	return HACKRF_SUCCESS;
}

int hackrf_sweep_set_range(hackrf_sweep_state_t *state,
	uint16_t frequency_list[],
	size_t range_count)
{
	unsigned int i;

	/* to be able to verify the IFFT range limit, output mode must be set explicitly first */
	if (!is_flag_set(state, SWEEP_STATE_OUTPUT_SET)) {
		return HACKRF_SWEEP_ERROR_NOT_READY;
	}

	if (0 == range_count) {
		set_default_range(state);
	} else {
		if ((state->output_mode ==  HACKRF_SWEEP_OUTPUT_MODE_IFFT) && (1 < range_count)) {
			return HACKRF_SWEEP_ERROR_INCOMPATIBLE_MODE;
		}

		if (range_count > MAX_SWEEP_RANGES) {
			return HACKRF_SWEEP_ERROR_INVALID_RANGE_COUNT;
		}
	}

	mutex_write_lock(state);
	memset(state->frequencies, 0, sizeof(state->frequencies));

	/* First step is copying the frequency list and validating it */
	for (i = 0; i < range_count; i++)
	{
		uint16_t freq_min = frequency_list[i];
		uint16_t freq_max = frequency_list[i+1];

		if (freq_min > freq_max  ||
			(freq_min > hackrf_sweep_freq_max || freq_min < hackrf_sweep_freq_min)) {
			mutex_write_unlock(state);
			return HACKRF_SWEEP_ERROR_INVALID_RANGE;
		}

		if (freq_max < freq_min  ||
			(freq_max > hackrf_sweep_freq_max || freq_max < hackrf_sweep_freq_min)) {
			mutex_write_unlock(state);
			return HACKRF_SWEEP_ERROR_INVALID_RANGE;
		}

		state->frequencies[i] = freq_min;
		state->frequencies[i + 1] = freq_max;
	}

	/*
	 * For each range, plan a whole number of tuning steps of a certain
	 * bandwidth. Increase high end of range if necessary to accommodate a
	 * whole number of steps, minimum 1.
	 */
	for (i = 0; i < state->num_ranges; i++) {
		state->step_count =
			1 + (state->frequencies[2 * i + 1] - state->frequencies[2 * i] - 1) / state->tune_step;
		state->frequencies[2 * i + 1] =
			(uint16_t) (state->frequencies[2 * i] + state->step_count * state->tune_step);
	}

	mutex_write_unlock(state);

	return HACKRF_SUCCESS;
}

int hackrf_sweep_start(hackrf_sweep_state_t *state, int max_sweeps)
{
	int err = HACKRF_SUCCESS;

	/* Stop the sweep(s) for this state (if any), to reapply any parameters */
	if (state->flags & SWEEP_STATE_RUNNING) {
		hackrf_sweep_state_set(state, SWEEP_STATE_STOPPED);
		clear_sweep_started(state);
	}

	mutex_write_lock(state);

	if (max_sweeps)
		state->max_sweeps = max_sweeps;

	/* reset counters */
	state->byte_count = 0;
	state->sweep_count = 0;

	mutex_write_unlock(state);

	err = hackrf_init_sweep(
		state->device,
		state->frequencies,
		state->num_ranges,
		BYTES_PER_BLOCK,
		state->tune_step * FREQ_ONE_MHZ,
		OFFSET,
		INTERLEAVED);
	if (err != HACKRF_SUCCESS) {
		return err;
	}

	mutex_write_lock(state);

	if (max_sweeps) {
		if (max_sweeps == 1) {
			set_sweep_finiteness(state, SWEEP_STATE_ONESHOT);
		} else {
			set_sweep_finiteness(state, SWEEP_STATE_FINITE);
		}
	}

	hackrf_sweep_state_set(state, SWEEP_STATE_RUNNING);

	mutex_write_unlock(state);

	err = hackrf_start_rx_sweep(state->device, rx_callback, (void *) state);
	if (err != HACKRF_SUCCESS) {
		state->flags &= ~SWEEP_STATE_RUNNING;
		return err;
	}

	return HACKRF_SUCCESS;
}

int hackrf_sweep_set_output(hackrf_sweep_state_t *state,
	hackrf_sweep_output_mode_t output_mode,
	hackrf_sweep_output_type_t output_type, void *arg)
{
	if (output_mode < HACKRF_SWEEP_OUTPUT_MODE_LAST)
	{
		state->output_mode = output_mode;

		if (output_type == HACKRF_SWEEP_OUTPUT_TYPE_NOP) {
			state->output_type = HACKRF_SWEEP_OUTPUT_TYPE_NOP;
			set_sweep_flag(state, SWEEP_STATE_OUTPUT_SET);
			return HACKRF_SUCCESS;

		} else if (output_type == HACKRF_SWEEP_OUTPUT_TYPE_FILE) {
			state->output = (FILE *) arg;
			state->output_type = HACKRF_SWEEP_OUTPUT_TYPE_FILE;
			set_sweep_flag(state, SWEEP_STATE_OUTPUT_SET);
			return HACKRF_SUCCESS;
		}
	}

	return HACKRF_ERROR_INVALID_PARAM;
}

int hackrf_sweep_set_fft_rx_callback(hackrf_sweep_state_t *state,
	hackrf_sweep_rx_cb_fn fft_ready_cb)
{
	mutex_write_lock(state);
	state->ext_cb_fft_ready = fft_ready_cb;
	mutex_write_unlock(state);

	return HACKRF_SUCCESS;
}

int hackrf_sweep_set_raw_sample_rx_callback(hackrf_sweep_state_t *state,
	hackrf_sweep_sample_block_cb_fn transfer_ready_cb, bool bypass)
{
	mutex_write_lock(state);

	state->ext_cb_sample_block = transfer_ready_cb;

	if (bypass) {
		set_sweep_flag(state, SWEEP_STATE_BYPASS_FFT);
	} else {
		clear_sweep_flag(state, SWEEP_STATE_BYPASS_FFT);
	}

	mutex_write_unlock(state);

	return HACKRF_SUCCESS;
}

int hackrf_sweep_stop(hackrf_sweep_state_t *state)
{
	mutex_write_lock(state);

	hackrf_sweep_state_set(state, SWEEP_STATE_EXITING);
	hackrf_sweep_state_set(state, SWEEP_STATE_STOPPED);

	/* reset counters */
	state->byte_count = 0;
	state->sweep_count = 0;

	mutex_write_unlock(state);

	return HACKRF_SUCCESS;
}

static void hackrf_sweep_free(hackrf_sweep_state_t *state)
{
	hackrf_sweep_fft_ctx_t *fft = (hackrf_sweep_fft_ctx_t *) &state->fft;

	if (fft != NULL)
	{
		if (fft->fftw_out != NULL) {
			fftwf_free(fft->fftw_out);
			fft->fftw_out = NULL;
		}

		if (fft->fftw_out != NULL) {
			fftwf_free(fft->fftw_out);
			fft->fftw_out = NULL;
		}

		if (fft->pwr != NULL) {
			fftwf_free(fft->pwr);
			fft->pwr = NULL;
		}

		if (fft->ifftw_out != NULL) {
			fftwf_free(fft->ifftw_out);
			fft->ifftw_out = NULL;
		}

		if (fft->ifftw_out != NULL) {
			fftwf_free(fft->ifftw_out);
			fft->ifftw_out = NULL;
		}

		if (fft->ifftw_out != NULL) {
			fftwf_free(fft->ifftw_out);
			fft->ifftw_out = NULL;
		}

		if (fft->plan != NULL) {
			fftwf_destroy_plan(fft->plan);
			fft->plan = NULL;
		}

		if (fft->plan_inverted != NULL) {
			fftwf_destroy_plan(fft->plan_inverted);
			fft->plan_inverted = NULL;
		}
	}

	state->mutex_lock = NULL;
	state->mutex_unlock = NULL;
	state->ext_cb_fft_ready = NULL;
	state->ext_cb_sample_block = NULL;
}

int hackrf_sweep_close(hackrf_sweep_state_t *state)
{
	int err = HACKRF_SUCCESS;

	err = hackrf_sweep_stop(state);
	if (err != HACKRF_SUCCESS) {
		return err;
	}

	hackrf_sweep_free(state);

	fftw_forget_wisdom();

	set_state_condition(state, SWEEP_STATE_RELEASED);

	return HACKRF_SUCCESS;
}

