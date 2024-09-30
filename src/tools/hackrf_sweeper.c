/*
 * Copyright 2024 Subreption LLC <research@subreption.com>
 * Copyright 2016-2022 Great Scott Gadgets <info@greatscottgadgets.com>
 * Copyright 2016 Dominic Spill <dominicgs@gmail.com>
 * Copyright 2016 Mike Walters <mike@flomp.net>
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

#include <libhackrf/hackrf.h>
#include <hackrf_sweeper.h>

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

#define _FILE_OFFSET_BITS 64

#ifndef bool
typedef int bool;
	#define true 1
	#define false 0
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

FILE* outfile = NULL;

struct timeval time_start;
struct timeval t_start;

bool amp = false;
uint32_t amp_enable;

bool antenna = false;
uint32_t antenna_enable;

bool timestamp_normalized = false;
bool binary_output = false;
bool ifft_output = false;
bool one_shot = false;
bool finite_mode = false;
volatile bool sweep_started = false;
volatile bool do_exit = false;

static hackrf_sweep_state_t *sweep_state = NULL;

static float TimevalDiff(const struct timeval* a, const struct timeval* b)
{
	return (a->tv_sec - b->tv_sec) + 1e-6f * (a->tv_usec - b->tv_usec);
}

int parse_u32(char* s, uint32_t* const value)
{
	uint_fast8_t base = 10;
	char* s_end;
	uint64_t ulong_value;

	if (strlen(s) > 2) {
		if (s[0] == '0') {
			if ((s[1] == 'x') || (s[1] == 'X')) {
				base = 16;
				s += 2;
			} else if ((s[1] == 'b') || (s[1] == 'B')) {
				base = 2;
				s += 2;
			}
		}
	}

	s_end = s;
	ulong_value = strtoul(s, &s_end, base);
	if ((s != s_end) && (*s_end == 0)) {
		*value = (uint32_t) ulong_value;
		return HACKRF_SUCCESS;
	} else {
		return HACKRF_ERROR_INVALID_PARAM;
	}
}

int parse_u32_range(char* s, uint32_t* const value_min, uint32_t* const value_max)
{
	int result;

	char* sep = strchr(s, ':');
	if (!sep) {
		return HACKRF_ERROR_INVALID_PARAM;
	}

	*sep = 0;

	result = parse_u32(s, value_min);
	if (result != HACKRF_SUCCESS) {
		return result;
	}
	result = parse_u32(sep + 1, value_max);
	if (result != HACKRF_SUCCESS) {
		return result;
	}

	return HACKRF_SUCCESS;
}

static void usage()
{
	fprintf(stderr,
		"Usage:\n"
		"\t[-h] # this help\n"
		"\t[-d serial_number] # Serial number of desired HackRF\n"
		"\t[-a amp_enable] # RX RF amplifier 1=Enable, 0=Disable\n"
		"\t[-f freq_min:freq_max] # minimum and maximum frequencies in MHz\n"
		"\t[-p antenna_enable] # Antenna port power, 1=Enable, 0=Disable\n"
		"\t[-l gain_db] # RX LNA (IF) gain, 0-40dB, 8dB steps\n"
		"\t[-g gain_db] # RX VGA (baseband) gain, 0-62dB, 2dB steps\n"
		"\t[-w bin_width] # FFT bin width (frequency resolution) in Hz, 2445-5000000\n"
		"\t[-W wisdom_file] # Use FFTW wisdom file (will be created if necessary)\n"
		"\t[-P estimate|measure|patient|exhaustive] # FFTW plan type, default is 'measure'\n"
		"\t[-1] # one shot mode\n"
		"\t[-N num_sweeps] # Number of sweeps to perform\n"
		"\t[-B] # binary output\n"
		"\t[-I] # binary inverse FFT output\n"
		"\t[-n] # keep the same timestamp within a sweep\n"
		"\t-r filename # output file\n"
		"\n"
		"Output fields:\n"
		"\tdate, time, hz_low, hz_high, hz_bin_width, num_samples, dB, dB, . . .\n");
}

static hackrf_device* device = NULL;

#ifdef _MSC_VER
BOOL WINAPI sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Caught signal %d\n", signum);
		do_exit = true;
        hackrf_sweep_stop(sweep_state);
		return TRUE;
	}
	return FALSE;
}
#else
void sigint_callback_handler(int signum)
{
	fprintf(stderr, "Caught signal %d\n", signum);
	do_exit = true;
    hackrf_sweep_stop(sweep_state);
}
#endif

int import_wisdom(const char* path)
{
	// Returns nonzero
	if (!fftwf_import_wisdom_from_filename(path)) {
		fprintf(stderr,
			"Wisdom file %s not found; will attempt to create it\n",
			path);
		return 0;
	}

	return 1;
}

int import_default_wisdom()
{
	return fftwf_import_system_wisdom();
}

int export_wisdom(const char* path)
{
	if (path != NULL) {
		if (!fftwf_export_wisdom_to_filename(path)) {
			fprintf(stderr, "Could not write FFTW wisdom file to %s", path);
			return 0;
		}
	}

	return 1;
}

static void cleanup()
{
    if (sweep_state != NULL) {
        free(sweep_state);
        sweep_state = NULL;
    }
}

int main(int argc, char** argv)
{
	int opt, result = 0;
	const char* path = NULL;
	const char* serial_number = NULL;
	int exit_code = EXIT_SUCCESS;
	struct timeval time_now;
	struct timeval time_prev;
	float time_diff;
	float sweep_rate = 0;
	unsigned int lna_gain = 16, vga_gain = 20;
	uint32_t freq_min = 0;
	uint32_t freq_max = 6000;
	uint32_t requested_fft_bin_width = 0;
	const char* fftwWisdomPath = NULL;
	int fftw_plan_type = FFTW_MEASURE;
    int fftSize = 20;
    hackrf_sweep_output_mode_t output_mode = HACKRF_SWEEP_OUTPUT_MODE_TEXT;
    uint32_t num_sweeps = 0;
    int num_ranges = 0;
    uint16_t frequencies[MAX_SWEEP_RANGES * 2];

    memset(&frequencies, 0, sizeof(frequencies));

	while ((opt = getopt(argc, argv, "a:f:p:l:g:d:N:w:W:P:n1BIr:h?")) != EOF) {
		result = HACKRF_SUCCESS;
		switch (opt) {
		case 'd':
			serial_number = optarg;
			break;

		case 'a':
			amp = true;
			result = parse_u32(optarg, &amp_enable);
			break;

		case 'f':
			result = parse_u32_range(optarg, &freq_min, &freq_max);
			if (freq_min >= freq_max) {
				fprintf(stderr,
					"argument error: freq_max must be greater than freq_min.\n");
				usage();
                cleanup();
				return EXIT_FAILURE;
			}
			if (FREQ_MAX_MHZ < freq_max) {
				fprintf(stderr,
					"argument error: freq_max may not be higher than %u.\n",
					FREQ_MAX_MHZ);
				usage();
                cleanup();
				return EXIT_FAILURE;
			}
			if (MAX_SWEEP_RANGES <= num_ranges) {
				fprintf(stderr,
					"argument error: specify a maximum of %u frequency ranges.\n",
					MAX_SWEEP_RANGES);
				usage();
                cleanup();
				return EXIT_FAILURE;
			}
			frequencies[2 * num_ranges] = (uint16_t) freq_min;
			frequencies[2 * num_ranges + 1] = (uint16_t) freq_max;
			num_ranges++;
			break;

		case 'p':
			antenna = true;
			result = parse_u32(optarg, &antenna_enable);
			break;

		case 'l':
			result = parse_u32(optarg, &lna_gain);
			break;

		case 'g':
			result = parse_u32(optarg, &vga_gain);
			break;

		case 'N':
			finite_mode = true;
			result = parse_u32(optarg, &num_sweeps);
			break;

		case 'w':
			result = parse_u32(optarg, &requested_fft_bin_width);
            fftSize = DEFAULT_SAMPLE_RATE_HZ / requested_fft_bin_width;
            /*
             * The FFT bin width must be no more than a quarter of the sample rate
             * for interleaved mode. With our fixed sample rate of 20 Msps, that
             * results in a maximum bin width of 5000000 Hz.
             */
            if (4 > fftSize) {
                fprintf(stderr,
                    "argument error: FFT bin width (-w) must be no more than 5000000\n");
                return EXIT_FAILURE;
            }
            /*
             * The maximum number of FFT bins we support is equal to the number of
             * samples in a block. Each block consists of 16384 bytes minus 10
             * bytes for the frequency header, leaving room for 8187 two-byte
             * samples. As we pad fftSize up to the next odd multiple of four, this
             * makes our maximum supported fftSize 8180.  With our fixed sample
             * rate of 20 Msps, that results in a minimum bin width of 2445 Hz.
             */
            if (8180 < fftSize) {
                fprintf(stderr,
                    "argument error: FFT bin width (-w) must be no less than 2445\n");
                return EXIT_FAILURE;
            }
			break;

		case 'W':
			fftwWisdomPath = optarg;
			break;

		case 'P':
			if (strcmp("estimate", optarg) == 0) {
				fftw_plan_type = FFTW_ESTIMATE;
			} else if (strcmp("measure", optarg) == 0) {
				fftw_plan_type = FFTW_MEASURE;
			} else if (strcmp("patient", optarg) == 0) {
				fftw_plan_type = FFTW_PATIENT;
			} else if (strcmp("exhaustive", optarg) == 0) {
				fftw_plan_type = FFTW_EXHAUSTIVE;
			} else {
				fprintf(stderr, "Unknown FFTW plan type '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			break;

		case 'n':
			timestamp_normalized = true;
			break;

		case '1':
			one_shot = true;
			break;

        case 'T':
			output_mode = HACKRF_SWEEP_OUTPUT_MODE_TEXT;
            fprintf(stderr, "selected table (text) output");
			break;

		case 'B':
			output_mode = HACKRF_SWEEP_OUTPUT_MODE_BINARY;
            fprintf(stderr, "selected FFT (binary) output");
			break;

		case 'I':
			output_mode = HACKRF_SWEEP_OUTPUT_MODE_IFFT;
            fprintf(stderr, "selected inverted FFT (binary) output");
			break;

		case 'r':
			path = optarg;
			break;

		case 'h':
		case '?':
			usage();
			return EXIT_SUCCESS;

		default:
			fprintf(stderr, "unknown argument '-%c %s'\n", opt, optarg);
			usage();
			return EXIT_FAILURE;
		}

		if (result != HACKRF_SUCCESS) {
			fprintf(stderr,
				"argument error: '-%c %s' %s (%d)\n",
				opt,
				optarg,
				hackrf_error_name(result),
				result);
			usage();
			return EXIT_FAILURE;
		}
	}

	if (lna_gain % 8) {
		fprintf(stderr, "warning: lna_gain (-l) must be a multiple of 8\n");
	}

	if (vga_gain % 2) {
		fprintf(stderr, "warning: vga_gain (-g) must be a multiple of 2\n");
	}

	if (amp) {
		if (amp_enable > 1) {
			fprintf(stderr, "argument error: amp_enable shall be 0 or 1.\n");
			usage();
			return EXIT_FAILURE;
		}
	}

	if (antenna) {
		if (antenna_enable > 1) {
			fprintf(stderr,
				"argument error: antenna_enable shall be 0 or 1.\n");
			usage();
			return EXIT_FAILURE;
		}
	}

	if (0 == num_ranges) {
		frequencies[0] = (uint16_t) freq_min;
		frequencies[1] = (uint16_t) freq_max;
		num_ranges++;
	}

	if (output_mode == HACKRF_SWEEP_OUTPUT_MODE_IFFT && (1 < num_ranges)) {
		fprintf(stderr,
			"argument error: only one frequency range is supported in IFFT output (-I) mode.\n");
		return EXIT_FAILURE;
	}

