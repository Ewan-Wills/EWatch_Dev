/* model_blob.S - embed the llama2.c model + tokenizer into flash (.rodata).
 *
 * On the ESP32-S3 the linker maps .rodata into the flash-backed DROM region,
 * which the MMU memory-maps so the data is directly addressable by a pointer
 * with no RAM copy. That is exactly what llama2.c's run.c wants in place of its
 * mmap() of the checkpoint file: the Transformer weight pointers are set to
 * offsets inside this blob and read straight out of flash.
 *
 * .balign 4 so the first float of the weights (which begins right after the
 * 28-byte Config header, itself 4-aligned) lands on a 4-byte boundary.
 *
 * The .incbin paths are ABSOLUTE on purpose: GNU `as` resolves .incbin
 * relative to its own CWD/-I search path, and the gcc driver does not reliably
 * forward include dirs to `as` for .incbin. An absolute path is the one form
 * that always works. If this folder is moved, update the two paths below.
 */
    .section .rodata.embedded, "a"

    .global stories_bin
    .global stories_bin_end
    .balign 4
stories_bin:
    .incbin "/Users/ewills/Documents/EWatch/EWatch_Firmware/llm_ondevice/src/assets/stories260K.bin"
stories_bin_end:

    .global tok_bin
    .global tok_bin_end
    .balign 4
tok_bin:
    .incbin "/Users/ewills/Documents/EWatch/EWatch_Firmware/llm_ondevice/src/assets/tok512.bin"
tok_bin_end:
