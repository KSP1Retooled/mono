/**
 * \file
 * \brief Taking the `resume` back off the pads of a compiled method.
 */

#ifndef MONO_LLVM_PASSES_UNROUTED_RESUME_HPP
#define MONO_LLVM_PASSES_UNROUTED_RESUME_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Turns each `resume` into `unreachable`.
///
/// A pad resumes for a clause that is not its own method's. Inlining the method
/// into a try sends that resume on to the pad of the try. The runtime never enters
/// a pad with a clause the pad does not name, so a resume no inline moved is dead.
///
/// Run it behind every inliner. Codegen lowers a `resume` to a call of
/// _Unwind_Resume, which the runtime does not have. Run it behind the PGO hash as
/// well: a method and its inline copy have to hash the same CFG.
class DropUnroutedResumePass : public llvm::PassInfoMixin<DropUnroutedResumePass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
