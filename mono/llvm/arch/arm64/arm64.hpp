/**
 * \file
 * \brief The arm64 definitions of the arch layer's types and constants.
 */

#ifndef MONO_LLVM_ARCH_ARM64_ARM64_HPP
#define MONO_LLVM_ARCH_ARM64_ARM64_HPP

#include "config.h"

#include "sidetables.hpp"

#include <llvm/ExecutionEngine/Orc/OrcABISupport.h>
#include <llvm/TargetParser/Triple.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mono::arch {

constexpr llvm::Triple::ArchType target_arch = llvm::Triple::aarch64;

/// Whether methods can be compiled past tier 0 here.
constexpr bool has_tier1_codegen = false;

/// What a DWARF CIE for this target says. Every instruction is four bytes, and
/// a saved register always lands on an 8-byte boundary below the CFA.
constexpr unsigned dwarf_return_address_reg = 30; /* x30, the link register */
constexpr unsigned dwarf_stack_pointer_reg = 31;  /* sp */
constexpr unsigned dwarf_frame_pointer_reg = 29;  /* x29 */
constexpr int dwarf_code_alignment_factor = 4;
constexpr int dwarf_data_alignment_factor = -8;

/// Stack the re-entry resolver reserves for the LMF it links across a compile.
/// A multiple of 16 keeps sp aligned.
constexpr unsigned managed_frame_size = 128;

/// How many registers of each file a return value's leaves can be spread over.
constexpr unsigned ret_gregs = 2, ret_scalar_fregs = 4, ret_vector_fregs = 4;

/// How much code a context stub takes, and what it wants to be aligned to.
///
/// Three instructions and a nop, then two 8-byte literals.
constexpr size_t context_stub_size = 32;
constexpr size_t context_stub_align = 16;

/// What the re-entry resolver spilled of the call it interrupted. The resolver
/// passes a pointer to this to its callback.
///
/// The callee-saved registers are not here, because every function the
/// resolver calls preserves them. The LMF the resolver links carries the copy
/// a stack walk reads.
struct LazyEntryFrame {
	/// The argument registers, x8 for an indirect return, and the scratch
	/// registers a call leaves undefined. x9 holds what a keyed thunk loaded,
	/// and x15 is MONO_ARCH_RGCTX_REG and MONO_ARCH_IMT_REG together.
	void *x [16];

	/// Where the trampoline's own branch-and-link returns to.
	void *trampoline_ret;

	/// The address the callback returned, kept here across the call that
	/// unlinks the LMF.
	void *landing;

	/// q0..q7, the floating-point and vector argument registers.
	uint8_t q [8][16];
};

/// The trampoline the call in \p frame arrived through.
///
/// ORC's AArch64 trampoline is a move, a literal load and a branch-and-link,
/// so its return address is twelve bytes past where it starts.
inline void *
trampoline_of (const LazyEntryFrame *frame)
{
	return (char *) frame->trampoline_ret - 12;
}

/// ORC's re-entry ABI, resolving through a mono lazy-entry frame.
///
/// Only the trampoline and stub writers come from the base.
struct LazyEntryABI : public llvm::orc::OrcAArch64 {
	static constexpr unsigned ResolverCodeSize = 0x108;

	/// Name reported to profilers.
	static constexpr const char *DisplayName = "mono_lazy_entry_resolver";

	static void writeResolverCode (char *resolver_mem,
	                               llvm::orc::ExecutorAddr resolver_addr,
	                               llvm::orc::ExecutorAddr reentry_fn,
	                               llvm::orc::ExecutorAddr reentry_ctx);
};

/// arm64 passes no vector in a register wider than a q register, so the
/// wide-vector resolver is the ordinary one.
using LazyEntryAvxABI = LazyEntryABI;

/// Builds the CFI program for LazyEntryABI's re-entry resolver, which unwinds
/// to the managed caller rather than to the trampoline.
std::vector<UnwindRecord> lazy_resolver_frame ();

} // namespace mono::arch

#endif
