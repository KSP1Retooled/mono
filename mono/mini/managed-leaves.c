/**
 * \file
 * \brief The scalars that a managed value type travels as.
 *
 * A managed struct crosses a call as the scalars that it is made of, each
 * placed on its own. The JIT hands LLVM a first-class struct, and LLVM's
 * argument lowering flattens it.
 *
 * The scalars are the leaves of the packed struct that the translator builds
 * for the class. Layout that no field covers becomes an array of bytes in that
 * struct. Each of those bytes is a scalar too, and LLVM gives it a register of
 * its own.
 *
 * The walk here reads the class metadata rather than the IR, and it must find
 * the same scalars that the translator's struct has. Each target's call info
 * decides where the scalars go.
 */

#include "managed-leaves.h"

#include <mono/metadata/abi-details.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/metadata-internals.h>

/*
 * Returns the width primitive_type_to_llvm_type () (signature.cpp) gives
 * type, or 0 for a type it has none for.
 */
int
mono_managed_primitive_size (int type)
{
	switch (type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return 1;
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return 2;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_R4:
		return 4;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R8:
		return 8;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		return TARGET_SIZEOF_VOID_P;
	default:
		return 0;
	}
}

/*
 * Whether klass converts to a vector rather than to a struct, and how wide
 * that vector is. simd_class_to_llvm_type () (signature.cpp) is the rule. The
 * width is the vector's rather than the class's, because the three
 * System.Numerics types share one four-float register.
 */
gboolean
mono_managed_simd_size (MonoClass *klass, int *size)
{
	const char *name;

	if (!m_class_is_simd_type (klass))
		return FALSE;

	if (mono_class_is_ginst (klass)) {
		MonoType *etype = mono_class_get_generic_class (klass)->context.class_inst->type_argv [0];
		int esize = mono_managed_primitive_size (etype->type);
		int value_size = mono_class_value_size (klass, NULL);

		if (esize == 0 || value_size == 0 || (value_size % esize) != 0)
			return FALSE;
		*size = value_size;
		return TRUE;
	}

	name = m_class_get_name (klass);
	if (!strcmp (name, "Vector2d") || !strcmp (name, "Vector2l")
	    || !strcmp (name, "Vector2ul") || !strcmp (name, "Vector4i")
	    || !strcmp (name, "Vector4ui") || !strcmp (name, "Vector8s")
	    || !strcmp (name, "Vector8us") || !strcmp (name, "Vector16sb")
	    || !strcmp (name, "Vector16b") || !strcmp (name, "Vector4f")
	    || !strcmp (name, "Vector2") || !strcmp (name, "Vector3")
	    || !strcmp (name, "Vector4")) {
		*size = 16;
		return TRUE;
	}

	return FALSE;
}

static void
add_managed_leaf (GArray *leaves, int offset, int size, gboolean sse, gboolean vector)
{
	ManagedLeaf leaf;

	leaf.offset = offset;
	leaf.size = size;
	leaf.sse = sse ? 1 : 0;
	leaf.vector = vector ? 1 : 0;
	g_array_append_val (leaves, leaf);
}

static void
add_padding_leaves (GArray *leaves, int offset, int bytes)
{
	int i;

	for (i = 0; i < bytes; ++i)
		add_managed_leaf (leaves, offset + i, 1, FALSE, FALSE);
}

typedef struct {
	MonoType *type;
	int offset;
	int size;
} ManagedField;

/*
 * Orders fields by offset, keeping two at one offset in declaration order.
 * That order is what decides which field of an overlap keeps its slot.
 */
static void
sort_managed_fields (GArray *fields)
{
	ManagedField *data = (ManagedField *) fields->data;
	guint i, j;

	for (i = 1; i < fields->len; ++i) {
		ManagedField key = data [i];

		for (j = i; j > 0 && data [j - 1].offset > key.offset; --j)
			data [j] = data [j - 1];
		data [j] = key;
	}
}

