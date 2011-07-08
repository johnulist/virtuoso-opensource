/*
 *  col.c
 *
 *  $Id$
 *
 *  Column compression
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2011 OpenLink Software
 *
 *  This project is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; only version 2 of the License, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "sqlnode.h"
#include "sqlfn.h"
#include "lisprdr.h"
#include "date.h"
#include "datesupp.h"
#include "multibyte.h"
#include "srvmultibyte.h"
#include "bif_xper.h"		/* IvAn/DvBlobXper/001212 Include added */
#include "sqltype.h"
#include "xmltree.h"
#include "xml.h"
#include "arith.h"
#include "sqlbif.h"
#include "mhash.h"


#define IS_64(n) \
  (!((n) >= (int64) INT32_MIN && (n) <= (int64) INT32_MAX))

#define IS_64_T(n, dtp)						\
  (IS_IRI_DTP (dtp) ? (((iri_id_t)n) > (iri_id_t)0xffffffff) \
: (!((n) >= (int64) INT32_MIN && (n) <= (int64) INT32_MAX)))


dtp_t
any_ce_flags (dtp_t * any)
{
  uint32 len;
  switch (any[0])
    {
    case DV_NULL:
    case DV_SHORT_INT:
    case DV_LONG_INT:
      return 0;
    case DV_INT64:
      return CE_IS_64;
    case DV_IRI_ID:
      return CE_IS_IRI;
    case DV_IRI_ID_8:
      return CE_IS_IRI | CE_IS_64;
    case DV_STRING:
      len = LONG_REF_NA (any + 1);
      return len > 3 ? CET_CHARS : CET_ANY;
    case DV_SHORT_STRING_SERIAL:
      len = any[1];
      return len > 3 ? CET_CHARS : CET_ANY;
    case DV_DB_NULL:
      return CET_NULL;
    default:
      return CET_ANY;
    }
}


int
any_ce_len (db_buf_t dv)
{
  dtp_t dtp = dv[0];
  switch (dtp)
    {
    case DV_NULL:
    case DV_SHORT_INT:
    case DV_IRI_ID:
    case DV_LONG_INT:
      return 4;
    case DV_INT64:
    case DV_IRI_ID_8:
      return 8;
    case DV_STRING:
      return 5 + LONG_REF_NA (dv + 1);
    case DV_SHORT_STRING_SERIAL:
      return 2 + dv[1];
    case DV_DB_NULL:
      return 0;
    default:
      {
	int len;
	DB_BUF_TLEN (len, dtp, dv);
	return len;
      }
    }
}


void cs_dict (compress_state_t * cs, int from, int to);


int
cs_non_comp_len (compress_state_t * cs, int from, int to, int *int_type)
{
  int l = 0, inx, best;
  if (!cs->cs_heterogenous && (DV_LONG_INT == cs->cs_dtp || DV_IRI_ID == cs->cs_dtp) && !cs->cs_any_64)
    {
      *int_type = 32;
      return (to - from) * 4;
    }
  if (CS_NO_ANY_INT_VEC & cs->cs_exclude && cs->cs_all_int)
    return 64;
  *int_type = cs_int_type (cs, (db_buf_t *) cs->cs_values, from, to, &best);
  if (64 == *int_type)
    return (to - from) * 8;
  if (32 == *int_type)
    return (to - from) * 4;
  for (inx = from; inx < to; inx++)
    l += box_length (cs->cs_values[inx]) - 1;
  return l + CE_VEC_LENGTH_BYTES (to - from);
}


dtp_t
any_canonical_dtp (db_buf_t dv)
{
  /* like the box dtp, leave out length variants */
  dtp_t dtp = *dv;
  switch (dtp)
    {
    case DV_IRI_ID_8:
      return DV_IRI_ID;
    case DV_SHORT_INT:
      return DV_LONG_INT;
    case DV_INT64:
      return DV_LONG_INT;
    case DV_SHORT_STRING_SERIAL:
      return DV_STRING;
    default:
      return dtp;
    }
}


int64
any_num_f (dtp_t * any)
{
  switch (any[0])
    {
    case DV_IRI_ID:
      return (iri_id_t) (uint32) LONG_REF_NA (any + 1);
    case DV_SHORT_INT:
      return ((char *) any)[1];
    case DV_SINGLE_FLOAT:
    case DV_RDF_ID:
    case DV_LONG_INT:
      return LONG_REF_NA (any + 1);
    case DV_IRI_ID_8:
    case DV_RDF_ID_8:
    case DV_DOUBLE_FLOAT:
    case DV_INT64:
      return INT64_REF_NA (any + 1);
    case DV_DATETIME:
      {
	if (DT_TYPE_DATE == DT_DT_TYPE (any + 1))
	  {
	    return DT_DAY (any + 1);
	  }
      }
    default:
      {
	int len;
	DB_BUF_TLEN (len, any[0], any);
	return (uint32) LONG_REF_NA (any + len - 4);
      }
    }
  return 0;
}

int
any_add (db_buf_t any, int len, int64 delta, db_buf_t res, dtp_t flags)
{
  int hl;
  uint32 last;
  if (CET_CHARS == (flags & CE_DTP_MASK))
    {
      if (len > 128)
	{
	  if (len > 255)
	    {
	      res[0] = DV_LONG_STRING;
	      LONG_SET_NA (res + 1, len);
	      memcpy (res + 5, any + 2, len);
	      hl = 5;
	    }
	  else
	    {
	      res[1] = len;
	      memcpy (res + 2, any + 2, len);
	      hl = 2;
	    }
	}
      else
	{
	  res[0] = DV_SHORT_STRING_SERIAL;
	  res[1] = len;
	  memcpy (res + 2, any + 1, len + 1);
	  hl = 2;
	}
      last = LONG_REF_NA (res + hl + len - 4);
      last += delta;
      LONG_SET_NA (res + hl + len - 4, last);
      return len + hl;
    }
  switch (any[0])
    {
    case DV_DATETIME:
      {
	int day = DT_DAY (any + 1);
	res[0] = DV_DATETIME;
	memcpy_dt (res + 1, any + 1);
	DT_SET_DAY (res + 1, (day + delta));
	break;
      }
    case DV_SHORT_INT:
      memcpy (res, any, 2);
      res[1] += delta;
      return len;
    default:
      if (delta)
	{
	  uint32 last = LONG_REF_NA (any + len - 4);
	  if (len < 4)
	    GPF_T1 ("dv any offset not allowd for len under 4");
	  memcpy_16 (res, any, len - 4);
	  LONG_SET_NA (res + len - 4, last + delta);
	}
      else
	memcpy_16 (res, any, len);
    }
  return len;
}


dtp_t
any_ce_dtp (db_buf_t dv)
{
  dtp_t dtp = dv[0];
  switch (dtp)
    {
    case DV_SHORT_INT:
    case DV_LONG_INT:
    case DV_INT64:
      return DV_LONG_INT;
    case DV_IRI_ID:
    case DV_IRI_ID_8:
      return DV_IRI_ID;
    case DV_DATETIME:
      switch (DT_DT_TYPE (dv + 1))
	{
	case DT_TYPE_DATE:
	  return DV_DATE;
	case DT_TYPE_COMPAT_POSITIVE_TZ:
	case DT_TYPE_COMPAT_NEGATIVE_TZ:
	case DT_TYPE_DATETIME:
	  return DV_DATETIME;
	case DT_TYPE_TIME:
	  return DV_TIME;
	default:
	  GPF_T1 ("bad datetime tag");
	}
    default:
      return dtp;
    }
}


int
cs_int_high_distinct (db_buf_t * values, int from, int to)
{
  int inx, ct;
  dk_hash_t *ht;
  if (to - from < 2)
    return 0;
  ht = hash_table_allocate (to - from);
  for (inx = from; inx < to; inx++)
    {
      int64 n = any_num (values[inx]);
      sethash ((void *) (n & CLEAR_LOW_BYTE), ht, (void *) 1);
    }
  ct = ht->ht_count;
  hash_table_free (ht);
  return ct;
}


int
cs_int_type (compress_state_t * cs, db_buf_t * values, int from, int to, int *best)
{
  /* 0 if not all same kind of int, 32 if firt in 32, else 64
   * If very few 64's or lots of repeats in leading bytes, return 0 for a variable length representation if that is better.  Only when all ints, set the best to be the approx byte count.   */
  int inx, r = 32, n_64 = 0, rep, n, avg_len;
  db_buf_t first = (db_buf_t) values[from];
  int is_iri;
  if (cs && cs->cs_all_int && CS_NO_ANY_INT_VEC & cs->cs_exclude)
    {
      int tp = cs->cs_any_64 ? 64 : 32;
      int l = (to - from) * (tp >> 3);
      *best = l + 3;
      return tp;
    }
  *best = 0xfffff;
  if (!IS_INTLIKE_DTP (first[0]))
    return 0;
  is_iri = IS_IRI_DTP (first[0]);
  if (IS_64_DTP (first[0]))
    {
      r = 64;
      n_64++;
    }
  for (inx = from + 1; inx < to; inx++)
    {
      db_buf_t xx = (db_buf_t) values[inx];
      if (!IS_INTLIKE_DTP (xx[0]))
	return 0;
      if (is_iri != IS_IRI_DTP (xx[0]))
	return 0;
      if (IS_64_DTP (xx[0]))
	{
	  r = 64;
	  n_64++;
	}
    }
  n = to - from;
  if (64 == r && (*best = ((CE_VEC_LENGTH_BYTES (n)) + (n - n_64) * 5 + n_64 * 9 < 7 * (n))))
    return 0;			/* few 64's, variable length format saves */
  rep = n - cs_int_high_distinct (values, from, to);
  avg_len = (9 * n_64 + 5 * (n - n_64)) / n;
  if ((*best = CE_VEC_LENGTH_BYTES (n) + 2 * rep + (n - rep) * avg_len) < n * (r / 8))
    return 0;			/* variable len better because of compression */
  *best = n * r / 8;
  return r;
}

char cs_mark[] = { 0xde, 0xad, 0xbe, 0xef };


void
cs_buf_mark (db_buf_t b)
{
  memcpy (b + box_length (b) - 5, cs_mark, 4);
}


void
cs_buf_mark_check (db_buf_t b)
{
  if (b && 0 != memcmp (b + box_length (b) - 5, cs_mark, 4))
    GPF_T1 ("cs buf end mark overwritten");
}


void
cs_length_check (db_buf_t * place, int fill, int bytes)
{
  int l = box_length ((caddr_t) * place) - 5;
  cs_buf_mark_check (*place);
  if (fill > l)
    GPF_T1 ("write [past end of compress buffer, check more often");
  if (fill + bytes >= l)
    {
      db_buf_t n = (db_buf_t) mp_alloc_box_ni (THR_TMP_POOL, l * 2 + bytes + 5, DV_STRING);
      cs_buf_mark (n);
      memcpy (n, *place, fill);
      *place = n;
    }
}


void
pfh_col_init (pf_hash_t * pfh, db_buf_t out)
{
  int last_col = 1;
  pfh->pfh_fill = 0;
  pfh->pfh_page = out;
  pfh->pfh_n_cols = last_col;
  pfh->pfh_kv = PFH_KV_ANY;
  memset (pfh->pfh_start, -1, sizeof (short) * last_col * PFH_N_WAYS);
}

void
cs_write_vec_body (dtp_t * out, int *fill_ret, db_buf_t * values, int n_values, int *order, int is_int, int len_bias)
{
  /* vec body */
  int inx, fill = *fill_ret, org_fill = fill;
  if (64 == is_int)
    {
      for (inx = 0; inx < n_values; inx++)
	{
	  dtp_t *off = out + fill + (8 - len_bias) * inx;
	  int64 n = any_num (values[order ? order[inx] : inx]);
	  INT64_SET_CA (off, n);
	}
      fill += (8 - len_bias) * n_values;
    }
  else if (32 == is_int)
    {
      for (inx = 0; inx < n_values; inx++)
	{
	  dtp_t *off = out + fill + (4 - len_bias) * inx;
	  uint32 n = any_num (values[order ? order[inx] : inx]);
	  LONG_SET_CA (off, n);
	}
      fill += (4 - len_bias) * n_values;
    }
  else
    {
      dbe_col_loc_t cl;
      pf_hash_t pfh;
      int len_fill = fill;
      int end = CE_VEC_LENGTH_BYTES (n_values);
      cl.cl_sqt.sqt_dtp = DV_ANY;
      cl.cl_nth = 0;
      pfh_col_init (&pfh, out + org_fill);
      fill += end;
      for (inx = 0; inx < n_values; inx++)
	{
	  caddr_t xx = (caddr_t) values[order ? order[inx] : inx];
	  int len = box_length (xx) - 1;
	  unsigned short ref;
	  if (len > len_bias)
	    len -= len_bias;
	  if (!order && CC_OFFSET == pfh_var (&pfh, &cl, (db_buf_t) xx, len, NULL, &ref, NULL, CC_LAST_BYTE))
	    {
	      if (ref < MAX_1_BYTE_CE_INX)
		{
		  out[fill++] = ref;
		  out[fill++] = xx[len - 1];
		  len = 2;
		}
	      else
		{
		  out[fill++] = (ref >> 8) + 1 + MAX_1_BYTE_CE_INX;
		  out[fill++] = ref;
		  out[fill++] = xx[len - 1];
		  len = 3;
		}
	    }
	  else
	    {
	      memcpy (out + fill, xx, len);
	      if (!order && fill - org_fill < 32000)
		pfh_set_var (&pfh, &cl, inx, out + fill, len);
	      fill += len;
	    }
	  if (inx >= 2 && 0 == (inx & 1))
	    {
	      SHORT_SET_CA (out + len_fill, end);
	      len_fill += 2;
	    }
	  end += len;
	}
    }
  *fill_ret = fill;
}


void
cs_write_typed_vec_1 (compress_state_t * cs, dtp_t * out, int *fill_ret, int from, int to, int is_int)
{
  db_buf_t *values = (db_buf_t *) cs->cs_values;
  int fill = *fill_ret, org_fill = fill, flags = 0;
  fill += 5;
  cs_write_vec_body (out, &fill, (db_buf_t *) & cs->cs_values[from], to - from, NULL, is_int, 0);
  if (0 == is_int)
    flags |= CET_ANY;
  if (64 == is_int)
    flags |= CE_IS_64;
  if ((32 == is_int || 64 == is_int) && IS_IRI_DTP (values[from][0]))
    flags |= CE_IS_IRI;
  cs_write_header (out + org_fill, CE_VEC | flags, to - from, fill - (org_fill + 5));
  *fill_ret = fill;
}


void
cs_write_typed_vec (compress_state_t * cs, dtp_t * out, int *fill_ret, int from, int to, int is_int, int bytes_guess)
{
  int n_values = to - from;
  int n_ways = (bytes_guess / 2000) + 1;
  int slice = n_values / n_ways, n;
  for (n = 0; n < n_ways; n++)
    {
      int first = n * slice;
      int last = n == n_ways - 1 ? n_values : (n + 1) * slice;
      int best;
      if (n_ways > 1)
	is_int = cs_int_type (cs, (db_buf_t *) cs->cs_values, from + first, from + last, &best);
      cs_write_typed_vec_1 (cs, out, fill_ret, from + first, from + last, is_int);
    }
}


void
cs_write_dict_head (compress_state_t * cs, dtp_t * out, int *fill_ret, int *r_is_int, db_buf_t * values, int n_values, int *order,
    int len_bias)
{
  /* dict value list, sorted */
  int fill = *fill_ret, is_int = 32;
  if (n_values > 255)
    GPF_T1 ("rl or dict ce has max 255 values");
  out[fill++] = n_values;
  is_int = cs->cs_all_int ? (cs->cs_any_64 ? 64 : 32) : 0;
  cs_write_vec_body (out, &fill, values, n_values, order, is_int, 0);
  *r_is_int = is_int;
  *fill_ret = fill;
}


void
cs_123_byte (dtp_t * out, short *p_fill, int b)
{
  short fill = *p_fill;
  if (b > 0xffff)
    GPF_T1 ("123 format is not for over 654K");
  if (b < 0xf0)
    {
      out[fill] = b;
      (*p_fill)++;
    }
  else if (b < 0xeff)
    {
      out[fill] = 0xf0 | b >> 8;
      out[fill + 1] = (dtp_t) b;
      (*p_fill) += 2;
    }
  else
    {
      out[fill] = 0xff;
      out[fill + 1] = b >> 8;
      out[fill + 2] = (dtp_t) b;
      (*p_fill) += 3;
    }
}

void
cs_write_gap (db_buf_t out, int bytes)
{
  if (0 == bytes)
    return;
  if (1 == bytes)
    *out = CE_GAP_1;
  else if (bytes < 256)
    {
      out[0] = CE_SHORT_GAP;
      out[1] = bytes - 2;
    }
  else
    {
      out[0] = CE_GAP;
      SHORT_SET_CA (out + 1, bytes - 3);
    }
}



dtp_t *
cs_write_header (dtp_t * out, int flags, int n_values, int n_bytes)
{
  /* the data starts at out + 5.  Write a 3 or 5 byte header and return the header */
  int fill = 0;
  cs_append_header (out, &fill, flags, n_values, n_bytes);
  if (5 == fill)
    return out + 5;
  if (3 == fill)
    {
      memmove (out + 2, out, 3);
      cs_write_gap (out, 2);
      return out + 2;
    }
  if (2 == fill)
    {
      memmove (out + 3, out, 2);
      cs_write_gap (out, 3);
      return out + 3;
    }
  else
    GPF_T1 ("header is either 5, 3 or 2 bytes");
  return 0;
}


dtp_t
any_norm_dtp (dtp_t * a)
{
  switch (*a)
    {
    case DV_SHORT_INT:
      return DV_LONG_INT;
    case DV_INT64:
      return DV_LONG_INT;
    case DV_IRI_ID_8:
      return DV_IRI_ID;
    default:
      return *a;
    }
}


int
ce_1_len (dtp_t * ce, dtp_t flags)
{
  int bytes = 0;
  if (CET_ANY == (flags & CE_DTP_MASK))
    {
      DB_BUF_TLEN (bytes, ce[0], ce);
    }
  else if (CET_CHARS == (flags & CE_DTP_MASK))
    {
      bytes = (ce)[0];
      if (bytes > 127)
	bytes = 2 + (bytes & 0x7f) + 128 * ce[1];
      else
	bytes++;
    }
  else if (CE_INTLIKE (flags))
    bytes = (CE_IS_64 & flags) ? 8 : 4;
  else
    GPF_T1 ("ce data type unclear");
  return bytes;
}

void
ce_head_info (db_buf_t ce, int *r_bytes, int *r_values, dtp_t * r_ce_type, dtp_t * r_flags, int *r_hl)
{
  dtp_t flags, ce_type;
  int n_bytes, n_values, hl, is_null;
  flags = ce[0];
  ce_type = flags & CE_TYPE_MASK;
  if (ce_type < CE_BITS)
    {
      if (ce_type <= CE_RL)
	{
	  is_null = CET_NULL == (flags & CE_DTP_MASK);
	  if (is_null)
	    {
	      hl = (CE_IS_SHORT & flags) ? 2 : 3;
	      n_values = (CE_IS_SHORT & flags) ? ce[1] : SHORT_REF_CA (ce + 1);
	      n_bytes = 0;
	    }
	  else if ((CE_IS_SHORT & flags))
	    {
	      n_values = ce[1];
	      hl = 2;
	    }
	  else
	    {
	      n_values = SHORT_REF_CA (ce + 1);
	      hl = 3;
	    }
	  n_bytes = ce_1_len (ce + hl, flags);
	}
      else
	{
	  if (CE_GAP == ce_type)
	    {
	      n_values = 0;
	      n_bytes = CE_GAP_LENGTH (ce, flags);
	      hl = 0;
	    }
	  else if (CE_VEC == ce_type)
	    {
	      n_values = (CE_IS_SHORT & flags) ? ce[1] : SHORT_REF_CA (ce + 1);
	      if (CE_INTLIKE (flags))
		{
		  n_bytes = n_values * ((flags & CE_IS_64) ? 8 : 4);
		  hl = (CE_IS_SHORT & flags) ? 2 : 3;
		}
	      else
		{
		  n_bytes = n_values;
		  n_values = (CE_IS_SHORT & flags) ? ce[2] : SHORT_REF_CA (ce + 3);
		  hl = (CE_IS_SHORT & flags) ? 3 : 5;
		}
	    }
	}
    }
  else
    {
      if ((CE_IS_SHORT & flags))
	{
	  hl = 3;
	  n_bytes = ce[1];
	  n_values = ce[2];
	}
      else
	{
	  hl = 5;
	  n_bytes = SHORT_REF_CA (ce + 1);
	  n_values = SHORT_REF_CA (ce + 3);
	}
    }
  *r_bytes = n_bytes;
  *r_values = n_values;
  *r_ce_type = ce_type;
  *r_flags = flags;
  *r_hl = hl;
}




