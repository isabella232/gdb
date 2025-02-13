/* Common code for x86 XSAVE extended state.

   Copyright (C) 2010-2021 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef COMMON_X86_XSTATE_H
#define COMMON_X86_XSTATE_H

#include "nat/x86-cpuid.h"
#include <cstdint>

/* Function returns the xstate size based on the features
   supported in xcr0.  */
unsigned int get_x86_xstate_size (uint64_t xcr0);

/* Helper struct to return the CPUID values of the extended
   feature.  Example for AVX: ID = 3, size = 256, offset = 576.  */
struct x86_extended_feature
{
  unsigned int ID, size, offset;
};

/* Get xstate extended feature information based on the
   feature ID.  */
x86_extended_feature get_x86_extended_feature (unsigned int feature);

/* The extended state feature IDs in the state component bitmap.  */
#define X86_XSTATE_X87_ID		0
#define X86_XSTATE_SSE_ID		1
#define X86_XSTATE_AVX_ID		2
#define X86_XSTATE_BNDREGS_ID		3
#define X86_XSTATE_BNDCFG_ID		4
#define X86_XSTATE_K_ID			5
#define X86_XSTATE_ZMM_H_ID		6
#define X86_XSTATE_ZMM_ID		7
#define X86_XSTATE_PT_ID		8
#define X86_XSTATE_PKRU_ID		9
#define X86_XSTATE_XTILECFG_ID		17
#define X86_XSTATE_XTILEDATA_ID		18

/* The extended state feature bits.  */
#define X86_XSTATE_X87		(1ULL << X86_XSTATE_X87_ID)
#define X86_XSTATE_SSE		(1ULL << X86_XSTATE_SSE_ID)
#define X86_XSTATE_AVX		(1ULL << X86_XSTATE_AVX_ID)
#define X86_XSTATE_BNDREGS	(1ULL << X86_XSTATE_BNDREGS_ID)
#define X86_XSTATE_BNDCFG	(1ULL << X86_XSTATE_BNDCFG_ID)
#define X86_XSTATE_MPX		(X86_XSTATE_BNDREGS | X86_XSTATE_BNDCFG)

/* AVX 512 adds three feature bits.  All three must be enabled.  */
#define X86_XSTATE_K		(1ULL << X86_XSTATE_K_ID)
#define X86_XSTATE_ZMM_H	(1ULL << X86_XSTATE_ZMM_H_ID)
#define X86_XSTATE_ZMM		(1ULL << X86_XSTATE_ZMM_ID)
#define X86_XSTATE_AVX512	(X86_XSTATE_K | X86_XSTATE_ZMM_H \
				 | X86_XSTATE_ZMM)

#define X86_XSTATE_PKRU		(1ULL << X86_XSTATE_PKRU_ID)

/* AMX adds two feature bits.  Both must be enabled.  */
#define X86_XSTATE_XTILECFG	(1ULL << X86_XSTATE_XTILECFG_ID)
#define X86_XSTATE_XTILEDATA	(1ULL << X86_XSTATE_XTILEDATA_ID)
#define X86_XSTATE_AMX		(X86_XSTATE_XTILECFG | X86_XSTATE_XTILEDATA)

/* Supported mask and size of the extended state.  */
#define X86_XSTATE_X87_MASK	X86_XSTATE_X87
#define X86_XSTATE_SSE_MASK	(X86_XSTATE_X87 | X86_XSTATE_SSE)
#define X86_XSTATE_AVX_MASK	(X86_XSTATE_SSE_MASK | X86_XSTATE_AVX)
#define X86_XSTATE_MPX_MASK	(X86_XSTATE_SSE_MASK | X86_XSTATE_MPX)
#define X86_XSTATE_AMX_MASK	X86_XSTATE_AMX
#define X86_XSTATE_AVX_MPX_MASK	(X86_XSTATE_AVX_MASK | X86_XSTATE_MPX)
#define X86_XSTATE_AVX_AVX512_MASK	(X86_XSTATE_AVX_MASK | X86_XSTATE_AVX512)
#define X86_XSTATE_AVX_MPX_AVX512_PKU_AMX_MASK 	(X86_XSTATE_AVX_MPX_MASK\
					| X86_XSTATE_AVX512 | X86_XSTATE_PKRU\
					| X86_XSTATE_AMX_MASK)

#define X86_XSTATE_ALL_MASK		(X86_XSTATE_AVX_MPX_AVX512_PKU_AMX_MASK)

#define X86_XSTATE_SSE_SIZE	576

/* In case one of the MPX XCR0 bits is set we consider we have MPX.  */
#define HAS_MPX(XCR0) (((XCR0) & X86_XSTATE_MPX) != 0)
#define HAS_AVX(XCR0) (((XCR0) & X86_XSTATE_AVX) != 0)
#define HAS_AVX512(XCR0) (((XCR0) & X86_XSTATE_AVX512) != 0)
#define HAS_PKRU(XCR0) (((XCR0) & X86_XSTATE_PKRU) != 0)
#define HAS_AMX(XCR0) (((XCR0) & X86_XSTATE_AMX) != 0)

/* Initial value for fctrl register, as defined in the X86 manual, and
   confirmed in the (Linux) kernel source.  When the x87 floating point
   feature is not enabled in an inferior we use this as the value of the
   fcrtl register.  */

#define I387_FCTRL_INIT_VAL 0x037f

/* Initial value for mxcsr register.  When the avx and sse floating point
   features are not enabled in an inferior we use this as the value of the
   mxcsr register.  */

#define I387_MXCSR_INIT_VAL 0x1f80

#endif /* COMMON_X86_XSTATE_H */
