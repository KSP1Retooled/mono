/**
 * \file
 * \brief The scalars that a managed value type travels as.
 */

#ifndef __MONO_MINI_MANAGED_LEAVES_H__
#define __MONO_MINI_MANAGED_LEAVES_H__

#include "mini.h"

typedef struct {
	int offset;
	int size;
	guint8 sse;    /* rides the floating-point file rather than the integer one */
	guint8 vector; /* a SIMD value, which fills a whole vector register */
} ManagedLeaf;

/* Appends type's scalars to leaves, each at offset plus its own offset. */
void mono_collect_managed_leaves (MonoType *type, int offset, GArray *leaves);

/* Returns the width that the translator gives a primitive type, or 0 when the
 * translator gives the type no width. */
int mono_managed_primitive_size (int type);

/* Whether the translator converts klass to a vector rather than to a struct.
 * Stores the vector's width in size. */
gboolean mono_managed_simd_size (MonoClass *klass, int *size);

#endif
