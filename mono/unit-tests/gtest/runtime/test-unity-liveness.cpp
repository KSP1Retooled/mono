/*
 * test-unity-liveness.cpp: Unit test for the liveness walk Unity runs over
 * the managed heap with the world stopped.
 *
 * Each case checks that every object the walk reaches is reported once and
 * that no mark bit outlives mono_unity_liveness_finalize (). CMake
 * runs the suite once with one walker thread and once with several.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/object-internals.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/metadata/unity-utils.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_set>
#include <vector>

#include "harness.hpp"

namespace {

#define TESTPROG "unity-liveness.exe"

/* Runs with the world stopped, where growing the vector can deadlock on a heap
 * lock a suspended thread holds, so each case reserves room up front. */
void collect (gpointer *arr, int size, void *userdata)
{
	auto *found = static_cast<std::vector<MonoObject *> *> (userdata);
	if (found->size () + size > found->capacity ())
		abort ();
	found->insert (found->end (), (MonoObject **) arr, (MonoObject **) arr + size);
}

class UnityLiveness : public ::testing::Test {
protected:
	MonoImage *image = nullptr;
	MonoClass *tracked = nullptr;

	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();

		MonoDomain *domain = mono_domain_get ();
		MonoAssemblyOpenRequest req;
		mono_assembly_request_prepare_open (
			&req, MONO_ASMCTX_DEFAULT, mono_domain_default_alc (domain));

		MonoImageOpenStatus status;
		MonoAssembly *assembly = mono_assembly_request_open (TESTPROG, &req, &status);
		ASSERT_NE (nullptr, assembly) << "failed loading " TESTPROG;

		image = mono_assembly_get_image_internal (assembly);
		tracked = class_named ("Tracked");
		ASSERT_NE (nullptr, tracked);
	}

	void TearDown () override
	{
		if (image)
			invoke ("Drop", nullptr);
	}

	MonoClass *class_named (const char *name)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (image, "", name, error);
		mono_error_assert_ok (error);
		return klass;
	}

	MonoObject *invoke (const char *method_name, int *arg)
	{
		ERROR_DECL (error);
		MonoMethod *method = mono_class_get_method_from_name_checked (
			class_named ("LivenessRoots"), method_name, arg ? 1 : 0, 0, error);
		mono_error_assert_ok (error);
		void *args[] = { arg };
		MonoObject *result = mono_runtime_invoke_checked (method, nullptr, args, error);
		mono_error_assert_ok (error);
		return result;
	}

	int invoke_int (const char *method_name, int arg)
	{
		return *(int *) mono_object_unbox_internal (invoke (method_name, &arg));
	}

	void expect_each_tracked_once (const std::vector<MonoObject *> &found, size_t expected)
	{
		EXPECT_EQ (expected, found.size ());
		std::unordered_set<MonoObject *> distinct (found.begin (), found.end ());
		EXPECT_EQ (found.size (), distinct.size ()) << "an object was reported twice";
		for (MonoObject *object : found) {
			ASSERT_EQ (0u, (gsize) object->vtable & 1) << "mark bit left set";
			ASSERT_EQ (tracked, mono_object_class (object));
		}
	}

	LivenessState *begin_walk (std::vector<MonoObject *> &found)
	{
		LivenessState *state = mono_unity_liveness_allocate_struct (tracked, 0, collect, &found, nullptr);
		mono_unity_stop_gc_world ();
		return state;
	}

	void end_walk (LivenessState *state)
	{
		mono_unity_liveness_finalize (state);
		mono_unity_start_gc_world ();
		mono_unity_liveness_free_struct (state);
	}
};

TEST_F (UnityLiveness, FromStaticsReportsEachReachableObjectOnce)
{
	int expected = invoke_int ("Build", 200000);
	std::vector<MonoObject *> found;
	found.reserve (expected);

	LivenessState *state = begin_walk (found);
	mono_unity_liveness_calculation_from_statics (state);
	end_walk (state);

	expect_each_tracked_once (found, expected);
}

TEST_F (UnityLiveness, FromRootReportsOnlyWhatStaticsDidNot)
{
	int expected = invoke_int ("Build", 20000);
	int fresh = 1000;
	MonoObject *detached = invoke ("MakeDetached", &fresh);
	MonoGCHandle handle = mono_gchandle_new_internal (detached, FALSE);
	std::vector<MonoObject *> found;
	found.reserve (expected + fresh);

	LivenessState *state = begin_walk (found);
	mono_unity_liveness_calculation_from_statics (state);
	size_t from_statics = found.size ();
	mono_unity_liveness_calculation_from_root (detached, state);
	end_walk (state);
	mono_gchandle_free_internal (handle);

	EXPECT_EQ ((size_t) expected, from_statics);
	expect_each_tracked_once (found, expected + fresh);
}

/* Neither wait takes a lock, which a thread the stopped world suspended can hold. */
void wait_for (const std::atomic<int> &counter, int value)
{
	while (counter.load () < value)
		std::this_thread::yield ();
}

void meet (std::atomic<int> &counter, int parties)
{
	counter++;
	wait_for (counter, parties);
}

