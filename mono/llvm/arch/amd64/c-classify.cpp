/**
 * \file
 * \brief How amd64's C conventions carry a value type.
 *
 * The classification is done from IR types and the DataLayout. Under System V:
 *
 *   - a value type of up to 16 bytes travels as one or two register words,
 *     each in the integer or the float file;
 *   - one bigger than 16 bytes, or one that arrives after the argument
 *     registers have run out, is copied onto the stack - a byval pointer here;
 *   - a return that does not fit the return registers travels through a
 *     pointer the caller passes as the first argument.
 *
 * The Microsoft convention classifies by size alone:
 *
 *   - a value type of 1, 2, 4 or 8 bytes travels as an integer of that width,
 *     whatever its fields are;
 *   - one of any other size travels as a pointer to a copy the caller made,
 *     which is not the same as a copy in the outgoing argument area and so is
 *     a plain pointer here rather than a byval one;
 *   - a return follows the same rule, through a pointer the caller passes as
 *     the first argument when it does not fit.
 *
 * Only System V walks the fields and counts the registers that are left.
 */

#include "config.h"

#include "arch/c-lowering.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/ErrorHandling.h>

#include <cassert>

using namespace llvm;

namespace mono::arch {

namespace {

/// One eightbyte of a value type, classified as the register file it rides in.
enum class Quad { None, Integer, Sse };

/// How a value type crosses a call, before register availability is known.
struct AggShape {
	bool memory = false; ///< always a stack copy, no matter the registers
	unsigned nquads = 0; ///< 0 when nothing travels at all
	Quad cls[2] = { Quad::None, Quad::None };
	unsigned qsize[2] = { 0, 0 };
};

/// Rounds \p n up to the next power of two, the width a register load moves
/// at.
unsigned
pow2_width (uint64_t n)
{
	unsigned width = 1;

	while (width < n)
		width <<= 1;
	return width;
}

#ifdef HOST_WIN32

/// Classifies by size alone, which is the whole of the Microsoft rule. One
/// quad comes back for a value type that fits a register, and `memory` for one
/// that has to be reached through a pointer.
AggShape
classify_aggregate (Type *t, const DataLayout &dl)
{
	AggShape shape;
	uint64_t size = dl.getTypeAllocSize (t);

	/*
	 * C and C++ give an empty struct one byte, so the Microsoft convention
	 * gives it an argument slot like any other one-byte value type. A managed
	 * declaration can still ask for a size of zero, and the native declaration
	 * it is called against takes that slot, so the arguments behind it move
	 * along by one when the slot is dropped.
	 */
	if (size == 0)
		size = 1;

	if (size != 1 && size != 2 && size != 4 && size != 8) {
		shape.memory = true;
		return shape;
	}

	shape.nquads = 1;
	shape.cls[0] = Quad::Integer;
	shape.qsize[0] = (unsigned) size;
	return shape;
}

#else

AggShape
classify_aggregate (Type *t, const DataLayout &dl)
{
	AggShape shape;
	uint64_t size = dl.getTypeAllocSize (t);
	uint64_t aligned = alignTo (size, 8);

	if (size == 0)
		return shape;

	if (size > 16) {
		shape.memory = true;
		return shape;
	}

	SmallVector<Leaf, 8> leaves;
	collect_leaves (t, 0, dl, leaves);

	for (const Leaf &leaf : leaves)
		if (leaf.offset < 8 && leaf.offset + leaf.size > 8) {
			/*
			 * mini refuses to marshal a straddling field (NOT_IMPLEMENTED in
			 * add_valuetype), so no call like this has two working ends to
			 * agree with.
			 */
			report_fatal_error ("mono: a field of a native by-value struct "
			                    "straddles an eightbyte, which the runtime "
			                    "does not marshal");
		}

	if (leaves.empty ())
		return shape;

	shape.nquads = aligned > 8 ? 2 : 1;

	for (unsigned quad = 0; quad < shape.nquads; ++quad) {
		for (const Leaf &leaf : leaves) {
			if (quad == 0 && leaf.offset >= 8)
				continue;
			if (quad == 1 && leaf.offset < 8)
				continue;

			shape.qsize[quad] = (unsigned) (leaf.offset + leaf.size - quad * 8);

			Quad cls = leaf.type->isFloatingPointTy () ? Quad::Sse
			                                           : Quad::Integer;
			if (shape.cls[quad] == Quad::None)
				shape.cls[quad] = cls;
			else if (shape.cls[quad] != cls)
				shape.cls[quad] = Quad::Integer;
		}

		shape.qsize[quad] = pow2_width (shape.qsize[quad]);
		assert (shape.qsize[quad] <= 8);
	}

	return shape;
}

#endif

/// Returns the IR type \p shape's register words travel as. LLVM's own lowering
/// of a first-class struct argument passes each element separately, which is
/// exactly what puts one word per register.
Type *
travel_type (LLVMContext &ctx, const AggShape &shape)
{
	SmallVector<Type *, 2> words;

	for (unsigned quad = 0; quad < shape.nquads; ++quad) {
		switch (shape.cls[quad]) {
		case Quad::None:
			break;
		case Quad::Integer:
			words.push_back (Type::getIntNTy (ctx, shape.qsize[quad] * 8));
			break;
		case Quad::Sse:
			words.push_back (shape.qsize[quad] <= 4
			                         ? Type::getFloatTy (ctx)
			                         : Type::getDoubleTy (ctx));
			break;
		}
	}

	if (words.empty ())
		return StructType::get (ctx);
	if (words.size () == 1)
		return words[0];
	return StructType::get (ctx, { words[0], words[1] });
}

} // namespace

#ifdef HOST_WIN32

/// No register counts here: the shape a value takes is the same wherever it
/// lands, so an argument past the fourth differs only in that LLVM puts it on
/// the stack.
CallLowering
compute_lowering (FunctionType *type, function_ref<bool (unsigned)> is_nest,
                  const DataLayout &dl, LLVMContext &ctx)
{
	CallLowering low;
	Type *ret = type->getReturnType ();

	if (is_aggregate (ret)) {
		AggShape shape = classify_aggregate (ret, dl);

		if (shape.memory)
			low.ret_by_address = true;
		else
			low.ret_travel = travel_type (ctx, shape);
	}

	for (unsigned i = 0; i < type->getNumParams (); ++i) {
		Type *t = type->getParamType (i);
		ParamLowering p;

		/* The nest key rides its own register, outside the convention. */
		if (is_nest (i)) {
			low.params.push_back (p);
			continue;
		}

		if (is_aggregate (t)) {
			AggShape shape = classify_aggregate (t, dl);

			if (shape.memory) {
				p.kind = ParamLowering::Indirect;
			} else {
				p.kind = ParamLowering::Coerced;
				p.travel = travel_type (ctx, shape);
			}
		}

		low.params.push_back (p);
	}

	return low;
}

#else

CallLowering
compute_lowering (FunctionType *type, function_ref<bool (unsigned)> is_nest,
                  const DataLayout &dl, LLVMContext &ctx)
{
	constexpr unsigned param_gregs = 6, param_fregs = 8;

	CallLowering low;
	unsigned gr = 0, fr = 0;

	Type *ret = type->getReturnType ();

	if (is_aggregate (ret)) {
		AggShape shape = classify_aggregate (ret, dl);

		if (!shape.memory) {
			low.ret_travel = travel_type (ctx, shape);
		} else {
			/*
			 * The hidden pointer is spelled out, in front of everything,
			 * rather than left to LLVM. Left alone, LLVM does not demote a
			 * memory-class aggregate at all. Three doubles come back in
			 * XMM0, XMM1 and ST0, which no C callee writes. The first
			 * argument then takes the register the hidden pointer was
			 * owed.
			 */
			low.ret_by_address = true;
			if (gr < param_gregs)
				gr++;
		}
	}

	for (unsigned i = 0; i < type->getNumParams (); ++i) {
		Type *t = type->getParamType (i);
		ParamLowering p;

		/* The nest key rides its own register, outside the convention. */
		if (is_nest (i)) {
			low.params.push_back (p);
			continue;
		}

		if (is_aggregate (t)) {
			AggShape shape = classify_aggregate (t, dl);
			unsigned need_gr = 0, need_fr = 0;

			for (unsigned quad = 0; quad < shape.nquads; ++quad) {
				if (shape.cls[quad] == Quad::Integer)
					need_gr++;
				else if (shape.cls[quad] == Quad::Sse)
					need_fr++;
			}

			/*
			 * A value type that does not fit skips counting its
			 * registers, so they stay free for later arguments, matching
			 * the rewind mini's own classifier performs.
			 */
			if (shape.memory || gr + need_gr > param_gregs
			    || fr + need_fr > param_fregs) {
				p.kind = ParamLowering::Memory;
			} else {
				gr += need_gr;
				fr += need_fr;
				p.kind = ParamLowering::Coerced;
				p.travel = travel_type (ctx, shape);
			}
		} else if (t->isFloatingPointTy () || t->isVectorTy ()) {
			if (fr < param_fregs)
				fr++;
		} else {
			if (gr < param_gregs)
				gr++;
		}

		low.params.push_back (p);
	}

	return low;
}

#endif

} // namespace mono::arch
