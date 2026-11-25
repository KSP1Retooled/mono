using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

/*
 * Takes function pointers for delegates over dynamic methods, calls them, and drops the
 * delegates. A pointer has to work while its delegate is alive, and nothing may keep the
 * methods once the delegates are gone.
 */
public class DynamicMethodFtnptr : MarshalByRefObject
{
	class Trigger { ~Trigger () { } }

	delegate int Add (int a, int b);
	delegate int Length (string s);
	[UnmanagedFunctionPointer (CallingConvention.Cdecl)]
	delegate long CdeclSum (Pair p, ref int count);

	[StructLayout (LayoutKind.Sequential)]
	public struct Pair { public long a, b; }

	delegate int CallAdd (IntPtr ftn, int a, int b);
	delegate int CallLength (IntPtr ftn, string s);
	delegate long CallSum (IntPtr ftn, Pair p, ref int count);

	static int failures;

	static void Expect (string what, long actual, long expected)
	{
		if (actual == expected)
			return;
		failures++;
		Console.WriteLine (what + ": got " + actual + ", expected " + expected);
	}

	static DynamicMethod Define (string name, Type ret, params Type[] args)
	{
		return new DynamicMethod (name, ret, args, typeof (DynamicMethodFtnptr).Module, true);
	}

	/* A method that calls its first argument as a native function. */
	static Delegate Caller (Type type, CallingConvention convention, Type ret, params Type[] args)
	{
		Type[] all = new Type [args.Length + 1];

		all [0] = typeof (IntPtr);
		Array.Copy (args, 0, all, 1, args.Length);

		DynamicMethod dm = Define ("caller", ret, all);
		ILGenerator il = dm.GetILGenerator ();

		for (int i = 0; i < args.Length; i++)
			il.Emit (OpCodes.Ldarg, (short) (i + 1));
		il.Emit (OpCodes.Ldarg_0);
		il.EmitCalli (OpCodes.Calli, convention, ret, args);
		il.Emit (OpCodes.Ret);
		return dm.CreateDelegate (type);
	}

	static CallAdd call_add;

	static DynamicMethod AddMethod (int extra, bool closed)
	{
		DynamicMethod dm = closed ? Define ("add", typeof (int), typeof (object), typeof (int), typeof (int))
		                          : Define ("add", typeof (int), typeof (int), typeof (int));
		ILGenerator il = dm.GetILGenerator ();
		int first = closed ? 1 : 0;

		il.Emit (OpCodes.Ldarg, (short) first);
		il.Emit (OpCodes.Ldarg, (short) (first + 1));
		il.Emit (OpCodes.Add);
		il.Emit (OpCodes.Ldc_I4, extra);
		il.Emit (OpCodes.Add);
		il.Emit (OpCodes.Ret);
		return dm;
	}

