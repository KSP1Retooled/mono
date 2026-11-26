/**
 * \file
 * The arm64 support the runtime needs whether or not a code generator runs:
 * the calling convention, dyn calls, the hand-emitted delegate and IMT stubs,
 * call-site patching and the debugger's breakpoint hooks.
 *
 * Copyright 2013 Xamarin, Inc (http://www.xamarin.com)
 *
 * Based on mini-arm.c:
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2003 Ximian, Inc.
 * Copyright 2003-2011 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <config.h>

#include "mini.h"
#include "aot-runtime.h"
#include "mini-runtime.h"
#include "mini-arm64-emit.h"

#include <mono/arch/arm64/arm64-codegen.h>
#include <mono/utils/mono-mmap.h>
#include <mono/utils/mono-memory-model.h>
#include <mono/metadata/abi-details.h>
#include <mono/metadata/marshal-ilgen.h>

#define THUNK_SIZE (4 * 4)

#define MAX_ARCH_DELEGATE_PARAMS 7

gpointer mono_arm64_ss_trampoline;
gpointer mono_arm64_bp_trampoline;

const char*
mono_arch_regname (int reg)
{
	static const char * rnames[] = {
		"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9",
		"r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19",
		"r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "fp",
		"lr", "sp"
	};
	if (reg >= 0 && reg < 32)
		return rnames [reg];
	return "unknown";
}

const char*
mono_arch_fregname (int reg)
{
	static const char * rnames[] = {
		"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9",
		"d10", "d11", "d12", "d13", "d14", "d15", "d16", "d17", "d18", "d19",
		"d20", "d21", "d22", "d23", "d24", "d25", "d26", "d27", "d28", "d29",
		"d30", "d31"
	};
	if (reg >= 0 && reg < 32)
		return rnames [reg];
	return "unknown fp";
}

const char *
mono_arch_xregname (int reg)
{
	static const char * rnames[] = {
		"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9",
		"v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19",
		"v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29",
		"v30", "v31"
	};
	if (reg >= 0 && reg < 32)
		return rnames [reg];
	return "unknown";
}

gboolean
mono_is_regsize_var (MonoType *t)
{
	t = mini_get_underlying_type (t);
	switch (t->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
		return TRUE;
	case MONO_TYPE_GENERICINST:
		return !mono_type_generic_inst_is_valuetype (t);
	default:
		return FALSE;
	}
}

static gpointer
get_delegate_invoke_impl (gboolean has_target, gboolean param_count, guint32 *code_size)
{
	guint8 *code, *start;

	MINI_BEGIN_CODEGEN ();

	if (has_target) {
		start = code = mono_global_codeman_reserve (12);

		/* Replace the this argument with the target */
		arm_ldrx (code, ARMREG_IP0, ARMREG_R0, MONO_STRUCT_OFFSET (MonoDelegate, method_ptr));
		arm_ldrx (code, ARMREG_R0, ARMREG_R0, MONO_STRUCT_OFFSET (MonoDelegate, target));
		code = mono_arm_emit_brx (code, ARMREG_IP0);

		g_assert ((code - start) <= 12);
	} else {
		int size, i;

		size = 8 + param_count * 4;
		start = code = mono_global_codeman_reserve (size);

		arm_ldrx (code, ARMREG_IP0, ARMREG_R0, MONO_STRUCT_OFFSET (MonoDelegate, method_ptr));
		/* slide down the arguments */
		for (i = 0; i < param_count; ++i)
			arm_movx (code, i, i + 1);
		code = mono_arm_emit_brx (code, ARMREG_IP0);

		g_assert ((code - start) <= size);
	}
	MINI_END_CODEGEN (start, code - start, MONO_PROFILER_CODE_BUFFER_DELEGATE_INVOKE, NULL);

	if (code_size)
		*code_size = code - start;

	return MINI_ADDR_TO_FTNPTR (start);
}

/*
 * mono_arch_get_delegate_invoke_impls:
 *
 *   Return a list of MonoAotTrampInfo structures for the delegate invoke impl
 * trampolines.
 */
GSList*
mono_arch_get_delegate_invoke_impls (void)
{
	GSList *res = NULL;
	guint8 *code;
	guint32 code_len;
	int i;
	char *tramp_name;

	code = (guint8*)get_delegate_invoke_impl (TRUE, 0, &code_len);
	res = g_slist_prepend (res, mono_tramp_info_create ("delegate_invoke_impl_has_target", code, code_len, NULL, NULL));

	for (i = 0; i <= MAX_ARCH_DELEGATE_PARAMS; ++i) {
		code = (guint8*)get_delegate_invoke_impl (FALSE, i, &code_len);
		tramp_name = g_strdup_printf ("delegate_invoke_impl_target_%d", i);
		res = g_slist_prepend (res, mono_tramp_info_create (tramp_name, code, code_len, NULL, NULL));
		g_free (tramp_name);
	}

	return res;
}

gpointer
mono_arch_get_delegate_invoke_impl (MonoMethodSignature *sig, gboolean has_target)
{
	guint8 *code, *start;

	/*
	 * vtypes are returned in registers, or using the dedicated r8 register, so
	 * they can be supported by delegate invokes.
	 */

	if (has_target) {
		static guint8* cached = NULL;

		if (cached)
			return cached;

		if (mono_ee_features.use_aot_trampolines)
			start = (guint8*)mono_aot_get_trampoline ("delegate_invoke_impl_has_target");
		else
			start = (guint8*)get_delegate_invoke_impl (TRUE, 0, NULL);
		mono_memory_barrier ();
		cached = start;
		return cached;
	} else {
		static guint8* cache [MAX_ARCH_DELEGATE_PARAMS + 1] = {NULL};
		int i;

		if (sig->param_count > MAX_ARCH_DELEGATE_PARAMS)
			return NULL;
		for (i = 0; i < sig->param_count; ++i)
			if (!mono_is_regsize_var (sig->params [i]))
				return NULL;

		code = cache [sig->param_count];
		if (code)
			return code;

		if (mono_ee_features.use_aot_trampolines) {
			char *name = g_strdup_printf ("delegate_invoke_impl_target_%d", sig->param_count);
			start = (guint8*)mono_aot_get_trampoline (name);
			g_free (name);
		} else {
			start = (guint8*)get_delegate_invoke_impl (FALSE, sig->param_count, NULL);
		}
		mono_memory_barrier ();
		cache [sig->param_count] = start;
		return start;
	}

	return NULL;
}