#define CE_OUT(val, len, n, rl)			\
  target = cpo->cpo_value_cb (cpo, last_row, flags, val, len, n, rl);


extern unsigned char byte_logcount[256];


void
ce_skip_bits (db_buf_t bits, int skip, int *byte_ret, int *bit_ret)
{
  /* count skip one bits forward from byte/bit and return the byte/bit in byte/bit */
  int byte = *byte_ret, bit = *bit_ret;
  int n_ones = 0;
  dtp_t init_mask = 0xff;
  if (bit)
    init_mask = init_mask << bit;	/* ignore the bit lowest bits of first byte */
  for (;;)
    {
      n_ones = byte_logcount[init_mask & bits[byte]];
      if (n_ones > skip)
	{
	  /* the targeted bit is the n_ones - skip 1 bit  of the byte */
	  char bit2, n = 0;
	  dtp_t b = bits[byte] & init_mask;
	  for (bit2 = bit; bit2 < 8; bit2++)
	    {
	      if (b & (1 << bit2))
		{
		  n++;
		  if (n > skip)
		    {
		      *byte_ret = byte;
		      *bit_ret = bit2;
		      return;
		    }
		}
	    }
	}
      skip -= n_ones;
      init_mask = 0xff;
      bit = 0;
      byte++;
      if (skip > 64)
	{
	  int n = byte_logcount[bits[byte]]
	      + byte_logcount[bits[byte + 1]]
	      + byte_logcount[bits[byte + 2]]
	      + byte_logcount[bits[byte + 3]]
	      + byte_logcount[bits[byte + 4]]
	      + byte_logcount[bits[byte + 5]] + byte_logcount[bits[byte + 6]] + byte_logcount[bits[byte + 7]];
	  skip -= n;
	  byte += 8;
	}
    }
}

db_buf_t
ce_any_dict_array (db_buf_t ce, dtp_t flags)
{
  /* return the first byte of the content array of a dict ce with variable lengths */
  dtp_t n_distinct = ce[0];
  int len;
  db_buf_t ptr;
  ce_vec_nth (ce + 1, flags, n_distinct, n_distinct - 1, &ptr, &len, 0);
  return ptr + len;
}

int
ce_vec_item_len (db_buf_t it, dtp_t flags)
{
  uint32 l;
  if (CET_CHARS == (flags & CE_DTP_MASK))
    {
      l = it[0];
      if (l > 127)
	{
	  l = (l - 128) * 256 + it[1];
	  return l + 2;
	}
      return l + 1;
    }
  if (it[0] < DV_ANY_FIRST)
    return it[0] <= MAX_1_BYTE_CE_INX ? 2 : 3;
  DB_BUF_TLEN (l, it[0], it);
  return l;
}


void
ce_vec_nth (db_buf_t ce, dtp_t flags, int n_values, int inx, db_buf_t * val_ret, int *len_ret, int len_bias)
{
  db_buf_t val;
  int mod, pos, ctr;
  int len;
  CE_VEC_POS (ce, n_values, inx, pos, mod);
  val = ce + pos;
  len = ce_vec_item_len (val, flags);
  for (ctr = 0; ctr < mod; ctr++)
    {
      val += len - len_bias;
      len = ce_vec_item_len (val, flags);
    }
  *val_ret = val;
  *len_ret = len;
}


int
ce_cmp_1 (col_pos_t * cpo, int row, dtp_t flags, db_buf_t val, int len, int64 offset, int rl)
{
  /* for integers, offset is the number.  For anys val is the 1st byte of the any and offset is the tail offset */
  cpo->cpo_rc = ce_col_cmp (val, offset, flags, cpo->cpo_cl, cpo->cpo_cmp_min);
  if (DVC_GREATER == cpo->cpo_rc && CE_INTLIKE (flags))
    cpo->cpo_itc->itc_last_cmp_value = offset;
  return COL_NO_ROW;
}


int
ce_filter (col_pos_t * cpo, int row, dtp_t flags, db_buf_t val, int len, int64 offset, int rl)
{
  /* for integers, offset is the number.  For anys val is the 1st byte of the any and offset is the tail offset */
  int ctr;
  int rc, next;
  it_cursor_t *itc = cpo->cpo_itc;
#if 0
  if (itc->itc_n_matches && row != (next = itc->itc_matches[itc->itc_match_in]))
    GPF_T1 ("ce_filter called with row no out of whack");
#endif
  switch (cpo->cpo_min_op)
    {
    case CMP_NONE:
      break;
    case CMP_LIKE:
      if (CE_INTLIKE (flags))
	{
	  dtp_t dtp = CE_IS_IRI & flags ? DV_IRI_ID : DV_LONG_INT;
	  if (dtp != (dtp_t) cpo->cpo_cmp_min[3])
	    goto filter_done;
	}
      else if (DVC_MATCH != ce_like_filter (cpo, row, flags, val, len, offset, rl))
	goto filter_done;
      break;
    case CMP_HASH_RANGE:
      {
	int rl2;
	/* one hit may make many results if the hash extracts values, so must know how many */
	if (itc->itc_n_matches)
	  {
	    int match_in = itc->itc_match_in, next;
	    rl2 = 1;
	    for (;;)
	      {
		if (++match_in == itc->itc_n_matches)
		  break;
		next = itc->itc_matches[match_in];
		if (next < rl + row)
		  rl2++;
		else
		  break;
	      }
	  }
	else
	  rl2 = rl;
	if (!ce_int_chash_check (cpo, val, flags, offset, rl2))
	  goto filter_done;
	goto found;
      }
    default:
      rc = ce_col_cmp (val, offset, flags, cpo->cpo_cl, cpo->cpo_cmp_min);
      if (!(rc & cpo->cpo_min_op))
	goto filter_done;
    }

  if (cpo->cpo_max_op)
    {
      rc = ce_col_cmp (val, offset, flags, cpo->cpo_cl, cpo->cpo_cmp_max);
      if (!(rc & cpo->cpo_max_op))
	goto filter_done;
    }
found:
  itc->itc_matches[itc->itc_match_out++] = row;
  if (itc->itc_n_matches)
    {
      for (;;)
	{
	  if (++itc->itc_match_in == itc->itc_n_matches)
	    return CE_AT_END;
	  next = itc->itc_matches[itc->itc_match_in];
	  if (next < rl + row)
	    {
	      itc->itc_matches[itc->itc_match_out++] = next;
	    }
	  else
	    {
	      return next;
	    }
	}
    }
  else
    {
      for (ctr = 1; ctr < rl; ctr++)
	itc->itc_matches[itc->itc_match_out++] = row + ctr;
      return row + rl;
    }


filter_done:
  if (itc->itc_n_matches)
    {
      for (;;)
	{
	  if (++itc->itc_match_in == itc->itc_n_matches)
	    return CE_AT_END;
	  if (itc->itc_matches[itc->itc_match_in] >= row + rl)
	    return itc->itc_matches[itc->itc_match_in];
	}
    }
  return row + rl;
}


caddr_t
mp_box_deserialize_ce_string (mem_pool_t * mp, db_buf_t dv, int len, int64 offset)
{
  if (DV_DATETIME == *dv && offset)
    {
      int day;
      db_buf_t dt;
      if (mp)
	dt = (db_buf_t) mp_box_deserialize_string (mp, (caddr_t) dv, len, 0);
      else
	dt = (db_buf_t) box_deserialize_string ((caddr_t) dv, len, 0);
      day = DT_DAY (dt);
      DT_SET_DAY (dt, (day + offset));
      return (caddr_t) dt;
    }
  if (mp)
    return mp_box_deserialize_string (mp, (caddr_t) dv, len, offset);
  return box_deserialize_string ((caddr_t) dv, len, offset);
}

caddr_t
mp_box_n_chars (mem_pool_t * mp, caddr_t b, int l)
{
  caddr_t str = mp_alloc_box_ni (mp, l + 1, DV_STRING);
  memcpy (str, b, l);
  str[l] = 0;
  return str;
}

#define ce_res_bytes(dc, b, n) \
  if (DCT_FROM_POOL & dc->dc_type) ((caddr_t*)dc->dc_values)[dc->dc_n_values++] = mp_box_n_chars (dc->dc_mp, (caddr_t)b, n); \
 else dc_append_bytes (dc, b, n, NULL, 0);

int enable_ce_result_cvt = 1;


int
ce_result (col_pos_t * cpo, int row, dtp_t flags, db_buf_t val, int len, int64 offset, int rl)
{
  int ctr;
  int next, target;
  it_cursor_t *itc = cpo->cpo_itc;
  data_col_t *dc = cpo->cpo_dc;
#if 0
  if (itc->itc_n_matches && row != (next = itc->itc_matches[itc->itc_match_in]))
    GPF_T1 ("ce result getting row no out of whack");
#endif
  if (itc->itc_n_matches)
    {
      int ctr = 1;
      for (;;)
	{
	  if (++itc->itc_match_in == itc->itc_n_matches)
	    {
	      target = CE_AT_END;
	      break;
	    }
	  next = itc->itc_matches[itc->itc_match_in];
	  if (next >= row + rl)
	    {
	      target = next;
	      break;
	    }
	  ctr++;
	}
      rl = ctr;
    }
  else
    target = row + rl;

  if (0 == dc->dc_n_values)
    {
      if (enable_ce_result_cvt && DV_ANY == dc->dc_sqt.sqt_col_dtp && 0 == (DCT_BOXES & dc->dc_type))
	{
	  dtp_t ce_dtp = flags & CE_DTP_MASK;
	  dtp_t val_dtp;
	  val_dtp = CET_ANY == ce_dtp ? dtp_canonical[*val]
	      : CE_INTLIKE (ce_dtp) ? ((CE_IS_IRI & ce_dtp) ? DV_IRI_ID : DV_LONG_INT) : DV_ANY;
	  dc_convert_empty (dc, dv_ce_dtp[val_dtp]);
	}
    }
  else if (DV_ANY == dc->dc_sqt.sqt_col_dtp && DV_ANY != dc->dc_dtp)
    {
      dtp_t ce_dtp = flags & CE_DTP_MASK;
      if (CET_ANY == ce_dtp)
	{
	  if (dtp_canonical[*val] != dc->dc_dtp)
	    dc_heterogenous (dc);
	}
      else if (CE_INTLIKE (flags)
	  && (((CE_IS_IRI & flags) && DV_IRI_ID != dc->dc_dtp) || (!(CE_IS_IRI & flags) && DV_LONG_INT != dc->dc_dtp)))
	dc_heterogenous (dc);
    }
  if (CE_INTLIKE (flags))
    {
      if (DCT_NUM_INLINE & dc->dc_type)
	{
	  if (DV_SINGLE_FLOAT == dc->dc_sqt.sqt_dtp)
	    {
	      for (ctr = 0; ctr < rl; ctr++)
		((int32 *) dc->dc_values)[dc->dc_n_values++] = offset;
	    }
	  else
	    {
	      for (ctr = 0; ctr < rl; ctr++)
		((int64 *) dc->dc_values)[dc->dc_n_values++] = offset;
	    }
	}
      else if (DV_ANY == dc->dc_dtp)
	{
	  dtp_t tmp[10];
	  if (CE_IS_IRI & flags)
	    {
	      if ((iri_id_t) offset > 0xffffffff)
		{
		  tmp[0] = DV_IRI_ID_8;
		  INT64_SET_NA (&tmp[1], offset);
		  ce_res_bytes (dc, tmp, 9);
		}
	      else
		{
		  tmp[0] = DV_IRI_ID;
		  LONG_SET_NA (&tmp[1], offset);
		  ce_res_bytes (dc, tmp, 5);
		}
	    }
	  else
	    {
	      if ((offset > -128) && (offset < 128))
		{
		  tmp[0] = DV_SHORT_INT;
		  tmp[1] = offset;
		  ce_res_bytes (dc, tmp, 2);
		}
	      else if (offset < INT32_MIN || offset > INT32_MAX)
		{
		  tmp[0] = DV_INT64;
		  INT64_SET_NA (&tmp[1], offset);
		  ce_res_bytes (dc, tmp, 9);
		}
	      else
		{
		  tmp[0] = DV_LONG_INT;
		  LONG_SET_NA (&tmp[1], offset);
		  ce_res_bytes (dc, tmp, 5);
		}
	    }
	  for (ctr = 1; ctr < rl; ctr++)
	    {
	      ((caddr_t *) dc->dc_values)[dc->dc_n_values] = ((caddr_t *) dc->dc_values)[dc->dc_n_values - 1];
	      dc->dc_n_values++;
	    }
	}
      else if (DCT_BOXES & dc->dc_type)
	{
	  dtp_t dtp = CE_IS_IRI & flags ? DV_IRI_ID : DV_LONG_INT;
	  for (ctr = 0; ctr < rl; ctr++)
	    {
	      caddr_t box =
		  DCT_FROM_POOL & dc->dc_type ? mp_alloc_box (dc->dc_mp, sizeof (int64), dtp) : dk_alloc_box (sizeof (int64), dtp);
	      *(int64 *) box = offset;
	      ((caddr_t *) dc->dc_values)[dc->dc_n_values++] = box;
	    }
	}
      else
	GPF_T1 ("dc not suitable for cs decode");
    }
  else if (CET_ANY == (flags & CE_DTP_MASK))
    {
      if (DCT_NUM_INLINE & dc->dc_type)
	{
	  int64 n = any_num_f (val) + offset;
	  if (DV_SINGLE_FLOAT == dc->dc_sqt.sqt_dtp)
	    {
	      for (ctr = 0; ctr < rl; ctr++)
		((int32 *) dc->dc_values)[dc->dc_n_values++] = n;
	    }
	  else
	    {
	      for (ctr = 0; ctr < rl; ctr++)
		((int64 *) dc->dc_values)[dc->dc_n_values++] = n;
	    }
	}
      else if (DCT_BOXES & dc->dc_type)
	{
	  caddr_t box = DCT_FROM_POOL & dc->dc_type
	      ? mp_box_deserialize_ce_string (dc->dc_mp, val, len, offset) : mp_box_deserialize_ce_string (NULL, val, len, offset);
	  ((caddr_t *) dc->dc_values)[dc->dc_n_values++] = box;
	  for (ctr = 1; ctr < rl; ctr++)
	    {
	      if (0 == (DCT_FROM_POOL & dc->dc_type))
		box = box_copy_tree (box);
	      ((caddr_t *) dc->dc_values)[dc->dc_n_values++] = box;
	    }
	}
      else if (DV_ANY == dc->dc_dtp)
	{
	  db_buf_t ptr;
	  if (DCT_FROM_POOL & dc->dc_type)
	    ((caddr_t *) dc->dc_values)[dc->dc_n_values++] = mp_alloc_box (dc->dc_mp, len + 1, DV_STRING);
	  else
	    dc_reserve_bytes (dc, len);
	  ptr = ((db_buf_t *) dc->dc_values)[dc->dc_n_values - 1];
	  any_add (val, len, offset, ptr, flags);
	  for (ctr = 1; ctr < rl; ctr++)
	    {
	      ((caddr_t *) dc->dc_values)[dc->dc_n_values] = ((caddr_t *) dc->dc_values)[dc->dc_n_values - 1];
	      dc->dc_n_values++;
	    }
	}
      else if (DV_DATETIME == dc->dc_sqt.sqt_dtp || DV_DATE == dc->dc_sqt.sqt_dtp || DV_TIME == dc->dc_sqt.sqt_dtp)
	{
	  dtp_t tmp[DT_LENGTH + 1];
	  db_buf_t ptr = dc->dc_values + DT_LENGTH * dc->dc_n_values;
	  any_add (val, len, offset, tmp, flags);
	  memcpy (ptr, &tmp[1], DT_LENGTH);
	  dc->dc_n_values++;
	  for (ctr = 1; ctr < rl; ctr++)
	    {
	      memcpy (dc->dc_values + dc->dc_n_values * DT_LENGTH, ptr, DT_LENGTH);
	      dc->dc_n_values++;
	    }
	}
    }
  else if (CET_CHARS == (flags & CE_DTP_MASK))
    {
      if (DCT_BOXES & dc->dc_type)
	{
	  uint32 l;
	  caddr_t box = (DCT_FROM_POOL & dc->dc_type) ? mp_alloc_box (dc->dc_mp, len + 1, DV_STRING)
	      : dk_alloc_box (len + 1, DV_STRING);
	  memcpy (box, val + (len > 127 ? 2 : 1), len);
	  if (offset)
	    {
	      l = LONG_REF_NA (box + len - 4);
	      l += offset;
	      LONG_SET_NA (box + len - 4, l);
	    }
	  box[len] = 0;
	  ((caddr_t *) dc->dc_values)[dc->dc_n_values++] = box;
	  for (ctr = 1; ctr < rl; ctr++)
	    {
	      if (0 == (DCT_FROM_POOL & dc->dc_type))
		box = box_copy_tree (box);
	      ((caddr_t *) dc->dc_values)[dc->dc_n_values++] = box;
	    }
	}
      else if (DV_ANY == dc->dc_sqt.sqt_dtp)
	{
	  int hl = len > 255 ? 5 : 2;
	  db_buf_t ptr;
	  if (DCT_FROM_POOL & dc->dc_type)
	    ((caddr_t *) dc->dc_values)[dc->dc_n_values++] = mp_alloc_box (dc->dc_mp, len + hl + 1, DV_STRING);
	  else
	    dc_reserve_bytes (dc, len + hl);
	  ptr = ((db_buf_t *) dc->dc_values)[dc->dc_n_values - 1];
	  any_add (val, len, offset, ptr, flags);
	  for (ctr = 1; ctr < rl; ctr++)
	    {
	      ((caddr_t *) dc->dc_values)[dc->dc_n_values] = ((caddr_t *) dc->dc_values)[dc->dc_n_values - 1];
	      dc->dc_n_values++;
	    }
	}
      else
	GPF_T1 ("bad dc flags for cet chars ce result");
    }
  return target;
}


int
cpo_next_pre (col_pos_t * cpo, int is_first)
{
  it_cursor_t *itc = cpo->cpo_itc;
  row_lock_t *rl = itc->itc_rl;
  int n_significant = itc->itc_insert_key->key_n_significant;
  int inx;
  if (!is_first)
    cpo->cpo_clk_inx++;
  for (inx = cpo->cpo_clk_inx; inx < rl->rl_n_cols; inx++)
    {
      col_row_lock_t *clk = rl->rl_cols[inx];
      if (clk->clk_change & CLK_REVERT_AT_ROLLBACK
	  && clk->pl_owner != itc->itc_ltrx && clk->clk_rbe[cpo->cpo_cl->cl_nth - n_significant])
	break;
    }
  if (inx >= rl->rl_n_cols)
    {
      cpo->cpo_value_cb = cpo->cpo_dc ? ce_result : ce_filter;
      cpo->cpo_next_pre = COL_NO_ROW;
    }
  else
    {
      cpo->cpo_clk_inx = inx;
      cpo->cpo_next_pre = rl->rl_cols[inx]->clk_pos;
    }
}


