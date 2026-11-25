/*
 * test-sgen-gchandle.cpp: Unit test for the representation of SGen's GC
 * handles, which Unity depends on: it dereferences a strong handle.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/gc-internals.h"
#include "metadata/object-internals.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/handle.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/unity-utils.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>

#include "harness.hpp"

namespace {

#if defined(_MSC_VER)
#define NOINLINE __declspec (noinline)
#else
#define NOINLINE __attribute__ ((noinline))
#endif

bool is_tagged (MonoGCHandle handle)
{
	return ((gsize) handle & 1) != 0;
}

MonoObject *dereference (MonoGCHandle handle)
{
	return *(MonoObject *volatile *) handle;
}

MonoObject *new_target (int value)
{
	ERROR_DECL (error);
	MonoArray *array = mono_array_new_checked (mono_domain_get (), mono_defaults.int32_class, 1, error);
	mono_error_assert_ok (error);
	mono_array_set_internal (array, gint32, 0, value);
	return (MonoObject *) array;
}

int target_value (MonoObject *target)
{
	return mono_array_get_internal ((MonoArray *) target, gint32, 0);
}

/* Leaves no plain pointer to the targets in the caller's frame, so a collection
 * is free to move them. */
NOINLINE void allocate_handles (std::vector<MonoGCHandle> &handles, std::vector<gsize> &hidden_addresses)
{
	for (size_t i = 0; i < handles.size (); i++) {
		MonoObject *target = new_target ((int) i);
		handles [i] = mono_gchandle_new_internal (target, FALSE);
		hidden_addresses [i] = ~(gsize) target;
	}
}

NOINLINE size_t count_moved (const std::vector<MonoGCHandle> &handles, const std::vector<gsize> &hidden_addresses)
{
	size_t moved = 0;
	for (size_t i = 0; i < handles.size (); i++)
		moved += ~(gsize) dereference (handles [i]) != hidden_addresses [i];
	return moved;
}

class SgenGCHandle : public ::testing::Test {
protected:
	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();
	}
};

TEST_F (SgenGCHandle, StrongHandleIsTheAddressOfItsSlot)
{
	MonoObject *target = new_target (1);
	for (gboolean pinned : { FALSE, TRUE }) {
		MonoGCHandle handle = mono_gchandle_new_internal (target, pinned);
		EXPECT_FALSE (is_tagged (handle));
		EXPECT_EQ (target, dereference (handle));
		EXPECT_EQ (target, mono_gchandle_get_target_internal (handle));
		EXPECT_EQ (pinned ? HANDLE_PINNED : HANDLE_NORMAL, mono_gchandle_get_type_internal (handle));

		mono_gchandle_free_internal (handle);
		EXPECT_EQ (nullptr, dereference (handle));
		EXPECT_EQ (nullptr, mono_gchandle_get_target_internal (handle));
		EXPECT_FALSE (mono_gchandle_is_in_domain_internal (handle, mono_domain_get ()));
	}
}

TEST_F (SgenGCHandle, WeakHandleIsTagged)
{
	MonoObject *target = new_target (1);
	for (gboolean track : { FALSE, TRUE }) {
		MonoGCHandle handle = mono_gchandle_new_weakref_internal (target, track);
		EXPECT_TRUE (is_tagged (handle));
		EXPECT_EQ (target, mono_gchandle_get_target_internal (handle));
		EXPECT_EQ (track ? HANDLE_WEAK_TRACK : HANDLE_WEAK, mono_gchandle_get_type_internal (handle));
		mono_gchandle_free_internal (handle);
	}
}

TEST_F (SgenGCHandle, SlotFollowsAMovedTarget)
{
	std::vector<MonoGCHandle> handles (2000);
	std::vector<gsize> hidden_addresses (handles.size ());
	allocate_handles (handles, hidden_addresses);

	mono_gc_collect (0);
	EXPECT_GT (count_moved (handles, hidden_addresses), 0u) << "the collection moved nothing";
	mono_gc_collect (mono_gc_max_generation ());

	for (size_t i = 0; i < handles.size (); i++) {
		MonoObject *target = dereference (handles [i]);
		ASSERT_EQ (0u, (gsize) target & 7);
		ASSERT_EQ (target, mono_gchandle_get_target_internal (handles [i]));
		ASSERT_EQ ((int) i, target_value (target));
	}

	for (MonoGCHandle handle : handles)
		mono_gchandle_free_internal (handle);
}

TEST_F (SgenGCHandle, NullTargetReadsAsNull)
{
	MonoDomain *domain = mono_domain_get ();
	for (gboolean pinned : { FALSE, TRUE }) {
		MonoGCHandle empty = mono_gchandle_new_internal (nullptr, pinned);
		EXPECT_EQ (nullptr, dereference (empty));
		EXPECT_TRUE (mono_gchandle_is_in_domain_internal (empty, domain));

		MonoGCHandle other = mono_gchandle_new_internal (nullptr, pinned);
		EXPECT_NE (empty, other) << "an empty handle's slot was handed out again";

		MonoObject *target = new_target (1);
		mono_gchandle_set_target (empty, target);
		EXPECT_EQ (target, dereference (empty));
		EXPECT_TRUE (mono_gchandle_is_in_domain_internal (empty, domain));

		mono_gchandle_set_target (empty, nullptr);
		EXPECT_EQ (nullptr, dereference (empty));
		EXPECT_TRUE (mono_gchandle_is_in_domain_internal (empty, domain));

		mono_gchandle_free_internal (empty);
		mono_gchandle_free_internal (other);
	}
}

