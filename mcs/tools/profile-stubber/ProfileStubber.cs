using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

sealed class ProfileStubber
{
	sealed class Importer : DefaultMetadataImporter
	{
		AssemblyNameReference local;

		public Importer (ModuleDefinition module) : base (module) {}

		protected override IMetadataScope ImportScope (TypeReference type)
		{
			var owner = type.Scope as ModuleDefinition;
			if (owner != null && owner.Assembly.Name.FullName == module.Assembly.Name.FullName) {
				if (local == null)
					local = new AssemblyNameReference (module.Assembly.Name.Name, module.Assembly.Name.Version);
				return local;
			}
			return base.ImportScope (type);
		}

		// Cecil recognizes CLI primitives in an assembly scope. Localize after import so Void keeps ELEMENT_TYPE_VOID.
		void Localize (TypeReference type)
		{
			if (type is GenericParameter)
				return;
			var spec = type as TypeSpecification;
			if (spec != null) {
				Localize (spec.ElementType);
				var generic = spec as GenericInstanceType;
				if (generic != null)
					foreach (var argument in generic.GenericArguments)
						Localize (argument);
				var modifier = spec as IModifierType;
				if (modifier != null)
					Localize (modifier.ModifierType);
			} else if (type.IsNested) {
				Localize (type.DeclaringType);
			} else if (type.Scope == local) {
				type.Scope = module;
			}
		}

		public override TypeReference ImportReference (TypeReference type, IGenericParameterProvider context)
		{
			var result = base.ImportReference (type, context);
			Localize (result);
			return result;
		}

		public override MethodReference ImportReference (MethodReference method, IGenericParameterProvider context)
		{
			var result = base.ImportReference (method, context);
			Localize (result.DeclaringType);
			Localize (result.ReturnType);
			foreach (var parameter in result.Parameters)
				Localize (parameter.ParameterType);
			return result;
		}
	}

	sealed class ImporterProvider : IMetadataImporterProvider
	{
		public IMetadataImporter GetMetadataImporter (ModuleDefinition destination) { return new Importer (destination); }
	}

	readonly ProfileContract contract;
	readonly ModuleDefinition output;
	readonly Dictionary<string, TypeDefinition> types;
	readonly Dictionary<MethodDefinition, MethodDefinition> methods = new Dictionary<MethodDefinition, MethodDefinition> ();
	readonly HashSet<TypeDefinition> added = new HashSet<TypeDefinition> ();
	readonly MethodReference unsupported;
	bool changed;

	ProfileStubber (ModuleDefinition output, ProfileContract contract)
	{
		this.output = output;
		this.contract = contract;
		types = output.GetTypes ().ToDictionary (t => t.FullName);
		var exception = new TypeReference ("System", "PlatformNotSupportedException", output, output.TypeSystem.CoreLibrary);
		unsupported = new MethodReference (".ctor", output.TypeSystem.Void, exception) { HasThis = true };
	}

	internal static bool Visible (TypeDefinition t)
	{
		return t.IsPublic || (t.IsNested && (t.IsNestedPublic || t.IsNestedFamily || t.IsNestedFamilyOrAssembly) && Visible (t.DeclaringType));
	}


	internal static string Key (TypeReference t)
	{
		var p = t as GenericParameter;
		if (p != null)
			return (p.Type == GenericParameterType.Method ? "!!" : "!") + p.Position;
		var g = t as GenericInstanceType;
		if (g != null)
			return g.ElementType.FullName + "<" + string.Join (",", g.GenericArguments.Select (Key)) + ">";
		var spec = t as TypeSpecification;
		if (spec != null)
			return Key (spec.ElementType) + t.FullName.Substring (spec.ElementType.FullName.Length);
		return t.FullName;
	}

	internal static string Key (MethodReference m)
	{
		return m.Name + "`" + m.GenericParameters.Count + "(" + string.Join (",", m.Parameters.Select (p => Key (p.ParameterType))) + "):" + Key (m.ReturnType);
	}

	void Attributes (ICustomAttributeProvider source, ICustomAttributeProvider target)
	{
		foreach (var a in source.CustomAttributes)
			target.CustomAttributes.Add (new CustomAttribute (output.ImportReference (a.Constructor), a.GetBlob ()));
	}

	static void AddGenerics (IGenericParameterProvider source, IGenericParameterProvider target)
	{
		foreach (var gp in source.GenericParameters)
			target.GenericParameters.Add (new GenericParameter (gp.Name, target));
	}


