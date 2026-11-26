/**
 * \file
 * \brief The arm64 definitions that the classic code generator needs.
 */

#ifndef __MONO_MINI_TIER0_ARM64_H__
#define __MONO_MINI_TIER0_ARM64_H__

#include "mini-arm64.h"

#define MONO_ARCH_CPU_SPEC mono_arm64_cpu_desc

/* Parameters used by the register allocator */
/* r0..r7, r9..r14 (r15 is the imt/rgctx reg) */
#define MONO_ARCH_CALLEE_REGS 0xfeff

/* v16/v17 is reserved for a scratch reg */
#define MONO_ARCH_CALLEE_FREGS 0xfffc00ff
/* v8..v15 */
#define MONO_ARCH_CALLEE_SAVED_FREGS 0xff00

#define MONO_ARCH_CALLEE_SAVED_XREGS 0

#define MONO_ARCH_CALLEE_XREGS MONO_ARCH_CALLEE_FREGS

#define MONO_ARCH_USE_FPSTACK FALSE

#define MONO_ARCH_INST_SREG2_MASK(ins) (0)

#define MONO_ARCH_INST_FIXED_REG(desc) ((desc) == 'a' ? ARMREG_R0 : -1)

#define MONO_ARCH_INST_IS_REGPAIR(desc) (0)

#define MONO_ARCH_INST_IS_FLOAT(desc) ((desc) == 'f')

#define MONO_ARCH_INST_REGPAIR_REG2(desc,hreg1) (-1)

#define MONO_ARCH_CODE_ALIGNMENT 32

typedef struct MonoCompileArch {
	CallInfo *cinfo;
	int saved_gregs_offset;
	/* Points to arguments received on the stack */
	int args_reg;
	gboolean cond_branch_islands;
	MonoInst *vret_addr_loc;
	MonoInst *seq_point_info_var;
	MonoInst *ss_tramp_var;
	MonoInst *bp_tramp_var;
	guint8 *thunks;
	int thunks_size;
} MonoCompileArch;

#define MONO_ARCH_HAVE_OP_TAILCALL_MEMBASE 1
#define MONO_ARCH_HAVE_OP_TAILCALL_REG 1
#define MONO_ARCH_HAVE_GENERALIZED_IMT_TRAMPOLINE 1
#define MONO_ARCH_HAVE_FULL_AOT_TRAMPOLINES 1
#define MONO_ARCH_HAVE_EXCEPTIONS_INIT 1
#define MONO_ARCH_HAVE_GET_TRAMPOLINES 1
#define MONO_ARCH_HAVE_GENERAL_RGCTX_LAZY_FETCH_TRAMPOLINE 1
#define MONO_ARCH_HAVE_OP_GENERIC_CLASS_INIT 1
#define MONO_ARCH_HAVE_OPCODE_NEEDS_EMULATION 1
#define MONO_ARCH_HAVE_DECOMPOSE_LONG_OPTS 1
#define MONO_ARCH_FLOAT32_SUPPORTED 1

// Does the ABI have a volatile non-parameter register, so tailcall
// can pass context to generics or interfaces?
#define MONO_ARCH_HAVE_VOLATILE_NON_PARAM_REGISTER 1

#endif /* __MONO_MINI_TIER0_ARM64_H__ */
