CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -g -D_ERROR_PRINT
B := ../build-host
VJ := ../components/videojet
DEC := ../components/videojet/h264bsd
LIB_SRC := $(wildcard $(VJ)/src/*.c) $(wildcard $(DEC)/*.c)
LIB_HDR := $(wildcard $(VJ)/include/*.h) $(wildcard $(VJ)/src/*.h) $(wildcard $(DEC)/*.h)
LIB_INC := -I$(VJ)/include -I$(VJ)/src -I$(DEC)
INC := -I$(VJ)/include -I../components/vj_hash/include
HASH := ../components/vj_hash/include/vj_hash.h

all: $(B)/vjdec $(B)/vjhash_raw $(B)/test_api $(B)/test_mc $(B)/test_cavlc

$(B):
	mkdir -p $@

$(B)/vjdec: vjdec.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) $(LIB_INC) -o $@ vjdec.c $(LIB_SRC)

$(B)/test_api: test_api.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) $(LIB_INC) -o $@ test_api.c $(LIB_SRC)

# Motion compensation against the standard's interpolation equations, exhaustive over positions, partitions, edges.
$(B)/test_mc: test_mc.c $(LIB_SRC) $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(LIB_INC) -o $@ test_mc.c $(LIB_SRC)

# CAVLC symbol decoders against the original h264bsd routines, exhaustive over every input. The test includes
# h264bsd_cavlc.c (its decoders are static), so it links only what that file calls.
$(B)/test_cavlc: test_cavlc.c $(DEC)/h264bsd_cavlc.c $(DEC)/h264bsd_stream.c $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(LIB_INC) -o $@ test_cavlc.c $(DEC)/h264bsd_stream.c

$(B)/vjhash_raw: vjhash_raw.c $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjhash_raw.c

# The correctness floor: API unit tests, the MC and CAVLC oracles, then every stream with a .golden decodes bit-exact.
check: all
	@$(B)/test_api ../streams vectors
	@$(B)/test_mc
	@$(B)/test_cavlc
	@./check.sh $(B)/vjdec ../streams

.PHONY: all check
