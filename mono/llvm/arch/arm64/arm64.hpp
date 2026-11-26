/**
 * \file
 * \brief The types and constants that the arch layer defines for arm64.
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

/// The values that a DWARF CIE gives for arm64. Every instruction is four bytes
/// long, and every saved register is stored at a multiple of 8 bytes below the
/// CFA.
constexpr unsigned dwarf_return_address_reg = 30; /* x30, the link register */
constexpr unsigned dwarf_stack_pointer_reg = 31;  /* sp */
constexpr unsigned dwarf_frame_pointer_reg = 29;  /* x29 */
constexpr int dwarf_code_alignment_factor = 4;
constexpr int dwarf_data_alignment_factor = -8;

/// The stack space that the re-entry resolver reserves for the LMF that it
/// links while a method compiles. The size is a multiple of 16 so that sp stays
/// aligned.
constexpr unsigned managed_frame_size = 128;

/// How many general, scalar floating-point and vector registers a return value
/// can be split across.
constexpr unsigned ret_gregs = 2, ret_scalar_fregs = 4, ret_vector_fregs = 4;

/// The size of a context stub, and the alignment that it needs. A stub is three
/// instructions and a nop, followed by two 8-byte literals.
constexpr size_t context_stub_size = 32;
constexpr size_t context_stub_align = 16;

/// The registers that the re-entry resolver saves from the call that it
/// interrupted. The resolver passes its callback a pointer to this struct.
///
/// The callee-saved registers are not saved here, because every function that
/// the resolver calls preserves them. The LMF that the resolver links holds the
/// copy that a stack walk reads.
struct LazyEntryFrame {
	/// The argument registers, x8 for an indirect return, and the scratch
	/// registers that a call does not preserve. x9 holds the key that a keyed
	/// thunk loaded, and x15 is both MONO_ARCH_RGCTX_REG and MONO_ARCH_IMT_REG.
	void *x [16];

	/// The address that the trampoline's branch-and-link returns to.
	void *trampoline_ret;

	/// The address that the callback returned. It is kept here while the
	/// resolver calls the function that unlinks the LMF.
	void *landing;

	/// q0..q7, the floating-point and vector argument registers.
	uint8_t q [8][16];
};

/// The trampoline that the call in \p frame came through.
///
/// ORC's AArch64 trampoline is a move, a literal load and a branch-and-link,
/// so its return address is 12 bytes past its start.
inline void *
trampoline_of (const LazyEntryFrame *frame)
{
	return (char *) frame->trampoline_ret - 12;
}

/// ORC's re-entry ABI, with a resolver that builds a mono lazy-entry frame.
///
/// Only the code that writes trampolines and stubs comes from the base class.
struct LazyEntryABI : public llvm::orc::OrcAArch64 {
	static constexpr unsigned ResolverCodeSize = 0x108;

	/// Name reported to profilers.
	static constexpr const char *DisplayName = "mono_lazy_entry_resolver";

	static void writeResolverCode (char *resolver_mem,
	                               llvm::orc::ExecutorAddr resolver_addr,
	                               llvm::orc::ExecutorAddr reentry_fn,
	                               llvm::orc::ExecutorAddr reentry_ctx);
};

/// No arm64 argument register is wider than a q register, so the resolver for
/// wide vectors is the same as the ordinary one.
using LazyEntryAvxABI = LazyEntryABI;

/// Builds the CFI program for the re-entry resolver of LazyEntryABI. The
/// program unwinds to the managed caller, not to the trampoline.
std::vector<UnwindRecord> lazy_resolver_frame ();

} // namespace mono::arch

#endif
