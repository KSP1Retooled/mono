/**
 * \file
 *
 * Copyright 2013 Xamarin Inc
 *
 * Based on mini-arm.h:
 *
 * Copyright 2011 Xamarin Inc
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#ifndef __MONO_MINI_ARM64_H__
#define __MONO_MINI_ARM64_H__

#include <mono/arch/arm64/arm64-codegen.h>
#include <mono/utils/mono-sigcontext.h>
#include <mono/utils/mono-signal-handler.h>
#include <mono/utils/mono-context.h>
#include <glib.h>

/* Leave room for native crash reporting on the alternate signal stack. */
#define MONO_ARCH_SIGNAL_STACK_SIZE (64 * 1024)

#define MONO_MAX_IREGS 32
#define MONO_MAX_FREGS 32
#define MONO_MAX_XREGS 32

#define MONO_CONTEXT_SET_LLVM_EXC_REG(ctx, exc) do { (ctx)->regs [0] = (gsize)exc; } while (0)
#define MONO_CONTEXT_SET_LLVM_EH_SELECTOR_REG(ctx, sel) do { (ctx)->regs [1] = (gsize)(sel); } while (0)

#define MONO_INIT_CONTEXT_FROM_FUNC(ctx,func) do {	\
		MONO_CONTEXT_SET_BP ((ctx), __builtin_frame_address (0));	\
		MONO_CONTEXT_SET_SP ((ctx), __builtin_frame_address (0));	\
		MONO_CONTEXT_SET_IP ((ctx), (func));	\
	} while (0)

#define MONO_ARCH_INIT_TOP_LMF_ENTRY(lmf)

#define MONO_ARCH_FRAME_ALIGNMENT 16

/* r19..r28 */
#define MONO_ARCH_CALLEE_SAVED_REGS (0x3ff << 19)

/* callee saved regs + fp + sp */
#define MONO_ARCH_LMF_REGS ((0x3ff << 19) | (1 << ARMREG_FP) | (1 << ARMREG_SP))
#define MONO_ARCH_NUM_LMF_REGS (10 + 2)
#define MONO_ARCH_FIRST_LMF_REG ARMREG_R19
#define MONO_ARCH_LMF_REG_FP 10
#define MONO_ARCH_LMF_REG_SP 11

struct MonoLMF {
	/*
	 * If the second lowest bit is set to 1, then this is a MonoLMFExt structure, and
	 * the other fields are not valid.
	 */
	gpointer    previous_lmf;
	gpointer    lmf_addr;
	host_mgreg_t pc;
	host_mgreg_t gregs [MONO_ARCH_NUM_LMF_REGS];
};

/* Structure used by the sequence points in AOTed code */
struct SeqPointInfo {
	gpointer ss_tramp_addr;
	guint8* bp_addrs [MONO_ZERO_LEN_ARRAY];
};

#define PARAM_REGS 8
#define FP_PARAM_REGS 8

typedef struct {
	host_mgreg_t res, res2;
	guint8 *ret;
	double fpregs [FP_PARAM_REGS];
	int n_fpargs, n_fpret, n_stackargs;
	/* This should come last as the structure is dynamically extended */
	/* The +1 is for r8 */
	host_mgreg_t regs [PARAM_REGS + 1];
} DynCallArgs;

#define MONO_ARCH_EMULATE_FCONV_TO_U4 1
#define MONO_ARCH_EMULATE_FCONV_TO_U8 1
#define MONO_ARCH_NO_EMULATE_LONG_SHIFT_OPS 1
#define MONO_ARCH_NO_EMULATE_LONG_MUL_OPTS 1

#define MONO_ARCH_EMULATE_FREM 1
#define MONO_ARCH_EMULATE_LONG_MUL_OVF_OPTS 1
#define MONO_ARCH_NEED_DIV_CHECK 1
#define MONO_ARCH_EMULATE_MUL_OVF 1
#define MONO_ARCH_RGCTX_REG ARMREG_R15
#define MONO_ARCH_IMT_REG MONO_ARCH_RGCTX_REG
#define MONO_ARCH_VTABLE_REG ARMREG_R0
#define MONO_ARCH_USE_SIGACTION 1
#define MONO_ARCH_HAVE_CONTEXT_SET_INT_REG 1
#define MONO_ARCH_GSHARED_SUPPORTED 1
#define MONO_ARCH_AOT_SUPPORTED 1
#define MONO_ARCH_DYN_CALL_SUPPORTED 1
#define MONO_ARCH_DYN_CALL_PARAM_AREA 0
#define MONO_ARCH_SOFT_DEBUG_SUPPORTED 1
#define MONO_ARCH_GSHAREDVT_SUPPORTED 1
#define MONO_ARCH_HAVE_SETUP_RESUME_FROM_SIGNAL_HANDLER_CTX 1
#define MONO_ARCH_HAVE_SETUP_ASYNC_CALLBACK 1
#define MONO_ARCH_HAVE_SDB_TRAMPOLINES 1
#define MONO_ARCH_HAVE_PATCH_CODE_NEW 1
#define MONO_ARCH_HAVE_PATCH_JUMP_TRAMPOLINE 1
#define MONO_ARCH_HAVE_UNWIND_BACKTRACE 1

#define MONO_ARCH_REDZONE_SIZE 0

