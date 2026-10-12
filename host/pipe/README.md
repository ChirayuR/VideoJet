# Differential apparatus for the parse pipeline (host)

Committed, no machine-specific paths. `make -C host check-diff` already compares each configuration with the sequential
decoder of the same tree; these scripts are for the one thing it cannot do, a comparison against an older decoder (the
base the pipeline was proved against, 07e4818).

- `ncgen.py SEED W H FRAMES [plain|aso|fmo0|fmo1] > x.h264`: a crafted CAVLC stream whose parse depends on the neighbours'
  totalCoeff (I_PCM, skip runs, I4x4 / I16x16 / P16x16 with 0/1-coefficient blocks, mid-row slices; aso = slices of a picture
  in random order, fmo0 / fmo1 = two slice groups). The generator replicates the standard's nC rule, so a decoder whose
  neighbour model differs cannot decode its output cleanly. `make check-diff` generates 24 of them.
- `job.sh BASEFZ TESTFZ STREAM SEED RATE`: one run of both `fz` builds, `OK` or `DIFF` (stdout hashes and exit codes; the
  address space is capped, env `ULIM` in kB or 0 for none, so a damaged SPS that asks for GBs fails the same way in both).
- `runall.sh BASEFZ TESTFZ SEEDS RATES REPS STREAM...`: every stream x seed x damage rate x repetition, `P` jobs at once
  (default 16), prints the `OK` / `DIFF` counts.

A differential against the old decoder:
```
SCRATCH=<some empty dir>
git archive 07e4818 | tar -x -C $SCRATCH/base
cp host/fz.c $SCRATCH/base/host/fz.c      # fz.c needs only the private headers, so the new one builds against the old tree
make -C $SCRATCH/base/host B=../build WORKER=0 CFLAGS='-std=gnu99 -O2 -g -DVJ_FZ_STORAGE' ../build/fz
make -C host B=$SCRATCH/build-pipe WORKER=1 PIPE=1 $SCRATCH/build-pipe/fz        # (B is relative to host/ or absolute)
host/pipe/runall.sh $SCRATCH/base/build/fz $SCRATCH/build-pipe/fz 50 "3 6" 1 streams/*/*.h264
```
fz prints, per call, the status, the frame and changed_mb hashes, and (VJ_FZ_STORAGE, always on for fz) a hash of the decoder's
whole mb[] storage and slice counters.
