using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using Mono.Cecil;

sealed class ProfileContract
{
	public readonly List<int[]> PropertyBindings = new List<int[]> ();
	readonly Dictionary<int, string> entries = new Dictionary<int, string> ();

	public string Get (IMetadataTokenProvider item)
	{
		string value;
		return entries.TryGetValue (item.MetadataToken.ToInt32 (), out value) ? value : null;
	}

	void Set (IMetadataTokenProvider item, string action)
	{
		entries [item.MetadataToken.ToInt32 ()] = action;
	}

	static int Hex (string s)
	{
		return int.Parse (s, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
	}

	public static ProfileContract Read (string file, ModuleDefinition reference)
	{
		var lines = File.ReadAllLines (file);
		if (lines [0] != reference.Mvid.ToString ())
			throw new InvalidDataException ("Reference assembly mismatch for " + file);
		var result = new ProfileContract ();
		foreach (var line in lines.Skip (1)) {
			var words = line.Split (' ');
			if (words [0] == "bind") {
				result.PropertyBindings.Add (words.Skip (1).Select (Hex).ToArray ());
				continue;
			}
			foreach (var range in words.Skip (1)) {
				var ends = range.Split ('-');
				int start = Hex (ends [0]), end = Hex (ends.Last ());
				for (int token = start; token <= end; ++token)
					result.entries.Add (token, words [0]);
			}
		}
		return result;
	}

	void Write (string file, ModuleDefinition reference)
	{
		using (var writer = File.CreateText (file)) {
			writer.WriteLine (reference.Mvid);
			foreach (var binding in PropertyBindings)
				writer.WriteLine ("bind " + string.Join (" ", binding.Select (n => n.ToString ("x8"))));
			foreach (var group in entries.GroupBy (kv => kv.Value).OrderBy (g => g.Key, StringComparer.Ordinal)) {
				var tokens = group.Select (kv => kv.Key).OrderBy (n => n).ToArray ();
				for (int i = 0; i < tokens.Length;) {
					writer.Write (group.Key);
					for (int count = 0; i < tokens.Length && count < 8; ++count) {
						int start = tokens [i++];
						int end = start;
						while (i < tokens.Length && tokens [i] == end + 1)
							end = tokens [i++];
						writer.Write (" " + start.ToString ("x8"));
						if (end != start)
							writer.Write ("-" + end.ToString ("x8"));
					}
					writer.WriteLine ();
				}
			}
		}
	}

	static Dictionary<string, T> Index<T> (IEnumerable<T> items, Func<T,string> key)
	{
		return items.GroupBy (key).ToDictionary (g => g.Key, g => g.First ());
	}

	static string FieldKey (FieldDefinition f) { return f.Name + ":" + ProfileStubber.Key (f.FieldType); }
	static string PropertyKey (PropertyDefinition p)
	{
		return p.Name + "(" + string.Join (",", p.Parameters.Select (i => ProfileStubber.Key (i.ParameterType))) + "):" + ProfileStubber.Key (p.PropertyType);
	}

	static void AddMissing<T> (ProfileContract plan, IEnumerable<T> reference, IEnumerable<T> stock, IEnumerable<T> raw,
		Func<T,string> key, string action) where T : IMetadataTokenProvider
	{
		var missing = new HashSet<string> (stock.Select (key));
		missing.ExceptWith (raw.Select (key));
		foreach (var item in reference)
			if (missing.Contains (key (item)))
				plan.Set (item, action);
	}

	static bool SameAccessor (MethodDefinition a, MethodDefinition b)
	{
		return a == null || b == null ? a == b : ProfileStubber.Key (a) == ProfileStubber.Key (b);
	}

	static bool NoOp (MethodDefinition m)
	{
		return m.HasBody && m.Body.Instructions.Count == 1 && m.Body.Instructions [0].OpCode == Mono.Cecil.Cil.OpCodes.Ret;
	}

	static bool Stub (MethodDefinition m)
	{
		return !m.HasBody || NoOp (m) || m.Body.Instructions.Any (i => i.Operand is MethodReference &&
			((MethodReference)i.Operand).DeclaringType.FullName == "Unity.ThrowStub");
	}

	static bool DifferentParameters (MethodDefinition a, MethodDefinition b)
	{
		return !a.Parameters.Select (p => p.Attributes).SequenceEqual (b.Parameters.Select (p => p.Attributes)) ||
			!a.Parameters.Select (p => p.Constant).SequenceEqual (b.Parameters.Select (p => p.Constant));
	}

	static void RecordType (ProfileContract plan, TypeDefinition reference, TypeDefinition stock, TypeDefinition raw)
	{
		if (raw == null || stock.Attributes != raw.Attributes)
			plan.Set (reference, "type:" + (uint)stock.Attributes);
		Func<InterfaceImplementation,string> iface = i => ProfileStubber.Key (i.InterfaceType);
		AddMissing (plan, reference.Interfaces, stock.Interfaces, raw == null ? Enumerable.Empty<InterfaceImplementation> () : raw.Interfaces, iface, "interface");
		AddMissing (plan, reference.Fields, stock.Fields, raw == null ? Enumerable.Empty<FieldDefinition> () : raw.Fields, FieldKey, "field");
		var originals = Index (raw == null ? Enumerable.Empty<PropertyDefinition> () : raw.Properties, PropertyKey);
		var accessors = Index (reference.Methods, ProfileStubber.Key);
		foreach (var prop in stock.Properties) {
			PropertyDefinition orig;
			originals.TryGetValue (PropertyKey (prop), out orig);
			if (orig != null && SameAccessor (orig.GetMethod, prop.GetMethod) && SameAccessor (orig.SetMethod, prop.SetMethod))
				continue;
			var rp = reference.Properties.FirstOrDefault (x => x.Name == prop.Name &&
				((prop.GetMethod != null && SameAccessor (prop.GetMethod, x.GetMethod)) ||
				 (prop.SetMethod != null && SameAccessor (prop.SetMethod, x.SetMethod))));
			if (rp == null)
				continue;
			if (PropertyKey (rp) == PropertyKey (prop) && SameAccessor (prop.GetMethod, rp.GetMethod) && SameAccessor (prop.SetMethod, rp.SetMethod)) {
				plan.Set (rp, "property");
				continue;
			}
			int index = -1;
			if (ProfileStubber.Key (rp.PropertyType) != ProfileStubber.Key (prop.PropertyType))
				index = prop.SetMethod.Parameters.First (p => ProfileStubber.Key (p.ParameterType) == ProfileStubber.Key (prop.PropertyType)).Index;
			plan.PropertyBindings.Add (new [] { rp.MetadataToken.ToInt32 (),
				prop.GetMethod == null ? 0 : accessors [ProfileStubber.Key (prop.GetMethod)].MetadataToken.ToInt32 (),
				prop.SetMethod == null ? 0 : accessors [ProfileStubber.Key (prop.SetMethod)].MetadataToken.ToInt32 (), index });
		}
		AddMissing (plan, reference.Events, stock.Events, raw == null ? Enumerable.Empty<EventDefinition> () : raw.Events, x => x.Name, "event");

		var editorMethods = Index (stock.Methods, ProfileStubber.Key);
		var compiled = Index (raw == null ? Enumerable.Empty<MethodDefinition> () : raw.Methods, ProfileStubber.Key);
		foreach (var method in reference.Methods) {
			MethodDefinition expected, original;
			if (!editorMethods.TryGetValue (ProfileStubber.Key (method), out expected))
				continue;
			var present = compiled.TryGetValue (ProfileStubber.Key (method), out original);
			if (present && original.Attributes == expected.Attributes && !DifferentParameters (original, expected))
				continue;
			if (!present && !Stub (expected))
				throw new InvalidOperationException ("Missing implementation: " + expected.FullName);
			var action = !present && NoOp (expected) ? "return" : "method";
			plan.Set (method, action + ":" + (uint)expected.Attributes);
		}
	}

	public static void Record (string references, string editor, string compiled, string directory)
	{
		Directory.CreateDirectory (directory);
		foreach (var path in Directory.GetFiles (references, "*.dll")) {
			string file = Path.GetFileName (path);
			if (!File.Exists (Path.Combine (editor, file)) || !File.Exists (Path.Combine (compiled, file)))
				continue;
			using (var reference = ModuleDefinition.ReadModule (path))
			using (var stock = ModuleDefinition.ReadModule (Path.Combine (editor, file)))
			using (var raw = ModuleDefinition.ReadModule (Path.Combine (compiled, file))) {
				var expected = Index (stock.GetTypes (), t => t.FullName);
				var original = Index (raw.GetTypes (), t => t.FullName);
				var plan = new ProfileContract ();
				foreach (var t in reference.GetTypes ().Where (ProfileStubber.Visible)) {
					TypeDefinition found, impl;
					if (!expected.TryGetValue (t.FullName, out found) || !ProfileStubber.Visible (found))
						continue;
					original.TryGetValue (t.FullName, out impl);
					RecordType (plan, t, found, impl);
				}
				if (plan.entries.Count > 0 || plan.PropertyBindings.Count > 0)
					plan.Write (Path.Combine (directory, file + ".contract"), reference);
			}
		}
	}
}
