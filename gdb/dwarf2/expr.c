/* DWARF 2 Expression Evaluator.

   Copyright (C) 2001-2021 Free Software Foundation, Inc.

   Contributed by Daniel Berlin (dan@dberlin.org)

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
#include "symtab.h"
#include "gdbtypes.h"
#include "value.h"
#include "gdbcore.h"
#include "dwarf2.h"
#include "dwarf2/expr.h"
#include "dwarf2/loc.h"
#include "dwarf2/read.h"
#include "gdbsupport/underlying.h"
#include "gdbarch.h"

/* Cookie for gdbarch data.  */

static struct gdbarch_data *dwarf_arch_cookie;

/* This holds gdbarch-specific types used by the DWARF expression
   evaluator.  See comments in execute_stack_op.  */

struct dwarf_gdbarch_types
{
  struct type *dw_types[3];
};

/* Allocate and fill in dwarf_gdbarch_types for an arch.  */

static void *
dwarf_gdbarch_types_init (struct gdbarch *gdbarch)
{
  struct dwarf_gdbarch_types *types
    = GDBARCH_OBSTACK_ZALLOC (gdbarch, struct dwarf_gdbarch_types);

  /* The types themselves are lazily initialized.  */

  return types;
}

/* Return the type used for DWARF operations where the type is
   unspecified in the DWARF spec.  Only certain sizes are
   supported.  */

struct type *
dwarf_expr_context::address_type () const
{
  struct dwarf_gdbarch_types *types
    = (struct dwarf_gdbarch_types *) gdbarch_data (this->gdbarch,
						   dwarf_arch_cookie);
  int ndx;

  if (this->addr_size == 2)
    ndx = 0;
  else if (this->addr_size == 4)
    ndx = 1;
  else if (this->addr_size == 8)
    ndx = 2;
  else
    error (_("Unsupported address size in DWARF expressions: %d bits"),
	   8 * this->addr_size);

  if (types->dw_types[ndx] == NULL)
    types->dw_types[ndx]
      = arch_integer_type (this->gdbarch,
			   8 * this->addr_size,
			   0, "<signed DWARF address type>");

  return types->dw_types[ndx];
}

/* Create a new context for the expression evaluator.  */

dwarf_expr_context::dwarf_expr_context (dwarf2_per_objfile *per_objfile)
: gdbarch (NULL),
  addr_size (0),
  ref_addr_size (0),
  recursion_depth (0),
  max_recursion_depth (0x100),
  location (DWARF_VALUE_MEMORY),
  len (0),
  data (NULL),
  initialized (0),
  per_objfile (per_objfile)
{
}

/* Push VALUE onto the stack.  */

void
dwarf_expr_context::push (struct value *value, bool in_stack_memory)
{
  stack.emplace_back (value, in_stack_memory);
}

/* Push VALUE onto the stack.  */

void
dwarf_expr_context::push_address (CORE_ADDR value, bool in_stack_memory)
{
  push (value_from_ulongest (address_type (), value), in_stack_memory);
}

/* Pop the top item off of the stack.  */

void
dwarf_expr_context::pop ()
{
  if (stack.empty ())
    error (_("dwarf expression stack underflow"));

  stack.pop_back ();
}

/* Retrieve the N'th item on the stack.  */

struct value *
dwarf_expr_context::fetch (int n)
{
  if (stack.size () <= n)
     error (_("Asked for position %d of stack, "
	      "stack only has %zu elements on it."),
	    n, stack.size ());
  return stack[stack.size () - (1 + n)].value;
}

/* Require that TYPE be an integral type; throw an exception if not.  */

static void
dwarf_require_integral (struct type *type)
{
  if (type->code () != TYPE_CODE_INT
      && type->code () != TYPE_CODE_CHAR
      && type->code () != TYPE_CODE_BOOL)
    error (_("integral type expected in DWARF expression"));
}

/* Return the unsigned form of TYPE.  TYPE is necessarily an integral
   type.  */

static struct type *
get_unsigned_type (struct gdbarch *gdbarch, struct type *type)
{
  switch (TYPE_LENGTH (type))
    {
    case 1:
      return builtin_type (gdbarch)->builtin_uint8;
    case 2:
      return builtin_type (gdbarch)->builtin_uint16;
    case 4:
      return builtin_type (gdbarch)->builtin_uint32;
    case 8:
      return builtin_type (gdbarch)->builtin_uint64;
    default:
      error (_("no unsigned variant found for type, while evaluating "
	       "DWARF expression"));
    }
}

/* Return the signed form of TYPE.  TYPE is necessarily an integral
   type.  */

static struct type *
get_signed_type (struct gdbarch *gdbarch, struct type *type)
{
  switch (TYPE_LENGTH (type))
    {
    case 1:
      return builtin_type (gdbarch)->builtin_int8;
    case 2:
      return builtin_type (gdbarch)->builtin_int16;
    case 4:
      return builtin_type (gdbarch)->builtin_int32;
    case 8:
      return builtin_type (gdbarch)->builtin_int64;
    default:
      error (_("no signed variant found for type, while evaluating "
	       "DWARF expression"));
    }
}

/* Retrieve the N'th item on the stack, converted to an address.  */

CORE_ADDR
dwarf_expr_context::fetch_address (int n)
{
  struct value *result_val = fetch (n);
  enum bfd_endian byte_order = gdbarch_byte_order (this->gdbarch);
  ULONGEST result;

  dwarf_require_integral (value_type (result_val));
  result = extract_unsigned_integer (value_contents (result_val),
				     TYPE_LENGTH (value_type (result_val)),
				     byte_order);

  /* For most architectures, calling extract_unsigned_integer() alone
     is sufficient for extracting an address.  However, some
     architectures (e.g. MIPS) use signed addresses and using
     extract_unsigned_integer() will not produce a correct
     result.  Make sure we invoke gdbarch_integer_to_address()
     for those architectures which require it.  */
  if (gdbarch_integer_to_address_p (this->gdbarch))
    {
      gdb_byte *buf = (gdb_byte *) alloca (this->addr_size);
      struct type *int_type = get_unsigned_type (this->gdbarch,
						 value_type (result_val));

      store_unsigned_integer (buf, this->addr_size, byte_order, result);
      return gdbarch_integer_to_address (this->gdbarch, int_type, buf);
    }

  return (CORE_ADDR) result;
}