gpointer
mono_arch_get_delegate_virtual_invoke_impl (MonoMethodSignature *sig, MonoMethod *method, int offset, gboolean load_imt_reg)
{
	return NULL;
}

gpointer
mono_arch_get_this_arg_from_call (host_mgreg_t *regs, guint8 *code)
{
	return (gpointer)regs [ARMREG_R0];
}

void
mono_arch_cpu_init (void)
{
}

void
mono_arch_init (void)
{
	if (!mono_aot_only)
		mono_arm64_bp_trampoline = mini_get_breakpoint_trampoline ();

	mono_arm_gsharedvt_init ();
}

void
mono_arch_cleanup (void)
{
}

guint32
mono_arch_cpu_optimizations (guint32 *exclude_mask)
{
	*exclude_mask = 0;
	return 0;
}

void
mono_arch_register_lowlevel_calls (void)
{
}

void
mono_arch_finish_init (void)
{
}

guint8*
mono_arm_emit_imm64 (guint8 *code, int dreg, gint64 imm)
{
	return emit_imm64 (code, dreg, imm);
}

guint8*
mono_arm_emit_ldrx (guint8 *code, int rt, int rn, int imm)
{
	return emit_ldrx (code, rt, rn, imm);
}

gboolean
mono_arch_have_fast_tls (void)
{
	return TRUE;
}

/*
 * Emits
 * - mov sp, fp
 * - ldrp [fp, lr], [sp], !stack_offfset
 * Clobbers TEMP_REGS.
 */
__attribute__ ((__warn_unused_result__)) guint8*
mono_arm_emit_destroy_frame (guint8 *code, int stack_offset, guint64 temp_regs)
{
	// At least one of these registers must be available, or both.
	gboolean const temp0 = (temp_regs & (1 << ARMREG_IP0)) != 0;
	gboolean const temp1 = (temp_regs & (1 << ARMREG_IP1)) != 0;
	g_assert (temp0 || temp1);
	int const temp = temp0 ? ARMREG_IP0 : ARMREG_IP1;

	arm_movspx (code, ARMREG_SP, ARMREG_FP);

	if (arm_is_ldpx_imm (stack_offset)) {
		arm_ldpx_post (code, ARMREG_FP, ARMREG_LR, ARMREG_SP, stack_offset);
	} else {
		arm_ldpx (code, ARMREG_FP, ARMREG_LR, ARMREG_SP, 0);
		/* sp += stack_offset */
		if (temp0 && temp1) {
			code = emit_addx_sp_imm (code, stack_offset);
		} else {
			int imm = stack_offset;

			/* Can't use addx_sp_imm () since we can't clobber both ip0/ip1 */
			arm_addx_imm (code, temp, ARMREG_SP, 0);
			while (imm > 256) {
				arm_addx_imm (code, temp, temp, 256);
				imm -= 256;
			}
			arm_addx_imm (code, ARMREG_SP, temp, imm);
		}
	}
	return code;
}

guint8*
mono_arm64_emit_thunk (guint8 *code, gconstpointer target)
{
	guint8 *p = code;

	arm_ldrx_lit (code, ARMREG_IP0, code + 8);
	arm_brx (code, ARMREG_IP0);
	*(guint64*)code = (guint64)target;
	code += sizeof (guint64);

	mono_arch_flush_icache (p, code - p);
	return code;
}

static gpointer
create_thunk (MonoCompile *cfg, MonoDomain *domain, guchar *code, const guchar *target)
{
	MonoJitInfo *ji;
	MonoThunkJitInfo *info;
	guint8 *thunks, *p;
	int thunks_size;
	guint8 *orig_target;
	guint8 *target_thunk;

	if (!domain)
		domain = mono_domain_get ();

	if (cfg) {
		return mono_arm64_tier0_create_thunk (cfg, code, target);
	} else {
		ji = mini_jit_info_table_find (domain, (char*)code, NULL);
		g_assert (ji);
		info = mono_jit_info_get_thunk_info (ji);
		g_assert (info);

		thunks = (guint8*)ji->code_start + info->thunks_offset;
		thunks_size = info->thunks_size;

		orig_target = mono_arch_get_call_target (code + 4);

		mono_domain_lock (domain);

		target_thunk = NULL;
		if (orig_target >= thunks && orig_target < thunks + thunks_size) {
			/* The call already points to a thunk, because of trampolines etc. */
			target_thunk = orig_target;
		} else {
			for (p = thunks; p < thunks + thunks_size; p += THUNK_SIZE) {
				if (((guint32*)p) [0] == 0) {
					/* Free entry */
					target_thunk = p;
					break;
				} else if (((guint64*)p) [1] == (guint64)target) {
					/* Thunk already points to target */
					target_thunk = p;
					break;
				}
			}
		}

		//printf ("THUNK: %p %p %p\n", code, target, target_thunk);

		if (!target_thunk) {
			mono_domain_unlock (domain);
			g_print ("thunk failed %p->%p, thunk space=%d method %s", code, target, thunks_size, mono_method_full_name (jinfo_get_method (ji), TRUE));
			g_assert_not_reached ();
		}

		mono_arm64_emit_thunk (target_thunk, target);

		mono_domain_unlock (domain);

		return target_thunk;
	}
}