#ifdef _MSC_VER
	if (binary_output) {
		_setmode(_fileno(stdout), _O_BINARY);
	}
#endif

	result = hackrf_init();
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_init() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		usage();
		return EXIT_FAILURE;
	}

	result = hackrf_open_by_serial(serial_number, &device);
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_open() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		usage();
		return EXIT_FAILURE;
	}

	if ((NULL == path) || (strcmp(path, "-") == 0)) {
		outfile = stdout;
	} else {
		outfile = fopen(path, "wb");
	}

	if (NULL == outfile) {
		fprintf(stderr, "Failed to open file: %s\n", path);
		return EXIT_FAILURE;
	}
	/* Change outfile buffer to have bigger one to store or read data on/to HDD */
	result = setvbuf(outfile, NULL, _IOFBF, FD_BUFFER_SIZE);
	if (result != 0) {
		fprintf(stderr, "setvbuf() failed: %d\n", result);
		usage();
		return EXIT_FAILURE;
	}

#ifdef _MSC_VER
	SetConsoleCtrlHandler((PHANDLER_ROUTINE) sighandler, TRUE);
#else
	signal(SIGINT, &sigint_callback_handler);
	signal(SIGILL, &sigint_callback_handler);
	signal(SIGFPE, &sigint_callback_handler);
	signal(SIGSEGV, &sigint_callback_handler);
	signal(SIGTERM, &sigint_callback_handler);
	signal(SIGABRT, &sigint_callback_handler);
