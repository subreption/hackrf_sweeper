#!/usr/bin/env python
#
# Copyright 2024 Subreption LLC <research@subreption.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

import zmq
import msgpack
import argparse
import os
import stat
import sys
from zmq.utils import z85
from zmq.auth import load_certificate, create_certificates
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd  # Import pandas
import threading
import time
import matplotlib.ticker as ticker
from matplotlib.gridspec import GridSpec

data_lock = threading.Lock()
frequency_df = pd.DataFrame(columns=['last', 'min', 'max', 'timestamp'])
message_timestamps = []

def parse_args():
    parser = argparse.ArgumentParser(description='ZeroMQ Subscriber with CURVE encryption')
    parser.add_argument('-k', '--key-dir', required=True, help='Directory to store/load CURVE keys')
    parser.add_argument('-s', '--server-address', default='tcp://localhost:5555', help='ZeroMQ server address')
    parser.add_argument('-p', '--server-public-key', help='Path to server public key file (if not in key directory)')
    args = parser.parse_args()
    return args

def ensure_keys_exist(key_dir):
    client_public_key_file = os.path.join(key_dir, 'client.key')
    client_secret_key_file = os.path.join(key_dir, 'client.key_secret')
    server_public_key_file = os.path.join(key_dir, 'server.key')

    if not os.path.exists(key_dir):
        os.makedirs(key_dir, mode=0o700)

    # Check if client keys exist
    if not os.path.exists(client_public_key_file) or not os.path.exists(client_secret_key_file):
        # Generate client key pair using zmq.auth
        create_certificates(key_dir, "client")

        os.chmod(client_public_key_file, stat.S_IRUSR | stat.S_IWUSR)
        os.chmod(client_secret_key_file, stat.S_IRUSR | stat.S_IWUSR)

        print(f"Generated new client key pair in {key_dir}")
    else:
        print(f"Client key pair found in {key_dir}")

    return client_public_key_file, client_secret_key_file, server_public_key_file

def process_data(data):
    global frequency_df
    global spectrogram_df

    with data_lock:
        # Extract frequency ranges and power values
        freq_ranges = [
            (data['sec'], data['usec'], data['start'], data['end'], data['pwr']),
            (data['sec'], data['usec'], data['start2'], data['end2'], data['pwr2'])
        ]

        for time_sec, time_usec, start_freq, end_freq, power_values in freq_ranges:
            # Ensure start_freq <= end_freq
            if start_freq > end_freq:
                start_freq, end_freq = end_freq, start_freq

            timestamp = time_sec + time_usec / 1e6

            # Number of bins in this range
            num_bins = len(power_values)

            # Generate frequency bins
            freqs = np.linspace(start_freq, end_freq, num_bins, endpoint=False)

            # Convert frequencies to integer Hz
            freqs_int = freqs.astype(int)

            # Create a DataFrame from the frequencies and power values
            df_new = pd.DataFrame({
                'timestamp': timestamp,
                'frequency': freqs_int,
                'power': power_values
            })

            df_new.set_index('frequency', inplace=True)

            # Update the main DataFrame
            for freq in df_new.index:
                power = df_new.at[freq, 'power']

                if freq in frequency_df.index:
                    frequency_df.at[freq, 'last'] = power
                    frequency_df.at[freq, 'min'] = min(power, frequency_df.at[freq, 'min'])
                    frequency_df.at[freq, 'max'] = max(power, frequency_df.at[freq, 'max'])
                    frequency_df.at[freq, 'timestamp'] = timestamp
                else:
                    frequency_df.loc[freq] = {
                        'timestamp': timestamp,
                        'last': power,
                        'min': power,
                        'max': power
                    }

        # Ensure the index is sorted
        frequency_df.sort_index(inplace=True)

def get_spectrum_data():
    with data_lock:
        if frequency_df.empty:
            return None, None, None, None, None

        frequencies_int = frequency_df.index.values
        last_powers = frequency_df['last'].values
        min_powers = frequency_df['min'].values
        max_powers = frequency_df['max'].values
        timestamps = frequency_df['timestamp'].values

        # Convert frequencies to MHz for plotting
        frequencies_mhz = frequencies_int / 1e6

        return frequencies_mhz, last_powers, min_powers, max_powers, timestamps