int
ce_preimage (col_pos_t * cpo, int row, dtp_t flags, db_buf_t val, int len, int64 offset, int rl)
{
  it_cursor_t *itc = cpo->cpo_itc;
  row_lock_t *rlock = itc->itc_rl;
  col_row_lock_t *clk;
  int r, pre_len;
  if (row <= cpo->cpo_next_pre && row + rl > cpo->cpo_next_pre)
    {
      int n_significant = itc->itc_insert_key->key_n_significant;
      if (!itc->itc_n_matches)
	{
	  for (r = row; r < row + rl; r++)
	    {
	      if (r == cpo->cpo_next_pre)
		{
		  db_buf_t pre;
		  clk = rlock->rl_cols[cpo->cpo_clk_inx];
		  pre = ((db_buf_t *) clk->clk_rbe)[cpo->cpo_cl->cl_nth - n_significant];
		  DB_BUF_TLEN (pre_len, pre[0], pre);
		  (cpo->cpo_dc ? ce_result : ce_filter) (cpo, r, CE_VEC | CET_ANY, pre, pre_len, 0, 1);
		  cpo_next_pre (cpo, 0);
		}
	      else
		(cpo->cpo_dc ? ce_result : ce_filter) (cpo, r, flags, val, len, offset, 1);
	    }
	  return row + rl;
	}
      else
	{
	  int next;
	  for (;;)
	    {
	      int r = itc->itc_matches[itc->itc_match_in];
	      if (r == cpo->cpo_next_pre)
		{
		  db_buf_t pre;
		  clk = rlock->rl_cols[cpo->cpo_clk_inx];
		  pre = ((db_buf_t *) clk->clk_rbe)[cpo->cpo_cl->cl_nth - n_significant];
		  DB_BUF_TLEN (pre_len, pre[0], pre);
		  next = (cpo->cpo_dc ? ce_result : ce_filter) (cpo, r, CE_VEC | CET_ANY, pre, pre_len, 0, 1);
		  cpo_next_pre (cpo, 0);
		}
	      else
		next = (cpo->cpo_dc ? ce_result : ce_filter) (cpo, r, flags, val, len, offset, 1);
	      if (next >= row + rl)
		return next;
	    }
	}
    }
  else
    return (cpo->cpo_dc ? ce_result : ce_filter) (cpo, r, flags, val, len, offset, 1);
}


int
cs_preimage_init (col_pos_t * cpo, int from)
{
  it_cursor_t *itc = cpo->cpo_itc;
  row_no_t ign = 0;
  itc_clk_at (itc, from, &cpo->cpo_clk_inx, &ign);
  cpo_next_pre (cpo, 1);
  if (COL_NO_ROW != cpo->cpo_next_pre)
    cpo->cpo_value_cb = ce_preimage;
}


int64 ce_gen_touch[256];
int64 ce_gen_rows[256];


int enable_ce_inline = 1;

int
cs_decode (col_pos_t * cpo, int from, int to)
{
  /* the string has ce's possibly with gaps in bytes.  Get values in the range from from to to, if mask is given, take only values with a 1 bit in the mask */
  db_buf_t first_ce = cpo->cpo_string;
  int str_bytes = cpo->cpo_bytes;
  db_buf_t ce = first_ce;
  int last_row = cpo->cpo_ce_row_no, target, ce_row;
  int init_pm_pos, pm_pos;
  page_map_t *pm = NULL;
  cpo->cpo_to = to;
  if (cpo->cpo_itc && cpo->cpo_itc->itc_col_need_preimage)
    {
      cpo->cpo_ce_op = NULL;
      if (ce_filter == cpo->cpo_value_cb)
	cpo->cpo_dc = NULL;
      cs_preimage_init (cpo, from);
    }
  if (!enable_ce_inline)
    cpo->cpo_ce_op = NULL;
new_ce:
  while (ce < first_ce + str_bytes)
    {
      dtp_t flags, ce_type, is_null;
      unsigned short n_bytes, n_values;
      dtp_t *ce_first, *ce_first_val;
      dtp_t *ce_end;
      int64 first;
      ce_op_t ce_op;
      int skip = 0, hl, first_len;

      {
	flags = ce[0];
	ce_type = flags & CE_TYPE_MASK;
	if (ce_type < CE_BITS)
	  {
	    if (ce_type <= CE_RL)
	      {
		is_null = CET_NULL == (flags & CE_DTP_MASK);
		if (is_null)
		  {
		    hl = (CE_IS_SHORT & flags) ? 2 : 3;
		    n_values = (CE_IS_SHORT & flags) ? ce[1] : SHORT_REF_CA (ce + 1);
		    n_bytes = 0;
		  }
		else if ((CE_IS_SHORT & flags))
		  {
		    n_values = ce[1];
		    hl = 2;
		  }
		else
		  {
		    n_values = SHORT_REF_CA (ce + 1);
		    hl = 3;
		  }
		n_bytes = ce_1_len (ce + hl, flags);
	      }
	    else
	      {
		if (CE_GAP == ce_type)
		  {
		    ce_gen_touch[flags]++;
		    n_bytes = CE_GAP_LENGTH (ce, flags);
		    ce += n_bytes;
		    continue;
		  }
		else if (CE_VEC == ce_type)
		  {
		    n_values = (CE_IS_SHORT & flags) ? ce[1] : SHORT_REF_CA (ce + 1);
		    if (CE_INTLIKE (flags))
		      {
			n_bytes = n_values * ((flags & CE_IS_64) ? 8 : 4);
			hl = (CE_IS_SHORT & flags) ? 2 : 3;
		      }
		    else
		      {
			n_bytes = n_values;
			n_values = (CE_IS_SHORT & flags) ? ce[2] : SHORT_REF_CA (ce + 3);
			hl = (CE_IS_SHORT & flags) ? 3 : 5;
		      }
		  }
	      }
	  }
	else
	  {
	    if ((CE_IS_SHORT & flags))
	      {
		hl = 3;
		n_bytes = ce[1];
		n_values = ce[2];
	      }
	    else
	      {
		hl = 5;
		n_bytes = SHORT_REF_CA (ce + 1);
		n_values = SHORT_REF_CA (ce + 3);
	      }
	  }
      }

      if (n_values + last_row <= from)
	{
	  ce_gen_touch[0xff]++;
	  last_row += n_values;
	  ce += n_bytes + hl;
	  continue;
	}
      if (last_row <= from)
	skip = from - last_row;
      ce_row = last_row;
      ce_first = ce + hl;
      if (cpo->cpo_ce_op && (ce_op = cpo->cpo_ce_op[flags & ~CE_IS_SHORT]))
	{
	  int res;
	  cpo->cpo_skip = skip;
	  cpo->cpo_ce_row_no = last_row;
	  cpo->cpo_ce = ce;
	  res = ce_op (cpo, ce_first, n_values, n_bytes);
	  if (res >= to)
	    return res;
	  if (res)
	    {
	      from = res;
	      if (cpo->cpo_pm)
		goto next_from_pm;
	      last_row = ce_row + n_values;
	      cpo->cpo_ce_row_no = last_row;
	      skip = 0;
	      ce += n_bytes + hl;
	      continue;
	    }
	}
      ce_gen_touch[flags]++;
      switch (ce_type)
	{
	case CE_GAP:
	  break;
	case CE_RL:
	  {
	    int from_here = MIN (n_values, (to - last_row)) - skip;
	    CE_FIRST;
	    last_row += skip;
	    CE_OUT (ce_first_val, first_len, first, from_here);
	    if (target >= to)
	      return target;
	    if (target >= ce_row + n_values)
	      {
		from = target;
		break;
	      }
	    last_row = target;
	    break;
	  }
	case CE_RL_DELTA:
	  {
	    dtp_t *ce_end = ce + hl + n_bytes;
	    int from_this;
	    CE_FIRST;
	    for (ce_first = ce_first; ce_first < ce_end; ce_first++)
	      {
		dtp_t byte = *ce_first;
		dtp_t delta = byte >> 4;
		dtp_t rl = byte & 0xf;
		if (!rl)
		  {
		    first += delta;
		    continue;
		  }
		if (rl <= skip)
		  {
		    skip -= rl;
		    first += delta;
		    last_row += rl;
		    continue;
		  }
		from_this = rl - skip;
		first += delta;
		if (to <= last_row + rl)
		  {
		    from_this = to - skip - last_row;
		    last_row += skip;
		    CE_OUT (ce_first_val, first_len, first, from_this);
		    return target;
		  }
		target = cpo->cpo_value_cb (cpo, last_row + skip, flags, ce_first_val, first_len, first, from_this);
		skip = 0;
		last_row += rl;
		if (target >= to)
		  return target;
		if (target >= ce_row + n_values)
		  {
		    from = target;
		    break;
		  }
		skip = target - last_row;
	      }
	    break;
	  }
	case CE_VEC:
	  {
	    int inx;
	    int last = MIN (n_values, to - last_row);
	    if (CET_ANY == (flags & CE_DTP_MASK))
	      {
	      any_vec_skip:
		last_row += skip;
		ce_vec_nth (ce_first, flags, n_values, last_row - ce_row, &ce_first_val, &first_len, 0);
		if (ce_first_val[0] < DV_ANY_FIRST)
		  {
		    dtp_t off;
		    short inx = ce_first_val[0] <= MAX_1_BYTE_CE_INX ? (off = ce_first_val[1], ce_first_val[0])
			: (off = ce_first_val[2], (ce_first_val[0] - MAX_1_BYTE_CE_INX - 1) * 256 + ce_first_val[1]);
		    db_buf_t org;
		    int org_len;
		    ce_vec_nth (ce_first, flags, n_values, inx, &org, &org_len, 0);
		    CE_OUT (org, org_len, off - org[org_len - 1], 1);
		  }
		else
		  CE_OUT (ce_first_val, first_len, 0, 1);
		if (target >= to)
		  return target;
		if (target >= ce_row + n_values)
		  {
		    from = target;
		    break;
		  }
		if (target > last_row + 1)
		  {
		    skip = target - last_row;
		    goto any_vec_skip;
		  }
		last_row++;
		for (inx = last_row - ce_row; inx < last; inx++)
		  {
		    ce_first_val += first_len;
		    if (ce_first_val[0] < DV_ANY_FIRST)
		      {
			dtp_t off;
			short inx = ce_first_val[0] <= MAX_1_BYTE_CE_INX ? (first_len = 2, off = ce_first_val[1], ce_first_val[0])
			    : (first_len = 3, off =
			    ce_first_val[2], (ce_first_val[0] - MAX_1_BYTE_CE_INX - 1) * 256 + ce_first_val[1]);
			db_buf_t org;
			int org_len;
			ce_vec_nth (ce_first, flags, n_values, inx, &org, &org_len, 0);
			CE_OUT (org, org_len, off - org[org_len - 1], 1);
		      }
		    else
		      {
			first_len = ce_vec_item_len (ce_first_val, flags);
			CE_OUT (ce_first_val, first_len, 0, 1);
		      }
		    if (target >= to)
		      return target;
		    if (target >= ce_row + n_values)
		      {
			from = target;
			break;
		      }
		    if (target > last_row + 1)
		      {
			skip = target - last_row;
			goto any_vec_skip;
		      }
		    last_row++;
		  }
	      }
	    else if (CE_IS_64 & flags)
	      {
		last_row += skip;
		for (inx = skip; inx < last;)
		  {
		    int64 n = INT64_REF_CA ((ce_first + (8 * inx)));
		    CE_OUT (NULL, 0, n, 1);
		    if (target >= to)
		      return target;
		    if (target >= ce_row + n_values)
		      {
			from = target;
			break;
		      }
		    last_row = target;
		    inx = target - ce_row;
		  }
	      }
	    else
	      {
		last_row += skip;
		for (inx = skip; inx < last;)
		  {
		    int64 n = LONG_REF_CA ((ce_first + (4 * inx)));
		    if (CE_IS_IRI & flags)
		      n &= 0xffffffff;
		    CE_OUT (NULL, 0, n, 1);
		    if (target >= to)
		      return target;
		    if (target >= ce_row + n_values)
		      {
			from = target;
			break;
		      }
		    last_row = target;
		    inx = target - ce_row;
		  }
	      }
	    break;
	  }
	case CE_INT_DELTA:
	  {
	    int64 base, base_1;
	    uint32 d, run, run1;
	    CE_FIRST;
	    if (!CE_INTLIKE (flags))
	      {
		if (DV_DATE == any_ce_dtp (ce_first_val))
		  {
		    base = DT_DAY (ce_first_val + 1);
		    run1 = run = base & 0xff;
		    base_1 = base = (base & CLEAR_LOW_BYTE) - base + run1;
		  }
		else
		  {
		    first = LONG_REF_NA (ce_first - 4);
		    base = base_1 = 0;
		    run1 = run = first & 0xff;
		  }
	      }
	    else
	      {
		base_1 = base = first & CLEAR_LOW_BYTE;
		run1 = run = first & 0xff;
	      }
	    ce_end = ce + hl + n_bytes;
	    while (ce_first < ce_end)
	      {
		if (!run)
		  ;
		else if (skip >= run)
		  {
		    skip -= run;
		    last_row += run;
		  }
		else
		  {
		    /* get values from this run */
		    int start_of_run = last_row;
		    int last = MIN (run, to - last_row), inx;
		    int skip2 = skip;
		    last_row += skip;
		    skip = 0;
		    for (inx = skip2; inx < last; inx++)
		      {
			uint32 n = SHORT_REF_CA (ce_first + 2 * inx);
			if (!CE_INTLIKE (flags))
			  n -= run1;	/* the 1st run length is the last byte of the base val so compensate */
			CE_OUT (ce_first_val, first_len, base + n, 1);
			if (target >= to)
			  return target;
			if (target > ce_row + n_values)
			  {
			    from = target;
			    goto after_int_delta;
			  }
			last_row++;
			if (target > last_row)
			  {
			    if (target - last_row < last - inx)
			      {
				int fwd = target - last_row;
				inx += fwd;
				last_row += fwd;
				continue;
			      }
			    skip = target - start_of_run - run;
			    last_row = start_of_run + run;
			    break;
			  }
		      }
		  }
		ce_first += run * 2;
		if (ce_first >= ce_end)
		  break;
		d = LONG_REF_NA (ce_first);
		ce_first += 4;
		run = d & 0xff;
		base = base_1 + (d & CLEAR_LOW_BYTE);
	      }
	  after_int_delta:
	    break;
	  }
	case CE_BITS:
	  {
	    int byte = 0, bit = 0;
	    CE_FIRST;
	    if (0 == skip)
	      {
		CE_OUT (ce_first_val, first_len, first, 1);
		last_row++;
		if (target >= to)
		  return target;
		if (target >= ce_row + n_values)
		  {
		    from = target;
		    break;
		  }
		skip = target - last_row;
		byte = bit = 0;
		ce_skip_bits (ce_first, skip, &byte, &bit);
		last_row += skip;
	      }
	    else
	      {
		ce_skip_bits (ce_first, skip - 1, &byte, &bit);
		last_row += skip;
	      }
	    for (;;)
	      {
		CE_OUT (ce_first_val, first_len, first + byte * 8 + bit + 1, 1);
		last_row++;
		bit++;
		if (target >= to)
		  return target;
		if (target >= ce_row + n_values)
		  {
		    from = target;
		    break;
		  }
		skip = target - last_row;
		if (8 == bit)
		  {
		    byte++;
		    bit = 0;
		  }
		ce_skip_bits (ce_first, skip, &byte, &bit);
		last_row = target;
	      }
	    break;
	  }
	case CE_DICT:
	  {
	    dtp_t n_distinct = ce_first[0];
	    db_buf_t array;
	    int last = MIN (n_values, to - last_row), inx;
	    if (ce_filter == cpo->cpo_value_cb && cpo->cpo_itc->itc_col_spec->sp_min_op < CMP_LIKE && enable_ce_inline)
	      {
		cpo->cpo_skip = skip;
		cpo->cpo_ce = ce;
		cpo->cpo_ce_row_no = last_row;
		if (cpo->cpo_itc->itc_n_matches)
		  from = ce_dict_generic_sets_filter (cpo, ce_first, n_values, n_bytes);
		else
		  from = ce_dict_generic_range_filter (cpo, ce_first, n_values, n_bytes);
		if (from >= to)
		  return from;
		break;
	      }
	    array =
		!CE_INTLIKE (flags) ? ce_any_dict_array (ce_first,
		flags) : (CE_IS_64 & flags) ? ce_first + 1 + (8 * n_distinct) : ce_first + 1 + (4 * n_distinct);
	    for (inx = skip; inx < last;)
	      {
		int v_inx;
		int64 n;
		if (n_distinct <= 16)
		  {
		    v_inx = array[inx / 2];
		    if (inx & 1)
		      v_inx = v_inx >> 4;
		    v_inx &= 0xf;
		  }
		else
		  v_inx = array[inx];
		last_row = ce_row + inx;
		if (CE_INTLIKE (flags))
		  {
		    n = (CE_IS_64 & flags) ? INT64_REF_CA (ce_first + 1 + (8 * v_inx)) : LONG_REF_CA (ce_first + 1 + (4 * v_inx));
		    if (CET_IRI == (CE_DTP_MASK & flags))
		      n &= 0xffffffff;	/* 32 bit iri is unsigned */
		    CE_OUT (NULL, 0, n, 1);
		  }
		else
		  {
		    ce_vec_nth (ce_first + 1, flags, ce_first[0], v_inx, &ce_first_val, &first_len, 0);
		    CE_OUT (ce_first_val, first_len, 0, 1);
		  }
		if (target >= to)
		  return target;
		if (target >= ce_row + n_values)
		  {
		    from = target;
		    break;
		  }
		inx = target - ce_row;
	      }
	    break;
	  }
	default:
	  GPF_T1 ("unknown ce type");
	}
    next_from_pm:
      last_row = ce_row + n_values;
      if (cpo->cpo_pm)
	{
	  if (!pm)
	    {
	      pm = cpo->cpo_pm;
	      init_pm_pos = cpo->cpo_pm_pos;
	      pm_pos = init_pm_pos;
	    }
	  for (pm_pos = pm_pos + 2;; pm_pos += 2)
	    {
	      short n_in_ce = pm->pm_entries[pm_pos + 1];
	      if (from < last_row + n_in_ce)
		{
		  ce = cpo->cpo_string + pm->pm_entries[pm_pos] - pm->pm_entries[init_pm_pos];
		  skip = 0;
		  cpo->cpo_ce_row_no = last_row;
		  goto new_ce;
		}
	      last_row += n_in_ce;
	      if (pm_pos >= pm->pm_count)
		return from;
	    }
	}
      else
	{
	  ce += n_bytes + hl;
	  cpo->cpo_ce_row_no = last_row;
	  skip = 0;
	}
    }
  return to;
}


#define CE_HEAD_LEN(n_bytes, n_vals) ((n_bits > 255 || n_vals > 255) ? 5 : 3)


int
cs_count_repeats (caddr_t * values, int from, int to, caddr_t prev)
{
  int inx, ctr = 0;
  for (inx = from; inx < to; inx++)
    {
      if (box_equal (values[inx], prev))
	ctr++;
      else
	return ctr;
    }
  return ctr;
}


int
cs_best_rl (compress_state_t * cs, int from, int to, int *start_ret, int *end_ret)
{
  /* find the best candidate for rl.  Must be the same thing over 15 times followed by another thing over 15 times.  The run ends when there is a thing that is not repeated */
  int64 *numbers = cs->cs_numbers;
  caddr_t *values = cs->cs_values;
  int inx;
  int any_64 = 0;
  int64 prev_n;
  int rl = 1;
  int run_start = from;
  prev_n = numbers[from];
  if (DV_SINGLE_FLOAT == (dtp_t) values[from][0] || DV_DOUBLE_FLOAT == (dtp_t) values[from][0])
    {
      /* floats and doubles are considered an asc sequence only if they are all equal. rl is always the best */
      *start_ret = from;
      *end_ret = to;
      return 1;
    }
  if (IS_64 (prev_n))
    any_64 = 1;
  for (inx = from + 1; inx < to; inx++)
    {
      if (numbers[inx] == prev_n)
	rl++;
      else
	{
	  if (rl < 128)
	    {
	      prev_n = numbers[inx];
	      rl = 1;
	      run_start = inx;
	      continue;
	    }
	  *start_ret = run_start;
	  *end_ret = inx;
	  return rl * (any_64 ? 9 : 5);;
	}
    }
  if (rl < 128)
    return 0;
  *start_ret = run_start;
  *end_ret = inx;
  if (rl)
    return rl * (any_64 ? 9 : 5);
  return 0;
}


