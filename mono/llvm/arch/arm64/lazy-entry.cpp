/**
 * \file
 * \brief The re-entry resolver that a stub's first call lands in.
 *
 * While a method compiles, the resolver links an LMF that stands for the
 * caller, so that a stack walk during the compile still finds the managed
 * frames. After the resolver unlinks the LMF, it takes a forced interruption
 * checkpoint.
 */

#include "arch/arch.hpp"

#include "debugging/perf/jitdump.hpp"

#include "mono/metadata/profiler-private.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

namespace mono::arch {

/*
 * The CFA is chosen so that an unwind skips the trampoline between the caller
 * and the resolver. ORC's trampoline copies the caller's return address into
 * x17 before its own branch-and-link overwrites x30. The resolver's first
 * instruction stores x17 in the slot where a frame record keeps the return
 * address. The frame record that the resolver builds therefore names the
 * managed caller, and a stack walk skips the trampoline.
 *
 * The offsets below are offsets into the resolver's code. If an instruction
 * moves, these offsets have to move with it. A rule at the wrong offset unwinds
 * to the wrong frame without any error.
 */
std::vector<UnwindRecord>
lazy_resolver_frame ()
{
	const int32_t fp = dwarf_frame_pointer_reg;
	const int32_t sp = dwarf_stack_pointer_reg;
	const int32_t lr = dwarf_return_address_reg;

	return {
		{ 0x00, MONO_UNWIND_OP_DEF_CFA, sp, 0 },

		/* stp x29, x17, [sp, #-16]! */
		{ 0x04, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 16 },
		{ 0x04, MONO_UNWIND_OP_OFFSET, fp, -16 },
		{ 0x04, MONO_UNWIND_OP_OFFSET, lr, -8 },

		/* mov x29, sp */
		{ 0x08, MONO_UNWIND_OP_DEF_CFA_REGISTER, fp, 0 },

		/* ldp x29, x30, [sp], #16. Both registers hold the caller's values
		 * again. */
		{ 0xc8, MONO_UNWIND_OP_DEF_CFA, sp, 0 },
		{ 0xc8, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },
		{ 0xc8, MONO_UNWIND_OP_SAME_VALUE, lr, 0 },

		/* The throw path. Control reaches it by a branch from the body, so
		 * it needs the body's rules again, not the rules that the lines above
		 * leave in effect. */
		{ 0xcc, MONO_UNWIND_OP_DEF_CFA, fp, 16 },
		{ 0xcc, MONO_UNWIND_OP_OFFSET, fp, -16 },
		{ 0xcc, MONO_UNWIND_OP_OFFSET, lr, -8 },

		{ 0xd4, MONO_UNWIND_OP_DEF_CFA, sp, 0 },
		{ 0xd4, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },
		{ 0xd4, MONO_UNWIND_OP_SAME_VALUE, lr, 0 },
	};
}

ExecutorAddr
take_published_resolver ()
{
	return ExecutorAddr ();
}

void
unregister_resolver_unwind_info (ExecutorAddr resolver)
{
	(void) resolver;
}

/*
 * The frame is built around the state that the trampoline leaves behind. x17
 * holds the managed caller's return address, x30 holds the trampoline's own
 * return address, and sp and x29 still hold the caller's values. After the
 * frame record is pushed, the layout is:
 *
 *      [sp, #0x00]   the transition frame and its LMF. The resolver stores the
 *                    LMF's callee-saved registers itself.
 *      [sp, #0x80]   arch::LazyEntryFrame: x0..x15, x30, the landing, q0..q7
 *      [x29, #0]     the caller's x29
 *      [x29, #8]     the caller's return address
 *      x29 + 16      the caller's sp
 *
 * On the normal path, the resolver restores x29 and x30 from the frame record
 * and branches to the landing. The method is then entered as if the caller had
 * called it directly. The throw path unwinds the stack in the same way and
 * then enters the rethrow trampoline. x0 holds the exception, and x30 holds the
 * caller's return address, which becomes the throw site.
 */
void
LazyEntryABI::writeResolverCode (char *resolver_mem, ExecutorAddr resolver_addr,
                                 ExecutorAddr reentry_fn, ExecutorAddr reentry_ctx)
{
	static_assert (managed_frame_size == 0x80,
	               "the LazyEntryFrame offsets below sit above the transition frame");
	static_assert (offsetof (LazyEntryFrame, trampoline_ret) == 0x80
	                       && offsetof (LazyEntryFrame, landing) == 0x88
	                       && offsetof (LazyEntryFrame, q) == 0x90
	                       && sizeof (LazyEntryFrame) == 0x110,
	               "the spill offsets below are LazyEntryFrame's layout");

	const uint32_t resolver_code[] = {
		0xa9bf47fd, // 0x00: stp   x29, x17, [sp, #-16]!
		0x910003fd, // 0x04: mov   x29, sp
		0xd10643ff, // 0x08: sub   sp, sp, #0x190
		0xa90807e0, // 0x0c: stp   x0, x1, [sp, #0x80]
		0xa9090fe2, // 0x10: stp   x2, x3, [sp, #0x90]
		0xa90a17e4, // 0x14: stp   x4, x5, [sp, #0xa0]
		0xa90b1fe6, // 0x18: stp   x6, x7, [sp, #0xb0]
		0xa90c27e8, // 0x1c: stp   x8, x9, [sp, #0xc0]
		0xa90d2fea, // 0x20: stp   x10, x11, [sp, #0xd0]
		0xa90e37ec, // 0x24: stp   x12, x13, [sp, #0xe0]
		0xa90f3fee, // 0x28: stp   x14, x15, [sp, #0xf0]
		0xf90083fe, // 0x2c: str   x30, [sp, #0x100]
		0xad0887e0, // 0x30: stp   q0, q1, [sp, #0x110]
		0xad098fe2, // 0x34: stp   q2, q3, [sp, #0x130]
		0xad0a97e4, // 0x38: stp   q4, q5, [sp, #0x150]
		0xad0b9fe6, // 0x3c: stp   q6, q7, [sp, #0x170]

		// The LMF that stands for the caller while the method compiles.
		// MonoLMF::gregs is at offset 24 and starts with x19.
		0xa901d3f3, // 0x40: stp   x19, x20, [sp, #24]
		0xa902dbf5, // 0x44: stp   x21, x22, [sp, #40]
		0xa903e3f7, // 0x48: stp   x23, x24, [sp, #56]
		0xa904ebf9, // 0x4c: stp   x25, x26, [sp, #72]
		0xa905f3fb, // 0x50: stp   x27, x28, [sp, #88]
		0x910003e0, // 0x54: mov   x0, sp
		0xf94003a1, // 0x58: ldr   x1, [x29]
		0x910043a2, // 0x5c: add   x2, x29, #16
		0x58000410, // 0x60: ldr   x16, enter
		0xd63f0200, // 0x64: blr   x16

		0x58000400, // 0x68: ldr   x0, ctx
		0x910203e1, // 0x6c: add   x1, sp, #0x80
		0x58000410, // 0x70: ldr   x16, reentry
		0xd63f0200, // 0x74: blr   x16
		0xf90087e0, // 0x78: str   x0, [sp, #0x108]
		0x910003e0, // 0x7c: mov   x0, sp
		0x580003d0, // 0x80: ldr   x16, leave
		0xd63f0200, // 0x84: blr   x16
		0xb5000220, // 0x88: cbnz  x0, throw

		0xad4887e0, // 0x8c: ldp   q0, q1, [sp, #0x110]
		0xad498fe2, // 0x90: ldp   q2, q3, [sp, #0x130]
		0xad4a97e4, // 0x94: ldp   q4, q5, [sp, #0x150]
		0xad4b9fe6, // 0x98: ldp   q6, q7, [sp, #0x170]
		0xa94807e0, // 0x9c: ldp   x0, x1, [sp, #0x80]
		0xa9490fe2, // 0xa0: ldp   x2, x3, [sp, #0x90]
		0xa94a17e4, // 0xa4: ldp   x4, x5, [sp, #0xa0]
		0xa94b1fe6, // 0xa8: ldp   x6, x7, [sp, #0xb0]
		0xa94c27e8, // 0xac: ldp   x8, x9, [sp, #0xc0]
		0xa94d2fea, // 0xb0: ldp   x10, x11, [sp, #0xd0]
		0xa94e37ec, // 0xb4: ldp   x12, x13, [sp, #0xe0]
		0xa94f3fee, // 0xb8: ldp   x14, x15, [sp, #0xf0]
		0xf94087f1, // 0xbc: ldr   x17, [sp, #0x108]
		0x910003bf, // 0xc0: mov   sp, x29
		0xa8c17bfd, // 0xc4: ldp   x29, x30, [sp], #16
		0xd61f0220, // 0xc8: br    x17

		// throw: x0 holds the exception, and the callee-saved registers still
		// hold the caller's values. Unwind the stack to the call, then enter
		// the rethrow trampoline as if the caller had called it.
		0x910003bf, // 0xcc: mov   sp, x29
		0xa8c17bfd, // 0xd0: ldp   x29, x30, [sp], #16
		0x58000170, // 0xd4: ldr   x16, slot
		0xf9400210, // 0xd8: ldr   x16, [x16]
		0xd61f0200, // 0xdc: br    x16

		0, 0,       // 0xe0: enter: lazy_frame_enter ()
		0, 0,       // 0xe8: ctx: JIT re-entry ctx addr
		0, 0,       // 0xf0: reentry: JIT re-entry fn addr
		0, 0,       // 0xf8: leave: lazy_frame_leave ()
		0, 0,       // 0x100: slot: address of the variable that holds the rethrow trampoline
	};

	const unsigned enter_fn_offset = 0xe0;
	const unsigned reentry_ctx_offset = 0xe8;
	const unsigned reentry_fn_offset = 0xf0;
	const unsigned leave_fn_offset = 0xf8;
	const unsigned rethrow_slot_offset = 0x100;

	static_assert (sizeof (resolver_code) == ResolverCodeSize,
	               "the resolver does not fit what the pool allocated for it");

	void (*enter_fn) (void *, uint64_t, uint64_t) = &lazy_frame_enter;
	void *(*leave_fn) (void *) = &lazy_frame_leave;
	void **rethrow_slot = rethrow_trampoline_slot ();

	std::memcpy (resolver_mem, resolver_code, sizeof (resolver_code));
	std::memcpy (resolver_mem + enter_fn_offset, &enter_fn, sizeof (enter_fn));
	std::memcpy (resolver_mem + reentry_ctx_offset, &reentry_ctx, sizeof (uint64_t));
	std::memcpy (resolver_mem + reentry_fn_offset, &reentry_fn, sizeof (uint64_t));
	std::memcpy (resolver_mem + leave_fn_offset, &leave_fn, sizeof (leave_fn));
	std::memcpy (resolver_mem + rethrow_slot_offset, &rethrow_slot, sizeof (rethrow_slot));

	perf::FrameFunction fn;
	fn.size = ResolverCodeSize;
	fn.records = lazy_resolver_frame ();

	std::vector<perf::FrameFunction> functions;
	functions.push_back (std::move (fn));

	const char *name = DisplayName;

	/* The pool maps a page for the resolver alone, so the rest of that page
	 * after the resolver is free. */
	perf::publish (name,
	               { resolver_addr.toPtr<const uint8_t *> (), ResolverCodeSize,
	                 ResolverCodeSize + perf::code_slack () },
	               std::move (functions));

	/* The runtime has no MonoTrampInfo for the resolver, so it never reports
	 * the resolver to the profiler by itself. */
	MONO_PROFILER_RAISE (jit_code_stub,
	                     (resolver_addr.toPtr<const mono_byte *> (),
	                      ResolverCodeSize, name));
}

} // namespace mono::arch
