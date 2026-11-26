/**
 * \file
 * \brief How AAPCS64 carries a value type.
 *
 * The classification is done from IR types and the DataLayout, the way clang
 * lowers a C struct for aarch64-linux-gnu:
 *
 *   - a homogeneous aggregate of one to four floats, doubles or short vectors
 *     of one type travels as an array of that type, which LLVM places in
 *     consecutive vector registers or, when too few are left, all on the
 *     stack;
 *   - any other value type of up to 16 bytes travels as one or two 64-bit
 *     words, which LLVM also places all in registers or all on the stack;
 *   - a larger one is copied by the caller, and the copy's address travels in
 *     its place;
 *   - a return follows the first two rules, and a larger one comes back
 *     through a pointer in x8.
 *
 * LLVM counts the registers, because its AArch64 calling convention places
 * each of these types as AAPCS64 requires.
 */

#include "arch/c-lowering.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/Alignment.h>

using namespace llvm;

namespace mono::arch {

namespace {

/// Returns \p t as an array of its member type when it is a homogeneous
/// floating-point or short-vector aggregate of one to four members, and null
/// otherwise.
Type *
homogeneous_element (Type *t, const DataLayout &dl)
{
	SmallVector<Leaf, 8> leaves;

	collect_leaves (t, 0, dl, leaves);
	if (leaves.empty () || leaves.size () > 4)
		return nullptr;

	Type *element = leaves[0].type;
	bool is_member = element->isFloatTy () || element->isDoubleTy ()
	                 || (element->isVectorTy ()
	                     && (leaves[0].size == 8 || leaves[0].size == 16));

	if (!is_member)
		return nullptr;

	uint64_t stride = leaves[0].size;

	for (unsigned i = 0; i < leaves.size (); ++i)
		if (leaves[i].type != element || leaves[i].offset != i * stride)
			return nullptr;

	// Trailing padding, which the walk skipped, makes the type larger than its
	// members.
	if (dl.getTypeAllocSize (t) != leaves.size () * stride)
		return nullptr;

	return ArrayType::get (element, leaves.size ());
}

/// Returns the type that \p t travels as, or null when it is too large and
/// travels by address.
Type *
travel_type (Type *t, const DataLayout &dl, LLVMContext &ctx)
{
	if (Type *hfa = homogeneous_element (t, dl))
		return hfa;

	uint64_t size = dl.getTypeAllocSize (t);

	// An empty C struct takes no register and no stack.
	if (size == 0)
		return StructType::get (ctx);
	if (size > 16)
		return nullptr;

	Type *word = Type::getInt64Ty (ctx);

	return size <= 8 ? word : ArrayType::get (word, 2);
}

} // namespace

CallLowering
compute_lowering (FunctionType *type, function_ref<bool (unsigned)> is_nest,
                  const DataLayout &dl, LLVMContext &ctx)
{
	CallLowering low;
	Type *ret = type->getReturnType ();

	if (is_aggregate (ret)) {
		low.ret_travel = travel_type (ret, dl, ctx);
		low.ret_by_address = low.ret_travel == nullptr;
	}

	for (unsigned i = 0; i < type->getNumParams (); ++i) {
		Type *t = type->getParamType (i);
		ParamLowering p;

		if (!is_nest (i) && is_aggregate (t)) {
			p.travel = travel_type (t, dl, ctx);
			p.kind = p.travel != nullptr ? ParamLowering::Coerced
			                             : ParamLowering::Indirect;
		}

		low.params.push_back (p);
	}

	return low;
}

} // namespace mono::arch
