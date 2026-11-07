/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Force-included into every apfsprogs source (/FI): just enough POSIX and
 * GCC for apfsck to build with MSVC. See compat.c for the implementations.
 */
#ifndef KFSCK_COMPAT_H
#define KFSCK_COMPAT_H

/* 64-bit off_t (MSVC's is 32-bit long) */
#define _OFF_T_DEFINED
typedef long long off_t;
typedef long long _off_t;
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
typedef intptr_t ssize_t;
typedef long long loff_t;

#define __attribute__(x)
#define __builtin_expect(x, v)	(x)
#include <intrin.h>
#define __builtin_popcountll(x)	((int)__popcnt64(x))
#define __builtin_popcount(x)	((int)__popcnt(x))

#endif
