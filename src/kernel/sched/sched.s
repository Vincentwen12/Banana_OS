	.file	"sched.c"
	.text
	.p2align 4,,15
	.def	mlfq_remove;	.scl	3;	.type	32;	.endef
mlfq_remove:
	movl	520(%rdi), %r9d
	testl	%r9d, %r9d
	jle	.L8
	movl	512(%rdi), %r8d
	xorl	%ecx, %ecx
	movl	%r8d, %edx
	sarl	$31, %edx
	shrl	$26, %edx
	leal	(%r8,%rdx), %eax
	andl	$63, %eax
	subl	%edx, %eax
	cltq
	cmpq	(%rdi,%rax,8), %rsi
	jne	.L4
	jmp	.L3
	.p2align 4,,10
.L7:
	leal	(%rcx,%r8), %eax
	cltd
	shrl	$26, %edx
	addl	%edx, %eax
	andl	$63, %eax
	subl	%edx, %eax
	cltq
	cmpq	%rsi, (%rdi,%rax,8)
	je	.L3
.L4:
	addl	$1, %ecx
	cmpl	%r9d, %ecx
	jne	.L7
.L8:
	xorl	%eax, %eax
	ret
	.p2align 4,,10
.L3:
	leal	-1(%r9), %esi
	cmpl	%ecx, %esi
	jle	.L5
	leal	1(%rcx,%r8), %edx
	addl	%r9d, %r8d
	.p2align 4,,10
.L6:
	movl	%edx, %ecx
	sarl	$31, %ecx
	shrl	$26, %ecx
	leal	(%rdx,%rcx), %eax
	andl	$63, %eax
	subl	%ecx, %eax
	cltq
	movq	(%rdi,%rax,8), %r9
	leal	-1(%rdx), %eax
	addl	$1, %edx
	movl	%eax, %ecx
	sarl	$31, %ecx
	shrl	$26, %ecx
	addl	%ecx, %eax
	andl	$63, %eax
	subl	%ecx, %eax
	cmpl	%edx, %r8d
	cltq
	movq	%r9, (%rdi,%rax,8)
	jne	.L6
.L5:
	movl	516(%rdi), %eax
	movl	%esi, 520(%rdi)
	addl	$63, %eax
	cltd
	shrl	$26, %edx
	addl	%edx, %eax
	andl	$63, %eax
	subl	%edx, %eax
	movl	%eax, 516(%rdi)
	movl	$1, %eax
	ret
	.section .rdata,"dr"
.LC0:
	.ascii "Idle\0"
	.text
	.p2align 4,,15
	.globl	sched_init
	.def	sched_init;	.scl	2;	.type	32;	.endef
sched_init:
	leaq	mlfq(%rip), %rax
	movq	$0, current_task(%rip)
	leaq	2640(%rax), %rdx
.L14:
	movl	$0, 512(%rax)
	addq	$528, %rax
	movl	$0, -12(%rax)
	movl	$0, -8(%rax)
	cmpq	%rdx, %rax
	jne	.L14
	leaq	task_table(%rip), %rcx
	leaq	5632(%rcx), %rdx
	movq	%rcx, %rax
	.p2align 4,,10
.L15:
	movl	$0, (%rax)
	addq	$88, %rax
	movq	$0, -80(%rax)
	movq	$0, -72(%rax)
	movl	$4, -64(%rax)
	movl	$0, -60(%rax)
	movl	$0, -56(%rax)
	movl	$0, -52(%rax)
	movl	$-1, -48(%rax)
	movl	$-1, -44(%rax)
	movl	$0, -40(%rax)
	movq	$0, -32(%rax)
	movq	$0, -24(%rax)
	movq	$0, -16(%rax)
	movq	$0, -8(%rax)
	cmpq	%rdx, %rax
	jne	.L15
	leaq	.LC0(%rip), %rax
	movq	%rcx, idle_task(%rip)
	movq	%rax, 8+task_table(%rip)
	movabsq	$17179869185, %rax
	movq	%rax, 24+task_table(%rip)
	movabsq	$429496729700, %rax
	movl	$0, task_table(%rip)
	movq	$0, 16+task_table(%rip)
	movq	%rax, 32+task_table(%rip)
	movq	$-1, 40+task_table(%rip)
	movl	$4, 48+task_table(%rip)
	movq	$0, 56+task_table(%rip)
	movq	$0, 64+task_table(%rip)
	movq	$0, 72+task_table(%rip)
	movq	$0, 80+task_table(%rip)
	movl	$1, task_count(%rip)
	ret
	.p2align 4,,15
	.globl	sched_next
	.def	sched_next;	.scl	2;	.type	32;	.endef
sched_next:
	leaq	mlfq(%rip), %r9
	pushq	%rbx
	xorl	%edi, %edi
	movl	$33, %r11d
	movq	%r9, %rcx
	movl	$1016, %edx
	movl	$61, %r10d
