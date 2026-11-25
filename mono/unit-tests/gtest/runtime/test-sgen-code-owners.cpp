#include "config.h"

#include <mono/metadata/sgen-code-owners.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

namespace {

struct Range {
	gsize start;
	gsize end;
	gsize owner;
};

/// Random changes to the ranges of one region, against a list searched end to end.
class CodeOwners : public ::testing::Test {
protected:
	static constexpr gsize base = 0x10000000;
	static constexpr gsize unit = 16;

	std::mt19937_64 random { 1 };
	std::vector<Range> model;
	gsize next_owner = 1;

	gsize
	below (gsize bound)
	{
		return random () % bound;
	}

	void
	add (gsize start, gsize size)
	{
		gsize end = start + size;

		model.erase (std::remove_if (model.begin (), model.end (),
		                             [&] (const Range &r) { return r.start < end && start < r.end; }),
		             model.end ());
		model.push_back ({ start, end, next_owner });
		sgen_code_owners_add (start, size, (MonoGCHandle) next_owner);
		++next_owner;
	}

	void
	remove (size_t index)
	{
		Range r = model [index];

		model [index] = model.back ();
		model.pop_back ();
		sgen_code_owners_remove (r.start, r.end - r.start);
	}

	void
	check (gsize addr)
	{
		std::set<gsize> expected, found;

		for (const Range &r : model)
			if (r.start <= addr && addr <= r.end)
				expected.insert (r.owner);

		MonoGCHandle owners [SGEN_CODE_OWNERS_MAX];
		int count = sgen_code_owners_find (addr, owners);

		for (int i = 0; i < count; ++i)
			found.insert ((gsize) owners [i]);

		ASSERT_EQ (expected, found) << "at " << std::hex << addr;
		ASSERT_EQ ((size_t) count, found.size ());
		if (count > 0)
			ASSERT_TRUE (sgen_code_owner_possible (addr));
	}

	void
	TearDown () override
	{
		while (!model.empty ())
			remove (model.size () - 1);
		EXPECT_TRUE (sgen_code_owners_empty ());
	}
};

TEST_F (CodeOwners, AMeetingAddressHasBothOwners)
{
	add (base, 64);
	add (base + 64, 64);
	for (gsize addr = base - 2; addr < base + 132; ++addr)
		check (addr);
}

TEST_F (CodeOwners, NewCodeReplacesWhatItOverlaps)
{
	for (gsize i = 0; i < 8; ++i)
		add (base + i * 64, 64);
	add (base + 32, 5 * 64);
	EXPECT_EQ (model.size (), 3u);
	for (gsize addr = base - 2; addr < base + 8 * 64 + 4; ++addr)
		check (addr);
}

TEST_F (CodeOwners, OneOwnerIsWrittenOnce)
{
	MonoGCHandle owners [SGEN_CODE_OWNERS_MAX];

	add (base, 64);
	EXPECT_EQ (sgen_code_owners_find (base + 8, owners), 1);
	EXPECT_EQ (sgen_code_owners_find (base + 64, owners), 1);
	EXPECT_EQ (sgen_code_owners_find (base + 65, owners), 0);
	EXPECT_EQ (sgen_code_owners_find (0, owners), 0);
}

TEST_F (CodeOwners, AgreesWithALinearSearch)
{
	const gsize region = 1 << 15;

	// The region grows full, empties and grows again, so that nodes split and go away.
	for (int round = 0; round < 4; ++round) {
		int add_percent = round % 2 == 0 ? 90 : 10;

		for (int step = 0; step < 6000; ++step) {
			if (below (100) < (gsize) add_percent || model.empty ())
				add (base + below (region) * unit, (1 + below (4)) * unit);
			else
				remove (below (model.size ()));
			check (base + below (region) * unit + below (2));
		}

		for (const Range &r : std::vector<Range> (model)) {
			check (r.start);
			check (r.end);
		}
	}
}

} // namespace
