#   Copyright (C) 2026, The AROS Development Team. All rights reserved.
#
#   Desc: linuxso native syscall trampoline (x86-64).
#
#   IA32_EFER.SCE is off on AROS, so a guest `syscall` instruction raises
#   #UD.  The kernel.resource exception handler in lso_syscall.c stores the
#   instruction's return address 136 bytes below the trapped RSP (keeping
#   the 128-byte red zone intact), points RIP here and returns from the
#   trap; from then on we run in ordinary task context, so the emulation
#   may block.  Linux preserves every register except RAX, RCX and R11
#   across syscall; the C emulation clobbers the SysV caller-saved set and
#   any SSE state, hence the saves.
#
#   Entry:  RAX = nr, RDI RSI RDX R10 R8 R9 = arguments, [RSP] = return RIP
#   Exit:   RAX = result (negative errno on failure), RSP as before the trap

	.text
	.align	16
	.globl	lso_syscall_tramp
	.type	lso_syscall_tramp, @function

lso_syscall_tramp:
	push	%rbp
	mov	%rsp, %rbp
	push	%rdi
	push	%rsi
	push	%rdx
	push	%r8
	push	%r9
	push	%r10
	and	$-16, %rsp
	sub	$256, %rsp
	movups	%xmm0,   0(%rsp)
	movups	%xmm1,  16(%rsp)
	movups	%xmm2,  32(%rsp)
	movups	%xmm3,  48(%rsp)
	movups	%xmm4,  64(%rsp)
	movups	%xmm5,  80(%rsp)
	movups	%xmm6,  96(%rsp)
	movups	%xmm7, 112(%rsp)
	movups	%xmm8, 128(%rsp)
	movups	%xmm9, 144(%rsp)
	movups	%xmm10, 160(%rsp)
	movups	%xmm11, 176(%rsp)
	movups	%xmm12, 192(%rsp)
	movups	%xmm13, 208(%rsp)
	movups	%xmm14, 224(%rsp)
	movups	%xmm15, 240(%rsp)

	# lso_syscall(nr, a1, a2, a3, a4, a5, a6): a6 goes on the stack
	sub	$8, %rsp
	push	%r9
	mov	%r8, %r9
	mov	%r10, %r8
	mov	%rdx, %rcx
	mov	%rsi, %rdx
	mov	%rdi, %rsi
	mov	%rax, %rdi
	call	lso_syscall
	add	$16, %rsp

	movups	  0(%rsp), %xmm0
	movups	 16(%rsp), %xmm1
	movups	 32(%rsp), %xmm2
	movups	 48(%rsp), %xmm3
	movups	 64(%rsp), %xmm4
	movups	 80(%rsp), %xmm5
	movups	 96(%rsp), %xmm6
	movups	112(%rsp), %xmm7
	movups	128(%rsp), %xmm8
	movups	144(%rsp), %xmm9
	movups	160(%rsp), %xmm10
	movups	176(%rsp), %xmm11
	movups	192(%rsp), %xmm12
	movups	208(%rsp), %xmm13
	movups	224(%rsp), %xmm14
	movups	240(%rsp), %xmm15
	lea	-48(%rbp), %rsp
	pop	%r10
	pop	%r9
	pop	%r8
	pop	%rdx
	pop	%rsi
	pop	%rdi
	pop	%rbp
	# [RSP] = return RIP, stored 136 bytes below the original RSP
	pop	%r11
	add	$128, %rsp
	jmp	*%r11
	.size	lso_syscall_tramp, .-lso_syscall_tramp