	static void Collect ()
	{
		for (int i = 0; i < 3; i++) {
			new Trigger ();
			GC.Collect ();
			GC.WaitForPendingFinalizers ();
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void One (int i, bool closed, List<WeakReference> dead)
	{
		DynamicMethod dm = AddMethod (i, closed);
		Add add = (Add) (closed ? dm.CreateDelegate (typeof (Add), "closed") : dm.CreateDelegate (typeof (Add)));
		IntPtr ftn = Marshal.GetFunctionPointerForDelegate (add);

		Expect ("pointer", call_add (ftn, 10, 20), 30 + i);
		Expect ("same pointer", (long) Marshal.GetFunctionPointerForDelegate (add), (long) ftn);
		Expect ("back", Marshal.GetDelegateForFunctionPointer (ftn, typeof (Add)) == (object) add ? 1 : 0, 1);
		GC.KeepAlive (add);
		dead.Add (new WeakReference (dm));
	}

	/* Two delegates over one method. Freeing one pointer must leave the other. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntPtr DropOne (DynamicMethod dm)
	{
		/* The overload with a target makes a delegate on each call. */
		return Marshal.GetFunctionPointerForDelegate ((Add) dm.CreateDelegate (typeof (Add), null));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Add CloneOne (int i, bool closed)
	{
		DynamicMethod dm = AddMethod (i, closed);
		Add add = (Add) (closed ? dm.CreateDelegate (typeof (Add), "closed") : dm.CreateDelegate (typeof (Add), null));

		Expect ("original pointer", call_add (Marshal.GetFunctionPointerForDelegate (add), 1, 2), 3 + i);

		Add clone = (Add) add.Clone ();

		GC.KeepAlive (add);
		return clone;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Cloned (int i)
	{
		Add[] clones = new Add[] { CloneOne (i, false), CloneOne (i, true) };

		Collect ();
		foreach (Add clone in clones)
			Expect ("clone pointer", call_add (Marshal.GetFunctionPointerForDelegate (clone), 3, 4), 7 + i);
		GC.KeepAlive (clones);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Shared (int i)
	{
		DynamicMethod dm = AddMethod (i, false);
		Add kept = (Add) dm.CreateDelegate (typeof (Add), null);
		IntPtr before = DropOne (dm);
		IntPtr ftn = Marshal.GetFunctionPointerForDelegate (kept);

		if (before == ftn) {
			failures++;
			Console.WriteLine ("two delegates have one pointer");
		}
		Collect ();
		Expect ("kept pointer", call_add (ftn, 1, 2), 3 + i);
		Expect ("kept back", Marshal.GetDelegateForFunctionPointer (ftn, typeof (Add)) == (object) kept ? 1 : 0, 1);
		GC.KeepAlive (kept);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Marshalled ()
	{
		DynamicMethod dm = Define ("length", typeof (int), typeof (string));
		ILGenerator il = dm.GetILGenerator ();

		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Callvirt, typeof (string).GetProperty ("Length").GetGetMethod ());
		il.Emit (OpCodes.Ret);

		Length length = (Length) dm.CreateDelegate (typeof (Length));
		CallLength call_length = (CallLength) Caller (typeof (CallLength), CallingConvention.Winapi, typeof (int), typeof (string));

		Expect ("string", call_length (Marshal.GetFunctionPointerForDelegate (length), "four"), 4);
		GC.KeepAlive (length);

		dm = Define ("sum", typeof (long), typeof (Pair), typeof (int).MakeByRefType ());
		il = dm.GetILGenerator ();
		il.Emit (OpCodes.Ldarg_1);
		il.Emit (OpCodes.Ldarg_1);
		il.Emit (OpCodes.Ldind_I4);
		il.Emit (OpCodes.Ldc_I4_1);
		il.Emit (OpCodes.Add);
		il.Emit (OpCodes.Stind_I4);
		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Ldfld, typeof (Pair).GetField ("a"));
		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Ldfld, typeof (Pair).GetField ("b"));
		il.Emit (OpCodes.Add);
		il.Emit (OpCodes.Ret);

		CdeclSum sum = (CdeclSum) dm.CreateDelegate (typeof (CdeclSum));
		CallSum call_sum = (CallSum) Caller (typeof (CallSum), CallingConvention.Cdecl, typeof (long), typeof (Pair),
		                                     typeof (int).MakeByRefType ());
		int count = 5;

		Expect ("struct", call_sum (Marshal.GetFunctionPointerForDelegate (sum), new Pair { a = 1L << 40, b = 2 }, ref count),
		        (1L << 40) + 2);
		Expect ("byref", count, 6);
		GC.KeepAlive (sum);
	}

	public int Run (int count)
	{
		List<WeakReference> open = new List<WeakReference> ();
		List<WeakReference> closed = new List<WeakReference> ();

		call_add = (CallAdd) Caller (typeof (CallAdd), CallingConvention.Winapi, typeof (int), typeof (int), typeof (int));

		for (int i = 0; i < count; i++) {
			One (i, false, open);
			One (i, true, closed);
			if (i % 10 == 0)
				Collect ();
		}
		for (int i = 0; i < 3; i++) {
			Shared (i);
			Cloned (i);
		}
		Marshalled ();
		Collect ();

		/* A stale stack word can keep a few, and must not be able to keep most. */
		ExpectDead ("open", open);
		ExpectDead ("closed", closed);
		return failures;
	}

	static void ExpectDead (string what, List<WeakReference> methods)
	{
		int alive = 0;

		foreach (WeakReference w in methods)
			if (w.IsAlive)
				alive++;
		if (alive > methods.Count / 4) {
			failures++;
			Console.WriteLine (what + ": " + alive + " of " + methods.Count + " methods are alive");
		}
	}

	public static int Main ()
	{
		int failed = new DynamicMethodFtnptr ().Run (40);

		for (int i = 0; i < 2; i++) {
			AppDomain domain = AppDomain.CreateDomain ("ftnptr" + i);
			DynamicMethodFtnptr there = (DynamicMethodFtnptr) domain.CreateInstanceAndUnwrap (
				typeof (DynamicMethodFtnptr).Assembly.FullName, typeof (DynamicMethodFtnptr).FullName);

			failed += there.Run (20);
			AppDomain.Unload (domain);
		}
		return failed == 0 ? 0 : 1;
	}
}
