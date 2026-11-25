using System;
using System.Diagnostics;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

/*
 * Holds a dynamic method through objects other than its DynamicMethod, and collects. Each
 * of them has to keep the method callable, and the method has to die once it is dropped.
 */
class DynamicMethodInfoRoot
{
	/* WaitForPendingFinalizers returns at once unless a finalizer is pending. */
	class Trigger { ~Trigger () { } }

	struct W8 { public long a, b, c, d, e, f, g, h; }
	struct Block { public W8 a, b, c, d, e, f, g, h; }

	const int Count = 16;

	static WeakReference[] seen = new WeakReference [Count];
	static MethodBase frame_method;

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
			GC.Collect ();
			GC.WaitForPendingFinalizers ();
		}
	}

	public static void RecordCaller ()
	{
		frame_method = new StackFrame (1).GetMethod ();
	}

	static DynamicMethod Emit (string name, int result)
	{
		DynamicMethod dm = new DynamicMethod (name, typeof (int), new Type[] { typeof (int) },
		                                      typeof (DynamicMethodInfoRoot).Module, true);
		ILGenerator il = dm.GetILGenerator ();

		il.Emit (OpCodes.Call, typeof (DynamicMethodInfoRoot).GetMethod ("RecordCaller"));
		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Ldc_I4, result);
		il.Emit (OpCodes.Add);
		il.Emit (OpCodes.Ret);
		return dm;
	}

	/* Compiles more methods, so that released memory is written over. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Churn ()
	{
		for (int i = 0; i < 200; i++)
			((Func<int, int>) Emit ("info_root_churn", i).CreateDelegate (typeof (Func<int, int>))) (0);
		frame_method = null;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Func<int, int> Mint (int i, string name)
	{
		DynamicMethod dm = Emit (name, i);
		Func<int, int> d = (Func<int, int>) dm.CreateDelegate (typeof (Func<int, int>));

		seen [i] = new WeakReference (dm);
		d (0);
		return d;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static MethodInfo MintInfo (int i, string name)
	{
		return Mint (i, name).Method;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static MethodInfo MintInfoWithParameters (int i, string name)
	{
		MethodInfo info = Mint (i, name).Method;

		if (info.GetParameters ().Length != 1 || info.GetParameters () [0].Member != info)
			throw new Exception ("the parameters do not name their method");
		return info;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static MethodInfo MintFrame (int i, string name)
	{
		Mint (i, name);

		MethodInfo info = (MethodInfo) frame_method;

		frame_method = null;
		return info;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static MethodInfo MintInvoked (int i, string name)
	{
		DynamicMethod dm = Emit (name, i);

		seen [i] = new WeakReference (dm);
		dm.Invoke (null, new object[] { 0 });

		MethodInfo info = (MethodInfo) frame_method;

		frame_method = null;
		return info;
	}

	static int Alive ()
	{
		int alive = 0;

		foreach (WeakReference r in seen)
			if (r.IsAlive)
				alive++;
		return alive;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Held (Func<int, string, MethodInfo> mint, string name)
	{
		MethodInfo[] held = new MethodInfo [Count];

		for (int i = 0; i < Count; i++)
			held [i] = mint (i, name);

		frame_method = null;
		Collect ();
		if (Alive () != Count)
			return 1;

		Churn ();
		for (int i = 0; i < Count; i++)
			if ((int) held [i].Invoke (null, new object[] { 1000 }) != 1000 + i)
				return 2;

		/* A word that names the array must not keep all of them. */
		Array.Clear (held, 0, Count);
		return 0;
	}

	/* A word left on the stack can keep any one of them, so this asks for most. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Dropped ()
	{
		frame_method = null;
		Collect ();
		return Alive () < Count / 2 ? 0 : 3;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SameObject ()
	{
		Func<int, int> d = Mint (0, "info_root_same");
		MethodInfo from_frame = (MethodInfo) frame_method;

		Func<int, int> again = (Func<int, int>) d.Method.CreateDelegate (typeof (Func<int, int>));

		if (d.Method != from_frame || d.Method != again.Method)
			return 4;
		return again (5) == 5 ? 0 : 2;
	}

	static bool Check (string what, int code)
	{
		switch (code) {
		case 1: Console.WriteLine (what + ": a held method was collected"); break;
		case 2: Console.WriteLine (what + ": a held method did not run"); break;
		case 3: Console.WriteLine (what + ": dropped methods were not collected"); break;
		case 4: Console.WriteLine (what + ": one method has two MethodInfo objects"); break;
		}
		return code == 0;
	}

	static bool Run (string what, Func<int, string, MethodInfo> mint, string name)
	{
		return Check (what, Held (mint, name)) && Check (what, Dropped ());
	}

	public static int Main ()
	{
		bool ok = Check ("identity", SameObject ());

		ok = Run ("Delegate.Method", MintInfo, "info_root_method") && ok;
		ok = Run ("GetParameters", MintInfoWithParameters, "info_root_parameters") && ok;
		ok = Run ("StackFrame.GetMethod", MintFrame, "info_root_frame") && ok;
		ok = Run ("DynamicMethod.Invoke", MintInvoked, "info_root_invoke") && ok;
		return ok ? 0 : 1;
	}
}
