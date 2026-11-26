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

/// The selector of clause 0 in an inline copy. The copy's other clauses take the
/// selectors that follow it. A function without this attribute numbers its
/// selectors from 0.
constexpr llvm::StringRef clause_selector_attribute = "mono-clause-selector";

/// The first selector that an inline copy with clauses gets under a root. It is
/// larger than any clause index, because a method's clause section holds at most
/// 2^24 bytes.
constexpr std::uint32_t first_copy_selector = 1u << 24;

/// Reads the clause index, kind, owner and selector out of the clause marker
/// \p gv, the global that a landing pad's catch operand names. Returns false if
/// \p gv is not a marker. \p owner and \p selector may be null.
///
/// A marker has one of these forms:
/// - {i32 clause_index, i32 kind, i64 owner, i32 selector}
/// - {i32 clause_index, i32 kind, i64 owner}
/// - {i32 clause_index, i32 kind}, which leaves \p owner unchanged
/// - a bare i32 clause_index with kind 0, which also leaves \p owner unchanged
///
/// owner is the MonoMethod* that clause_index indexes into, as an integer. A
/// form without a selector word uses its clause index as its selector.
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

	// An all-zero struct is a ConstantAggregateZero rather than a ConstantStruct,
	// so each word is read with getAggregateElement.
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
