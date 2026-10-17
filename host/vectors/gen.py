"""Craft a tiny baseline stream (redundant_pic_cnt_present=1, deblocking control=1) that drives h264bsd into
IsEndOfPicture with two adjacent untouched MBs (double counting via failed redundant slices).
host/vectors: python3 gen.py attack > attack.h264; python3 gen.py attack2 > attack2.h264 (6x2 MBs, I_PCM IDR).
python3 gen.py elide > elide.h264: a redundant slice re-decodes, as a skip, an MB that its primary slice changed.
python3 gen.py slerr > slerr.h264: slices that fail mid-way, then re-decodes, around elidable skips.
python3 gen.py records > records.h264 (decode one NAL per call): slices rewrite MBs whose deblock records were made.
python3 gen.py idc2 6 4 > idc2.h264: two slices per picture with idc 2 and no redundant slices, so the deblock worker takes part.
python3 gen.py nonref 8 4 > nonref.h264: P pictures alternate reference / non-reference (hold: the spare buffer, skip elision)."""
import sys

class BW:
    def __init__(s): s.bits = []
    def u(s, n, v):
        for i in range(n - 1, -1, -1): s.bits.append((v >> i) & 1)
    def ue(s, v):
        v += 1; n = v.bit_length()
        s.u(n - 1, 0); s.u(n, v)
    def se(s, v): s.ue(2 * v - 1 if v > 0 else -2 * v)
    def align0(s):
        while len(s.bits) % 8: s.bits.append(0)
    def trailing(s):
        s.bits.append(1); s.align0()
    def bytes(s):
        assert len(s.bits) % 8 == 0
        return bytes(int(''.join(map(str, s.bits[i:i + 8])), 2) for i in range(0, len(s.bits), 8))

def nal(ref_idc, typ, rbsp):
    out = bytearray(); z = 0
    for b in rbsp:
        if z >= 2 and b <= 3: out.append(3); z = 0
        out.append(b); z = z + 1 if b == 0 else 0
    return b'\x00\x00\x00\x01' + bytes([(ref_idc << 5) | typ]) + bytes(out)

W, H = int(sys.argv[2]) if len(sys.argv) > 2 else 6, int(sys.argv[3]) if len(sys.argv) > 3 else 2
N = W * H
QP = 36

def sps():
    b = BW(); b.u(8, 66); b.u(8, 0); b.u(8, 30); b.ue(0)
    b.ue(0)            # log2_max_frame_num_minus4 -> 4 bits
    b.ue(2)            # poc type 2
    b.ue(1)            # num_ref_frames
    b.u(1, 0)          # gaps
    b.ue(W - 1); b.ue(H - 1)
    b.u(1, 1); b.u(1, 1); b.u(1, 0); b.u(1, 0)
    b.trailing(); return nal(3, 7, b.bytes())

def pps(redundant=1):
    b = BW(); b.ue(0); b.ue(0); b.u(1, 0); b.u(1, 0); b.ue(0)
    b.ue(0); b.ue(0); b.u(1, 0); b.u(2, 0)
    b.se(QP - 26); b.se(0); b.se(0)
    b.u(1, 1)          # deblocking_filter_control_present
    b.u(1, 0)          # constrained intra
    b.u(1, redundant)  # redundant_pic_cnt_present
    b.trailing(); return nal(3, 8, b.bytes())

def hdr(b, first, stype, fn, idr, rpc, idc, redundant=1, ref=True):
    b.ue(first); b.ue(stype); b.ue(0); b.u(4, fn)
    if idr: b.ue(0)
    if redundant: b.ue(rpc)
    if stype == 0:
        b.u(1, 0)      # num_ref_idx_active_override
        b.u(1, 0)      # ref_pic_list_reordering_flag_l0
    if idr: b.u(1, 0); b.u(1, 0)
    elif ref: b.u(1, 0) # adaptive_ref_pic_marking_mode_flag (dec_ref_pic_marking: nal_ref_idc != 0 only)
    b.se(0)            # slice_qp_delta
    b.ue(idc)
    if idc != 1: b.se(0); b.se(0)

