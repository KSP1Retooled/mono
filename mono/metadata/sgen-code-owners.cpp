#include "config.h"

#include "sgen-code-owners.h"

#include <iterator>
#include <map>

guint8 sgen_code_owner_filter [SGEN_CODE_OWNER_FILTER_SIZE];

namespace {

struct Range {
	gsize end;
	MonoGCHandle owner;
};

/// The ranges by start address. They do not overlap.
///
/// Null until the first range is added. A collection reads this while a stopped thread can
/// hold the heap's lock, so nothing it calls allocates. Never freed, because a collection
/// can run after the static destructors do.
std::map<gsize, Range> *ranges;

} // namespace

void
sgen_code_owners_add (gsize start, gsize size, MonoGCHandle owner)
{
	if (ranges == nullptr)
		ranges = new std::map<gsize, Range> ();

	std::map<gsize, Range> &table = *ranges;
	gsize end = start + size;
	auto at = table.lower_bound (start);

	if (at != table.begin () && std::prev (at)->second.end > start)
		--at;

	// New code lies here, so the code of an overlapped range is gone.
	while (at != table.end () && at->first < end)
		at = table.erase (at);

	table.emplace_hint (at, start, Range { end, owner });

	// Through end, because a range holds that address too.
	for (gsize block = start >> SGEN_CODE_OWNER_BLOCK_BITS; block <= end >> SGEN_CODE_OWNER_BLOCK_BITS; ++block)
		sgen_code_owner_filter [sgen_code_owner_filter_slot (block << SGEN_CODE_OWNER_BLOCK_BITS)] = 1;
}

void
sgen_code_owners_remove (gsize start, gsize size)
{
	g_assert (ranges != nullptr);

	auto at = ranges->find (start);

	g_assert (at != ranges->end () && at->second.end == start + size);
	ranges->erase (at);
}

gboolean
sgen_code_owners_empty (void)
{
	return ranges == nullptr || ranges->empty ();
}

int
sgen_code_owners_find (gsize addr, MonoGCHandle owners [SGEN_CODE_OWNERS_MAX])
{
	if (ranges == nullptr)
		return 0;

	auto at = ranges->upper_bound (addr);
	int count = 0;

	// One range can start at addr while the range in front of it ends there.
	while (at != ranges->begin () && count < SGEN_CODE_OWNERS_MAX) {
		--at;
		if (at->second.end < addr)
			break;
		owners [count++] = at->second.owner;
	}

	return count;
}
