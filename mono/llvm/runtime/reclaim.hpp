/**
 * \file
 * \brief Which methods' code memory is used again once the method is freed.
 */

#ifndef MONO_LLVM_RUNTIME_RECLAIM_HPP
#define MONO_LLVM_RUNTIME_RECLAIM_HPP

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Whether method's code and thunk can go to a later method once it is freed.
///
/// This holds where the collector counts an address in the code as a reference to
/// the object whose death frees it. Anywhere else a thread can still be in the code
/// of a freed method, so the code has to stay as it is.
bool code_is_reclaimable (MonoMethod *method);

} // namespace mono

#endif