int
cs_best_rld (compress_state_t * cs, int from, int to, int *start_ret, int *end_ret,
    int *n_values_ret, int *n_values_w_dup, int *first_dup)
{
  /* give first good rld run.  and return the start and end and compressed bytes.
   * The run is broken by a delta of over 8 * 16 or by over 10 * 16 repeats */
  int64 *numbers = cs->cs_numbers;
  int run_start = from, rl = 1;
  int last_val_start = run_start;
  int bytes = 8, last_val_bytes = 0;
  int inx;
  int64 prev = numbers[from];
  for (inx = from + 1; inx < to; inx++)
    {
      int64 n = numbers[inx];
      int64 delta = n - prev;
      if (0 == delta)
	{
	  if (1 == rl)
	    {
	      if (-1 == *first_dup)
		*first_dup = inx;
	      (*n_values_w_dup)++;
	    }
	  rl++;
	  (*n_values_ret)++;
	  if (rl > 16 * 12)
	    {
	      *start_ret = run_start;
	      *end_ret = last_val_start - 1;
	      return last_val_bytes;
	    }
	  if (rl > 15)
	    {
	      bytes++;
	      rl = 1;
	    }
	  continue;
	}
      else if (delta > 9 * 16 || delta < 0)
	{
	  *start_ret = run_start;
	  *end_ret = inx;
	  return bytes;
	}
      (*n_values_ret)++;
      bytes += 1 + (delta / 16);
      last_val_bytes = bytes;
      rl = 1;
      prev = n;
    }
  *start_ret = run_start;
  *end_ret = inx;
  return bytes;
}


int64
cs_min (compress_state_t * cs, int from, int to, db_buf_t * any_ret)
{
  int64 *numbers = cs->cs_numbers;
  int inx;
  int64 min = numbers[from];
  int min_inx = from;
  for (inx = from + 1; inx < to; inx++)
    {
      int64 n = numbers[inx];
      if (n < min)
	{
	  min = n;
	  min_inx = inx;
	}
    }
  if (any_ret)
    *any_ret = (db_buf_t) cs->cs_values[min_inx];
  return min;
}


int64
cs_int_delta_base (compress_state_t * cs, int from, int to, int *end_inx_ret)
{
  int64 *numbers = cs->cs_numbers, min, max;
  int inx;
  if (to - from > 255)
    to = from + 255;
  min = numbers[from];
  max = min;
  for (inx = from + 1; inx < to; inx++)
    {
      int64 n = numbers[inx];
      if (n < min)
	{
	  if (max - (n & CLEAR_LOW_BYTE) > 0xffff)
	    {
	      *end_inx_ret = inx;
	      return min & CLEAR_LOW_BYTE;
	    }
	  min = n;
	}
      else if (n > max)
	{
	  max = n;
	  if (max - (min & CLEAR_LOW_BYTE) > 0xffff)
	    {
	      *end_inx_ret = inx;
	      return min & CLEAR_LOW_BYTE;
	    }
	}
    }
  *end_inx_ret = inx;
  return min & CLEAR_LOW_BYTE;
}


int
cs_int_delta_bytes (compress_state_t * cs, int from, int to, int *start_ret, int *end_ret, int is_asc)
{
  int64 *numbers = cs->cs_numbers;
  caddr_t *values = cs->cs_values;
  int inx, bytes = 0;
  db_buf_t min_any;
  int64 min, base;
  dtp_t dtp = values[from][0];
  if (DV_STRING == dtp || DV_SHORT_STRING_SERIAL == dtp
      || DV_BIN == dtp || DV_LONG_BIN == dtp || DV_SINGLE_FLOAT == dtp || DV_DOUBLE_FLOAT == dtp || DV_NUMERIC == dtp)
    return 1000000;		/* not applied to variable len dtps */
  if (!is_asc)
    min = cs_min (cs, from, to, &min_any);
  else
    {
      min = numbers[from];
      min_any = (db_buf_t) values[from];
    }
  min &= CLEAR_LOW_BYTE;
  base = min;
  bytes = IS_64 (min) ? 11 : 7;
  for (inx = from; inx < to; inx++)
    {
      int64 n = numbers[inx];
      int64 delta = n - base;
      if (delta >= 0 && delta < 0x10000)
	bytes += 2;
      else if (n - min >= 0 && n - min < CE_INT_DELTA_MAX)
	{
	  int end_inx;
	  base = cs_int_delta_base (cs, inx, to, &end_inx);
	  bytes += 4 + 2 * (end_inx - inx);
	  inx = end_inx - 1;
	}
      else
	break;
    }
  *end_ret = inx;
  return bytes;
}


void
cs_append_header (dtp_t * out, int *fill_ret, int flags, int n_values, int n_bytes)
{
  int fill = *fill_ret;
  dtp_t type = flags & CE_TYPE_MASK;
  if (n_bytes > PAGE_DATA_SZ)
    GPF_T1 ("writing a ce that would be longer than a page");
  if (CE_GAP == type)
    {
      if (1 == n_bytes)
	{
	  out[fill] = CE_GAP_1;
	  (*fill_ret)++;
	}
      else if (n_bytes < 256)
	{
	  out[fill] = CE_SHORT_GAP;
	  out[fill + 1] = n_bytes - 2;
	  (*fill_ret) += 2;
	}
      else
	{
	  out[fill] = CE_GAP;
	  SHORT_SET_CA (out + 1, n_bytes - 3);
	  (*fill_ret) += 3;
	}
      return;
    }
  if (CE_RL == type || CE_DENSE == type)
    {
      if (n_values < 256)
	{
	  out[fill] = flags | CE_IS_SHORT;
	  out[fill + 1] = n_values;
	  *fill_ret = fill + 2;
	}
      else
	{
	  out[fill] = flags;
	  SHORT_SET_CA (out + fill + 1, n_values);
	  *fill_ret = fill + 3;
	}
      return;
    }
  if (CE_VEC == type && CE_INTLIKE (flags))
    {
      if (n_values < 256)
	{
	  out[fill] = flags | CE_IS_SHORT;
	  out[fill + 1] = n_values;
	  *fill_ret = fill + 2;
	}
      else
	{
	  out[fill] = flags;
	  SHORT_SET_CA (out + fill + 1, n_values);
	  *fill_ret = fill + 3;
	}
      return;
    }
  if (n_values < 256 && n_bytes < 256)
    {
      out[fill] = flags | CE_IS_SHORT;
      out[fill + 1] = n_bytes;
      out[fill + 2] = n_values;
      *fill_ret = fill + 3;
    }
  else
    {
      out[fill] = flags;
      SHORT_SET_CA (out + fill + 1, n_bytes);
      SHORT_SET_CA (out + fill + 3, n_values);
      *fill_ret = fill + 5;
    }
}


void
cs_write_array (compress_state_t * cs, int from, int to)
{
  caddr_t *values = cs->cs_values;
  int inx, is_iri;
  dtp_t *out;
  int fill = cs->cs_asc_fill;
  int non_comp_len = 0, head = fill;
  int int_type;
  if (cs->cs_asc_fill + 3 * (to - from) > cs->cs_asc_cutoff && cs->cs_asc_reset)
    longjmp_splice (cs->cs_asc_reset, 1);

  if (from == to)
    return;
  if (to - from > 250)
    {
      int is_int = 0;
      int bytes_guess = cs_non_comp_len (cs, from, to, &int_type);
      cs_length_check (&cs->cs_asc_output, cs->cs_asc_fill, bytes_guess * 2);
      cs_write_typed_vec (cs, cs->cs_asc_output, &cs->cs_asc_fill, from, to, is_int, bytes_guess);
      return;
    }
  if (cs->cs_all_int && !cs->cs_any_64)
    {
      int_type = 32;
      non_comp_len = (to - from) * sizeof (int32);
    }
  else
    non_comp_len = cs_non_comp_len (cs, from, to, &int_type);
  is_iri = IS_IRI_DTP ((dtp_t) values[from][0]) ? CE_IS_IRI : 0;
  cs_length_check (&cs->cs_asc_output, cs->cs_asc_fill, non_comp_len);
  out = cs->cs_asc_output;
  if (64 == int_type)
    {
      cs_append_header (out, &cs->cs_asc_fill, CE_VEC | CE_IS_64 | is_iri, to - from, sizeof (int64) * (to - from));
      fill = cs->cs_asc_fill;
      for (inx = from; inx < to; inx++)
	{
	  int64 n = any_num (values[inx]);
	  INT64_SET_CA (out + fill, n);
	  fill += 8;
	}
    }
  else if (32 == int_type)
    {
      cs_append_header (out, &cs->cs_asc_fill, CE_VEC | is_iri, to - from, sizeof (int32) * (to - from));
      fill = cs->cs_asc_fill;
      for (inx = from; inx < to; inx++)
	{
	  int64 n = any_num (values[inx]);
	  LONG_SET_CA (out + fill, n);
	  fill += 4;
	}
    }
  else
    {
      int n_values = to - from, org_fill;
      cs_append_header (out, &cs->cs_asc_fill, CE_VEC | CET_ANY, n_values, non_comp_len);
      org_fill = fill = cs->cs_asc_fill;
      cs_write_vec_body (out, &fill, (db_buf_t *) & values[from], n_values, NULL, int_type, 0);
      if (non_comp_len != fill - org_fill)
	{
	  if (CE_IS_SHORT & out[head])
	    out[head + 1] = fill - org_fill;
	  else
	    SHORT_SET_CA (out + head + 1, fill - org_fill);
	}
      {
      }
    }
  cs->cs_asc_fill = fill;
}


void
cs_write_rl (compress_state_t * cs, int from, int to)
{
  db_buf_t any = (db_buf_t) cs->cs_values[from];
  dtp_t flags = any_ce_flags (any);
  cs_append_header (cs->cs_asc_output, &cs->cs_asc_fill, CE_RL | flags, to - from, any_ce_len ((db_buf_t) cs->cs_values[from]));
  cs_append_any (cs->cs_asc_output, &cs->cs_asc_fill, any, 0);
}


void
cs_write_rld (compress_state_t * cs, int from, int to)
{
  int org_fill;
  dtp_t *out;
  db_buf_t any = (db_buf_t) cs->cs_values[from];
  dtp_t flags = any_ce_flags (any);
  int inx;
  int64 *numbers = cs->cs_numbers;
  int64 prev = numbers[from];
  int fill, rl = 1, prev_delta = 0;

  cs_length_check (&cs->cs_asc_output, cs->cs_asc_fill, (to - from) * 10);
  out = cs->cs_asc_output;
  cs_append_header (cs->cs_asc_output, &cs->cs_asc_fill, CE_RL_DELTA | flags, to - from, 1000);
  fill = cs->cs_asc_fill;
  org_fill = fill;
  cs_append_any (cs->cs_asc_output, &fill, any, 0), flags;
  for (inx = from + 1; inx < to; inx++)
    {
      int64 n = numbers[inx];
      int64 delta = n - prev;
      if (0 == delta)
	{
	  rl++;
	  if (15 == rl)
	    {
	      for (prev_delta = prev_delta; prev_delta > 15; prev_delta -= 15)
		{
		  out[fill++] = 0xf0;
		}
	      out[fill++] = (prev_delta << 4) | rl;
	      rl = 0;
	      prev_delta = 0;
	    }
	  continue;
	}
      if (0 == rl)
	{
	  rl = 1;
	  prev_delta = delta;
	  prev = n;
	  continue;
	}
      for (prev_delta = prev_delta; prev_delta > 15; prev_delta -= 15)
	{
	  out[fill++] = 0xf0;
	}
      out[fill++] = (prev_delta << 4) | rl;
      prev = n;
      rl = 1;
      prev_delta = delta;
      for (prev_delta = delta; prev_delta > 15; prev_delta -= 15)
	{
	  out[fill++] = 0xf0;
	}
    }
  if (rl)
    {
      for (prev_delta = prev_delta; prev_delta > 15; prev_delta -= 15)
	{
	  out[fill++] = 0xf0;
	}
      out[fill++] = prev_delta << 4 | rl;
    }
  SHORT_SET_CA (out + org_fill - 4, fill - org_fill);
  cs->cs_asc_fill = fill;
}


int
cs_append_any (dtp_t * out, int *fill_ret, db_buf_t any, int clear_last)
{
  /* for all the asc compressions like rl, rld, bits, int delta, write the initial value and return the dtp part of the ce flags */
  int flags = 0;
  int fill = *fill_ret;
  if (IS_INTLIKE_DTP (any[0]))
    {
      int64 n = any_num (any);
      if (clear_last)
	n &= CLEAR_LOW_BYTE;
      if (IS_64_T (n, any[0]))
	{
	  INT64_SET_CA (out + fill, n);
	  fill += 8;
	  flags = CE_IS_64;
	}
      else
	{
	  LONG_SET_CA (out + fill, n);
	  fill += 4;
	  flags = 0;
	}
      if (IS_IRI_DTP (any[0]))
	flags |= CE_IS_IRI;
    }
  else if (DV_DB_NULL == any[0])
    return CET_NULL;
  else
    {
      dtp_t last_byte;
      int l = box_length ((caddr_t) any) - 1, org_l = l;
      dtp_t dtp = any[0];
      if (clear_last)
	{
	  last_byte = any[l - 1];
	  any[l - 1] = 0;
	}
      if (DV_LONG_STRING == dtp)
	{
	  if (l <= 8)
	    goto str_as_any;
	  l -= 5;
	  out[fill++] = (l & 0x7f) | 0x80;
	  out[fill++] = l >> 7;
	  memcpy (out + fill, any + 5, l);
	  fill += l;
	  flags = CET_CHARS;
	}
      else if (DV_SHORT_STRING_SERIAL == dtp)
	{
	  if (l <= 5)
	    goto str_as_any;
	  l -= 2;
	  if (l > 127)
	    {
	      out[fill++] = (l & 0x7f) | 0x80;
	      out[fill++] = 1;
	    }
	  else
	    out[fill++] = l;
	  memcpy (out + fill, any + 2, l);
	  fill += l;
	  flags = CET_CHARS;
	}
      else
	{
	str_as_any:
	  memcpy (out + fill, any, l);
	  fill += l;
	  flags = CET_ANY;
	}
      if (clear_last)
	any[org_l - 1] = last_byte;
    }
  *fill_ret = fill;
  return flags;
}


void
cs_append_4 (dtp_t * out, int *fill_ret, int64 n)
{
  int fill = *fill_ret;
  LONG_SET_NA (out + fill, n);
  (*fill_ret) += 4;
}


void
cs_append_2 (dtp_t * out, int *fill_ret, int64 n)
{
  int fill = *fill_ret;
  SHORT_SET_CA (out + fill, n);
  (*fill_ret) += 2;
}


void
cs_write_int_delta (compress_state_t * cs, int from, int to, int is_asc)
{
  db_buf_t min_any = NULL;
  int flags, is_date;
  int end_inx;
  dtp_t *out;
  int inx, fill = 5 + cs->cs_asc_fill, prev_fill;
  int count_off, rl, init_fill = cs->cs_asc_fill;
  caddr_t *values = cs->cs_values;
  int64 *numbers = cs->cs_numbers;
  int64 min, base, local_base;
  cs_length_check (&cs->cs_asc_output, cs->cs_asc_fill, (to - from) * 10);
  out = cs->cs_asc_output;
  if (DV_SINGLE_FLOAT == (dtp_t) values[from][0] || DV_DOUBLE_FLOAT == (dtp_t) values[from][0])
    GPF_T1 ("int delta format not for float or double");
  if (!is_asc)
    min = cs_min (cs, from, to, &min_any);
  else
    {
      min = numbers[from];
      min_any = (db_buf_t) values[from];
    }
  is_date = DV_DATE == any_ce_dtp (min_any);
  base = min & CLEAR_LOW_BYTE;
  local_base = base;
  prev_fill = fill;
  flags = cs_append_any (out, &fill, min_any, is_date ? 0 : 1);
  if (is_date)
    count_off = fill - 8;
  else if (CE_INTLIKE (flags))
    {
#if WORDS_BIGENDIAN
      count_off = fill - 1;
#else
      count_off = prev_fill;
#endif
    }
  else
    count_off = fill - 1;
  rl = 0;
  for (inx = from; inx < to; inx++)
    {
      int64 n = numbers[inx];
      int64 local_delta = n - local_base;
      if (-1 == rl || (local_delta >= 0x10000 || local_delta < 0))
	{
	  /* start a new run of 16 bit offsets */
	  out[count_off] = rl;
	  local_base = cs_int_delta_base (cs, inx, to, &end_inx);
	  cs_append_4 (out, &fill, local_base - base);
	  count_off = fill - 1;
	  cs_append_2 (out, &fill, n - local_base);
	  rl = 1;
	  continue;
	}
      rl++;
      SHORT_SET_CA (out + fill, n - local_base);
      fill += 2;
      if (rl == 255)
	{
	  out[count_off] = rl;
	  rl = -1;
	  continue;
	}
    }
  if (-1 != rl)
    out[count_off] = rl;
  cs->cs_asc_fill = fill;
  cs_write_header (out + init_fill, CE_INT_DELTA | flags, to - from, fill - init_fill - 5);
}


void
cs_write_bits (compress_state_t * cs, int from, int to)
{
  int fill = cs->cs_asc_fill, byte = 0, fill1, bytes;
  dtp_t *out;
  int inx;
  caddr_t *values = cs->cs_values;
  int64 *numbers = cs->cs_numbers;
  int64 first = numbers[from];
  int64 last = numbers[to - 1];
  dtp_t flags = any_ce_flags ((dtp_t *) values[from]);
  bytes = (ALIGN_8 ((last - first)) / 8) + any_ce_len ((dtp_t *) values[from]);
  if (bytes < 0 || bytes > 4096)
    GPF_T1 ("bm size out of whack");
  cs_length_check (&cs->cs_asc_output, cs->cs_asc_fill, bytes + 5);
  out = cs->cs_asc_output;
  cs_append_header (cs->cs_asc_output, &cs->cs_asc_fill, CE_BITS | flags, to - from, bytes);
  fill1 = fill = cs->cs_asc_fill;
  memset (out + fill, 0, bytes);
  cs_append_any (out, &fill, (db_buf_t) values[from], 0);
  for (inx = from + 1; inx < to; inx++)
    {
      int64 n = numbers[inx] - first - 1;
      byte = n >> 3;
      out[fill + byte] |= 1 << (n & 0x7);
    }
  cs->cs_asc_fill = fill1 + bytes;
}


