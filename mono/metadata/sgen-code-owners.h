/**
 * \file
 * \brief The code ranges whose addresses keep an object alive.
 */

#ifndef __MONO_METADATA_SGEN_CODE_OWNERS_H__
#define __MONO_METADATA_SGEN_CODE_OWNERS_H__

#include <glib.h>

#include <mono/utils/mono-forward.h>
#include <mono/utils/mono-publib.h>

#define SGEN_CODE_OWNER_BLOCK_BITS 16
#define SGEN_CODE_OWNER_FILTER_SIZE (1 << 16)

/// The most owners one address has.
#define SGEN_CODE_OWNERS_MAX 2

MONO_BEGIN_DECLS

/* LOCKING: the GC lock guards everything in this file. */

/// Nonzero for each hashed block of addresses that a range was ever added in.
extern guint8 sgen_code_owner_filter [SGEN_CODE_OWNER_FILTER_SIZE];

static inline gsize
sgen_code_owner_filter_slot (gsize addr)
{
	gsize block = addr >> SGEN_CODE_OWNER_BLOCK_BITS;

	return (block ^ (block >> 16)) % SGEN_CODE_OWNER_FILTER_SIZE;
}

/// Returns FALSE only for an address that no range holds.
static inline gboolean
sgen_code_owner_possible (gsize addr)
{
	return sgen_code_owner_filter [sgen_code_owner_filter_slot (addr)] != 0;
}

/// Adds the size bytes at start, and drops every range they overlap.
void sgen_code_owners_add (gsize start, gsize size, MonoGCHandle owner);

void sgen_code_owners_remove (gsize start, gsize size);

gboolean sgen_code_owners_empty (void);

/// Writes the owners of the ranges that hold addr, and returns how many there are.
///
/// A range holds the address just past its last byte as well. A call that does not return
/// can be the last instruction, and that address is the one it pushes.
int sgen_code_owners_find (gsize addr, MonoGCHandle owners [SGEN_CODE_OWNERS_MAX]);

MONO_END_DECLS

#endif