static void
arm_patch_full (MonoCompile *cfg, MonoDomain *domain, guint8 *code, guint8 *target, int relocation)
{
	switch (relocation) {
	case MONO_R_ARM64_B:
		target = MINI_FTNPTR_TO_ADDR (target);
		if (arm_is_bl_disp (code, target)) {
			arm_b (code, target);
		} else {
			gpointer thunk;

			thunk = create_thunk (cfg, domain, code, target);
			g_assert (arm_is_bl_disp (code, thunk));
			arm_b (code, thunk);
		}
		break;
	case MONO_R_ARM64_BCC: {
		int cond;

		cond = arm_get_bcc_cond (code);
		arm_bcc (code, cond, target);
		break;
	}
	case MONO_R_ARM64_CBZ:
		arm_set_cbz_target (code, target);
		break;
	case MONO_R_ARM64_IMM: {
		guint64 imm = (guint64)target;
		int dreg;

		/* emit_imm64_template () */
		dreg = arm_get_movzx_rd (code);
		arm_movzx (code, dreg, imm & 0xffff, 0);
		arm_movkx (code, dreg, (imm >> 16) & 0xffff, 16);
		arm_movkx (code, dreg, (imm >> 32) & 0xffff, 32);
		arm_movkx (code, dreg, (imm >> 48) & 0xffff, 48);
		break;
	}
	case MONO_R_ARM64_BL:
		target = MINI_FTNPTR_TO_ADDR (target);
		if (arm_is_bl_disp (code, target)) {
			arm_bl (code, target);
		} else {
			gpointer thunk;

			thunk = create_thunk (cfg, domain, code, target);
			g_assert (arm_is_bl_disp (code, thunk));
			arm_bl (code, thunk);
		}
		break;
	default:
		g_assert_not_reached ();
	}
}

static void
arm_patch_rel (guint8 *code, guint8 *target, int relocation)
{
	arm_patch_full (NULL, NULL, code, target, relocation);
}

void
mono_arm_patch (guint8 *code, guint8 *target, int relocation)
{
	arm_patch_rel (code, target, relocation);
}

void
mono_arch_patch_code_new (MonoCompile *cfg, MonoDomain *domain, guint8 *code, MonoJumpInfo *ji, gpointer target)
{
	guint8 *ip;

	ip = ji->ip.i + code;

	switch (ji->type) {
	case MONO_PATCH_INFO_METHOD_JUMP:
		/* ji->relocation is not set by the caller */
		arm_patch_full (cfg, domain, ip, (guint8*)target, MONO_R_ARM64_B);
		mono_arch_flush_icache (ip, 8);
		break;
	default:
		arm_patch_full (cfg, domain, ip, (guint8*)target, ji->relocation);
		break;
	case MONO_PATCH_INFO_NONE:
		break;
	}
}

void
mono_arch_flush_register_windows (void)
{
}

MonoMethod*
mono_arch_find_imt_method (host_mgreg_t *regs, guint8 *code)
{
	return (MonoMethod*)regs [MONO_ARCH_RGCTX_REG];
}

MonoVTable*
mono_arch_find_static_call_vtable (host_mgreg_t *regs, guint8 *code)
{
	return (MonoVTable*)regs [MONO_ARCH_RGCTX_REG];
}

GSList*
mono_arch_get_cie_program (void)
{
	GSList *l = NULL;

	mono_add_unwind_op_def_cfa (l, (guint8*)NULL, (guint8*)NULL, ARMREG_SP, 0);

	return l;
}

host_mgreg_t
mono_arch_context_get_int_reg (MonoContext *ctx, int reg)
{
	return ctx->regs [reg];
}

void
mono_arch_context_set_int_reg (MonoContext *ctx, int reg, host_mgreg_t val)
{
	ctx->regs [reg] = val;
}

static void
add_general (CallInfo *cinfo, ArgInfo *ainfo, int size, gboolean sign)
{
	if (cinfo->gr >= PARAM_REGS) {
		ainfo->storage = ArgOnStack;
		/* Put arguments into 8 byte aligned stack slots */
		size = 8;
		sign = FALSE;
		cinfo->stack_usage = ALIGN_TO (cinfo->stack_usage, size);
		ainfo->offset = cinfo->stack_usage;
		ainfo->slot_size = size;
		ainfo->sign = sign;
		cinfo->stack_usage += size;
	} else {
		ainfo->storage = ArgInIReg;
		ainfo->reg = cinfo->gr;
		cinfo->gr ++;
	}
}

static void
add_fp (CallInfo *cinfo, ArgInfo *ainfo, gboolean single)
{
	if (cinfo->fr >= FP_PARAM_REGS) {
		ainfo->storage = single ? ArgOnStackR4 : ArgOnStackR8;
		ainfo->offset = cinfo->stack_usage;
		ainfo->slot_size = 8;
		/* Put arguments into 8 byte aligned stack slots */
		cinfo->stack_usage += 8;
	} else {
		if (single)
			ainfo->storage = ArgInFRegR4;
		else
			ainfo->storage = ArgInFReg;
		ainfo->reg = cinfo->fr;
		cinfo->fr ++;
	}
}

static gboolean
is_hfa (MonoType *t, int *out_nfields, int *out_esize, int *field_offsets)
{
	MonoClass *klass;
	gpointer iter;
	MonoClassField *field;
	MonoType *ftype, *prev_ftype = NULL;
	int nfields = 0;

	klass = mono_class_from_mono_type_internal (t);
	iter = NULL;
	while ((field = mono_class_get_fields_internal (klass, &iter))) {
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
			continue;
		ftype = mono_field_get_type_internal (field);
		ftype = mini_get_underlying_type (ftype);

		if (MONO_TYPE_ISSTRUCT (ftype)) {
			int nested_nfields, nested_esize;
			int nested_field_offsets [16];

			MonoType *fixed_etype;
			int fixed_len;
			if (get_fixed_buffer_attr (field, &fixed_etype, &fixed_len)) {
				if (fixed_etype->type != MONO_TYPE_R4 && fixed_etype->type != MONO_TYPE_R8)
					return FALSE;
				if (fixed_len > 16)
					return FALSE;
				nested_nfields = fixed_len;
				nested_esize = fixed_etype->type == MONO_TYPE_R4 ? 4 : 8;
				for (int i = 0; i < nested_nfields; ++i)
					nested_field_offsets [i] = i * nested_esize;
			} else {
				if (!is_hfa (ftype, &nested_nfields, &nested_esize, nested_field_offsets))
					return FALSE;
			}

			if (nested_esize == 4)
				ftype = m_class_get_byval_arg (mono_defaults.single_class);
			else
				ftype = m_class_get_byval_arg (mono_defaults.double_class);
			if (prev_ftype && prev_ftype->type != ftype->type)
				return FALSE;
			prev_ftype = ftype;
			for (int i = 0; i < nested_nfields; ++i) {
				if (nfields + i < 4)
					field_offsets [nfields + i] = field->offset - MONO_ABI_SIZEOF (MonoObject) + nested_field_offsets [i];
			}
			nfields += nested_nfields;
		} else {
			if (!(!ftype->byref && (ftype->type == MONO_TYPE_R4 || ftype->type == MONO_TYPE_R8)))
				return FALSE;
			if (prev_ftype && prev_ftype->type != ftype->type)
				return FALSE;
			prev_ftype = ftype;
			if (nfields < 4)
				field_offsets [nfields] = field->offset - MONO_ABI_SIZEOF (MonoObject);
			nfields ++;
		}
	}
	if (nfields == 0 || nfields > 4)
		return FALSE;
	*out_nfields = nfields;
	*out_esize = prev_ftype->type == MONO_TYPE_R4 ? 4 : 8;
	return TRUE;
}

