/**
 * \file
 */

#ifndef __MONO_MINI_ARCH_H__
#define __MONO_MINI_ARCH_H__

#if defined(TARGET_AMD64)
#include "mini-amd64.h"
#elif defined(TARGET_ARM64)
#include "mini-arm64.h"
#else
#error this runtime targets amd64 and arm64
#endif

#ifdef TARGET_WIN32
#include "mini-windows.h"
#endif

#if (MONO_ARCH_FRAME_ALIGNMENT == 4)
#define MONO_ARCH_LOCALLOC_ALIGNMENT 8
#else
#define MONO_ARCH_LOCALLOC_ALIGNMENT MONO_ARCH_FRAME_ALIGNMENT
#endif

#endif /* __MONO_MINI_ARCH_H__ */  
