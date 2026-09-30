"""Deterministic send-progress tests; no sockets or DUT connection needed."""
import socket
import threading
import unittest
from netshell_transport import send_exact


class FakeSocket:
    def __init__(self, actions=()):
        self.actions = iter(actions)
        self.received = bytearray()
        self.max_chunk = 0

    def send(self, view):
        self.max_chunk = max(self.max_chunk, len(view))
        action = next(self.actions, len(view))
        if isinstance(action, Exception):
            raise action
        count = min(action, len(view))
        self.received.extend(view[:count])
        return count


class SendTests(unittest.TestCase):
    def test_partial_send_and_poll_timeouts_keep_exact_offset(self):
        peer = FakeSocket([2, socket.timeout(), 3, socket.timeout(), 1])
        payload = b"header\n\x00\xff\nbody"
        self.assertEqual(send_exact(peer, payload, threading.Event()), len(payload))
        self.assertEqual(peer.received, payload)

    def test_large_payload_is_chunked(self):
        peer = FakeSocket()
        payload = bytes(range(256)) * 4000
        send_exact(peer, payload, threading.Event())
        self.assertEqual(peer.received, payload)
        self.assertEqual(peer.max_chunk, 32768)

    def test_disconnect_is_not_success(self):
        peer = FakeSocket([3, 0])
        with self.assertRaises(ConnectionError):
            send_exact(peer, b"abcdef", threading.Event())
        self.assertEqual(peer.received, b"abc")

    def test_stall_has_finite_deadline(self):
        peer = FakeSocket([socket.timeout()] * 8)
        ticks = iter(range(10))
        with self.assertRaises(TimeoutError):
            send_exact(peer, b"abcdef", threading.Event(),
                       idle_timeout=3, clock=lambda: next(ticks))
        self.assertEqual(peer.received, b"")

    def test_shutdown_stops_send(self):
        stop = threading.Event()
        stop.set()
        peer = FakeSocket()
        with self.assertRaises(ConnectionAbortedError):
            send_exact(peer, b"abcdef", stop)
        self.assertEqual(peer.received, b"")


if __name__ == "__main__":
    unittest.main()
