CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -g -D_ERROR_PRINT
B := ../build-host
SRC := $(wildcard ../components/h264bsd/src/*.c)
HDR := $(wildcard ../components/h264bsd/src/*.h)
INC := -I../components/h264bsd/src -I../components/vj_hash/include
HASH := ../components/vj_hash/include/vj_hash.h

all: $(B)/vjdec $(B)/vjhash_raw

$(B):
	mkdir -p $@

$(B)/vjdec: vjdec.c $(SRC) $(HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjdec.c $(SRC)

$(B)/vjhash_raw: vjhash_raw.c $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjhash_raw.c

# The correctness floor: every stream with a .golden decodes bit-exact, and at least one exists.
check: all
	@./check.sh $(B)/vjdec ../streams

.PHONY: all check
