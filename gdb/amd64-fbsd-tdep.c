/* Target-dependent code for FreeBSD/amd64.

   Copyright (C) 2003-2021 Free Software Foundation, Inc.

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

#include "defs.h"
#include "arch-utils.h"
#include "frame.h"
#include "gdbcore.h"
#include "regcache.h"
#include "osabi.h"
#include "regset.h"
#include "i386-fbsd-tdep.h"
#include "gdbsupport/x86-xstate.h"

#include "amd64-tdep.h"
#include "fbsd-tdep.h"
#include "solib-svr4.h"
#include "inferior.h"

/* Support for signal handlers.  */

/* Return whether THIS_FRAME corresponds to a FreeBSD sigtramp
   routine.  */

static const gdb_byte amd64fbsd_sigtramp_code[] =
{
  0x48, 0x8d, 0x7c, 0x24, 0x10, /* lea     SIGF_UC(%rsp),%rdi */
  0x6a, 0x00,			/* pushq   $0 */
  0x48, 0xc7, 0xc0, 0xa1, 0x01, 0x00, 0x00,
				/* movq    $SYS_sigreturn,%rax */
  0x0f, 0x05                    /* syscall */
};

static int
amd64fbsd_sigtramp_p (struct frame_info *this_frame)
{
  CORE_ADDR pc = get_frame_pc (this_frame);
  gdb_byte buf[sizeof amd64fbsd_sigtramp_code];

  if (!safe_frame_unwind_memory (this_frame, pc, buf, sizeof buf))
    return 0;
  if (memcmp (buf, amd64fbsd_sigtramp_code, sizeof amd64fbsd_sigtramp_code)
      != 0)
    return 0;

  return 1;
}

/* Assuming THIS_FRAME is for a BSD sigtramp routine, return the
   address of the associated sigcontext structure.  */

static CORE_ADDR
amd64fbsd_sigcontext_addr (struct frame_info *this_frame)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  CORE_ADDR sp;
  gdb_byte buf[8];

  /* The `struct sigcontext' (which really is an `ucontext_t' on
     FreeBSD/amd64) lives at a fixed offset in the signal frame.  See
     <machine/sigframe.h>.  */
  get_frame_register (this_frame, AMD64_RSP_REGNUM, buf);
  sp = extract_unsigned_integer (buf, 8, byte_order);
  return sp + 16;
}

/* FreeBSD 5.1-RELEASE or later.  */

/* Mapping between the general-purpose registers in `struct reg'
   format and GDB's register cache layout.

   Note that some registers are 32-bit, but since we're little-endian
   we get away with that.  */

/* From <machine/reg.h>.  */
static int amd64fbsd_r_reg_offset[] =
{
  14 * 8,			/* %rax */
  11 * 8,			/* %rbx */
  13 * 8,			/* %rcx */
  12 * 8,			/* %rdx */
  9 * 8,			/* %rsi */
  8 * 8,			/* %rdi */
  10 * 8,			/* %rbp */
  20 * 8,			/* %rsp */
  7 * 8,			/* %r8 ...  */
  6 * 8,
  5 * 8,
  4 * 8,
  3 * 8,
  2 * 8,
  1 * 8,
  0 * 8,			/* ... %r15 */
  17 * 8,			/* %rip */
  19 * 8,			/* %eflags */
  18 * 8,			/* %cs */
  21 * 8,			/* %ss */
  -1,				/* %ds */
  -1,				/* %es */
  -1,				/* %fs */
  -1				/* %gs */
};

/* Location of the signal trampoline.  */
CORE_ADDR amd64fbsd_sigtramp_start_addr;
CORE_ADDR amd64fbsd_sigtramp_end_addr;

/* From <machine/signal.h>.  */
int amd64fbsd_sc_reg_offset[] =
{
  24 + 6 * 8,			/* %rax */
  24 + 7 * 8,			/* %rbx */
  24 + 3 * 8,			/* %rcx */
  24 + 2 * 8,			/* %rdx */
  24 + 1 * 8,			/* %rsi */
  24 + 0 * 8,			/* %rdi */
  24 + 8 * 8,			/* %rbp */
  24 + 22 * 8,			/* %rsp */
  24 + 4 * 8,			/* %r8 ...  */
  24 + 5 * 8,
  24 + 9 * 8,
  24 + 10 * 8,
  24 + 11 * 8,
  24 + 12 * 8,
  24 + 13 * 8,
  24 + 14 * 8,			/* ... %r15 */
  24 + 19 * 8,			/* %rip */
  24 + 21 * 8,			/* %eflags */
  24 + 20 * 8,			/* %cs */
  24 + 23 * 8,			/* %ss */
  -1,				/* %ds */
  -1,				/* %es */
  -1,				/* %fs */
  -1				/* %gs */
};

/* Implement the core_read_description gdbarch method.  */

