"""usage: mkhook.py ROOT  -- adds the whole-storage hash (VJ_FZ_STORAGE) to videojet.c and a 'H' line per call to host/fz.c"""
import sys
r = sys.argv[1]
p = r + "/components/videojet/src/videojet.c"
s = open(p).read()
if "vj_fz_storage_hash" not in s:
    s += r'''
#ifdef VJ_FZ_STORAGE
/* test only: FNV-1a over every observable field of mb[] and the slice/picture counters */
static unsigned long long fzh(unsigned long long h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ULL;
    return h;
}
unsigned long long vj_fz_storage_hash(vj_decoder_t *dec)
{
    storage_t *s = S(dec);
    unsigned long long h = 0xcbf29ce484222325ULL;
    h = fzh(h, &s->slice->sliceId, 4);
    h = fzh(h, &s->slice->numDecodedMbs, 4);
    h = fzh(h, &s->slice->lastMbAddr, 4);
    h = fzh(h, &s->picSliceBase, 4);
    h = fzh(h, &s->numConcealedMbs, 4);
    h = fzh(h, &s->picStarted, 4);
    h = fzh(h, &s->vjRecNext, 4);
    if (!s->mb)
        return h;
    for (u32 i = 0; i < s->picSizeInMbs; i++) {
        const mbStorage_t *m = s->mb + i;
        h = fzh(h, &m->mbType, sizeof m->mbType);
        h = fzh(h, &m->sliceId, 4);
        h = fzh(h, &m->disableDeblockingFilterIdc, 4);
        h = fzh(h, &m->filterOffsetA, 4);
        h = fzh(h, &m->filterOffsetB, 4);
        h = fzh(h, &m->qpY, 4);
        h = fzh(h, &m->chromaQpIndexOffset, 4);
        h = fzh(h, m->totalCoeff, sizeof m->totalCoeff);
        h = fzh(h, m->intra4x4PredMode, 16);
        h = fzh(h, m->refPic, sizeof m->refPic);
        for (int k = 0; k < 4; k++) { unsigned char z = m->refAddr[k] != NULL; h = fzh(h, &z, 1); }
        h = fzh(h, m->mv, sizeof m->mv);
        h = fzh(h, &m->decoded, 4);
    }
    return h;
}
#endif
'''
    open(p, "w").write(s)
p = r + "/host/fz.c"
s = open(p).read()
if "vj_fz_storage_hash" not in s:
    s = s.replace("static vj_decoder_t *d;", "static vj_decoder_t *d;\n#ifdef VJ_FZ_STORAGE\nextern unsigned long long vj_fz_storage_hash(vj_decoder_t *);\n#endif", 1)
    old = "    calls++;\n"
    assert s.count(old) == 1
    s = s.replace(old, old + "#ifdef VJ_FZ_STORAGE\n    printf(\"H %016llx\\n\", vj_fz_storage_hash(d));\n#endif\n")
    open(p, "w").write(s)
