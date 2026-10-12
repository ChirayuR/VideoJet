CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -g -D_ERROR_PRINT
# WORKER=1 (default): the deblock worker (VJ_DEBLOCK_WORKER) runs as a pthread, so every check below exercises the
# threaded path. WORKER=0 builds the sequential decoder (make WORKER=0 B=../build-host-seq), the reference for diffs.
WORKER ?= 1
# PIPE=1 (default, the bench configuration): the parse pipeline too (needs WORKER=1); PIPE=0 is the library default (no pipeline).
# `make check` and `make check-diff` run both; PIPEF adds test macros (VJ_RING_DEPTH=n, VJ_PIPE_JITTER_US=n, ...)
PIPE ?= 1
VJF := $(if $(filter 1,$(WORKER)),-DVJ_DEBLOCK_WORKER -pthread,) $(if $(filter 1,$(PIPE)),-DVJ_PARSE_PIPELINE,) $(PIPEF)
B := ../build-host
STARVE_RUN = $(if $(filter 1,$(PIPE)), && $(B)/test_starve ../streams && $(B)/test_sleep ../streams,)
WORKER_TESTS = $(if $(filter 1,$(WORKER)),$(B)/test_join ../streams && $(B)/test_stall ../streams$(STARVE_RUN),true)
WORKER_BINS = $(if $(filter 1,$(WORKER)),$(B)/test_join $(B)/test_stall $(if $(filter 1,$(PIPE)),$(B)/test_starve $(B)/test_sleep,),)
VJ := ../components/videojet
DEC := ../components/videojet/h264bsd
LIB_SRC := $(wildcard $(VJ)/src/*.c) $(wildcard $(DEC)/*.c)
LIB_HDR := $(wildcard $(VJ)/include/*.h) $(wildcard $(VJ)/src/*.h) $(wildcard $(DEC)/*.h)
LIB_INC := -I$(VJ)/include -I$(VJ)/src -I$(DEC)
INC := -I$(VJ)/include -I../components/vj_hash/include
HASH := ../components/vj_hash/include/vj_hash.h

all: $(B)/vjdec $(B)/vjhash_raw $(B)/test_api $(B)/test_mc $(B)/test_cavlc $(B)/test_write $(B)/test_idct $(B)/test_nbr $(B)/fz $(WORKER_BINS)

$(B):
	mkdir -p $@

$(B)/vjdec: vjdec.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(VJF) $(INC) $(LIB_INC) -o $@ vjdec.c $(LIB_SRC)

# Damaged-stream decoder (fz.c): per call status and frame hash, for diffing two builds.
$(B)/fz: fz.c $(LIB_SRC) $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(VJF) -DVJ_FZ_STORAGE $(INC) $(LIB_INC) -o $@ fz.c $(LIB_SRC)

$(B)/test_api: test_api.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(VJF) $(INC) $(LIB_INC) -o $@ test_api.c $(LIB_SRC)

# Motion compensation against the standard's interpolation equations, exhaustive over positions, partitions, edges.
$(B)/test_mc: test_mc.c $(LIB_SRC) $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(VJF) $(LIB_INC) -o $@ test_mc.c $(LIB_SRC)

# CAVLC symbol decoders against the original h264bsd routines, exhaustive over every input. The test includes
# h264bsd_cavlc.c (its decoders are static), so it links only what that file calls.
$(B)/test_cavlc: test_cavlc.c $(DEC)/h264bsd_cavlc.c $(DEC)/h264bsd_stream.c $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(VJF) $(LIB_INC) -o $@ test_cavlc.c $(DEC)/h264bsd_stream.c

# The write-out (h264bsd_image.c) against a plain clip(pred + residual) reference.
$(B)/test_write: test_write.c $(LIB_SRC) $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(LIB_INC) -o $@ test_write.c $(LIB_SRC)

# h264bsdProcessBlock against the original routine, bit for bit (all coeffMaps x qps x skip, extreme coefficients).
# -fwrapv: the reference's i32 overflow on corrupt-stream coefficients is then defined wrap-around.
$(B)/test_idct: test_idct.c $(DEC)/h264bsd_transform.c $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) -fwrapv $(LIB_INC) -o $@ test_idct.c $(DEC)/h264bsd_transform.c

# Why the deblock may skip a macroblock whose three thresholds all have alpha or beta 0: no edge filter changes a byte then
# (every (indexA, indexB) pair, every bS), and EdgesInert is exactly that test. It includes h264bsd_deblocking.c (the filters are
# static), so that file is left out of the link.
all: $(B)/test_alpha0
$(B)/test_alpha0: test_alpha0.c $(LIB_SRC) $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(VJF) $(LIB_INC) -o $@ test_alpha0.c $(filter-out $(DEC)/h264bsd_deblocking.c,$(LIB_SRC))

# The worker's contracts: join on every return (worker delayed after its claim) and the owner taking a run back (worker
# not scheduled), plus thread-creation failure. Host shim hooks only, see test_worker.c.
$(B)/test_join: test_worker.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(VJF) -DVJ_WORKER_JITTER_US=3000 $(if $(filter 1,$(PIPE)),-DVJ_PIPE_JITTER_US=400,) $(INC) $(LIB_INC) -o $@ test_worker.c $(LIB_SRC)

$(B)/test_stall: test_worker.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(VJF) -DVJ_WORKER_STALL_MS=120 $(INC) $(LIB_INC) -o $@ test_worker.c $(LIB_SRC)

# The parse pipeline's worker never runs the parser (-DVJ_PIPE_NOSTEP): every slice is taken back by the decoding task after
# 50 ms and parsed inline, the output equals the build with no worker at all.
$(B)/test_starve: test_worker.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(VJF) -DVJ_PIPE_NOSTEP $(INC) $(LIB_INC) -o $@ test_worker.c $(LIB_SRC)

# The parser's shadow copies only the totalCoeff entries nC can read (VJ_COPY_TC): exactly the neighbour tables' MB_A / MB_B targets.
$(B)/test_nbr: test_nbr.c $(LIB_SRC) $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(VJF) $(INC) $(LIB_INC) -o $@ test_nbr.c $(LIB_SRC)

# The decoding task stalls every 40th record (VJ_PIPE_STALL_EVERY): the ring stays full, so the worker sleeps and is woken.
$(B)/test_sleep: test_worker.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(VJF) -DVJ_PIPE_STALL_EVERY=40 -DVJ_PIPE_JITTER_US=200 $(INC) $(LIB_INC) -o $@ test_worker.c $(LIB_SRC)

$(B)/vjhash_raw: vjhash_raw.c $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjhash_raw.c

# The correctness floor, for each configuration the library ships: the sequential decoder, the deblock worker, the deblock
# worker with the parse pipeline.
check:
	$(MAKE) B=$(B)-seq WORKER=0 PIPE=0 check-one
	$(MAKE) B=$(B)-pipe0 WORKER=1 PIPE=0 check-one
	$(MAKE) B=$(B)-pipe1 WORKER=1 PIPE=1 check-one

# One configuration: API unit tests, the MC, CAVLC, neighbour and IDCT oracles, the worker contracts, then every stream
# with a .golden decodes bit-exact.
check-one: all
	@$(B)/test_api ../streams vectors
	@$(B)/test_mc
	@$(B)/test_cavlc
	@$(B)/test_nbr
	@$(WORKER_TESTS)
	@$(B)/test_write
	@$(B)/test_idct
	@$(B)/test_alpha0
	@./check.sh $(B)/vjdec ../streams

# The same gate under ThreadSanitizer (worker on): any data race between the decoding thread and the worker fails it.
# setarch -R: TSan aborts ("unexpected memory mapping") under the high-entropy ASLR of recent kernels.
check-tsan:
	setarch $$(uname -m) -R $(MAKE) B=../build-host-tsan WORKER=1 PIPE=1 CFLAGS='-std=gnu99 -O1 -g -fsanitize=thread -D_ERROR_PRINT' check-one

# And under AddressSanitizer + UBSan, worker on and off (shift off: the filters shift negative values, benign on GCC, pre-existing).
ASAN_CFLAGS = -std=gnu99 -O1 -g -fsanitize=address,undefined -fno-sanitize=shift -fno-sanitize-recover=undefined -D_ERROR_PRINT
check-asan:
	$(MAKE) B=../build-host-asan WORKER=1 PIPE=1 CFLAGS='$(ASAN_CFLAGS)' check-one
	$(MAKE) B=../build-host-asan-pipe0 WORKER=1 PIPE=0 CFLAGS='$(ASAN_CFLAGS)' check-one
	$(MAKE) B=../build-host-asan-seq WORKER=0 PIPE=0 CFLAGS='$(ASAN_CFLAGS)' check-one

# The poison oracle: every parse and every skipped macroblock starts from a slot of random "plausible" bytes (VJ_POISON), so a
# reader of stale slot contents changes pixels; every stream must still match its golden. Both configurations.
check-poison:
	$(MAKE) B=../build-host-poison0 WORKER=1 PIPE=0 CFLAGS='-std=gnu99 -O2 -g -DVJ_POISON=7' ../build-host-poison0/vjdec
	./check.sh ../build-host-poison0/vjdec ../streams
	$(MAKE) B=../build-host-poison1 WORKER=1 PIPE=1 CFLAGS='-std=gnu99 -O2 -g -DVJ_POISON=7' ../build-host-poison1/vjdec
	./check.sh ../build-host-poison1/vjdec ../streams

# The pipeline's slice-group path (neighbours read from the storage) forced for every slice (VJ_NBR_STORAGE).
check-storage:
	$(MAKE) B=../build-host-nbrstorage WORKER=1 PIPE=1 CFLAGS='-std=gnu99 -O2 -g -DVJ_NBR_STORAGE' ../build-host-nbrstorage/vjdec
	./check.sh ../build-host-nbrstorage/vjdec ../streams

# The ring at its smallest depths (VJ_RING_DEPTH 1 and 2: the worker and the decoding task meet at every record).
check-depth:
	for d in 1 2; do \
	  $(MAKE) B=../build-host-depth$$d WORKER=1 PIPE=1 CFLAGS="-std=gnu99 -O2 -g -DVJ_RING_DEPTH=$$d" ../build-host-depth$$d/vjdec && \
	  ./check.sh ../build-host-depth$$d/vjdec ../streams || exit 1; \
	done

# Crafted streams (ncgen.py: I_PCM, nC across slices, skip runs, mid-row slices, ASO, FMO 0 / 1), generated into $(B)/crafted.
# The lowqp ones (QP 14, random mb_qp_delta, filter offsets and chroma qp offset) put the deblocking thresholds on both sides of
# alpha / beta 0, so the alpha-0 skip is taken for some macroblocks and not for others.
CRAFTED := $(B)/crafted
$(CRAFTED)/.stamp: pipe/ncgen.py | $(B)
	mkdir -p $(CRAFTED)
	for m in plain aso fmo0 fmo1; do for i in 1 2 3 4 5 6; do \
	  python3 pipe/ncgen.py $$i $$((5 + i % 4 * 3)) $$((3 + i % 3 * 2)) 12 $$m > $(CRAFTED)/$$m$$i.h264; done; done
	for m in plain aso fmo0 fmo1; do for i in 1 2 3 4 5 6; do \
	  python3 pipe/ncgen.py $$i $$((5 + i % 4 * 3)) $$((3 + i % 3 * 2)) 12 $$m 14 > $(CRAFTED)/lowqp-$$m$$i.h264; done; done
	touch $@

# Each configuration against the sequential decoder of the same tree, damaged streams included (fzdiff.sh): per call status, frame
# hash, changed_mb hash and the decoder's whole mb[] storage. The repo streams x 50 repetitions, the crafted ones x 3.
check-diff:
	$(MAKE) B=../build-host-seq WORKER=0 PIPE=0 ../build-host-seq/fz
	for pipe in 0 1; do \
	  $(MAKE) B=../build-host-pipe$$pipe WORKER=1 PIPE=$$pipe ../build-host-pipe$$pipe/fz && \
	  ./fzdiff.sh ../build-host-pipe$$pipe/fz ../build-host-seq/fz ../streams && \
	  $(MAKE) B=../build-host-pipe$$pipe WORKER=1 PIPE=$$pipe ../build-host-pipe$$pipe/crafted/.stamp && \
	  REPS=3 SEEDS=4 ./fzdiff.sh ../build-host-pipe$$pipe/fz ../build-host-seq/fz ../build-host-pipe$$pipe/crafted || exit 1; \
	done

.PHONY: all check check-one check-tsan check-asan check-poison check-storage check-depth check-diff