static void
add_valuetype (CallInfo *cinfo, ArgInfo *ainfo, MonoType *t)
{
	int i, size, align_size, nregs, nfields, esize;
	int field_offsets [16];
	guint32 align;

	size = mini_type_stack_size_full (t, &align, cinfo->pinvoke);
	align_size = ALIGN_TO (size, 8);

	nregs = align_size / 8;
	if (is_hfa (t, &nfields, &esize, field_offsets)) {
		/*
		 * The struct might include nested float structs aligned at 8,
		 * so need to keep track of the offsets of the individual fields.
		 */
		if (cinfo->fr + nfields <= FP_PARAM_REGS) {
			ainfo->storage = ArgHFA;
			ainfo->reg = cinfo->fr;
			ainfo->nregs = nfields;
			ainfo->size = size;
			ainfo->esize = esize;
			for (i = 0; i < nfields; ++i)
				ainfo->foffsets [i] = field_offsets [i];
			cinfo->fr += ainfo->nregs;
		} else {
			ainfo->nfregs_to_skip = FP_PARAM_REGS > cinfo->fr ? FP_PARAM_REGS - cinfo->fr : 0;
			cinfo->fr = FP_PARAM_REGS;
			size = ALIGN_TO (size, 8);
			ainfo->storage = ArgVtypeOnStack;
			cinfo->stack_usage = ALIGN_TO (cinfo->stack_usage, align);
			ainfo->offset = cinfo->stack_usage;
			ainfo->size = size;
			ainfo->hfa = TRUE;
			ainfo->nregs = nfields;
			ainfo->esize = esize;
			cinfo->stack_usage += size;
		}
		return;
	}

	if (align_size > 16) {
		ainfo->storage = ArgVtypeByRef;
		ainfo->size = size;
		return;
	}

	if (cinfo->gr + nregs > PARAM_REGS) {
		size = ALIGN_TO (size, 8);
		ainfo->storage = ArgVtypeOnStack;
		cinfo->stack_usage = ALIGN_TO (cinfo->stack_usage, align);
		ainfo->offset = cinfo->stack_usage;
		ainfo->size = size;
		cinfo->stack_usage += size;
		cinfo->gr = PARAM_REGS;
	} else {
		ainfo->storage = ArgVtypeInIRegs;
		ainfo->reg = cinfo->gr;
		ainfo->nregs = nregs;
		ainfo->size = size;
		cinfo->gr += nregs;
	}
}

static void
add_param (CallInfo *cinfo, ArgInfo *ainfo, MonoType *t)
{
	MonoType *ptype;

	ptype = mini_get_underlying_type (t);
	switch (ptype->type) {
	case MONO_TYPE_I1:
		add_general (cinfo, ainfo, 1, TRUE);
		break;
	case MONO_TYPE_U1:
		add_general (cinfo, ainfo, 1, FALSE);
		break;
	case MONO_TYPE_I2:
		add_general (cinfo, ainfo, 2, TRUE);
		break;
	case MONO_TYPE_U2:
		add_general (cinfo, ainfo, 2, FALSE);
		break;
	case MONO_TYPE_I4:
		add_general (cinfo, ainfo, 4, TRUE);
		break;
	case MONO_TYPE_U4:
		add_general (cinfo, ainfo, 4, FALSE);
		break;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
		add_general (cinfo, ainfo, 8, FALSE);
		break;
	case MONO_TYPE_R8:
		add_fp (cinfo, ainfo, FALSE);
		break;
	case MONO_TYPE_R4:
		add_fp (cinfo, ainfo, TRUE);
		break;
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_TYPEDBYREF:
		add_valuetype (cinfo, ainfo, ptype);
		break;
	case MONO_TYPE_VOID:
		ainfo->storage = ArgNone;
		break;
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (ptype)) {
			add_general (cinfo, ainfo, 8, FALSE);
		} else if (mini_is_gsharedvt_variable_type (ptype)) {
			/*
			 * Treat gsharedvt arguments as large vtypes
			 */
			ainfo->storage = ArgVtypeByRef;
			ainfo->gsharedvt = TRUE;
		} else {
			add_valuetype (cinfo, ainfo, ptype);
		}
		break;
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		g_assert (mini_is_gsharedvt_type (ptype));
		ainfo->storage = ArgVtypeByRef;
		ainfo->gsharedvt = TRUE;
		break;
	default:
		g_assert_not_reached ();
		break;
	}
}

/*
 * get_call_info:
 *
 *  Obtain information about a call according to the calling convention.
 */
