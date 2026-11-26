/**
 * \file
 * \brief What exception dispatch and stack walking assume about the machine.
 */

#include "arch/arch.hpp"

#include "mini.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>

namespace mono::arch {

bool
reg_is_recoverable (int hw_reg)
{
	return hw_reg == ARMREG_SP || hw_reg == ARMREG_FP
	       || (hw_reg >= ARMREG_R19 && hw_reg <= ARMREG_R28);
}

int
dwarf_reg_for_win_unwind (unsigned unwind_reg, bool is_vector)
{
	/* arm64 here runs on Linux alone, which describes frames in DWARF. */
	return -1;
}

llvm::Value *
emit_entered_exception (llvm::IRBuilderBase &b)
{
	llvm::Type *ptr = llvm::PointerType::get (b.getContext (), 0);

	return b.CreateCall (
		llvm::InlineAsm::get (llvm::FunctionType::get (ptr, false), "", "={x0}",
	                              /*hasSideEffects=*/true));
}

} // namespace mono::arch
