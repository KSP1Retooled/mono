/**
 * \file
 * Float to integer, which throws when the value does not fit.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

/*
 * Each range is tested before the cast, because C leaves a cast of a value
 * that does not fit undefined. arm64 and x86 give different results for one.
 * A NaN fails every comparison, so it lands outside each range too. The bounds
 * are exact in both float and double.
 */
#define I8_LOWER -9223372036854775808.0
#define I8_UPPER 9223372036854775808.0
#define U8_UPPER 18446744073709551616.0

static void
set_overflow (void)
{
	ERROR_DECL (error);
	mono_error_set_overflow (error);
	mono_error_set_pending_exception (error);
}

gint64
mono_fconv_ovf_i8 (double v)
{
	if (!(v >= I8_LOWER && v < I8_UPPER)) {
		set_overflow ();
		return 0;
	}
	return (gint64)v;
}

guint64
mono_fconv_ovf_u8 (double v)
{
	if (!(v > -1.0 && v < U8_UPPER)) {
		set_overflow ();
		return 0;
	}
	return (guint64)v;
}

gint64
mono_rconv_ovf_i8 (float v)
{
	if (!(v >= (float) I8_LOWER && v < (float) I8_UPPER)) {
		set_overflow ();
		return 0;
	}
	return (gint64)v;
}

guint64
mono_rconv_ovf_u8 (float v)
{
	if (!(v > -1.0f && v < (float) U8_UPPER)) {
		set_overflow ();
		return 0;
	}
	return (guint64)v;
}
