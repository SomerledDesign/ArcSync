/*
 * arcsync_banner — x86_64 Darwin
 */
	.section __TEXT,__text,regular,pure_instructions
	.globl	_arcsync_banner
	.p2align	4, 0x90
_arcsync_banner:
	movl	$0x2000004, %eax
	movl	$2, %edi
	leaq	Lmsg(%rip), %rsi
	movl	$Lmsg_len, %edx
	syscall
	ret

	.section __TEXT,__cstring,cstring_literals
Lmsg:
	.ascii	"arcsync 1.0.0 \xE2\x80\x94 Win32 asm photo-CD tools, 1998; this is that program after the API grew up.\n"
Lmsg_end:
	.set	Lmsg_len, Lmsg_end - Lmsg
