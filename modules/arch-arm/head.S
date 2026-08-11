#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/pgtable.h>

#include <asm/assembler.h>
#include <asm/cp15.h>
#include <asm/domain.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/page.h>
#include <asm/thread_info.h>

#if defined(CONFIG_DEBUG_LL) && !defined(CONFIG_DEBUG_SEMIHOSTING)
#include CONFIG_DEBUG_LL_INCLUDE
#endif
#define KERNEL_RAM_VADDR	(KERNEL_OFFSET + TEXT_OFFSET)
#if (KERNEL_RAM_VADDR & 0xffff) != 0x8000
#error KERNEL_RAM_VADDR must start at 0xXXXX8000
#endif

#ifdef CONFIG_ARM_LPAE
#define PG_DIR_SIZE	0x5000
#define PMD_ENTRY_ORDER	3
#else
#define PG_DIR_SIZE	0x4000
#define PMD_ENTRY_ORDER	2
#endif

	.globl	swapper_pg_dir
	.equ	swapper_pg_dir, KERNEL_RAM_VADDR - PG_DIR_SIZE

	.pushsection .data
	.align	2
	.globl	kernel_sec_start
	.globl	kernel_sec_end
kernel_sec_start:
	.long	0
	.long	0
kernel_sec_end:
	.long	0
	.long	0
	.popsection

	.macro	pgtbl, rd, phys
	add	\rd, \phys, #TEXT_OFFSET
	sub	\rd, \rd, #PG_DIR_SIZE
	.endm

	.arm

	__HEAD
ENTRY(stext)
 ARM_BE8(setend	be )

 THUMB(	badr	r9, 1f		)
 THUMB(	bx	r9		)
 THUMB(	.thumb			)
 THUMB(1:			)

#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install
#endif
	safe_svcmode_maskall r9

	mrc	p15, 0, r9, c0, c0
	bl	__lookup_processor_type
	movs	r10, r5
 THUMB( it	eq )
	beq	__error_p

#ifdef CONFIG_ARM_LPAE
	mrc	p15, 0, r3, c0, c1, 4
	and	r3, r3, #0xf
	cmp	r3, #5
 THUMB( it	lo )
	blo	__error_lpae
#endif

#ifndef CONFIG_XIP_KERNEL
	adr_l	r8, _text
	sub	r8, r8, #TEXT_OFFSET
#else
	ldr	r8, =PLAT_PHYS_OFFSET
#endif

	bl	__vet_atags
#ifdef CONFIG_SMP_ON_UP
	bl	__fixup_smp
#endif
#ifdef CONFIG_ARM_PATCH_PHYS_VIRT
	bl	__fixup_pv_table
#endif
	bl	__create_page_tables

	ldr	r13, =__mmap_switched
	badr	lr, 1f
#ifdef CONFIG_ARM_LPAE
	mov	r5, #0
	mov	r8, r4, lsr #12
#else
	mov	r8, r4
