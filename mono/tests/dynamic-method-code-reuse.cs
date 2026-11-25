using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

/*
 * Threads make dynamic methods, call them in several ways and drop them, while others
 * collect. Freed code is used again for the methods that follow, so a call that outlives
 * its method's code lands in another method and returns that one's number.
 */
public class DynamicMethodCodeReuse
{
	class Trigger { ~Trigger () { } }

	delegate int Number (int x);
	delegate int CallManaged (IntPtr ftn, int x);

	const int thread_count = 4;
	const int rounds = 40;

	static int failures;
	static volatile bool done;
	static CallManaged call_managed, call_native;

	static readonly Type[] ints = { typeof (int) };

	public static int Same (int x) { return x; }

	/* What a generated method calls in its middle, to be collected under. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Collecting (int x)
	{
		new Trigger ();
		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		return x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Throwing (int x)
	{
		throw new InvalidOperationException (x.ToString ());
	}

	static void Expect (string what, int actual, int expected)
	{
		if (actual == expected)
			return;
		Interlocked.Increment (ref failures);
		Console.WriteLine (what + ": got " + actual + ", expected " + expected);
	}

	static Delegate Caller (bool native)
	{
		DynamicMethod dm = new DynamicMethod ("caller", typeof (int), new Type[] { typeof (IntPtr), typeof (int) },
		                                      typeof (DynamicMethodCodeReuse).Module, true);
		ILGenerator il = dm.GetILGenerator ();

		il.Emit (OpCodes.Ldarg_1);
		il.Emit (OpCodes.Ldarg_0);
		if (native)
			il.EmitCalli (OpCodes.Calli, CallingConvention.Winapi, typeof (int), ints);
		else
			il.EmitCalli (OpCodes.Calli, CallingConventions.Standard, typeof (int), ints, null);
		il.Emit (OpCodes.Ret);
		return dm.CreateDelegate (typeof (CallManaged));
	}

	/* A method that returns its argument plus `id`. `middle` is what it calls on the way. */
	static DynamicMethod Make (int id, string middle, int padding)
	{
		DynamicMethod dm = new DynamicMethod ("number", typeof (int), ints, typeof (DynamicMethodCodeReuse).Module, true);
		ILGenerator il = dm.GetILGenerator ();
		MethodInfo same = typeof (DynamicMethodCodeReuse).GetMethod ("Same");

		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Call, typeof (DynamicMethodCodeReuse).GetMethod (middle));
		/* Methods of different sizes, so that they do not all fit in one another's place. */
		for (int i = 0; i < padding; i++) {
			il.Emit (OpCodes.Call, same);
			il.Emit (OpCodes.Ldc_I4, i);
			il.Emit (OpCodes.Xor);
			il.Emit (OpCodes.Ldc_I4, i);
			il.Emit (OpCodes.Xor);
		}
		il.Emit (OpCodes.Ldc_I4, id);
		il.Emit (OpCodes.Add);
		il.Emit (OpCodes.Ret);
		return dm;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ThroughDelegate (int id, int padding)
	{
		Number n = (Number) Make (id, "Same", padding).CreateDelegate (typeof (Number));

		for (int i = 0; i < 3; i++)
			Expect ("delegate", n (i), i + id);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntPtr Address (int id, int padding)
	{
		Number n = (Number) Make (id, "Collecting", padding).CreateDelegate (typeof (Number));

		return n.Method.MethodHandle.GetFunctionPointer ();
	}

	/* The address is all that is left of the method. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ThroughAddress (int id, int padding)
	{
		IntPtr ftn = Address (id, padding);

		if (RootsCode)
			Expect ("address", call_managed (ftn, 5), 5 + id);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ThroughNative (int id, int padding)
	{
		Number n = (Number) Make (id, "Collecting", padding).CreateDelegate (typeof (Number));

		Expect ("native", call_native (Marshal.GetFunctionPointerForDelegate (n), 6), 6 + id);
		GC.KeepAlive (n);
	}

	[ThreadStatic] static Number only_reference;

	/* Drops the delegate native code is calling through, from inside the call. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Dropping (int x)
	{
		only_reference = null;
		return Collecting (x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntPtr NativeAddress (int id, int padding)
	{
		only_reference = (Number) Make (id, "Dropping", padding).CreateDelegate (typeof (Number));
		return Marshal.GetFunctionPointerForDelegate (only_reference);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ThroughNativeDropped (int id, int padding)
	{
		IntPtr ftn = NativeAddress (id, padding);

		if (RootsCode)
			Expect ("native dropped", call_native (ftn, 8), 8 + id);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ThroughThrow (int id, int padding)
	{
		Number n = (Number) Make (id, "Throwing", padding).CreateDelegate (typeof (Number));

		try {
			n (id);
			Expect ("throw", 0, id);
		} catch (InvalidOperationException e) {
			Expect ("throw", int.Parse (e.Message), id);
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ThroughInvoke (int id, int padding)
	{
		Expect ("invoke", (int) Make (id, "Collecting", padding).Invoke (null, new object[] { 7 }), 7 + id);
	}

	/* Boehm does not count a code address as a reference. It has one generation. */
	static readonly bool RootsCode = GC.MaxGeneration > 0;

	static void Work (object start)
	{
		int id = (int) start * 1000000;

		for (int i = 0; i < rounds; i++) {
			int padding = (i * 7 + (int) start * 3) % 23;

			ThroughDelegate (id++, padding);
			ThroughAddress (id++, padding);
			ThroughNative (id++, padding);
			ThroughNativeDropped (id++, padding);
			ThroughThrow (id++, padding);
			ThroughInvoke (id++, padding);
		}
	}

	static void Collector ()
	{
		while (!done) {
			new Trigger ();
			GC.Collect ();
			GC.WaitForPendingFinalizers ();
			Thread.Sleep (1);
		}
	}

	public static int Main ()
	{
		call_managed = (CallManaged) Caller (false);
		call_native = (CallManaged) Caller (true);

		Thread collector = new Thread (Collector);
		Thread[] threads = new Thread [thread_count];

		collector.Start ();
		for (int i = 0; i < thread_count; i++) {
			threads [i] = new Thread (Work);
			threads [i].Start (i + 1);
		}
		foreach (Thread t in threads)
			t.Join ();
		done = true;
		collector.Join ();
		return failures == 0 ? 0 : 1;
	}
}
