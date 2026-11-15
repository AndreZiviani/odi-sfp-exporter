# The accept loop against a client that connects and sends nothing: the
# built metricsd under qemu-user, a silent connection first, then a normal
# scrape. Before the request deadline the scrape waited for the silent client
# to hang up; now it is answered once the silent client is dropped.
#
# Run by `make silent-client-test`, inside the toolchain image.
import socket
import subprocess
import sys
import time

PORT = 19100
BIN = sys.argv[1]

d = subprocess.Popen(["qemu-mips-static", BIN, str(PORT)],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fail = 0
try:
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            break
        except OSError:
            time.sleep(0.1)
    else:
        print("FAIL  metricsd did not listen")
        sys.exit(1)
    time.sleep(1)   # let the readiness probe connection be served first

    silent = socket.create_connection(("127.0.0.1", PORT))
    time.sleep(0.2)
    t0 = time.monotonic()
    s = socket.create_connection(("127.0.0.1", PORT), timeout=30)
    s.sendall(b"GET /metrics HTTP/1.0\r\n\r\n")
    body = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            body += chunk
    except socket.timeout:
        pass
    took = time.monotonic() - t0
    if body.startswith(b"HTTP/1.0 200 OK") and took < 15:
        print("ok    a scrape behind a silent client is answered (%.1f s)" % took)
    else:
        print("FAIL  a scrape behind a silent client: %d bytes after %.1f s" % (len(body), took))
        fail = 1

    silent.settimeout(1)
    try:
        got = silent.recv(1)
    except socket.timeout:
        got = None
    if got == b"":
        print("ok    and the silent client was closed unanswered")
    else:
        print("FAIL  the silent client was not closed: %r" % got)
        fail = 1
    silent.close()
finally:
    d.kill()
    d.wait()
sys.exit(fail)
