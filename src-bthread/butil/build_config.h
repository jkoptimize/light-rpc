// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from brpc butil/build_config.h, trimmed to Linux + x86_64/aarch64.

// Platform / compiler / architecture detection macros. This is the only file
// that may emit OS_/ARCH_/COMPILER_ macros; everything else consumes them.

#ifndef FAST_BUTIL_BUILD_CONFIG_H_
#define FAST_BUTIL_BUILD_CONFIG_H_

// ---------------------------------------------------------------------------
// Operating system
// ---------------------------------------------------------------------------
#if defined(__linux__)
#define OS_LINUX 1
#else
#error "fast::butil only supports Linux"
#endif

#if defined(OS_LINUX)
#define OS_POSIX 1
#endif

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------
#if defined(__GNUC__)
#define COMPILER_GCC 1
#else
#error "fast::butil only supports GCC/Clang"
#endif

// ---------------------------------------------------------------------------
// Processor architecture
// ---------------------------------------------------------------------------
#if defined(_M_X64) || defined(__x86_64__)
#define ARCH_CPU_X86_FAMILY 1
#define ARCH_CPU_X86_64 1
#define ARCH_CPU_64_BITS 1
#define ARCH_CPU_LITTLE_ENDIAN 1
#elif defined(__aarch64__)
#define ARCH_CPU_ARM_FAMILY 1
#define ARCH_CPU_ARM64 1
#define ARCH_CPU_64_BITS 1
#define ARCH_CPU_LITTLE_ENDIAN 1
#else
#error "fast::butil only supports x86_64 and aarch64"
#endif

// ---------------------------------------------------------------------------
// Language level
// ---------------------------------------------------------------------------
#if defined(__GXX_EXPERIMENTAL_CXX0X__) || __cplusplus >= 201103L
#define BUTIL_CXX11_ENABLED 1
#endif

#endif  // FAST_BUTIL_BUILD_CONFIG_H_