.L25:
	movl	520(%rcx), %esi
	testl	%esi, %esi
	jle	.L20
	movslq	%edi, %r8
	movslq	512(%rcx), %rbx
	subl	$1, %esi
	imulq	$66, %r8, %r8
	movq	%rbx, %rax
	addl	$1, %eax
	addq	%rbx, %r8
	movq	(%r9,%r8,8), %rbx
	movl	%eax, %r8d
	movl	%esi, 520(%rcx)
	sarl	$31, %r8d
	shrl	$26, %r8d
	addl	%r8d, %eax
	andl	$63, %eax
	subl	%r8d, %eax
	testq	%rbx, %rbx
	movl	%eax, 512(%rcx)
	je	.L20
	movl	24(%rbx), %r8d
	movl	(%rbx), %esi
	cmpl	$1, %r8d
	je	.L33
	movl	%r11d, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	leal	48(%rsi), %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movl	%r10d, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	leal	48(%r8), %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
.L20:
	addl	$1, %edi
	addq	$528, %rcx
	cmpl	$5, %edi
	jne	.L25
	movq	idle_task(%rip), %rdx
	movl	$-1, %eax
	testq	%rdx, %rdx
	je	.L18
	cmpl	$1, 24(%rdx)
	jne	.L18
	movl	(%rdx), %eax
	movl	$0, 24(%rdx)
	movq	%rdx, current_task(%rip)
.L18:
	popq	%rbx
	ret
	.p2align 4,,10
.L33:
	movl	36(%rbx), %eax
	movq	%rbx, current_task(%rip)
	movl	$0, 24(%rbx)
	testl	%eax, %eax
	jle	.L34
	movl	%esi, %eax
.L35:
	popq	%rbx
	ret
.L34:
	movl	$5, %eax
	subl	28(%rbx), %eax
	leal	(%rax,%rax,4), %eax
	addl	%eax, %eax
	movl	%eax, 36(%rbx)
	movl	%esi, %eax
	jmp	.L35
	.p2align 4,,15
	.globl	sched_tick
	.def	sched_tick;	.scl	2;	.type	32;	.endef
sched_tick:
	movq	current_task(%rip), %rax
	addq	$1, sched_total_ticks(%rip)
	testq	%rax, %rax
	je	.L36
	movl	24(%rax), %edx
	testl	%edx, %edx
	jne	.L36
	movl	36(%rax), %ecx
	addq	$1, sched_active_ticks(%rip)
	leal	-1(%rcx), %edx
	testl	%edx, %edx
	movl	%edx, 36(%rax)
	jle	.L42
.L36:
	ret
	.p2align 4,,10
.L42:
	movl	28(%rax), %edx
	cmpl	$3, %edx
	jg	.L40
	addl	$1, %edx
	movl	%edx, 28(%rax)
.L40:
	movslq	%edx, %rdx
	movl	$1, 24(%rax)
	imulq	$528, %rdx, %rsi
	leaq	mlfq(%rip), %r8
	addq	%r8, %rsi
	movl	520(%rsi), %edi
	cmpl	$63, %edi
	jg	.L41
	imulq	$66, %rdx, %rdx
	movslq	516(%rsi), %r9
	addl	$1, %edi
	movl	%edi, 520(%rsi)
	addq	%r9, %rdx
	movq	%rax, (%r8,%rdx,8)
	leal	1(%r9), %eax
	cltd
	shrl	$26, %edx
	addl	%edx, %eax
	andl	$63, %eax
	subl	%edx, %eax
	movl	%eax, 516(%rsi)
.L41:
	movq	$0, current_task(%rip)
	ret
	.p2align 4,,15
	.globl	sched_add
	.def	sched_add;	.scl	2;	.type	32;	.endef
