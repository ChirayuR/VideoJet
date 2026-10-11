# test/idct_pie: on-board oracle for the PIE 4x4 inverse transform

Re-run it after any change to `components/videojet/src/vj_idct_pie.S` or the full path of `h264bsdProcessBlock`
(`h264bsd_transform.c`). It needs `IDCT PASS` (0 mismatches).

The app links the library with `CONFIG_VJ_PIE=y` and runs `host/test_idct.c` (included, its `main` renamed): the original
`h264bsdProcessBlock` against the library's, over every coeffMap x every qp x skip, small / typical / large / extreme /
32-bit-random / structured blocks, blocks whose dequantised products sit at the PIE guard edge (+-2^26), and random
blocks; every fifth case also on a 4-byte-aligned block (the C fallback), with its neighbours checked for corruption.
~22 M cases, about a minute. Then it prints cycles per block, old vs new, per path (timed from IRAM, copy cost removed).

## Build, flash, read
From the repository root, with ESP-IDF v5.4.2 exported:
```
idf.py -C test/idct_pie -B build-idct-pie -DSDKCONFIG=build-idct-pie/sdkconfig build
(cd build-idct-pie && python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args)
```
then read the USB-Serial/JTAG port at 115200 (reset the board). Expected:
```
IDCT start
test_idct: 22048640 cases, 0 mismatches
IDCT RESULT 22048640 cases, 0 mismatches
IDCT PASS
IDCT TIME full qp 24: old <n> new <n> cycles/block (copy <n> removed)
...
IDCT done
```
It replaces the bench firmware (same partition table, the streams partition survives): flash the bench again afterwards.
The board is shared: take it only when nobody else is using it.
