/*
 * Copyright 2024 Subreption LLC <research@subreption.com>
 * Based off hackrf_sweep.c as re-implemented in hackrf_sweeper.c
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
 * WARNING: This program is not optimized. It is only a proof of concept showcasing
 * the FFT sampling collection callback usage in the context of a ZMQ publisher to
 * broadcast the data to an undetermined number of receivers. The queue, ring buffer
 * and msgpack routines have no optimizations and have not been implemented with
 * optimal performance in mind Do not use this code in production or commercial products.
 * If you make a fool of yourself, don't blame us :>
 */

#include <hackrf.h>
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

#include <pthread.h>
#include <czmq.h>
#include <msgpack.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>

#define QUEUE_SIZE 4096

typedef struct {
    void* buffer[QUEUE_SIZE];
    int head;
    int tail;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} ring_buffer_t;

typedef struct {
    hackrf_sweep_state_t *state;
    const char *zmq_connection_string;
    const char *server_secret_key_file;
    const char *key_directory;
} consumer_thread_args_t;

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
atomic_bool consumer_running = false;

#define MSGPACK_KEY(name, value) \
    const char *msgpack_key_##name = value; \
    const size_t msgpack_key_##name##_len = sizeof(value) - 1;

MSGPACK_KEY(time_sec, "sec")
MSGPACK_KEY(time_usec, "usec")
MSGPACK_KEY(binwidth, "binwidth")
MSGPACK_KEY(fftsize, "fftsize")
MSGPACK_KEY(sr, "sr")
MSGPACK_KEY(pwr, "pwr")
MSGPACK_KEY(start, "start")
MSGPACK_KEY(end, "end")
MSGPACK_KEY(start2, "start2")
MSGPACK_KEY(end2, "end2")
MSGPACK_KEY(pwr2, "pwr2")

static hackrf_sweep_state_t *sweep_state = NULL;
pthread_mutex_t sweep_state_write_lock;

void ring_buffer_init(ring_buffer_t* q) {
    q->head = 0;
    q->tail = 0;

    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void ring_buffer_destroy(ring_buffer_t* q) {
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);

    q->head = 0;
    q->tail = 0;
}

void ring_buffer_push(ring_buffer_t* q, void* data) {
    pthread_mutex_lock(&q->lock);
    while (((q->head + 1) % QUEUE_SIZE) == q->tail)
    {
        if (do_exit) {
            pthread_mutex_unlock(&q->lock);
            break;
        }

        pthread_cond_wait(&q->not_full, &q->lock);
    }
    q->buffer[q->head] = data;
    q->head = (q->head + 1) % QUEUE_SIZE;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}
void *ring_buffer_pop(ring_buffer_t* q) {
    pthread_mutex_lock(&q->lock);
    while (q->head == q->tail) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    void* data = q->buffer[q->tail];
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return data;
}

ring_buffer_t ring_buffer;

#ifdef DEBUG_OUTPUT
static void dump_fft(hackrf_sweep_state_t *state, uint64_t frequency)
{
	int i;
	struct tm* fft_time;
	char time_str[50];
	hackrf_sweep_fft_ctx_t *fft = &state->fft;

	time_t time_stamp_seconds = state->usb_transfer_time.tv_sec;
	fft_time = localtime(&time_stamp_seconds);

	strftime(time_str, 50, "%Y-%m-%d, %H:%M:%S", fft_time);
	fprintf(stderr,
		"%s.%06ld, %" PRIu64 ", %" PRIu64 ", %.2f, %u",
		time_str,
		(long int) state->usb_transfer_time.tv_usec,
		(uint64_t) (frequency),
		(uint64_t) (frequency + state->sample_rate_hz  / 4),
		fft->bin_width,
		fft->size);

	for (i = 0; (fft->size / 4) > i; i++) {
		fprintf(stderr,
			", %.2f",
			fft->pwr[i + 1 + (fft->size * 5) / 8]);
	}

	fprintf(stderr, "\n");
	fprintf(stderr,
		"%s.%06ld, %" PRIu64 ", %" PRIu64 ", %.2f, %u",
		time_str,
		(long int) state->usb_transfer_time.tv_usec,
		(uint64_t) (frequency + (state->sample_rate_hz  / 2)),
		(uint64_t) (frequency + ((state->sample_rate_hz  * 3) / 4)),
		fft->bin_width,
		fft->size);

	for (i = 0; (fft->size / 4) > i; i++) {
		fprintf(stderr, ", %.2f", fft->pwr[i + 1 + (fft->size / 8)]);
	}

	fprintf(stderr, "\n");
}
#endif