sched_add:
	movl	task_count(%rip), %edx
	cmpl	$63, %edx
	jg	.L43
	movq	(%rdi), %r8
	movslq	%edx, %rcx
	movabsq	$8589934593, %r9
	imulq	$88, %rcx, %rcx
	leaq	task_table(%rip), %rsi
	leaq	(%rsi,%rcx), %rax
	movq	%r8, (%rax)
	movq	8(%rdi), %r8
	movq	%r8, 8(%rax)
	movq	16(%rdi), %r8
	movq	%r8, 16(%rax)
	movq	24(%rdi), %r8
	movq	%r8, 24(%rax)
	movq	32(%rdi), %r8
	movq	%r8, 32(%rax)
	movq	40(%rdi), %r8
	movq	%r8, 40(%rax)
	movq	48(%rdi), %r8
	movq	%r8, 48(%rax)
	movq	56(%rdi), %r8
	movq	%r8, 56(%rax)
	movq	64(%rdi), %r8
	movq	%r8, 64(%rax)
	movq	72(%rdi), %r8
	movq	%r8, 72(%rax)
	movq	80(%rdi), %rdi
	movl	%edx, (%rax)
	addl	$1, %edx
	movl	%edx, task_count(%rip)
	movq	%rdi, 80(%rax)
	movq	%r9, 24(%rcx,%rsi)
	leaq	32(%rsi,%rcx), %rcx
	movabsq	$128849018910, %rsi
	movq	%rsi, (%rcx)
	movabsq	$-4294967296, %rsi
	movq	%rsi, 8(%rcx)
	movl	1576+mlfq(%rip), %ecx
	movl	$2, 48(%rax)
	cmpl	$63, %ecx
	jg	.L43
	movslq	1572+mlfq(%rip), %rdi
	leaq	mlfq(%rip), %rsi
	addl	$1, %ecx
	movl	%ecx, 1576+mlfq(%rip)
	movq	%rax, 1056(%rsi,%rdi,8)
	leal	1(%rdi), %eax
	cltd
	shrl	$26, %edx
	addl	%edx, %eax
	andl	$63, %eax
	subl	%edx, %eax
	movl	%eax, 1572+mlfq(%rip)
.L43:
	ret
	.section .rdata,"dr"
.LC1:
	.ascii "user\0"
.LC2:
	.ascii "/dev/tty\0"
	.text
	.p2align 4,,15
	.globl	sched_create_user_task
	.def	sched_create_user_task;	.scl	2;	.type	32;	.endef
sched_create_user_task:
	movl	task_count(%rip), %r8d
	cmpl	$63, %r8d
	jg	.L61
	pushq	%r12
	leaq	.LC1(%rip), %rcx
	movabsq	$8589934593, %r11
	pushq	%rbp
	leaq	task_table(%rip), %rbp
	pushq	%rbx
	leaq	mlfq(%rip), %r9
	movslq	%r8d, %rbx
	imulq	$88, %rbx, %rdx
	leaq	0(%rbp,%rdx), %rax
	movq	%rcx, 8(%rax)
	movabsq	$128849018910, %rcx
	movl	%r8d, (%rax)
	movq	$0, 16(%rax)
	movq	%r11, 24(%rdx,%rbp)
	leaq	32(%rbp,%rdx), %rdx
	movq	%rcx, (%rdx)
	movabsq	$-4294967296, %rcx
	movq	%rcx, 8(%rdx)
	movq	%rdi, 72(%rax)
	movl	1576+mlfq(%rip), %edi
	movl	$2, 48(%rax)
	movq	%rbx, 56(%rax)
	movq	%rsi, 64(%rax)
	cmpl	$63, %edi
	movq	$1, 80(%rax)
	jg	.L49
	movslq	1572+mlfq(%rip), %rcx
	addl	$1, %edi
	movl	%edi, 1576+mlfq(%rip)
	movq	%rax, 1056(%r9,%rcx,8)
	leal	1(%rcx), %eax
	cltd
	shrl	$26, %edx
	addl	%edx, %eax
	andl	$63, %eax
	subl	%edx, %eax
	movl	%eax, 1572+mlfq(%rip)
.L49:
	movl	$1016, %esi
	movl	$67, %eax
	movl	%esi, %edx
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	leal	48(%r8), %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movl	$58, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movl	$99, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movl	$1717986919, %edx
	movl	%edi, %eax
	imull	%edx
	movl	%edi, %eax
	sarl	$31, %eax
	sarl	$2, %edx
	movl	%edx, %ecx
	movl	%esi, %edx
	subl	%eax, %ecx
	leal	48(%rcx), %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	leal	(%rcx,%rcx,4), %eax
	movl	%edi, %ecx
	addl	%eax, %eax
	subl	%eax, %ecx
	movl	%ecx, %eax
	addl	$48, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movl	$91, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	testl	%edi, %edi
	jle	.L50
	movl	1568+mlfq(%rip), %r10d
	xorl	%ecx, %ecx
	jmp	.L52
	.p2align 4,,10
.L82:
	cmpl	%edi, %ecx
	jge	.L50
.L52:
	leal	(%rcx,%r10), %eax
	movl	%eax, %esi
	sarl	$31, %esi
	shrl	$26, %esi
	addl	%esi, %eax
	andl	$63, %eax
	subl	%esi, %eax
	cltq
	movq	1056(%r9,%rax,8), %rax
	testq	%rax, %rax
	je	.L51
	movl	(%rax), %eax
	addl	$48, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
.L51:
	addl	$1, %ecx
	cmpl	$9, %ecx
	jle	.L82