#endif
	fprintf(stderr,
		"call hackrf_sample_rate_set(%.03f MHz)\n",
		((float) DEFAULT_SAMPLE_RATE_HZ / (float) FREQ_ONE_MHZ));
	result = hackrf_set_sample_rate_manual(device, DEFAULT_SAMPLE_RATE_HZ, 1);
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sample_rate_set() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		usage();
		return EXIT_FAILURE;
	}

	fprintf(stderr,
		"call hackrf_baseband_filter_bandwidth_set(%.03f MHz)\n",
		((float) DEFAULT_BASEBAND_FILTER_BANDWIDTH / (float) FREQ_ONE_MHZ));
	result = hackrf_set_baseband_filter_bandwidth(
		device,
		DEFAULT_BASEBAND_FILTER_BANDWIDTH);
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_baseband_filter_bandwidth_set() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		usage();
		return EXIT_FAILURE;
	}

	result = hackrf_set_vga_gain(device, vga_gain);
	result |= hackrf_set_lna_gain(device, lna_gain);

    sweep_state = malloc(sizeof(hackrf_sweep_state_t));
    if (sweep_state == NULL) {
        fprintf(stderr, "failed to allocate state.\n");
        return EXIT_FAILURE;
    }

    memset(sweep_state, 0, sizeof(hackrf_sweep_state_t));

    result = hackrf_sweep_easy_init(device, sweep_state);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_init() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

    if (timestamp_normalized) {
        set_sweep_flag(sweep_state, SWEEP_STATE_NORMALIZED_TIMESTAMP);
    }

    result = hackrf_sweep_set_output(sweep_state, output_mode,
        HACKRF_SWEEP_OUTPUT_TYPE_FILE, outfile);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_set_output() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

    result = hackrf_sweep_set_range(sweep_state, frequencies,
        num_ranges);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_set_range() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

	// Try to load a wisdom file if specified, otherwise
	// try to load the system-wide wisdom file
	if (fftwWisdomPath) {
		hackrf_sweep_import_wisdom(sweep_state, fftwWisdomPath);
	} else {
		hackrf_sweep_import_wisdom(sweep_state, NULL);
	}

    result = hackrf_sweep_setup_fft(sweep_state, fftw_plan_type,
        requested_fft_bin_width);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_setup_fft() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

    if (one_shot) {
        num_sweeps = 1;
    }

    /* finite mode does not need special handling,
     * as num_sweeps is used to determine finiteness.
     */
    result = hackrf_sweep_start(sweep_state, num_sweeps);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_start() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

	if (amp) {
		fprintf(stderr, "call hackrf_set_amp_enable(%u)\n", amp_enable);
		result = hackrf_set_amp_enable(device, (uint8_t) amp_enable);
		if (result != HACKRF_SUCCESS) {
			fprintf(stderr,
				"hackrf_set_amp_enable() failed: %s (%d)\n",
				hackrf_error_name(result),
				result);
			usage();
            cleanup();
			return EXIT_FAILURE;
		}
	}

	if (antenna) {
		fprintf(stderr, "call hackrf_set_antenna_enable(%u)\n", antenna_enable);
		result = hackrf_set_antenna_enable(device, (uint8_t) antenna_enable);
		if (result != HACKRF_SUCCESS) {
			fprintf(stderr,
				"hackrf_set_antenna_enable() failed: %s (%d)\n",
				hackrf_error_name(result),
				result);
			usage();
            cleanup();
			return EXIT_FAILURE;
		}
	}

	gettimeofday(&t_start, NULL);
	time_prev = t_start;

	fprintf(stderr, "Stop with Ctrl-C\n");
	while ((hackrf_is_streaming(device) == HACKRF_TRUE) && (do_exit == false)) {
		float time_difference;
        float data_rate_kbps;
		m_sleep(50);

		gettimeofday(&time_now, NULL);
		if (TimevalDiff(&time_now, &time_prev) >= 1.0f) {
			time_difference = TimevalDiff(&time_now, &t_start);
			sweep_rate = (float) hackrf_sweep_count(sweep_state) / time_difference;
            data_rate_kbps = (float) sweep_state->byte_count / 1024.0f;

			fprintf(stderr,
				"%" PRIu64
				" total sweeps completed, %.2f sweeps/second, %.2f KBytes/second\n",
				hackrf_sweep_count(sweep_state),
				sweep_rate,
				data_rate_kbps);

			if (sweep_state->byte_count == 0) {
				exit_code = EXIT_FAILURE;
				fprintf(stderr,
					"\nCouldn't transfer any data for one second.\n");
				break;
			}

			sweep_state->byte_count = 0;
			time_prev = time_now;
		}
	}

	fflush(outfile);
	result = hackrf_is_streaming(device);
	if (do_exit) {
		fprintf(stderr, "\nExiting...\n");
	} else {
		fprintf(stderr,
			"\nExiting... hackrf_is_streaming() result: %s (%d)\n",
			hackrf_error_name(result),
			result);
	}

	gettimeofday(&time_now, NULL);
	time_diff = TimevalDiff(&time_now, &t_start);
	if ((sweep_rate == 0) && (time_diff > 0)) {
		sweep_rate = hackrf_sweep_count(sweep_state) / time_diff;
	}

	fprintf(stderr,
		"Total sweeps: %" PRIu64 " in %.5f seconds (%.2f sweeps/second)\n",
		hackrf_sweep_count(sweep_state),
		time_diff,
		sweep_rate);

	if (device != NULL) {
		result = hackrf_close(device);
		if (result != HACKRF_SUCCESS) {
			fprintf(stderr,
				"hackrf_close() failed: %s (%d)\n",
				hackrf_error_name(result),
				result);
		} else {
			fprintf(stderr, "hackrf_close() done\n");
		}

		hackrf_exit();
		fprintf(stderr, "hackrf_exit() done\n");
	}

	fflush(outfile);
	if ((outfile != NULL) && (outfile != stdout)) {
		fclose(outfile);
		outfile = NULL;
		fprintf(stderr, "fclose() done\n");
	}

    hackrf_sweep_close(sweep_state);

	export_wisdom(fftwWisdomPath);
	fprintf(stderr, "exit\n");
	return exit_code;
}
