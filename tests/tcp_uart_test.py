"""Verify TCP-UART forwarding through a separate USB-TTL serial port (requires pyserial)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import select
import socket
import sys
import time

import serial

TIMEOUT = 10.0
TCP_PAYLOAD = bytes(range(256)) * 64
UART_PAYLOAD = bytes(reversed(range(256))) * 64


def receive(channel, expected, label):
    received = bytearray()
    deadline = time.monotonic() + TIMEOUT
    while len(received) < len(expected):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"{label}: received {len(received)}/{len(expected)} bytes")
        if isinstance(channel, socket.socket):
            if not select.select([channel], [], [], remaining)[0]:
                raise TimeoutError(f"{label}: received {len(received)}/{len(expected)} bytes")
            part = channel.recv(len(expected) - len(received))
            if not part:
                raise ConnectionError(f"{label}: TCP disconnected")
        else:
            part = channel.read(len(expected) - len(received))
        received.extend(part)
    if received != expected:
        raise AssertionError(f"{label}: received data differs from the transmitted bytes")


def transmit_uart(uart):
    count = uart.write(UART_PAYLOAD)
    if count != len(UART_PAYLOAD):
        raise IOError(f"UART write: {count}/{len(UART_PAYLOAD)} bytes")


def test_connection(host, port, uart, number):
    with socket.create_connection((host, port), timeout=TIMEOUT) as tcp:
        # UART RX is flushed after accept; wait until the bridge is ready to send UART data.
        marker = f"tcp-uart-sync-{number}\r\n".encode("ascii")
        tcp.sendall(marker)
        receive(uart, marker, "TCP->UART synchronization")
        start = time.monotonic()
        with ThreadPoolExecutor(max_workers=4) as pool:
            tasks = [
                pool.submit(receive, tcp, UART_PAYLOAD, "UART->TCP"),
                pool.submit(receive, uart, TCP_PAYLOAD, "TCP->UART"),
                pool.submit(tcp.sendall, TCP_PAYLOAD),
                pool.submit(transmit_uart, uart),
            ]
            for task in tasks:
                task.result(timeout=TIMEOUT)
        if uart.in_waiting:
            raise AssertionError("Unexpected extra UART bytes")
        if select.select([tcp], [], [], 0.1)[0]:
            raise AssertionError(f"Unexpected TCP bytes or disconnect: {tcp.recv(32)!r}")
        print(f"PASS connection {number}: TCP->UART {len(TCP_PAYLOAD)} bytes, "
              f"UART->TCP {len(UART_PAYLOAD)} bytes; simultaneous, "
              f"exact match, {time.monotonic() - start:.3f}s")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="ESP32 IP address")
    parser.add_argument("serial_port", help="USB-TTL data port, for example COM12")
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.baud <= 0:
        parser.error("port must be 1..65535 and baud must be positive")
    uart = serial.Serial(port=None, baudrate=args.baud, timeout=0.1, write_timeout=TIMEOUT)
    uart.dtr = False
    uart.rts = False
    uart.port = args.serial_port
    try:
        with uart:
            uart.reset_input_buffer()
            for number in (1, 2):
                test_connection(args.host, args.port, uart, number)
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