void
cs_compress_asc (compress_state_t * cs, int from, int to, int is_left)
{
  caddr_t *values = cs->cs_values;
  int rl_start = 0, rl_end = 0, rld_bytes;
  int n_values = 0, n_values_w_dup = 0, first_dup = -1;
  if (to <= from)
    return;
  if (to - from < 2)
    {
      cs_write_array (cs, from, to);
      return;
    }
  if (!is_left)
    {
      if (!(CS_NO_RL & cs->cs_exclude) && cs_best_rl (cs, from, to, &rl_start, &rl_end))
	{
	  cs_compress_asc (cs, from, rl_start, 1);
	  cs_write_rl (cs, rl_start, rl_end);
	  cs_compress_asc (cs, rl_end, to, 0);
	  return;
	}
    }
  if ((CS_NO_RLD & cs->cs_exclude))
    rld_bytes = 1000000;
  else
    rld_bytes = cs_best_rld (cs, from, to, &rl_start, &rl_end, &n_values, &n_values_w_dup, &first_dup);
  if (rld_bytes <= 2 * (rl_end - rl_start))
    {
      int64 b1 = any_num (values[rl_start]), b2 = any_num (values[rl_end - 1]);
      int bits_bytes = ((b2 - b1) / 8) + 8 + 8 * n_values_w_dup;
      if (((unsigned int64) (b2 - b1)) > 30000 || (CS_NO_BITS & cs->cs_exclude))
	bits_bytes = 1000000;
      cs_compress_asc (cs, from, rl_start, 1);
      if (bits_bytes < rld_bytes)
	{
	  cs_write_bits (cs, rl_start, -1 == first_dup ? rl_end : first_dup);
	  if (-1 != first_dup)
	    cs_compress_asc (cs, first_dup, rl_end, 1);
	}
      else
	cs_write_rld (cs, rl_start, rl_end);
      cs_compress_asc (cs, rl_end, to, 1);
    }
  else
    {
      int vec_int_type;
      int vec_bytes = cs_non_comp_len (cs, from, to, &vec_int_type);
      int int_delta_bytes = (CS_NO_DELTA & cs->cs_exclude) ? 1000000 : cs_int_delta_bytes (cs, from, to, &rl_start, &rl_end, 1);
      vec_bytes *= (rl_end - from) / (to - from);
      if (int_delta_bytes < vec_bytes)
	{
	  cs_write_int_delta (cs, from, rl_end, 1);
	  cs_compress_asc (cs, rl_end + 1, to, 1);
	}
      else
	cs_write_array (cs, from, to);
    }
}


void
cs_best_rnd (compress_state_t * cs, int from, int to)
{
  caddr_t first;
  int inx;
  if (to <= from)
    return;
  first = cs->cs_values[from];
  for (inx = from + 1; inx < to; inx++)
    {
      if (first != cs->cs_values[inx])
	break;
    }

  if (inx - from > 1)
    {
      cs_write_rl (cs, from, inx);
      from = inx;
    }
  cs_write_array (cs, from, to);
}


void
cs_best_asc (compress_state_t * cs, int from, int to, int n_dups, int any_64)
{
  cs_compress_asc (cs, from, to, 0);
}


int
asc_cmp (dtp_t * dv1, dtp_t * dv2)
{
  /* 0 or 1  if dv1 fits before dv2 in compressed asc seq.  Same dtp and same length, else -1 if seq break, -2 if gt but length and dtp stay */
  int len1, len2;
  int rc;
  dtp_t dtp_1 = dtp_canonical[*dv1];
  dtp_t dtp_2 = dtp_canonical[*dv2];
  if (dtp_1 != dtp_2)
    return -1;
  switch (dtp_1)
    {
    case DV_SINGLE_FLOAT:
      return 0 == memcmp (dv1 + 1, dv2 + 1, 4) ? 0 : -1;
    case DV_DOUBLE_FLOAT:
      return 0 == memcmp (dv1 + 1, dv2 + 1, 8) ? 0 : -2;
    case DV_NUMERIC:
      return dv1[1] == dv2[1] && 0 == memcmp (dv1 + 2, dv2 + 2, dv1[1]) ? 0 : -1;
    case DV_LONG_INT:
    case DV_INT64:
    case DV_IRI_ID:
    case DV_IRI_ID_8:
    case DV_DATE:
      break;
    case DV_DATETIME:
      if (DT_TYPE_DATE != DT_DT_TYPE (dv1 + 1) || DT_TYPE_DATE != DT_DT_TYPE (dv2 + 1) || DT_TZ (dv1 + 1) != DT_TZ (dv2 + 1))
	return -1;
      break;
    case DV_RDF:
      DB_BUF_TLEN (len1, dv1[0], dv1);
      DB_BUF_TLEN (len2, dv2[0], dv2);
      /* A non-string rdf box is compared without the rdf type in search.  The delta on last bytes would fall on the type and different types would be distinguished by different deltas which means non-equality.  But the comparison is still meant to be equal hence delta type compressions do not apply  but rl does if all bytes eq, also type */
      if (len1 == len2 && 0 == memcmp (dv1, dv2, len1))
	return 0;
      return -1;

    default:
      {
	DB_BUF_TLEN (len1, dv1[0], dv1);
	DB_BUF_TLEN (len2, dv2[0], dv2);
	if (len1 != len2)
	  return -1;
	if (len1 < 4 || 0 != memcmp (dv1, dv2, len1 - 4))
	  return -1;
	if (any_num (dv1) > any_num (dv2))
	  return -2;
      }
    }
  rc = dv_compare (dv1, dv2, NULL, 0);
  if (DVC_MATCH == rc)
    return 0;
  if (DVC_LESS == rc)
    return 1;
  else
    return -2;
}


int
asc_str_cmp (db_buf_t dv1, db_buf_t dv2, int len1, int len2, uint32 * num_ret, char is_int_delta)
{
  /* compare different length strings for int compression.  Different length breaks the seq but still must find proper insertion point
   * dv1 is the 1st value of the asc ce.  If len1 */
  uint32 u1, u2;
  int rc;
  dtp_t tail[4];
  int i, f = 0;
  if (len1 == len2)
    {
      rc = memcmp (dv1, dv2, len1 - 4);
      if (rc < 0)
	return DVC_DTP_LESS;
      if (rc > 0)
	return DVC_DTP_GREATER;
      u1 = LONG_REF_NA (dv1 + len1 - 4);
      if (is_int_delta)
	u1 &= CLEAR_LOW_BYTE;
      u2 = LONG_REF_NA (dv2 + len2 - 4);
      *num_ret = u2 - u1;
      if (u1 > u2)
	return DVC_DTP_GREATER;
      return u2 == u1 ? DVC_MATCH : DVC_LESS;
    }
  if (len2 > len1)
    {
      rc = memcmp (dv1, dv2, len1 - 4);
      if (rc < 0)
	return DVC_DTP_LESS;
      if (rc > 1)
	return DVC_DTP_GREATER;
      u1 = LONG_REF_NA (dv1 + len1 - 4);
      u2 = LONG_REF_NA (dv2 + len1 - 4);
      if (is_int_delta)
	u1 &= CLEAR_LOW_BYTE;
      if (u1 > u2)
	return DVC_DTP_GREATER;
      *num_ret = u2 - u1;
      return ASC_LONGER;
    }
  if (len2 <= len1 - 4)
    {
      rc = memcmp (dv1, dv2, len2);
      return rc <= 0 ? DVC_DTP_LESS : DVC_DTP_GREATER;
    }
  /* len2 is less than len 1 but more than len1 - 4 */
  for (i = len2 - 4; i < len2; i++)
    tail[f++] = i < len1 ? dv1[i] : 0;
  u1 = LONG_REF_NA (dv1 + len1 - 4);
  if (is_int_delta)
    u1 &= CLEAR_LOW_BYTE;
  u2 = LONG_REF_NA (&tail[0]);
  if (u1 > u2)
    return DVC_DTP_GREATER;
  *num_ret = u2 - u1;
  return ASC_SHORTER;
}


int
asc_cmp_delta (dtp_t * dv1, dtp_t * dv2, uint32 * num_ret, int is_int_delta)
{
  /* use in search for an int compressed.  Cmp with 1st value and value being sought.  Returns the uin32 delta if they could fit in the same int ce, else returns dvc dtp lt or gt to indicate value sought either before or after all.
   * if different len for variable len int compress ce, also ret the number for use in finding insertion place even though different len means no hit */
  int64 n1, n2, delta;
  int len1, len2;
  int rc;
  dtp_t dtp_1 = dtp_canonical[*dv1];
  dtp_t dtp_2 = dtp_canonical[*dv2];
  if (dtp_1 != dtp_2)
    {
      int n1 = IS_NUM_DTP (dtp_1);
      int n2 = IS_NUM_DTP (dtp_2);
      if (DV_RDF_ID == dtp_1 && DV_RDF == dtp_2)
	{
	  int rcmp;
	  dtp_t temp[10];
	  int64 d;
	  if (is_int_delta)
	    {
	      int l1 = db_buf_const_length[*dv1];
	      memcpy (temp, dv1, l1);
	      temp[l1 - 1] = 0;
	      dv1 = temp;
	    }
	  rcmp = dv_rdf_id_compare (dv1, dv2, 0, &d);
	  if ((DVC_MATCH == rcmp || DVC_LESS == rcmp) && d >= 0 && d < CE_INT_DELTA_MAX)
	    {
	      *num_ret = d;
	      return rcmp;
	    }
	  return DVC_LESS == rcmp ? DVC_DTP_LESS : rcmp;
	}
      if (n1 && n2)
	return ASC_NUMBERS;
      if (DV_RDF == dtp_1 || DV_RDF == dtp_2)
	return ASC_NUMBERS;
      if (n1)
	dtp_1 = DV_LONG_INT;
      if (n2)
	dtp_2 = DV_LONG_INT;
      return (dtp_1 < dtp_2) ? DVC_DTP_LESS : DVC_DTP_GREATER;
    }
  switch (dtp_1)
    {
    case DV_LONG_INT:
    case DV_INT64:
    case DV_DATE:
    case DV_IRI_ID:
    case DV_IRI_ID_8:
    case DV_RDF_ID:
    case DV_RDF_ID_8:
      break;

    case DV_SINGLE_FLOAT:
    case DV_DOUBLE_FLOAT:
    case DV_NUMERIC:
      /* non int numbers can occur in a run length but not in one with deltas so set delta to 0 and indicate lt/gt for whole ce */
      *num_ret = 0;
      rc = dv_compare (dv1, dv2, NULL, 0);
      return DVC_MATCH == rc ? DVC_MATCH : DVC_LESS == rc ? DVC_DTP_LESS : DVC_DTP_GREATER;
    case DV_RDF:
      *num_ret = 0;
      if (DVC_MATCH == dv_compare (dv1, dv2, NULL, 0))
	return DVC_MATCH;
      return ASC_NUMBERS;
    case DV_DATETIME:
      if (DT_TZ (dv1 + 1) != DT_TZ (dv2 + 1))
	return ASC_NUMBERS;
      if (DT_TYPE_DATE != DT_DT_TYPE (dv1 + 1))
	{
	  /* a rl can start with a datetime */
	  if (0 == memcmp (dv1 + 1, dv2 + 1, DT_COMPARE_LENGTH))
	    {
	      *num_ret = 0;
	      return DVC_MATCH;
	    }
	  return ASC_NUMBERS;
	}
      if (DT_TYPE_DATE != DT_DT_TYPE (dv2 + 1))
	{
	  return ASC_NUMBERS;
	  n1 = DT_DAY (dv1 + 1);
	  if (is_int_delta)
	    n1 &= CLEAR_LOW_BYTE;
	  n2 = DT_DAY (dv2 + 1);
	  if (n1 > n2)
	    return DVC_DTP_GREATER;
	  *num_ret = n2 - n1;
	  return ASC_LONGER;
	}
      break;
    default:
      {
	DB_BUF_TLEN (len1, dv1[0], dv1);
	DB_BUF_TLEN (len2, dv2[0], dv2);
	rc = asc_str_cmp (dv1, dv2, len1, len2, num_ret, is_int_delta);
	return rc;
      }
    }
  n1 = any_num (dv1);
  if (is_int_delta)
    n1 &= CLEAR_LOW_BYTE;
  n2 = any_num (dv2);
  if (DV_IRI_ID == dtp_1 ? ((iri_id_t) n2 < (iri_id_t) n1) : (n2 < n1))
    return DVC_DTP_GREATER;
  delta = n2 - n1;
  /* delta is signed difference and can be neg if unsigned n2 > m1 by large value */
  if (delta < 0 || delta > INT32_MAX)
    return DVC_DTP_LESS;
  *num_ret = delta;
  return DVC_LESS;
}


int min_asc = 3;

int
cs_try_asc (compress_state_t * cs, int from, int to)
{
  int inx;
  int n_asc = 0, n_dups = 0, any_64 = 0;
  int last_compressed = from - 1;
  int first_asc = -1, first_dtp = -1;
  dtp_t *prev = (dtp_t *) cs->cs_values[from];
  int64 prev_n = cs->cs_numbers[from];
  if (IS_64 (prev_n))
    any_64 = CE_IS_64;
  for (inx = from + 1; inx < to; inx++)
    {
      dtp_t *val = (dtp_t *) cs->cs_values[inx];
      int rc = asc_cmp (prev, val);
      if (rc >= 0)
	{
	  if (IS_64_DTP (val[0]))
	    any_64 = CE_IS_64;
	  if (-1 == first_dtp)
	    first_dtp = inx - 1;
	  if (-1 == first_asc)
	    first_asc = inx - 1;
	  n_asc++;
	  if (0 == rc)
	    n_dups++;
	  prev = val;
	}
      else
	{
	  if (-2 == rc && -1 == first_dtp)
	    first_dtp = inx - 1;
	  if (-1 == rc)
	    first_dtp = -1;
	  if (n_asc > min_asc)
	    {
	      cs_best_rnd (cs, last_compressed + 1, first_asc);
	      cs_best_asc (cs, first_asc, inx, n_dups, any_64);
	      last_compressed = inx - 1;
	      any_64 = n_dups = n_asc = 0;
	      first_asc = -1;
	      prev = val;
	      prev_n = cs->cs_numbers[inx];
	      if (IS_64 (prev_n))
		any_64 = CE_IS_64;
	    }
	  else
	    {
	      first_asc = -1;
	      n_asc = n_dups = 0;
	    }
	}
      prev = (dtp_t *) cs->cs_values[inx];
    }
  if (n_asc > min_asc)
    {
      cs_best_rnd (cs, last_compressed + 1, first_asc);
      cs_best_asc (cs, first_asc, to, n_dups, any_64);
    }
  else
    cs_best_rnd (cs, last_compressed + 1, to);
  if (!cs->cs_is_asc && 0 == first_dtp)
    {
      int id_start = first_dtp, id_end = 0;
      int vec_bytes, vec_type;
      int int_delta_bytes = CS_NO_DELTA & cs->cs_exclude ? 1000000 : cs_int_delta_bytes (cs, from, to, &id_start, &id_end, 0);
      vec_type = cs_int_type (cs, (db_buf_t *) cs->cs_values, from, to, &vec_bytes);
      if (int_delta_bytes < (cs->cs_asc_fill / 10) * 8 && to == id_end && int_delta_bytes < vec_bytes)
	{
	  cs->cs_asc_fill = 0;
	  if (int_delta_bytes > 2000)
	    {
	      int n, n_split = (int_delta_bytes / 2000) + 1;
	      int slice = (to - from) / n_split;
	      for (n = 0; n < n_split; n++)
		{
		  int last = n < n_split - 1 ? from + ((n + 1) * slice) : to;
		  cs_write_int_delta (cs, from + n * slice, last, 0);
		}
	    }
	  else
	    cs_write_int_delta (cs, from, to, 0);
	  return cs->cs_asc_fill;
	}
      if (vec_bytes < cs->cs_asc_fill)
	{
	  cs->cs_asc_fill = 0;
	  cs_length_check (&cs->cs_asc_output, cs->cs_asc_fill, vec_bytes * 2);
	  cs_write_typed_vec (cs, cs->cs_asc_output, &cs->cs_asc_fill, from, to, vec_type, vec_bytes);
	  return cs->cs_asc_fill;
	}
    }
  return cs->cs_asc_fill;
}


void
cs_clear (compress_state_t * cs)
{
  cs->cs_ready_ces = NULL;
  cs->cs_prev_ready_ces = NULL;
  cs->cs_is_asc = 0;
  cs->cs_org_values = NULL;
}


int
ce_string_n_values (db_buf_t ce, int len)
{
  int n = 0;
  db_buf_t end = ce + len;
  while (ce < end)
    {
      ce = ce_skip_gap (ce);
      n += ce_n_values (ce);
      ce += ce_total_bytes (ce);
    }
  return n;
}


#define ASC_RESET 1

int
cs_try_asc_up_to (compress_state_t * cs, int from, int to, int max)
{
  int reset = 0;
  jmp_buf_splice rst;
  cs->cs_asc_reset = &rst;
  cs->cs_asc_cutoff = max;
  if (!setjmp_splice (&rst))
    cs_try_asc (cs, from, to);
  else
    reset = ASC_RESET;
  cs->cs_asc_reset = NULL;
  return reset;
}


void
cs_best (compress_state_t * cs, dtp_t ** best, int *len)
{
  jmp_buf_splice rst;
  int try_dict = !cs->cs_is_asc && !cs->cs_no_dict && (0 == (CS_NO_DICT & cs->cs_exclude))
      && cs->cs_n_values > 2 && cs->cs_any_distinct && cs->cs_any_distinct->ht_count > 1;
  int rnd_len, dict_only = 0;
  cs->cs_asc_fill = 0;
  cs->cs_dict_fill = 0;
  if (try_dict)
    cs->cs_asc_cutoff = cs->cs_unq_non_comp_len + (cs->cs_n_values / (cs->cs_any_distinct->ht_count > 16 ? 1 : 2));
  else
    cs->cs_asc_cutoff = 100000000;
  if (!setjmp_splice (&rst))
    {
      cs->cs_asc_reset = &rst;
      cs_try_asc (cs, 0, cs->cs_n_values);
    }
  else
    dict_only = 1;
  cs->cs_asc_reset = NULL;
  cs_buf_mark_check (cs->cs_asc_output);
  rnd_len = cs->cs_asc_fill;
  if (try_dict)
    {
      /* look at stats to see if dict makes any sense */
      int n_dist = cs->cs_any_distinct->ht_count;
      if (!dict_only && cs->cs_asc_fill < cs->cs_unq_non_comp_len + (cs->cs_n_values / (n_dist <= 16 ? 2 : 1)))
	try_dict = 0;
    }
  if (!try_dict && !dict_only)
    {
      *best = (db_buf_t) mp_box_n_chars (cs->cs_mp, (caddr_t) cs->cs_asc_output, *len = cs->cs_asc_fill);
      return;
    }
  cs_dict (cs, 0, cs->cs_n_values);
  cs_buf_mark_check (cs->cs_dict_output);
  if (!dict_only && rnd_len < cs->cs_dict_fill)
    {
      *best = (db_buf_t) mp_box_n_chars (cs->cs_mp, (caddr_t) cs->cs_asc_output, *len = cs->cs_asc_fill);
    }
  else
    {
      *best = (db_buf_t) mp_box_n_chars (cs->cs_mp, (caddr_t) cs->cs_dict_result, *len =
	  cs->cs_dict_fill - (cs->cs_dict_result - cs->cs_dict_output));
    }
}

int
dv_cmp (db_buf_t x, db_buf_t y)
{
  int rc = dv_compare (x, y, NULL, 0);
  return DVC_LESS == rc;
}

void
cs_bit_set (dtp_t * bytes, int inx, int width, int val)
{
  if (width <= 16)
    {
      dtp_t b = bytes[inx / 2];
      if (inx % 2)
	b = (0x0f & b) | (val << 4);
      else
	b = (0xf0 & b) | (0x0f & val);
      bytes[inx / 2] = b;
    }
  else
    bytes[inx] = val;
}

int
any_sort_cmp (int a1, int a2, void *cd)
{
  db_buf_t *arr = (db_buf_t *) cd;
  return ~DVC_NOORDER & dv_compare (arr[a1], arr[a2], NULL, 0);
}


void
cs_swap_asc_dict (compress_state_t * cs)
{
  db_buf_t out = cs->cs_asc_output;
  int f = cs->cs_asc_fill;
  cs->cs_asc_output = cs->cs_dict_output;
  cs->cs_asc_fill = cs->cs_dict_fill;
  cs->cs_dict_output = out;
  cs->cs_dict_fill = f;
}


int
cs_cast_incompatible_dict (compress_state_t * cs, int n_distinct, db_buf_t * distinct, int *sort_ids)
{
  /* if got 2 things that are logically eq but binary different can't put them in dict.  Search needs a single dict key to stand for all eq values and must not lose individual types cause of num precision */
  int inx;
  for (inx = 0; inx < n_distinct - 1; inx++)
    {
      db_buf_t dv1 = distinct[sort_ids[inx]];
      db_buf_t dv2 = distinct[sort_ids[inx + 1]];
      if ((IS_NUM_DTP (dv1[0]) || DV_RDF == dv1[0] || DV_DATETIME == dv1[0])
	  && (IS_NUM_DTP (dv2[0]) || DV_RDF == dv2[0] || DV_DATETIME == dv2[0]))
	{
	  if (DVC_MATCH == dv_compare (dv1, dv2, NULL, 0) && !dv_bin_equal (dv1, dv2))
	    return 1;
	}
    }
  return 0;
}


