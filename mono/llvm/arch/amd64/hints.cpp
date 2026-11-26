/**
 * \file
 * \brief The x86 instruction hints that the translator emits.
 */

#include "arch/arch.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicsX86.h>

namespace mono::arch {

void
emit_spin_wait_hint (llvm::IRBuilderBase &b)
{
	b.CreateIntrinsic (llvm::Intrinsic::x86_sse2_pause, llvm::ArrayRef<llvm::Value *> ());
}

} // namespace mono::arch
