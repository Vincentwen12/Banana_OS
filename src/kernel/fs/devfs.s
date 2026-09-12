	.file	"devfs.c"
	.text
	.p2align 4,,15
	.def	null_read;	.scl	3;	.type	32;	.endef
null_read:
	xorl	%eax, %eax
	ret
	.p2align 4,,15
	.def	null_write;	.scl	3;	.type	32;	.endef
null_write:
	movq	%rcx, %rax
	ret
	.p2align 4,,15
	.def	null_close;	.scl	3;	.type	32;	.endef
null_close:
	xorl	%eax, %eax
	ret
	.p2align 4,,15
	.def	zero_read;	.scl	3;	.type	32;	.endef
zero_read:
	testq	%rcx, %rcx
	movq	%rcx, %rax
	je	.L6
	leaq	(%rdx,%rcx), %rsi
	.p2align 4,,10
.L7:
	movb	$0, (%rdx)
	addq	$1, %rdx
	cmpq	%rsi, %rdx
	jne	.L7
.L6:
	ret
	.p2align 4,,15
	.def	tty_write;	.scl	3;	.type	32;	.endef
tty_write:
	pushq	%r14
	movl	$84, %eax
	movq	%rdx, %r14
	movl	$1016, %edx
	pushq	%r13
	movq	%rcx, %r13
	pushq	%r12
	pushq	%rbp
	pushq	%rbx
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movl	$60, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	testq	%rcx, %rcx
	je	.L13
	movq	.refptr.vga_putc(%rip), %rbp
	leaq	(%r14,%rcx), %r12
	movl	$1016, %ebx
	.p2align 4,,10
.L14:
	movsbl	(%r14), %edi
	movl	%ebx, %edx
	movl	%edi, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	call	*%rbp
	addq	$1, %r14
	cmpq	%r12, %r14
	jne	.L14
.L13:
	movl	$62, %eax
	movl	$1016, %edx
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	popq	%rbx
	movq	%r13, %rax
	popq	%rbp
	popq	%r12
	popq	%r13
	popq	%r14
	ret
	.p2align 4,,15
	.def	tty_read;	.scl	3;	.type	32;	.endef
tty_read:
	testq	%rcx, %rcx
	jne	.L30
	movq	%rcx, %rax
	ret
	.p2align 4,,10
.L30:
	pushq	%rbx
	movq	%rdx, %rbx
	call	*.refptr.kbd_getchar(%rip)
	movb	%al, (%rbx)
	movl	$1, %eax
	popq	%rbx
	ret
	.p2align 4,,15
	.def	zero_write;	.scl	3;	.type	32;	.endef
zero_write:
	movq	%rcx, %rax
	ret
	.p2align 4,,15
	.def	zero_close;	.scl	3;	.type	32;	.endef
zero_close:
	xorl	%eax, %eax
	ret
	.p2align 4,,15
	.def	tty_close;	.scl	3;	.type	32;	.endef
tty_close:
	xorl	%eax, %eax
	ret
	.p2align 4,,15
	.globl	devfs_init
	.def	devfs_init;	.scl	2;	.type	32;	.endef
devfs_init:
	leaq	null_ops(%rip), %rax
	movq	$1, dev_null(%rip)
	movq	%rax, 24+dev_null(%rip)
	leaq	48+dev_null(%rip), %rax
	movq	$0, 8+dev_null(%rip)
	leaq	64(%rax), %rdx
	movq	$0, 16+dev_null(%rip)
	movq	$0, 32+dev_null(%rip)
	movq	$1, 40+dev_null(%rip)
	.p2align 4,,10
.L35:
	movb	$0, (%rax)
	addq	$1, %rax
	cmpq	%rdx, %rax
	jne	.L35
	movabsq	$7815273878500238383, %rax
	movb	$108, 56+dev_null(%rip)
	movq	%rax, 48+dev_null(%rip)
	leaq	zero_ops(%rip), %rax
	movq	%rax, 24+dev_zero(%rip)
	leaq	48+dev_zero(%rip), %rax
	movq	$2, dev_zero(%rip)
	leaq	64(%rax), %rdx
	movq	$0, 8+dev_zero(%rip)
	movq	$0, 16+dev_zero(%rip)
	movq	$0, 32+dev_zero(%rip)
	movq	$1, 40+dev_zero(%rip)
	.p2align 4,,10