/* Retrieve the in_stack_memory flag of the N'th item on the stack.  */

bool
dwarf_expr_context::fetch_in_stack_memory (int n)
{
  if (stack.size () <= n)
     error (_("Asked for position %d of stack, "
	      "stack only has %zu elements on it."),
	    n, stack.size ());
  return stack[stack.size () - (1 + n)].in_stack_memory;
}

/* Return true if the expression stack is empty.  */

bool
dwarf_expr_context::stack_empty_p () const
{
  return stack.empty ();
}

/* Add a new piece to the dwarf_expr_context's piece list.  */
void
dwarf_expr_context::add_piece (ULONGEST size, ULONGEST offset)
{
  this->pieces.emplace_back ();
  dwarf_expr_piece &p = this->pieces.back ();

  p.location = this->location;
  p.size = size;
  p.offset = offset;

  if (p.location == DWARF_VALUE_LITERAL)
    {
      p.v.literal.data = this->data;
      p.v.literal.length = this->len;
    }
  else if (stack_empty_p ())
    {
      p.location = DWARF_VALUE_OPTIMIZED_OUT;
      /* Also reset the context's location, for our callers.  This is
	 a somewhat strange approach, but this lets us avoid setting
	 the location to DWARF_VALUE_MEMORY in all the individual
	 cases in the evaluator.  */
      this->location = DWARF_VALUE_OPTIMIZED_OUT;
    }
  else if (p.location == DWARF_VALUE_MEMORY)
    {
      p.v.mem.addr = fetch_address (0);
      p.v.mem.in_stack_memory = fetch_in_stack_memory (0);
    }
  else if (p.location == DWARF_VALUE_IMPLICIT_POINTER)
    {
      p.v.ptr.die_sect_off = (sect_offset) this->len;
      p.v.ptr.offset = value_as_long (fetch (0));
    }
  else if (p.location == DWARF_VALUE_REGISTER)
    p.v.regno = value_as_long (fetch (0));
  else
    {
      p.v.value = fetch (0);
    }
}

/* Evaluate the expression at ADDR (LEN bytes long).  */

void
dwarf_expr_context::eval (const gdb_byte *addr, size_t len)
{
  int old_recursion_depth = this->recursion_depth;

  execute_stack_op (addr, addr + len);

  /* RECURSION_DEPTH becomes invalid if an exception was thrown here.  */

  gdb_assert (this->recursion_depth == old_recursion_depth);
}

/* Helper to read a uleb128 value or throw an error.  */

const gdb_byte *
safe_read_uleb128 (const gdb_byte *buf, const gdb_byte *buf_end,
		   uint64_t *r)
{
  buf = gdb_read_uleb128 (buf, buf_end, r);
  if (buf == NULL)
    error (_("DWARF expression error: ran off end of buffer reading uleb128 value"));
  return buf;
}

/* Helper to read a sleb128 value or throw an error.  */

const gdb_byte *
safe_read_sleb128 (const gdb_byte *buf, const gdb_byte *buf_end,
		   int64_t *r)
{
  buf = gdb_read_sleb128 (buf, buf_end, r);
  if (buf == NULL)
    error (_("DWARF expression error: ran off end of buffer reading sleb128 value"));
  return buf;
}

const gdb_byte *
safe_skip_leb128 (const gdb_byte *buf, const gdb_byte *buf_end)
{
  buf = gdb_skip_leb128 (buf, buf_end);
  if (buf == NULL)
    error (_("DWARF expression error: ran off end of buffer reading leb128 value"));
  return buf;
}


/* Return true iff the types T1 and T2 are "the same".  This only does
   checks that might reasonably be needed to compare DWARF base
   types.  */

static int
base_types_equal_p (struct type *t1, struct type *t2)
{
  if (t1->code () != t2->code ())
    return 0;
  if (TYPE_UNSIGNED (t1) != TYPE_UNSIGNED (t2))
    return 0;
  return TYPE_LENGTH (t1) == TYPE_LENGTH (t2);
}

/* If <BUF..BUF_END] contains DW_FORM_block* with single DW_OP_reg* return the
   DWARF register number.  Otherwise return -1.  */

int
dwarf_block_to_dwarf_reg (const gdb_byte *buf, const gdb_byte *buf_end)
{
  uint64_t dwarf_reg;

  if (buf_end <= buf)
    return -1;
  if (*buf >= DW_OP_reg0 && *buf <= DW_OP_reg31)
    {
      if (buf_end - buf != 1)
	return -1;
      return *buf - DW_OP_reg0;
    }

  if (*buf == DW_OP_regval_type || *buf == DW_OP_GNU_regval_type)
    {
      buf++;
      buf = gdb_read_uleb128 (buf, buf_end, &dwarf_reg);
      if (buf == NULL)
	return -1;
      buf = gdb_skip_leb128 (buf, buf_end);
      if (buf == NULL)
	return -1;
    }
  else if (*buf == DW_OP_regx)
    {
      buf++;
      buf = gdb_read_uleb128 (buf, buf_end, &dwarf_reg);
      if (buf == NULL)
	return -1;
    }
  else
    return -1;
  if (buf != buf_end || (int) dwarf_reg != dwarf_reg)
    return -1;
  return dwarf_reg;
}

/* If <BUF..BUF_END] contains DW_FORM_block* with just DW_OP_breg*(0) and
   DW_OP_deref* return the DWARF register number.  Otherwise return -1.
   DEREF_SIZE_RETURN contains -1 for DW_OP_deref; otherwise it contains the
   size from DW_OP_deref_size.  */

