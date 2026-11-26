/**
 * \file
 * \brief The stub that hands a shared body the context of one instantiation.
 */

#include "arch/arch.hpp"

#include <cstdint>
#include <cstring>

namespace mono::arch {

void
write_context_stub (char *at, void *context, void *target)
{
	/* x15 is MONO_ARCH_RGCTX_REG. The target is a literal because a direct
	 * branch reaches only 128MB, and nothing keeps this target's code chunks
	 * that close together. x16 is the intra-procedure-call scratch register,
	 * which a branch between functions may clobber anyway. */
	static const uint32_t code[] = {
		0x5800008f, // ldr  x15, context
		0x580000b0, // ldr  x16, target
		0xd61f0200, // br   x16
		0xd503201f, // nop
	};
	static_assert (sizeof (code) + 2 * sizeof (uint64_t) == context_stub_size,
	               "the stub is its code and its two literals");

	uint64_t key = (uint64_t) (uintptr_t) context;
	uint64_t entry = (uint64_t) (uintptr_t) target;

	memcpy (at, code, sizeof (code));
	memcpy (at + sizeof (code), &key, sizeof (key));
	memcpy (at + sizeof (code) + sizeof (key), &entry, sizeof (entry));
}

} // namespace mono::arch
