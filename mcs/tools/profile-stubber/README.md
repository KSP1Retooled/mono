# Unity Windows profile

Unity finishes its Windows class libraries by adding .NET 4.8 API stubs. The editor's upstream build uses `external/buildscripts/stub_classlibs.pl` and the internal Stevedore ProfileStubber. This tool uses the reference assemblies already pinned in `external/binary-reference-assemblies/v4.8`. The Windows player DLLs go in `mcs/class/lib/unityjit-win32` under the build directory. The compiler and other BCL projects use the implementations in `mcs/class/lib/unstubbed/unityjit-win32` to keep stubs out of overload resolution.

The contracts select declarations by metadata token in the reference assembly. The first line pins its MVID. Ranges are inclusive. Type and method entries carry the editor's CLR attributes. New methods throw `PlatformNotSupportedException`, except for runtime-provided delegate methods, abstract declarations and empty attribute methods recorded as `return`. A `bind` row records the reference property, getter and setter tokens for an indexer whose editor metadata associates its accessors differently. The final number selects the setter parameter used as the property type, or `ffffffff` for the reference property type.

To regenerate for an editor update, start with an empty output directory and run the bootstrap tool after building the unmodified profile. Compare the resulting DLLs to the editor before replacing the recorded contracts.

```text
mono profile-stubber.exe --record <binary-reference-assemblies/v4.8> <editor-profile> <unstubbed-profile> <output-directory>
```

The compiler preserves a working implementation when the contract adds metadata to an existing member. The recorder fails if a missing method in the editor contains an implementation that would be replaced by a throw stub.

