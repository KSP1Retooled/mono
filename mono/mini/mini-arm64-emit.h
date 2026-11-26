/**
 * \file
 * \brief Instruction-sequence helpers for emitting arm64 code.
 */

#ifndef __MONO_MINI_ARM64_EMIT_H__
#define __MONO_MINI_ARM64_EMIT_H__

#include <mono/arch/arm64/arm64-codegen.h>

/* The maximum length is 2 instructions */
static inline guint8*
emit_imm (guint8 *code, int dreg, int imm)
{
	// FIXME: Optimize this
	if (imm < 0) {
		gint64 limm = imm;
		arm_movnx (code, dreg, (~limm) & 0xffff, 0);
		arm_movkx (code, dreg, (limm >> 16) & 0xffff, 16);
	} else {
		arm_movzx (code, dreg, imm & 0xffff, 0);
		if (imm >> 16)
			arm_movkx (code, dreg, (imm >> 16) & 0xffff, 16);
	}

	return code;
}

/* The maximum length is 4 instructions */
static inline guint8*
emit_imm64 (guint8 *code, int dreg, guint64 imm)
{
	// FIXME: Optimize this
	arm_movzx (code, dreg, imm & 0xffff, 0);
	if ((imm >> 16) & 0xffff)
		arm_movkx (code, dreg, (imm >> 16) & 0xffff, 16);
	if ((imm >> 32) & 0xffff)
		arm_movkx (code, dreg, (imm >> 32) & 0xffff, 32);
	if ((imm >> 48) & 0xffff)
		arm_movkx (code, dreg, (imm >> 48) & 0xffff, 48);

	return code;
}

