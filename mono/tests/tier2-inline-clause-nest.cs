using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Each case throws through a callee that has its own clauses, and tier 2 inlines
 * that callee inside a try in the caller. At tier 2, each case must run the same
 * handlers in the same order, and return the same answer, as it did before.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Program {
	static string log = "";
	static bool inlined;
	static int fails;

	static readonly MethodInfo address =
		typeof (StackFrame).GetMethod ("GetMethodAddress", BindingFlags.NonPublic | BindingFlags.Instance);

	// Whether the two frames above the caller run in the same compiled body.
	static bool CallersShareABody ()
	{
		StackTrace st = new StackTrace (2, false);

		return st.FrameCount >= 2
			&& address.Invoke (st.GetFrame (0), null).Equals (address.Invoke (st.GetFrame (1), null));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Log (string s) { log += s + ";"; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Throw (int k)
	{
		if (k != 0)
			inlined = CallersShareABody ();
		if (k == 1)
			throw new InvalidOperationException ("one");
		if (k == 2)
			throw new OverflowException ("two");
		if (k == 3)
			throw new ArgumentException ("three");
		return 10;
	}

	static int Catch (int k)
	{
		try {
			return Throw (k);
		} catch (OverflowException) {
			Log ("catch-ovf");
			return -1;
		}
	}

	static int Finally (int k)
	{
		try {
			return Throw (k);
		} finally {
			Log ("finally");
		}
	}

	static int FinallyInCatch (int k)
	{
		try {
			try {
				return Throw (k);
			} finally {
				Log ("finally");
			}
		} catch (OverflowException) {
			Log ("catch-ovf");
			return -1;
		}
	}

	static int Rethrow (int k)
	{
		try {
			return Throw (k);
		} catch (OverflowException) {
			Log ("rethrow");
			throw;
		} catch (InvalidOperationException e) {
			Log ("wrap");
			throw new TargetInvocationException (e);
		}
	}

	static int FinallyThrows (int k)
	{
		try {
			return Throw (k);
		} finally {
			Log ("finally");
			if (k == 1)
				throw new ArgumentException ("from finally");
		}
	}

	static int FinallyWrites (int k, ref int seen)
	{
		try {
			seen = 1;
			return Throw (k);
		} finally {
			seen += 100 + k;
		}
	}

	static int CatchInOuterCatch (int k)
	{
		try {
			return Catch (k);
		} catch (InvalidOperationException) {
			Log ("outer-iop");
			return -2;
		}
	}

	static int FinallyInOuterCatch (int k)
	{
		try {
			return Finally (k);
		} catch (InvalidOperationException) {
			Log ("outer-iop");
			return -2;
		}
	}

	static int CatchInTwoOuterCatches (int k)
	{
		try {
			return Catch (k);
		} catch (ArgumentException) {
			Log ("outer-arg");
			return -2;
		} catch (InvalidOperationException) {
			Log ("outer-iop");
			return -3;
		}
	}

	static int CatchInOuterFinally (int k)
	{
		try {
			return Catch (k);
		} finally {
			Log ("outer-finally");
		}
	}

	static int FinallyInOuterFinally (int k)
	{
		try {
			return Finally (k);
		} finally {
			Log ("outer-finally");
		}
	}

	static int FinallyInCatchInOuterFinallyInCatch (int k)
	{
		try {
			try {
				return FinallyInCatch (k);
			} finally {
				Log ("outer-finally");
			}
		} catch (InvalidOperationException) {
			Log ("outer-iop");
			return -2;
		}
	}

	static int RethrowInOuterCatches (int k)
	{
		try {
			return Rethrow (k);
		} catch (MethodAccessException) {
			Log ("outer-access");
			return -2;
		} catch (TargetInvocationException e) {
			Log ("outer-tie-" + e.InnerException.Message);
			return -3;
		} catch (OverflowException) {
			Log ("outer-ovf");
			return -4;
		}
	}

	static int FinallyThrowsInOuterCatch (int k)
	{
		try {
			return FinallyThrows (k);
		} catch (ArgumentException e) {
			Log ("outer-" + e.Message);
			return -2;
		}
	}

	static int FinallyWritesInOuterCatch (int k)
	{
		int seen = 0;

		try {
			return FinallyWrites (k, ref seen);
		} catch (InvalidOperationException) {
			return seen;
		}
	}

	// Two inlined copies of one body. After each copy's finally, the unwind goes to
	// the pad at that copy's own call site.
	static int Twice (int k)
	{
		int total = 0;

		try {
			total += FinallyInCatch (k == 2 ? 0 : k == 1 ? 2 : k);
			total += FinallyInCatch (k);
		} catch (InvalidOperationException) {
			Log ("outer-iop");
			total += -100;
		}
		return total;
	}

	static int CatchInOuterFilter (int k)
	{
		try {
			return Catch (k);
		} catch (Exception e) when (e is ArgumentException) {
			Log ("outer-filter");
			return -2;
		} catch (InvalidOperationException) {
			Log ("outer-iop");
			return -3;
		}
	}

	static int FinallyInOuterFilter (int k)
	{
		try {
			try {
				return Finally (k);
			} catch (Exception e) when (e is ArgumentException) {
				Log ("outer-filter");
				return -2;
			}
		} catch (Exception e) when (e is InvalidOperationException) {
			Log ("outermost-filter");
			return -3;
		}
	}

	static int Middle (int k)
	{
		try {
			return Catch (k);
		} catch (ArgumentException) {
			Log ("middle-arg");
			return -2;
		} finally {
			Log ("middle-finally");
		}
	}

	static int ThreeDeep (int k)
	{
		try {
			return Middle (k);
		} catch (InvalidOperationException) {
			Log ("outer-iop");
			return -3;
		}
	}

	static int RecurseFinally (int k, int depth)
	{
		try {
			return depth == 0 ? Throw (k) : RecurseFinally (k, depth - 1);
		} finally {
			Log ("recurse-" + depth);
		}
	}

	static int RecursiveInOuterCatch (int k)
	{
		try {
			return RecurseFinally (k, 3);
		} catch (InvalidOperationException) {
			Log ("outer-iop");
			return -2;
		}
	}

	static string Run (Func<int, int> f, int k)
	{
		string answer;

		log = "";
		try {
			answer = f (k).ToString ();
		} catch (Exception e) {
			answer = e.GetType ().Name;
		}
		return answer + " " + log;
	}

	static void Check (Func<int, int> f, bool expect_inline = true)
	{
		string name = f.Method.Name;
		var want = new List<string> ();

		for (int k = 0; k <= 3; ++k)
			want.Add (Run (f, k));

		for (int tier = 3; tier <= 4; ++tier) {
			if (!Mono.Tiering.MonoTier.PromoteNow (f.Method.MethodHandle.Value, tier)) {
				Console.WriteLine ("FAIL: {0} would not compile at tier {1}", name, tier - 2);
				++fails;
				return;
			}

			for (int k = 0; k <= 3; ++k) {
				inlined = false;

				string got = Run (f, k);

				if (got != want[k]) {
					Console.WriteLine ("FAIL: {0} ({1}) at tier {2}: got {3}, expected {4}",
						name, k, tier - 2, got, want[k]);
					++fails;
				}
				if (tier == 4 && k != 0 && expect_inline && !inlined) {
					Console.WriteLine ("FAIL: {0} inlined nothing with a clause", name);
					++fails;
					return;
				}
			}

			for (int i = 0; tier == 3 && i < 20000; ++i) {
				log = "";
				f (0);
			}
		}
	}

	public static int Main ()
	{
		bool inlining = Environment.GetEnvironmentVariable ("MONO_INLINE_CLAUSES") != "off";

		Check (CatchInOuterCatch, inlining);
		Check (FinallyInOuterCatch, inlining);
		Check (CatchInTwoOuterCatches, inlining);
		Check (CatchInOuterFinally, inlining);
		Check (FinallyInOuterFinally, inlining);
		Check (FinallyInCatchInOuterFinallyInCatch, inlining);
		Check (RethrowInOuterCatches, inlining);
		Check (FinallyThrowsInOuterCatch, inlining);
		Check (FinallyWritesInOuterCatch, inlining);
		Check (Twice, inlining);
		Check (CatchInOuterFilter, inlining);
		Check (FinallyInOuterFilter, inlining);
		Check (ThreeDeep, inlining);
		Check (RecursiveInOuterCatch, false);

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
