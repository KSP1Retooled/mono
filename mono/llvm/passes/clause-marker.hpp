/**
 * \file
 * \brief Recognizing the front end's smuggled clause-index-and-kind globals
 * from IR.
 */

#ifndef MONO_LLVM_PASSES_CLAUSE_MARKER_HPP
#define MONO_LLVM_PASSES_CLAUSE_MARKER_HPP

#include <cstdint>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>

namespace mono {

/// On an inline copy, the selector of the body's clause 0, which the rest follow.
/// A body without it starts at 0.
constexpr llvm::StringRef clause_selector_attribute = "mono-clause-selector";

/// The selector the first clause-bearing copy under a root starts at. Past any
/// clause index, because a method's clause section holds at most 2^24 bytes.
constexpr std::uint32_t first_copy_selector = 1u << 24;

/// Reads the IL clause index, the clause kind, the clause's owner and its
/// selector back out of the type_info_N global a landing pad's TypeId names.
/// Returns false when it does not decode. \p owner and \p selector may be null.
///
/// mono smuggles them through the clause's ttype entry, so they are recovered
/// in-process, with no ttype-table deref and no relocation dependency.
///
/// The v4 form is a 4-word {i32 clause_index, i32 kind, i64 owner, i32 selector}
/// struct. The selector is what the runtime hands the pad to enter the clause,
/// and a shorter form's is its clause index.
///
/// The v3 form leaves the selector off. owner is which method clause_index
/// indexes into:
/// (uint64_t)(uintptr_t) of a MonoMethod*, the same convention jit.hpp's
/// IlInlineRow::callee uses. It is read here rather than off ambient code
/// position, because an inline can move an inlined body's own code under a
/// clause of the root's that was never that body's own - innermost_try ()'s
/// widening in eh-gather.cpp does exactly that, on purpose. So nothing about
/// where the protected code ends up answers which method a clause belongs to.
///
/// The v2 form is a 2-word {i32 clause_index, i32 kind} struct, still built by
/// unwind_marker () (passes/tier-counter.cpp) for a pad that is always the
/// compile's own - it decodes with owner left untouched. A bare i32
/// ConstantInt is the legacy 1-word form, clause_index alone with kind 0, and
/// is still accepted the same way. An all-zero struct lowers to
/// ConstantAggregateZero, which is not a ConstantStruct, so every word is read
/// with Constant::getAggregateElement rather than a ConstantStruct cast.
inline bool
decode_clause_marker (const llvm::GlobalValue *gv, int &clause_index, int &kind,
                      std::uint64_t *owner = nullptr, std::uint32_t *selector = nullptr)
{
	const auto *var = llvm::dyn_cast_or_null<llvm::GlobalVariable> (gv);

	if (var == nullptr || !var->hasInitializer ())
		return false;

	const llvm::Constant *init = var->getInitializer ();

	if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt> (init)) {
		clause_index = (int) ci->getSExtValue ();
		kind = 0;
		if (selector != nullptr)
			*selector = (std::uint32_t) clause_index;
		return true;
	}

	auto *st = llvm::dyn_cast<llvm::StructType> (init->getType ());

	if (st == nullptr || st->getNumElements () < 2 || st->getNumElements () > 4)
		return false;

	const auto *ci0 =
		llvm::dyn_cast_or_null<llvm::ConstantInt> (init->getAggregateElement ((unsigned) 0));
	const auto *ci1 =
		llvm::dyn_cast_or_null<llvm::ConstantInt> (init->getAggregateElement ((unsigned) 1));

	if (ci0 == nullptr || ci1 == nullptr)
		return false;

	clause_index = (int) ci0->getSExtValue ();
	kind = (int) ci1->getZExtValue ();

	if (selector != nullptr) {
		const auto *ci3 = llvm::dyn_cast_or_null<llvm::ConstantInt> (
			init->getAggregateElement ((unsigned) 3));

		*selector = ci3 != nullptr ? (std::uint32_t) ci3->getZExtValue ()
		                           : (std::uint32_t) clause_index;
	}

	if (st->getNumElements () >= 3 && owner != nullptr) {
		const auto *ci2 = llvm::dyn_cast_or_null<llvm::ConstantInt> (
			init->getAggregateElement ((unsigned) 2));

		if (ci2 == nullptr)
			return false;

		*owner = ci2->getZExtValue ();
	}

	return true;
}

} // namespace mono

#endif
