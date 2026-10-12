"""Crafted CAVLC stream whose parse depends on the neighbours' totalCoeff (nC), with I_PCM (nC 16), skip runs (nC 0),
I4x4/I16x16/P16x16 with random 0/1-coefficient blocks, random slice partitions (mid-row starts), several slices per picture.
usage: ncgen.py SEED W H FRAMES [plain|aso|fmo0|fmo1] > out.h264   (aso: the slices of a picture after the first in random order;
fmo0 / fmo1: two slice groups, interleaved runs / dispersed; the generator replicates the standard's nC rule: a desync in a decoder's
neighbour model changes the parse, so a decoder that differs from the standard cannot decode this cleanly)."""
import sys, random
seed, W, H, NF = (int(x) for x in sys.argv[1:5])
MODE = sys.argv[5] if len(sys.argv) > 5 else 'plain'   # plain | aso (slices of a picture in random order) | fmo0 (interleaved groups) | fmo1 (dispersed groups)
R = random.Random(seed)
N = W * H
class BW:
    def __init__(s): s.bits = []
    def u(s, n, v):
        for i in range(n - 1, -1, -1): s.bits.append((v >> i) & 1)
    def s_(s, st):
        for c in st: s.bits.append(int(c))
    def ue(s, v):
        v += 1; n = v.bit_length(); s.u(n - 1, 0); s.u(n, v)
    def se(s, v): s.ue(2 * v - 1 if v > 0 else -2 * v)
    def align0(s):
        while len(s.bits) % 8: s.bits.append(0)
    def trailing(s): s.bits.append(1); s.align0()
    def bytes(s): return bytes(int(''.join(map(str, s.bits[i:i + 8])), 2) for i in range(0, len(s.bits), 8))
def nal(ref_idc, typ, rbsp):
    out = bytearray(); z = 0
    for b in rbsp:
        if z >= 2 and b <= 3: out.append(3); z = 0
        out.append(b); z = z + 1 if b == 0 else 0
    return b'\x00\x00\x00\x01' + bytes([(ref_idc << 5) | typ]) + bytes(out)
def sps():
    b = BW(); b.u(8, 66); b.u(8, 0); b.u(8, 30); b.ue(0); b.ue(0); b.ue(2); b.ue(1); b.u(1, 0)
    b.ue(W - 1); b.ue(H - 1); b.u(1, 1); b.u(1, 1); b.u(1, 0); b.u(1, 0); b.trailing(); return nal(3, 7, b.bytes())
