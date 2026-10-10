# test/mc_pie: on-board oracle for the PIE motion compensation

Re-run it after any change to `components/videojet/src/vj_mc_pie.S`, or to the C kernels in
`components/videojet/h264bsd/h264bsd_reconstruct.c` that it must match. It needs `MC PASS` (0 mismatches).

The app links the library with `CONFIG_VJ_PIE=y` and a second copy of `h264bsd_reconstruct.c` compiled with PIE off and
its symbols renamed (`main/rec_c.c`). It then compares their 384-byte prediction arrays:
- over the whole case space of `host/test_mc.c`: every quarter/eighth-sample position, partition shape and offset, and
  reference position from fully outside one picture edge to fully outside the other, on 3x2- and 1x1-MB pictures with
  random and random-0/255 content;
- on a 6x4-MB picture, where most windows lie inside the picture (the PIE path), with a 1/64 sample also checked
  against the standard's equations.

It then prints cycles per `h264bsdPredictSamples` call, C vs PIE, with the reference in internal RAM and in PSRAM.
Data is generated at run time. Pictures are allocated like the DPB (16-aligned, 32 bytes of slack after Cr), which
the kernels' over-reads rely on.

## Build, flash, read
From the repository root, with ESP-IDF v5.4.2 exported:
```
idf.py -C test/mc_pie -B build-mc-pie -DSDKCONFIG=build-mc-pie/sdkconfig build
(cd build-mc-pie && python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args)
idf.py -C test/mc_pie -B build-mc-pie -DSDKCONFIG=build-mc-pie/sdkconfig -p /dev/ttyACM0 monitor
```
Any 115200 serial reader on the USB-Serial/JTAG port works too: reset the board and read. It takes about 10 minutes.
```
MC start
MC pic 48x32 content 0: 2927376 cases, 0 mismatches so far (105 s)
...
MC BASE 8314432 cases (host/test_mc.c's space), 0 mismatches PIE vs C
MC pic 96x64 content 0: 7379984 cases, 0 mismatches so far (552 s)
MC RESULT 15694416 cases (N also misaligned), 0 mismatches PIE vs C; 115312 spec cases, 0 mismatches
MC PASS
MC TIME iram <position> <WxH> <C cycles> <PIE cycles>
...
MC TIME psram ...
MC done
```
A mismatch prints `MC MISMATCH ...` (picture, macroblock, partition, motion vector, first differing byte) and ends in
`MC FAIL`. The app uses the bench's partition table, so the streams partition survives, but it replaces the bench
firmware: flash the bench again afterwards (`tools/bench.py` fails on a stale ELF anyway). The board is shared, so take
it only when nobody else is using it.