def calculate_average_messages_per_second():
    with data_lock:
        current_time = time.time()
        window = 5
        while message_timestamps and message_timestamps[0] < current_time - window:
            message_timestamps.pop(0)
        num_messages = len(message_timestamps)
        average_mps = num_messages / window
        return average_mps

def zmq_subscriber(args, stop_event):
    key_dir = args.key_dir
    server_address = args.server_address
    server_public_key_path = args.server_public_key

    client_public_key_file, client_secret_key_file, default_server_public_key_file = ensure_keys_exist(key_dir)

    # Use the provided server public key path or the default
    if server_public_key_path:
        server_public_key_file = server_public_key_path
    else:
        server_public_key_file = default_server_public_key_file

    # Check if server public key file exists
    if not os.path.exists(server_public_key_file):
        print(f"Server public key file not found: {server_public_key_file}")
        sys.exit(1)

    # Load client keys
    client_public_key, client_secret_key = load_certificate(client_secret_key_file)
    server_public_key, _ = load_certificate(server_public_key_file)

    # Decode the Z85-encoded keys to binary
    client_public_key_bin = z85.decode(client_public_key)
    client_secret_key_bin = z85.decode(client_secret_key)
    server_public_key_bin = z85.decode(server_public_key)

    context = zmq.Context()

    subscriber = context.socket(zmq.SUB)

    subscriber.curve_publickey = client_public_key_bin
    subscriber.curve_secretkey = client_secret_key_bin
    subscriber.curve_serverkey = server_public_key_bin

    subscriber.subscribe("")
    subscriber.connect(server_address)

    print(f"Listening for data from {server_address} with CURVE encryption...")

    try:
        while not stop_event.is_set():
            if subscriber.poll(timeout=1000):
                message = subscriber.recv()

                unpacked_data = msgpack.unpackb(message, raw=False)

                process_data(unpacked_data)
                with data_lock:
                    message_timestamps.append(time.time())
            else:
                continue
    except KeyboardInterrupt:
        print("\nSubscriber interrupted by user.")
    finally:
        subscriber.close()
        context.term()
        print("Subscriber terminated.")

