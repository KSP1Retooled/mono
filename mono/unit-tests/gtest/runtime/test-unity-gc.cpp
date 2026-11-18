#include "config.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/class.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/handle.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/unity-utils.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

class UnityGC : public ::testing::Test {
protected:
	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();
		started = true;
	}

	void TearDown () override
	{
		if (started)
			mono_unity_gc_set_mode (MONO_GC_MODE_ENABLED);
	}

private:
	bool started = false;
};

MonoArray *allocate (MonoMethod *method, int count, int size)
{
	void *args[] = { &count, &size };
	ERROR_DECL (error);
	auto *result = (MonoArray *)mono_runtime_invoke_checked (method, nullptr, args, error);
	mono_error_assert_ok (error);
	EXPECT_NE (nullptr, result);
	if (result)
		EXPECT_EQ ((guint8)(count - 1), mono_array_get (result, guint8, size - 1));
	return result;
}

TEST_F (UnityGC, Modes)
{
	MonoDomain *domain = mono_domain_get ();
	MonoAssemblyOpenRequest request;
	mono_assembly_request_prepare_open (&request, MONO_ASMCTX_DEFAULT, mono_domain_default_alc (domain));
	MonoAssembly *assembly = mono_assembly_request_open ("unity-gc.exe", &request, nullptr);
	ASSERT_NE (nullptr, assembly);
	ERROR_DECL (error);
	MonoClass *host = mono_class_from_name_checked (mono_assembly_get_image_internal (assembly), "", "UnityGCHost", error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, host);
	MonoMethod *alloc = mono_class_get_method_from_name_checked (host, "Allocate", 2, 0, error);
	mono_error_assert_ok (error);
	MonoMethod *collect = mono_class_get_method_from_name_checked (host, "Collect", 0, 0, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, alloc);
	ASSERT_NE (nullptr, collect);
	ASSERT_NE (nullptr, allocate (alloc, 1, 16));

	int oldest = mono_gc_max_generation ();
	mono_unity_gc_set_mode (MONO_GC_MODE_DISABLED);
	EXPECT_TRUE (mono_unity_gc_is_disabled ());
	int minor_before = mono_gc_collection_count (0);
	int major_before = mono_gc_collection_count (oldest);
	mono_runtime_invoke_checked (collect, nullptr, nullptr, error);
	mono_error_assert_ok (error);
	mono_gc_collect (oldest);
	MonoArray *last = allocate (alloc, 4096, 4096);
	ASSERT_NE (nullptr, last);
	EXPECT_EQ (oldest, mono_gc_get_generation ((MonoObject *)last));
	ASSERT_NE (nullptr, allocate (alloc, 32, 128 * 1024));
	EXPECT_EQ (minor_before, mono_gc_collection_count (0));
	EXPECT_EQ (major_before, mono_gc_collection_count (oldest));

	mono_unity_gc_set_mode (MONO_GC_MODE_MANUAL);
	EXPECT_FALSE (mono_unity_gc_is_disabled ());
	ASSERT_NE (nullptr, allocate (alloc, 4096, 4096));
	EXPECT_EQ (minor_before, mono_gc_collection_count (0));
	EXPECT_EQ (major_before, mono_gc_collection_count (oldest));
	mono_runtime_invoke_checked (collect, nullptr, nullptr, error);
	mono_error_assert_ok (error);
	EXPECT_GT (mono_gc_collection_count (oldest), major_before);

	mono_unity_gc_set_mode (MONO_GC_MODE_ENABLED);
	minor_before = mono_gc_collection_count (0);
	ASSERT_NE (nullptr, allocate (alloc, 16384, 4096));
	EXPECT_GT (mono_gc_collection_count (0), minor_before);
}