void
cs_dict (compress_state_t * cs, int from, int to)
{
  db_buf_t out;
  int n_distinct = cs->cs_any_distinct->ht_count;
  int is_int, flags = 0;
  int *sort_ids, *sort_ids_2;
  int fill = 0, inx;
  cs->cs_distinct = (db_buf_t *) mp_alloc_box_ni (cs->cs_mp, n_distinct * sizeof (caddr_t), DV_ARRAY_OF_POINTER);
  sort_ids = (int *) mp_alloc_box_ni (cs->cs_mp, n_distinct * sizeof (int), DV_STRING);
  DO_IDHASH (dtp_t *, k, caddr_t, ign, cs->cs_any_distinct)
  {
    sort_ids[fill] = fill;
    cs->cs_distinct[fill++] = k;
  }
  END_DO_IDHASH;
  /* sort */
  sort_ids_2 = (int *) dk_alloc_box (box_length ((caddr_t) sort_ids), DV_STRING);
  gen_qsort (sort_ids, sort_ids_2, n_distinct, 0, any_sort_cmp, (void *) cs->cs_distinct);
  if (cs_cast_incompatible_dict (cs, n_distinct, cs->cs_distinct, sort_ids))
    {
      cs_swap_asc_dict (cs);
      cs_try_asc (cs, from, to);
      cs_swap_asc_dict (cs);
      cs->cs_dict_result = cs->cs_dict_output;
      return;
    }
  if (!cs->cs_dict)
    cs->cs_dict = hash_table_allocate (3 + n_distinct + (n_distinct >> 1));
  else
    clrhash (cs->cs_dict);
  for (inx = 0; inx < n_distinct; inx++)
    {
      sethash (cs->cs_distinct[sort_ids[inx]], cs->cs_dict, (void *) (ptrlong) inx);
    }
  out = cs->cs_dict_output;
  cs->cs_dict_fill = 5;
  cs_write_dict_head (cs, out, &cs->cs_dict_fill, &is_int, cs->cs_distinct, n_distinct, sort_ids, 0);
  for (inx = from; inx < to; inx++)
    {
      cs_bit_set (cs->cs_dict_output + cs->cs_dict_fill, inx - from, n_distinct, (ptrlong) gethash (cs->cs_values[inx],
	      cs->cs_dict));
    }
  cs->cs_dict_fill += n_distinct <= 16 ? ALIGN_2 (to - from) / 2 : (to - from);
  dk_free_box ((caddr_t) sort_ids_2);
  if (VEC_ANY == is_int)
    flags = CET_ANY;
  else if (VEC_ALL_STRINGS == is_int)
    flags = CET_CHARS;
  else if (64 == is_int)
    flags = CE_IS_64;
  if (is_int && IS_IRI_DTP ((dtp_t) cs->cs_values[from][0]))
    flags |= CE_IS_IRI;
  if (to - from > 255 || cs->cs_dict_fill - 5 > 255)
    {
      int fill = 0;
      cs_append_header (cs->cs_dict_output, &fill, CE_DICT | flags, to - from, cs->cs_dict_fill - 5);
      cs->cs_dict_result = cs->cs_dict_output;
    }
  else
    {
      int fill = 2;
      cs_append_header (cs->cs_dict_output, &fill, CE_DICT | flags, to - from, cs->cs_dict_fill - 5);
      cs->cs_dict_result = cs->cs_dict_output + 2;
    }
}

void
cs_reset_check (compress_state_t * cs)
{
  dk_set_t ready = cs->cs_ready_ces;
  int n_values = 0;
  cs_buf_mark_check (cs->cs_asc_output);
  cs_buf_mark_check (cs->cs_dict_output);
  while (ready)
    {
      db_buf_t ce = ready->data;
      if (ready == cs->cs_prev_ready_ces)
	break;
      n_values += ce_string_n_values (ce, box_length (ce) - 1);
      ready = ready->next;
    }
  if (n_values != cs->cs_n_values)
    GPF_T1 ("pre and post compress value counts do not match");
  cs->cs_prev_ready_ces = cs->cs_ready_ces;
}


void
cs_reset (compress_state_t * cs)
{
  cs_reset_check (cs);
  cs->cs_asc_fill = 0;
  cs->cs_dict_fill = 0;
  if (cs->cs_any_distinct)
    t_id_hash_clear (cs->cs_any_distinct);
  if (cs->cs_any_delta_distinct)
    t_id_hash_clear (cs->cs_any_delta_distinct);
  cs->cs_n_values = 0;
  cs->cs_no_dict = 0;
  cs->cs_heterogenous = 0;
  cs->cs_all_int = 0;
  cs->cs_any_64 = 0;
  cs->cs_non_comp_len = 0;
  cs->cs_unq_non_comp_len = 0;
  cs->cs_unq_delta_non_comp_len = 0;
  if (cs->cs_dict)
    clrhash (cs->cs_dict);
}


void
cs_free_allocd_parts (compress_state_t * cs)
{
  if (!cs)
    return;
  if (cs->cs_dict)
    hash_table_free (cs->cs_dict);
  cs->cs_dict = NULL;
}


int
cs_check_dict (compress_state_t * cs)
{
  int n_dist = cs->cs_any_distinct->ht_count;
  if ((CS_NO_DICT & cs->cs_exclude))
    return 0;
  if (cs->cs_unq_non_comp_len + cs->cs_n_values + 10 > cs->cs_non_comp_len)
    return 0;
  if (15 == n_dist)
    {
      cs->cs_asc_fill = 0;
      cs_try_asc (cs, 0, cs->cs_n_values);
      cs_buf_mark_check (cs->cs_asc_output);
      if (cs->cs_asc_fill < cs->cs_unq_non_comp_len + (cs->cs_n_values / 2))
	return 0;
      if (cs->cs_n_values > 1000)
	goto dict_now;
      return 0;
    }
  if (255 == n_dist)
    {
      int n, n_split, slice, any_vec_est, dict_est;
      cs->cs_asc_fill = 0;
      cs_try_asc (cs, 0, cs->cs_n_values);
      if (cs->cs_asc_fill < cs->cs_unq_non_comp_len + cs->cs_n_values)
	{
	  cs->cs_no_dict = 1;
	  return 0;
	}
      any_vec_est = cs->cs_asc_fill;
      dict_est = cs->cs_n_values + cs->cs_unq_delta_non_comp_len;
      if (any_vec_est < (dict_est + (dict_est / 10)))
	{
	  cs->cs_no_dict = 1;
	  return 0;
	}
      n_split = (dict_est / 3000) + 1;
      slice = cs->cs_n_values / n_split;
      for (n = 0; n < n_split; n++)
	{
	  int last = n < n_split - 1 ? (n + 1) * slice : cs->cs_n_values;
	  cs->cs_dict_fill = 0;
	  cs_dict (cs, n * slice, last);
	  if (cs->cs_dict_fill > 3000)
	    {
	      cs->cs_asc_fill = 0;
	      cs_try_asc (cs, n * slice, last);
	      if (cs->cs_asc_fill < cs->cs_dict_fill)
		t_set_push (&cs->cs_ready_ces, mp_box_n_chars (cs->cs_mp, (caddr_t) cs->cs_asc_output, cs->cs_asc_fill));
	      else
		t_set_push (&cs->cs_ready_ces, mp_box_n_chars (cs->cs_mp, (caddr_t) cs->cs_dict_result,
			cs->cs_dict_fill - (cs->cs_dict_result - cs->cs_dict_output)));
	    }
	  else
	    t_set_push (&cs->cs_ready_ces, mp_box_n_chars (cs->cs_mp, (caddr_t) cs->cs_dict_result,
		    cs->cs_dict_fill - (cs->cs_dict_result - cs->cs_dict_output)));
	}
      cs_reset (cs);
      return 1;
    }
dict_now:
  cs->cs_dict_fill = 0;
  cs_dict (cs, 0, cs->cs_n_values);
  t_set_push (&cs->cs_ready_ces, (void *) box_n_chars (cs->cs_dict_result,
	  cs->cs_dict_fill - (cs->cs_dict_result - cs->cs_dict_output)));
  if (cs->cs_for_test)
    t_set_push (&cs->cs_org_values, (void *) cs_org_values (cs));
  cs_reset (cs);
  return 1;
}


id_hashed_key_t anyhashf_head (char *strp);
int anyhashcmp_head (char *x, char *y);


void
cs_compress (compress_state_t * cs, caddr_t any)
{
  dtp_t dtp = dtp_canonical[((db_buf_t) any)[0]];
  int64 n = any_num_f ((db_buf_t) any);
  int64 hash = 1;
  int box_len = box_length (any) - 1;
  cs->cs_non_comp_len += box_len;
  if (cs->cs_is_asc || cs->cs_no_dict)
    mp_array_add (cs->cs_mp, &cs->cs_values, &cs->cs_n_values, any);
  else
    {
      caddr_t place;
      MHASH_VAR (hash, any, box_len);
      hash &= 0x7fffffff;
      place = id_hash_get_with_hash_number (cs->cs_any_distinct, (caddr_t) & any, hash);
      if (!place)
	{
	  if ((15 == cs->cs_any_distinct->ht_count || 255 == cs->cs_any_distinct->ht_count) && cs_check_dict (cs))
	    {
	      cs_compress (cs, any);
	      return;
	    }
	  if (255 == cs->cs_any_distinct->ht_count)
	    cs->cs_no_dict = 1;
	  else
	    t_id_hash_set_with_hash_number (cs->cs_any_distinct, (caddr_t) & any, (caddr_t) & any, hash);
	  mp_array_add (cs->cs_mp, &cs->cs_values, &cs->cs_n_values, any);
	  cs->cs_unq_non_comp_len += box_len;
	}
      else
	{
	  any = ((caddr_t *) place)[-1];
	  mp_array_add (cs->cs_mp, &cs->cs_values, &cs->cs_n_values, any);
	}
    }
  if (DV_LONG_INT == dtp || DV_IRI_ID == dtp)
    {
      if (IS_64_T (n, dtp))
	cs->cs_any_64 = CE_IS_64;
    }
  if (1 == cs->cs_n_values)
    {
      cs->cs_dtp = dtp;
      if (DV_LONG_INT == dtp || DV_IRI_ID == dtp)
	cs->cs_all_int = 1;
    }
  else
    {
      if (dtp != cs->cs_dtp)
	{
	  cs->cs_heterogenous = 1;
	  cs->cs_all_int = 0;
	  cs->cs_dtp = DV_UNKNOWN;
	}
    }
  if (cs->cs_n_values >= box_length (cs->cs_numbers) / sizeof (int64))
    {
      int64 *nv = (int64 *) mp_alloc_box_ni (cs->cs_mp, sizeof (int64) * (2 + (cs->cs_n_values * 2)), DV_BIN);
      memcpy (nv, cs->cs_numbers, sizeof (int64) * (cs->cs_n_values - 1));
      cs->cs_numbers = nv;
    }
  cs->cs_numbers[cs->cs_n_values - 1] = n;

  if (CE_VEC_MAX_VALUES == cs->cs_n_values)
    goto enough;

  if (!cs->cs_no_dict && cs->cs_unq_non_comp_len + cs->cs_n_values < 2500)
    return;
  if (cs->cs_all_int)
    return;
  if (cs->cs_non_comp_len < 2000)
    return;
  if (!cs->cs_any_delta_distinct)
    {
      int inx;
      cs->cs_any_delta_distinct = t_id_hash_allocate (cs->cs_n_values + 11, sizeof (caddr_t), 0, anyhashf_head, anyhashcmp_head);
      for (inx = 0; inx < cs->cs_n_values; inx++)
	{
	  caddr_t box = cs->cs_values[inx];
	  int box_len = box_length (box) - 2;
	  hash = 1;
	  MHASH_VAR (hash, box, box_len);
	  hash &= 0x7fffffff;
	  if (!id_hash_get_with_hash_number (cs->cs_any_delta_distinct, (caddr_t) & box, hash))
	    {
	      t_id_hash_set_with_hash_number (cs->cs_any_delta_distinct, (caddr_t) & box, (caddr_t) & box, hash);
	      cs->cs_unq_delta_non_comp_len += box_len;
	    }
	}
    }
  else
    {
      hash = 1;
      MHASH_VAR (hash, any, (box_len - 1));
      hash &= 0x7fffffff;
      if (!id_hash_get_with_hash_number (cs->cs_any_delta_distinct, (caddr_t) & any, hash))
	{
	  t_id_hash_set_with_hash_number (cs->cs_any_delta_distinct, (caddr_t) & any, (caddr_t) & any, hash);
	  cs->cs_unq_delta_non_comp_len += box_len - 1;
	}
    }
  if (cs->cs_unq_delta_non_comp_len /*+ cs->cs_n_values */  > 1850)
    goto enough;
  return;
enough:
  {
    db_buf_t best = NULL;
    int len;
    cs_best (cs, &best, &len);
    t_set_push (&cs->cs_ready_ces, (void *) best);
    cs_reset (cs);
  }
}

void
cs_check_over (dtp_t ** place, int *fill)
{
  if (*fill > CS_MAX_BYTES)
    {
      *place = NULL;
    }
}


void
cs_destroy (compress_state_t * cs)
{
  mp_free (cs->cs_mp);
  cs->cs_asc_output = NULL;
  cs->cs_dict_output = NULL;
  cs_reset (cs);
  if (cs->cs_dict)
    hash_table_free (cs->cs_dict);
  cs->cs_org_values = NULL;
}

int
anyhashcmp (char *x, char *y)
{
  caddr_t b1 = *(caddr_t *) x;
  caddr_t b2 = *(caddr_t *) y;
  int l1 = box_length (b1);
  if (l1 != box_length (b2))
    return 0;
  l1--;
  memcmp_8 (b1, b2, l1, neq);
  return 1;
neq:
  return 0;
}

id_hashed_key_t
anyhashf (char *strp)
{
  int64 h = 1;
  caddr_t box = *(caddr_t *) strp;
  int l = box_length (box) - 1;
  MHASH_VAR (h, box, l);
  return 0x7fffffff & h;
}

int
anyhashcmp_head (char *x, char *y)
{
  caddr_t b1 = *(caddr_t *) x;
  caddr_t b2 = *(caddr_t *) y;
  int l1 = box_length (b1);
  if (l1 != box_length (b2))
    return 0;
  l1 -= 2;
  memcmp_8 (b1, b2, l1, neq);
  return 1;
neq:
  return 0;
}

id_hashed_key_t
anyhashf_head (char *strp)
{
  int64 h = 1;
  caddr_t box = *(caddr_t *) strp;
  int l = box_length (box) - 2;
  MHASH_VAR (h, box, l);
  return 0x7fffffff & h;
}


void
cs_init (compress_state_t * cs, mem_pool_t * mp, int f, int sz)
{
  memset (cs, 0, sizeof (*cs));
  cs->cs_mp = mp;
  cs->cs_is_asc = f & 1;
  cs->cs_is_num = f & 2;
  SET_THR_TMP_POOL (cs->cs_mp);
  cs->cs_asc_output = (db_buf_t) t_alloc_box (CS_MAX_BYTES + 1, DV_STRING);
  cs->cs_dict_output = (db_buf_t) t_alloc_box (CS_MAX_BYTES + 1, DV_STRING);
  cs_buf_mark (cs->cs_asc_output);
  cs_buf_mark (cs->cs_dict_output);
  if (sz > 301)
    sz = 301;
  cs->cs_any_distinct = t_id_hash_allocate (sz, sizeof (caddr_t), 0, anyhashf, anyhashcmp);

  cs->cs_values = (caddr_t *) mp_alloc_box_ni (cs->cs_mp, 2048 * sizeof (caddr_t), DV_ARRAY_OF_POINTER);
  cs->cs_numbers = (int64 *) mp_alloc_box_ni (cs->cs_mp, 2048 * sizeof (int64), DV_BIN);
  SET_THR_TMP_POOL (NULL);
}

caddr_t
bif_cs_new (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  int f = bif_long_arg (qst, args, 0, "cs_new");
  compress_state_t *cs = (compress_state_t *) dk_alloc_box_zero (sizeof (compress_state_t), DV_STRING);
  cs_init (cs, mem_pool_alloc (), f, 300);
  return (caddr_t) cs;
}



caddr_t
bif_cs_compress (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  compress_state_t *cs = (compress_state_t *) bif_string_arg (qst, args, 0, "cs_compress");
  caddr_t x = bif_arg (qst, args, 1, "cs_compress");
  caddr_t err = NULL;
  SET_THR_TMP_POOL (cs->cs_mp);
  x = box_to_any (x, &err);
  x = t_box_copy (x);
  cs->cs_for_test = 1;
  cs_compress (cs, x);
  SET_THR_TMP_POOL (NULL);
  return NULL;
}


caddr_t
bif_cs_string (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  compress_state_t *cs = (compress_state_t *) bif_string_arg (qst, args, 0, "cs_string");
  dtp_t *best;
  int len, fill = 0;
  SET_THR_TMP_POOL (cs->cs_mp);
  cs_best (cs, &best, &len);
  t_set_push (&cs->cs_ready_ces, (void *) best);
  cs->cs_ready_ces = dk_set_nreverse (cs->cs_ready_ces);
  len = 0;
  DO_SET (caddr_t, ce, &cs->cs_ready_ces)
  {
    len += box_length (ce) - 1;
  }
  END_DO_SET ();
  best = dk_alloc_box (len + 1, DV_STRING);
  best[len] = 0;
  DO_SET (caddr_t, ce, &cs->cs_ready_ces)
  {
    int l = box_length (ce) - 1;
    memcpy (best + fill, ce, l);
    fill += l;
  }
  END_DO_SET ();
  cs_reset (cs);
  cs->cs_n_values = 0;
  SET_THR_TMP_POOL (NULL);
  return (caddr_t) best;
}


caddr_t
bif_cs_done (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  compress_state_t *cs = (compress_state_t *) bif_arg (qst, args, 0, "cs_string");
  if (DV_STRINGP (cs))
    cs_destroy (cs);
  return NULL;
}

caddr_t
cs_org_values (compress_state_t * cs)
{
  int inx;
  caddr_t *res = (caddr_t *) dk_alloc_box (sizeof (caddr_t) * cs->cs_n_values, DV_ARRAY_OF_POINTER);
  for (inx = 0; inx < cs->cs_n_values; inx++)
    res[inx] = box_deserialize_string (cs->cs_values[inx], box_length (cs->cs_values[inx]) - 1, 0);
  return (caddr_t) res;
}


caddr_t
bif_cs_values (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  compress_state_t *cs = (compress_state_t *) bif_string_arg (qst, args, 0, "cs_values");
  int inx;
  caddr_t *res;
  int len = 0, fill = 0;
  dk_set_push (&cs->cs_org_values, (void *) cs_org_values (cs));
  DO_SET (caddr_t, r, &cs->cs_org_values) len += BOX_ELEMENTS (r);
  END_DO_SET ();
  res = (caddr_t *) dk_alloc_box (sizeof (caddr_t) * len, DV_ARRAY_OF_POINTER);
  cs->cs_org_values = dk_set_nreverse (cs->cs_org_values);

  DO_SET (db_buf_t *, r, &cs->cs_org_values)
  {
    DO_BOX (db_buf_t, v, inx, r)
    {
      res[fill++] = box_copy (v);
    }
    END_DO_BOX;
  }
  END_DO_SET ();
  return (caddr_t) res;
}


