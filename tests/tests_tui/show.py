import sys
sys.argv=[sys.argv[0]]+sys.argv[1:]
exec(open(__file__.replace("show.py","vt.py")).read().replace("\nmain()\n","\n"))
want=set(tuple(map(int,a.split("x")))+(int(b),) for a,b in (x.split(":") for x in sys.argv[1:]))
buf=sys.stdin.buffer
while True:
    h=buf.readline()
    if not h: break
    c,r,sid,nb=map(int,h.split()); d=buf.read(nb).decode()
    if (c,r,sid) in want:
        vt=VT(c,r); vt.feed(d)
        print(f"--- {SCREENS[sid]} {c}x{r}"); print("\n".join(x.rstrip() for x in vt.rows()))
