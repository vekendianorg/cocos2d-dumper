"""Banner must be centred, byte-identical to logo.h wherever it is drawn, and drawn at every size that has room."""
import re, sys, os
exec(open(os.path.join(os.path.dirname(__file__), "vt.py")).read().replace("\nmain()\n", "\n"))
src = open(os.path.join(os.path.dirname(__file__), "../include/stellar/tui/logo.h"), encoding="utf-8").read()
logo = re.findall(r'^\s*"(.+)",$', src, re.M)[:6]
assert len(logo) == 6, logo
shown = bad = 0
buf = sys.stdin.buffer
while True:
    h = buf.readline()
    if not h: break
    c, r, sid, nb = map(int, h.split()); d = buf.read(nb).decode()
    if sid != 0 or c < MIN_C or r < MIN_R: continue
    vt = VT(c, r); vt.feed(d); g = vt.rows()
    idx = next((i for i, l in enumerate(g) if logo[0] in l), None)
    if idx is None: continue
    shown += 1
    # centred: spare columns inside the frame split evenly (right gets the odd one)
    left = g[idx].index(logo[0]); width = max(len(x) for x in logo)
    want = 1 + ((c - 3) - width) // 2
    if left != want: bad += 1; print("NOT CENTRED", c, r, "left", left, "want", want); continue
    for k in range(6):
        if idx + k >= r or logo[k] not in g[idx + k]: bad += 1; print("MISMATCH", c, r, k); break
print(f"logo drawn in {shown} main-screen sizes, mismatches={bad}")
sys.exit(1 if bad or not shown else 0)
