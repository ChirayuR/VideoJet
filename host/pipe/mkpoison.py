"""usage: mkpoison.py ROOT -- adds VJ_POISON (random plausible bytes into every ring slot before each parse, and into a skipped
MB's slot except mbType/mbPred before its recon) and VJ_STATS (record counters printed at exit) to the scratch copy ROOT"""
import sys
r=sys.argv[1]
d=r+"/components/videojet/h264bsd/"
def sub(fn,old,new):
    p=d+fn; s=open(p).read()
    assert s.count(old)==1,(fn,old)
    open(p,'w').write(s.replace(old,new))
fn=r'''
#ifdef VJ_POISON
static u32 vjp_seed = VJ_POISON;
static u32 vjp_rnd(void)
{
    vjp_seed = vjp_seed * 1664525u + 1013904223u;
    return vjp_seed >> 8;
}
/* keepPred: leave mbType and mbPred (the skip contract) */
static void vjPoisonX(vjSlot_t *s, u32 keepPred)
{
    macroblockLayer_t *m = &s->layer;
    u32 i, j;
    if (!keepPred)
    {
        m->mbType = (mbType_e)(vjp_rnd() % 32);
        for (i = 0; i < 16; i++)
        {
            m->mbPred.prevIntra4x4PredModeFlag[i] = vjp_rnd() & 1;
            m->mbPred.remIntra4x4PredMode[i] = vjp_rnd() & 7;
        }
        m->mbPred.intraChromaPredMode = vjp_rnd() & 3;
        for (i = 0; i < 4; i++)
        {
            m->mbPred.refIdxL0[i] = vjp_rnd() & 1;
            m->mbPred.mvdL0[i].hor = (i16)((i32)(vjp_rnd() % 9) - 4);
            m->mbPred.mvdL0[i].ver = (i16)((i32)(vjp_rnd() % 9) - 4);
        }
    }
    m->codedBlockPattern = vjp_rnd() % 48;
    m->mbQpDelta = (i32)(vjp_rnd() % 7) - 3;
    for (i = 0; i < 4; i++)
    {
        m->subMbPred.subMbType[i] = (subMbType_e)(vjp_rnd() & 3);
        m->subMbPred.refIdxL0[i] = vjp_rnd() & 1;
        for (j = 0; j < 4; j++)
        {
            m->subMbPred.mvdL0[i][j].hor = (i16)((i32)(vjp_rnd() % 9) - 4);
            m->subMbPred.mvdL0[i][j].ver = (i16)((i32)(vjp_rnd() % 9) - 4);
        }
    }
    for (i = 0; i < 27; i++)
        m->residual.totalCoeff[i] = (i16)(vjp_rnd() % 17);
    for (i = 0; i < 26; i++)
        for (j = 0; j < 16; j++)
            m->residual.level[i][j] = (i32)(vjp_rnd() % 5) - 2;
    for (i = 0; i < 24; i++)
        m->residual.coeffMap[i] = vjp_rnd() & 0xFFFF;
    if (!keepPred) { s->kind = vjp_rnd() % 3; s->count = vjp_rnd(); s->status = vjp_rnd(); s->last = vjp_rnd() & 1; }
}
void vjPoison(vjSlot_t *s) { vjPoisonX(s, 0); }
void vjPoisonSkip(vjSlot_t *s) { vjPoisonX(s, 1); }
#endif
#ifdef VJ_STATS
#include <stdio.h>
#include <stdlib.h>
unsigned long vjStat[16];
static void vjStatPrint(void)
{
    static const char *n[] = {"mb","skiprun","skipmbs","err","pcm","i16","i4","inter","sub8x8","cbp0","midrow","storage","redundant","slices","i_slice","last"};
    for (int i = 0; i < 16; i++) fprintf(stderr, "%s=%lu ", n[i], vjStat[i]);
    fprintf(stderr, "\n");
}
static void vjStatRec(const vjParse_t *p, const vjSlot_t *s)
{
    static int reg;
    if (!reg) { reg = 1; atexit(vjStatPrint); }
    if (s->kind == VJ_REC_SKIPRUN) { vjStat[1]++; vjStat[2] += s->count; }
    else if (s->kind == VJ_REC_ERR) vjStat[3]++;
    else
    {
        mbType_e t = s->layer.mbType; vjStat[0]++;
        if (t == I_PCM) vjStat[4]++;
        else if (h264bsdMbPartPredMode(t) == PRED_MODE_INTRA16x16) vjStat[5]++;
        else if (h264bsdMbPartPredMode(t) == PRED_MODE_INTRA4x4) vjStat[6]++;
        else { vjStat[7]++; if (h264bsdNumMbPart(t) == 4) vjStat[8]++; }
        if (t != I_PCM && !s->layer.codedBlockPattern) vjStat[9]++;
    }
    if (s->last) vjStat[15]++;
}
#endif
'''
sub("h264bsd_slice_data.c","void vjParseBegin(vjParse_t *p, storage_t *pStorage, const strmData_t *pStrm,\n    const sliceHeader_t *pSliceHeader)\n{",
    fn+"void vjParseBegin(vjParse_t *p, storage_t *pStorage, const strmData_t *pStrm,\n    const sliceHeader_t *pSliceHeader)\n{")
# stats hooks: wrap returns of vjParseNext by renaming
sub("h264bsd_slice_data.c","void vjParseNext(vjParse_t *p, vjSlot_t *s)\n{","static void vjParseNext0(vjParse_t *p, vjSlot_t *s);\nvoid vjParseNext(vjParse_t *p, vjSlot_t *s)\n{\n    vjParseNext0(p, s);\n#ifdef VJ_STATS\n    vjStatRec(p, s);\n#endif\n}\nstatic void vjParseNext0(vjParse_t *p, vjSlot_t *s)\n{")
sub("h264bsd_slice_data.c","            DEBUG((\"Skipping macroblock %d\\n\", currMbAddr));\n","            DEBUG((\"Skipping macroblock %d\\n\", currMbAddr));\n#ifdef VJ_POISON\n            vjPoisonSkip(slot);\n#endif\n")