RUNS = [R.randint(1, 4), R.randint(1, 4)]
def gmap():
    if MODE == 'fmo1': return [((i % W) + (i // W)) % 2 for i in range(N)]
    m = []; i = 0
    while i < N:
        for g in range(2):
            for j in range(RUNS[g]):
                if i < N: m.append(g); i += 1
    return m
def pps():
    b = BW(); b.ue(0); b.ue(0); b.u(1, 0); b.u(1, 0)
    if MODE.startswith('fmo'):
        b.ue(1); b.ue(0 if MODE == 'fmo0' else 1)
        if MODE == 'fmo0':
            for g in range(2): b.ue(RUNS[g] - 1)
    else: b.ue(0)
    b.ue(0); b.ue(0); b.u(1, 0); b.u(2, 0)
    b.se(4); b.se(0); b.se(0); b.u(1, 1); b.u(1, 0); b.u(1, 0); b.trailing(); return nal(3, 8, b.bytes())
BLK = [(0,0),(1,0),(0,1),(1,1),(2,0),(3,0),(2,1),(3,1),(0,2),(1,2),(0,3),(1,3),(2,2),(3,2),(2,3),(3,3)]
POS = {p: i for i, p in enumerate(BLK)}
TOK = {  # (T1,TC) -> codes for nC classes 0..3
    (0,0): ['1','11','1111','000011'], (1,1): ['01','10','1110','000001']}
def nclass(n): return 0 if n < 2 else 1 if n < 4 else 2 if n < 8 else 3
tc = [[0]*16 for _ in range(N)]   # luma totalCoeff per MB (chroma never coded)
sl = [-1]*N                        # slice index of each MB (-1: not yet in the current picture)
def nC(mb, idx, cur, cursl):
    x, y = BLK[idx]; col = mb % W
    nA = nB = None
    if x > 0: nA = cur[POS[(x-1, y)]]
    elif col > 0 and sl[mb-1] == cursl: nA = tc[mb-1][POS[(3, y)]]
    if y > 0: nB = cur[POS[(x, y-1)]]
    elif mb >= W and sl[mb-W] == cursl: nB = tc[mb-W][POS[(x, 3)]]
    if nA is not None and nB is not None: return (nA + nB + 1) >> 1
    if nA is not None: return nA
    if nB is not None: return nB
    return 0
def block(b, mb, idx, cur, cursl, n, pdens):
    t = 1 if R.random() < pdens else 0
    code = TOK[(1, 1) if t else (0, 0)][nclass(nC(mb, idx, cur, cursl))]
    b.s_(code)
    if t: b.u(1, R.randint(0, 1)); b.s_('1')   # trailing-one sign, total_zeros = 0
    cur[idx] = t
def luma_cbp15(b, mb, cur, cursl, pdens):
    for i in range(16): block(b, mb, i, cur, cursl, 16, pdens)
def mb_data(b, mb, cursl, ptype, pdens):
    """append one non-skipped macroblock; returns the new tc list"""
    cur = [0]*16
    pi = 5 if ptype else 0
    k = R.choice(['pcm', 'i16', 'i4', 'p'] if ptype else ['pcm', 'i16', 'i4'])
    if k == 'pcm':
        b.ue(pi + 25); b.align0()
        for _ in range(384): b.u(8, R.randint(0, 255))
        cur = [16]*16
    elif k == 'i16':
        luma = R.randint(0, 1)
        b.ue(pi + 1 + 2 + 12*luma); b.ue(0); b.se(0)      # pred DC, chroma cbp 0; intra_chroma_pred_mode DC; qp delta 0
        dc = [0]
        block(b, mb, 0, dc, cursl, 16, pdens)             # Intra16x16DCLevel: nC of block 0 (its tc is not stored)
        if luma: luma_cbp15(b, mb, cur, cursl, pdens)
    elif k == 'i4':
        b.ue(pi + 0)
        for _ in range(16): b.u(1, 1)                     # prev_intra4x4_pred_mode_flag: predicted mode (DC)
        b.ue(0)                                           # intra_chroma_pred_mode DC
        cbp = R.randint(0, 1)
        b.ue(2 if cbp else 3)                             # coded_block_pattern me(v), intra: 15 -> 2, 0 -> 3
        if cbp: b.se(0); luma_cbp15(b, mb, cur, cursl, pdens)
    else:
        b.ue(0); b.se(R.randint(-3, 3)); b.se(R.randint(-3, 3))   # P_L0_16x16, one ref: no ref_idx
        cbp = R.randint(0, 1)
        b.ue(11 if cbp else 0)                            # inter cbp 15 -> 11, 0 -> 0
        if cbp: b.se(0); luma_cbp15(b, mb, cur, cursl, pdens)
    return cur
def partition():
    """list of slices, each the list of its MB addresses in decoding order"""
    if MODE.startswith('fmo'):
        gm = gmap(); out = []
        for g in range(2):
            mbs = [i for i in range(N) if gm[i] == g]
            if not mbs: continue
            k = min(len(mbs) - 1, R.randint(0, 2))
            cuts = [0] + sorted(R.sample(range(1, len(mbs)), k)) if k else [0]
            for i in range(len(cuts)):
                out.append(mbs[cuts[i]:(cuts[i+1] if i + 1 < len(cuts) else len(mbs))])
        return out
    cuts = sorted(R.sample(range(1, N), R.randint(0, min(5, N-1))))
    cuts = [0] + cuts
    return [list(range(cuts[i], (cuts[i+1] if i+1 < len(cuts) else N))) for i in range(len(cuts))]
out = sps() + pps()
for f in range(NF):
    idr = f == 0
    for s in range(N): sl[s] = -1
    pdens = R.choice([0.1, 0.5, 0.9])
    slices = []
    for si, mbs in enumerate(partition()):
        a = mbs[0]
        b = BW()
        b.ue(a); b.ue(2 if idr else 0); b.ue(0); b.u(4, f % 16)
        if idr: b.ue(0)
        if not idr: b.u(1, 0); b.u(1, 0)
        if idr: b.u(1, 0); b.u(1, 0)
        else: b.u(1, 0)
        b.se(R.randint(-2, 2)); idc = R.choice([0, 1, 2]); b.ue(idc)
        if idc != 1: b.se(0); b.se(0)
        skip = 0
        for mb in mbs:
            sl[mb] = si
            if not idr and R.random() < (0.5 if R.random() < 0.5 else 0.15):
                skip += 1; tc[mb] = [0]*16; continue
            if not idr: b.ue(skip); skip = 0
            tc[mb] = mb_data(b, mb, si, not idr, pdens)
        if not idr and skip: b.ue(skip)
        b.trailing()
        slices.append(nal(3, 5 if idr else 1, b.bytes()))
    if MODE == 'aso':   # the slice holding MB 0 stays first (vj_au_length starts an AU at first_mb 0); the others in random order
        rest = slices[1:]; R.shuffle(rest); slices = slices[:1] + rest
    out += b''.join(slices)
sys.stdout.buffer.write(out)
