CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -g -D_ERROR_PRINT
# WORKER=1 (default): the deblock worker (VJ_DEBLOCK_WORKER) runs as a pthread, so every check below exercises the
# threaded path. WORKER=0 builds the sequential decoder (make WORKER=0 B=../build-host-seq), the reference for diffs.
WORKER ?= 1
VJF := $(if $(filter 1,$(WORKER)),-DVJ_DEBLOCK_WORKER -pthread,)
B := ../build-host
VJ := ../components/videojet
DEC := ../components/videojet/h264bsd
LIB_SRC := $(wildcard $(VJ)/src/*.c) $(wildcard $(DEC)/*.c)
LIB_HDR := $(wildcard $(VJ)/include/*.h) $(wildcard $(VJ)/src/*.h) $(wildcard $(DEC)/*.h)
LIB_INC := -I$(VJ)/include -I$(VJ)/src -I$(DEC)
INC := -I$(VJ)/include -I../components/vj_hash/include
HASH := ../components/vj_hash/include/vj_hash.h

all: $(B)/vjdec $(B)/vjhash_raw $(B)/test_api $(B)/test_mc $(B)/test_cavlc $(B)/test_write $(B)/test_idct $(B)/fz

$(B):
	mkdir -p $@

$(B)/vjdec: vjdec.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(VJF) $(INC) $(LIB_INC) -o $@ vjdec.c $(LIB_SRC)

# Damaged-stream decoder (fz.c): per call status and frame hash, for diffing two builds.
$(B)/fz: fz.c $(LIB_SRC) $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(VJF) $(INC) $(LIB_INC) -o $@ fz.c $(LIB_SRC)

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

$(B)/vjhash_raw: vjhash_raw.c $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjhash_raw.c

# The correctness floor: API unit tests, the MC, CAVLC and IDCT oracles, then every stream with a .golden decodes bit-exact.
check: all
	@$(B)/test_api ../streams vectors
	@$(B)/test_mc
	@$(B)/test_cavlc
	@$(B)/test_write
	@$(B)/test_idct
	@./check.sh $(B)/vjdec ../streams

# The same gate under ThreadSanitizer (worker on): any data race between the decoding thread and the worker fails it.
# setarch -R: TSan aborts ("unexpected memory mapping") under the high-entropy ASLR of recent kernels.
check-tsan:
	setarch $$(uname -m) -R $(MAKE) B=../build-host-tsan WORKER=1 CFLAGS='-std=gnu99 -O1 -g -fsanitize=thread -D_ERROR_PRINT' check

# And under AddressSanitizer + UBSan, worker on and off (shift off: the filters shift negative values, benign on GCC, pre-existing).
check-asan:
	$(MAKE) B=../build-host-asan WORKER=1 CFLAGS='-std=gnu99 -O1 -g -fsanitize=address,undefined -fno-sanitize=shift -fno-sanitize-recover=undefined -D_ERROR_PRINT' check
	$(MAKE) B=../build-host-asan-seq WORKER=0 CFLAGS='-std=gnu99 -O1 -g -fsanitize=address,undefined -fno-sanitize=shift -fno-sanitize-recover=undefined -D_ERROR_PRINT' check

# Worker on against worker off, damaged streams included (see fzdiff.sh).
check-diff: all
	$(MAKE) B=../build-host-seq WORKER=0 all
	./fzdiff.sh $(B)/fz ../build-host-seq/fz ../streams

.PHONY: all check check-tsan check-asan check-diff
