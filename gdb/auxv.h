/* Auxiliary vector support for GDB, the GNU debugger.

   Copyright (C) 2004-2021 Free Software Foundation, Inc.

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

#ifndef AUXV_H
#define AUXV_H

#include "target.h"

/* See "include/elf/common.h" for the definition of valid AT_* values.  */

/* The default implementation of to_auxv_parse, used by the target
   stack.

   Read one auxv entry from *READPTR, not reading locations >= ENDPTR.
   Return 0 if *READPTR is already at the end of the buffer.
   Return -1 if there is insufficient buffer for a whole entry.
   Return 1 if an entry was read into *TYPEP and *VALP.  */
extern int default_auxv_parse (struct target_ops *ops, gdb_byte **readptr,
			       gdb_byte *endptr, CORE_ADDR *typep,
			       CORE_ADDR *valp);

/* The SVR4 psABI implementation of to_auxv_parse, that uses an int to
   store the type rather than long as assumed by the default parser.

   Read one auxv entry from *READPTR, not reading locations >= ENDPTR.
   Return 0 if *READPTR is already at the end of the buffer.
   Return -1 if there is insufficient buffer for a whole entry.
   Return 1 if an entry was read into *TYPEP and *VALP.  */
extern int svr4_auxv_parse (struct gdbarch *gdbarch, gdb_byte **readptr,
			    gdb_byte *endptr, CORE_ADDR *typep,
			    CORE_ADDR *valp);

/* Read one auxv entry from *READPTR, not reading locations >= ENDPTR.
   Return 0 if *READPTR is already at the end of the buffer.
   Return -1 if there is insufficient buffer for a whole entry.
   Return 1 if an entry was read into *TYPEP and *VALP.  */
extern int target_auxv_parse (gdb_byte **readptr, gdb_byte *endptr,
			      CORE_ADDR *typep, CORE_ADDR *valp);

/* Extract the auxiliary vector entry with a_type matching MATCH.
   Return zero if no such entry was found, or -1 if there was
   an error getting the information.  On success, return 1 after
   storing the entry's value field in *VALP.  */
extern int target_auxv_search (struct target_ops *ops,
			       CORE_ADDR match, CORE_ADDR *valp);

/* Print a description of a single AUXV entry on the specified file.  */
enum auxv_format { AUXV_FORMAT_DEC, AUXV_FORMAT_HEX, AUXV_FORMAT_STR };

extern void fprint_auxv_entry (struct ui_file *file, const char *name,
			       const char *description,
			       enum auxv_format format, CORE_ADDR type,
			       CORE_ADDR val);

/* The default implementation of gdbarch_print_auxv_entry.  */

extern void default_print_auxv_entry (struct gdbarch *gdbarch,
				      struct ui_file *file, CORE_ADDR type,
				      CORE_ADDR val);

/* Print the contents of the target's AUXV on the specified file.  */
extern int fprint_target_auxv (struct ui_file *file, struct target_ops *ops);

enum target_xfer_status memory_xfer_auxv (struct target_ops *ops,
					  enum target_object object,
					  const char *annex,
					  gdb_byte *readbuf,
					  const gdb_byte *writebuf,
					  ULONGEST offset,
					  ULONGEST len,
					  ULONGEST *xfered_len);

#endif