int
dwarf_block_to_dwarf_reg_deref (const gdb_byte *buf, const gdb_byte *buf_end,
				CORE_ADDR *deref_size_return)
{
  uint64_t dwarf_reg;
  int64_t offset;

  if (buf_end <= buf)
    return -1;

  if (*buf >= DW_OP_breg0 && *buf <= DW_OP_breg31)
    {
      dwarf_reg = *buf - DW_OP_breg0;
      buf++;
      if (buf >= buf_end)
	return -1;
    }
  else if (*buf == DW_OP_bregx)
    {
      buf++;
      buf = gdb_read_uleb128 (buf, buf_end, &dwarf_reg);
      if (buf == NULL)
	return -1;
      if ((int) dwarf_reg != dwarf_reg)
       return -1;
    }
  else
    return -1;

  buf = gdb_read_sleb128 (buf, buf_end, &offset);
  if (buf == NULL)
    return -1;
  if (offset != 0)
    return -1;

  if (*buf == DW_OP_deref)
    {
      buf++;
      *deref_size_return = -1;
    }
  else if (*buf == DW_OP_deref_size)
    {
      buf++;
      if (buf >= buf_end)
       return -1;
      *deref_size_return = *buf++;
    }
  else
    return -1;

  if (buf != buf_end)
    return -1;

  return dwarf_reg;
}

/* If <BUF..BUF_END] contains DW_FORM_block* with single DW_OP_fbreg(X) fill
   in FB_OFFSET_RETURN with the X offset and return 1.  Otherwise return 0.  */

int
dwarf_block_to_fb_offset (const gdb_byte *buf, const gdb_byte *buf_end,
			  CORE_ADDR *fb_offset_return)
{
  int64_t fb_offset;

  if (buf_end <= buf)
    return 0;

  if (*buf != DW_OP_fbreg)
    return 0;
  buf++;

  buf = gdb_read_sleb128 (buf, buf_end, &fb_offset);
  if (buf == NULL)
    return 0;
  *fb_offset_return = fb_offset;
  if (buf != buf_end || fb_offset != (LONGEST) *fb_offset_return)
    return 0;

  return 1;
}

/* If <BUF..BUF_END] contains DW_FORM_block* with single DW_OP_bregSP(X) fill
   in SP_OFFSET_RETURN with the X offset and return 1.  Otherwise return 0.
   The matched SP register number depends on GDBARCH.  */

int
dwarf_block_to_sp_offset (struct gdbarch *gdbarch, const gdb_byte *buf,
			  const gdb_byte *buf_end, CORE_ADDR *sp_offset_return)
{
  uint64_t dwarf_reg;
  int64_t sp_offset;

  if (buf_end <= buf)
    return 0;
  if (*buf >= DW_OP_breg0 && *buf <= DW_OP_breg31)
    {
      dwarf_reg = *buf - DW_OP_breg0;
      buf++;
    }
  else
    {
      if (*buf != DW_OP_bregx)
       return 0;
      buf++;
      buf = gdb_read_uleb128 (buf, buf_end, &dwarf_reg);
      if (buf == NULL)
	return 0;
    }

  if (dwarf_reg_to_regnum (gdbarch, dwarf_reg)
      != gdbarch_sp_regnum (gdbarch))
    return 0;

  buf = gdb_read_sleb128 (buf, buf_end, &sp_offset);
  if (buf == NULL)
    return 0;
  *sp_offset_return = sp_offset;
  if (buf != buf_end || sp_offset != (LONGEST) *sp_offset_return)
    return 0;

  return 1;
}

/* The engine for the expression evaluator.  Using the context in this
   object, evaluate the expression between OP_PTR and OP_END.  */

