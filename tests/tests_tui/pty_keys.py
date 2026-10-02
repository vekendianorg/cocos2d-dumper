"""Types into the real binary: r/q/s must be text in the input field, shortcuts only after Tab."""
import os, pty, sys, time, fcntl, termios, struct, select, signal
exec(open(os.path.join(os.path.dirname(__file__), "vt.py")).read().replace("\nmain()\n", "\n"))
BIN = sys.argv[1] if len(sys.argv) > 1 else "/tmp/stellar"
C, R = 100, 34   # tall/wide enough for the banner
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"; os.environ.pop("NO_COLOR", None); os.execv(BIN, [BIN])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", R, C, 0, 0))
def drain(t=0.4):
    out = b""; end = time.time() + t
    while time.time() < end:
        if select.select([fd], [], [], 0.05)[0]:
            try: out += os.read(fd, 65536)
            except OSError: break
    return out.decode("utf-8", "replace")
def screen(data):
    vt = VT(C, R); k = data.rfind("\x1b[2J"); vt.feed(data[k:] if k >= 0 else data); return vt.rows()
def alive(): return os.waitpid(pid, os.WNOHANG) == (0, 0)
ok = True
def check(name, cond):
    global ok; ok &= bool(cond); print(("PASS " if cond else "FAIL "), name)

log = drain(1.0)
os.write(fd, b"/tmp/rqsRQS"); log += drain()
g = screen(log)
check("no STELLAR title in the border (banner says it)", "STELLAR" not in g[0])
row = next((l for l in g if "███████╗████████╗" in l), "")
check("ASCII logo centred", row and abs(row.index("█") + 57/2 - (C - 1)/2) <= 1.01)
check("r q s R Q S typed into the field", any("/tmp/rqsRQS" in l for l in g))
check("still on main screen, process alive", alive() and "INPUT" in "".join(g))
check("footer does not advertise Q/R while typing", "Run" in g[-2] and "Quit" in g[-2] and "Settings" not in g[-2])
os.write(fd, b"\t"); log += drain(); g = screen(log)
check("Tab moves to menu (footer shows shortcuts)", "Help" in g[-2])
os.write(fd, b"s"); log += drain(); g = screen(log)
check("S in the menu opens Settings", any("Settings" in l for l in g[:9]))
os.write(fd, b"\x1b"); log += drain(); os.write(fd, b"q"); time.sleep(0.4)
check("Q in the menu quits", not alive())
print("ALL OK" if ok else "FAILED"); sys.exit(0 if ok else 1)