static CallInfo*
get_call_info (MonoMemPool *mp, MonoMethodSignature *sig)
{
	CallInfo *cinfo;
	ArgInfo *ainfo;
	int n, pstart, pindex;

	n = sig->hasthis + sig->param_count;

	if (mp)
		cinfo = mono_mempool_alloc0 (mp, sizeof (CallInfo) + (sizeof (ArgInfo) * n));
	else
		cinfo = g_malloc0 (sizeof (CallInfo) + (sizeof (ArgInfo) * n));

	cinfo->nargs = n;
	cinfo->pinvoke = sig->pinvoke;

	/* Return value */
	add_param (cinfo, &cinfo->ret, sig->ret);
	if (cinfo->ret.storage == ArgVtypeByRef)
		cinfo->ret.reg = ARMREG_R8;
	/* Reset state */
	cinfo->gr = 0;
	cinfo->fr = 0;
	cinfo->stack_usage = 0;

	/* Parameters */
	if (sig->hasthis)
		add_general (cinfo, cinfo->args + 0, 8, FALSE);
	pstart = 0;
	for (pindex = pstart; pindex < sig->param_count; ++pindex) {
		ainfo = cinfo->args + sig->hasthis + pindex;

		if ((sig->call_convention == MONO_CALL_VARARG) && (pindex == sig->sentinelpos)) {
			/* Prevent implicit arguments and sig_cookie from
			   being passed in registers */
			cinfo->gr = PARAM_REGS;
			cinfo->fr = FP_PARAM_REGS;
			/* Emit the signature cookie just before the implicit arguments */
			add_param (cinfo, &cinfo->sig_cookie, mono_get_int_type ());
		}

		add_param (cinfo, ainfo, sig->params [pindex]);
		if (ainfo->storage == ArgVtypeByRef) {
			/* Pass the argument address in the next register */
			if (cinfo->gr >= PARAM_REGS) {
				ainfo->storage = ArgVtypeByRefOnStack;
				cinfo->stack_usage = ALIGN_TO (cinfo->stack_usage, 8);
				ainfo->offset = cinfo->stack_usage;
				cinfo->stack_usage += 8;
			} else {
				ainfo->reg = cinfo->gr;
				cinfo->gr ++;
			}
		}
	}

	/* Handle the case where there are no implicit arguments */
	if ((sig->call_convention == MONO_CALL_VARARG) && (pindex == sig->sentinelpos)) {
		/* Prevent implicit arguments and sig_cookie from
		   being passed in registers */
		cinfo->gr = PARAM_REGS;
		cinfo->fr = FP_PARAM_REGS;
		/* Emit the signature cookie just before the implicit arguments */
		add_param (cinfo, &cinfo->sig_cookie, mono_get_int_type ());
	}

	cinfo->stack_usage = ALIGN_TO (cinfo->stack_usage, MONO_ARCH_FRAME_ALIGNMENT);

	return cinfo;
}

typedef struct {
	MonoMethodSignature *sig;
	CallInfo *cinfo;
	MonoType *rtype;
	MonoType **param_types;
	int n_fpargs, n_fpret, nullable_area;
} ArchDynCallInfo;

static gboolean
dyn_call_supported (CallInfo *cinfo, MonoMethodSignature *sig)
{
	int i;

	// FIXME: Add more cases
	switch (cinfo->ret.storage) {
	case ArgNone:
	case ArgInIReg:
	case ArgInFReg:
	case ArgInFRegR4:
	case ArgVtypeByRef:
		break;
	case ArgVtypeInIRegs:
		if (cinfo->ret.nregs > 2)
			return FALSE;
		break;
	case ArgHFA:
		break;
	default:
		return FALSE;
	}

	for (i = 0; i < cinfo->nargs; ++i) {
		ArgInfo *ainfo = &cinfo->args [i];

		switch (ainfo->storage) {
		case ArgInIReg:
		case ArgVtypeInIRegs:
		case ArgInFReg:
		case ArgInFRegR4:
		case ArgHFA:
		case ArgVtypeByRef:
		case ArgVtypeByRefOnStack:
		case ArgOnStack:
		case ArgVtypeOnStack:
			break;
		default:
			return FALSE;
		}
	}

	return TRUE;
}

MonoDynCallInfo*
mono_arch_dyn_call_prepare (MonoMethodSignature *sig)
{
	ArchDynCallInfo *info;
	CallInfo *cinfo;
	int i, aindex;

	cinfo = get_call_info (NULL, sig);

	if (!dyn_call_supported (cinfo, sig)) {
		g_free (cinfo);
		return NULL;
	}

	info = g_new0 (ArchDynCallInfo, 1);
	// FIXME: Preprocess the info to speed up start_dyn_call ()
	info->sig = sig;
	info->cinfo = cinfo;
	info->rtype = mini_get_underlying_type (sig->ret);
	info->param_types = g_new0 (MonoType*, sig->param_count);
	for (i = 0; i < sig->param_count; ++i)
		info->param_types [i] = mini_get_underlying_type (sig->params [i]);

	switch (cinfo->ret.storage) {
	case ArgInFReg:
	case ArgInFRegR4:
		info->n_fpret = 1;
		break;
	case ArgHFA:
		info->n_fpret = cinfo->ret.nregs;
		break;
	default:
		break;
	}

	for (aindex = 0; aindex < sig->param_count; aindex++) {
		MonoType *t = info->param_types [aindex];

		if (t->byref)
			continue;

		switch (t->type) {
		case MONO_TYPE_GENERICINST:
			if (t->type == MONO_TYPE_GENERICINST && mono_class_is_nullable (mono_class_from_mono_type_internal (t))) {
				MonoClass *klass = mono_class_from_mono_type_internal (t);
				int size;

				/* Nullables need a temporary buffer, its stored at the end of DynCallArgs.regs after the stack args */
				size = mono_class_value_size (klass, NULL);
				info->nullable_area += size;
			}
			break;
		default:
			break;
		}
	}
	
	return (MonoDynCallInfo*)info;
}

void
mono_arch_dyn_call_free (MonoDynCallInfo *info)
{
	ArchDynCallInfo *ainfo = (ArchDynCallInfo*)info;

	g_free (ainfo->cinfo);
	g_free (ainfo->param_types);
	g_free (ainfo);
}

int
mono_arch_dyn_call_get_buf_size (MonoDynCallInfo *info)
{
	ArchDynCallInfo *ainfo = (ArchDynCallInfo*)info;

	g_assert (ainfo->cinfo->stack_usage % MONO_ARCH_FRAME_ALIGNMENT == 0);
	return sizeof (DynCallArgs) + ainfo->cinfo->stack_usage + ainfo->nullable_area;
}

static double
bitcast_r4_to_r8 (float f)
{
	float *p = &f;

	return *(double*)p;
}

static float
bitcast_r8_to_r4 (double f)
{
	double *p = &f;

	return *(float*)p;
}