.L50:
	movl	$93, %eax
	movl	$1016, %edx
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	addl	$1, %r8d
	xorl	%esi, %esi
	movq	.refptr.devfs_open(%rip), %r12
	movl	%r8d, task_count(%rip)
	leaq	.LC2(%rip), %rdi
	call	*%r12
	testq	%rax, %rax
	je	.L54
	movq	%rax, %rdi
	call	*.refptr.vfs_fd_alloc(%rip)
.L54:
	leaq	.LC2(%rip), %rdi
	xorl	%esi, %esi
	call	*%r12
	testq	%rax, %rax
	je	.L55
	movq	%rax, %rdi
	call	*.refptr.vfs_fd_alloc(%rip)
.L55:
	leaq	.LC2(%rip), %rdi
	xorl	%esi, %esi
	call	*%r12
	testq	%rax, %rax
	je	.L56
	movq	%rax, %rdi
	call	*.refptr.vfs_fd_alloc(%rip)
.L56:
	movl	$1, %edi
	call	*.refptr.vfs_fd_get(%rip)
	movl	$1016, %edx
	movq	%rax, %rcx
	movl	$70, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	testq	%rcx, %rcx
	je	.L57
	movl	$49, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	movq	24(%rcx), %rcx
	testq	%rcx, %rcx
	je	.L58
	movl	$111, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	cmpq	$0, 8(%rcx)
	je	.L59
	movl	$119, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	imulq	$88, %rbx, %rbx
	movl	0(%rbp,%rbx), %eax
	popq	%rbx
	popq	%rbp
	popq	%r12
	ret
	.p2align 4,,10
.L58:
	movl	$110, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	imulq	$88, %rbx, %rbx
	movl	0(%rbp,%rbx), %eax
	popq	%rbx
	popq	%rbp
	popq	%r12
	ret
	.p2align 4,,10
.L57:
	movl	$48, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	imulq	$88, %rbx, %rbx
	movl	0(%rbp,%rbx), %eax
	popq	%rbx
	popq	%rbp
	popq	%r12
	ret
	.p2align 4,,10
.L59:
	movl	$112, %eax
/APP
 # 14 "src/kernel/core/port.h" 1
	outb %al, %dx
 # 0 "" 2
/NO_APP
	imulq	$88, %rbx, %rbx
	movl	0(%rbp,%rbx), %eax
	popq	%rbx
	popq	%rbp
	popq	%r12
	ret
.L61:
	movl	$-1, %eax
	ret
	.p2align 4,,15
	.globl	sched_user_task_exit
	.def	sched_user_task_exit;	.scl	2;	.type	32;	.endef
sched_user_task_exit:
	testl	%edi, %edi
	js	.L83
	cmpl	%edi, task_count(%rip)
	jle	.L83
	leaq	task_table(%rip), %rax
	movslq	%edi, %r10
	imulq	$88, %r10, %r10
	leaq	mlfq_remove(%rip), %r11
	leaq	mlfq(%rip), %rdi
	addq	%rax, %r10
	movq	%r10, %rsi
	movl	$4, 24(%r10)
	movq	$0, 80(%r10)
	call	*%r11
	leaq	528+mlfq(%rip), %rdi
	movq	%r10, %rsi
	call	*%r11
	leaq	1056+mlfq(%rip), %rdi
	movq	%r10, %rsi
	call	*%r11
	leaq	1584+mlfq(%rip), %rdi
	movq	%r10, %rsi
	call	*%r11
	leaq	2112+mlfq(%rip), %rdi
	movq	%r10, %rsi
	jmp	*%r11
	.p2align 4,,10
.L83:
	ret
	.p2align 4,,15
	.globl	sched_set_user_task_rsp
	.def	sched_set_user_task_rsp;	.scl	2;	.type	32;	.endef
sched_set_user_task_rsp:
	movq	current_task(%rip), %rax
	testq	%rax, %rax
	je	.L85
	cmpq	$0, 80(%rax)
	je	.L85
	movq	%rdi, 64(%rax)
.L85:
	ret
	.p2align 4,,15
	.globl	sched_block
	.def	sched_block;	.scl	2;	.type	32;	.endef
sched_block:
	testq	%rdi, %rdi
	je	.L90
	movl	task_count(%rip), %eax
	cmpl	%eax, (%rdi)
	jb	.L94
.L90:
	ret
	.p2align 4,,10
.L94:
	movl	%esi, 44(%rdi)
	movq	%rdi, %r10
	movl	$2, 24(%rdi)
	movslq	28(%rdi), %rdi
	leaq	mlfq(%rip), %rax
	movq	%r10, %rsi
	imulq	$528, %rdi, %rdi
	addq	%rax, %rdi
	leaq	mlfq_remove(%rip), %rax
	call	*%rax
	cmpq	%r10, current_task(%rip)
	jne	.L90
	movq	$0, current_task(%rip)
	ret
	.p2align 4,,15
	.globl	sched_wake
	.def	sched_wake;	.scl	2;	.type	32;	.endef