def idr_slice(luma, redundant=1):
    b = BW(); hdr(b, 0, 2, 0, True, 0, 0, redundant)
    for m in range(N):
        b.ue(25); b.align0()
        for _ in range(256): b.u(8, luma[m])
        for _ in range(128): b.u(8, 128)
    b.trailing(); return nal(3, 5, b.bytes())

def p_slice(first, fn, skip, rpc=0, idc=0, bad_after=False, redundant=1, ref=2):
    b = BW(); hdr(b, first, 0, fn, False, rpc, idc, redundant, ref != 0); b.ue(skip)
    if bad_after: b.ue(31)             # invalid P mb_type -> macroblock_layer error
    b.trailing(); return nal(ref, 1, b.bytes())

def p_pcm_slice(first, fn, luma, skip, rpc=0, idc=0, bad_after=False, redundant=1, ref=2):
    b = BW(); hdr(b, first, 0, fn, False, rpc, idc, redundant, ref != 0); b.ue(0)
    b.ue(30); b.align0()               # I_PCM in a P slice: mb_type 5 + 25
    for _ in range(256): b.u(8, luma)
    for _ in range(128): b.u(8, 128)
    b.ue(skip)
    if bad_after: b.ue(31)             # invalid P mb_type -> macroblock_layer error
    b.trailing(); return nal(ref, 1, b.bytes())

mode = sys.argv[1]
luma = [100 if (m % W) < 3 else 110 for m in range(N)]   # step between MB 2 and MB 3 of each row
plain_pps = mode in ('idc2', 'nonref')
s = sps() + pps(0 if plain_pps else 1) + idr_slice(luma, 0 if plain_pps else 1)
if mode == 'nonref':
    # P pictures alternate reference / non-reference (nal_ref_idc 0; frame_num as the standard numbers them, POC type 2
    # never has two non-reference pictures in a row), no redundant slices. Mostly skips, so skip elision runs; an I_PCM MB
    # (at MB m: skip m, then a slice from m) changes now and then. A non-reference picture is still the last frame
    # returned when the next picture starts, in the buffer that picture would reuse: with hold on, the spare takes over.
    for fn, ref, pcm in [(1, 2, None), (2, 0, (0, 200)), (2, 2, None), (3, 0, None), (3, 2, (5, 60)), (4, 0, None),
                         (4, 2, None), (5, 0, (N - 1, 30)), (5, 2, (9, 180)), (6, 0, None), (6, 2, None), (7, 0, (3, 90)),
                         (7, 2, None), (8, 0, None)]:
        if pcm is None:
            s += p_slice(0, fn, N, redundant=0, ref=ref)
            continue
        m, lum = pcm
        if m:
            s += p_slice(0, fn, m, redundant=0, ref=ref)
        s += p_pcm_slice(m, fn, lum, N - m - 1, redundant=0, ref=ref)
    sys.stdout.buffer.write(s)
    sys.exit()
if mode == 'idc2':
    # python3 gen.py idc2 6 4 > idc2.h264: 6x4 MBs, no redundant slices (so the deblock worker takes part), every picture
    # in two slices [0,12) [12,24) with disable_deblocking_filter_idc 2 (no filtering across the slice boundary, filtering
    # inside each slice). Each slice puts one I_PCM MB in front of skips: MB 0 and MB 12 differ from their neighbours, so
    # real edges are filtered inside slices and the boundary MB 12 / MB 6 is not.
    h = N // 2
    for fn, (a, b2) in enumerate([(90, 140), (None, None), (None, 60), (200, None), (None, None)], 1):
        for first, lum in ((0, a), (h, b2)):
            s += (p_pcm_slice(first, fn, lum, h - 1, idc=2, redundant=0) if lum is not None
                  else p_slice(first, fn, h, idc=2, redundant=0))
    sys.stdout.buffer.write(s)
    sys.exit()
