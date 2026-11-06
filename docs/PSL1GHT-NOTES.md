# PSL1GHT and RSX pitfalls

Found while writing TEE PS3 Benchmark with PSL1GHT and a GCC 13 toolchain. Each one was checked in
`librsx.a`'s disassembly, in RPCS3, and where it matters on a real PS3. They may save you some time.

## Command buffer

- **Segments.** The command buffer is handed out in segments of about 32 KB (the first one is shorter). libgcm's callback changes segments often. Most PSL1GHT command functions keep writing at the old position and set `current = old + n`, which is seamless.
- **`rsxLoadVertexProgram` / `rsxLoadVertexProgramBlock` break at a segment change.**
  - They load `current`, reserve room for the whole program, and after the callback write their first header at the *old* position. The 8-instruction blocks then go relative to the *new* `current`, so stale words lie in between.
  - RPCS3 reports "Vertex program block aborts prematurely ... no POS output", then "FIFO desync ... Dead FIFO". A console hangs.
  - This happens often with big programs (a 268-instruction program is about 4.4 KB). Fix: ask for the room first:
    ```c
    extern s32 rsxContextCallback(gcmContextData *context, u32 count);   /* in librsx.a */
    if (ctx->current + words > ctx->end)
        rsxContextCallback(ctx, words);
    rsxLoadVertexProgram(ctx, program, ucode);
    ```
- **`rsxDrawVertex4f` (immediate vertices) after a segment change** writes at the old position but sets `current` to the new one + 20. A gap of stale words follows; RPCS3 then reads, for example, a 1.0f as a jump. Use vertex arrays (`rsxBindVertexArrayAttrib` + `rsxDrawVertexArray`) instead.
- **`rsxSetTransferData` never returns** when the input pitch, the output pitch and the line length are all equal (a contiguous copy). It queues copies until the buffer is full. Use `rsxSetTransferDataMode` + `rsxSetTransferDataFormat` + `rsxSetTransferDataOffset` (the last one launches the copy).
- **Never let the ring wrap.** When libgcm's callback leaves the last segment, the PPU can wait forever. Jump back to the first segment yourself once half the buffer is used and the RSX has read up to there. This includes long runs of commands without a flip.

## Reports and time stamps

- **`rsxSetTimeStamp` is broken.** Its unsafe version stores the argument at offset 8 instead of 4, so the RSX gets a stale word; RPCS3 says "Bad type". Use `rsxSetReport(ctx, GCM_ZPASS_PIXEL_CNT, index)` and read the 64-bit `timer` (ns) of `gcmGetReportDataAddress(index)`. A report is written once everything before it is drawn. On a PS3 it agreed with the PPU's clock to 0.1 %.

## Shaders

- **`cgcomp` cannot compile Cg** without NVIDIA's toolkit. `cgcomp -v/-f -a` assembles ARB assembly, with these limits:
  - **Only the fixed register names `R0`–`R31`.** A declared `TEMP r0` (or any other name) assembles as *no register*: writes go nowhere and reads come from the input attribute, without any error.
  - **It drops every constant** (PARAM, literals, `program.local`). Take the numbers from vertex attributes, or hand-encode the program.
- **Hand-encoded fragment programs** use the NV40 layout: constants inline after the instruction, every word halfword-swapped, `fp_control` 0x40 for an R0 result. tiny3d's `nv_shaders.h` has good examples.
- **RPCS3 can show you what it made of a program:** set `Log shader programs: true` (it writes decompiled GLSL as `.spirv` files to `~/Library/Caches/rpcs3/shaderlog/` on macOS).
- RPCS3 needs up to about 135 ms to translate a 268-instruction vertex program before its first draw. Self-test timeouts must allow for that.

## The RSX on a real PS3

- **Drawing into a `GCM_SURFACE_F_X32` (32-bit float) linear surface** with a Z16 depth buffer hung the graphics engine for good; RPCS3 drew it. A8R8G8B8 / X8R8G8B8 targets work. A hung RSX plus an app that keeps queueing frames freezes the whole console, so end the app instead.
- **A flip resets the RSX's 3D state**, the clear colour included. Never cache RSX state across a flip: a cached clear colour gave a black background with the right one every few pictures, a visible flicker.
- **Who waits for the vertical blank.** With VSYNC flips, on the console the RSX waits at `gcmSetWaitFlip`; the flip command itself takes about 1 ms. In RPCS3 it is the other way round.
- **Render targets.**
  - Linear 1920x1080 targets blend slowly: 1.99 G pixels a second for blending alone.
  - The same target in a tile region (`gcmSetTile`, pitch from `gcmGetTiledPitchSize`, here 8192) managed 3.43 G pixels a second.
  - A fill program with two texture reads ran twice as fast there.
- **Vertex units.** Batches of culled points drawn between full-screen layers did *not* run alongside the pixels (0 % overlap measured), and switching vertex programs costs time.

## RPCS3 (macOS, MoltenVK) quirks

- Within one draw call, an alpha-blended quad blends with the picture as it was *before* that call, not with earlier quads of the same call. Start a new draw call where items overlap. The console blends in order.
- GPU copies into main memory (NV3089, M2MF) arrive 11 ms to 1.7 s after their backend label. Poll the pixels before judging a read-back.
- Labels arrive when the FIFO is processed, not when the GPU is done, so shader timings in RPCS3 mean nothing.
- A new shader's first runs can round 1–3 ulp differently from later ones (interpreter vs. compiled). Take a bit-exact reference from a later run.
- "ROP reads from r0/h0 without writing to it" is harmless for hand-encoded fragment programs.