sched_wake:
	testq	%rdi, %rdi
	je	.L95
	movl	task_count(%rip), %eax
	cmpl	%eax, (%rdi)
	jnb	.L95
	cmpl	$2, 24(%rdi)
	je	.L99
.L95:
	ret
	.p2align 4,,10
.L99:
	movslq	48(%rdi), %rax
	leaq	mlfq(%rip), %r8
	movl	$5, %edx
	movl	$1, 24(%rdi)
	movl	$-1, 44(%rdi)
	imulq	$528, %rax, %rcx
	subl	%eax, %edx
	movl	%eax, 28(%rdi)
	leal	(%rdx,%rdx,4), %edx
	addl	%edx, %edx
	movl	%edx, 36(%rdi)
	addq	%r8, %rcx
	movl	520(%rcx), %esi
	cmpl	$63, %esi
	jg	.L95
	movslq	516(%rcx), %r9
	imulq	$66, %rax, %rax
	addl	$1, %esi
	movl	%esi, 520(%rcx)
	movq	%r9, %rdx
	addq	%r9, %rax
	addl	$1, %edx
	movq	%rdi, (%r8,%rax,8)
	movl	%edx, %eax
	sarl	$31, %eax
	shrl	$26, %eax
	addl	%eax, %edx
	andl	$63, %edx
	subl	%eax, %edx
	movl	%edx, 516(%rcx)
	ret
	.p2align 4,,15
	.globl	sched_boost
	.def	sched_boost;	.scl	2;	.type	32;	.endef
sched_boost:
	testq	%rdi, %rdi
	je	.L100
	movl	task_count(%rip), %eax
	cmpl	%eax, (%rdi)
	jb	.L104
.L100:
	ret
	.p2align 4,,10
.L104:
	leaq	mlfq(%rip), %r11
	movq	%rdi, %r10
	movslq	28(%rdi), %rdi
	movq	%r10, %rsi
	movl	$0, 28(%r10)
	movl	$50, 36(%r10)
	leaq	mlfq_remove(%rip), %rax
	movl	%edi, 48(%r10)
	imulq	$528, %rdi, %rdi
	addq	%r11, %rdi
	call	*%rax
	movl	520+mlfq(%rip), %edx
	movl	$1, 24(%r10)
	cmpl	$63, %edx
	jg	.L100
	movslq	516+mlfq(%rip), %rcx
	addl	$1, %edx
	movl	%edx, 520+mlfq(%rip)
	movq	%rcx, %rax
	movq	%r10, (%r11,%rcx,8)
	addl	$1, %eax
	movl	%eax, %ecx
	sarl	$31, %ecx
	shrl	$26, %ecx
	addl	%ecx, %eax
	andl	$63, %eax
	subl	%ecx, %eax
	movl	%eax, 516+mlfq(%rip)
	ret
	.p2align 4,,15
	.globl	sched_kill
	.def	sched_kill;	.scl	2;	.type	32;	.endef
sched_kill:
	testl	%edi, %edi
	js	.L105
	cmpl	%edi, task_count(%rip)
	jle	.L105
	leaq	task_table(%rip), %r10
	movslq	%edi, %rdi
	imulq	$88, %rdi, %rdi
	addq	%rdi, %r10
	cmpl	$4, 24(%r10)
	je	.L105
	movslq	28(%r10), %rdi
	leaq	mlfq(%rip), %rax
	movq	%r10, %rsi
	imulq	$528, %rdi, %rdi
	addq	%rax, %rdi
	leaq	mlfq_remove(%rip), %rax
	call	*%rax
	cmpq	%r10, current_task(%rip)
	movl	$4, 24(%r10)
	je	.L109
.L105:
	ret
	.p2align 4,,10
.L109:
	movq	$0, current_task(%rip)
	ret
	.section .rdata,"dr"
.LC3:
	.ascii "?\0"
.LC4:
	.ascii "???\0"
.LC5:
	.ascii "RUN\0"
.LC6:
	.ascii "RDY\0"
.LC7:
	.ascii "BLK\0"
.LC8:
	.ascii "SLP\0"
.LC9:
	.ascii "ZMB\0"
	.align 8
.LC10:
	.ascii "ID  Name        State Pri Core\12\0"
	.text
	.p2align 4,,15
	.globl	sched_list
	.def	sched_list;	.scl	2;	.type	32;	.endef
