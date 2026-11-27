/**
 * \file
 * \brief The LMF that marks a managed-to-native transition on arm64.
 */

/*
 * Before anything else, so that MonoError is the internal struct the rest of
 * the runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "arch/arch.hpp"

#include "mini.h"
#include "mini-runtime.h"

#include "mono/utils/mono-tls-inline.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/Support/ErrorHandling.h>

#include <cstddef>
#include <cstdint>

namespace mono::arch {

/// The frame that the re-entry resolver reserves stack space for. The LMF holds
/// the caller's pc, callee-saved registers, fp and sp, which are all the values
/// that a stack walk restores when it crosses an LMF. The resolver stores the
/// callee-saved registers itself.
struct TransitionFrame {
	MonoLMF lmf;
	MonoLMF **addr;
};

static_assert (sizeof (TransitionFrame) <= managed_frame_size,
               "a caller does not reserve enough for a transition frame");
static_assert (offsetof (TransitionFrame, lmf) == 0
                       && offsetof (MonoLMF, gregs) == 24,
               "the resolver stores x19..x28 at [sp, #24]");

void
lazy_frame_enter (void *frame, uint64_t caller_fp, uint64_t caller_sp)
{
	TransitionFrame *transition = static_cast<TransitionFrame *> (frame);

	/*
	 * No LMF chain means that the thread has not run managed code. Such a
	 * thread reaches a stub only when the runtime calls the stub directly. In
	 * that case there is no managed caller for a stack walk to find, and no
	 * managed code that an abort could be delivered to.
	 */
	transition->addr = mono_tls_get_lmf_addr ();

	if (!transition->addr)
		return;

	/* The resolver's frame record sits just below the caller's sp, and its
	 * second word is the caller's return address. */
	transition->lmf.pc = ((host_mgreg_t *) caller_sp) [-1];
	transition->lmf.gregs [MONO_ARCH_LMF_REG_FP] = caller_fp;
	transition->lmf.gregs [MONO_ARCH_LMF_REG_SP] = caller_sp;
	transition->lmf.lmf_addr = transition->addr;
	transition->lmf.previous_lmf = *transition->addr;
	*transition->addr = &transition->lmf;
}

void *
lazy_frame_leave (void *frame)
{
	TransitionFrame *transition = static_cast<TransitionFrame *> (frame);

	if (!transition->addr)
		return nullptr;

	*transition->addr = (MonoLMF *) (((gsize) transition->lmf.previous_lmf) & ~3);

	/*
	 * When a thread is aborted while it is inside the compiler, the runtime
	 * sets a flag instead of hijacking the thread. This checkpoint reads the
	 * flag.
	 */
	return mono_thread_force_interruption_checkpoint_noraise ();
}

void **
rethrow_trampoline_slot ()
{
	return (void **) mono_get_rethrow_preserve_exception_addr ();
}

void
emit_callee_saved_clobber (llvm::IRBuilderBase &b)
{
	const char *clobbers = "~{x19},~{x20},~{x21},~{x22},~{x23},~{x24},~{x25},"
	                       "~{x26},~{x27},~{x28}";

	b.CreateCall (llvm::InlineAsm::get (
		llvm::FunctionType::get (b.getVoidTy (), false), "", clobbers,
		/*hasSideEffects=*/true));
}

llvm::Value *
emit_lmf_address (llvm::IRBuilderBase &b)
{
	return nullptr;
}

llvm::Value *
emit_sgen_thread_info (llvm::IRBuilderBase &b)
{
	return nullptr;
}

void
emit_lmf_capture_registers (llvm::IRBuilderBase &b, llvm::Value *slot)
{
	llvm::report_fatal_error ("mono: arm64 has no tier-1 code generator", false);
}

} // namespace mono::arch
