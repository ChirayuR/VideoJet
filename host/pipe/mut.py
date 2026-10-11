"""usage: mut.py NAME CFLAGS  (mutant source edits come from MUTS[NAME]) -> copies new/ to mut-NAME/, applies, builds fz"""
import sys, os, shutil, subprocess
SP = "$SCRATCH/pipe-h"
F = "components/videojet/h264bsd/h264bsd_slice_data.c"
MUTS = {
 "noclear": (F, "    for (i = 0; i <= w; i++)\n        p->nbr[i].st = VJ_NBR_NONE;\n", "", ""),
 "skipoff1": (F, "for (i = skipRun; i--;)", "for (i = skipRun - 1; i--;)", ""),
 "lastbefore": (F, "    tmp = h264bsdDecodeMacroblockLayer(&p->strm, mbLayer, tcA, tcB,", "    s->last = h264bsdMoreRbspData(&p->strm) ? HANTRO_FALSE : HANTRO_TRUE;\n    tmp = h264bsdDecodeMacroblockLayer(&p->strm, mbLayer, tcA, tcB,", "last"),
 "unavail0": (F, "        default:          return NULL;", "        default:          return vjZero24;", ""),
 "pcmzero": (F, "        i = VJ_NBR_PCM;", "        i = VJ_NBR_ZERO;", ""),
 "tc16": (F, "for (i = 0; i < 24; i++)\n                n->tc[i] = (u8)tc[i];", "for (i = 0; i < 16; i++)\n                n->tc[i] = (u8)tc[i];", ""),
 "stalemb": (F, "                memset(&mbLayer->mbPred, 0, sizeof(mbPred_t));\n                /* mark", "                /* mark", "pois"),
 "colnoreset": (F, "        if (++p->col == p->width)\n            p->col = 0;", "        ++p->col;", ""),
 "stor_slice": (F, "if (mbA && mbA->sliceId == p->sliceId)", "if (mbA && mbA->sliceId <= p->sliceId)", "stor"),
 "stor_slice2": (F, "if (mbB && mbB->sliceId == p->sliceId)", "if (mbB)", "stor"),
 "prevskip": (F, "                p->prevSkipped = HANTRO_TRUE;\n                memset", "                memset", ""),
 "zerocbp": (F, "    else if (!mbLayer->codedBlockPattern &&\n             h264bsdMbPartPredMode(mbLayer->mbType) != PRED_MODE_INTRA16x16)", "    else if (!mbLayer->codedBlockPattern)", ""),
}
name = sys.argv[1]; cflags = sys.argv[2] if len(sys.argv) > 2 else ""
f, old, new, kind = MUTS[name]
d = f"{SP}/mut-{name}"
shutil.rmtree(d, ignore_errors=True)
shutil.copytree(f"{SP}/new", d, ignore=shutil.ignore_patterns("build-*"))
p = f"{d}/{f}"; s = open(p).read()
assert s.count(old) == 1, (name, s.count(old))
s = s.replace(old, new)
if name == "lastbefore":
    t = "    s->last = h264bsdMoreRbspData(&p->strm) ? HANTRO_FALSE : HANTRO_TRUE;\n    if (mbLayer->mbType == I_PCM)"
    assert s.count(t) == 1
    s = s.replace(t, "    if (mbLayer->mbType == I_PCM)")
open(p, "w").write(s)
extra = "-DVJ_POISON=7" if kind == "pois" else "-DVJ_NBR_STORAGE" if kind == "stor" else ""
r = subprocess.run(["make", f"B=../build", "WORKER=0", f"CFLAGS=-std=gnu99 -O2 -g -DVJ_FZ_STORAGE {extra} {cflags}", "../build/fz"], cwd=f"{d}/host", capture_output=True, text=True)
print(name, "built" if os.path.exists(f"{d}/build/fz") else "FAILED\n" + r.stderr[-800:])