/* The walks Unity's asset collector runs: one state for each of these threads,
 * none of which is attached to the runtime. */
struct RootWalkers {
	static const int n_threads = 16;
	static const int per_root = 2000;

	std::vector<std::vector<MonoObject *>> found;
	std::vector<LivenessState *> states;
	std::vector<std::thread> threads;
	std::atomic<int> started { 0 }, walk { 0 }, walked { 0 }, finished { 0 };

	/* The finalize calls wait for `walked` to reach n_threads + extra_parties. */
	void start (MonoClass *filter, int extra_parties)
	{
		found.resize (n_threads);
		for (int i = 0; i < n_threads; i++) {
			found [i].reserve (per_root);
			states.push_back (mono_unity_liveness_allocate_struct (filter, 0, collect, &found [i], nullptr));
		}
		for (int i = 0; i < n_threads; i++) {
			threads.emplace_back ([this, i, extra_parties] {
				started++;
				wait_for (walk, 1);
				mono_unity_liveness_calculation_from_root (root_objects [i], states [i]);
				meet (walked, n_threads + extra_parties);
				mono_unity_liveness_finalize (states [i]);
				finished++;
			});
		}
		wait_for (started, n_threads);
	}

	void add_root (MonoObject *root)
	{
		root_objects.push_back (root);
		roots.push_back (mono_gchandle_new_internal (root, TRUE));
	}

	void stop ()
	{
		for (std::thread &thread : threads)
			thread.join ();
		for (LivenessState *state : states)
			mono_unity_liveness_free_struct (state);
		for (MonoGCHandle root : roots)
			mono_gchandle_free_internal (root);
	}

private:
	std::vector<MonoObject *> root_objects;
	std::vector<MonoGCHandle> roots;
};

TEST_F (UnityLiveness, ThreadsFinalizeConcurrently)
{
	for (int round = 0; round < 20; round++) {
		RootWalkers walkers;
		int count = RootWalkers::per_root;
		for (int i = 0; i < RootWalkers::n_threads; i++)
			walkers.add_root (invoke ("MakeIsolated", &count));
		walkers.start (tracked, 0);

		mono_unity_stop_gc_world ();
		walkers.walk = 1;
		wait_for (walkers.finished, RootWalkers::n_threads);
		mono_unity_start_gc_world ();
		walkers.stop ();

		for (const auto &found : walkers.found)
			expect_each_tracked_once (found, RootWalkers::per_root);
	}
}

TEST_F (UnityLiveness, FromStaticsWhileThreadsFinalize)
{
	int expected = invoke_int ("Build", 20000);

	for (int round = 0; round < 20; round++) {
		RootWalkers walkers;
		int count = RootWalkers::per_root;
		for (int i = 0; i < RootWalkers::n_threads; i++)
			walkers.add_root (invoke ("MakeIsolated", &count));
		walkers.start (tracked, 1);
		std::vector<MonoObject *> found;
		found.reserve (expected);

		LivenessState *state = begin_walk (found);
		walkers.walk = 1;
		meet (walkers.walked, RootWalkers::n_threads + 1);
		mono_unity_liveness_calculation_from_statics (state);
		mono_unity_liveness_finalize (state);
		wait_for (walkers.finished, RootWalkers::n_threads);
		mono_unity_start_gc_world ();
		mono_unity_liveness_free_struct (state);
		walkers.stop ();

		expect_each_tracked_once (found, expected);
		for (const auto &isolated : walkers.found)
			expect_each_tracked_once (isolated, RootWalkers::per_root);
	}
}

TEST_F (UnityLiveness, ManySmallObjects)
{
	int expected = invoke_int ("BuildVessels", 20000);
	std::vector<MonoObject *> found;
	found.reserve (expected);

	LivenessState *state = begin_walk (found);
	mono_unity_liveness_calculation_from_statics (state);
	end_walk (state);

	expect_each_tracked_once (found, expected);
}

/*
 * Not a test: times from_statics over LIVENESS_BENCH_PARTS vessels. Run it
 * with --gtest_also_run_disabled_tests and MONO_UNITY_LIVENESS_THREADS set to
 * each thread count to compare.
 */
TEST_F (UnityLiveness, DISABLED_Benchmark)
{
	const char *parts = getenv ("LIVENESS_BENCH_PARTS");
	invoke_int ("BuildVessels", parts ? atoi (parts) : 200000);
	invoke_int ("Build", parts ? atoi (parts) : 200000);

	for (int run = 0; run < 5; run++) {
		std::vector<MonoObject *> found;
		found.reserve (16 * 1024 * 1024);
		LivenessState *state = begin_walk (found);
		auto start = std::chrono::steady_clock::now ();
		mono_unity_liveness_calculation_from_statics (state);
		auto walked = std::chrono::steady_clock::now ();
		end_walk (state);
		auto ended = std::chrono::steady_clock::now ();
		printf ("from_statics %.2f ms, end %.2f ms, %zu objects\n",
			std::chrono::duration<double, std::milli> (walked - start).count (),
			std::chrono::duration<double, std::milli> (ended - walked).count (),
			found.size ());
	}
}

} // namespace
