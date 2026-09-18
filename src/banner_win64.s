/* arcsync_banner — Windows x86_64 MinGW: _write(2, msg, len) */
	.globl	arcsync_banner
	.text
arcsync_banner:
	subq	$40, %rsp
	movl	$2, %ecx
	leaq	msg(%rip), %rdx
	movl	$msglen, %r8d
	call	_write
	addq	$40, %rsp
	ret
	.data
msg:
	.ascii	"arcsync 1.0.0 - Win32 asm photo-CD tools, 1998; this is that program after the API grew up.\n"
	.set	msglen, . - msg
