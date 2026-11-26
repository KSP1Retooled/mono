/**
 * \file
 * \brief The arm64 instruction hints that the translator emits.
 */

#include "arch/arch.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicsAArch64.h>

namespace mono::arch {

void
emit_spin_wait_hint (llvm::IRBuilderBase &b)
{
	// `hint #1` is `yield`.
	b.CreateIntrinsic (llvm::Intrinsic::aarch64_hint, { b.getInt32 (1) });
}

} // namespace mono::arch