	void Declare (TypeDefinition source)
	{
		var action = contract.Get (source);
		if (action == null)
			return;
		var flags = (TypeAttributes)uint.Parse (action.Split (':') [1]);
		TypeDefinition type;
		if (types.TryGetValue (source.FullName, out type)) {
			type.Attributes = flags;
			changed = true;
			return;
		}
		type = new TypeDefinition (source.Namespace, source.Name, flags);
		if (source.IsNested)
			types [source.DeclaringType.FullName].NestedTypes.Add (type);
		else
			output.Types.Add (type);
		types.Add (source.FullName, type);
		added.Add (type);
		AddGenerics (source, type);
		changed = true;
	}

	void Field (TypeDefinition type, FieldDefinition f)
	{
		if (type.Fields.Any (other => other.Name == f.Name && Key (other.FieldType) == Key (f.FieldType)))
			return;
		var field = new FieldDefinition (f.Name, f.Attributes, output.ImportReference (f.FieldType, type));
		if (f.HasConstant)
			field.Constant = f.Constant;
		if (f.HasLayoutInfo)
			field.Offset = f.Offset;
		if (f.HasMarshalInfo)
			field.MarshalInfo = f.MarshalInfo;
		Attributes (f, field);
		type.Fields.Add (field);
		changed = true;
	}

	ParameterDefinition Parameter (ParameterDefinition p, IGenericParameterProvider context)
	{
		var arg = new ParameterDefinition (p.Name, p.Attributes, output.ImportReference (p.ParameterType, context));
		if (p.HasConstant)
			arg.Constant = p.Constant;
		if (p.HasMarshalInfo)
			arg.MarshalInfo = p.MarshalInfo;
		Attributes (p, arg);
		return arg;
	}

	void Method (TypeDefinition type, MethodDefinition source, Dictionary<string, MethodDefinition> existing)
	{
		var action = contract.Get (source);
		MethodDefinition m;
		if (existing.TryGetValue (Key (source), out m)) {
			methods.Add (source, m);
			if (action == null)
				return;
			m.Attributes = (MethodAttributes)uint.Parse (action.Split (':') [1]);
			changed = true;
			if (!m.HasSecurity)
				m.SecurityDeclarations.Clear ();
			else if (source.HasSecurityDeclarations && !m.HasSecurityDeclarations) {
				foreach (var d in source.SecurityDeclarations)
					m.SecurityDeclarations.Add (new SecurityDeclaration (d.Action, d.GetBlob ()));
				m.HasSecurity = true;
				changed = true;
			}
			return;
		}
		if (action == null)
			return;
		m = new MethodDefinition (source.Name, (MethodAttributes)uint.Parse (action.Split (':') [1]), output.TypeSystem.Void) { CallingConvention = source.CallingConvention };
		type.Methods.Add (m);
		methods.Add (source, m);
		AddGenerics (source, m);

		m.ReturnType = output.ImportReference (source.ReturnType, m);
		Attributes (source.MethodReturnType, m.MethodReturnType);
		if (source.MethodReturnType.HasMarshalInfo)
			m.MethodReturnType.MarshalInfo = source.MethodReturnType.MarshalInfo;
		foreach (var p in source.Parameters)
			m.Parameters.Add (Parameter (p, m));
		Attributes (source, m);
		foreach (var d in source.SecurityDeclarations)
			m.SecurityDeclarations.Add (new SecurityDeclaration (d.Action, d.GetBlob ()));
		foreach (var ov in source.Overrides)
			m.Overrides.Add (output.ImportReference (ov, m));
		if (type.BaseType != null && type.BaseType.FullName == "System.MulticastDelegate") {
			m.ImplAttributes = MethodImplAttributes.Runtime;
		} else if (!m.IsAbstract) {
			m.IsPInvokeImpl = false;
			var il = m.Body.GetILProcessor ();
			if (action.StartsWith ("return:", StringComparison.Ordinal)) {
				il.Emit (OpCodes.Ret);
			} else {
				il.Emit (OpCodes.Newobj, unsupported);
				il.Emit (OpCodes.Throw);
			}
		}
		changed = true;
	}

	MethodDefinition Accessor (MethodDefinition source)
	{
		MethodDefinition dest;
		return source != null && methods.TryGetValue (source, out dest) ? dest : null;
	}

