	.file	"vfs.c"
	.text
	.p2align 4,,15
	.globl	vfs_init
	.def	vfs_init;	.scl	2;	.type	32;	.endef
vfs_init:
	leaq	fd_table(%rip), %rax
	leaq	512(%rax), %rdx
	.p2align 4,,10
.L2:
	movq	$0, (%rax)
	addq	$8, %rax
	cmpq	%rdx, %rax
	jne	.L2
	ret
	.p2align 4,,15
	.globl	vfs_fd_alloc
	.def	vfs_fd_alloc;	.scl	2;	.type	32;	.endef
vfs_fd_alloc:
	leaq	fd_table(%rip), %rcx
	xorl	%edx, %edx
	jmp	.L8
	.p2align 4,,10
.L6:
	addq	$1, %rdx
	cmpq	$64, %rdx
	je	.L10
.L8:
	cmpq	$0, (%rcx,%rdx,8)
	movl	%edx, %eax
	jne	.L6
	movslq	%edx, %rdx
	movq	%rdi, (%rcx,%rdx,8)
	ret
	.p2align 4,,10
.L10:
	movl	$-1, %eax
	ret
	.p2align 4,,15
	.globl	vfs_fd_get
	.def	vfs_fd_get;	.scl	2;	.type	32;	.endef
vfs_fd_get:
	cmpl	$63, %edi
	ja	.L13
	leaq	fd_table(%rip), %rax
	movslq	%edi, %rdi
	movq	(%rax,%rdi,8), %rax
	ret
	.p2align 4,,10
.L13:
	xorl	%eax, %eax
	ret
	.p2align 4,,15
	.globl	vfs_fd_free
	.def	vfs_fd_free;	.scl	2;	.type	32;	.endef
vfs_fd_free:
	cmpl	$63, %edi
	ja	.L14
	leaq	fd_table(%rip), %rax
	movslq	%edi, %rdi
	movq	$0, (%rax,%rdi,8)
.L14:
	ret
	.p2align 4,,15
	.globl	vfs_open
	.def	vfs_open;	.scl	2;	.type	32;	.endef
vfs_open:
	xorl	%eax, %eax
	ret
	.p2align 4,,15
	.globl	vfs_read
	.def	vfs_read;	.scl	2;	.type	32;	.endef
vfs_read:
	testq	%rdi, %rdi
	je	.L21
	movq	24(%rdi), %rax
	testq	%rax, %rax
	je	.L21
	movq	(%rax), %rax
	testq	%rax, %rax
	je	.L21
	movq	%rdx, %rcx
	movq	%rsi, %rdx
	movq	16(%rdi), %rsi
	jmp	*%rax
	.p2align 4,,10
.L21:
	movq	$-1, %rax
	ret
	.p2align 4,,15
	.globl	vfs_write
	.def	vfs_write;	.scl	2;	.type	32;	.endef
vfs_write:
	testq	%rdi, %rdi
	je	.L30
	movq	24(%rdi), %rax
	testq	%rax, %rax
	je	.L31
	movq	8(%rax), %r8
	testq	%r8, %r8
	je	.L32
	pushq	%rbx
	movl	$1016, %ebx
	movq	%rdx, %rcx
	movl	$118, %eax
	movl	%ebx, %edx
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movl	$33, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movq	%rsi, %rdx
	movq	16(%rdi), %rsi
	call	*%r8
	movl	%ebx, %edx
	movq	%rax, %rcx
	movl	$126, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movq	%rcx, %rax
	popq	%rbx
	ret
	.p2align 4,,10
.L30:
	movl	$86, %eax
	movl	$1016, %edx
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movq	$-1, %rcx
.L28:
	movq	%rcx, %rax
	ret
	.p2align 4,,10
.L31:
	movl	$79, %eax
	movl	$1016, %edx
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movq	$-1, %rcx
	jmp	.L28
	.p2align 4,,10
.L32:
	movl	$80, %eax
	movl	$1016, %edx
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movq	$-1, %rcx
	jmp	.L28
	.p2align 4,,15
	.globl	vfs_close
	.def	vfs_close;	.scl	2;	.type	32;	.endef
vfs_close:
	testq	%rdi, %rdi
	je	.L37
	movq	24(%rdi), %rax
	testq	%rax, %rax
	je	.L37
	movq	24(%rax), %rax
	testq	%rax, %rax
	je	.L37
	jmp	*%rax
	.p2align 4,,10
.L37:
	movq	$-1, %rax
	ret
	.p2align 4,,15
	.globl	vfs_ioctl
	.def	vfs_ioctl;	.scl	2;	.type	32;	.endef
vfs_ioctl:
	testq	%rdi, %rdi
	je	.L42
	movq	24(%rdi), %rax
	testq	%rax, %rax
	je	.L42
	movq	32(%rax), %rax
	testq	%rax, %rax
	je	.L42
	jmp	*%rax
	.p2align 4,,10
.L42:
	movq	$-1, %rax
	ret
	.p2align 4,,15
	.globl	vfs_lseek
	.def	vfs_lseek;	.scl	2;	.type	32;	.endef
vfs_lseek:
	testq	%rdi, %rdi
	movq	$-1, %rax
	je	.L43
	cmpq	$1, %rdx
	je	.L45
	testq	%rdx, %rdx
	je	.L46
	cmpq	$2, %rdx
	je	.L47
	ret
	.p2align 4,,10
.L47:
	movq	8(%rdi), %rax
	addq	%rsi, %rax
	movq	%rax, 16(%rdi)
.L43:
	ret
	.p2align 4,,10
.L46:
	movq	%rsi, 16(%rdi)
	movq	%rsi, %rax
	ret
	.p2align 4,,10
.L45:
	movq	16(%rdi), %rax
	addq	%rsi, %rax
	movq	%rax, 16(%rdi)
	ret
.lcomm fd_table,512,32
	.ident	"GCC: (x86_64-posix-seh-rev0, Built by MinGW-W64 project) 8.1.0"
