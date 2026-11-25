#include "config.h"

#include "sgen-code-owners.h"

#include <llvm/ADT/IntervalMap.h>

guint8 sgen_code_owner_filter [SGEN_CODE_OWNER_FILTER_SIZE];

namespace {

/// Half-open ranges that stay separate where they meet, so that a range is removed with the
/// bounds it was added with.
struct RangeTraits : llvm::IntervalMapHalfOpenInfo<gsize> {
	static bool
	adjacent (gsize, gsize)
	{
		return false;
	}
};

using RangeMap = llvm::IntervalMap<gsize, MonoGCHandle,
                                   llvm::IntervalMapImpl::NodeSizer<gsize, MonoGCHandle>::LeafSize,
                                   RangeTraits>;

struct Ranges {
	RangeMap::Allocator allocator;
	RangeMap map { allocator };
};

/// Null until the first range is added. Never freed, because a collection can run after the
/// static destructors do.
Ranges *ranges;

} // namespace

void
sgen_code_owners_add (gsize start, gsize size, MonoGCHandle owner)
{
	g_assert (size > 0);

	if (ranges == nullptr)
		ranges = new Ranges ();

	gsize end = start + size;
	auto at = ranges->map.find (start);

	// New code lies here, so the code of an overlapped range is gone.
	while (at.valid () && at.start () < end)
		at.erase ();

	at.insert (start, end, owner);

	// Through end, because a range holds that address too.
	for (gsize block = start >> SGEN_CODE_OWNER_BLOCK_BITS; block <= end >> SGEN_CODE_OWNER_BLOCK_BITS; ++block)
		sgen_code_owner_filter [sgen_code_owner_filter_slot (block << SGEN_CODE_OWNER_BLOCK_BITS)] = 1;
}

void
sgen_code_owners_remove (gsize start, gsize size)
{
	g_assert (ranges != nullptr);

	auto at = ranges->map.find (start);

	g_assert (at.valid () && at.start () == start && at.stop () == start + size);
	at.erase ();
}

gboolean
sgen_code_owners_empty (void)
{
	return ranges == nullptr || ranges->map.empty ();
}

int
sgen_code_owners_find (gsize addr, MonoGCHandle owners [SGEN_CODE_OWNERS_MAX])
{
	if (ranges == nullptr)
		return 0;

	// A stopped thread can hold the heap's lock, and an iterator allocates once the tree is
	// deep enough. lookup () never does.
	MonoGCHandle holds = ranges->map.lookup (addr);
	MonoGCHandle ends = ranges->map.lookup (addr - 1);
	int count = 0;

	if (holds != nullptr)
		owners [count++] = holds;
	if (ends != nullptr && ends != holds)
		owners [count++] = ends;

	return count;
}