def main():
    args = parse_args()

    stop_event = threading.Event()

    # Start the ZeroMQ subscriber in a background thread
    subscriber_thread = threading.Thread(target=zmq_subscriber, args=(args, stop_event))
    subscriber_thread.start()

    fig = plt.figure(layout="constrained", figsize=(15, 10))
    gs = GridSpec(2, 2, figure=fig, width_ratios=[3, 1.5], height_ratios=[3, 1])

    graph_ax = fig.add_subplot(gs[0])
    tables_gs = gs[1].subgridspec(2, 2)
    peak_ax = fig.add_subplot(tables_gs[0])
    abspeak_ax = fig.add_subplot(tables_gs[1])

    subgraph_gs = tables_gs[2:].subgridspec(1, 1)
    subgraph_ax = fig.add_subplot(subgraph_gs[0])

    footer_gs = gs[2:].subgridspec(2, 1)
    footer_table_ax = fig.add_subplot(footer_gs[0])

    freq_label = "Freq (MHz)"
    mag_label = "Mag (dB)"

    try:
        while True:
            frequencies, last_powers, min_powers, max_powers, timestamps = get_spectrum_data()

            graph_ax.clear()

            peak_ax.clear()
            peak_ax.grid(False)
            peak_ax.axis('off')
            peak_ax.set_title("Peak (Last)")

            abspeak_ax.clear()
            abspeak_ax.grid(False)
            abspeak_ax.axis('off')
            abspeak_ax.set_title("Abs Peak (Max)")

            footer_table_ax.clear()
            footer_table_ax.grid(False)
            footer_table_ax.axis('off')

            subgraph_ax.clear()

            if frequencies is None:
                graph_ax.text(0.5, 0.5, "Waiting for incoming data...")
                abspeak_ax.text(0.5, 0.5, "...")
                peak_ax.text(0.5, 0.5, "...")

                # Let's teach important things while waiting for data...
                x = np.linspace(0, 10, 100)
                y = x

                subgraph_ax.plot(x, y, color='blue', linewidth=2)
                subgraph_ax.set_title("Linear relationship in FAFO", fontsize=16, fontweight='bold')
                subgraph_ax.set_xlabel("F'ing Around", fontsize=14)
                subgraph_ax.set_ylabel("Finding Out", fontsize=14)
                subgraph_ax.set_xlim(0, 10)
                subgraph_ax.set_ylim(0, 10)
                subgraph_ax.grid(True)

                footer_table_ax.text(0.5, 0.5, "No ZMQ frames received.")

                plt.draw()
                plt.pause(1)
                continue

            frequency_min, frequency_max = frequencies.min(), frequencies.max()

            if frequencies is not None:
                graph_ax.plot(frequencies, last_powers, label='Last', color='blue')
                graph_ax.plot(frequencies, min_powers, label='Minimum', color='green')
                graph_ax.plot(frequencies, max_powers, label='Maximum', color='red')
                graph_ax.set_xlabel(freq_label)
                graph_ax.set_ylabel(mag_label)
                graph_ax.set_title(f"RF Spectrum ({frequency_min}-{frequency_max})")
                plt.setp(graph_ax.get_xticklabels(), rotation=45, ha='right')
                graph_ax.grid(True)
                graph_ax.legend()

                # Set x-axis limits to data range
                graph_ax.set_xlim(frequency_min, frequency_max)

                # Adjust tick locators to prevent too many ticks
                freq_range = frequency_max - frequency_min
                major_tick = freq_range / 25
                minor_tick = major_tick / 10

                graph_ax.xaxis.set_major_locator(ticker.MultipleLocator(major_tick))
                graph_ax.xaxis.set_minor_locator(ticker.MultipleLocator(minor_tick))

                # Calculate average messages per second
                average_mps = calculate_average_messages_per_second()

                # Peak tracking variables
                peakN = 1000
                displayed_peak = 15

                top_peaks = None
                top_maxes = None

                with data_lock:
                    if not frequency_df.empty:
                        # Get the top N frequencies by 'last' power
                        top_peaks = frequency_df.nlargest(peakN, 'last')
                        top_maxes = frequency_df.nlargest(peakN, 'max')

                        last_peaks_table_data = []
                        for freq, row in top_peaks.iterrows():
                            freq_mhz = freq / 1e6
                            power = row['last']
                            last_peaks_table_data.append([f'{freq_mhz:.2f}', f'{power:.2f}'])

                        maxes_table_data = []
                        for freq, row in top_maxes.iterrows():
                            freq_mhz = freq / 1e6
                            power = row['max']
                            maxes_table_data.append([f'{freq_mhz:.2f}', f'{power:.2f}'])
                    else:
                        last_peaks_table_data = []
                        maxes_table_data = []

                headers = [ freq_label, mag_label ]

                last_peaks_table = peak_ax.table(cellText=last_peaks_table_data[:displayed_peak], colLabels=headers, loc='center')
                last_peaks_table.auto_set_font_size(False)
                last_peaks_table.set_fontsize(10)
                last_peaks_table.scale(1, 1.2)

                maxes_table = abspeak_ax.table(cellText=maxes_table_data[:displayed_peak], colLabels=headers, loc='center')
                maxes_table.auto_set_font_size(False)
                maxes_table.set_fontsize(10)
                maxes_table.scale(1, 1.2)

                info_table_data = [
                    ["Messages/s", average_mps ],
                    ["ZMQ Source", args.server_address ],
                    ["Nsamples", frequencies.size ]
                ]

                info_table = footer_table_ax.table(cellText=info_table_data, loc='center', cellLoc='left')
                info_table.auto_set_font_size(False)
                info_table.set_fontsize(10)
                info_table.auto_set_column_width([0, 1])
                info_table.scale(1, 2)

                subgraph_ax.clear()
                max_power = top_maxes['max'].values
                frequencies_mhz = top_maxes.index.values / 1e6

                scatter = subgraph_ax.scatter(frequencies_mhz, max_power, c=max_power,
                                              cmap='hot', marker='x', s=10, alpha=0.8)

                subgraph_ax.set_xlabel(freq_label)
                subgraph_ax.set_ylabel(mag_label)

                plt.draw()
                plt.pause(1)

            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nMain thread interrupted by user.")
    finally:
        stop_event.set()
        subscriber_thread.join()
        plt.ioff()
        plt.close(fig)

        print("Exiting...")

if __name__ == "__main__":
    main()
