using System;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * Runs dynamic methods that only an address on the stack refers to, and collects while
 * they run. A failure means the collector released code that a thread was executing.
 */
class DynamicMethodCodeRoot
{
	/* WaitForPendingFinalizers returns at once unless a finalizer is pending. */
	class Trigger { ~Trigger () { } }

	struct W8 { public long a, b, c, d, e, f, g, h; }
	struct Block { public W8 a, b, c, d, e, f, g, h; }

	static IntPtr slot;
	static WeakReference outer_seen, inner_seen;
	static bool armed, outer_alive, inner_alive;
	static int inner_runs;
	static ManualResetEvent parked, release;
	static Func<IntPtr, int> call;

	/* Clears the stack the minting ran on, where words that name the method are left. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static long Wipe ()
	{
		Block b0 = new Block (), b1 = new Block (), b2 = new Block (), b3 = new Block ();
		Block b4 = new Block (), b5 = new Block (), b6 = new Block (), b7 = new Block ();

		return b0.a.a + b1.b.b + b2.c.c + b3.d.d + b4.e.e + b5.f.f + b6.g.g + b7.h.h;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Collect ()
	{
		for (int i = 0; i < 5; i++) {
			Wipe ();
			new Trigger ();
			GC.Collect (i % 2);
			GC.Collect ();
			GC.WaitForPendingFinalizers ();
		}
	}

	public static void OuterBody ()
	{
		if (!armed)
			return;

		Collect ();
		outer_alive = outer_seen.IsAlive;
		inner_alive = inner_seen.IsAlive;
		Churn ();
	}

	public static void InnerBody ()
	{
		inner_runs++;
	}

	public static void ParkedBody ()
	{
		if (!armed)
			return;

		parked.Set ();
		release.WaitOne ();
	}

	static DynamicMethod Emit (string name, string body, DynamicMethod callee, int mark)
	{
		DynamicMethod dm = new DynamicMethod (name, typeof (int), Type.EmptyTypes,
		                                      typeof (DynamicMethodCodeRoot).Module, true);
		ILGenerator il = dm.GetILGenerator ();

		il.Emit (OpCodes.Call, typeof (DynamicMethodCodeRoot).GetMethod (body));
		if (callee != null) {
			il.Emit (OpCodes.Call, callee);
			il.Emit (OpCodes.Pop);
		}
		il.Emit (OpCodes.Ldc_I4, mark);
		il.Emit (OpCodes.Ret);
		return dm;
	}

	/* Compiles more methods, so that released memory is written over. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Churn ()
	{
		bool was = armed;

		armed = false;
		for (int i = 0; i < 100; i++) {
			DynamicMethod dm = Emit ("code_root_churn", "InnerBody", null, i);

			((Func<int>) dm.CreateDelegate (typeof (Func<int>))) ();
		}
		armed = was;
	}

	/* Leaves a compiled method's address in `slot` and no other reference to it. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Mint (string body)
	{
		DynamicMethod inner = Emit ("code_root_inner", "InnerBody", null, 4242);
		DynamicMethod outer = Emit ("code_root_outer", body, inner, 2121);
		Func<int> d = (Func<int>) outer.CreateDelegate (typeof (Func<int>));

		d ();
		outer_seen = new WeakReference (outer);
		inner_seen = new WeakReference (inner);
		slot = d.Method.MethodHandle.GetFunctionPointer ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntPtr Take ()
	{
		IntPtr p = slot;

		slot = IntPtr.Zero;
		return p;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Fire ()
	{
		return call (Take ());
	}

	/*
	 * Drops several pairs and checks that most of them die. A word left on the stack can
	 * keep any one of them.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool DroppedPairsDie (string body)
	{
		WeakReference[] dropped = new WeakReference [32];
		int alive = 0;

		for (int i = 0; i < dropped.Length; i += 2) {
			Mint (body);
			Take ();
			dropped [i] = outer_seen;
			dropped [i + 1] = inner_seen;
		}

		Collect ();
		foreach (WeakReference r in dropped)
			if (r.IsAlive)
				alive++;
		return alive < dropped.Length / 2;
	}

	static Func<IntPtr, int> BuildCall ()
	{
		AssemblyBuilder ab = AppDomain.CurrentDomain.DefineDynamicAssembly (
			new AssemblyName ("code_root_call"), AssemblyBuilderAccess.Run);
		TypeBuilder tb = ab.DefineDynamicModule ("m").DefineType ("Call", TypeAttributes.Public);
		MethodBuilder mb = tb.DefineMethod ("Through", MethodAttributes.Public | MethodAttributes.Static,
		                                    typeof (int), new Type[] { typeof (IntPtr) });
		ILGenerator il = mb.GetILGenerator ();

		il.Emit (OpCodes.Ldarg_0);
		il.EmitCalli (OpCodes.Calli, CallingConventions.Standard, typeof (int), Type.EmptyTypes, null);
		il.Emit (OpCodes.Ret);
		return (Func<IntPtr, int>) Delegate.CreateDelegate (
			typeof (Func<IntPtr, int>), tb.CreateType ().GetMethod ("Through"));
	}

	static int OwnThread ()
	{
		armed = false;
		if (!DroppedPairsDie ("OuterBody"))
			return 1;

		Mint ("OuterBody");
		armed = true;

		int runs = inner_runs;

		if (Fire () != 2121)
			return 2;
		if (!outer_alive)
			return 3;
		if (!inner_alive)
			return 4;
		if (inner_runs == runs)
			return 5;
		return 0;
	}

	static int OtherThread ()
	{
		int result = 0;

		armed = false;
		if (!DroppedPairsDie ("ParkedBody"))
			return 6;

		parked = new ManualResetEvent (false);
		release = new ManualResetEvent (false);
		Mint ("ParkedBody");
		armed = true;

		Thread t = new Thread (() => result = Fire ());

		t.Start ();
		parked.WaitOne ();
		Collect ();

		bool outer = outer_seen.IsAlive, inner = inner_seen.IsAlive;

		Churn ();
		release.Set ();
		t.Join ();
		if (!outer)
			return 7;
		if (!inner)
			return 8;
		if (result != 2121)
			return 9;
		return 0;
	}

	static string Explain (int code)
	{
		switch (code) {
		case 1: case 6: return "dropped dynamic methods were not collected";
		case 2: case 9: return "the body did not run to its end";
		case 3: case 7: return "a dynamic method was collected while its body was running";
		case 4: case 8: return "the callee of a running dynamic method was collected";
		case 5: return "the callee of a running dynamic method did not run";
		}
		return "unknown";
	}

	public static int Main ()
	{
		call = BuildCall ();

		int code = OwnThread ();

		if (code == 0)
			code = OtherThread ();
		if (code != 0)
			Console.WriteLine (Explain (code));
		return code;
	}
}
