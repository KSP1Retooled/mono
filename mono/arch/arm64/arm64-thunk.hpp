/**
 * \file
 * \brief The instructions that make up a redirectable thunk.
 */

#ifndef MONO_ARCH_ARM64_THUNK_HPP
#define MONO_ARCH_ARM64_THUNK_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mono::arch {

// clang-format off
static constexpr std::uint32_t thunk_code[8] = {
	0x00000000, 0x00000000, // +0  slot: null until the thunk is redirected
	0x91004000,             // +8  unbox: add  x0, x0, #16  (sizeof (MonoObject))
	0x58ffffb0,             // +12 entry: ldr  x16, slot
	0xd61f0200,             // +16 br   x16
	0xd4200000,             // +20 brk  #0
	0xd4200000,             // +24 brk  #0
	0xd4200000,             // +28 brk  #0
};

static constexpr std::uint32_t thunk_keyed_code[8] = {
	0x00000000, 0x00000000, // +0  slot: null until the thunk is redirected
	0x91004000,             // +8  unbox: add  x0, x0, #16  (sizeof (MonoObject))

	// +12 entry: ldr x9, key
	//
	// The key goes in x9, a scratch register that no argument is passed in, so
	// the thunk leaves x15 as the caller set it. x15 is MONO_ARCH_IMT_REG and
	// MONO_ARCH_RGCTX_REG. When a dispatched call site loads a key into x15, the
	// key still reaches the body through the thunk.
	0x58000069,
	0x58ffff90,             // +16 ldr  x16, slot
	0xd61f0200,             // +20 br   x16
	0x00000000, 0x00000000, // +24 key
};
// clang-format on

static constexpr size_t thunk_unbox_offset = 8;
static constexpr size_t thunk_entry_offset = 12;
static constexpr size_t thunk_key_offset = 24;
static constexpr size_t thunk_size = 32;

/// The size of the jump block alone, from entry_offset to the end of the
/// group. A runtime detour (Harmony, MonoMod) has to fit inside it.
static constexpr size_t thunk_block_size = thunk_size - thunk_entry_offset;

/// The alignment that the whole group needs.
static constexpr size_t thunk_alignment = 16;

/// Writes a thunk of thunk_size bytes at \p group. When \p key is given, the
/// block loads it into x9 from a literal.
inline void
write_thunk (char *group, const void *key = nullptr)
{
	if (key != nullptr) {
		std::memcpy (group, thunk_keyed_code, thunk_size);
		std::memcpy (group + thunk_key_offset, &key, sizeof (key));
	} else {
		std::memcpy (group, thunk_code, thunk_size);
	}
}

} // namespace mono::arch

#endif // MONO_ARCH_ARM64_THUNK_HPP