#define MSGPACK_PACK_KV(pk, key_name, pack_value_call) \
    do { \
        msgpack_pack_str(&(pk), msgpack_key_##key_name##_len); \
        msgpack_pack_str_body(&(pk), msgpack_key_##key_name, msgpack_key_##key_name##_len); \
        pack_value_call; \
    } while (0)

static int fft_bins_callback(void *p_state, uint64_t frequency, hackrf_transfer *transfer)
{
    hackrf_sweep_state_t *state = (hackrf_sweep_state_t *) p_state;
    msgpack_packer pk;
    msgpack_sbuffer *sbuf;

    sbuf = malloc(sizeof(msgpack_sbuffer));
    if (sbuf == NULL) {
        fprintf(stderr, "Failed to allocate msgpack_sbuffer\n");
        return 0;
    }

    msgpack_sbuffer_init(sbuf);
    msgpack_packer_init(&pk, sbuf, msgpack_sbuffer_write);

    msgpack_pack_map(&pk, 10);

    MSGPACK_PACK_KV(pk, time_sec, msgpack_pack_uint64(&pk, state->usb_transfer_time.tv_sec));
    MSGPACK_PACK_KV(pk, time_usec, msgpack_pack_uint64(&pk, state->usb_transfer_time.tv_usec));
    MSGPACK_PACK_KV(pk, binwidth, msgpack_pack_double(&pk, state->fft.bin_width));
    MSGPACK_PACK_KV(pk, fftsize, msgpack_pack_int32(&pk, state->fft.size));
    MSGPACK_PACK_KV(pk, start, msgpack_pack_uint64(&pk, frequency));
    MSGPACK_PACK_KV(pk, end, msgpack_pack_uint64(&pk, frequency + state->sample_rate_hz / 4));
    MSGPACK_PACK_KV(pk, pwr, {
        msgpack_pack_array(&pk, state->fft.size / 4);
        for (uint32_t i = 0; i < state->fft.size / 4; i++) {
            msgpack_pack_float(&pk, state->fft.pwr[1 + (state->fft.size * 5) / 8 + i]);
        }
    });

    MSGPACK_PACK_KV(pk, start2,
        msgpack_pack_uint64(&pk, frequency + state->sample_rate_hz / 2));

    MSGPACK_PACK_KV(pk, end2,
        msgpack_pack_uint64(&pk, frequency + (state->sample_rate_hz * 3) / 4));

    MSGPACK_PACK_KV(pk, pwr2, {
        msgpack_pack_array(&pk, state->fft.size / 4);
        for (uint32_t i = 0; i < state->fft.size / 4; i++) {
            msgpack_pack_float(&pk, state->fft.pwr[i + 1 + (state->fft.size / 8)]);
        }
    });

    #ifdef DEBUG_OUTPUT
        dump_fft(state, frequency);
    #endif

    ring_buffer_push(&ring_buffer, sbuf);

    return 0;
}

void *consumer_thread(void *arg)
{
    int ret = 0;
    consumer_thread_args_t *args = (consumer_thread_args_t *) arg;
    hackrf_sweep_state_t *state = args->state;
    const char *zmq_connection_string = args->zmq_connection_string;
    const char *server_secret_key_file = args->server_secret_key_file;
    zcert_t *server_cert = NULL;

    consumer_running = true;

    zsock_t *publisher = zsock_new(ZMQ_PUB);
    if (!publisher) {
        zsys_error("Failed to create ZeroMQ PUB socket");
        goto end;
    }

    if (server_secret_key_file != NULL && zsys_has_curve())
    {
        zsock_set_curve_server(publisher, true);

        server_cert = zcert_load(server_secret_key_file);
        if (server_cert == NULL)
        {
            zsys_error("Failed to read %s: %s", server_secret_key_file, zmq_strerror(zmq_errno()));
            zsys_info("Attempting to create a new certificate...");

            server_cert = zcert_new();
            if (server_cert == NULL) {
                zsys_error("Failed to create new certificate: %s", zmq_strerror(zmq_errno()));
                goto end;
            }

            zcert_set_meta(server_cert, "generator", "hackrf_sweeper_zmq");

            ret = zcert_save(server_cert, server_secret_key_file);
            if (ret == -1) {
                zsys_error("Failed to save certificate to %s: %s",
                    server_secret_key_file, zmq_strerror(zmq_errno()));
                goto end;
            }

        }

        zsys_info("Applying certificate...");
        zcert_apply (server_cert, publisher);
    }

    ret = zsock_bind(publisher, "%s", zmq_connection_string);
    if (ret == -1) {
        zsys_error("Failed to bind to %s: %s", zmq_connection_string, zmq_strerror(zmq_errno()));
        goto end;
    }

    while (1) {
        void *data = ring_buffer_pop(&ring_buffer);

        if (data == NULL) {
            zsys_error("Received NULL data from ring buffer");
            break;
        }

        msgpack_sbuffer *sbuf = (msgpack_sbuffer *) data;

        zmsg_t *msg = zmsg_new();
        if (!msg) {
            zsys_error("Failed to create ZeroMQ message");
            msgpack_sbuffer_destroy(sbuf);
            free(sbuf);
            continue;
        }

        zframe_t *frame = zframe_new(sbuf->data, sbuf->size);
        if (!frame) {
            zsys_error("Failed to create ZeroMQ frame");
            zmsg_destroy(&msg);
            msgpack_sbuffer_destroy(sbuf);
            free(sbuf);
            continue;
        }

        zmsg_append(msg, &frame);

        ret = zmsg_send(&msg, publisher);
        if (ret != 0) {
            zsys_error("Failed to send message: %s", zmq_strerror(zmq_errno()));
        }

        msgpack_sbuffer_destroy(sbuf);
        free(sbuf);

        if (is_flag_set(state, SWEEP_STATE_RELEASED)) {
            zsys_info("Sweep state released, exiting...");
            break;

        }
    }

end:
    if (publisher != NULL) {
        zsock_destroy(&publisher);
    }

    if (server_cert != NULL) {
        zcert_destroy(&server_cert);
    }

    consumer_running = false;
    hackrf_sweep_stop(state);

    return NULL;
}

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

static hackrf_device* device = NULL;

void sigint_callback_handler(int signum)
{
	fprintf(stderr, "Caught signal %d\n", signum);
	do_exit = true;
    hackrf_sweep_stop(sweep_state);
    ring_buffer_push(&ring_buffer, NULL);
}

static void cleanup()
{
    if (sweep_state != NULL) {
        free(sweep_state);
        sweep_state = NULL;
    }
}

static struct option long_options[] = {
    {"secret-key", required_argument, NULL, 'S'},
    {"connect", required_argument, NULL, 'C'},
    {"help", no_argument, NULL, 'h'},
    {0, 0, 0, 0}
};

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
		"\t[-n] # keep the same timestamp within a sweep\n"
        "\t[-1] # one shot mode\n"
		"\t[-N num_sweeps] # Number of sweeps to perform\n"
        "\n\nZMQ options:\n"
        "\t[-C str] # ZMQ connection string\n"
        "\n\nCURVE (encryption) options:\n"
        "\t[-S path] # Path to server secret key for CURVE encryption\n"
		"\n");
}

