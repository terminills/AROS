# sentinel_gfx1200 — minimal GFX12/RDNA4 compute dispatch test kernel

Assembled with LLVM-20 llvm-mc for gfx1200 (RDNA4/Navi44):
  llvm-mc -triple=amdgcn-amd-amdhsa -mcpu=gfx1200 -filetype=obj sentinel_gfx1200.s

Kernel: reads output-buffer VA from s[0:1] (= COMPUTE_USER_DATA_0/1), stores the
sentinel 0x600DC0DE there via global_store_b32, waits, s_endpgm. If the CPU reads
0x600DC0DE back from the buffer, a compute SHADER ran on the CUs (distinct from a
CP WRITE_DATA, which the REUSE self-test already proved).

.text (9 dwords, LE):
  7E000200  v_mov_b32 v0, s0
  7E020201  v_mov_b32 v1, s1
  7E0402FF 600DC0DE  v_mov_b32 v2, 0x600dc0de
  EE06807C 01000000 00000000  global_store_b32 v[0:1], v2, off
  BFC10000  s_wait_storecnt 0
  BFB00000  s_endpgm

Descriptor-derived compute state:
  COMPUTE_PGM_RSRC1 = 0x600C0000
  COMPUTE_PGM_RSRC2 = 0x00000084   (USER_SGPR=2, wgid_x enabled)
  NUM_THREAD_X/Y/Z  = 1/1/1 ; DISPATCH_DIRECT(1,1,1)
