	.amdgcn_target "amdgcn-amd-amdhsa--gfx1200"
	.text
	.globl	sentinel
	.p2align 8
	.type	sentinel,@function
sentinel:
	v_mov_b32 v0, s0
	v_mov_b32 v1, s1
	v_mov_b32 v2, 0x600dc0de
	global_store_b32 v[0:1], v2, off
	s_wait_storecnt 0x0
	s_endpgm
.Lfunc_end0:
	.size	sentinel, .Lfunc_end0-sentinel
	.rodata
	.p2align 6
	.amdhsa_kernel sentinel
		.amdhsa_next_free_vgpr 3
		.amdhsa_next_free_sgpr 2
		.amdhsa_user_sgpr_count 2
	.end_amdhsa_kernel
