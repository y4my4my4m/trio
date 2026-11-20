import pickle, sys
D=pickle.load(open('/tmp/grids.pkl','rb'))
def solid(a): return 0x33<=a<0x3E
def stand(a): return solid(a) or 1<=a<=0x0C or a==0x13
JUMP_UP=52; JUMP_GAP=96
def analyze(g,st,sx,sy,W,H):
    d=D[(g,st)]; scr=d['scr']; grid=d['grid']
    for y in range(sy,sy+H):
        for x in range(sx,sx+W):
            if y>31 or x>31 or not scr[y][x]: return None
    X0,Y0=sx*256,sy*256; G1,Y1=X0+W*256,Y0+H*256
    def at(x,y):
        if x<X0 or y<Y0 or x>=G1 or y>=Y1: return 0
        return grid.get((x>>4,y>>4),0)
    bad=0; empty=0; tot=0
    for y in range(Y0,Y1,16):
        for x in range(X0,G1,16):
            a=at(x,y); tot+=1
            if a in (0x0D,0x0E,0x3E,0x3F): bad+=1
            if a==0: empty+=1
    if bad: return None
    if empty/tot<0.45: return None
    plats=[]
    for y in range(Y0+32,Y1,16):
        run=None
        for x in range(X0,G1+16,16):
            ok= x<G1 and stand(at(x,y)) and at(x,y-16)==0 and at(x,y-32)==0
            if ok and run is None: run=x
            if not ok and run is not None:
                if x-run>=32: plats.append((y,run,x-1))
                run=None
    n=len(plats)
    if n<2: return None
    E=[set() for _ in range(n)]
    for a,(ay,ax0,ax1) in enumerate(plats):
        for b,(by,bx0,bx1) in enumerate(plats):
            if a==b: continue
            gap= bx0-ax1 if bx0>ax1 else (ax0-bx1 if ax0>bx1 else 0)
            up=ay-by
            if (0<up<=JUMP_UP and gap<=JUMP_GAP-up) or (-8<=up<=0 and 0<gap<=JUMP_GAP): E[a].add(b)
            elif up<-8 and ((bx0<=ax1+40 and bx1>=ax1+8) or (bx1>=ax0-40 and bx0<=ax0-8)): E[a].add(b)
        for x in range(ax0-16,ax1+17,16):
            for side in (-1,1):
                y=ay-16
                if at(x,y)!=0 or not solid(at(x+side*16,y)): continue
                while y>Y0 and at(x,y-16)==0 and solid(at(x+side*16,y-16)): y-=16
                top=y
                if ay-top<JUMP_UP+16: continue
                wx=x+side*16
                for b,(by,bx0,bx1) in enumerate(plats):
                    if b==a or by>=ay-JUMP_UP or by<top-48: continue
                    on_top= bx0<=wx+8 and bx1>=wx-8
                    beside= (bx1>=x-56 and bx1<wx) if side>0 else (bx0<=x+72 and bx0>wx)
                    if on_top or beside: E[a].add(b)
    # ladders: a ladder top (0x13) on platform B, its column of 0x12 down to a floor A
    for b,(by,bx0,bx1) in enumerate(plats):
        for x in range(bx0,bx1+1,16):
            if at(x,by)!=0x13: continue
            y=by+16
            while y<Y1 and at(x,y)==0x12: y+=16
            for a,(ay,ax0,ax1) in enumerate(plats):
                if a!=b and ay==y and ax0-8<=x<=ax1+8: E[a].add(b); E[b].add(a)
    # reachability matrix
    R=[]
    for a in range(n):
        seen={a}; st_=[a]
        while st_:
            u=st_.pop()
            for v in E[u]:
                if v not in seen: seen.add(v); st_.append(v)
        R.append(seen)
    L=[p[2]-p[1]+1 for p in plats]; total=sum(L)
    # largest strongly connected group (mutually reachable) by length
    best=0
    for a in range(n):
        grp=[b for b in R[a] if a in R[b]]
        best=max(best,sum(L[b] for b in grp))
    conn=best/total
    levels=len(set(p[0] for p in plats if p[2]-p[1]>=48))
    span=(max(p[0] for p in plats)-min(p[0] for p in plats))//16
    return conn, levels, span, empty/tot, n
res=[]
names={g:['G%d STAGE %d' % (g, n) for n in range(16)] for g in (1, 2, 3)}
for (g,st) in sorted(D):
    best=[]
    for W,H in ((3,2),(4,2),(3,3),(2,2),(4,1),(3,1)):
        for sy in range(32):
            for sx in range(32):
                r=analyze(g,st,sx,sy,W,H)
                if not r: continue
                conn,levels,span,empty,n=r
                if conn<0.9: continue
                s= levels*2 + min(span,24)*0.6 + empty*4 + W*H*0.8 + conn*5
                best.append((s,sx,sy,W,H,r))
    best.sort(reverse=True)
    keep=[]
    for b in best:
        if all(abs(b[1]-k[1])>=2 or abs(b[2]-k[2])>=2 for k in keep): keep.append(b)
        if len(keep)==2: break
    for k in keep:
        c,l,sp,e,n=k[5]
        print('  {"X%d %s", SNES_GAME_TRIO%d, %d, BOX(%d, %d, %d, %d)},  // score %.1f conn %.2f levels %d span %d empty %.2f plats %d'%(g,names[g][st],g,st,k[1],k[2],k[3],k[4],k[0],c,l,sp,e,n))