sched_list:
	pushq	%rbp
	leaq	.LC5(%rip), %rax
	movq	%rsp, %rbp
	pushq	%r15
	pushq	%r14
	pushq	%r13
	pushq	%r12
	pushq	%rbx
	andq	$-16, %rsp
	subq	$48, %rsp
	testl	%esi, %esi
	movl	task_count(%rip), %r14d
	movq	%rax, (%rsp)
	leaq	.LC6(%rip), %rax
	movq	%rax, 8(%rsp)
	leaq	.LC7(%rip), %rax
	movq	%rax, 16(%rsp)
	leaq	.LC8(%rip), %rax
	movq	%rax, 24(%rsp)
	leaq	.LC9(%rip), %rax
	movq	%rax, 32(%rsp)
	jle	.L111
	movl	$1, %eax
	movl	$73, %edx
	leaq	.LC10(%rip), %r9
	jmp	.L112
	.p2align 4,,10
.L155:
	cmpl	%eax, %esi
	jle	.L114
	movq	%r8, %rax
.L112:
	movb	%dl, -1(%rdi,%rax)
	movzbl	(%r9,%rax), %edx
	leaq	1(%rax), %r8
	movl	%eax, %ecx
	testb	%dl, %dl
	jne	.L155
.L114:
	testl	%r14d, %r14d
	jle	.L117
.L115:
	leaq	task_table(%rip), %r8
	xorl	%ebx, %ebx
	leaq	.LC4(%rip), %r15
	.p2align 4,,10
.L135:
	cmpl	$4, 24(%r8)
	jne	.L118
	cmpq	$0, 8(%r8)
	je	.L119
.L118:
	leal	3(%rcx), %r10d
	cmpl	%esi, %r10d
	jge	.L120
	movl	(%r8), %edx
	leal	1(%rcx), %r9d
	movslq	%ecx, %r12
	leaq	(%rdi,%r12), %r13
	movslq	%r9d, %r9
	leal	2(%rcx), %r11d
	addq	%rdi, %r9
	cmpl	$9, %edx
	ja	.L121
	movb	$32, 0(%r13)
	movl	(%r8), %eax
	addl	$48, %eax
	movb	%al, (%r9)
.L122:
	leal	4(%rcx), %r9d
	movslq	%r11d, %r11
	movslq	%r10d, %r10
	movb	$32, (%rdi,%r11)
	movb	$32, (%rdi,%r10)
	movq	8(%r8), %rdx
	testq	%rdx, %rdx
	je	.L141
	movzbl	(%rdx), %r10d
	testb	%r10b, %r10b
	je	.L156
.L123:
	cmpl	%r9d, %esi
	jle	.L128
	addl	$5, %ecx
	subq	%r12, %rdx
	xorl	%eax, %eax
	movslq	%ecx, %rcx
	movq	%rdx, %r12
	jmp	.L127
	.p2align 4,,10
.L157:
	addq	$1, %rcx
	testb	%dl, %dl
	je	.L128
.L127:
	movb	%r10b, -1(%rdi,%rcx)
	addl	$1, %eax
	movzbl	-4(%r12,%rcx), %r10d
	movl	%ecx, %r9d
	cmpl	$11, %eax
	setle	%dl
	cmpl	%ecx, %esi
	setg	%r11b
	andl	%r11d, %edx
	testb	%r10b, %r10b
	jne	.L157
.L126:
	testb	%dl, %dl
	je	.L128
	leal	1(%r9), %edx
	subl	%r9d, %eax
	movslq	%edx, %rdx
	.p2align 4,,10
.L129:
	leal	(%rax,%rdx), %ecx
	movl	%edx, %r9d
	movb	$32, -1(%rdi,%rdx)
	cmpl	$11, %ecx
	setle	%r10b
	cmpl	%edx, %esi
	setg	%cl
	addq	$1, %rdx
	testb	%cl, %r10b
	jne	.L129
.L128:
	movslq	24(%r8), %rax
	movq	%r15, %r11
	cmpl	$4, %eax
	ja	.L130
	movq	(%rsp,%rax,8), %r11
.L130:
	cmpl	%r9d, %esi
	jle	.L131
	addl	$1, %r9d
	movl	$1, %edx
	movslq	%r9d, %rax
.L132:
	movzbl	-1(%r11,%rdx), %ecx
	cmpl	$2, %edx
	movl	%eax, %r9d
	setle	%r10b
	cmpl	%eax, %esi
	movb	%cl, -1(%rdi,%rax)
	setg	%cl
	addq	$1, %rax
	addq	$1, %rdx
	testb	%cl, %r10b
	jne	.L132
.L131:
	leal	1(%r9), %edx
	movslq	%r9d, %rax
	movb	$32, (%rdi,%rax)
	leal	2(%r9), %eax
	movslq	%edx, %rdx
	movb	$32, (%rdi,%rdx)
	movslq	%eax, %rdx
	addq	%rdi, %rdx
	cmpl	%eax, %esi
	jle	.L133
	movl	28(%r8), %ecx
	leal	3(%r9), %eax
	addl	$48, %ecx
	movb	%cl, (%rdx)
	movslq	%eax, %rdx
	addq	%rdi, %rdx
