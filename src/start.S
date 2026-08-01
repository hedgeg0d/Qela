// Entry stack: [rsp]=argc, [rsp+8]=argv, then NULL, envp, NULL, auxv.
	.section .text._start,"ax",@progbits
	.globl _start
	.type _start,@function
_start:
	xorl	%ebp, %ebp
	movq	(%rsp), %rdi
	leaq	8(%rsp), %rsi
	andq	$-16, %rsp
	call	qmain
	movl	%eax, %edi
	movl	$60, %eax
	syscall
	hlt
	.size _start,.-_start
	.section .note.GNU-stack,"",@progbits