TEST_F (UnityGC, DeprecatedDisableNests)
{
	mono_unity_gc_set_mode (MONO_GC_MODE_MANUAL);
	mono_unity_gc_disable ();
	mono_unity_gc_disable ();
	EXPECT_TRUE (mono_unity_gc_is_disabled ());
	mono_unity_gc_enable ();
	EXPECT_TRUE (mono_unity_gc_is_disabled ());
	mono_unity_gc_enable ();
	EXPECT_FALSE (mono_unity_gc_is_disabled ());
	int generation = mono_gc_max_generation ();
	int before = mono_gc_collection_count (generation);
	mono_gc_collect (generation);
	EXPECT_GT (mono_gc_collection_count (generation), before);
}

struct HeapChunk {
	void *start;
	size_t size;
};

struct Snapshot {
	MonoObject *targets[4];
	bool covered[4] = {};
	unsigned handles[4] = {};
};

void report_chunk (gpointer chunk_data, gpointer user_data)
{
	HeapChunk chunk;
	memcpy (&chunk, chunk_data, sizeof (chunk));
	auto &snapshot = *(Snapshot *)user_data;
	for (size_t i = 0; i < G_N_ELEMENTS (snapshot.targets); ++i) {
		if ((uintptr_t)snapshot.targets[i] - (uintptr_t)chunk.start < chunk.size)
			snapshot.covered[i] = true;
	}
}

void report_target (gpointer target, gpointer user_data)
{
	auto &snapshot = *(Snapshot *)user_data;
	for (size_t i = 0; i < G_N_ELEMENTS (snapshot.targets); ++i) {
		if (snapshot.targets[i] == target)
			++snapshot.handles[i];
	}
}

TEST_F (UnityGC, Snapshot)
{
	MonoDomain *domain = mono_domain_get ();
	MonoClass *byte_class = mono_get_byte_class ();
	mono_unity_gc_set_mode (MONO_GC_MODE_MANUAL);
	mono_gc_collect (mono_gc_max_generation ());
	ERROR_DECL (error);
	MonoObject *major = mono_object_new_pinned (domain, mono_get_object_class (), error);
	mono_error_assert_ok (error);
	MonoObject *small = (MonoObject *)mono_array_new_checked (domain, byte_class, 16, error);
	mono_error_assert_ok (error);
	MonoObject *large = (MonoObject *)mono_array_new_checked (domain, byte_class, 128 * 1024, error);
	mono_error_assert_ok (error);
	MonoObject *weak = (MonoObject *)mono_array_new_checked (domain, byte_class, 16, error);
	mono_error_assert_ok (error);
	Snapshot snapshot {{ small, major, large, weak }};
	for (auto *target : snapshot.targets) {
		ASSERT_NE (nullptr, target);
		EXPECT_TRUE (mono_unity_gc_is_heap_ptr (target));
		EXPECT_TRUE (mono_unity_gc_is_heap_ptr ((char *)target + 1));
	}
	EXPECT_FALSE (mono_unity_gc_is_heap_ptr (&snapshot));
	EXPECT_EQ (0, mono_gc_get_generation (small));
	EXPECT_EQ (mono_gc_max_generation (), mono_gc_get_generation (major));
	EXPECT_EQ (mono_gc_max_generation (), mono_gc_get_generation (large));
	MonoGCHandle handles[] = {
		mono_gchandle_new_internal (small, FALSE),
		mono_gchandle_new_internal (major, TRUE),
		mono_gchandle_new_internal (large, FALSE),
		mono_gchandle_new_weakref_internal (weak, FALSE),
	};

	mono_unity_stop_gc_world ();
	mono_unity_gc_heap_foreach (report_chunk, &snapshot);
	mono_unity_gc_handles_foreach_get_target (report_target, &snapshot);
	mono_unity_start_gc_world ();

	for (size_t i = 0; i < G_N_ELEMENTS (handles); ++i) {
		EXPECT_TRUE (snapshot.covered[i]) << i;
		EXPECT_EQ (i < 3 ? 1u : 0u, snapshot.handles[i]) << i;
		mono_gchandle_free_internal (handles[i]);
	}
}

} // namespace
