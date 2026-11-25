/*
 * Tests for the code memory JIT'd objects are linked into.
 *
 * Protections are read from the operating system -- /proc/self/maps, or
 * VirtualQuery -- rather than out of the arena's own bookkeeping, so a mapping
 * that is not what it claims cannot pass by
 * agreeing with itself. The property nothing else here would catch is reach:
 * every fixup inside an object, and the .eh_frame delta back to the code, is a
 * PCRel32 that JITLink hard-errors on rather than stubbing, so two reservations
 * that end up more than 2GB apart break a compile a long way from here.
 */

#include "config.h"

#include "jitlink-memory.hpp"

#include "harness.hpp"

#ifdef HOST_WIN32
#include <windows.h>
#endif

#include <gtest/gtest.h>

#include <llvm/Support/Error.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/*
 * An arena reaches mono's code manager, which wants the runtime's counters and
 * profiler up - so these need a runtime, which needs a class library to boot on.
 */
class CodeMemory : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
	}

protected:
	CodeArena arena;

	char *must_reserve (size_t size, size_t align)
	{
		Expected<char *> mem = arena.reserve (size, align);
		EXPECT_TRUE (bool (mem)) << toString (mem.takeError ());
		return mem ? *mem : nullptr;
	}
};

#ifdef HOST_WIN32

/// The base page protection Windows reports for addr, or zero where it names
/// nothing committed. The guard and caching bits are masked off, so this is one
/// of the PAGE_* values on its own.
DWORD
mapping_protection (const void *addr)
{
	MEMORY_BASIC_INFORMATION info;

	if (VirtualQuery (addr, &info, sizeof (info)) == 0)
		return 0;
	if (info.State != MEM_COMMIT)
		return 0;

	return info.Protect & 0xff;
}

bool
is_writable (const void *addr)
{
	DWORD protection = mapping_protection (addr);

	return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY
	       || protection == PAGE_EXECUTE_READWRITE
	       || protection == PAGE_EXECUTE_WRITECOPY;
}

bool
is_executable (const void *addr)
{
	DWORD protection = mapping_protection (addr);

	return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
	       || protection == PAGE_EXECUTE_READWRITE
	       || protection == PAGE_EXECUTE_WRITECOPY;
}

#else

/// What /proc/self/maps says about addr, as the four rwxp characters.
std::string
mapping_perms (const void *addr)
{
	FILE *maps = fopen ("/proc/self/maps", "r");
	uintptr_t want = reinterpret_cast<uintptr_t> (addr);
	char line[512];
	std::string perms;

	if (!maps)
		return perms;

	while (fgets (line, sizeof (line), maps)) {
		unsigned long long start = 0, end = 0;
		char got[8] = { 0 };

		if (sscanf (line, "%llx-%llx %4s", &start, &end, got) != 3)
			continue;
		if (want >= start && want < end) {
			perms = got;
			break;
		}
	}

	fclose (maps);
	return perms;
}

bool
is_writable (const void *addr)
{
	std::string perms = mapping_perms (addr);
	return perms.size () >= 2 && perms[1] == 'w';
}

bool
is_executable (const void *addr)
{
	std::string perms = mapping_perms (addr);
	return perms.size () >= 3 && perms[2] == 'x';
}

#endif

TEST_F (CodeMemory, ReservationsAreDisjoint)
{
	char *a = must_reserve (64, 16);
	char *b = must_reserve (64, 16);
	char *c = must_reserve (200, 16);

	ASSERT_NE (a, nullptr);
	ASSERT_NE (b, nullptr);
	ASSERT_NE (c, nullptr);

	EXPECT_TRUE (a + 64 <= b || b + 64 <= a);
	EXPECT_TRUE (a + 64 <= c || c + 200 <= a);
	EXPECT_TRUE (b + 64 <= c || c + 200 <= b);
}

TEST_F (CodeMemory, ReservationsAreWritableAndExecutable)
{
	char *a = must_reserve (128, 16);

	ASSERT_NE (a, nullptr);
	EXPECT_TRUE (is_writable (a));
	EXPECT_TRUE (is_executable (a));

	/* Writable means writable, not merely marked so. */
	memset (a, 0xcc, 128);
}

/*
 * The code manager promises 16, so anything wider is served by over-allocating.
 * A constant pool of AVX vectors is where this comes up.
 */
TEST_F (CodeMemory, AlignmentAboveWhatTheCodeManagerPromises)
{
	must_reserve (7, 1);

	for (size_t align : { size_t (16), size_t (32), size_t (64), size_t (256) }) {
		char *a = must_reserve (32, align);

		ASSERT_NE (a, nullptr);
		EXPECT_EQ (reinterpret_cast<uintptr_t> (a) % align, 0u)
			<< "at alignment " << align;
	}
}

/*
 * Every fixup a compiled object makes to itself is a PCRel32 with no stub to
 * fall back on, so two allocations have to stay inside what one can encode. The
 * code manager gets that by mapping its chunks MAP_32BIT.
 */
TEST_F (CodeMemory, ReservationsStayWithinPcrel32Reach)
{
	constexpr uintptr_t reach = uintptr_t (2) * 1024 * 1024 * 1024;
	std::vector<char *> mem;

	for (int i = 0; i < 64; i++) {
		char *a = must_reserve (4096, 16);

		ASSERT_NE (a, nullptr);
		mem.push_back (a);
	}

	for (char *a : mem)
		EXPECT_LT (reinterpret_cast<uintptr_t> (a) + 4096, reach);
}