/* Relocations */
#define MONO_R_ARM64_B 1
#define MONO_R_ARM64_BCC 2
#define MONO_R_ARM64_IMM 3
#define MONO_R_ARM64_BL 4
#define MONO_R_ARM64_BL_SHORT 5
#define MONO_R_ARM64_CBZ 6

typedef enum {
	ArgInIReg,
	ArgInFReg,
	ArgInFRegR4,
	ArgOnStack,
	ArgOnStackR8,
	ArgOnStackR4,
	/*
	 * Vtype passed in consecutive int registers.
	 * ainfo->reg is the firs register,
	 * ainfo->nregs is the number of registers,
	 * ainfo->size is the size of the structure.
	 */
	ArgVtypeInIRegs,
	ArgVtypeByRef,
	ArgVtypeByRefOnStack,
	ArgVtypeOnStack,
	ArgHFA,
	/*
	 * A managed value type, placed one scalar at a time the way LLVM places a
	 * first-class struct. ainfo->leaves says where each scalar goes, and
	 * ainfo->size is the size of the local slot that holds the value.
	 */
	ArgVtypeInLeaves,
	/* Only in an ArgLeaf: a whole SIMD value in a q register */
	ArgInVReg,
	ArgNone
} ArgStorage;

/*
 * One scalar of a managed value type, and where the convention puts it.
 */
typedef struct {
	/* ArgInIReg, ArgInFReg, ArgInFRegR4, ArgInVReg or ArgOnStack */
	ArgStorage storage;
	int reg;
	/* The outgoing stack offset, when storage is ArgOnStack */
	int at;
	/* Where the scalar sits inside the value, and how many bytes it is */
	int offset;
	int size;
} ArgLeaf;

typedef struct {
	ArgStorage storage;
	int reg;
	/* ArgVtypeInLeaves */
	ArgLeaf *leaves;
	int nleaves;
	/* ArgOnStack */
	int offset;
	/* ArgVtypeInIRegs/ArgHFA */
	int nregs, size;
	/* ArgHFA */
	int esize;
	/* ArgHFA */
	/* The offsets of the float values inside the arg */
	guint16 foffsets [4];
	/* ArgOnStack */
	int slot_size;
	/* hfa */
	int nfregs_to_skip;
	gboolean sign;
	gboolean gsharedvt;
	gboolean hfa;
} ArgInfo;

struct CallInfo {
	int nargs;
	int gr, fr, stack_usage;
	gboolean pinvoke, vararg;
	ArgInfo ret;
	ArgInfo sig_cookie;
	ArgInfo args [1];
};

typedef struct {
	/* General registers + ARMREG_R8 for indirect returns */
	host_mgreg_t gregs [PARAM_REGS + 1];
	/* Floating registers */
	double fregs [FP_PARAM_REGS];
	/* Stack usage, used for passing params on stack */
	guint32 stack_size;
	guint8* stack;
} CallContext;

#include <mono/mini/mini-arm64-gsharedvt.h>

guint8* mono_arm_emit_imm64 (guint8 *code, int dreg, gint64 imm);

guint8* mono_arm_emit_ldrx (guint8 *code, int rt, int rn, int imm);

guint8* mono_arm_emit_destroy_frame (guint8 *code, int stack_offset, guint64 temp_regs);

guint8* mono_arm_emit_store_regset (guint8 *code, guint64 regs, int basereg, int offset);

guint8* mono_arm_emit_store_regarray (guint8 *code, guint64 regs, int basereg, int offset);

guint8* mono_arm_emit_load_regarray (guint8 *code, guint64 regs, int basereg, int offset);

guint8* mono_arm_emit_brx (guint8 *code, int reg);

guint8* mono_arm_emit_blrx (guint8 *code, int reg);

void mono_arm_patch (guint8 *code, guint8 *target, int relocation);

/* The single step trampoline */
extern gpointer mono_arm64_ss_trampoline;

/* The breakpoint trampoline */
extern gpointer mono_arm64_bp_trampoline;

/* Writes a far-jump thunk to target at code and returns the end of it. */
guint8* mono_arm64_emit_thunk (guint8 *code, gconstpointer target);

/*
 * Takes the next free thunk from the method cfg is compiling, writes a jump to
 * target into it and returns it. Defined by the code generator, which owns
 * MonoCompile.
 */
struct MonoCompile;
gpointer mono_arm64_tier0_create_thunk (struct MonoCompile *cfg, guint8 *code, gconstpointer target);

void mono_arm_throw_exception (gpointer arg, host_mgreg_t pc, host_mgreg_t *int_regs, gdouble *fp_regs, gboolean corlib, gboolean rethrow, gboolean preserve_ips);

void mono_arm_gsharedvt_init (void);

GSList* mono_arm_get_exception_trampolines (gboolean aot);

void mono_arm_resume_unwind (gpointer arg, host_mgreg_t pc, host_mgreg_t *int_regs, gdouble *fp_regs, gboolean corlib, gboolean rethrow);

CallInfo* mono_arch_get_call_info (MonoMemPool *mp, MonoMethodSignature *sig);

#define MONO_TRAMPOLINE_UNWINDINFO_SIZE(max_code_count) 0
#define MONO_MAX_TRAMPOLINE_UNWINDINFO_SIZE 0

static inline gboolean
mono_arch_unwindinfo_validate_size (GSList *unwind_ops, guint max_size)
{
	return TRUE;
}

#endif /* __MONO_MINI_ARM64_H__ */