.L36:
	movb	$0, (%rax)
	addq	$1, %rax
	cmpq	%rdx, %rax
	jne	.L36
	movabsq	$8243129037239968815, %rax
	movb	$111, 56+dev_zero(%rip)
	movq	%rax, 48+dev_zero(%rip)
	leaq	tty_ops(%rip), %rax
	movq	%rax, 24+dev_tty(%rip)
	leaq	48+dev_tty(%rip), %rax
	movq	$3, dev_tty(%rip)
	leaq	64(%rax), %rdx
	movq	$0, 8+dev_tty(%rip)
	movq	$0, 16+dev_tty(%rip)
	movq	$0, 32+dev_tty(%rip)
	movq	$1, 40+dev_tty(%rip)
	.p2align 4,,10
.L37:
	movb	$0, (%rax)
	addq	$1, %rax
	cmpq	%rax, %rdx
	jne	.L37
	movabsq	$8751747723086357551, %rax
	movq	%rax, 48+dev_tty(%rip)
	ret
	.p2align 4,,15
	.globl	devfs_open
	.def	devfs_open;	.scl	2;	.type	32;	.endef
devfs_open:
	xorl	%eax, %eax
	cmpb	$47, (%rdi)
	jne	.L41
	cmpb	$100, 1(%rdi)
	jne	.L41
	cmpb	$101, 2(%rdi)
	jne	.L41
	cmpb	$118, 3(%rdi)
	jne	.L41
	cmpb	$47, 4(%rdi)
	jne	.L41
	movzbl	5(%rdi), %edx
	cmpb	$110, %dl
	je	.L62
	cmpb	$122, %dl
	jne	.L44
	cmpb	$101, 6(%rdi)
	jne	.L41
	cmpb	$114, 7(%rdi)
	jne	.L41
	cmpb	$111, 8(%rdi)
	jne	.L41
	cmpb	$0, 9(%rdi)
	leaq	dev_zero(%rip), %rax
	movl	$0, %edx
	cmovne	%rdx, %rax
	ret
	.p2align 4,,10
.L44:
	cmpb	$116, %dl
	jne	.L41
	cmpb	$116, 6(%rdi)
	jne	.L41
	cmpb	$121, 7(%rdi)
	jne	.L41
	cmpb	$0, 8(%rdi)
	leaq	dev_tty(%rip), %rax
	movl	$0, %edx
	cmovne	%rdx, %rax
.L41:
	ret
	.p2align 4,,10
.L62:
	cmpb	$117, 6(%rdi)
	jne	.L41
	cmpb	$108, 7(%rdi)
	jne	.L41
	cmpb	$108, 8(%rdi)
	jne	.L41
	cmpb	$0, 9(%rdi)
	leaq	dev_null(%rip), %rdx
	cmove	%rdx, %rax
	ret
.lcomm dev_tty,112,32
.lcomm dev_zero,112,32
.lcomm dev_null,112,32
	.data
	.align 32
tty_ops:
	.quad	tty_read
	.quad	tty_write
	.quad	0
	.quad	tty_close
	.quad	0
	.quad	0
	.align 32
zero_ops:
	.quad	zero_read
	.quad	zero_write
	.quad	0
	.quad	zero_close
	.quad	0
	.quad	0
	.align 32
null_ops:
	.quad	null_read
	.quad	null_write
	.quad	0
	.quad	null_close
	.quad	0
	.quad	0
	.ident	"GCC: (x86_64-posix-seh-rev0, Built by MinGW-W64 project) 8.1.0"
	.section	.rdata$.refptr.kbd_getchar, "dr"
	.globl	.refptr.kbd_getchar
	.linkonce	discard
.refptr.kbd_getchar:
	.quad	kbd_getchar
	.section	.rdata$.refptr.vga_putc, "dr"
	.globl	.refptr.vga_putc
	.linkonce	discard
.refptr.vga_putc:
	.quad	vga_putc
