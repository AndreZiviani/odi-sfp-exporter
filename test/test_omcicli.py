# metricsd against stand-ins for /bin/omcicli, under qemu-user: what one
# scrape reports for each way omcicli can answer `dump srvflow`. The
# stand-in is a host shell script (qemu-user hands execve to the host),
# written for this run and removed after it, so this runs only where no
# real omcicli is installed: inside the toolchain image.
#
# Run by `make omcicli-test`, inside the toolchain image.
import os
import signal
import socket
import subprocess
import sys
import time

PORT = 19101
BIN = sys.argv[1]
STUB = "/bin/omcicli"

if os.path.exists(STUB):
    print("FAIL  %s exists; run this in the toolchain image (make omcicli-test)" % STUB)
    sys.exit(1)

vendor = "".join("SERVID %d: Used: %d, (DIR=0, USFID=0, DSFID=0, SERVID=0, UNIMASK=0)\n"
                 % (i, i == 200) for i in range(256))
odi_two = ("SERVID 0: Used: 1, (DIR=1, USFID=0, DSFID=1, SERVID=0, UNIMASK=1)\n"
           "    rule_gen 1  stagMode 0  ctagMode 1  tag_count 1  out_tag (pri 0, vid 7)\n"
           "SERVID 3: Used: 1, (DIR=1, USFID=2, DSFID=3, SERVID=3, UNIMASK=1)\n"
           "    rule_gen 1  stagMode 0  ctagMode 1  tag_count 1  out_tag (pri 0, vid 8)\n"
           "2 services\n")

# (what, the shell for `dump srvflow`, gpon_omci_up, gpon_omci_services)
# None means the line is absent.
CASES = [
    ("the vendor dump, 18 KB, with the only row in use at 200",
     "cat <<'X'\n" + vendor + "X\n", "1", "1"),
    ("odi-oss omcid with two services", "cat <<'X'\n" + odi_two + "X\n", "1", "2"),
    ("omcid not started: omcicli's own error",
     "echo '% no command queue at key 0x800 -- is omci_app running? (-2)'; exit 1", "0", None),
    ("an omcicli that prints nothing", "exit 0", None, None),
    ("no answer within the bound", "exec sleep 10", "0", None),
    ("an answer, then the output closed and no exit",
     "cat <<'X'\n" + odi_two + "X\nexec >&- 2>&-\nexec sleep 10", "0", None),
    ("a row, then omcicli says the dump may be incomplete",
     "printf 'SERVID 0: Used: 0, (DIR=0, USFID=0, DSFID=0, SERVID=0, UNIMASK=0)\\n"
     "%% the daemon did not answer; the dump may be incomplete\\n'; exit 1", "0", None),
]


def scrape():
    c = socket.create_connection(("127.0.0.1", PORT), timeout=30)
    c.sendall(b"GET /metrics HTTP/1.0\r\n\r\n")
    data = b""
    while True:
        chunk = c.recv(65536)
        if not chunk:
            break
        data += chunk
    c.close()
    return data.split(b"\r\n\r\n", 1)[-1].decode()


def start(sigchld_ignored=False):
    # A launcher that ignores SIGCHLD makes the kernel reap metricsd's
    # children on its own: waitpid() then answers ECHILD for a child that
    # has exited.
    pre = (lambda: signal.signal(signal.SIGCHLD, signal.SIG_IGN)) if sigchld_ignored else None
    p = subprocess.Popen(["qemu-mips-static", BIN, str(PORT)], preexec_fn=pre,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            break
        except OSError:
            time.sleep(0.1)
    else:
        print("FAIL  metricsd did not listen")
        p.kill()
        p.wait()
        sys.exit(1)
    time.sleep(1)   # let the readiness probe connection be served first
    return p


def stub(srvflow):
    with open(STUB, "w") as f:
        f.write('#!/bin/sh\n[ "$1 $2" = "dump srvflow" ] || exit 0\n' + srvflow + "\n")
    os.chmod(STUB, 0o755)


def sample(body, name):
    for line in body.splitlines():
        if line.startswith(name + " "):
            return line.split(" ", 1)[1]
    return None


d = None
fail = 0
try:
    d = start()
    for what, srvflow, up, services in CASES:
        stub(srvflow)
        t0 = time.monotonic()
        body = scrape()
        took = time.monotonic() - t0
        got = (sample(body, "gpon_omci_up"), sample(body, "gpon_omci_services"))
        # The bound is 2 s for omcicli; qemu and the rest of the scrape add
        # some, a wait for the child's exit would add 8 more.
        if got == (up, services) and took < 6:
            print("ok    %s: up %s, services %s (%.1f s)" % (what, up, services, took))
        else:
            print("FAIL  %s: got up %s, services %s after %.1f s; want %s, %s"
                  % (what, got[0], got[1], took, up, services))
            fail = 1

    # The same answer from a metricsd whose children the kernel reaps: it
    # has exited, and the answer stands. It was taken for a child that would
    # not exit, waited out to the bound and reported as down.
    d.kill()
    d.wait()
    d = start(sigchld_ignored=True)
    stub("cat <<'X'\n" + odi_two + "X\n")
    t0 = time.monotonic()
    body = scrape()
    took = time.monotonic() - t0
    got = (sample(body, "gpon_omci_up"), sample(body, "gpon_omci_services"))
    if got == ("1", "2") and took < 1.5:
        print("ok    with SIGCHLD ignored by the launcher: up 1, services 2 (%.1f s)" % took)
    else:
        print("FAIL  with SIGCHLD ignored by the launcher: got up %s, services %s after %.1f s; want 1, 2"
              % (got[0], got[1], took))
        fail = 1
finally:
    if d:
        d.kill()
        d.wait()
    if os.path.exists(STUB):
        os.remove(STUB)
sys.exit(fail)