int main(int argc, char** argv)
{
    pthread_t consumer_tid;
	int opt, result = 0;
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
    int fftSize = 20;
    hackrf_sweep_output_mode_t output_mode = HACKRF_SWEEP_OUTPUT_MODE_TEXT;
    uint32_t num_sweeps = 0;
    int num_ranges = 0;
    uint16_t frequencies[MAX_SWEEP_RANGES * 2];
    consumer_thread_args_t consumer_args;
    const char *zmq_connection_string = "tcp://*:5555";
    const char *server_secret_key_file = NULL;
    const char* fftwWisdomPath = NULL;

    // Initialize the ring buffer
    ring_buffer_init(&ring_buffer);

    zsys_handler_set(NULL);

    memset(&frequencies, 0, sizeof(frequencies));

	while ((opt = getopt_long(argc, argv, "a:f:p:l:g:d:N:w:W:n1:C:S:h?", long_options, NULL)) != -1) {
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

		case 'n':
			timestamp_normalized = true;
			break;

		case '1':
			one_shot = true;
			break;

        case 'C':
            zmq_connection_string = optarg;
            break;

        case 'S':
            server_secret_key_file = optarg;
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

    result = hackrf_sweep_set_output(sweep_state, output_mode, HACKRF_SWEEP_OUTPUT_TYPE_NOP, NULL);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_set_output() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

    result = hackrf_sweep_set_fft_rx_callback(sweep_state, &fft_bins_callback);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_set_fft_rx_callback() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

    result = hackrf_sweep_set_range(sweep_state, frequencies, num_ranges);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_set_range() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

	if (fftwWisdomPath) {
		hackrf_sweep_import_wisdom(sweep_state, fftwWisdomPath);
	} else {
		hackrf_sweep_import_wisdom(sweep_state, NULL);
	}

    result = hackrf_sweep_setup_fft(sweep_state, FFTW_MEASURE, requested_fft_bin_width);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_setup_fft() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

    pthread_mutex_init(&sweep_state_write_lock, NULL);

    result = hackrf_sweep_set_write_mutex(sweep_state,
        &sweep_state_write_lock,
        (hackrf_sweep_mutex_lock_fn) pthread_mutex_lock,
        (hackrf_sweep_mutex_unlock_fn) pthread_mutex_unlock);
    if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_sweep_set_write_mutex() failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
        cleanup();
		return EXIT_FAILURE;
	}

    consumer_args.state = sweep_state;
    consumer_args.zmq_connection_string = zmq_connection_string;
    consumer_args.server_secret_key_file = server_secret_key_file;

    pthread_create(&consumer_tid, NULL, consumer_thread, &consumer_args);

    if (one_shot) {
        num_sweeps = 1;
    }

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
				" total sweeps completed, %.2f sweeps/second, %.2f KB/s\n",
				hackrf_sweep_count(sweep_state),
				sweep_rate,
				data_rate_kbps);

			if (sweep_state->byte_count == 0) {
				exit_code = EXIT_FAILURE;
				fprintf(stderr,
					"\nCouldn't transfer any data for one second.\n");
				break;
			}

            if (!consumer_running) {
                fprintf(stderr,
					"\nConsumer thread stopped, exiting.\n");
                break;
            }

			sweep_state->byte_count = 0;
			time_prev = time_now;
		}
	}

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
		}

		hackrf_exit();
	}

    if (consumer_running) {
        fprintf(stderr, "joining consumer thread\n");
        ring_buffer_push(&ring_buffer, NULL);
        pthread_join(consumer_tid, NULL);
    }

    fprintf(stderr, "closing sweep\n");
    hackrf_sweep_close(sweep_state);
    ring_buffer_destroy(&ring_buffer);

	return exit_code;
}
