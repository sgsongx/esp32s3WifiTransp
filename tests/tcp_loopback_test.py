"""Check a TCP-UART bridge with its UART TX and RX pins connected together."""
import argparse
import queue
import select
import socket
import sys
import threading
import time

PAYLOAD = bytes(range(256)) * 64
TIMEOUT = 10.0


def roundtrip(host, port, number):
    with socket.create_connection((host, port), timeout=TIMEOUT) as sock:
        deadline = time.monotonic() + TIMEOUT
        errors = queue.Queue()

        def transmit():
            try:
                sock.sendall(PAYLOAD)
            except Exception as error:
                errors.put(error)

        writer = threading.Thread(target=transmit)
        writer.start()
        try:
            received = bytearray()
            while len(received) < len(PAYLOAD):
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([sock], [], [], remaining)[0]:
                    raise TimeoutError("Timed out waiting for the UART loopback")
                block = sock.recv(min(4096, len(PAYLOAD) - len(received)))
                if not block:
                    raise ConnectionError("TCP closed before all bytes were returned")
                received.extend(block)
            writer.join(max(0, deadline - time.monotonic()))
            if writer.is_alive():
                raise TimeoutError("Timed out sending the test payload")
            if not errors.empty():
                raise errors.get()
            if received != PAYLOAD:
                raise AssertionError("Returned bytes do not match the test payload")
        finally:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            writer.join(TIMEOUT)
    print(f"PASS connection {number}: {len(PAYLOAD)} binary bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="ESP32 IP address")
    parser.add_argument("--port", type=int, default=3333)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in the range 1..65535")
    try:
        for number in (1, 2):
            roundtrip(args.host, args.port, number)
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
