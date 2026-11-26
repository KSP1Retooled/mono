/**
 * \file
 */

#ifndef __MONO_MINI_TIER0_ARCH_H__
#define __MONO_MINI_TIER0_ARCH_H__

#if defined(TARGET_AMD64)
#include "arch/amd64/tier0-amd64.h"
#elif defined(TARGET_ARM64)
#include "arch/arm64/tier0-arm64.h"
#else
#error this runtime targets amd64 and arm64
#endif

#endif /* __MONO_MINI_TIER0_ARCH_H__ */