void
dwarf_expr_context::execute_stack_op (const gdb_byte *op_ptr,
				      const gdb_byte *op_end)
{
  enum bfd_endian byte_order = gdbarch_byte_order (this->gdbarch);
  /* Old-style "untyped" DWARF values need special treatment in a
     couple of places, specifically DW_OP_mod and DW_OP_shr.  We need
     a special type for these values so we can distinguish them from
     values that have an explicit type, because explicitly-typed
     values do not need special treatment.  This special type must be
     different (in the `==' sense) from any base type coming from the
     CU.  */
  struct type *address_type = this->address_type ();

  this->location = DWARF_VALUE_MEMORY;
  this->initialized = 1;  /* Default is initialized.  */

  if (this->recursion_depth > this->max_recursion_depth)
    error (_("DWARF-2 expression error: Loop detected (%d)."),
	   this->recursion_depth);
  this->recursion_depth++;

  while (op_ptr < op_end)
    {
      enum dwarf_location_atom op = (enum dwarf_location_atom) *op_ptr++;
      ULONGEST result;
      /* Assume the value is not in stack memory.
	 Code that knows otherwise sets this to true.
	 Some arithmetic on stack addresses can probably be assumed to still
	 be a stack address, but we skip this complication for now.
	 This is just an optimization, so it's always ok to punt
	 and leave this as false.  */
      bool in_stack_memory = false;
      uint64_t uoffset, reg;
      int64_t offset;
      struct value *result_val = NULL;

      /* The DWARF expression might have a bug causing an infinite
	 loop.  In that case, quitting is the only way out.  */
      QUIT;

      switch (op)
	{
	case DW_OP_lit0:
	case DW_OP_lit1:
	case DW_OP_lit2:
	case DW_OP_lit3:
	case DW_OP_lit4:
	case DW_OP_lit5:
	case DW_OP_lit6:
	case DW_OP_lit7:
	case DW_OP_lit8:
	case DW_OP_lit9:
	case DW_OP_lit10:
	case DW_OP_lit11:
	case DW_OP_lit12:
	case DW_OP_lit13:
	case DW_OP_lit14:
	case DW_OP_lit15:
	case DW_OP_lit16:
	case DW_OP_lit17:
	case DW_OP_lit18:
	case DW_OP_lit19:
	case DW_OP_lit20:
	case DW_OP_lit21:
	case DW_OP_lit22:
	case DW_OP_lit23:
	case DW_OP_lit24:
	case DW_OP_lit25:
	case DW_OP_lit26:
	case DW_OP_lit27:
	case DW_OP_lit28:
	case DW_OP_lit29:
	case DW_OP_lit30:
	case DW_OP_lit31:
	  result = op - DW_OP_lit0;
	  result_val = value_from_ulongest (address_type, result);
	  break;

	case DW_OP_addr:
	  result = extract_unsigned_integer (op_ptr,
					     this->addr_size, byte_order);
	  op_ptr += this->addr_size;
	  /* Some versions of GCC emit DW_OP_addr before
	     DW_OP_GNU_push_tls_address.  In this case the value is an
	     index, not an address.  We don't support things like
	     branching between the address and the TLS op.  */
	  if (op_ptr >= op_end || *op_ptr != DW_OP_GNU_push_tls_address)
	    result += this->per_objfile->objfile->text_section_offset ();
	  result_val = value_from_ulongest (address_type, result);
	  break;

	case DW_OP_addrx:
	case DW_OP_GNU_addr_index:
	  op_ptr = safe_read_uleb128 (op_ptr, op_end, &uoffset);
	  result = this->get_addr_index (uoffset);
	  result += this->per_objfile->objfile->text_section_offset ();
	  result_val = value_from_ulongest (address_type, result);
	  break;
	case DW_OP_GNU_const_index:
	  op_ptr = safe_read_uleb128 (op_ptr, op_end, &uoffset);
	  result = this->get_addr_index (uoffset);
	  result_val = value_from_ulongest (address_type, result);
	  break;

	case DW_OP_const1u:
	  result = extract_unsigned_integer (op_ptr, 1, byte_order);
	  result_val = value_from_ulongest (address_type, result);
	  op_ptr += 1;
	  break;
	case DW_OP_const1s:
	  result = extract_signed_integer (op_ptr, 1, byte_order);
	  result_val = value_from_ulongest (address_type, result);
	  op_ptr += 1;
	  break;
	case DW_OP_const2u:
	  result = extract_unsigned_integer (op_ptr, 2, byte_order);
	  result_val = value_from_ulongest (address_type, result);
	  op_ptr += 2;
	  break;
	case DW_OP_const2s:
	  result = extract_signed_integer (op_ptr, 2, byte_order);
	  result_val = value_from_ulongest (address_type, result);
	  op_ptr += 2;
	  break;
	case DW_OP_const4u:
	  result = extract_unsigned_integer (op_ptr, 4, byte_order);
	  result_val = value_from_ulongest (address_type, result);
	  op_ptr += 4;
	  break;
	case DW_OP_const4s:
	  result = extract_signed_integer (op_ptr, 4, byte_order);
	  result_val = value_from_ulongest (address_type, result);
	  op_ptr += 4;
	  break;
	case DW_OP_const8u:
	  result = extract_unsigned_integer (op_ptr, 8, byte_order);
	  result_val = value_from_ulongest (address_type, result);
	  op_ptr += 8;
	  break;
	case DW_OP_const8s:
	  result = extract_signed_integer (op_ptr, 8, byte_order);
	  result_val = value_from_ulongest (address_type, result);
	  op_ptr += 8;
	  break;
	case DW_OP_constu:
	  op_ptr = safe_read_uleb128 (op_ptr, op_end, &uoffset);
	  result = uoffset;
	  result_val = value_from_ulongest (address_type, result);
	  break;
	case DW_OP_consts:
	  op_ptr = safe_read_sleb128 (op_ptr, op_end, &offset);
	  result = offset;
	  result_val = value_from_ulongest (address_type, result);
	  break;

	/* The DW_OP_reg operations are required to occur alone in
	   location expressions.  */
	case DW_OP_reg0:
	case DW_OP_reg1:
	case DW_OP_reg2:
	case DW_OP_reg3:
	case DW_OP_reg4:
	case DW_OP_reg5:
	case DW_OP_reg6:
	case DW_OP_reg7:
	case DW_OP_reg8:
	case DW_OP_reg9:
	case DW_OP_reg10:
	case DW_OP_reg11:
	case DW_OP_reg12:
	case DW_OP_reg13:
	case DW_OP_reg14:
	case DW_OP_reg15:
	case DW_OP_reg16:
	case DW_OP_reg17:
	case DW_OP_reg18:
	case DW_OP_reg19:
	case DW_OP_reg20:
	case DW_OP_reg21:
	case DW_OP_reg22:
	case DW_OP_reg23:
	case DW_OP_reg24:
	case DW_OP_reg25:
	case DW_OP_reg26:
	case DW_OP_reg27:
	case DW_OP_reg28:
	case DW_OP_reg29:
	case DW_OP_reg30:
	case DW_OP_reg31:
	  result = op - DW_OP_reg0;
	  result_val = value_from_ulongest (address_type, result);
	  this->location = DWARF_VALUE_REGISTER;
	  break;

	case DW_OP_regx:
	  op_ptr = safe_read_uleb128 (op_ptr, op_end, &reg);

	  result = reg;
	  result_val = value_from_ulongest (address_type, result);
	  this->location = DWARF_VALUE_REGISTER;
	  break;

	case DW_OP_INTEL_regs:
	  {
	    value * const vregno = fetch (0);
	    pop ();

	    dwarf_require_integral (value_type (vregno));

	    result = (ULONGEST) value_as_long (vregno);
	    result_val = value_from_ulongest (address_type, result);
	    this->location = DWARF_VALUE_REGISTER;
	  }
	  break;

	case DW_OP_implicit_value:
	  {
	    uint64_t len;

	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &len);
	    if (op_ptr + len > op_end)
	      error (_("DW_OP_implicit_value: too few bytes available."));
	    this->len = len;
	    this->data = op_ptr;
	    this->location = DWARF_VALUE_LITERAL;
	    op_ptr += len;
	  }
	  goto no_push;

	case DW_OP_stack_value:
	  this->location = DWARF_VALUE_STACK;
	  goto no_push;

	case DW_OP_implicit_pointer:
	case DW_OP_GNU_implicit_pointer:
	  {
	    int64_t len;

	    if (this->ref_addr_size == -1)
	      error (_("DWARF-2 expression error: DW_OP_implicit_pointer "
		       "is not allowed in frame context"));

	    /* The referred-to DIE of sect_offset kind.  */
	    this->len = extract_unsigned_integer (op_ptr, this->ref_addr_size,
						 byte_order);
	    op_ptr += this->ref_addr_size;

	    /* The byte offset into the data.  */
	    op_ptr = safe_read_sleb128 (op_ptr, op_end, &len);
	    result = (ULONGEST) len;
	    result_val = value_from_ulongest (address_type, result);

	    this->location = DWARF_VALUE_IMPLICIT_POINTER;
	  }
	  break;

	case DW_OP_breg0:
	case DW_OP_breg1:
	case DW_OP_breg2:
	case DW_OP_breg3:
	case DW_OP_breg4:
	case DW_OP_breg5:
	case DW_OP_breg6:
	case DW_OP_breg7:
	case DW_OP_breg8:
	case DW_OP_breg9:
	case DW_OP_breg10:
	case DW_OP_breg11:
	case DW_OP_breg12:
	case DW_OP_breg13:
	case DW_OP_breg14:
	case DW_OP_breg15:
	case DW_OP_breg16:
	case DW_OP_breg17:
	case DW_OP_breg18:
	case DW_OP_breg19:
	case DW_OP_breg20:
	case DW_OP_breg21:
	case DW_OP_breg22:
	case DW_OP_breg23:
	case DW_OP_breg24:
	case DW_OP_breg25:
	case DW_OP_breg26:
	case DW_OP_breg27:
	case DW_OP_breg28:
	case DW_OP_breg29:
	case DW_OP_breg30:
	case DW_OP_breg31:
	  {
	    op_ptr = safe_read_sleb128 (op_ptr, op_end, &offset);
	    result = this->read_addr_from_reg (op - DW_OP_breg0);
	    result += offset;
	    result_val = value_from_ulongest (address_type, result);
	  }
	  break;
	case DW_OP_bregx:
	  {
	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &reg);
	    op_ptr = safe_read_sleb128 (op_ptr, op_end, &offset);
	    result = this->read_addr_from_reg (reg);
	    result += offset;
	    result_val = value_from_ulongest (address_type, result);
	  }
	  break;
	case DW_OP_fbreg:
	  {
	    const gdb_byte *datastart;
	    size_t datalen;

	    op_ptr = safe_read_sleb128 (op_ptr, op_end, &offset);

	    /* Rather than create a whole new context, we simply
	       backup the current stack locally and install a new empty stack,
	       then reset it afterwards, effectively erasing whatever the
	       recursive call put there.  */
	    std::vector<dwarf_stack_value> saved_stack = std::move (stack);
	    stack.clear ();

	    /* FIXME: cagney/2003-03-26: This code should be using
               get_frame_base_address(), and then implement a dwarf2
               specific this_base method.  */
	    this->get_frame_base (&datastart, &datalen);
	    eval (datastart, datalen);
	    if (this->location == DWARF_VALUE_MEMORY)
	      result = fetch_address (0);
	    else if (this->location == DWARF_VALUE_REGISTER)
	      result = this->read_addr_from_reg (value_as_long (fetch (0)));
	    else
	      error (_("Not implemented: computing frame "
		       "base using explicit value operator"));
	    result = result + offset;
	    result_val = value_from_ulongest (address_type, result);
	    in_stack_memory = true;

	    /* Restore the content of the original stack.  */
	    stack = std::move (saved_stack);

	    this->location = DWARF_VALUE_MEMORY;
	  }
	  break;

	case DW_OP_dup:
	  result_val = fetch (0);
	  in_stack_memory = fetch_in_stack_memory (0);
	  break;

	case DW_OP_drop:
	  pop ();
	  goto no_push;

	case DW_OP_pick:
	  offset = *op_ptr++;
	  result_val = fetch (offset);
	  in_stack_memory = fetch_in_stack_memory (offset);
	  break;
	  
	case DW_OP_swap:
	  {
	    if (stack.size () < 2)
	       error (_("Not enough elements for "
			"DW_OP_swap.  Need 2, have %zu."),
		      stack.size ());

	    dwarf_stack_value &t1 = stack[stack.size () - 1];
	    dwarf_stack_value &t2 = stack[stack.size () - 2];
	    std::swap (t1, t2);
	    goto no_push;
	  }

	case DW_OP_over:
	  result_val = fetch (1);
	  in_stack_memory = fetch_in_stack_memory (1);
	  break;

	case DW_OP_rot:
	  {
	    if (stack.size () < 3)
	       error (_("Not enough elements for "
			"DW_OP_rot.  Need 3, have %zu."),
		      stack.size ());

	    dwarf_stack_value temp = stack[stack.size () - 1];
	    stack[stack.size () - 1] = stack[stack.size () - 2];
	    stack[stack.size () - 2] = stack[stack.size () - 3];
	    stack[stack.size () - 3] = temp;
	    goto no_push;
	  }

	case DW_OP_deref:
	case DW_OP_deref_size:
	case DW_OP_deref_type:
	case DW_OP_GNU_deref_type:
	  {
	    int addr_size = (op == DW_OP_deref ? this->addr_size : *op_ptr++);
	    gdb_byte *buf = (gdb_byte *) alloca (addr_size);
	    CORE_ADDR addr = fetch_address (0);
	    struct type *type;

	    pop ();

	    if (op == DW_OP_deref_type || op == DW_OP_GNU_deref_type)
	      {
		op_ptr = safe_read_uleb128 (op_ptr, op_end, &uoffset);
		cu_offset type_die_cu_off = (cu_offset) uoffset;
		type = get_base_type (type_die_cu_off, 0);
	      }
	    else
	      type = address_type;

	    this->read_mem (buf, addr, addr_size);

	    /* If the size of the object read from memory is different
	       from the type length, we need to zero-extend it.  */
	    if (TYPE_LENGTH (type) != addr_size)
	      {
		ULONGEST datum =
		  extract_unsigned_integer (buf, addr_size, byte_order);

		buf = (gdb_byte *) alloca (TYPE_LENGTH (type));
		store_unsigned_integer (buf, TYPE_LENGTH (type),
					byte_order, datum);
	      }

	    result_val = value_from_contents_and_address (type, buf, addr);
	    break;
	  }

	case DW_OP_abs:
	case DW_OP_neg:
	case DW_OP_not:
	case DW_OP_plus_uconst:
	  {
	    /* Unary operations.  */
	    result_val = fetch (0);
	    pop ();

	    switch (op)
	      {
	      case DW_OP_abs:
		if (value_less (result_val,
				value_zero (value_type (result_val), not_lval)))
		  result_val = value_neg (result_val);
		break;
	      case DW_OP_neg:
		result_val = value_neg (result_val);
		break;
	      case DW_OP_not:
		dwarf_require_integral (value_type (result_val));
		result_val = value_complement (result_val);
		break;
	      case DW_OP_plus_uconst:
		dwarf_require_integral (value_type (result_val));
		result = value_as_long (result_val);
		op_ptr = safe_read_uleb128 (op_ptr, op_end, &reg);
		result += reg;
		result_val = value_from_ulongest (address_type, result);
		break;
	      }
	  }
	  break;

	case DW_OP_and:
	case DW_OP_div:
	case DW_OP_minus:
	case DW_OP_mod:
	case DW_OP_mul:
	case DW_OP_or:
	case DW_OP_plus:
	case DW_OP_shl:
	case DW_OP_shr:
	case DW_OP_shra:
	case DW_OP_xor:
	case DW_OP_le:
	case DW_OP_ge:
	case DW_OP_eq:
	case DW_OP_lt:
	case DW_OP_gt:
	case DW_OP_ne:
	  {
	    /* Binary operations.  */
	    struct value *first, *second;

	    second = fetch (0);
	    pop ();

	    first = fetch (0);
	    pop ();

	    if (! base_types_equal_p (value_type (first), value_type (second)))
	      error (_("Incompatible types on DWARF stack"));

	    switch (op)
	      {
	      case DW_OP_and:
		dwarf_require_integral (value_type (first));
		dwarf_require_integral (value_type (second));
		result_val = value_binop (first, second, BINOP_BITWISE_AND);
		break;
	      case DW_OP_div:
		result_val = value_binop (first, second, BINOP_DIV);
                break;
	      case DW_OP_minus:
		result_val = value_binop (first, second, BINOP_SUB);
		break;
	      case DW_OP_mod:
		{
		  int cast_back = 0;
		  struct type *orig_type = value_type (first);

		  /* We have to special-case "old-style" untyped values
		     -- these must have mod computed using unsigned
		     math.  */
		  if (orig_type == address_type)
		    {
		      struct type *utype
			= get_unsigned_type (this->gdbarch, orig_type);

		      cast_back = 1;
		      first = value_cast (utype, first);
		      second = value_cast (utype, second);
		    }
		  /* Note that value_binop doesn't handle float or
		     decimal float here.  This seems unimportant.  */
		  result_val = value_binop (first, second, BINOP_MOD);
		  if (cast_back)
		    result_val = value_cast (orig_type, result_val);
		}
		break;
	      case DW_OP_mul:
		result_val = value_binop (first, second, BINOP_MUL);
		break;
	      case DW_OP_or:
		dwarf_require_integral (value_type (first));
		dwarf_require_integral (value_type (second));
		result_val = value_binop (first, second, BINOP_BITWISE_IOR);
		break;
	      case DW_OP_plus:
		result_val = value_binop (first, second, BINOP_ADD);
		break;
	      case DW_OP_shl:
		dwarf_require_integral (value_type (first));
		dwarf_require_integral (value_type (second));
		result_val = value_binop (first, second, BINOP_LSH);
		break;
	      case DW_OP_shr:
		dwarf_require_integral (value_type (first));
		dwarf_require_integral (value_type (second));
		if (!TYPE_UNSIGNED (value_type (first)))
		  {
		    struct type *utype
		      = get_unsigned_type (this->gdbarch, value_type (first));

		    first = value_cast (utype, first);
		  }

		result_val = value_binop (first, second, BINOP_RSH);
		/* Make sure we wind up with the same type we started
		   with.  */
		if (value_type (result_val) != value_type (second))
		  result_val = value_cast (value_type (second), result_val);
                break;
	      case DW_OP_shra:
		dwarf_require_integral (value_type (first));
		dwarf_require_integral (value_type (second));
		if (TYPE_UNSIGNED (value_type (first)))
		  {
		    struct type *stype
		      = get_signed_type (this->gdbarch, value_type (first));

		    first = value_cast (stype, first);
		  }

		result_val = value_binop (first, second, BINOP_RSH);
		/* Make sure we wind up with the same type we started
		   with.  */
		if (value_type (result_val) != value_type (second))
		  result_val = value_cast (value_type (second), result_val);
		break;
	      case DW_OP_xor:
		dwarf_require_integral (value_type (first));
		dwarf_require_integral (value_type (second));
		result_val = value_binop (first, second, BINOP_BITWISE_XOR);
		break;
	      case DW_OP_le:
		/* A <= B is !(B < A).  */
		result = ! value_less (second, first);
		result_val = value_from_ulongest (address_type, result);
		break;
	      case DW_OP_ge:
		/* A >= B is !(A < B).  */
		result = ! value_less (first, second);
		result_val = value_from_ulongest (address_type, result);
		break;
	      case DW_OP_eq:
		result = value_equal (first, second);
		result_val = value_from_ulongest (address_type, result);
		break;
	      case DW_OP_lt:
		result = value_less (first, second);
		result_val = value_from_ulongest (address_type, result);
		break;
	      case DW_OP_gt:
		/* A > B is B < A.  */
		result = value_less (second, first);
		result_val = value_from_ulongest (address_type, result);
		break;
	      case DW_OP_ne:
		result = ! value_equal (first, second);
		result_val = value_from_ulongest (address_type, result);
		break;
	      default:
		internal_error (__FILE__, __LINE__,
				_("Can't be reached."));
	      }
	  }
	  break;

	case DW_OP_call_frame_cfa:
	  result = this->get_frame_cfa ();
	  result_val = value_from_ulongest (address_type, result);
	  in_stack_memory = true;
	  break;

	case DW_OP_GNU_push_tls_address:
	case DW_OP_form_tls_address:
	  /* Variable is at a constant offset in the thread-local
	  storage block into the objfile for the current thread and
	  the dynamic linker module containing this expression.  Here
	  we return returns the offset from that base.  The top of the
	  stack has the offset from the beginning of the thread
	  control block at which the variable is located.  Nothing
	  should follow this operator, so the top of stack would be
	  returned.  */
	  result = value_as_long (fetch (0));
	  pop ();
	  result = this->get_tls_address (result);
	  result_val = value_from_ulongest (address_type, result);
	  break;

	case DW_OP_skip:
	  offset = extract_signed_integer (op_ptr, 2, byte_order);
	  op_ptr += 2;
	  op_ptr += offset;
	  goto no_push;

	case DW_OP_bra:
	  {
	    struct value *val;

	    offset = extract_signed_integer (op_ptr, 2, byte_order);
	    op_ptr += 2;
	    val = fetch (0);
	    dwarf_require_integral (value_type (val));
	    if (value_as_long (val) != 0)
	      op_ptr += offset;
	    pop ();
	  }
	  goto no_push;

	case DW_OP_nop:
	  goto no_push;

        case DW_OP_piece:
          {
            uint64_t size;

            /* Record the piece.  */
            op_ptr = safe_read_uleb128 (op_ptr, op_end, &size);
	    add_piece (8 * size, 0);

            /* Pop off the address/regnum, and reset the location
	       type.  */
	    if (this->location != DWARF_VALUE_LITERAL
		&& this->location != DWARF_VALUE_OPTIMIZED_OUT)
	      pop ();
            this->location = DWARF_VALUE_MEMORY;
          }
          goto no_push;

	case DW_OP_bit_piece:
	  {
	    uint64_t size, uleb_offset;

            /* Record the piece.  */
	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &size);
	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &uleb_offset);
	    add_piece (size, uleb_offset);

	    /* Pop off the address/regnum, and reset the location
	       type.  */
	    if (this->location != DWARF_VALUE_LITERAL
		&& this->location != DWARF_VALUE_OPTIMIZED_OUT)
	      pop ();
	    this->location = DWARF_VALUE_MEMORY;
	  }
	  goto no_push;

	case DW_OP_INTEL_piece_stack:
	  {
	    value * const vsize = fetch (0);
	    pop ();

	    dwarf_require_integral (value_type (vsize));
	    const ULONGEST size
	      = extract_unsigned_integer (value_contents (vsize),
					  TYPE_LENGTH (value_type (vsize)),
					  byte_order);

	    /* Record the piece.  */
	    add_piece (8 * size, 0);

	    /* Pop off the address/regnum, and reset the location
	       type.  */
	    if (this->location != DWARF_VALUE_LITERAL
		&& this->location != DWARF_VALUE_OPTIMIZED_OUT)
	      pop ();
	    this->location = DWARF_VALUE_MEMORY;
	  }
	  goto no_push;

	case DW_OP_INTEL_bit_piece_stack:
	  {
	    value * const vsize = fetch (0);
	    pop ();

	    value * const voffset = fetch (0);
	    pop ();

	    dwarf_require_integral (value_type (vsize));
	    const ULONGEST size
	      = extract_unsigned_integer (value_contents (vsize),
					  TYPE_LENGTH (value_type (vsize)),
					  byte_order);

	    dwarf_require_integral (value_type (voffset));
	    const ULONGEST off
	      = extract_unsigned_integer (value_contents (voffset),
					  TYPE_LENGTH (value_type (voffset)),
					  byte_order);
	    /* Record the piece.  */
	    add_piece (size, off);

            /* Pop off the address/regnum, and reset the location
	       type.  */
	    if (this->location != DWARF_VALUE_LITERAL
		&& this->location != DWARF_VALUE_OPTIMIZED_OUT)
	      pop ();
            this->location = DWARF_VALUE_MEMORY;
	  }
	  goto no_push;

	case DW_OP_INTEL_push_bit_piece_stack:
	  {
	    value * const vsize = fetch (0);
	    pop ();

	    value * const voffset = fetch (0);
	    pop ();

	    dwarf_require_integral (value_type (vsize));
	    const ULONGEST size
	      = extract_unsigned_integer (value_contents (vsize),
					  TYPE_LENGTH (value_type (vsize)),
					  byte_order);

	    dwarf_require_integral (value_type (voffset));
	    const ULONGEST off
	      = extract_unsigned_integer (value_contents (voffset),
					  TYPE_LENGTH (value_type (voffset)),
					  byte_order);

	    const int addr_bit_size = this->addr_size * 8;
	    if (addr_bit_size < size)
	      error (_("DW_OP_INTEL_push_bit_piece_stack: "
		       "size '%" PRIu64 "' too big."), size);

	    switch (this->location)
	      {
	      case DWARF_VALUE_MEMORY:
		{
		  const CORE_ADDR addr = fetch_address (0);
		  pop ();

		  /* We can access memory only byte-wise.  Compute the
		     container and the offset into that container.  */
		  const ULONGEST boff = off & 7;
		  const ULONGEST coff = off >> 3;
		  const ULONGEST bsize = size + boff;
		  const ULONGEST csize = (bsize + 7) >> 3;
		  const CORE_ADDR caddr = addr + coff;

		  gdb_byte * const buf = (gdb_byte *) alloca (csize);
		  this->read_mem (buf, caddr, csize);

		  result = 0;
		  copy_bitwise ((gdb_byte *) &result, addr_bit_size - size,
				buf, boff, size, byte_order);
		}
		break;

	      case DWARF_VALUE_REGISTER:
		{
		  const LONGEST dwregnum = value_as_long (fetch (0));
		  pop ();

		  result = 0;
		  read_reg ((gdb_byte *) &result, off, size, dwregnum);
		}
		break;

	      case DWARF_VALUE_STACK:
		/* Nothing to be done.  The value is already on the top of
		   the stack.  */
		goto no_push;

	      case DWARF_VALUE_LITERAL:
		{
		  const ULONGEST bitsize = this->len * 8;
		  if (bitsize < (size + off))
		    error (_("DW_OP_INTEL_push_bit_piece_stack: underflow: "
			     "requested '%" PRIu64 "' at  '%" PRIu64 "', "
			     "available: '%" PRIu64 "."),
			   size, off, bitsize);

		  result = 0;
		  copy_bitwise ((gdb_byte *) &result, (addr_size * 8) - size,
				this->data, off, size, byte_order);
		}
		break;

	      case DWARF_VALUE_OPTIMIZED_OUT:
		error (_("DW_OP_INTEL_push_bit_piece_stack: "
			 "value optimized out"));
		break;

	      case DWARF_VALUE_IMPLICIT_POINTER:
		error (_("DW_OP_INTEL_push_bit_piece_stack: "
			 "pointer value is implicit"));
		break;
	      }

	    /* We consumed the underlying location description.  Let's
	       reset the location to its default.  */
	    this->location = DWARF_VALUE_MEMORY;
	    result_val = value_from_ulongest (address_type, result);
	  }
	  break;

	case DW_OP_GNU_uninit:
	  if (op_ptr != op_end)
	    error (_("DWARF-2 expression error: DW_OP_GNU_uninit must always "
		   "be the very last op."));

	  this->initialized = 0;
	  goto no_push;

	case DW_OP_call2:
	  {
	    cu_offset cu_off
	      = (cu_offset) extract_unsigned_integer (op_ptr, 2, byte_order);
	    op_ptr += 2;
	    this->dwarf_call (cu_off);
	  }
	  goto no_push;

	case DW_OP_call4:
	  {
	    cu_offset cu_off
	      = (cu_offset) extract_unsigned_integer (op_ptr, 4, byte_order);
	    op_ptr += 4;
	    this->dwarf_call (cu_off);
	  }
	  goto no_push;

	case DW_OP_GNU_variable_value:
	  {
	    sect_offset sect_off
	      = (sect_offset) extract_unsigned_integer (op_ptr,
	                                                this->ref_addr_size,
							byte_order);
	    op_ptr += this->ref_addr_size;
	    result_val = value_cast (address_type,
				     this->dwarf_variable_value (sect_off));
	  }
	  break;
	
	case DW_OP_entry_value:
	case DW_OP_GNU_entry_value:
	  {
	    uint64_t len;
	    CORE_ADDR deref_size;
	    union call_site_parameter_u kind_u;

	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &len);
	    if (op_ptr + len > op_end)
	      error (_("DW_OP_entry_value: too few bytes available."));

	    kind_u.dwarf_reg = dwarf_block_to_dwarf_reg (op_ptr, op_ptr + len);
	    if (kind_u.dwarf_reg != -1)
	      {
		op_ptr += len;
		this->push_dwarf_reg_entry_value (CALL_SITE_PARAMETER_DWARF_REG,
						  kind_u,
						  -1 /* deref_size */);
		goto no_push;
	      }

	    kind_u.dwarf_reg = dwarf_block_to_dwarf_reg_deref (op_ptr,
							       op_ptr + len,
							       &deref_size);
	    if (kind_u.dwarf_reg != -1)
	      {
		if (deref_size == -1)
		  deref_size = this->addr_size;
		op_ptr += len;
		this->push_dwarf_reg_entry_value (CALL_SITE_PARAMETER_DWARF_REG,
						  kind_u, deref_size);
		goto no_push;
	      }

	    error (_("DWARF-2 expression error: DW_OP_entry_value is "
		     "supported only for single DW_OP_reg* "
		     "or for DW_OP_breg*(0)+DW_OP_deref*"));
	  }

	case DW_OP_GNU_parameter_ref:
	  {
	    union call_site_parameter_u kind_u;

	    kind_u.param_cu_off
	      = (cu_offset) extract_unsigned_integer (op_ptr, 4, byte_order);
	    op_ptr += 4;
	    this->push_dwarf_reg_entry_value (CALL_SITE_PARAMETER_PARAM_OFFSET,
					      kind_u,
					      -1 /* deref_size */);
	  }
	  goto no_push;

	case DW_OP_const_type:
	case DW_OP_GNU_const_type:
	  {
	    int n;
	    const gdb_byte *data;
	    struct type *type;

	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &uoffset);
	    cu_offset type_die_cu_off = (cu_offset) uoffset;

	    n = *op_ptr++;
	    data = op_ptr;
	    op_ptr += n;

	    type = get_base_type (type_die_cu_off, n);
	    result_val = value_from_contents (type, data);
	  }
	  break;

	case DW_OP_regval_type:
	case DW_OP_GNU_regval_type:
	  {
	    struct type *type;

	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &reg);
	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &uoffset);
	    cu_offset type_die_cu_off = (cu_offset) uoffset;

	    type = get_base_type (type_die_cu_off, 0);
	    result_val = this->get_reg_value (type, reg);
	  }
	  break;

	case DW_OP_convert:
	case DW_OP_GNU_convert:
	case DW_OP_reinterpret:
	case DW_OP_GNU_reinterpret:
	  {
	    struct type *type;

	    op_ptr = safe_read_uleb128 (op_ptr, op_end, &uoffset);
	    cu_offset type_die_cu_off = (cu_offset) uoffset;

	    if (to_underlying (type_die_cu_off) == 0)
	      type = address_type;
	    else
	      type = get_base_type (type_die_cu_off, 0);

	    result_val = fetch (0);
	    pop ();

	    if (op == DW_OP_convert || op == DW_OP_GNU_convert)
	      result_val = value_cast (type, result_val);
	    else if (type == value_type (result_val))
	      {
		/* Nothing.  */
	      }
	    else if (TYPE_LENGTH (type)
		     != TYPE_LENGTH (value_type (result_val)))
	      error (_("DW_OP_reinterpret has wrong size"));
	    else
	      result_val
		= value_from_contents (type,
				       value_contents_all (result_val));
	  }
	  break;

	case DW_OP_push_object_address:
	  /* Return the address of the object we are currently observing.  */
	  result = this->get_object_address ();
	  result_val = value_from_ulongest (address_type, result);
	  break;

	case DW_OP_INTEL_push_simd_lane:
	  {
	    /* Return the current SIMD lane.  */
	    result = this->get_simd_lane ();
	    result_val = value_from_ulongest (address_type, result);
	  }
	  break;

	default:
	  error (_("Unhandled dwarf expression opcode 0x%x"), op);
	}

      /* Most things push a result value.  */
      gdb_assert (result_val != NULL);
      push (result_val, in_stack_memory);
    no_push:
      ;
    }

  /* To simplify our main caller, if the result is an implicit
     pointer, then make a pieced value.  This is ok because we can't
     have implicit pointers in contexts where pieces are invalid.  */
  if (this->location == DWARF_VALUE_IMPLICIT_POINTER)
    add_piece (8 * this->addr_size, 0);

  this->recursion_depth--;
  gdb_assert (this->recursion_depth >= 0);
}

void _initialize_dwarf2expr ();
void
_initialize_dwarf2expr ()
{
  dwarf_arch_cookie
    = gdbarch_data_register_post_init (dwarf_gdbarch_types_init);
}
