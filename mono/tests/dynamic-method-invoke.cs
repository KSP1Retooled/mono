using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

/*
 * Invokes dynamic methods of many signatures through reflection, and drops them between
 * rounds. A failure means an invoke wrapper was used for a signature it does not fit, or
 * after something it names was gone.
 */
public class DynamicMethodInvoke : MarshalByRefObject
{
	class Trigger { ~Trigger () { } }

	public enum Small : byte { A = 1, B = 200 }
	public enum Wide : long { A = 1, B = 0x1234567890L }
	public struct Pair { public long a; public int b; }
	public struct Wrap<T> { public T value; }

	static int failures;

	static void Expect (string what, object actual, object expected)
	{
		if (Equals (actual, expected))
			return;
		failures++;
		Console.WriteLine (what + ": got " + (actual ?? "null") + ", expected " + (expected ?? "null"));
	}

	/* A method that returns its argument number `arg`. */
	static DynamicMethod Echo (int arg, Type ret, params Type[] args)
	{
		DynamicMethod dm = new DynamicMethod ("echo", ret, args, typeof (DynamicMethodInvoke).Module, true);
		ILGenerator il = dm.GetILGenerator ();

		il.Emit (OpCodes.Ldarg, (short) arg);
		il.Emit (OpCodes.Ret);
		return dm;
	}

	/* A method that stores `value` through its first argument. */
	static DynamicMethod Store (Type target, Type value)
	{
		DynamicMethod dm = new DynamicMethod ("store", typeof (void), new Type[] { target.MakeByRefType (), value },
		                                      typeof (DynamicMethodInvoke).Module, true);
		ILGenerator il = dm.GetILGenerator ();

		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Ldarg_1);
		il.Emit (OpCodes.Stobj, target);
		il.Emit (OpCodes.Ret);
		return dm;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void EchoOne (object value)
	{
		Type t = value.GetType ();

		Expect (t + " alone", Echo (0, t, t).Invoke (null, new object[] { value }), value);
		Expect (t + " second", Echo (1, t, typeof (string), t).Invoke (null, new object[] { "s", value }), value);
		Expect (t + " before", Echo (1, typeof (int), t, typeof (int)).Invoke (null, new object[] { value, 7 }), 7);
		Expect (t + " array", ((Array) Echo (0, t.MakeArrayType (), t.MakeArrayType ())
			.Invoke (null, new object[] { Array.CreateInstance (t, 3) })).Length, 3);

		object[] byref = new object[] { Activator.CreateInstance (t), value };

		Store (t, t).Invoke (null, byref);
		Expect (t + " byref", byref [0], value);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void EchoNullable<T> (T value) where T : struct
	{
		Expect ("nullable " + typeof (T), Echo (0, typeof (T?), typeof (T?)).Invoke (null, new object[] { value }), value);
		Expect ("null " + typeof (T), Echo (0, typeof (T?), typeof (T?)).Invoke (null, new object[] { null }), null);

		object[] byref = new object[] { null, value };

		Store (typeof (T?), typeof (T?)).Invoke (null, byref);
		Expect ("nullable byref " + typeof (T), byref [0], value);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Round ()
	{
		EchoOne ((sbyte) -5);
		EchoOne ((byte) 250);
		EchoOne ((short) -3000);
		EchoOne ((ushort) 60000);
		EchoOne (-70000);
		EchoOne (4000000000u);
		EchoOne (-5000000000L);
		EchoOne (10000000000000000000UL);
		EchoOne (1.5f);
		EchoOne (2.25);
		EchoOne (true);
		EchoOne ('x');
		EchoOne (new IntPtr (77));
		EchoOne (new DateTime (2020, 1, 2));
		EchoOne (12345.678m);
		EchoOne (Small.B);
		EchoOne (Wide.B);
		EchoOne (DayOfWeek.Friday);
		EchoOne (new Pair { a = 1L << 40, b = 3 });
		EchoOne (new Wrap<Pair> { value = new Pair { a = 9 } });
		EchoOne (new Wrap<string> { value = "w" });
		EchoOne (new KeyValuePair<Pair, int> (new Pair { b = 4 }, 5));

		EchoNullable (5);
		EchoNullable (Small.B);
		EchoNullable (new Pair { a = 6 });
		EchoNullable (new DateTime (2021, 3, 4));

		Expect ("string", Echo (0, typeof (string), typeof (string)).Invoke (null, new object[] { "text" }), "text");
		Expect ("object", Echo (0, typeof (object), typeof (object)).Invoke (null, new object[] { 8 }), 8);

		object[] text = new object[] { null, "stored" };

		Store (typeof (string), typeof (string)).Invoke (null, text);
		Expect ("string byref", text [0], "stored");

		DynamicMethod thrower = new DynamicMethod ("thrower", typeof (int), new Type[] { typeof (Pair) },
		                                           typeof (DynamicMethodInvoke).Module, true);
		ILGenerator il = thrower.GetILGenerator ();

		il.Emit (OpCodes.Ldstr, "thrown");
		il.Emit (OpCodes.Newobj, typeof (InvalidOperationException).GetConstructor (new Type[] { typeof (string) }));
		il.Emit (OpCodes.Throw);
		try {
			thrower.Invoke (null, new object[] { new Pair () });
			Expect ("throw", "returned", "threw");
		} catch (TargetInvocationException e) {
			Expect ("throw", e.InnerException.Message, "thrown");
		}
	}

	/*
	 * The signature takes structs of two assemblies, and one of them is emitted into this
	 * domain, so that both go when a domain does.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Emitted ()
	{
		AssemblyBuilder ab = AppDomain.CurrentDomain.DefineDynamicAssembly (
			new AssemblyName ("invoke_emitted"), AssemblyBuilderAccess.Run);
		TypeBuilder tb = ab.DefineDynamicModule ("m").DefineType (
			"Emitted", TypeAttributes.Public | TypeAttributes.SequentialLayout, typeof (ValueType));

		tb.DefineField ("x", typeof (long), FieldAttributes.Public);
		tb.DefineField ("y", typeof (long), FieldAttributes.Public);
		tb.DefineField ("z", typeof (long), FieldAttributes.Public);

		Type t = tb.CreateType ();
		object value = Activator.CreateInstance (t);

		t.GetField ("y").SetValue (value, 55L);

		object alone = Echo (0, t, t).Invoke (null, new object[] { value });
		object both = Echo (1, t, typeof (Pair), t).Invoke (null, new object[] { new Pair (), value });

		Expect ("emitted", t.GetField ("y").GetValue (alone), 55L);
		Expect ("emitted with another", t.GetField ("y").GetValue (both), 55L);
	}

	static void Collect ()
	{
		for (int i = 0; i < 3; i++) {
			new Trigger ();
			GC.Collect ();
			GC.WaitForPendingFinalizers ();
		}
	}

	public int Run (int rounds)
	{
		for (int i = 0; i < rounds; i++) {
			Round ();
			Emitted ();
			Collect ();
		}
		return failures;
	}

	public static int Main ()
	{
		int failed = new DynamicMethodInvoke ().Run (2);

		for (int i = 0; i < 2; i++) {
			AppDomain domain = AppDomain.CreateDomain ("invoke" + i);
			DynamicMethodInvoke there = (DynamicMethodInvoke) domain.CreateInstanceAndUnwrap (
				typeof (DynamicMethodInvoke).Assembly.FullName, typeof (DynamicMethodInvoke).FullName);

			failed += there.Run (1);
			AppDomain.Unload (domain);
			failed += new DynamicMethodInvoke ().Run (1);
		}
		return failed == 0 ? 0 : 1;
	}
}
