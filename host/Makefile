CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -g -D_ERROR_PRINT
B := ../build-host
SRC := $(wildcard ../components/h264bsd/src/*.c)
INC := -I../components/h264bsd/src -I../components/vj_hash/include
HASH := ../components/vj_hash/include/vj_hash.h

all: $(B)/vjdec $(B)/vjhash_raw

$(B):
	mkdir -p $@

$(B)/vjdec: vjdec.c $(SRC) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjdec.c $(SRC)

$(B)/vjhash_raw: vjhash_raw.c $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjhash_raw.c

# The correctness floor: every stream with a .golden decodes bit-exact.
check: all
	@fail=0; for g in $$(find ../streams -name '*.golden' | sort); do \
	  s=$${g%.golden}.h264; \
	  if $(B)/vjdec $$s 2>/dev/null | cmp -s - $$g; then echo "PASS $$s"; \
	  else echo "FAIL $$s"; fail=1; fi; \
	done; exit $$fail

.PHONY: all check