static inline guint8*
emit_imm64_template (guint8 *code, int dreg)
{
	arm_movzx (code, dreg, 0, 0);
	arm_movkx (code, dreg, 0, 16);
	arm_movkx (code, dreg, 0, 32);
	arm_movkx (code, dreg, 0, 48);

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_addw_imm (guint8 *code, int dreg, int sreg, int imm)
{
	if (!arm_is_arith_imm (imm)) {
		code = emit_imm (code, ARMREG_LR, imm);
		arm_addw (code, dreg, sreg, ARMREG_LR);
	} else {
		arm_addw_imm (code, dreg, sreg, imm);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_addx_imm (guint8 *code, int dreg, int sreg, int imm)
{
	if (!arm_is_arith_imm (imm)) {
		code = emit_imm (code, ARMREG_LR, imm);
		arm_addx (code, dreg, sreg, ARMREG_LR);
	} else {
		arm_addx_imm (code, dreg, sreg, imm);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_subw_imm (guint8 *code, int dreg, int sreg, int imm)
{
	if (!arm_is_arith_imm (imm)) {
		code = emit_imm (code, ARMREG_LR, imm);
		arm_subw (code, dreg, sreg, ARMREG_LR);
	} else {
		arm_subw_imm (code, dreg, sreg, imm);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_subx_imm (guint8 *code, int dreg, int sreg, int imm)
{
	if (!arm_is_arith_imm (imm)) {
		code = emit_imm (code, ARMREG_LR, imm);
		arm_subx (code, dreg, sreg, ARMREG_LR);
	} else {
		arm_subx_imm (code, dreg, sreg, imm);
	}
	return code;
}

/* Emit sp+=imm. Clobbers ip0/ip1 */
static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_addx_sp_imm (guint8 *code, int imm)
{
	code = emit_imm (code, ARMREG_IP0, imm);
	arm_movspx (code, ARMREG_IP1, ARMREG_SP);
	arm_addx (code, ARMREG_IP1, ARMREG_IP1, ARMREG_IP0);
	arm_movspx (code, ARMREG_SP, ARMREG_IP1);
	return code;
}

/* Emit sp-=imm. Clobbers ip0/ip1 */
static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_subx_sp_imm (guint8 *code, int imm)
{
	code = emit_imm (code, ARMREG_IP0, imm);
	arm_movspx (code, ARMREG_IP1, ARMREG_SP);
	arm_subx (code, ARMREG_IP1, ARMREG_IP1, ARMREG_IP0);
	arm_movspx (code, ARMREG_SP, ARMREG_IP1);
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_andw_imm (guint8 *code, int dreg, int sreg, int imm)
{
	// FIXME:
	code = emit_imm (code, ARMREG_LR, imm);
	arm_andw (code, dreg, sreg, ARMREG_LR);

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_andx_imm (guint8 *code, int dreg, int sreg, int imm)
{
	// FIXME:
	code = emit_imm (code, ARMREG_LR, imm);
	arm_andx (code, dreg, sreg, ARMREG_LR);

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_orrw_imm (guint8 *code, int dreg, int sreg, int imm)
{
	// FIXME:
	code = emit_imm (code, ARMREG_LR, imm);
	arm_orrw (code, dreg, sreg, ARMREG_LR);

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_orrx_imm (guint8 *code, int dreg, int sreg, int imm)
{
	// FIXME:
	code = emit_imm (code, ARMREG_LR, imm);
	arm_orrx (code, dreg, sreg, ARMREG_LR);

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_eorw_imm (guint8 *code, int dreg, int sreg, int imm)
{
	// FIXME:
	code = emit_imm (code, ARMREG_LR, imm);
	arm_eorw (code, dreg, sreg, ARMREG_LR);

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_eorx_imm (guint8 *code, int dreg, int sreg, int imm)
{
	// FIXME:
	code = emit_imm (code, ARMREG_LR, imm);
	arm_eorx (code, dreg, sreg, ARMREG_LR);

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_cmpw_imm (guint8 *code, int sreg, int imm)
{
	if (imm == 0) {
		arm_cmpw (code, sreg, ARMREG_RZR);
	} else {
		// FIXME:
		code = emit_imm (code, ARMREG_LR, imm);
		arm_cmpw (code, sreg, ARMREG_LR);
	}

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_cmpx_imm (guint8 *code, int sreg, int imm)
{
	if (imm == 0) {
		arm_cmpx (code, sreg, ARMREG_RZR);
	} else {
		// FIXME:
		code = emit_imm (code, ARMREG_LR, imm);
		arm_cmpx (code, sreg, ARMREG_LR);
	}

	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_strb (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_strb_imm (imm)) {
		arm_strb (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_strb_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_strh (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_strh_imm (imm)) {
		arm_strh (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_strh_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_strw (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_strw_imm (imm)) {
		arm_strw (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_strw_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_strfpw (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_strw_imm (imm)) {
		arm_strfpw (code, rt, rn, imm);
	} else {
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_addx (code, ARMREG_IP0, rn, ARMREG_IP0);
		arm_strfpw (code, rt, ARMREG_IP0, 0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_strfpx (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_strx_imm (imm)) {
		arm_strfpx (code, rt, rn, imm);
	} else {
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_addx (code, ARMREG_IP0, rn, ARMREG_IP0);
		arm_strfpx (code, rt, ARMREG_IP0, 0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_strx (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_strx_imm (imm)) {
		arm_strx (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_strx_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrb (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 1)) {
		arm_ldrb (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_ldrb_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrsbx (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 1)) {
		arm_ldrsbx (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_ldrsbx_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrh (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 2)) {
		arm_ldrh (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_ldrh_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrshx (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 2)) {
		arm_ldrshx (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_ldrshx_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrswx (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 4)) {
		arm_ldrswx (code, rt, rn, imm);
	} else {
		g_assert (rt != ARMREG_IP0);
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_ldrswx_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrw (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 4)) {
		arm_ldrw (code, rt, rn, imm);
	} else {
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_ldrw_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrx (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 8)) {
		arm_ldrx (code, rt, rn, imm);
	} else {
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_ldrx_reg (code, rt, rn, ARMREG_IP0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrfpw (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 4)) {
		arm_ldrfpw (code, rt, rn, imm);
	} else {
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_addx (code, ARMREG_IP0, rn, ARMREG_IP0);
		arm_ldrfpw (code, rt, ARMREG_IP0, 0);
	}
	return code;
}

static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_ldrfpx (guint8 *code, int rt, int rn, int imm)
{
	if (arm_is_pimm12_scaled (imm, 8)) {
		arm_ldrfpx (code, rt, rn, imm);
	} else {
		g_assert (rn != ARMREG_IP0);
		code = emit_imm (code, ARMREG_IP0, imm);
		arm_addx (code, ARMREG_IP0, rn, ARMREG_IP0);
		arm_ldrfpx (code, rt, ARMREG_IP0, 0);
	}
	return code;
}

/*
 * emit_store_regarray:
 *
 *   Emit code to store the registers in REGS into the appropriate elements of
 * the register array at BASEREG+OFFSET.
 */
static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_store_regarray (guint8 *code, guint64 regs, int basereg, int offset)
{
	int i;

	for (i = 0; i < 32; ++i) {
		if (regs & (1 << i)) {
			if (i + 1 < 32 && (regs & (1 << (i + 1))) && (i + 1 != ARMREG_SP)) {
				arm_stpx (code, i, i + 1, basereg, offset + (i * 8));
				i++;
			} else if (i == ARMREG_SP) {
				arm_movspx (code, ARMREG_IP1, ARMREG_SP);
				arm_strx (code, ARMREG_IP1, basereg, offset + (i * 8));
			} else {
				arm_strx (code, i, basereg, offset + (i * 8));
			}
		}
	}
	return code;
}

/*
 * emit_load_regarray:
 *
 *   Emit code to load the registers in REGS from the appropriate elements of
 * the register array at BASEREG+OFFSET.
 */
static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_load_regarray (guint8 *code, guint64 regs, int basereg, int offset)
{
	int i;

	for (i = 0; i < 32; ++i) {
		if (regs & (1 << i)) {
			if ((regs & (1 << (i + 1))) && (i + 1 != ARMREG_SP)) {
				if (offset + (i * 8) < 500)
					arm_ldpx (code, i, i + 1, basereg, offset + (i * 8));
				else {
					code = emit_ldrx (code, i, basereg, offset + (i * 8));
					code = emit_ldrx (code, i + 1, basereg, offset + ((i + 1) * 8));
				}
				i++;
			} else if (i == ARMREG_SP) {
				g_assert_not_reached ();
			} else {
				code = emit_ldrx (code, i, basereg, offset + (i * 8));
			}
		}
	}
	return code;
}

/*
 * emit_store_regset:
 *
 *   Emit code to store the registers in REGS into consecutive memory locations starting
 * at BASEREG+OFFSET.
 */
static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_store_regset (guint8 *code, guint64 regs, int basereg, int offset)
{
	int i, pos;

	pos = 0;
	for (i = 0; i < 32; ++i) {
		if (regs & (1 << i)) {
			if ((regs & (1 << (i + 1))) && (i + 1 != ARMREG_SP)) {
				arm_stpx (code, i, i + 1, basereg, offset + (pos * 8));
				i++;
				pos++;
			} else if (i == ARMREG_SP) {
				arm_movspx (code, ARMREG_IP1, ARMREG_SP);
				arm_strx (code, ARMREG_IP1, basereg, offset + (pos * 8));
			} else {
				arm_strx (code, i, basereg, offset + (pos * 8));
			}
			pos++;
		}
	}
	return code;
}

/*
 * emit_load_regset:
 *
 *   Emit code to load the registers in REGS from consecutive memory locations starting
 * at BASEREG+OFFSET.
 */
static inline __attribute__ ((__warn_unused_result__)) guint8*
emit_load_regset (guint8 *code, guint64 regs, int basereg, int offset)
{
	int i, pos;

	pos = 0;
	for (i = 0; i < 32; ++i) {
		if (regs & (1 << i)) {
			if ((regs & (1 << (i + 1))) && (i + 1 != ARMREG_SP)) {
				arm_ldpx (code, i, i + 1, basereg, offset + (pos * 8));
				i++;
				pos++;
			} else if (i == ARMREG_SP) {
				g_assert_not_reached ();
			} else {
				arm_ldrx (code, i, basereg, offset + (pos * 8));
			}
			pos++;
		}
	}
	return code;
}

static inline guint8*
emit_blrx (guint8 *code, int reg)
{
	arm_blrx (code, reg);
	return code;
}

static inline guint8*
emit_brx (guint8 *code, int reg)
{
	arm_brx (code, reg);
	return code;
}

#endif /* __MONO_MINI_ARM64_EMIT_H__ */
