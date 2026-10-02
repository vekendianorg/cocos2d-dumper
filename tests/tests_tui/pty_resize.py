"""Drives the real binary in a pty through a series of resizes and checks the settled screen each time."""
import os, pty, sys, time, fcntl, termios, struct, select, signal
exec(open(os.path.join(os.path.dirname(__file__), "vt.py")).read().replace("\nmain()\n", "\n"))
BIN = sys.argv[1] if len(sys.argv) > 1 else "/tmp/stellar"
SIZES = [(80,24),(120,30),(40,12),(20,5),(1,1),(61,14),(200,60),(30,10),(29,10),(80,9),(80,24)]

def setsize(fd, c, r): fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", r, c, 0, 0))
def drain(fd, t):
    out = b""; end = time.time() + t
    while time.time() < end:
        if select.select([fd], [], [], 0.05)[0]:
            try: d = os.read(fd, 65536)
            except OSError: break
            if not d: break
            out += d
    return out

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"; os.environ.pop("NO_COLOR", None)
    os.execv(BIN, [BIN]); 
setsize(fd, *SIZES[0]); os.kill(pid, signal.SIGWINCH)
bad = 0
for (c, r) in SIZES:
    setsize(fd, c, r); os.kill(pid, signal.SIGWINCH)
    data = drain(fd, 0.5).decode("utf-8", "replace")
    # a settled frame starts from the clear the app issues on resize; replay the whole tail after the last clear
    k = data.rfind("\x1b[2J"); tail = data[k:] if k >= 0 else data
    vt = VT(c, r); vt.feed(tail)
    errs = []
    g = vt.rows()
    if vt.scrolls: errs.append("scrolled")
    if c >= MIN_C and r >= MIN_R:
        if g[0][0] != "┌" or g[r-1][0] != "└": errs.append("corners")
        if any(g[y][0] not in "│├" for y in range(1, r-1)): errs.append("left border")
        if c > 2 and any(g[y][c-2] not in "│┤" for y in range(1, r-1)): errs.append("right border")
    elif not "".join(g).strip(): errs.append("blank")
    print(f"{c:>3}x{r:<3}", "OK " if not errs else "BAD", errs, "|", (g[0][:40] if c>=MIN_C else " ".join(x.strip() for x in g).strip()))
    bad += bool(errs)
os.write(fd, b"\x03"); time.sleep(0.3)  # Ctrl-C always quits, from any panel
try: _, st = os.waitpid(pid, os.WNOHANG)
except ChildProcessError: pass
print("FAILED" if bad else "ALL OK"); sys.exit(1 if bad else 0)