void
mono_arch_start_dyn_call (MonoDynCallInfo *info, gpointer **args, guint8 *ret, guint8 *buf)
{
	ArchDynCallInfo *dinfo = (ArchDynCallInfo*)info;
	DynCallArgs *p = (DynCallArgs*)buf;
	int aindex, arg_index, greg, i, pindex;
	MonoMethodSignature *sig = dinfo->sig;
	CallInfo *cinfo = dinfo->cinfo;
	int buffer_offset = 0;
	guint8 *nullable_buffer;

	p->res = 0;
	p->ret = ret;
	p->n_fpargs = dinfo->n_fpargs;
	p->n_fpret = dinfo->n_fpret;
	p->n_stackargs = cinfo->stack_usage / sizeof (host_mgreg_t);

	arg_index = 0;
	greg = 0;
	pindex = 0;

	/* Stored after the stack arguments */
	nullable_buffer = (guint8*)&(p->regs [PARAM_REGS + 1 + (cinfo->stack_usage / sizeof (host_mgreg_t))]);

	if (sig->hasthis)
		p->regs [greg ++] = (host_mgreg_t)*(args [arg_index ++]);

	if (cinfo->ret.storage == ArgVtypeByRef)
		p->regs [ARMREG_R8] = (host_mgreg_t)ret;

	for (aindex = pindex; aindex < sig->param_count; aindex++) {
		MonoType *t = dinfo->param_types [aindex];
		gpointer *arg = args [arg_index ++];
		ArgInfo *ainfo = &cinfo->args [aindex + sig->hasthis];
		int slot = -1;

		if (ainfo->storage == ArgOnStack || ainfo->storage == ArgVtypeOnStack || ainfo->storage == ArgVtypeByRefOnStack) {
			slot = PARAM_REGS + 1 + (ainfo->offset / sizeof (host_mgreg_t));
		} else {
			slot = ainfo->reg;
		}

		if (t->byref) {
			p->regs [slot] = (host_mgreg_t)*arg;
			continue;
		}

		switch (t->type) {
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_PTR:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
			p->regs [slot] = (host_mgreg_t)*arg;
			break;
		case MONO_TYPE_U1:
			p->regs [slot] = *(guint8*)arg;
			break;
		case MONO_TYPE_I1:
			p->regs [slot] = *(gint8*)arg;
			break;
		case MONO_TYPE_I2:
			p->regs [slot] = *(gint16*)arg;
			break;
		case MONO_TYPE_U2:
			p->regs [slot] = *(guint16*)arg;
			break;
		case MONO_TYPE_I4:
			p->regs [slot] = *(gint32*)arg;
			break;
		case MONO_TYPE_U4:
			p->regs [slot] = *(guint32*)arg;
			break;
		case MONO_TYPE_R4:
			p->fpregs [ainfo->reg] = bitcast_r4_to_r8 (*(float*)arg);
			p->n_fpargs ++;
			break;
		case MONO_TYPE_R8:
			p->fpregs [ainfo->reg] = *(double*)arg;
			p->n_fpargs ++;
			break;
		case MONO_TYPE_GENERICINST:
			if (MONO_TYPE_IS_REFERENCE (t)) {
				p->regs [slot] = (host_mgreg_t)*arg;
				break;
			} else {
				if (t->type == MONO_TYPE_GENERICINST && mono_class_is_nullable (mono_class_from_mono_type_internal (t))) {
					MonoClass *klass = mono_class_from_mono_type_internal (t);
					guint8 *nullable_buf;
					int size;

					/*
					 * Use p->buffer as a temporary buffer since the data needs to be available after this call
					 * if the nullable param is passed by ref.
					 */
					size = mono_class_value_size (klass, NULL);
					nullable_buf = nullable_buffer + buffer_offset;
					buffer_offset += size;
					g_assert (buffer_offset <= dinfo->nullable_area);

					/* The argument pointed to by arg is either a boxed vtype or null */
					mono_nullable_init (nullable_buf, (MonoObject*)arg, klass);

					arg = (gpointer*)nullable_buf;
					/* Fall though */
				} else {
					/* Fall though */
				}
			}
		case MONO_TYPE_VALUETYPE:
			switch (ainfo->storage) {
			case ArgVtypeInIRegs:
				for (i = 0; i < ainfo->nregs; ++i)
					p->regs [slot ++] = ((host_mgreg_t*)arg) [i];
				break;
			case ArgHFA:
				if (ainfo->esize == 4) {
					for (i = 0; i < ainfo->nregs; ++i)
						p->fpregs [ainfo->reg + i] = bitcast_r4_to_r8 (((float*)arg) [ainfo->foffsets [i] / 4]);
				} else {
					for (i = 0; i < ainfo->nregs; ++i)
						p->fpregs [ainfo->reg + i] = ((double*)arg) [ainfo->foffsets [i] / 8];
				}
				p->n_fpargs += ainfo->nregs;
				break;
			case ArgVtypeByRef:
			case ArgVtypeByRefOnStack:
				p->regs [slot] = (host_mgreg_t)arg;
				break;
			case ArgVtypeOnStack:
				for (i = 0; i < ainfo->size / 8; ++i)
					p->regs [slot ++] = ((host_mgreg_t*)arg) [i];
				break;
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		default:
			g_assert_not_reached ();
		}
	}
}

void
mono_arch_finish_dyn_call (MonoDynCallInfo *info, guint8 *buf)
{
	ArchDynCallInfo *ainfo = (ArchDynCallInfo*)info;
	CallInfo *cinfo = ainfo->cinfo;
	DynCallArgs *args = (DynCallArgs*)buf;
	MonoType *ptype = ainfo->rtype;
	guint8 *ret = args->ret;
	host_mgreg_t res = args->res;
	host_mgreg_t res2 = args->res2;
	int i;

	if (cinfo->ret.storage == ArgVtypeByRef)
		return;

	switch (ptype->type) {
	case MONO_TYPE_VOID:
		*(gpointer*)ret = NULL;
		break;
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
		*(gpointer*)ret = (gpointer)res;
		break;
	case MONO_TYPE_I1:
		*(gint8*)ret = res;
		break;
	case MONO_TYPE_U1:
		*(guint8*)ret = res;
		break;
	case MONO_TYPE_I2:
		*(gint16*)ret = res;
		break;
	case MONO_TYPE_U2:
		*(guint16*)ret = res;
		break;
	case MONO_TYPE_I4:
		*(gint32*)ret = res;
		break;
	case MONO_TYPE_U4:
		*(guint32*)ret = res;
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		*(guint64*)ret = res;
		break;
	case MONO_TYPE_R4:
		*(float*)ret = bitcast_r8_to_r4 (args->fpregs [0]);
		break;
	case MONO_TYPE_R8:
		*(double*)ret = args->fpregs [0];
		break;
	case MONO_TYPE_GENERICINST:
		if (MONO_TYPE_IS_REFERENCE (ptype)) {
			*(gpointer*)ret = (gpointer)res;
			break;
		} else {
			/* Fall though */
		}
	case MONO_TYPE_VALUETYPE:
		switch (ainfo->cinfo->ret.storage) {
		case ArgVtypeInIRegs:
			*(host_mgreg_t*)ret = res;
			if (ainfo->cinfo->ret.nregs > 1)
				((host_mgreg_t*)ret) [1] = res2;
			break;
		case ArgHFA:
			/* Use the same area for returning fp values */
			if (cinfo->ret.esize == 4) {
				for (i = 0; i < cinfo->ret.nregs; ++i)
					((float*)ret) [cinfo->ret.foffsets [i] / 4] = bitcast_r8_to_r4 (args->fpregs [i]);
			} else {
				for (i = 0; i < cinfo->ret.nregs; ++i)
					((double*)ret) [cinfo->ret.foffsets [i] / 8] = args->fpregs [i];
			}
			break;
		default:
			g_assert_not_reached ();
			break;
		}
		break;
	default:
		g_assert_not_reached ();
	}
}

void
mono_arch_flush_icache (guint8 *code, gint size)
{
	/* Don't rely on GCC's __clear_cache implementation, as it caches
	 * icache/dcache cache line sizes, that can vary between cores on
	 * big.LITTLE architectures. */
	guint64 end = (guint64) (code + size);
	guint64 addr;
	/* always go with cacheline size of 4 bytes as this code isn't perf critical
	 * anyway. Reading the cache line size from a machine register can be racy
	 * on a big.LITTLE architecture if the cores don't have the same cache line
	 * sizes. */
	const size_t icache_line_size = 4;
	const size_t dcache_line_size = 4;

	addr = (guint64) code & ~(guint64) (dcache_line_size - 1);
	for (; addr < end; addr += dcache_line_size)
		asm volatile("dc civac, %0" : : "r" (addr) : "memory");
	asm volatile("dsb ish" : : : "memory");

	addr = (guint64) code & ~(guint64) (icache_line_size - 1);
	for (; addr < end; addr += icache_line_size)
		asm volatile("ic ivau, %0" : : "r" (addr) : "memory");

	asm volatile ("dsb ish" : : : "memory");
	asm volatile ("isb" : : : "memory");
}

__attribute__ ((__warn_unused_result__)) guint8*
mono_arm_emit_load_regarray (guint8 *code, guint64 regs, int basereg, int offset)
{
	return emit_load_regarray (code, regs, basereg, offset);
}

__attribute__ ((__warn_unused_result__)) guint8*
mono_arm_emit_store_regarray (guint8 *code, guint64 regs, int basereg, int offset)
{
	return emit_store_regarray (code, regs, basereg, offset);
}

__attribute__ ((__warn_unused_result__)) guint8*
mono_arm_emit_store_regset (guint8 *code, guint64 regs, int basereg, int offset)
{
	return emit_store_regset (code, regs, basereg, offset);
}

gpointer
mono_arch_build_imt_trampoline (MonoVTable *vtable, MonoDomain *domain, MonoIMTCheckItem **imt_entries, int count,
								gpointer fail_tramp)
{
	int i, buf_len, imt_reg;
	guint8 *buf, *code;

#if DEBUG_IMT
	printf ("building IMT trampoline for class %s %s entries %d code size %d code at %p end %p vtable %p\n", m_class_get_name_space (vtable->klass), m_class_get_name (vtable->klass), count, size, start, ((guint8*)start) + size, vtable);
	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		printf ("method %d (%p) %s vtable slot %p is_equals %d chunk size %d\n", i, item->key, item->key->name, &vtable->vtable [item->value.vtable_slot], item->is_equals, item->chunk_size);
	}
#endif

	buf_len = 0;
	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		if (item->is_equals) {
			gboolean fail_case = !item->check_target_idx && fail_tramp;

			if (item->check_target_idx || fail_case) {
				if (!item->compare_done || fail_case) {
					buf_len += 4 * 4 + 4;
				}
				buf_len += 4;
				if (item->has_target_code) {
					buf_len += 5 * 4;
				} else {
					buf_len += 6 * 4;
				}
				if (fail_case) {
					buf_len += 5 * 4;
				}
			} else {
				buf_len += 6 * 4;
			}
		} else {
			buf_len += 6 * 4;
		}
	}

	if (fail_tramp) {
		buf = (guint8*)mono_method_alloc_generic_virtual_trampoline (mono_domain_ambient_memory_manager (domain), buf_len);
	} else {
		MonoMemoryManager *mem_manager = m_class_get_mem_manager (domain, vtable->klass);
		buf = mono_mem_manager_code_reserve (mem_manager, buf_len);
	}
	code = buf;

	MINI_BEGIN_CODEGEN ();

	/*
	 * We are called by JITted code, which passes in the IMT argument in
	 * MONO_ARCH_RGCTX_REG (r27). We need to preserve all caller saved regs
	 * except ip0/ip1.
	 */
	imt_reg = MONO_ARCH_RGCTX_REG;
	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];

		item->code_target = code;

		if (item->is_equals) {
			/*
			 * Check the imt argument against item->key, if equals, jump to either
			 * item->value.target_code or to vtable [item->value.vtable_slot].
			 * If fail_tramp is set, jump to it if not-equals.
			 */
			gboolean fail_case = !item->check_target_idx && fail_tramp;

			if (item->check_target_idx || fail_case) {
				/* Compare imt_reg with item->key */
				if (!item->compare_done || fail_case) {
					// FIXME: Optimize this
					code = emit_imm64 (code, ARMREG_IP0, (guint64)item->key);
					arm_cmpx (code, imt_reg, ARMREG_IP0);
				}
				item->jmp_code = code;
				arm_bcc (code, ARMCOND_NE, 0);
				/* Jump to target if equals */
				if (item->has_target_code) {
					code = emit_imm64 (code, ARMREG_IP0, (guint64)item->value.target_code);
					code = mono_arm_emit_brx (code, ARMREG_IP0);
				} else {
					guint64 imm = (guint64)&(vtable->vtable [item->value.vtable_slot]);

					code = emit_imm64 (code, ARMREG_IP0, imm);
					arm_ldrx (code, ARMREG_IP0, ARMREG_IP0, 0);
					code = mono_arm_emit_brx (code, ARMREG_IP0);
				}

				if (fail_case) {
					arm_patch_rel (item->jmp_code, code, MONO_R_ARM64_BCC);
					item->jmp_code = NULL;
					code = emit_imm64 (code, ARMREG_IP0, (guint64)fail_tramp);
					code = mono_arm_emit_brx (code, ARMREG_IP0);
				}
			} else {
				guint64 imm = (guint64)&(vtable->vtable [item->value.vtable_slot]);

				code = emit_imm64 (code, ARMREG_IP0, imm);
				arm_ldrx (code, ARMREG_IP0, ARMREG_IP0, 0);
				code = mono_arm_emit_brx (code, ARMREG_IP0);
			}
		} else {
			code = emit_imm64 (code, ARMREG_IP0, (guint64)item->key);
			arm_cmpx (code, imt_reg, ARMREG_IP0);
			item->jmp_code = code;
			arm_bcc (code, ARMCOND_HS, 0);
		}
	}
	/* Patch the branches */
	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		if (item->jmp_code && item->check_target_idx)
			arm_patch_rel (item->jmp_code, imt_entries [item->check_target_idx]->code_target, MONO_R_ARM64_BCC);
	}

	g_assert ((code - buf) <= buf_len);

	MINI_END_CODEGEN (buf, code - buf, MONO_PROFILER_CODE_BUFFER_IMT_TRAMPOLINE, NULL);

	return MINI_ADDR_TO_FTNPTR (buf);
}