caddr_t
bif_cs_decode (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  db_buf_t ce = (db_buf_t) bif_string_arg (qst, args, 0, "cs_decode");
  int n1 = bif_long_arg (qst, args, 1, "cs_decode");
  int n2 = bif_long_arg (qst, args, 2, "cs_decode");
  it_cursor_t itc_auto;
  it_cursor_t *itc = &itc_auto;
  int inx;
  mem_pool_t *mp = mem_pool_alloc ();
  col_pos_t cpo;
  data_col_t dc;
  caddr_t *res;
  int fill = 0;
  memset (&cpo, 0, sizeof (cpo));
  memset (&dc, 0, sizeof (data_col_t));
ITC_INIT (itc, NUL:NULL, NULL);
  if (n1 > n2)
    n1 = n2;
  res = (caddr_t *) dk_alloc_box ((n2 - n1) * sizeof (caddr_t), DV_ARRAY_OF_POINTER);
  itc->itc_n_matches = 0;
  cpo.cpo_itc = itc;
  cpo.cpo_string = ce;
  cpo.cpo_bytes = box_length (ce) - 1;

  dc.dc_type = DCT_BOXES | DCT_FROM_POOL;
  dc.dc_mp = mp;
  dc.dc_n_places = n2 - n1;
  dc.dc_values = (db_buf_t) mp_alloc (mp, sizeof (caddr_t) * (n2 - n1));
  cpo.cpo_dc = &dc;
  cpo.cpo_value_cb = ce_result;
  cpo.cpo_ce_op = NULL;
  cpo.cpo_pm = NULL;
  cs_decode (&cpo, n1, n2);
  for (inx = n1; inx < n2; inx++)
    res[fill++] = box_copy_tree (((caddr_t *) dc.dc_values)[inx - n1]);
  mp_free (mp);
  return (caddr_t) res;
}


void
ce_print_f (FILE * f, db_buf_t ce, int n1, int n2)
{
  it_cursor_t itc_auto;
  it_cursor_t *itc = &itc_auto;
  int inx, ctr = 0;
  mem_pool_t *mp = mem_pool_alloc ();
  int n_values = ce_n_values (ce);
  col_pos_t cpo;
  data_col_t dc;
  memset (&cpo, 0, sizeof (cpo));
  memset (&dc, 0, sizeof (data_col_t));
ITC_INIT (itc, NUL:NULL, NULL);
  itc->itc_n_matches = 0;
  cpo.cpo_itc = itc;
  if (n2 >= n_values)
    n2 = n_values;
  cpo.cpo_string = ce;
  cpo.cpo_bytes = ce_total_bytes (ce);

  dc.dc_type = DCT_BOXES | DCT_FROM_POOL;
  dc.dc_mp = mp;
  dc.dc_n_places = n2 - n1;
  dc.dc_values = (db_buf_t) mp_alloc (mp, sizeof (caddr_t) * (n2 - n1));
  cpo.cpo_dc = &dc;
  cpo.cpo_value_cb = ce_result;
  cpo.cpo_ce_op = NULL;
  cpo.cpo_pm = NULL;
  cs_decode (&cpo, n1, n2);
  for (inx = n1; inx < n2; inx++)
    {
      if (0 == (ctr % 10))
	fprintf (f, "%d: ", n1 + ctr);
      sqlo_box_print (((caddr_t *) dc.dc_values)[inx - n1]);
      if (0 == (ctr % 10))
	fprintf (f, "\n");
      ctr++;
    }
  fprintf (f, "\n");
  mp_free (mp);
}


void
ce_print (db_buf_t ce, int n1, int n2)
{
  ce_print_f (stdout, ce, n1, n2);
}


caddr_t *
cr_mp_array (col_data_ref_t * cr, mem_pool_t * mp, int from, int to, int print)
{
  it_cursor_t itc_auto;
  it_cursor_t *itc = &itc_auto;
  int nth_page, ce_row = 0, prev_fill, n, is_first = 1, first, end;
  int n_values = COL_NO_ROW == to ? cr_n_rows (cr) - from : to - from;
  col_pos_t cpo;
  data_col_t dc;
  memset (&cpo, 0, sizeof (cpo));
  memset (&dc, 0, sizeof (data_col_t));
ITC_INIT (itc, NUL:NULL, NULL);
  itc->itc_n_matches = 0;
  cpo.cpo_itc = itc;
  dc.dc_type = DCT_BOXES | DCT_FROM_POOL;
  dc.dc_mp = mp;
  dc.dc_n_places = n_values;
  dc.dc_values = (db_buf_t) mp_alloc_box (mp, sizeof (caddr_t) * n_values, DV_ARRAY_OF_POINTER);
  cpo.cpo_dc = &dc;
  cpo.cpo_value_cb = ce_result;

  for (nth_page = 0; nth_page < cr->cr_n_pages; nth_page++)
    {
      page_map_t *pm = cr->cr_pages[nth_page].cp_map;
      buffer_desc_t *buf = cr->cr_pages[nth_page].cp_buf;
      int limit = nth_page == cr->cr_n_pages - 1 ? cr->cr_limit_ce : pm->pm_count;
      int r;
      for (r = 0 == nth_page ? cr->cr_first_ce * 2 : 0; r < limit; r += 2)
	{
	  db_buf_t ce;
	  if (ce_row + pm->pm_entries[r + 1] < from)
	    {
	      ce_row += pm->pm_entries[r + 1];
	      continue;
	    }
	  if (is_first)
	    {
	      is_first = 0;
	      first = from - ce_row;
	    }
	  else
	    first = 0;
	  ce = BUF_ROW (buf, r);
	  cpo.cpo_string = ce;
	  cpo.cpo_ce_row_no = ce_row;
	  cpo.cpo_bytes = ce_total_bytes (ce);
	  cpo.cpo_ce_op = NULL;
	  cpo.cpo_pm = NULL;
	  n = ce_n_values (ce);
	  end = MIN (ce_row + n, to);
	  if (print)
	    {
	      printf ("ce t %d dtp %d values %d\n", (int) ce[0] & CE_TYPE_MASK, ce[0] & CE_DTP_MASK, n);
	    }
	  prev_fill = dc.dc_n_values;
	  cs_decode (&cpo, first + ce_row, end);
	  if (print)
	    {
	      int inx;
	      for (inx = prev_fill; inx < dc.dc_n_values; inx++)
		{
		  if (0 == (inx + first + ce_row) % 10)
		    printf ("%d: ", inx + first + ce_row);
		  sqlo_box_print (((caddr_t *) dc.dc_values)[inx]);
		}
	    }
	  ce_row += n;
	  if (ce_row > to)
	    goto done;
	}
    }
done:
  return (caddr_t *) dc.dc_values;
}


void
itc_cr_print (it_cursor_t * itc, int nth, int from, int to)
{
  mem_pool_t *mp = mem_pool_alloc ();
  cr_mp_array (itc->itc_col_refs[nth], mp, from, to, 1);
  mp_free (mp);
}

caddr_t *
itc_box_col_seg (it_cursor_t * itc, buffer_desc_t * buf, dbe_col_loc_t * cl)
{
  int inx, len, fill = 0, nth_page;
  caddr_t *seg;
  mem_pool_t *mp = mem_pool_alloc ();
  col_pos_t cpo;
  data_col_t dc;
  col_data_ref_t *cr = itc->itc_col_refs[cl->cl_nth - itc->itc_insert_key->key_n_significant];
  if (!cr)
    cr = itc->itc_col_refs[cl->cl_nth - itc->itc_insert_key->key_n_significant] = itc_new_cr (itc);
  memset (&cpo, 0, sizeof (cpo));
  memset (&dc, 0, sizeof (data_col_t));
  if (!cr->cr_is_valid)
    itc_fetch_col (itc, buf, cl, 0, COL_NO_ROW);
  len = cr_n_rows (cr);
  seg = (caddr_t *) dk_alloc_box_zero (sizeof (caddr_t) * len, DV_ARRAY_OF_POINTER);
  itc->itc_n_matches = 0;
  cpo.cpo_itc = itc;
  dc.dc_values = (db_buf_t) mp_alloc (mp, sizeof (caddr_t) * len);
  dc.dc_type = DCT_BOXES;
  dc.dc_mp = mp;
  dc.dc_n_places = len;
  cpo.cpo_dc = &dc;
  cpo.cpo_value_cb = ce_result;
  for (nth_page = 0; nth_page < cr->cr_n_pages; nth_page++)
    {
      page_map_t *pm = cr->cr_pages[nth_page].cp_map;
      int cinx, first = 0 == nth_page ? cr->cr_first_ce * 2 : 0;
      int last = nth_page == cr->cr_n_pages - 1 ? cr->cr_limit_ce : pm->pm_count, nv;
      for (cinx = first; cinx < last; cinx += 2)
	{
	  dc.dc_n_values = 0;
	  cpo.cpo_string = cr->cr_pages[nth_page].cp_string + pm->pm_entries[cinx];
	  cpo.cpo_bytes = ce_total_bytes (cpo.cpo_string);
	  cpo.cpo_ce_op = NULL;
	  nv = ce_n_values (cpo.cpo_string);
	  cpo.cpo_pm = NULL;
	  cs_decode (&cpo, 0, nv);
	  if (dc.dc_n_values != nv)
	    log_error ("decode count and ce value count differ");
	  for (inx = 0; inx < dc.dc_n_values; inx++)
	    seg[fill++] = ((caddr_t *) dc.dc_values)[inx];
	  if (fill > len)
	    GPF_T1 ("more stuff than len indicates in ce stat");
	}
    }
  mp_free (mp);
  itc_col_leave (itc, 0);
  return seg;
}


caddr_t
bif_cs_stats (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  dk_set_t res = NULL;
  db_buf_t str = (db_buf_t) bif_string_arg (qst, args, 0, "cs_stats");
  assertion_on_read_fail = 0;
  if (0 == setjmp_splice (&structure_fault_ctx))
    {
      DO_CE (ce, bytes, values, ce_type, flags, str, box_length ((caddr_t) str) - 1)
      {
	dk_set_push (&res, (caddr_t) list (4, box_num (bytes + __hl), box_num (values), box_num (ce_type),
		box_num (flags & CE_DTP_MASK)));
      }
      END_DO_CE (ce, n_bytes);
    }
  assertion_on_read_fail = 0;
  return list_to_array (dk_set_nreverse (res));
}


int
col_ac_set_dirty (caddr_t * qst, state_slot_t ** args, it_cursor_t * itc, buffer_desc_t * buf, int first, int n_last)
{
  int n_dirty;
  index_tree_t *it = buf->bd_tree;
  dbe_key_t *key = it->it_key;
  page_map_t *pm;
  int n_segs, r, p, icol;
  dk_hash_t *dist = hash_table_allocate (1000);
  if (args && BOX_ELEMENTS (args) < 2 || !key->key_is_col)
    return;
  if (DPF_INDEX != SHORT_REF (buf->bd_buffer + DP_FLAGS))
    return;
  if (buf->bd_storage)
    {
      ITC_IN_KNOWN_MAP (itc, buf->bd_page);
      itc_delta_this_buffer (itc, buf, DELTA_MAY_LEAVE);
      ITC_LEAVE_MAP_NC (itc);
    }
  pm = buf->bd_content_map;
  n_segs = pm->pm_count;
  if (!itc->itc_is_col)
    itc_col_init (itc);
  itc->itc_dive_mode = PA_WRITE;
  for (r = first; r < n_segs - n_last; r++)
    {
      itc->itc_map_pos = r;
      icol = 0;
      DO_CL (cl, key->key_row_var)
      {
	col_data_ref_t *cr = itc->itc_col_refs[icol];
	if (!cr)
	  cr = itc->itc_col_refs[icol] = itc_new_cr (itc);
	itc_fetch_col (itc, buf, cl, 0, COL_NO_ROW);
	for (p = 0; p < cr->cr_n_pages; p++)
	  {
	    buffer_desc_t *c_buf = cr->cr_pages[p].cp_buf;
	    sethash (DP_ADDR2VOID (c_buf->bd_page), dist, (void *) 1);
	    ITC_IN_KNOWN_MAP (itc, c_buf->bd_page);
	    itc_delta_this_buffer (itc, c_buf, DELTA_MAY_LEAVE);
	    ITC_LEAVE_MAP_NC (itc);
	  }
	icol++;
      }
      END_DO_CL;
      itc_col_leave (itc, 0);
    }
  n_dirty = dist->ht_count;
  hash_table_free (dist);
  return n_dirty;
}


#define add8(v)  \
  v = v + (v >> 32); \
v = v + (v >> 16); \
  v = v + (v >> 8);				\
  v &= 0x7f;

#define logc8(w, w2) \
  w2 = (w >> 1) & 0x5555555555555555;     \
  w &= 0x55555555; \
  w += w2; \
  w2 = (w >> 2) & 0x3333333333333333;   \
  w &= 0x33333333; \
  w += w2; \
  w2 = (w >> 4) & 0x0f0f0f0f0f0f0f0f;   \
  w &= 0x0f0f0f0f; \
  w += w2; \
  w += w >> 8; \
  w += w >> 16; \
  w += w >> 32; \
  w &= 0xff;

void
test_restr (int **res, int *a1, int *a2, int n)
{
  int inx;
  for (inx = 0; inx <= n - 4; inx += 4)
    {
      res[inx][0] = a1[inx] + a2[inx];
      res[inx + 1][0] = a1[inx + 1] + a2[inx + 1];
      res[inx + 2][0] = a1[inx + 2] + a2[inx + 2];
      res[inx + 3][0] = a1[inx + 3] + a2[inx + 3];
    }
}

void
test_restr2 (int **__restrict res, int *__restrict a1, int *__restrict a2, int n)
{
  int inx;
  for (inx = 0; inx <= n - 4; inx += 4)
    {
      res[inx][0] = a1[inx] + a2[inx];
      res[inx + 1][0] = a1[inx + 1] + a2[inx + 1];
      res[inx + 2][0] = a1[inx + 2] + a2[inx + 2];
      res[inx + 3][0] = a1[inx + 3] + a2[inx + 3];
    }
}


void
test_restr3 (int *res, int *a1, int n)
{
  int inx;
  for (inx = 0; inx <= n - 4; inx += 4)
    {
      res[inx] = a1[inx];
      res[inx + 1] = a1[inx + 1];
      res[inx + 2] = a1[inx + 2];
      res[inx + 3] = a1[inx + 3];
    }
}


void
test_restr4 (int *__restrict res, int *__restrict a1, int n)
{
  int inx;
  for (inx = 0; inx <= n - 4; inx += 4)
    {
      res[inx] = a1[inx];
      res[inx + 1] = a1[inx + 1];
      res[inx + 2] = a1[inx + 2];
      res[inx + 3] = a1[inx + 3];
    }
}


int
test_shift (unsigned char v, int i)
{
  return 0xf & (v >> ((i & 1) << 2));
}


int
test_shift2 (unsigned char v, int i)
{
  return i & 1 ? v >> 4 : v & 0x0f;
}

int
test_sw (int f, int i)
{
  switch (f & 7)
    {
    case 7:
      i += 1;
    case 6:
      i += 1;
    case 5:
      i += 1;
    case 4:
      i += 1;
    case 3:
      i += 2;
    case 2:
      i += 4;
    case 1:
      i += 5;
    case 0:
      i += 6;
    }
  return i;
}


int
miss_test (char *str, int len, int mask)
{
  uint32 i;
  int res = 0;
  for (i = 0; i < len; i++)
    {
      if (i & mask)
	res += str[i];
      else
	res -= str[i];
    }
  return res;
}

int
miss_test_2 (char *str, int len, int mask)
{
  uint32 i;
  int res = 0;
  for (i = 0; i < len; i++)
    {
      if (str[i] & mask)
	res += str[i];
      else
	res -= str[i];
    }
  return res;
}


int
miss_test_3 (char *str, int len, int mask)
{
  uint32 i;
  int res = 0;
  for (i = 0; i < len; i++)
    {
      res -= str[i];
    }
  return res;
}


int
mt4f (int n, int mask, int res)
{
  if (n & mask)
    res += n;
  else
    res -= n;
  return res;
}


int
miss_test_4 (char *str, int len, int mask)
{
  uint32 i;
  int res = 0;
  for (i = 0; i < len; i++)
    {
      res = mt4f (str[i], mask, res);
    }
  return res;
}


int
dveq (db_buf_t dv1, db_buf_t dv2)
{
  int len1, len2;
  DB_BUF_TLEN (len1, dv1[0], dv1);
  DB_BUF_TLEN (len2, dv2[0], dv2);
  if (len1 != len2)
    return 1;
#if defined(__GNUC__)
  return __builtin_memcmp (dv1, dv2, len1);
#else
  return memcmp (dv1, dv2, len1);
#endif
}


int
dveq2 (db_buf_t dv1, db_buf_t dv2)
{
  int len1, len2;
  DB_BUF_TLEN (len1, dv1[0], dv1);
  DB_BUF_TLEN (len2, dv2[0], dv2);
  if (len1 != len2)
    return 1;
  memcmp_8 (dv1, dv2, len1, neq);
  return 0;
neq:
  return 1;
}


int
dveq3 (db_buf_t dv1, db_buf_t dv2)
{
  uint64 w1 = *(int64 *) dv1;
  uint64 xo = w1 ^ *(int64 *) dv2;
  int l = db_buf_const_length[(dtp_t) (w1 >> 8)];
  if (l < 0)
    {
      l = 2 + (int) (dtp_t) (w1 >> 8);
    }
  else if (l == 0)
    {
      long l2, hl2;
      db_buf_length (dv1, &l2, &hl2);
      l = l2 + hl2;
    }
  if (l >= 8)
    {
      if (!xo)
	goto neq;
      dv1 += 8;
      dv2 += 8;
      l -= 8;
      memcmp_8 (dv1, dv2, l, neq);
    }
  else
    {
      if (xo & (((int64) 1 << (l << 3)) - 1))
	goto neq;
    }
  return 1;
neq:
  return 0;
}


int
dveq_test (int mode, int repeats)
{
  caddr_t err;
  caddr_t a1 = box_to_any (box_num (0x1234567890abcdef), &err);
  caddr_t a2 = box_to_any (box_num (0x1234567890abcdef), &err);
  caddr_t s1 = box_to_any (box_dv_short_string ("shimmy"), &err);
  caddr_t s2 = box_to_any (box_dv_short_string ("shimmy good"), &err);
  caddr_t s3 = box_to_any (box_dv_short_string ("shimmy goop"), &err);
  int s = 0, ctr;
  a1[12] = 1;
  a2[12] = 13;
  if (1 == mode)

    {
      for (ctr = 0; ctr < repeats; ctr++)
	{
	  s += dveq ((db_buf_t) a1, (db_buf_t) a1);
	  s += dveq ((db_buf_t) s1, (db_buf_t) s1);
	  s += dveq ((db_buf_t) s2, (db_buf_t) s3);
	}
    }
  if (2 == mode)
    {
      for (ctr = 0; ctr < repeats; ctr++)
	{
	  s += dveq2 ((db_buf_t) a1, (db_buf_t) a1);
	  s += dveq2 ((db_buf_t) s1, (db_buf_t) s1);
	  s += dveq2 ((db_buf_t) s2, (db_buf_t) s3);
	}
    }
  return s;
}

int
memcmp_inl (char *s1, char *s2, int l)
{
#if defined(__GNUC__)
  return __builtin_memcmp (s1, s2, l);
#else
  return memcmp (s1, s2, l);
#endif
}


void
memcpy_test (int mode, int cnt)
{
  int inx;
  caddr_t box1 = box_dv_short_string ("12345");
  caddr_t box1c = box_copy (box1);
  caddr_t box2 = box_iri_id (1234567890);
  caddr_t box2c = box_copy (box2);
  caddr_t box3 = box_dv_short_string ("1234567890123456789012345678901234567890123");
  caddr_t box3c = box_copy (box3);
  if (mode)
    {
      for (inx = 0; inx < cnt; inx++)
	{
	  memcpy (box1, box1c, box_length (box1));
	  memcpy (box2, box2c, box_length (box2));
	  memcpy (box3, box3c, box_length (box3));
	}
    }
  else
    {
      for (inx = 0; inx < cnt; inx++)
	{
	  memcpy_16 (box1, box1c, box_length (box1));
	  memcpy_16 (box2, box2c, box_length (box2));
	  memcpy_16 (box3, box3c, box_length (box3));
	}
    }
}