.L133:
	movb	$32, (%rdx)
	leal	1(%rax), %edx
	leal	2(%rax), %ecx
	movslq	%edx, %rdx
	movb	$32, (%rdi,%rdx)
	movslq	%ecx, %rdx
	addq	%rdi, %rdx
	cmpl	%ecx, %esi
	jle	.L134
	leal	3(%rax), %ecx
	movl	40(%r8), %eax
	addl	$48, %eax
	movb	%al, (%rdx)
	movslq	%ecx, %rdx
	addq	%rdi, %rdx
.L134:
	addl	$1, %ecx
	movb	$10, (%rdx)
.L119:
	addl	$1, %ebx
	addq	$88, %r8
	cmpl	%r14d, %ebx
	jl	.L135
.L120:
	cmpl	%ecx, %esi
	jg	.L158
	testl	%ecx, %ecx
	jle	.L110
	movslq	%ecx, %rax
.L138:
	movb	$0, -1(%rdi,%rax)
.L110:
	leaq	-40(%rbp), %rsp
	movl	%ecx, %eax
	popq	%rbx
	popq	%r12
	popq	%r13
	popq	%r14
	popq	%r15
	popq	%rbp
	ret
	.p2align 4,,10
.L121:
	movl	$-858993459, %eax
	mull	%edx
	movl	$-858993459, %eax
	shrl	$3, %edx
	addl	$48, %edx
	movb	%dl, 0(%r13)
	movl	(%r8), %r13d
	mull	%r13d
	shrl	$3, %edx
	leal	(%rdx,%rdx,4), %eax
	addl	%eax, %eax
	subl	%eax, %r13d
	addl	$48, %r13d
	movb	%r13b, (%r9)
	jmp	.L122
	.p2align 4,,10
.L141:
	leaq	.LC3(%rip), %rdx
	movl	$63, %r10d
	jmp	.L123
.L158:
	movslq	%ecx, %rax
.L137:
	movb	$0, (%rdi,%rax)
	jmp	.L110
.L156:
	cmpl	%r9d, %esi
	setg	%dl
	xorl	%eax, %eax
	jmp	.L126
.L111:
	xorl	%ecx, %ecx
	testl	%r14d, %r14d
	jg	.L115
	jmp	.L110
.L117:
	cmpl	%ecx, %esi
	jg	.L137
	jmp	.L138
	.p2align 4,,15
	.globl	sched_get_task
	.def	sched_get_task;	.scl	2;	.type	32;	.endef
sched_get_task:
	testl	%edi, %edi
	js	.L162
	cmpl	%edi, task_count(%rip)
	jle	.L162
	leaq	task_table(%rip), %rax
	movslq	%edi, %rdi
	imulq	$88, %rdi, %rdi
	addq	%rax, %rdi
	movl	$0, %eax
	cmpl	$4, 24(%rdi)
	cmovne	%rdi, %rax
	ret
	.p2align 4,,10
.L162:
	xorl	%eax, %eax
	ret
	.p2align 4,,15
	.globl	sched_task_count
	.def	sched_task_count;	.scl	2;	.type	32;	.endef
sched_task_count:
	movl	task_count(%rip), %eax
	ret
	.p2align 4,,15
	.globl	sched_loop
	.def	sched_loop;	.scl	2;	.type	32;	.endef
sched_loop:
	pushq	%r13
	leaq	mlfq(%rip), %r13
	pushq	%r12
	leaq	sched_next(%rip), %r12
	pushq	%rbp
	leaq	task_table(%rip), %rbp
	pushq	%rbx
	subq	$8, %rsp
	.p2align 4,,10
.L166:
	call	*%r12
	testl	%eax, %eax
	js	.L167
.L177:
	movslq	%eax, %rbx
	imulq	$88, %rbx, %rax
	movq	16(%rbp,%rax), %rax
	testq	%rax, %rax
	je	.L168
	call	*%rax
.L168:
	imulq	$88, %rbx, %rax
	addq	%rbp, %rax
	movl	24(%rax), %edx
	testl	%edx, %edx
	jne	.L170
	movslq	28(%rax), %rsi
	movl	$1, 24(%rax)
	imulq	$528, %rsi, %rcx
	addq	%r13, %rcx
	movl	520(%rcx), %edi
	cmpl	$63, %edi
	jg	.L170
	imulq	$66, %rsi, %rsi
	movslq	516(%rcx), %r8
	addl	$1, %edi
	movl	%edi, 520(%rcx)
	addq	%r8, %rsi
	movq	%rax, 0(%r13,%rsi,8)
	leal	1(%r8), %eax
	cltd
	shrl	$26, %edx
	addl	%edx, %eax
	andl	$63, %eax
	subl	%edx, %eax
	movl	%eax, 516(%rcx)