GSList *
mono_arch_get_trampolines (gboolean aot)
{
	return mono_arm_get_exception_trampolines (aot);
}

#ifdef MONO_ARCH_SOFT_DEBUG_SUPPORTED

void
mono_arch_set_breakpoint (MonoJitInfo *ji, guint8 *ip)
{
	guint8 *code = MINI_FTNPTR_TO_ADDR (ip);
	guint32 native_offset = ip - (guint8*)ji->code_start;

	if (ji->from_aot) {
		SeqPointInfo *info = mono_arch_get_seq_point_info (mono_domain_get (), (guint8*)ji->code_start);

		g_assert (native_offset % 4 == 0);
		g_assert (info->bp_addrs [native_offset / 4] == 0);
		info->bp_addrs [native_offset / 4] = (guint8*)mini_get_breakpoint_trampoline ();
	} else {
		/* ip points to an ldrx */
		code += 4;
		mono_codeman_enable_write ();
		code = mono_arm_emit_blrx (code, ARMREG_IP0);
		mono_codeman_disable_write ();
		mono_arch_flush_icache (ip, code - ip);
	}
}

void
mono_arch_clear_breakpoint (MonoJitInfo *ji, guint8 *ip)
{
	guint8 *code = MINI_FTNPTR_TO_ADDR (ip);

	if (ji->from_aot) {
		guint32 native_offset = ip - (guint8*)ji->code_start;
		SeqPointInfo *info = mono_arch_get_seq_point_info (mono_domain_get (), (guint8*)ji->code_start);

		g_assert (native_offset % 4 == 0);
		info->bp_addrs [native_offset / 4] = NULL;
	} else {
		/* ip points to an ldrx */
		code += 4;
		mono_codeman_enable_write ();
		arm_nop (code);
		mono_codeman_disable_write ();
		mono_arch_flush_icache (ip, code - ip);
	}
}

