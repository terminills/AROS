"""Exact-progress sends for the DUT bridge's timeout-polled socket.

The receive thread uses a short socket timeout to observe shutdown. sendall()
on that same socket can time out after sending an unknown prefix of a binary.
Use send() and retain each acknowledged byte count; a receive-poll timeout is
not a failed upload while the peer is still making progress.
"""
import socket
import time


def send_exact(conn, payload, stop, *, idle_timeout=60.0, clock=time.monotonic):
    """Send each byte once; abort after idle_timeout without progress or stop.

    This confirms only host socket writes, never the DUT's !ok/file commit.
    Leave the shared socket timeout unchanged for the receive thread.
    """
    view = memoryview(payload)
    sent = 0
    deadline = clock() + idle_timeout
    while sent < len(view):
        if stop.is_set():
            raise ConnectionAbortedError("NetShell session closed during send")
        if clock() >= deadline:
            raise TimeoutError(f"NetShell send stalled at {sent}/{len(view)} bytes")
        try:
            count = conn.send(view[sent:sent + 32768])
        except socket.timeout:
            continue
        if count <= 0:
            raise ConnectionError(f"NetShell disconnected at {sent}/{len(view)} bytes")
        sent += count
        deadline = clock() + idle_timeout
    return sent