#endif
	ldr	r12, [r10, #PROCINFO_INITFUNC]
	add	r12, r12, r10
	ret	r12
1:	b	__enable_mmu
ENDPROC(stext)
	.ltorg

__create_page_tables:
	pgtbl	r4, r8

	mov	r0, r4
	mov	r3, #0
	add	r6, r0, #PG_DIR_SIZE
1:	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	teq	r0, r6
	bne	1b

#ifdef CONFIG_ARM_LPAE
	mov	r0, r4
	add	r3, r4, #0x1000
	orr	r3, r3, #3
	mov	r6, #4
	mov	r7, #1 << (55 - 32)
1:
#ifdef CONFIG_CPU_ENDIAN_BE8
	str	r7, [r0], #4
	str	r3, [r0], #4
#else
	str	r3, [r0], #4
	str	r7, [r0], #4
#endif
	add	r3, r3, #0x1000
	subs	r6, r6, #1
	bne	1b

	add	r4, r4, #0x1000
#ifdef CONFIG_CPU_ENDIAN_BE8
	add	r4, r4, #4
#endif
#endif

	ldr	r7, [r10, #PROCINFO_MM_MMUFLAGS]

	adr_l	r5, __turn_mmu_on
	adr_l	r6, __turn_mmu_on_end
	mov	r5, r5, lsr #SECTION_SHIFT
	mov	r6, r6, lsr #SECTION_SHIFT

1:	orr	r3, r7, r5, lsl #SECTION_SHIFT
	str	r3, [r4, r5, lsl #PMD_ENTRY_ORDER]
	cmp	r5, r6
	addlo	r5, r5, #1
	blo	1b

	ldr	r3, =0x700c
	bic	r3, r7, r3
	ldr	r6, =(0x40E00000 >> SECTION_SHIFT)
	orr	r5, r3, r6, lsl #SECTION_SHIFT
	str	r5, [r4, r6, lsl #PMD_ENTRY_ORDER]
	ldr	r6, =(0x10800000 >> SECTION_SHIFT)
	orr	r5, r3, r6, lsl #SECTION_SHIFT
	str	r5, [r4, r6, lsl #PMD_ENTRY_ORDER]

	add	r0, r4, #KERNEL_OFFSET >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
	ldr	r6, =(_end - 1)

#ifndef CONFIG_XIP_KERNEL
	adr_l	r5, kernel_sec_start
#if defined CONFIG_CPU_ENDIAN_BE8 || defined CONFIG_CPU_ENDIAN_BE32
	str	r8, [r5, #4]
#else
	str	r8, [r5]
#endif
#endif
	orr	r3, r8, r7
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ENTRY_ORDER)
1:	str	r3, [r0], #1 << PMD_ENTRY_ORDER
	add	r3, r3, #1 << SECTION_SHIFT
	cmp	r0, r6
	bls	1b
#ifndef CONFIG_XIP_KERNEL
	eor	r3, r3, r7
	adr_l	r5, kernel_sec_end
#if defined CONFIG_CPU_ENDIAN_BE8 || defined CONFIG_CPU_ENDIAN_BE32
	str	r3, [r5, #4]
#else
	str	r3, [r5]
#endif
#else
#define XIP_START XIP_VIRT_ADDR(CONFIG_XIP_PHYS_ADDR)
	mov	r3, pc
	mov	r3, r3, lsr #SECTION_SHIFT
	orr	r3, r7, r3, lsl #SECTION_SHIFT
	add	r0, r4,  #(XIP_START & 0xff000000) >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
	str	r3, [r0, #((XIP_START & 0x00f00000) >> SECTION_SHIFT) << PMD_ENTRY_ORDER]!
	ldr	r6, =(_edata_loc - 1)
	add	r0, r0, #1 << PMD_ENTRY_ORDER
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ENTRY_ORDER)
1:	cmp	r0, r6
	add	r3, r3, #1 << SECTION_SHIFT
	strls	r3, [r0], #1 << PMD_ENTRY_ORDER
	bls	1b
#endif

	mov	r0, r2, lsr #SECTION_SHIFT
	cmp	r2, #0
	ldrne	r3, =FDT_FIXED_BASE >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
	addne	r3, r3, r4
	orrne	r6, r7, r0, lsl #SECTION_SHIFT
	strne	r6, [r3], #1 << PMD_ENTRY_ORDER
	addne	r6, r6, #1 << SECTION_SHIFT
	strne	r6, [r3]

#if defined(CONFIG_ARM_LPAE) && defined(CONFIG_CPU_ENDIAN_BE8)
	sub	r4, r4, #4
#endif

#ifdef CONFIG_DEBUG_LL
#if !defined(CONFIG_DEBUG_ICEDCC) && !defined(CONFIG_DEBUG_SEMIHOSTING)
	addruart r7, r3, r0

	mov	r3, r3, lsr #SECTION_SHIFT
	mov	r3, r3, lsl #PMD_ENTRY_ORDER

	add	r0, r4, r3
	mov	r3, r7, lsr #SECTION_SHIFT
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS]
	orr	r3, r7, r3, lsl #SECTION_SHIFT
#ifdef CONFIG_ARM_LPAE
	mov	r7, #1 << (54 - 32)
#ifdef CONFIG_CPU_ENDIAN_BE8
	str	r7, [r0], #4
	str	r3, [r0], #4
#else
	str	r3, [r0], #4
	str	r7, [r0], #4
#endif
#else
	orr	r3, r3, #PMD_SECT_XN
	str	r3, [r0], #4
#endif

#else
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS]
#endif

#if defined(CONFIG_ARCH_NETWINDER)
	add	r0, r4, #0xff000000 >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
	orr	r3, r7, #0x7c000000
	str	r3, [r0]
#endif
#ifdef CONFIG_ARCH_RPC
	add	r0, r4, #0x02000000 >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
	orr	r3, r7, #0x02000000
	str	r3, [r0]
	add	r0, r4, #0xd8000000 >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
	str	r3, [r0]
#endif
#endif
#ifdef CONFIG_ARM_LPAE
	sub	r4, r4, #0x1000
#endif
	ret	lr
ENDPROC(__create_page_tables)
	.ltorg

#if defined(CONFIG_SMP)
	.text
	.arm
ENTRY(secondary_startup_arm)
 THUMB(	badr	r9, 1f		)
 THUMB(	bx	r9		)
 THUMB(	.thumb			)
 THUMB(1:			)
ENTRY(secondary_startup)

 ARM_BE8(setend	be)

#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install_secondary
#endif
	safe_svcmode_maskall r9

	mrc	p15, 0, r9, c0, c0
	bl	__lookup_processor_type
	movs	r10, r5
	moveq	r0, #'p'
 THUMB( it	eq )
	beq	__error_p

#ifdef CONFIG_XIP_KERNEL
	ldr	r3, =(secondary_data + PLAT_PHYS_OFFSET - PAGE_OFFSET)
#else
	adr_l	r3, secondary_data
#endif
	mov_l	r12, __secondary_switched
	ldrd	r4, r5, [r3, #0]
ARM_BE8(eor	r4, r4, r5)
ARM_BE8(eor	r5, r4, r5)
ARM_BE8(eor	r4, r4, r5)
	ldr	r8, [r3, #8]
	badr	lr, __enable_mmu
	mov	r13, r12
	ldr	r12, [r10, #PROCINFO_INITFUNC]
	add	r12, r12, r10
	ret	r12
ENDPROC(secondary_startup)
ENDPROC(secondary_startup_arm)

ENTRY(__secondary_switched)
#if defined(CONFIG_VMAP_STACK) && !defined(CONFIG_ARM_LPAE)
	mrc	p15, 0, ip, c2, c0, 1
	mcr	p15, 0, ip, c2, c0, 0
	instr_sync
#endif
	adr_l	r7, secondary_data + 12
	ldr	sp, [r7]
	ldr	r0, [r7, #4]
	mov	fp, #0
	b	secondary_start_kernel
ENDPROC(__secondary_switched)

#endif

__enable_mmu:
#if defined(CONFIG_ALIGNMENT_TRAP) && __LINUX_ARM_ARCH__ < 6
	orr	r0, r0, #CR_A
#else
	bic	r0, r0, #CR_A
#endif
#ifdef CONFIG_CPU_DCACHE_DISABLE
	bic	r0, r0, #CR_C
#endif
#ifdef CONFIG_CPU_BPREDICT_DISABLE
	bic	r0, r0, #CR_Z
#endif
#ifdef CONFIG_CPU_ICACHE_DISABLE
	bic	r0, r0, #CR_I
#endif
#ifdef CONFIG_ARM_LPAE
	mcrr	p15, 0, r4, r5, c2
#else
	mov	r5, #DACR_INIT
	mcr	p15, 0, r5, c3, c0, 0
	mcr	p15, 0, r4, c2, c0, 0
#endif
	b	__turn_mmu_on
ENDPROC(__enable_mmu)

	.align	5
	.pushsection	.idmap.text, "ax"
ENTRY(__turn_mmu_on)
	mov	r0, r0
	instr_sync
	mcr	p15, 0, r0, c1, c0, 0
	mrc	p15, 0, r3, c0, c0, 0
	instr_sync
	mov	r3, r3
	mov	r3, r13
	ret	r3
__turn_mmu_on_end:
ENDPROC(__turn_mmu_on)
	.popsection

#ifdef CONFIG_SMP_ON_UP
	__HEAD
__fixup_smp:
	and	r3, r9, #0x000f0000
	teq	r3, #0x000f0000
	bne	__fixup_smp_on_up

	bic	r3, r9, #0x00ff0000
	bic	r3, r3, #0x0000000f
	mov	r4, #0x41000000
	orr	r4, r4, #0x0000b000
	orr	r4, r4, #0x00000020
	teq	r3, r4
	reteq	lr

	mrc	p15, 0, r0, c0, c0, 5
	and	r0, r0, #0xc0000000
	teq	r0, #0x80000000
	bne    __fixup_smp_on_up

	mov	r4, #0x41000000
	orr	r4, r4, #0x0000c000
	orr	r4, r4, #0x00000090
	teq	r3, r4
	retne	lr

	mrc	p15, 4, r0, c15, c0
	teq	r0, #0x0
	beq	__fixup_smp_on_up
	ldr	r0, [r0, #4]
ARM_BE8(rev	r0, r0)
	and	r0, r0, #0x3
	teq	r0, #0x0
	retne	lr

__fixup_smp_on_up:
	adr_l	r4, __smpalt_begin
	adr_l	r5, __smpalt_end
	b	__do_fixup_smp_on_up
ENDPROC(__fixup_smp)

	.pushsection .data
	.align	2
	.globl	smp_on_up
smp_on_up:
	ALT_SMP(.long	1)
	ALT_UP(.long	0)
	.popsection
#endif

	.text
__do_fixup_smp_on_up:
	cmp	r4, r5
	reths	lr
	ldmia	r4, {r0, r6}
 ARM(	str	r6, [r0, r4]	)
 THUMB(	add	r0, r0, r4	)
	add	r4, r4, #8
#ifdef __ARMEB__
 THUMB(	mov	r6, r6, ror #16	)
#endif
 THUMB(	strh	r6, [r0], #2	)
 THUMB(	mov	r6, r6, lsr #16	)
 THUMB(	strh	r6, [r0]	)
	b	__do_fixup_smp_on_up
ENDPROC(__do_fixup_smp_on_up)

ENTRY(fixup_smp)
	stmfd	sp!, {r4 - r6, lr}
	mov	r4, r0
	add	r5, r0, r1
	bl	__do_fixup_smp_on_up
	ldmfd	sp!, {r4 - r6, pc}
ENDPROC(fixup_smp)

#include "head-common.S"