void
mono_arch_start_single_stepping (void)
{
	mono_arm64_ss_trampoline = mini_get_single_step_trampoline ();
}

void
mono_arch_stop_single_stepping (void)
{
	mono_arm64_ss_trampoline = NULL;
}

gpointer*
mono_arch_get_single_step_tramp_addr (void)
{
	return &mono_arm64_ss_trampoline;
}

gboolean
mono_arch_is_single_step_event (void *info, void *sigctx)
{
	/* We use soft breakpoints on arm64 */
	return FALSE;
}

gboolean
mono_arch_is_breakpoint_event (void *info, void *sigctx)
{
	/* We use soft breakpoints on arm64 */
	return FALSE;
}

void
mono_arch_skip_breakpoint (MonoContext *ctx, MonoJitInfo *ji)
{
	g_assert_not_reached ();
}

void
mono_arch_skip_single_step (MonoContext *ctx)
{
	g_assert_not_reached ();
}

SeqPointInfo*
mono_arch_get_seq_point_info (MonoDomain *domain, guint8 *code)
{
	SeqPointInfo *info;
	MonoJitInfo *ji;

	// FIXME: Add a free function

	mono_domain_lock (domain);
	info = (SeqPointInfo*)g_hash_table_lookup (domain_jit_info (domain)->arch_seq_points,
								code);
	mono_domain_unlock (domain);

	if (!info) {
		ji = mono_jit_info_table_find (domain, code);
		g_assert (ji);

		info = g_malloc0 (sizeof (SeqPointInfo) + (ji->code_size / 4) * sizeof(guint8*));

		info->ss_tramp_addr = &mono_arm64_ss_trampoline;

		mono_domain_lock (domain);
		g_hash_table_insert (domain_jit_info (domain)->arch_seq_points,
							 code, info);
		mono_domain_unlock (domain);
	}

	return info;
}

#endif /* MONO_ARCH_SOFT_DEBUG_SUPPORTED */

CallInfo*
mono_arch_get_call_info (MonoMemPool *mp, MonoMethodSignature *sig)
{
	return get_call_info (mp, sig);
}

gpointer
mono_arch_load_function (MonoJitICallId jit_icall_id)
{
	gpointer target = NULL;
	switch (jit_icall_id) {
#undef MONO_AOT_ICALL
#define MONO_AOT_ICALL(x) case MONO_JIT_ICALL_ ## x: target = (gpointer)x; break;
	MONO_AOT_ICALL (mono_arm_resume_unwind)
	MONO_AOT_ICALL (mono_arm_start_gsharedvt_call)
	MONO_AOT_ICALL (mono_arm_throw_exception)
	}
	return target;
}

guint8*
mono_arm_emit_blrx (guint8 *code, int reg)
{
	return emit_blrx (code, reg);
}

guint8*
mono_arm_emit_brx (guint8 *code, int reg)
{
	return emit_brx (code, reg);
}
