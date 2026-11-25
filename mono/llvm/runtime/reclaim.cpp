#include "reclaim.hpp"

#include "debugging/perf/jitdump.hpp"

#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/gc-internals.h"

namespace mono {

bool
code_is_reclaimable (MonoMethod *method)
{
	if (!method->dynamic || !mono_gc_is_moving ()
	    || mono_jit_code_owner_handle (method) == nullptr)
		return false;

	// A dump names each range once, so a range cannot be another method's later.
	if (perf::enabled ())
		return false;

	// A breakpoint is a byte in the body, and clearing one writes it.
	return !mini_get_debug_options ()->gen_sdb_seq_points;
}

} // namespace mono
