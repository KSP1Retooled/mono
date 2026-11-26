/**
 * \file
 * \brief Turning the `resume`s that no inline used back into `unreachable`.
 */

#ifndef MONO_LLVM_PASSES_UNROUTED_RESUME_HPP
#define MONO_LLVM_PASSES_UNROUTED_RESUME_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Turns each `resume` into `unreachable`.
///
/// A landing pad runs `resume` for a selector that belongs to none of its own
/// method's clauses. When the method is inlined into a try, LLVM replaces that
/// `resume` with a branch to the try's pad. The runtime never enters a pad with a
/// clause that the pad does not list, so a `resume` that no inline replaced never
/// runs.
///
/// Run this pass after every inliner, because codegen lowers a `resume` to a call
/// of _Unwind_Resume, and the runtime does not provide that function. Also run it
/// after the PGO hash, so that a method and its inline copy hash the same CFG.
class DropUnroutedResumePass : public llvm::PassInfoMixin<DropUnroutedResumePass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