.L170:
	movq	$0, current_task(%rip)
	call	*%r12
	testl	%eax, %eax
	jns	.L177
.L167:
/APP
 # 392 "src/kernel/sched/sched.c" 1
	pause
 # 0 "" 2
/NO_APP
	jmp	.L166
	.p2align 4,,15
	.globl	sched_requeue
	.def	sched_requeue;	.scl	2;	.type	32;	.endef
sched_requeue:
	testq	%rdi, %rdi
	je	.L178
	movl	24(%rdi), %eax
	testl	%eax, %eax
	jne	.L178
	movslq	28(%rdi), %rcx
	leaq	mlfq(%rip), %r8
	movl	$1, 24(%rdi)
	imulq	$528, %rcx, %rdx
	addq	%r8, %rdx
	movl	520(%rdx), %esi
	cmpl	$63, %esi
	jg	.L182
	movslq	516(%rdx), %r9
	imulq	$66, %rcx, %rcx
	addl	$1, %esi
	movl	%esi, 520(%rdx)
	movq	%r9, %rax
	addq	%r9, %rcx
	addl	$1, %eax
	movq	%rdi, (%r8,%rcx,8)
	movl	%eax, %ecx
	sarl	$31, %ecx
	shrl	$26, %ecx
	addl	%ecx, %eax
	andl	$63, %eax
	subl	%ecx, %eax
	movl	%eax, 516(%rdx)
.L182:
	cmpq	%rdi, current_task(%rip)
	je	.L183
.L178:
	ret
	.p2align 4,,10
.L183:
	movq	$0, current_task(%rip)
	ret
	.p2align 4,,15
	.globl	sched_load_sample
	.def	sched_load_sample;	.scl	2;	.type	32;	.endef
sched_load_sample:
	subq	$8, %rsp
	call	*.refptr.timer_ms(%rip)
	movq	sched_total_ticks(%rip), %rcx
	pxor	%xmm0, %xmm0
	testq	%rcx, %rcx
	je	.L184
	movq	sched_active_ticks(%rip), %rdx
	testq	%rdx, %rdx
	js	.L186
	testq	%rcx, %rcx
	pxor	%xmm0, %xmm0
	cvtsi2ssq	%rdx, %xmm0
	js	.L188
.L193:
	pxor	%xmm1, %xmm1
	cvtsi2ssq	%rcx, %xmm1
.L189:
	divss	%xmm1, %xmm0
	movq	%rax, %rdx
	subq	sched_sample_start_ms(%rip), %rdx
	cmpq	$99, %rdx
	jbe	.L184
	movq	$0, sched_active_ticks(%rip)
	movq	$0, sched_total_ticks(%rip)
	movq	%rax, sched_sample_start_ms(%rip)
.L184:
	addq	$8, %rsp
	ret
	.p2align 4,,10
.L186:
	movq	%rdx, %rsi
	andl	$1, %edx
	pxor	%xmm0, %xmm0
	shrq	%rsi
	orq	%rdx, %rsi
	testq	%rcx, %rcx
	cvtsi2ssq	%rsi, %xmm0
	addss	%xmm0, %xmm0
	jns	.L193
.L188:
	movq	%rcx, %rdx
	andl	$1, %ecx
	pxor	%xmm1, %xmm1
	shrq	%rdx
	orq	%rcx, %rdx
	cvtsi2ssq	%rdx, %xmm1
	addss	%xmm1, %xmm1
	jmp	.L189
.lcomm idle_task,8,8
.lcomm sched_sample_start_ms,8,8
.lcomm sched_total_ticks,8,8
.lcomm sched_active_ticks,8,8
	.globl	current_task
	.bss
	.align 8
current_task:
	.space 8
.lcomm task_count,4,4
.lcomm mlfq,2640,32
.lcomm task_table,5632,32
	.ident	"GCC: (x86_64-posix-seh-rev0, Built by MinGW-W64 project) 8.1.0"
	.section	.rdata$.refptr.timer_ms, "dr"
	.globl	.refptr.timer_ms
	.linkonce	discard
.refptr.timer_ms:
	.quad	timer_ms
	.section	.rdata$.refptr.vfs_fd_get, "dr"
	.globl	.refptr.vfs_fd_get
	.linkonce	discard
.refptr.vfs_fd_get:
	.quad	vfs_fd_get
	.section	.rdata$.refptr.vfs_fd_alloc, "dr"
	.globl	.refptr.vfs_fd_alloc
	.linkonce	discard
.refptr.vfs_fd_alloc:
	.quad	vfs_fd_alloc
	.section	.rdata$.refptr.devfs_open, "dr"
	.globl	.refptr.devfs_open
	.linkonce	discard
.refptr.devfs_open:
	.quad	devfs_open