TEST_F (SgenGCHandle, SurvivesTheTripThroughU32)
{
	MonoObject *target = new_target (1);
	std::vector<MonoGCHandle> handles;
	for (int i = 0; i < 1000; i++) {
		handles.push_back (mono_gchandle_new_internal (target, FALSE));
		handles.push_back (mono_gchandle_new_internal (target, TRUE));
		handles.push_back (mono_gchandle_new_weakref_internal (target, FALSE));
		handles.push_back (mono_gchandle_new_weakref_internal (target, TRUE));
	}

	std::unordered_set<guint32> distinct;
	for (MonoGCHandle handle : handles) {
		guint32 u32 = mono_gchandle_to_u32 (handle);
		ASSERT_NE (0u, u32);
		ASSERT_EQ (handle, mono_gchandle_from_u32 (u32));
		distinct.insert (u32);
	}
	EXPECT_EQ (handles.size (), distinct.size ());

	for (MonoGCHandle handle : handles)
		mono_gchandle_free_internal (handle);
}

TEST_F (SgenGCHandle, RefusesAValueThatIsNotAHandle)
{
	MonoGCHandle real = mono_gchandle_new_internal (new_target (1), FALSE);
	MonoGCHandle on_stack = &real;
	MonoGCHandle bogus [] = {
		nullptr, (MonoGCHandle) (gsize) 1, (MonoGCHandle) (gsize) 0x10, (MonoGCHandle) (gsize) 0x3e42437a3de2c368,
		on_stack, (char *) real + 2,
	};

	for (MonoGCHandle handle : bogus) {
		EXPECT_EQ (nullptr, mono_gchandle_get_target_internal (handle)) << handle;
		EXPECT_EQ (HANDLE_TYPE_MAX, mono_gchandle_get_type_internal (handle)) << handle;
		EXPECT_EQ (0u, mono_gchandle_to_u32 (handle)) << handle;
		EXPECT_FALSE (mono_gchandle_is_in_domain_internal (handle, mono_domain_get ())) << handle;
		mono_gchandle_set_target (handle, nullptr);
		mono_gchandle_free_internal (handle);
	}
	for (guint32 u32 : { 0u, 1u, 2u, 0xffffffffu, 0x7ffffff7u })
		EXPECT_EQ (nullptr, mono_gchandle_from_u32 (u32)) << u32;

	EXPECT_EQ (1, target_value (dereference (real)));
	mono_gchandle_free_internal (real);
}

struct VisitedTargets {
	MonoObject *const *wanted;
	int visits [2];
};

void visit_target (gpointer target, gpointer user_data)
{
	auto *visited = static_cast<VisitedTargets *> (user_data);
	for (int i = 0; i < 2; i++)
		visited->visits [i] += target == visited->wanted [i];
}

TEST_F (SgenGCHandle, ForeachVisitsStrongTargets)
{
	MonoObject *targets [] = { new_target (1), new_target (2) };
	MonoGCHandle handles [] = {
		mono_gchandle_new_internal (targets [0], FALSE),
		mono_gchandle_new_internal (targets [1], TRUE),
		mono_gchandle_new_internal (nullptr, FALSE),
		mono_gchandle_new_weakref_internal (targets [0], FALSE),
	};

	VisitedTargets visited = { targets, { 0, 0 } };
	mono_unity_stop_gc_world ();
	mono_unity_gc_handles_foreach_get_target (visit_target, &visited);
	mono_unity_start_gc_world ();

	EXPECT_EQ (1, visited.visits [0]);
	EXPECT_EQ (1, visited.visits [1]);
	for (MonoGCHandle handle : handles)
		mono_gchandle_free_internal (handle);
}

TEST_F (SgenGCHandle, ThreadsAllocateDistinctHandles)
{
	const int n_threads = 8;
	const int per_thread = 4096;
	MonoDomain *domain = mono_domain_get ();
	std::vector<std::vector<MonoGCHandle>> handles (n_threads);
	std::atomic<int> wrong { 0 };
	std::vector<std::thread> threads;

	for (int t = 0; t < n_threads; t++) {
		threads.emplace_back ([&, t] {
			MonoThread *thread = mono_thread_internal_attach (domain);
			for (int i = 0; i < per_thread; i++) {
				MonoGCHandle handle = mono_gchandle_new_internal (new_target (t * per_thread + i), i & 1);
				handles [t].push_back (handle);
				if (i % 3 == 0) {
					mono_gchandle_free_internal (handles [t] [i / 2]);
					handles [t] [i / 2] = mono_gchandle_new_internal (new_target (t * per_thread + i / 2), FALSE);
				}
			}
			for (int i = 0; i < per_thread; i++)
				wrong += target_value (dereference (handles [t] [i])) != t * per_thread + i;
			mono_thread_internal_detach (thread);
		});
	}
	for (std::thread &thread : threads)
		thread.join ();

	std::unordered_set<MonoGCHandle> distinct;
	for (const auto &list : handles)
		distinct.insert (list.begin (), list.end ());
	EXPECT_EQ (0, wrong.load ());
	EXPECT_EQ ((size_t) n_threads * per_thread, distinct.size ());

	for (const auto &list : handles)
		for (MonoGCHandle handle : list)
			mono_gchandle_free_internal (handle);
}

} // namespace
