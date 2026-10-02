"""Tiny VT model (xterm semantics: pending-wrap, scroll, CUP/EL/ED) + layout invariants.

Usage: frame_dump MAXC MAXR | python3 vt.py
Checks every (cols,rows,screen) frame as a *real terminal of exactly that size* would show it.
"""
import re, sys, unicodedata

import os
AMBIG = set("✓✗▶↑↓—") if os.environ.get("AMBIG") else set()   # glyphs a fallback font draws double-width

def wcw(ch):
    if ch in AMBIG: return 2
    if unicodedata.combining(ch): return 0
    return 2 if unicodedata.east_asian_width(ch) in "WF" else 1

class VT:
    def __init__(s, cols, rows):
        s.c, s.r = cols, rows
        s.g = [[" "] * cols for _ in range(rows)]
        s.x = s.y = 0
        s.pend = False
        s.scrolls = 0
        s.max_param_err = 0
    def lf(s):
        if s.y == s.r - 1:
            s.g.pop(0); s.g.append([" "] * s.c); s.scrolls += 1
        else: s.y += 1
    def put(s, ch):
        w = wcw(ch)
        if w == 0: return
        if s.pend:
            s.x = 0; s.lf(); s.pend = False
        if s.x + w > s.c:       # wide glyph does not fit: wrap
            s.x = 0; s.lf()
        s.g[s.y][s.x] = ch
        if w == 2 and s.x + 1 < s.c: s.g[s.y][s.x + 1] = ""
        if s.x + w >= s.c: s.pend = True; s.x = s.c - 1
        else: s.x += w
    def feed(s, data):
        i, n = 0, len(data)
        while i < n:
            ch = data[i]
            if ch == "\x1b":
                m = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])").match(data, i)
                if not m: i += 1; continue
                p, f = m.group(1), m.group(2)
                a = [int(v) if v.isdigit() else 0 for v in p.replace("?", "").split(";")] if p else []
                if f == "H":
                    s.y = min(s.r - 1, max(0, (a[0] if a and a[0] else 1) - 1))
                    s.x = min(s.c - 1, max(0, (a[1] if len(a) > 1 and a[1] else 1) - 1)); s.pend = False
                elif f == "K":
                    for k in range(s.x, s.c): s.g[s.y][k] = " "
                    # EL does not clear pending wrap in xterm either
                elif f == "J":
                    mode = a[0] if a else 0
                    if mode == 2: s.g = [[" "] * s.c for _ in range(s.r)]
                i = m.end(); continue
            if ch == "\r": s.x = 0; s.pend = False
            elif ch == "\n": s.lf()
            elif ord(ch) >= 32: s.put(ch)
            i += 1
    def rows(s): return ["".join(r) for r in s.g]

SCREENS_TITLE = ["STELLAR", "STELLAR / EMIT", "STELLAR / ANALYSIS", "STELLAR / COMPLETE", "STELLAR / SETTINGS", "STELLAR / INFO"]
SCREENS = ["main", "emit", "analysis", "complete", "settings", "info"]
MIN_C, MIN_R = 30, 10   # below this the app must show the "too small" fallback

def check(c, r, sid, data):
    errs = []
    vt = VT(c, r); vt.feed(data)
    g = vt.rows()
    if vt.scrolls: errs.append(f"scrolled {vt.scrolls}x (frame taller than screen / wrapped)")
    if c >= MIN_C and r >= MIN_R:
        if g[0][0] != "┌": errs.append("top-left corner missing")
        if g[r-1][0] != "└": errs.append("bottom-left corner missing")
        if AMBIG:
            for y in range(1, r - 1):
                if g[y][0] not in "│├": errs.append(f"left border broken row {y}"); break
            return errs
        if c >= 3 and g[0][c-2] != "┐": errs.append("top-right corner missing")
        if c >= 3 and g[r-1][c-2] != "┘": errs.append("bottom-right corner missing")
        for y in range(1, r - 1):
            if g[y][0] not in "│├": errs.append(f"left border broken row {y}: {g[y][:6]!r}"); break
        for y in range(1, r - 1):
            if g[y][c-2] not in "│┤": errs.append(f"right border broken row {y}: {g[y][c-4:]!r}"); break
        if any(g[y][c-1].strip() for y in range(r)): errs.append("last column written")
    else:
        txt = " ".join(x.strip() for x in g).strip()
        if not txt: errs.append("tiny size: blank screen (no fallback message)")
    return errs

def main():
    buf = sys.stdin.buffer; bad = 0; total = 0; by = {}
    while True:
        h = buf.readline()
        if not h: break
        c, r, sid, nb = map(int, h.split())
        data = buf.read(nb).decode("utf-8")
        total += 1
        e = check(c, r, sid, data)
        if e:
            bad += 1
            by.setdefault((SCREENS[sid], e[0].split(" row")[0]), []).append((c, r))
    for (sc, msg), v in sorted(by.items()):
        print(f"{sc:9} {msg:48} x{len(v):4}  e.g. {v[:4]}")
    print(f"frames={total} failing={bad}")
    sys.exit(1 if bad else 0)
main()