s += p_slice(0, 1, N)                                     # frame 1: copy of the IDR
# frame 2 (picture n-1): MB 2 missing -> concealed (I_4x4, qp 40, idc 0); MBs 3.. decoded with idc 2
s += p_slice(0, 2, 2, idc=2) + p_slice(3, 2, N - 3, idc=2)
if mode == 'attack':
    # frame 3 (picture n): P1 [0,2) | R1 fails at MB 1 | R2 fails at MB 0 | P2 [0,2) + P3 [4,N): count N, MBs 2,3 untouched
    s += p_slice(0, 3, 2)
    s += p_slice(0, 3, 1, rpc=1, bad_after=True)
    s += p_slice(0, 3, 1000, rpc=1)
    s += p_slice(0, 3, 2) + p_slice(4, 3, N - 4)
elif mode == 'attack2':
    # frame 3: P0 [0,2) P1 [3,N) primary; R (idc 2) [2..] re-touches MB 3 (decoded 2) and fails at MB 4:
    # MB 2 concealed with R's id, MB 3 survives with R's id and idc 2 -> its left edge compares R == R
    s += p_slice(0, 3, 2) + p_slice(3, 3, N - 3) + p_slice(2, 3, 2, rpc=1, idc=2, bad_after=True)
elif mode == 'plain':
    s += p_slice(0, 3, N)
elif mode == 'elide':
    # frames 3 and 4 copy: skip elision runs by frame 4. Frame 5: primary [0,6) makes MB 0 an I_PCM, primary [6,N) is
    # lost, the redundant slice skips all N (MBs 0-5 decoded twice), no deblocking. Frame 6 copies, so it may elide
    # MB 0 only if frame 5 wrongly recorded MB 0 as unchanged.
    s += p_slice(0, 3, N) + p_slice(0, 4, N)
    s += p_pcm_slice(0, 5, 200, 5, idc=1) + p_slice(0, 5, N, rpc=1, idc=1) + p_slice(0, 6, N)
    sys.stdout.buffer.write(s)
    sys.exit()
elif mode == 'slerr':
    # frames 3 and 4 copy: skip elision runs by frame 4. Frame 5: a slice writes MB 0 (I_PCM) and fails at MB 1, its
    # resend skips all N: MB 0 must be rebuilt, not elided. Frame 6: a slice skips MB 0 and fails at MB 1, its resend
    # makes MB 0 an I_PCM without deblocking: MB 0 must count as changed. Frames 7 and 8 copy.
    s += p_slice(0, 3, N) + p_slice(0, 4, N)
    s += p_pcm_slice(0, 5, 200, 0, idc=1, bad_after=True) + p_slice(0, 5, N, idc=1)
    s += p_slice(0, 6, 1, idc=1, bad_after=True) + p_pcm_slice(0, 6, 50, N - 1, idc=1)
    s += p_slice(0, 7, N, idc=1) + p_slice(0, 8, N, idc=1)
    sys.stdout.buffer.write(s)
    sys.exit()
elif mode == 'records':
    # Frame 3: primary [0,6) with MB 0 an I_PCM 2 levels above its right neighbour (an edge deblocking would smooth),
    # primary [6,N) lost, a redundant slice skips all N with deblocking off: its storage, not the records the primary
    # left for MBs 0-5, decides, and nothing is filtered. Frame 4: the same primary [0,6), a redundant slice turns
    # MB 1's deblocking off and fails on it, primary [6,N) completes the picture in raster order. Frame 5 copies.
    s += p_pcm_slice(0, 3, 102, 5) + p_slice(0, 3, N, rpc=1, idc=1)
    s += p_pcm_slice(0, 4, 102, 5) + p_slice(1, 4, 0, rpc=1, idc=1, bad_after=True) + p_slice(6, 4, N - 6)
    s += p_slice(0, 5, N)
    sys.stdout.buffer.write(s)
    sys.exit()
s += p_slice(0, 4, N)                                     # frame 4
sys.stdout.buffer.write(s)
