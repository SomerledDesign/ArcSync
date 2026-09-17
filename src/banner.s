/*
 * arcsync_banner — arm64 Darwin
 * One line to stderr via write(2); no libc.
 */
	.section __TEXT,__text,regular,pure_instructions
	.globl	_arcsync_banner
	.p2align	2
_arcsync_banner:
	mov	x0, #2
	adrp	x1, Lmsg@PAGE
	add	x1, x1, Lmsg@PAGEOFF
	mov	x2, #Lmsg_len
	mov	x16, #0x4
	svc	#0x80
	ret

	.section __TEXT,__cstring,cstring_literals
Lmsg:
	.ascii	"arcsync 1.0.0 \xE2\x80\x94 Win32 asm photo-CD tools, 1998; this is that program after the API grew up.\n"
Lmsg_end:
	.set	Lmsg_len, Lmsg_end - Lmsg
