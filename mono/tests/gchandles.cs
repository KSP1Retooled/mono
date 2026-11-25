using System;
using System.Threading;
using System.Runtime.InteropServices;

public class Test1
{
	public GCHandle self;
	public static bool fail;
	public static Test1 instance;

	~Test1 () {
		if (self.Target == null)
			fail = true;
	}
}

public class Test2
{
	public GCHandle self;
	public static bool fail;
	public static Test2 instance;

	~Test2 () {
		if (self.Target == null)
			fail = true;
	}
}

public class Tests
{
	public static int Main (String[] args) {
		return TestDriver.RunTests (typeof (Tests), args);
	}

	static void create1 () {
		Test1.instance = new Test1 ();
		Test1.instance.self = GCHandle.Alloc (Test1.instance, GCHandleType.WeakTrackResurrection);
		Test1.instance = null;
	}

	public static unsafe int test_0_track_resurrection () {
		/* The GCHandle should not be cleared before calling the finalizers */
		create1 ();
		GC.Collect ();
		GC.WaitForPendingFinalizers ();

		// WaitForPendingFinalizers doesn't seem to work ?
		System.Threading.Thread.Sleep (100);
		/* If the finalizers do not get ran by this time, the test will still succeed */
		return Test1.fail ? 1 : 0;
	}

	static void create2 () {
		Test2.instance = new Test2 ();
		object o = new object ();
		Test2.instance.self = GCHandle.Alloc (o, GCHandleType.WeakTrackResurrection);
		Test2.instance.self.Target = Test2.instance;
		Test2.instance = null;
	}

	public static int test_0_track_resurrection_set_target () {
		/* Same test but the handle target is set dynamically after it is created */
		create2 ();
		GC.Collect ();
		GC.WaitForPendingFinalizers ();

		System.Threading.Thread.Sleep (100);
		return Test2.fail ? 1 : 0;
	}

	const int HandleCount = 2000;

	static void fill_targets (GCHandle[] handles, int bias) {
		for (int i = 0; i < handles.Length; ++i)
			handles [i].Target = new int [] { i + bias };
	}

	static bool check_targets (GCHandle[] handles, int bias) {
		for (int i = 0; i < handles.Length; ++i) {
			var target = (int[]) handles [i].Target;
			if (target == null || target [0] != i + bias)
				return false;
		}
		return true;
	}

	public static int test_0_normal_handle_follows_target () {
		var handles = new GCHandle [HandleCount];
		for (int i = 0; i < handles.Length; ++i)
			handles [i] = GCHandle.Alloc (new int [] { i });

		GC.Collect (0);
		if (!check_targets (handles, 0))
			return 1;

		/* The setter writes the slot itself, without entering the runtime. */
		for (int round = 1; round < 20; ++round) {
			fill_targets (handles, round);
			GC.Collect (round % 5 == 0 ? GC.MaxGeneration : 0);
			if (!check_targets (handles, round))
				return 2;
		}

		foreach (var h in handles)
			h.Free ();
		return 0;
	}

	public static int test_0_normal_handle_null_target () {
		var h = GCHandle.Alloc (null);
		if (!h.IsAllocated || h.Target != null)
			return 1;

		var other = GCHandle.Alloc (new object ());
		if ((IntPtr) h == (IntPtr) other)
			return 2;

		/* The conversion throws if the handle is not in the current domain. */
		h = GCHandle.FromIntPtr (GCHandle.ToIntPtr (h));
		h.Target = "target";
		GC.Collect ();
		if ((string) h.Target != "target")
			return 3;

		h.Target = null;
		GC.Collect ();
		h = GCHandle.FromIntPtr (GCHandle.ToIntPtr (h));
		if (h.Target != null)
			return 4;

		h.Free ();
		other.Free ();
		return 0;
	}

	static GCHandle handle;
	static IntPtr original_addr;
	
	/*
	 * The target is an array because AddrOfPinnedObject () answers -1 for an
	 * auto-layout class, which System.Object is, and the C# side turns that
	 * into an ArgumentException (mono/metadata/gc.c).
	 */
	static void alloc_handle () {
		handle = GCHandle.Alloc (new byte [1], GCHandleType.Pinned);
		original_addr = handle.AddrOfPinnedObject ();
	}

	public static int test_0_pinned_handle_pins_target () {
		var t = new Thread (alloc_handle);
		t.Start ();
		t.Join ();
		GC.Collect ();

		var newAddr = handle.AddrOfPinnedObject ();

		return original_addr == newAddr ? 0 : 1;
	}
}