	void Complete (TypeDefinition source)
	{
		TypeDefinition type;
		if (!types.TryGetValue (source.FullName, out type))
			return;
		if (added.Contains (type)) {
			if (source.BaseType != null) {
				TypeDefinition baseType;
				type.BaseType = source.BaseType.Scope == source.Module && types.TryGetValue (source.BaseType.FullName, out baseType)
					? baseType : output.ImportReference (source.BaseType, type);
			}
			if (source.HasLayoutInfo) {
				type.PackingSize = source.PackingSize;
				type.ClassSize = source.ClassSize;
			}
			Attributes (source, type);

		}
		foreach (var i in source.Interfaces) {
			if (contract.Get (i) != null && !type.Interfaces.Any (x => Key (x.InterfaceType) == Key (i.InterfaceType))) {
				type.Interfaces.Add (new InterfaceImplementation (output.ImportReference (i.InterfaceType, type)));
				changed = true;
			}
		}
		foreach (var f in source.Fields)
			if (contract.Get (f) != null)
				Field (type, f);
		var existing = type.Methods.GroupBy (m => Key (m)).ToDictionary (g => g.Key, g => g.First ());
		foreach (var m in source.Methods)
			Method (type, m, existing);
		foreach (var p in source.Properties)
			if (contract.Get (p) != null)
				BindProperty (type, p, Accessor (p.GetMethod), Accessor (p.SetMethod), output.ImportReference (p.PropertyType, type));
		foreach (var e in source.Events) {
			if (contract.Get (e) == null || type.Events.Any (x => x.Name == e.Name) || (Accessor (e.AddMethod) == null && Accessor (e.RemoveMethod) == null))
				continue;
			var evt = new EventDefinition (e.Name, e.Attributes, output.ImportReference (e.EventType, type)) {
				AddMethod = Accessor (e.AddMethod), RemoveMethod = Accessor (e.RemoveMethod), InvokeMethod = Accessor (e.InvokeMethod)
			};
			type.Events.Add (evt);
			Attributes (e, evt);
			changed = true;
		}
	}

	void BindProperty (TypeDefinition type, PropertyDefinition source, MethodDefinition getter, MethodDefinition setter, TypeReference propertyType)
	{
		var prop = new PropertyDefinition (source.Name, source.Attributes, propertyType) { GetMethod = getter, SetMethod = setter };
		var original = type.Properties.FirstOrDefault (p => p.Name == prop.Name && Key (p.PropertyType) == Key (propertyType) &&
			p.Parameters.Select (i => Key (i.ParameterType)).SequenceEqual (prop.Parameters.Select (i => Key (i.ParameterType))));
		if (original != null) {
			original.GetMethod = getter;
			original.SetMethod = setter;
		} else {
			type.Properties.Add (prop);
			Attributes (source, prop);
		}
		changed = true;
	}

	bool Apply (ModuleDefinition reference)
	{
		var contract = reference.GetTypes ().Where (Visible).ToArray ();
		foreach (var t in contract)
			Declare (t);
		foreach (var t in contract)
			Complete (t);
		var properties = contract.SelectMany (t => t.Properties).ToDictionary (p => p.MetadataToken.ToInt32 ());
		foreach (var b in this.contract.PropertyBindings) {
			var p = properties [b [0]];
			var type = types [p.DeclaringType.FullName];
			var getter = b [1] == 0 ? null : methods [(MethodDefinition)reference.LookupToken (b [1])];
			var setter = b [2] == 0 ? null : methods [(MethodDefinition)reference.LookupToken (b [2])];
			var propertyType = b [3] < 0 ? output.ImportReference (p.PropertyType, type) : setter.Parameters [b [3]].ParameterType;
			BindProperty (type, p, getter, setter, propertyType);
		}
		return changed;
	}

	public static void Main (string[] args)
	{
		if (args [0] == "--record") {
			ProfileContract.Record (args [1], args [2], args [3], args [4]);
			return;
		}
		var reference = args [0];
		var input = args [1];
		var dest = args [2];
		var resolver = new DefaultAssemblyResolver ();
		resolver.AddSearchDirectory (Path.GetDirectoryName (input));
		resolver.AddSearchDirectory (Path.GetDirectoryName (reference));
		var options = new ReaderParameters { AssemblyResolver = resolver, SymbolReaderProvider = new DefaultSymbolReaderProvider (false) };
		options.MetadataImporterProvider = new ImporterProvider ();
		using (var impl = ModuleDefinition.ReadModule (input, options))
		using (var api = ModuleDefinition.ReadModule (reference))
		{
			if (new ProfileStubber (impl, ProfileContract.Read (args [3], api)).Apply (api)) {
				impl.Write (dest, new WriterParameters { WriteSymbols = impl.HasSymbols });
			} else {
				File.Copy (input, dest, true);
				var symbols = Path.ChangeExtension (input, ".pdb");
				if (File.Exists (symbols))
					File.Copy (symbols, Path.ChangeExtension (dest, ".pdb"), true);
			}
		}
	}
}
