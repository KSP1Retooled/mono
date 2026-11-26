/**
 * \file
 * \brief How a call that crosses into C carries each of its values.
 *
 * Every target rewrites such a call the same way. Only the classification
 * differs, and each target defines its own compute_lowering ().
 */

#ifndef MONO_LLVM_ARCH_C_LOWERING_HPP
#define MONO_LLVM_ARCH_C_LOWERING_HPP

#include "layout.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>

#include <cstdint>

namespace mono::arch {

struct Leaf {
	uint64_t offset;
	uint64_t size;
	llvm::Type *type;
};

/// Collects every leaf field of \p t into \p out, each with its offset from the
/// start of the outermost type. The walk skips the padding that the translator
/// emits where the class declares no field. So it finds the same leaves that
/// mini finds in the class metadata.
inline void
collect_leaves (llvm::Type *t, uint64_t offset, const llvm::DataLayout &dl,
                llvm::SmallVectorImpl<Leaf> &out)
{
	if (is_padding_type (t))
		return;

	if (auto *st = llvm::dyn_cast<llvm::StructType> (t)) {
		const llvm::StructLayout *layout = dl.getStructLayout (st);

		for (unsigned i = 0; i < st->getNumElements (); ++i)
			collect_leaves (st->getElementType (i),
			                offset + layout->getElementOffset (i), dl, out);
		return;
	}
	if (auto *at = llvm::dyn_cast<llvm::ArrayType> (t)) {
		llvm::Type *element = at->getElementType ();
		uint64_t stride = dl.getTypeAllocSize (element);

		for (uint64_t i = 0; i < at->getNumElements (); ++i)
			collect_leaves (element, offset + i * stride, dl, out);
		return;
	}

	out.push_back ({ offset, dl.getTypeStoreSize (t), t });
}

/// How one argument crosses the boundary.
struct ParamLowering {
	enum Kind {
		Direct,   ///< the natural type itself: scalars, pointers, vectors
		Coerced,  ///< a value type travelling as register-sized words
		Memory,   ///< a value type copied onto the stack: a byval pointer
		Indirect, ///< a value type reached through a pointer to the caller's copy
	} kind = Direct;
	llvm::Type *travel = nullptr; ///< the lowered parameter type when Coerced
};

/// A whole call's lowering against the C convention.
struct CallLowering {
	llvm::SmallVector<ParamLowering, 8> params; ///< one per natural parameter
	/// A value-type return travelling in registers travels as this.
	llvm::Type *ret_travel = nullptr;
	bool ret_by_address = false;
};

inline bool
is_aggregate (llvm::Type *t)
{
	return t->isStructTy () || t->isArrayTy ();
}

/// Classifies every parameter and the return of \p type against the target's
/// C convention. \p is_nest names the parameters that ride their own register
/// outside the convention, which stay Direct.
CallLowering compute_lowering (llvm::FunctionType *type,
                               llvm::function_ref<bool (unsigned)> is_nest,
                               const llvm::DataLayout &dl, llvm::LLVMContext &ctx);

} // namespace mono::arch

#endif