static const struct target_desc *
amd64fbsd_core_read_description (struct gdbarch *gdbarch,
				 struct target_ops *target,
				 bfd *abfd)
{
  return amd64_target_description (i386fbsd_core_read_xcr0 (abfd), true);
}

/* Similar to amd64_supply_fpregset, but use XSAVE extended state.  */

static void
amd64fbsd_supply_xstateregset (const struct regset *regset,
			       struct regcache *regcache, int regnum,
			       const void *xstateregs, size_t len)
{
  amd64_supply_xsave (regcache, regnum, xstateregs);
}

/* Similar to amd64_collect_fpregset, but use XSAVE extended state.  */

static void
amd64fbsd_collect_xstateregset (const struct regset *regset,
				const struct regcache *regcache,
				int regnum, void *xstateregs, size_t len)
{
  amd64_collect_xsave (regcache, regnum, xstateregs, 1);
}

static const struct regset amd64fbsd_xstateregset =
  {
    NULL,
    amd64fbsd_supply_xstateregset,
    amd64fbsd_collect_xstateregset
  };

/* Iterate over core file register note sections.  */

static void
amd64fbsd_iterate_over_regset_sections (struct gdbarch *gdbarch,
					iterate_over_regset_sections_cb *cb,
					void *cb_data,
					const struct regcache *regcache)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  cb (".reg", tdep->sizeof_gregset, tdep->sizeof_gregset, &i386_gregset, NULL,
      cb_data);
  cb (".reg2", tdep->sizeof_fpregset, tdep->sizeof_fpregset, &amd64_fpregset,
      NULL, cb_data);
  unsigned int xstate_size = get_x86_xstate_size (tdep->xcr0);
  cb (".reg-xstate", xstate_size, xstate_size,
      &amd64fbsd_xstateregset, "XSAVE extended state", cb_data);
}

/* Implement the get_thread_local_address gdbarch method.  */

static CORE_ADDR
amd64fbsd_get_thread_local_address (struct gdbarch *gdbarch, ptid_t ptid,
				    CORE_ADDR lm_addr, CORE_ADDR offset)
{
  struct regcache *regcache;

  regcache = get_thread_arch_regcache (current_inferior ()->process_target (),
				       ptid, gdbarch);

  target_fetch_registers (regcache, AMD64_FSBASE_REGNUM);

  ULONGEST fsbase;
  if (regcache->cooked_read (AMD64_FSBASE_REGNUM, &fsbase) != REG_VALID)
    error (_("Unable to fetch %%fsbase"));

  CORE_ADDR dtv_addr = fsbase + gdbarch_ptr_bit (gdbarch) / 8;
  return fbsd_get_thread_local_address (gdbarch, dtv_addr, lm_addr, offset);
}

static void
amd64fbsd_init_abi (struct gdbarch_info info, struct gdbarch *gdbarch)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  /* Generic FreeBSD support. */
  fbsd_init_abi (info, gdbarch);

  /* Obviously FreeBSD is BSD-based.  */
  i386bsd_init_abi (info, gdbarch);

  tdep->gregset_reg_offset = amd64fbsd_r_reg_offset;
  tdep->gregset_num_regs = ARRAY_SIZE (amd64fbsd_r_reg_offset);
  tdep->sizeof_gregset = 22 * 8;

  amd64_init_abi (info, gdbarch,
		  amd64_target_description (X86_XSTATE_SSE_MASK, true));

  tdep->sigtramp_p = amd64fbsd_sigtramp_p;
  tdep->sigtramp_start = amd64fbsd_sigtramp_start_addr;
  tdep->sigtramp_end = amd64fbsd_sigtramp_end_addr;
  tdep->sigcontext_addr = amd64fbsd_sigcontext_addr;
  tdep->sc_reg_offset = amd64fbsd_sc_reg_offset;
  tdep->sc_num_regs = ARRAY_SIZE (amd64fbsd_sc_reg_offset);

  tdep->xsave_xcr0_offset = I386_FBSD_XSAVE_XCR0_OFFSET;

  /* Iterate over core file register note sections.  */
  set_gdbarch_iterate_over_regset_sections
    (gdbarch, amd64fbsd_iterate_over_regset_sections);

  set_gdbarch_core_read_description (gdbarch,
				     amd64fbsd_core_read_description);

  /* FreeBSD uses SVR4-style shared libraries.  */
  set_solib_svr4_fetch_link_map_offsets
    (gdbarch, svr4_lp64_fetch_link_map_offsets);

  set_gdbarch_fetch_tls_load_module_address (gdbarch,
					     svr4_fetch_objfile_link_map);
  set_gdbarch_get_thread_local_address (gdbarch,
					amd64fbsd_get_thread_local_address);
}

void _initialize_amd64fbsd_tdep ();
void
_initialize_amd64fbsd_tdep ()
{
  gdbarch_register_osabi (bfd_arch_i386, bfd_mach_x86_64,
			  GDB_OSABI_FREEBSD, amd64fbsd_init_abi);
}
