/* SPIKE e3: layout perturbation. A 300-nop function linked ahead of the decoder so that every decoder function moves.
 * It touches a global so that it owns a literal: the Xtensa linker places functions that have literals first, the
 * leaf functions after them. */
volatile int vj_pad_sink;

__attribute__((used, retain, noinline)) void vj_pad(void)
{
    vj_pad_sink = 1;
    __asm__ volatile(".rept 300\n nop\n .endr");
}
