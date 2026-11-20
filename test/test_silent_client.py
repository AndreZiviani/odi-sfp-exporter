# The accept loop against a client that connects and sends nothing: the
# built metricsd under qemu-user, a silent connection first, then a normal
# scrape. Before the request deadline the scrape waited for the silent client
# to hang up; now it is answered once the silent client is dropped. Then
# requests longer than one read, which must end in a FIN, not an RST.
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
    first = body
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

    # Requests that one read does not take. metricsd closed with the rest
    # unread, Linux sent an RST instead of a FIN, and the client saw
    # "connection reset by peer" (curl exit 56, a failed scrape for
    # Prometheus). A request sent in two segments goes the same way on a
    # stick, where the scrape takes seconds; here it is over before the
    # second segment comes, so only long requests are checked: one with a
    # header the size of a cookie, and one past the 8,703 bytes the first
    # version of the fix read (511 and then 8 reads of 1,024). The client
    # reads late, through a small receive buffer, so most of the answer is
    # still queued at metricsd when it closes, as over a slow link: an RST
    # then throws it away. The answer has to be the whole scrape, the same
    # metric families as above, and not only the headers, down to the last
    # sample: gpon_diag_sections_expected, with its value. The third request
    # is longer than the 64 KiB the drain read at most before; all of it has
    # arrived when metricsd hangs up, and it is read whole.
    def families(data):
        return [l for l in data.split(b"\r\n\r\n", 1)[-1].split(b"\n") if l.startswith(b"# TYPE ")]

    def last_sample(data):
        lines = [l for l in data.split(b"\r\n\r\n", 1)[-1].split(b"\n") if l]
        return lines[-1] if lines else b""

    want = families(first)
    tail = last_sample(first)
    if tail.startswith(b"gpon_diag_sections_expected ") and tail.split(b" ")[1].isdigit():
        print("ok    the scrape ends in its last sample, %s" % tail.decode())
    else:
        print("FAIL  the scrape does not end in gpon_diag_sections_expected: %r" % tail)
        fail = 1
    for pad in (600, 16000, 72000):
        req = b"GET /metrics HTTP/1.0\r\nX-Pad: " + b"x" * pad + b"\r\n\r\n"
        c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        c.settimeout(30)
        c.connect(("127.0.0.1", PORT))
        c.sendall(req)
        time.sleep(1)
        body = b""
        err = None
        try:
            while True:
                chunk = c.recv(65536)
                if not chunk:
                    break
                body += chunk
        except OSError as e:
            err = e
        c.close()
        if (err is None and body.startswith(b"HTTP/1.0 200 OK") and body.endswith(b"\n")
                and want and families(body) == want and last_sample(body) == tail):
            print("ok    a %d-byte request is answered in full, then a clean EOF (%d bytes)"
                  % (len(req), len(body)))
        else:
            print("FAIL  a %d-byte request: %d bytes, %d of %d families, %r"
                  % (len(req), len(body), len(families(body)), len(want), err))
            fail = 1
finally:
    d.kill()
    d.wait()
sys.exit(fail)
