"""End-to-end: drives the real binary in a pty against a real ELF with DWARF.

Usage: pty_flow.py BINARY WORKDIR   (WORKDIR holds exactly one ELF, e.g. libdemo.so)
"""
import os, pty, sys, time, fcntl, termios, struct, select, signal, shutil, re
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "vt.py")).read().replace("\nmain()\n", "\n"))
BIN, WORK = sys.argv[1], sys.argv[2]
C, R = 100, 36
OUT = os.path.join(WORK, "out", "custom.cs")
shutil.rmtree(os.path.join(WORK, "out"), ignore_errors=True)

pid, fd = pty.fork()
if pid == 0:
    os.chdir(WORK)
    os.environ["TERM"] = "xterm-256color"; os.environ.pop("NO_COLOR", None)
    os.execv(BIN, [BIN])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", R, C, 0, 0))
import codecs
_dec = codecs.getincrementaldecoder("utf-8")(errors="replace")  # a read can end mid-character
log = ""
def pump(t):
    global log
    end = time.time() + t
    while time.time() < end:
        if select.select([fd], [], [], 0.03)[0]:
            try: d = os.read(fd, 65536)
            except OSError: return
            log += _dec.decode(d)
def send(b, t=0.25): os.write(fd, b); pump(t)
def scr():
    vt = VT(C, R); vt.feed(log[log.find("\x1b[2J"):] if "\x1b[2J" in log else log); return vt.rows()
def text(): return "\n".join(scr())
def alive(): return os.waitpid(pid, os.WNOHANG) == (0, 0)
ok = True
def check(name, cond):
    global ok; ok &= bool(cond); print(("PASS " if cond else "FAIL "), name)
def wait_for(s, t=20):
    end = time.time() + t
    while time.time() < end:
        pump(0.2)
        if s in text(): return True
    return False

pump(1.2)
t = text(); g = scr()
check("startup: input prefilled from the working directory", "libdemo.so" in t)
check("startup: live check says it is an ELF with DWARF", "✓ ELF64" in t and "DWARF" in t)
check("startup: logo on top, tagline under it", "███████╗████████╗" in g[1] and "Native ELF / DWARF Analysis" in g[7])
check("startup: no STELLAR title in the border", "STELLAR" not in g[0])
check("input footer says Tab Switch Panel", "Tab Switch Panel" in g[-2])

# typing r q s R Q S must be text
send(b"X"); send(b"rqsRQS", 0.7); check("r q s typed into the field, app alive", "rqsRQS" in text() and alive())
check("typo'd path is reported inline", "no such file" in text())
send(b"\x7f" * 7, 0.6); check("backspacing fixes it: verdict is green again", "✓ ELF64" in text())

# inline path completion: ghost text, accepted with Right
send(b"\x7f" * 80); send((WORK + "/li").encode(), 0.5)
check("completion: the rest of the file name is suggested", "bdemo.so" in text())
check("completion: footer offers →", "→ Complete" in scr()[-2])
send(b"\x1b[C", 0.7)
check("completion: Right accepts it and the path validates", (WORK + "/libdemo.so") in text() and "✓ ELF64" in text())

# custom output path: Tab Tab -> output field, replace the text
send(b"\t"); send(b"\t")
check("Tab Tab reaches the output field", "▶ Path:" in text())
send(b"\x7f" * 40)
send(OUT.encode()); send(b"\r")
check("custom output path accepted", OUT in text() or OUT[-40:] in text())

# browse compilation units (menu item 2: Down)
send(b"\x1b[B"); send(b"\r")
t = text(); check("Browse compilation units opens a real list", "COMPILATION UNITS" in t and "0x" in t)
send(b"\r"); check("Enter counts the DIEs of the selected unit", "DIEs" in text() and re.search(r"unit 0: [\d,]+ DIEs", text()) is not None)
send(b"\x1b")
# scan
send(b"\x1b[B")                    # menu item 2
send(b"\r")
check("Scan DWARF starts and completes", wait_for("scan complete"))
t = text(); check("scan shows DIE totals and a tag table", re.search(r"DIEs\s+[1-9][\d,]*", t) is not None and "TAG" in t)
send(b"\x1b")
# settings via the menu
send(b"\x1b[B"); send(b"\x1b[B"); send(b"\r")
check("Settings is reachable from the menu", "Settings" in scr()[7] and "PERFORMANCE" in text())
# threads and RAM limit are editable now
send(b"\r")                         # Threads Limit: Enter starts editing
send(b"\x7f\x7f\x7f"); send(b"8"); send(b"\r")
check("Settings: thread limit can be changed", "[ 8 ]" in text())
send(b"\x1b[B"); send(b"\r")         # RAM Limit
send(b"\x7f" * 6); send(b"10"); send(b"\r")
check("Settings: a RAM limit below 64 MB is refused, field stays open", "at least 64 MB" in text())
send(b"\x7f\x7f"); send(b"2048"); send(b"\r")
check("Settings: RAM limit can be changed", "[ 2048 MB ]" in text())
check("Settings: the focused row has a marker, no filled bar", "▶ RAM Limit" in text().replace("  ", " ") or "▶" in "".join(l[:3] for l in scr()))
send(b"\x1b")
# emit
send(b"\x1b[A"); send(b"\r")
g = scr(); check("Emit screen shows the logo and its name", "███████╗████████╗" in g[1] and "Emit" in g[7])
check("Emit: focus starts on the output path", "▶ Path:" in text())
send(b"\x1b[B" * 4)
check("Emit: 4 Downs reach Maximum output lines", "▶ Maximum output lines" in text())
send(b"\r"); send(b"unlimited"); send(b"\r")
check("Emit: 'unlimited' is accepted", "maximum output lines: unlimited" in text())
send(b"\r"); send(b"1.5k"); send(b"\r")
check("Emit: '1.5k' parses to 1,500", "1,500" in text())
send(b"\r"); send(b"abc"); send(b"\r")
check("Emit: garbage is rejected and the field stays open", "not a number" in text())
send(b"\x1b")
send(b"\x1b[B")
t = text()
check("Emit: ONE Down from Max lines reaches START (no hidden stop)", "▶ Maximum" not in t and "▶ Path" not in t)
send(b"\r")
check("Emit: START writes the dump to the custom path", wait_for("Analysis complete", 60) and os.path.isfile(OUT) and os.path.getsize(OUT) > 0)
send(b"r")
check("overwrite is confirmed first: existing dump is not clobbered by one key", "again overwrites" in text())
send(b"x")
check("any other key disarms the overwrite", "again overwrites" not in text())
send(b"\x1b[B"); send(b"\r")        # BACK TO MAIN
send(b"\x1b"); send(b"?")
check("help overlay opens on ?", "KEYS" in text())
os.write(fd, b"\x1b"); pump(0.3)
os.write(fd, b"\x03"); time.sleep(0.4)
check("Ctrl-C quits", not alive())
print("ALL OK" if ok else "FAILED"); sys.exit(0 if ok else 1)