/*
 * Fills the tail of a value type that no field reaches, repeating the last
 * field the way fill_tail () does where it is a scalar the classifier can
 * tell apart. A `fixed` buffer is one field of the element type inside a
 * class sized for the whole array, and repeating it keeps a float buffer on
 * the SSE file.
 */
static void
fill_managed_tail (GArray *leaves, int offset, const ManagedField *last, int at, int size)
{
	int gap = size - at;
	int width, count, i;

	if (last == NULL) {
		add_padding_leaves (leaves, offset + at, gap);
		return;
	}

	width = mono_managed_primitive_size (mini_get_underlying_type (last->type)->type);
	if (width > 0 && last->size == width) {
		count = gap / width;
		for (i = 0; i < count; ++i)
			mono_collect_managed_leaves (last->type, offset + at + (i * width), leaves);
		at += count * width;
		gap -= count * width;
	}

	if (gap > 0)
		add_padding_leaves (leaves, offset + at, gap);
}

static void
collect_managed_class_leaves (MonoClass *klass, int offset, GArray *leaves)
{
	int size = mono_class_value_size (klass, NULL);
	int simd_size, at = 0;
	guint entry = leaves->len;
	gboolean expressible = TRUE;
	GArray *fields;
	const ManagedField *last = NULL;
	gpointer iter;
	MonoClassField *field;
	guint i;

	if (mono_managed_simd_size (klass, &simd_size)) {
		add_managed_leaf (leaves, offset, simd_size, TRUE, TRUE);
		return;
	}

	if (m_class_is_enumtype (klass)) {
		mono_collect_managed_leaves (mono_class_enum_basetype_internal (klass), offset, leaves);
		return;
	}

	fields = g_array_new (FALSE, TRUE, sizeof (ManagedField));

	iter = NULL;
	while ((field = mono_class_get_fields_internal (klass, &iter))) {
		ManagedField f;
		int align;

		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
			continue;

		f.type = field->type;
		f.offset = field->offset - MONO_ABI_SIZEOF (MonoObject);
		f.size = mono_type_size (field->type, &align);
		g_array_append_val (fields, f);
	}

	sort_managed_fields (fields);

	for (i = 0; i < fields->len; ++i) {
		const ManagedField *f = &g_array_index (fields, ManagedField, i);

		/* An explicit layout can overlap. The first field at an offset
		 * keeps it, and the rest of the overlap becomes layout no field
		 * claims. */
		if (f->offset < at)
			continue;
		if (f->offset > at)
			add_padding_leaves (leaves, offset + at, f->offset - at);
		if (f->offset + f->size > size) {
			expressible = FALSE;
			break;
		}
		mono_collect_managed_leaves (f->type, offset + f->offset, leaves);
		at = f->offset + f->size;
		last = f;
	}

	if (!expressible) {
		/* A layout the walk cannot restate keeps its size, opaquely. */
		g_array_set_size (leaves, entry);
		add_padding_leaves (leaves, offset, size);
	} else if (at < size) {
		fill_managed_tail (leaves, offset, last, at, size);
	}

	g_array_free (fields, TRUE);
}

void
mono_collect_managed_leaves (MonoType *type, int offset, GArray *leaves)
{
	MonoType *t;
	int width;

	if (type->byref) {
		add_managed_leaf (leaves, offset, TARGET_SIZEOF_VOID_P, FALSE, FALSE);
		return;
	}

	t = mini_get_underlying_type (type);
	width = mono_managed_primitive_size (t->type);
	if (width > 0) {
		gboolean sse = t->type == MONO_TYPE_R4 || t->type == MONO_TYPE_R8;

		add_managed_leaf (leaves, offset, width, sse, FALSE);
		return;
	}

	switch (t->type) {
	case MONO_TYPE_GENERICINST:
		if (mono_type_generic_inst_is_valuetype (t))
			break;
		/* fall through */
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		add_managed_leaf (leaves, offset, TARGET_SIZEOF_VOID_P, FALSE, FALSE);
		return;
	default:
		break;
	}

	collect_managed_class_leaves (mono_class_from_mono_type_internal (t), offset, leaves);
}