TEST_F (CodeMemory, ConcurrentReservationsDoNotOverlap)
{
	constexpr int threads_count = 8;
	constexpr int per_thread = 500;
	constexpr size_t size = 48;

	std::vector<std::thread> threads;
	std::vector<char *> mem;
	std::mutex mutex;
	std::atomic<int> failures { 0 };

	for (int t = 0; t < threads_count; t++)
		threads.emplace_back ([&] {
			std::vector<char *> mine;

			for (int i = 0; i < per_thread; i++) {
				Expected<char *> a = arena.reserve (size, 16);

				if (!a) {
					consumeError (a.takeError ());
					failures++;
					return;
				}
				memset (*a, 0x90, size);
				mine.push_back (*a);
			}

			std::lock_guard<std::mutex> lock (mutex);
			mem.insert (mem.end (), mine.begin (), mine.end ());
		});

	for (std::thread &t : threads)
		t.join ();

	EXPECT_EQ (failures.load (), 0);
	ASSERT_EQ (mem.size (), size_t (threads_count * per_thread));

	std::sort (mem.begin (), mem.end ());
	for (size_t i = 1; i < mem.size (); i++)
		ASSERT_GE (mem[i] - mem[i - 1], ptrdiff_t (size))
			<< "reservations " << (i - 1) << " and " << i << " overlap";
}

TEST_F (CodeMemory, AReleasedBlockIsUsedAgain)
{
	for (size_t size : { size_t (1), size_t (32), size_t (33), size_t (500), size_t (70000) }) {
		Expected<char *> a = arena.reserve_block (size, 16);
		ASSERT_TRUE (bool (a)) << toString (a.takeError ());

		memset (*a, 0x90, size);
		arena.release_block (*a, size);

		Expected<char *> b = arena.reserve_block (size, 16);
		ASSERT_TRUE (bool (b)) << toString (b.takeError ());
		EXPECT_EQ (*a, *b) << "at size " << size;
	}
}

TEST_F (CodeMemory, ReusedBlocksHoldWhatWasAskedFor)
{
	struct Block {
		char *base;
		size_t size;
		char fill;
	};

	std::vector<Block> live;
	uint32_t seed = 1;
	auto next = [&] (uint32_t limit) {
		seed = seed * 1664525 + 1013904223;
		return (seed >> 8) % limit;
	};

	for (int i = 0; i < 5000; i++) {
		if (live.size () > 40 || (!live.empty () && next (3) == 0)) {
			size_t at = next (uint32_t (live.size ()));
			Block going = live[at];

			for (size_t j = 0; j < going.size; j++)
				ASSERT_EQ (going.base[j], going.fill) << "after " << i << " steps";
			arena.release_block (going.base, going.size);
			live.erase (live.begin () + at);
			continue;
		}

		size_t size = 1 + next (900);
		size_t align = size_t (1) << next (7);
		Expected<char *> base = arena.reserve_block (size, align);

		ASSERT_TRUE (bool (base)) << toString (base.takeError ());
		ASSERT_EQ (uintptr_t (*base) % align, 0u);
		memset (*base, char (i), size);
		live.push_back ({ *base, size, char (i) });
	}
}

#ifdef HOST_WIN32

/// The address RtlLookupFunctionEntry () says the function at code begins at, or
/// null where it knows of none.
void *
described_function (char *code)
{
	DWORD64 image_base = 0;
	PRUNTIME_FUNCTION found = RtlLookupFunctionEntry (DWORD64 (code), &image_base, nullptr);

	return found != nullptr ? (void *) (image_base + found->BeginAddress) : nullptr;
}

TEST_F (CodeMemory, ARetractedFunctionTableLeavesItsNeighbours)
{
	constexpr size_t code_size = 64;
	struct Object {
		uint8_t code[code_size];
		uint8_t unwind[16];
		uint32_t record[3];
	};

	std::vector<Object *> objects;
	auto publish = [&] (Object *object) {
		memset (object, 0, sizeof (Object));
		object->unwind[0] = 1;
		object->record[0] = uint32_t (uintptr_t (object->code));
		object->record[1] = uint32_t (uintptr_t (object->code + code_size));
		object->record[2] = uint32_t (uintptr_t (object->unwind));
		arena.publish_function_table (object->record, sizeof (object->record));
	};

	for (int i = 0; i < 5; i++) {
		Expected<char *> base = arena.reserve_block (sizeof (Object), 16);
		ASSERT_TRUE (bool (base)) << toString (base.takeError ());

		objects.push_back ((Object *) *base);
		publish (objects.back ());
	}

	arena.retract_function_table (objects[2], sizeof (Object));
	for (int i = 0; i < 5; i++)
		EXPECT_EQ (described_function ((char *) objects[i]->code + 8),
		           i == 2 ? nullptr : objects[i]->code);

	publish (objects[2]);
	for (int i = 0; i < 5; i++)
		EXPECT_EQ (described_function ((char *) objects[i]->code + 8), objects[i]->code);
}

#endif

} // namespace
} // namespace test
} // namespace mono