int gldummy;

void
test_const (it_cursor_t * itc)
{
  int inx;
  int *c = &gldummy;
  for (inx = itc->itc_ranges[0].r_first; inx < itc->itc_ranges[0].r_end; inx++)
    itc->itc_param_order[itc->itc_match_out++] = inx + *c;
}

typedef int int_xx;

void
test_const_1 (it_cursor_t * itc)
{
  int inx;
  int_xx *__restrict c = &gldummy;
  int *__restrict po = itc->itc_param_order;
  for (inx = itc->itc_ranges[0].r_first; inx < itc->itc_ranges[0].r_end; inx++)
    po[itc->itc_match_out++] = inx + *c;
}

void
test_const2 (it_cursor_t * __restrict itc)
{
  int inx;
  const row_range_t *const ranges = itc->itc_ranges;
  const int *c = &gldummy;
  for (inx = (const int) ranges[0].r_first; inx < (const int) ranges[0].r_end; inx++)
    itc->itc_matches[itc->itc_match_out++] = inx + *c;
}


void
test_const3 (it_cursor_t * itc)
{
  int inx;
  const row_range_t *ranges = itc->itc_ranges;
  const int *c = &gldummy;
  int end = ranges[0].r_end;
  for (inx = ranges[0].r_first; inx < end; inx++)
    itc->itc_matches[itc->itc_match_out++] = inx + *c;
}

int
test_if (int n)
{
  return 10 * (n < 2 ? 1 : n < 5 ? 3 : 7);
}


int
test_popcnt (unsigned long l)
{
#if defined(__GNUC__)
  return __builtin_popcountl (l);
#endif
  return 0;
}

#include "simd.h"
#include "mhash.h"


uint64
mhash64 (const void *key, int len, uint64 seed)
{
  uint64 h = seed ^ (len * MHASH_M);
  const unsigned char *data2;
  const uint64 *data = (const uint64 *) key;
  const uint64 *end = data + (len / 8);

  while (data != end)
    {
      uint64 k = *data++;

      k *= MHASH_M;
      k ^= k >> MHASH_R;
      k *= MHASH_M;
      h ^= k;
      h *= MHASH_M;
    }

  data2 = (const unsigned char *) data;

  switch (len & 7)
    {
    case 7:
      h ^= ((uint64) data2[6]) << 48;
    case 6:
      h ^= ((uint64) data2[5]) << 40;
    case 5:
      h ^= ((uint64) data2[4]) << 32;
    case 4:
      h ^= ((uint64) data2[3]) << 24;
    case 3:
      h ^= ((uint64) data2[2]) << 16;
    case 2:
      h ^= ((uint64) data2[1]) << 8;
    case 1:
      h ^= ((uint64) data2[0]);
      h *= MHASH_M;
    };

  h ^= h >> MHASH_R;
  h *= MHASH_M;
  h ^= h >> MHASH_R;
  return h;
}



#if defined(__GNUC__)
v2di_t mhash_r_v;
v2di_t mhash_m_v;

#define MHASH_STEP_V(h, data) \
{ \
  v2di_t k = data; \
      k *= mhash_m_v;  \
  tmp = __builtin_ia32_vpshlq (k, mhash_r_v);	\
      k ^= tmp; \
      k *= mhash_m_v;  \
      h ^= k; \
      h *= mhash_m_v; \
    }
#else
#define MHASH_STEP_V(h, data)
#endif


#if 0
void
vhtst (v2di_t data, v2di_t h)
{
  v2di_t tmp, k = data;
  k *= mhash_m_v;
  /* tmp = __builtin_ia32_vpshlq (k, mhash_r_v);*/
  k ^= tmp;
  k *= mhash_m_v;
  h ^= k;
  h *= mhash_m_v;
}
#endif


uint64 *hash_test_m;
int hash_test_sz = 2048 * 2048;


#define HASH_LOOK(h1) \
      {\
	uint64 d = hash_test_m[(h1 & 0xffffffff) / hash_test_sz]; \
	if (d & 1)\
	  d = hash_test_m[(h1 >> 32) / hash_test_sz];\
	res += d;\
      }


int
hash_test_1 (uint64 * in, int n)
{
  int res = 0;
  int i;
  for (i = 0; i < n; i++)
    {
      uint64 h1 = 1;
      MHASH_STEP (h1, in[i]);
      HASH_LOOK (h1);
    }
  return res;
}


int
hash_test_4 (uint64 * in, int n)
{
  int res = 0;
  int i;
  for (i = 0; i < n; i += 4)
    {
      uint64 h1 = 1, h2 = 1, h3 = 1, h4 = 1;
      MHASH_STEP (h1, in[i]);
      MHASH_STEP (h2, in[i + 1]);
      MHASH_STEP (h3, in[i + 2]);
      MHASH_STEP (h4, in[i + 3]);
      HASH_LOOK (h1);
      HASH_LOOK (h2);
      HASH_LOOK (h3);
      HASH_LOOK (h4);
    }
  return res;
}

v2di_u_t test_vs;

#if 0
int
hash_test_4v (uint64 * in, int n)
{
  v2di_t tmp;
  int res = 0;
  int i;
  long kl[2];
  kl[0] = kl[1] = MHASH_M;
  memcpy (&mhash_m_v, &kl, sizeof (kl));
  kl[0] = kl[1] = -47;
  memcpy (&mhash_r_v, &kl, sizeof (kl));
  test_vs.l[0] = -1;
  test_vs.l[1] = -1;
  /* tmp = __builtin_ia32_vpshlq (tmp, mhash_r_v); */
  for (i = 0; i < n; i += 4)
    {
      v2di_u_t h1, h2;
      v2di_u_t k1, k2;
      h1.l[0] = 1;
      h1.l[1] = 1;
      h2.l[0] = 1;
      h2.l[1] = 1;
      k1.l[0] = in[i];
      k1.l[1] = in[i + 1];
      k2.l[0] = in[i + 2];
      k2.l[1] = in[i + 3];
      MHASH_STEP_V (h1.v, k1.v);
      MHASH_STEP_V (h2.v, k2.v);
      HASH_LOOK (h1.l[0]);
      HASH_LOOK (h1.l[1]);
      HASH_LOOK (h2.l[0]);
      HASH_LOOK (h2.l[1]);
    }
  return res;
}
#endif


int
test_add (int64 * str, int n)
{
  int i, res = 0;
  for (i = 0; i < n; i++)
    res += str[i];
  return res;
}


int64
test_add_2 (int64 * str, int n)
{
  int i;
  int64 res = 0;
  for (i = 0; i < n; i++)
    res += INT64_REF_NA ((((caddr_t) str) + 8 * i));
  return res;
}


void
bzero16 (long *p, int n)
{
#if defined(__GNUC__)
  int i;
  v2di_u_t z;
  z.l[0] = 0;
  z.l[1] = 0;
  for (i = 0; i < n; i += 2)
    {
      *(v2di_t *) p = z.v;
      p += 2;
    }
#else
  memset ((void *) p, 0, n);
#endif
}


void
cpy16 (long *t, long *s, int n)
{
#if defined(__GNUC__)
  ptrlong t2 = (ptrlong) t;
  ptrlong s2 = (ptrlong) s;
  int i;
  t2 = (t2 + 16) & ~0xfLL;
  s2 = (s2 + 16) & ~0xfLL;
  t = (long *) (t2 + 8);
  s = (long *) (s2 + 8);
  for (i = 0; i < n; i++)
    {
      __builtin_ia32_storeups ((float *) t, __builtin_ia32_loadups ((float *) s));
      /* *(v2di_u_t*)t = *(v2di_u_t*)s; */
      s += 2;
      t += 2;
    }
#else
  GPF_T;
#endif
}

void
memcpy_c_inl (char *s1, char *s2, int l)
{
#if defined(__GNUC__)
  __builtin_memcpy (s1, s2, l);
#else
  memcpy (s1, s2, l);
#endif
}

void
memcpy_d_inl (long *s1, long *s2, int l)
{
#if defined(__GNUC__)
  __builtin_memcpy (s1, s2, l);
#else
  memcpy (s1, s2, l);
#endif
}


void
test_bzero (long *p, int n)
{
#if defined(__GNUC__)
  __builtin_memset (p, 0, 8 * n);
#endif
}



void
test_vecplus (double *res, double *d1, double *d2, int n)
{
  int i;
  for (i = 0; i < n; i += 2)
    {
      res[i] = d1[i] + d2[i];
      res[i + 1] = d1[i + 1] + d2[i + 1];
    }
}


int
test_cmp (char *s1, char *s2)
{
#if defined(__GNUC__)
  v2di_u_t r;
  r.v = *(v2di_t *) s1 - *(v2di_t *) s2;
  if (0 == (r.l[0] | r.l[1]))
    return 1;
#endif
  return 0;
}


int
test_cmp_2 (int64 * s1, int64 * s2)
{
  if (s1[0] == s2[0] && s1[1] == s2[1])
    return 1;
  return 0;
}

extern uint32 byte_bits[256];

int test_int_global;


int
test_bits (query_instance_t * qi, db_buf_t set_mask, int n_sets)
{
  int first_set = 0;
  int res = 0, set;
  SET_LOOP res += qi->qi_set;
  END_SET_LOOP;
  return res;
}

int
test_bits_2 (query_instance_t * qi, dtp_t * set_mask, int n_sets)
{
  int res = 0;
  int set;
  int byte, bytes = ALIGN_8 (n_sets) / 8;
  for (byte = 0; byte < bytes; byte++)
    {
      uint32 binx, bits = byte_bits[set_mask[byte]];
      int cnt = bits >> 28;
      for (binx = 0; binx < cnt; binx++)
	{
	  set = (byte * 8) + (bits & 7);
	  bits = bits >> 3;
	  qi->qi_set = set;
	  res += set;
	}
    }
  return res;
}




caddr_t
bif_rnd_string (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  caddr_t str;
  static int s;
  int len = bif_long_arg (qst, args, 0, "rnd_string");
  int inx;
  str = dk_alloc_box (len * 8, DV_STRING);
  for (inx = 0; inx < len; inx++)
    ((int64 *) str)[inx] = sqlbif_rnd (&s);
  return str;
}


int
crc_test (caddr_t str, int rep)
{
#if defined(__GNUC__) && defined (__builtin_ia32_crc32di)
  int inx, len = box_length (str) - 1;
  int64 h = 1;
  for (inx = 0; inx < rep; inx++)
    {
      caddr_t end = str + (len & ~7);
      caddr_t p = str;
      h = 1;
      while (p != end)
	{
	  h = __builtin_ia32_crc32di (h, *(int64 *) p);
	  p += 8;
	}
      if (len & 7)
	{
	  int64 n = *(int64 *) p;
	  n &= (1L << ((len & 7) << 3)) - 1;
	  h = __builtin_ia32_crc32di (h, n);
	}
    }
  return h;
#else
  return 0;
#endif
}


int
mhash_no_test (caddr_t str, int rep)
{
  int inx, len = box_length (str) - 1;
  int64 h = 1;
  for (inx = 0; inx < rep; inx++)
    {
      h = 1;
      MHASH_VAR (h, str, len);
    }
  return h;
}


caddr_t
bif_col_count_test (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
  /* test bit ops. op is 0 for rld count and 1 for bm count.  Add 2 for word op. add 1024 for non aligned */
  QNCAST (query_instance_t, qi, qst);
  db_buf_t str = (db_buf_t) bif_string_arg (qst, args, 0, "col_count_test");
  int op = bif_long_arg (qst, args, 1, "col_count_test");
  int repeat = bif_long_arg (qst, args, 2, "col_count_test");
  int len = box_length (str) - 1;
  int n, res = 0, res2 = 0, max = 0x7ffffff, inx;
  if (!hash_test_m)
    {
      int s = 0;
      int i;
      hash_test_m = (uint64 *) malloc (sizeof (int64) * hash_test_sz);
      for (i = 0; i < hash_test_sz; i++)
	hash_test_m[i] = sqlbif_rnd (&s);
    }
  if (1024 & op)
    {
      str++;
      len--;
      op &= ~1024;
    }
  for (n = 0; n < repeat; n++)
    {
      int point = 0;
      res2 = res = 0;
      switch (op)
	{
	case 2:
	  for (point = 0; point < (len & 0xffff8); point += 8)
	    {
	      unsigned int64 wo = *((int64 *) (str + point));
	      unsigned int64 w = wo & 0x0f0f0f0f0f0f0f0f, w2 = (wo >> 4) & 0x0f0f0f0f0f0f0f0f;
	      add8 (w);
	      add8 (w2);
	      res += w;
	      res2 += w2;
	    }
	case 0:
	  for (inx = point; inx < len; inx++)
	    {
	      dtp_t byte = str[inx];
	      res += byte & 0xf;
	      res2 += byte >> 4;
	      if (res2 > max)
		break;
	    }
	  break;
	case 8:
	  for (point = 0; point < (len & 0xffff8); point += 8)
	    {
	      res += byte_logcount[str[point]]
		  + byte_logcount[str[point + 1]]
		  + byte_logcount[str[point + 2]]
		  + byte_logcount[str[point + 3]]
		  + byte_logcount[str[point + 4]]
		  + byte_logcount[str[point + 5]] + byte_logcount[str[point + 6]] + byte_logcount[str[point + 7]];
	    }
	  goto logc;
	case 3:
	  for (point = 0; point < (len & 0xffff8); point += 8)
	    {
	      unsigned int64 w = *((int64 *) (str + point)), w2;
	      w2 = (w >> 1) & 0x5555555555555555;
	      w &= 0x5555555555555555;
	      w += w2;
	      w2 = (w >> 2) & 0x3333333333333333;
	      w &= 0x3333333333333333;
	      w += w2;
	      w2 = (w >> 4) & 0x0f0f0f0f0f0f0f0f;
	      w &= 0x0f0f0f0f0f0f0f0f;
	      w += w2;
	      w += w >> 8;
	      w += w >> 16;
	      w += w >> 32;
	      w &= 0xff;

	      res += w;
	    }
	case 1:
	logc:
	  for (inx = point; inx < len; inx++)
	    {
	      res += byte_logcount[str[inx]];
	      if (res > max)
		break;
	    }
	case 128:
	  {
	    unsigned int64 w;
	    w = ((int64 *) str)[0];
	    w = (w >> 32) | (w << 32);
	    w = ((w & 0xffff0000ffff0000) >> 16) | ((w & 0x0000ffff0000ffff) << 16);
	    w = ((w & 0xff00ff00ff00ff00) >> 8) | ((w & 0x00ff00ff00ff00ff) << 8);
	    res += w;
	    w = ((int64 *) str)[1];
	    w = (w >> 32) | (w << 32);
	    w = ((w & 0xffff0000ffff0000) >> 16) | ((w & 0x0000ffff0000ffff) << 16);
	    w = ((w & 0xff00ff00ff00ff00) >> 8) | ((w & 0x00ff00ff00ff00ff) << 8);
	    res += w;
	    break;
	  }
	case 64:
	  res = INT64_REF_NA (str) + INT64_REF_NA (str + 8);
	  break;
	case 32:
	  res = ((int64 *) str)[0] + ((int64 *) str)[1];
	  break;
	case 10:
	  test_bzero (str, MIN (len - 16, 0) / 8);
	  break;
	case 11:
	  bzero16 (str, MIN (0, (len / 8) - 2));
	  break;
	case 12:
	  len = MAX (0, len - 16);
	  cpy16 (str, str + (len / 2), len / 32);
	  break;
	case 13:
	  len = MAX (0, len - 16);
	  memcpy_c_inl (str, str + (len / 2), len / 2);
	  break;
	case 14:
	  res = test_bits (qi, str, 8 * len);
	  break;
	case 15:
	  res = test_bits_2 (qi, str, 8 * len);
	  break;
	case 16:
	  hash_test_1 (str, len / 8);
	  break;
	case 17:
	  hash_test_4 (str, len / 8);
	  break;
	case 18:
	  test_add (str, len / 8);
	  break;
	case 19:
	  test_add_2 (str, len / 8);
	  break;
	case 20:
	  miss_test (str, len, 1);
	  break;
	case 21:
	  miss_test (str, len, 32);
	  break;
	case 22:
	  miss_test_2 (str, len, 32);
	  break;
	case 23:
	  miss_test_3 (str, len, 32);
	  break;
	case 24:
	  miss_test_4 (str, len, 32);
	  break;
	case 25:
	  res = dveq_test (1, repeat);
	  break;
	case 26:
	  res = dveq_test (2, repeat);
	  break;
	case 27:
	  memcpy_test (0, repeat);
	  break;
	case 29:
	  memcpy_test (1, repeat);
	  break;
	case 30:
	  crc_test (str, repeat);
	  break;
	case 31:
	  mhash_no_test (str, repeat);
	  break;


	}
    }
  return box_num (res + 100000 * res2);
}


caddr_t
bif_string_test (caddr_t * qst, caddr_t * err_ret, state_slot_t ** args)
{
#if defined(__GNUC__) && defined (__builtin_ia32_pcmpestri128)
  /* test bit ops. op is 0 for rld count and 1 for bm count.  Add 2 for word op. add 1024 for non aligned */
  QNCAST (query_instance_t, qi, qst);
  db_buf_t str1 = (db_buf_t) bif_string_arg (qst, args, 0, "col_count_test");
  db_buf_t str2 = (db_buf_t) bif_string_arg (qst, args, 1, "col_count_test");
  int op = bif_long_arg (qst, args, 2, "str_test");
  int repeat = bif_long_arg (qst, args, 3, "str_test");
  int len1 = box_length (str1) - 1;
  int len2 = box_length (str2) - 1;
  int inx;
  int res[16];
  if (op & 4)
    {
      str1++;
      str2++;
      len1--;
      len2--;
    }
  for (inx = 0; inx < repeat; inx++)
    {
      res[inx & 0xf] =
	  __builtin_ia32_pcmpestri128 (*(v16qi_t *) str1, len1, *(v16qi_t *) str2, len2, PSTR_EQUAL_EACH | PSTR_NEGATIVE_POLARITY);
      str1++;
      str2++;
      len1--;
      len2--;
    }
  return res;
#else
  return 0;
#endif
}


int
is_prime (int n)
{
  int try, sq = (int) sqrt ((float) n);
  for (try = 3; try < sq; try += 2)
    if (0 == (n % try))
      return 0;
  return 1;
}

int ce_op_fill = 1;
ce_op_desc_t ce_op_desc[256];
ce_op_t *ce_op[512];		/* the even is the op for ranges, the odd is the same for sets */


void
ce_op_register (dtp_t ce_type, int op, int is_sets, ce_op_t f)
{
  int inx;
  for (inx = 0; inx < ce_op_fill; inx++)
    {
      if (op == ce_op_desc[inx].ced_op)
	goto found;
    }
  ce_op_desc[inx].ced_op = op;
  ce_op_fill++;
found:
  inx = inx * 2 + is_sets;
  if (!ce_op[inx])
    {
      ce_op[inx] = (ce_op_t *) dk_alloc (sizeof (caddr_t) * 128);
      memset (ce_op[inx], 0, 128 * sizeof (caddr_t));
    }
  ce_op[inx][ce_type] = f;
}


int
col_find_op (int op)
{
  int inx;
  for (inx = 0; inx < ce_op_fill; inx++)
    if (op == ce_op_desc[inx].ced_op)
      return inx;
  return 0;
}


void
col_init ()
{
  bif_define ("cs_new", bif_cs_new);
  bif_define ("cs_compress", bif_cs_compress);
  bif_define ("cs_string", bif_cs_string);
  bif_define ("cs_done", bif_cs_done);
  bif_define ("cs_decode", bif_cs_decode);
  bif_define ("cs_values", bif_cs_values);
  bif_define ("cs_stats", bif_cs_stats);
  bif_define ("col_count_test", bif_col_count_test);
  bif_define ("rnd_string", bif_rnd_string);
  colin_init ();
}