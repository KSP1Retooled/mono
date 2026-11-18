mono_declare_managed(
  PROGRAM
  NAME profile-stubber.exe
  PROFILES build
  REFS System System.Core Mono.Cecil
  SOURCES ProfileStubber.cs ProfileContract.cs
  TARGET_NET_REFERENCE v4.7
  INTERMEDIATE
  NO_INSTALL)
