/*
 *  sqlvrun.c
 *
 *  $Id$
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
#include "xmlnode.h"
#include "sqlfn.h"
#include "sqlcomp.h"
#include "lisprdr.h"
#include "sqlopcod.h"
#include "security.h"
#include "sqlbif.h"
#include "sqltype.h"
#include "libutil.h"
#include "aqueue.h"
#include "arith.h"


extern int32 enable_qp;
int enable_split_range = 1;
int enable_split_sets = 1;
int32 enable_dyn_batch_sz = 0;
int qp_thread_min_usec = 5000;
int qp_range_split_min_rows = 20;
int dc_init_sz = 10000;
int dc_default_var_len = 8;
int32 dc_batch_sz = 10000;
int32 dc_max_batch_sz = 1000000 /*(1024 * 1024 * 4)  - 16 */ ;
int dc_str_buf_unit = 0x10000;

search_spec_t *sp_list_copy (search_spec_t * sp);


data_col_t *
bif_dc_arg (caddr_t * qst, state_slot_t ** args, int nth, char *name)
{
  state_slot_t *ssl;
  if (((uint32) nth) >= BOX_ELEMENTS (args))
    sqlr_new_error ("22003", "SR030", "Too few (only %d) arguments for %s.", (int) (BOX_ELEMENTS (args)), name);
  ssl = args[nth];
  if (SSL_VEC != ssl->ssl_type)
    sqlr_new_error ("42000", "VEC..", "%s vectored applied to non vector arg", name);
  return ((data_col_t **) qst)[ssl->ssl_index];
}


int
dc_int_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  int64 i1 = ((int64 *) dc->dc_values)[r1];
  int64 i2 = ((int64 *) dc->dc_values)[r2];
  __builtin_prefetch (&((int64 *) dc->dc_values)[r_prefetch]);
  return NUM_COMPARE (i1, i2);
}


int
dc_float_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  float i1 = ((float *) dc->dc_values)[r1];
  float i2 = ((float *) dc->dc_values)[r2];
  __builtin_prefetch (&((float *) dc->dc_values)[r_prefetch]);
  return NUM_COMPARE (i1, i2);
}

int
dc_iri_id_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  iri_id_t i1 = ((iri_id_t *) dc->dc_values)[r1];
  iri_id_t i2 = ((iri_id_t *) dc->dc_values)[r2];
  __builtin_prefetch (&((iri_id_t *) dc->dc_values)[r_prefetch]);
  return NUM_COMPARE (i1, i2);
}


#define NULL_CMP \
  if (dc->dc_any_null)  \
    { \
      int nu1 = BIT_IS_SET (dc->dc_nulls, r1); \
      int nu2 = BIT_IS_SET (dc->dc_nulls, r2); \
      if (nu1 || nu2) \
	return nu1 && nu2 ? DVC_MATCH : nu1 ? DVC_LESS : DVC_GREATER; \
    }

int
dc_int_null_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  int64 i1 = ((int64 *) dc->dc_values)[r1];
  int64 i2 = ((int64 *) dc->dc_values)[r2];
  __builtin_prefetch (&((int64 *) dc->dc_values)[r_prefetch]);
  NULL_CMP;
  return NUM_COMPARE (i1, i2);
}


int
dc_float_null_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  float i1 = ((float *) dc->dc_values)[r1];
  float i2 = ((float *) dc->dc_values)[r2];
  __builtin_prefetch (&((float *) dc->dc_values)[r_prefetch]);
  NULL_CMP;
  return NUM_COMPARE (i1, i2);
}

int
dc_iri_id_null_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  iri_id_t i1 = ((iri_id_t *) dc->dc_values)[r1];
  iri_id_t i2 = ((iri_id_t *) dc->dc_values)[r2];
  __builtin_prefetch (&((iri_id_t *) dc->dc_values)[r_prefetch]);
  NULL_CMP;
  return NUM_COMPARE (i1, i2);
}


int
dc_any_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  db_buf_t i1 = ((db_buf_t *) dc->dc_values)[r1];
  db_buf_t i2 = ((db_buf_t *) dc->dc_values)[r2];
  __builtin_prefetch (((db_buf_t *) dc->dc_values)[r_prefetch]);
  if (DV_RDF == *i1 && DV_RDF == *i2)
    return dv_rdf_dc_compare (i1, i2) & ~DVC_NOORDER;
  return dv_compare (i1, i2, NULL, 0) & ~DVC_NOORDER;
}


int
dc_datetime_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  db_buf_t i1 = ((db_buf_t) dc->dc_values) + DT_LENGTH * r1;
  db_buf_t i2 = ((db_buf_t) dc->dc_values) + DT_LENGTH * r2;
  int rc;
  __builtin_prefetch (dc->dc_values + DT_LENGTH * r_prefetch);
  rc = memcmp (i1, i2, DT_COMPARE_LENGTH);
  return rc < 0 ? DVC_LESS : 0 == rc ? DVC_MATCH : DVC_GREATER;
}


int
dc_datetime_null_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  db_buf_t i1 = ((db_buf_t) dc->dc_values) + DT_LENGTH * r1;
  db_buf_t i2 = ((db_buf_t) dc->dc_values) + DT_LENGTH * r2;
  int rc;
  __builtin_prefetch (dc->dc_values + DT_LENGTH * r_prefetch);
  NULL_CMP;
  rc = memcmp (i1, i2, DT_COMPARE_LENGTH);
  return rc < 0 ? DVC_LESS : 0 == rc ? DVC_MATCH : DVC_GREATER;
}

int
dc_box_cmp (data_col_t * dc, int r1, int r2, int r_prefetch)
{
  caddr_t i1 = ((caddr_t *) dc->dc_values)[r1];
  caddr_t i2 = ((caddr_t *) dc->dc_values)[r2];
  int rc = key_cmp_boxes (i1, i2, &dc->dc_sqt);
  __builtin_prefetch (((caddr_t *) dc->dc_values)[r_prefetch]);

  if (DVC_DTP_LESS == rc)
    return DVC_LESS;
  if (DVC_DTP_GREATER == rc)
    return DVC_GREATER;
  return rc;
}


char vec_box_dtps[256];

void
vec_dtp_init ()
{
  int inx;
  vec_box_dtps[DV_NUMERIC] = 1;
  vec_box_dtps[DV_XML_ENTITY] = 1;
  vec_box_dtps[DV_BLOB] = 1;
  vec_box_dtps[DV_BLOB_BIN] = 1;
  vec_box_dtps[DV_BLOB_WIDE] = 1;
  vec_box_dtps[DV_ITC] = 1;
  vec_box_dtps[DV_PLACEHOLDER] = 1;
  vec_box_dtps[DV_CLRG] = 1;
  vec_box_dtps[DV_CLOP] = 1;
  vec_box_dtps[DV_INDEX_TREE] = 1;
  vec_box_dtps[DV_CUSTOM] = 1;
  vec_box_dtps[DV_OBJECT] = 1;
  vec_box_dtps[DV_ARRAY_OF_POINTER] = 1;
  for (inx = DV_ANY_FIRST; inx < 256; inx++)
    dv_ce_dtp[inx] = DV_ANY;
  dv_ce_dtp[DV_SHORT_INT] = DV_LONG_INT;
  dv_ce_dtp[DV_LONG_INT] = DV_LONG_INT;
  dv_ce_dtp[DV_INT64] = DV_LONG_INT;
  dv_ce_dtp[DV_IRI_ID] = DV_IRI_ID;
  dv_ce_dtp[DV_IRI_ID_8] = DV_IRI_ID;
  dv_ce_dtp[DV_SINGLE_FLOAT] = DV_SINGLE_FLOAT;
  dv_ce_dtp[DV_DOUBLE_FLOAT] = DV_DOUBLE_FLOAT;
  dv_ce_dtp[DV_DATETIME] = DV_DATETIME;
}


void
dc_set_flags (data_col_t * dc, sql_type_t * sqt, dtp_t dcdtp)
{
  dc->dc_sqt.sqt_non_null = sqt->sqt_non_null;
  switch (dtp_canonical[sqt->sqt_dtp])
    {
    case DV_LONG_INT:
    case DV_DOUBLE_FLOAT:
      dc->dc_type = DCT_NUM_INLINE;
      dc->dc_sort_cmp = dc->dc_sqt.sqt_non_null ? dc_int_cmp : dc_int_null_cmp;
      break;
    case DV_IRI_ID:
      dc->dc_type = DCT_NUM_INLINE;
      dc->dc_sort_cmp = dc->dc_sqt.sqt_non_null ? dc_iri_id_cmp : dc_iri_id_null_cmp;
      break;
    case DV_SINGLE_FLOAT:
      dc->dc_type = DCT_NUM_INLINE;
      dc->dc_sort_cmp = dc->dc_sqt.sqt_non_null ? dc_float_cmp : dc_float_null_cmp;
      break;
    case DV_DATETIME:
    case DV_DATE:
    case DV_TIME:
    case DV_TIMESTAMP:
      dc->dc_dtp = DV_DATETIME;
      dc->dc_type = 0;
      dc->dc_sort_cmp = dc->dc_sqt.sqt_non_null ? dc_datetime_cmp : dc_datetime_null_cmp;
      break;
    case DV_ANY:
      /* only set if dc_dtp does not contraditc, if it does, control falls through to default case */
      if (0 == dcdtp || DV_ANY == dcdtp)
	{
	  dc->dc_dtp = DV_ANY;
	  dc->dc_type = 0;
	  dc->dc_sort_cmp = dc_any_cmp;
	  break;
	}
    default:
      dc->dc_type = DCT_BOXES;
      dc->dc_sort_cmp = dc_box_cmp;
      break;
    }
  if (!dc->dc_sort_cmp)
    dc->dc_sort_cmp = dc_int_cmp;
}


data_col_t *
mp_data_col_large (mem_pool_t * mp, state_slot_t * ssl, int n_sets)
{
  sql_type_t *sqt = &ssl->ssl_sqt;
  int len = sqt_fixed_length (sqt);
  data_col_t *dc;
  int is_box = vec_box_dtps[ssl->ssl_dtp] | vec_box_dtps[ssl->ssl_dc_dtp];
  if ((-1 == len || DV_ANY == ssl->ssl_dc_dtp) && !is_box)
    {
      dc = (data_col_t *) mp_alloc_box_ni (mp, sizeof (data_col_t), DV_DATA);
      memset (dc, 0, sizeof (data_col_t));
      dc->dc_mp = mp;
      dc->dc_values = (db_buf_t) mp_alloc_box (mp, 8 + (n_sets * sizeof (caddr_t)), DV_NON_BOX);
      dc->dc_values = (db_buf_t) ALIGN_16 ((ptrlong) dc->dc_values);
      dc->dc_buf_len = 0x10000;
      dc->dc_buffer = (db_buf_t) mp_alloc_box (mp, 0x10000, DV_BIN);
      dc->dc_sort_cmp = dc_any_cmp;
      mp_set_push (dc->dc_mp, &dc->dc_buffers, (void *) dc->dc_buffer);
    }
  else
    {
      if (len < 0)
	len = sizeof (caddr_t);
      if (len < sizeof (boxint))
	len = sizeof (boxint);
      dc = (data_col_t *) mp_alloc_box_ni (mp, sizeof (data_col_t), DV_DATA);
      memset (dc, 0, sizeof (data_col_t));
      dc->dc_mp = mp;
      dc_set_flags (dc, sqt, ssl->ssl_dc_dtp);
      dc->dc_values = (db_buf_t) mp_alloc_box (mp, 8 + (n_sets * len), DV_NON_BOX);
      dc->dc_values = (db_buf_t) ALIGN_16 ((ptrlong) dc->dc_values);
    }
  if (ssl->ssl_dc_dtp)
    dc->dc_dtp = ssl->ssl_dc_dtp;
  else
    dc->dc_dtp = ssl->ssl_sqt.sqt_dtp;
  dc->dc_dtp = dtp_canonical[dc->dc_dtp];
  if (ssl->ssl_sqt.sqt_col_dtp)
    dc->dc_sqt.sqt_col_dtp = ssl->ssl_sqt.sqt_col_dtp;
  dc->dc_n_places = n_sets;
  return dc;
}


data_col_t *
mp_data_col (mem_pool_t * mp, state_slot_t * ssl, int n_sets)
{
  sql_type_t *sqt = &ssl->ssl_sqt;
  int len = sqt_fixed_length (sqt);
  data_col_t *dc;
  int is_box = vec_box_dtps[ssl->ssl_dtp] | vec_box_dtps[ssl->ssl_dc_dtp];
  if (n_sets > 20000)
    return mp_data_col_large (mp, ssl, n_sets);
  if ((-1 == len || DV_ANY == ssl->ssl_dc_dtp) && !is_box)
    {
      db_buf_t ptr;
      int bytes = ALIGN_8 (sizeof (data_col_t)) + sizeof (caddr_t) * n_sets + n_sets * dc_default_var_len + 16;
      dc = (data_col_t *) mp_alloc_box_ni (mp, bytes, DV_DATA);
      memset (dc, 0, sizeof (data_col_t));
      dc->dc_mp = mp;
      dc->dc_values = ((db_buf_t) dc) + ALIGN_8 (sizeof (data_col_t));
      dc->dc_values = (db_buf_t) ALIGN_16 ((ptrlong) dc->dc_values);
      dc->dc_buf_len = n_sets * dc_default_var_len;
      dc->dc_buffer = dc->dc_values + sizeof (caddr_t) * n_sets + 8;
      dc->dc_sort_cmp = dc_any_cmp;
      ptr = dc->dc_buffer - 4;
      WRITE_BOX_HEADER (ptr, n_sets * dc_default_var_len, DV_STRING);
      mp_set_push (dc->dc_mp, &dc->dc_buffers, (void *) dc->dc_buffer);
    }
  else
    {
      int bytes;
      if (len < 0)
	len = sizeof (caddr_t);
      if (len < sizeof (boxint))
	len = sizeof (boxint);
      bytes = 16 + ALIGN_8 (sizeof (data_col_t)) + len * n_sets;
      dc = (data_col_t *) mp_alloc_box_ni (mp, bytes, DV_DATA);
      memset (dc, 0, sizeof (data_col_t));
      dc->dc_mp = mp;
      dc_set_flags (dc, sqt, ssl->ssl_dc_dtp);
      dc->dc_values = ((db_buf_t) dc) + ALIGN_8 (sizeof (data_col_t));
      dc->dc_values = (db_buf_t) ALIGN_16 ((ptrlong) dc->dc_values);
    }
  if (ssl->ssl_dc_dtp)
    dc->dc_dtp = ssl->ssl_dc_dtp;
  else
    dc->dc_dtp = ssl->ssl_sqt.sqt_dtp;
  dc->dc_dtp = dtp_canonical[dc->dc_dtp];
  if (ssl->ssl_sqt.sqt_col_dtp)
    dc->dc_sqt.sqt_col_dtp = ssl->ssl_sqt.sqt_col_dtp;
  dc->dc_n_places = n_sets;
  return dc;
}


int *
qn_extend_sets (data_source_t * qn, caddr_t * inst, int n)
{
  QNCAST (query_instance_t, qi, inst);
  int *prev = QST_BOX (int *, inst, qn->src_sets);
  int fill = QST_INT (inst, qn->src_out_fill);
  caddr_t box;
  n = MIN (n, dc_max_batch_sz);
  box = QST_BOX (caddr_t, inst, qn->src_sets) = mp_alloc_box_ni (qi->qi_mp, sizeof (int) * n, DV_BIN);
  memcpy_16 (box, prev, fill * sizeof (int));
  return (int *) box;
}


void
qi_vec_init_nodes (query_instance_t * qi, int n_sets, query_t * qr)
{
  QNCAST (caddr_t, inst, qi);
  DO_SET (data_source_t *, qn, &qr->qr_nodes)
  {
    if (qn->src_sets)
      QST_BOX (int *, inst, qn->src_sets) = (int *) mp_alloc_box_ni (qi->qi_mp, n_sets * sizeof (int), DV_BIN);
    if (IS_QN (qn, subq_node_input))
      qi_vec_init_nodes (qi, n_sets, ((subq_source_t *) qn)->sqs_query);
  }
  END_DO_SET ();
  DO_SET (query_t *, sq, &qr->qr_subq_queries) qi_vec_init_nodes (qi, n_sets, sq);
  END_DO_SET ();
}


void
qi_vec_init (query_instance_t * qi, int n_sets)
{
  QNCAST (caddr_t, inst, qi);
  int inx, n_dcs;
  query_t *qr = qi->qi_query;
  mem_pool_t *mp;
  mp = qi->qi_mp = mem_pool_alloc ();
  if (n_sets <= 0)
    n_sets = dc_batch_sz;
  n_dcs = BOX_ELEMENTS (qi->qi_query->qr_vec_ssls);
  qi->qi_mp->mp_block_size = sizeof (caddr_t) * (dc_batch_sz + 50) * MIN (n_dcs, 10);
  DO_BOX (state_slot_t *, ssl, inx, qi->qi_query->qr_vec_ssls)
  {
    QST_GET_V (inst, ssl) = (caddr_t) mp_data_col (mp, ssl, n_sets);
    if (ssl->ssl_sets)
      QST_BOX (int *, inst, ssl->ssl_sets) = (int *) mp_alloc_box_ni (qi->qi_mp, n_sets * sizeof (int), DV_BIN);
  }
  END_DO_BOX;
  qi_vec_init_nodes (qi, n_sets, qr);
}


void
itc_vec_box (it_cursor_t * itc, dtp_t dtp, int nth, data_col_t * dc)
{
  /* if it is dct_boxes then go to default case */
  if (dc->dc_type & DCT_BOXES)
    goto boxes;
  switch (dtp)
    {
    case DV_LONG_INT:
    case DV_SHORT_INT:
    case DV_INT64:
      dtp = DV_LONG_INT;
    case DV_IRI_ID:
    case DV_IRI_ID_8:
    case DV_SINGLE_FLOAT:
    case DV_DOUBLE_FLOAT:
      if (DV_IRI_ID_8 == dtp)
	dtp = DV_IRI_ID;
      if (itc->itc_search_par_fill <= nth || !itc->itc_search_params[nth])
	{
	  itc->itc_search_params[nth] = dk_alloc_box (sizeof (double), dtp);
	  ITC_OWNS_PARAM (itc, itc->itc_search_params[nth]);
	  if (itc->itc_search_par_fill <= nth)
	    itc->itc_search_par_fill = nth + 1;
	}
      break;
    case DV_NUMERIC:
      if (itc->itc_search_par_fill <= nth || !itc->itc_search_params[nth])
	{
	  itc->itc_search_params[nth] = (caddr_t) numeric_allocate ();
	  ITC_OWNS_PARAM (itc, itc->itc_search_params[nth]);
	  if (itc->itc_search_par_fill <= nth)
	    itc->itc_search_par_fill = nth + 1;
	}
      break;
    case DV_DATETIME:
    case DV_TIMESTAMP:
    case DV_DATE:
    case DV_TIME:
      if (itc->itc_search_par_fill <= nth || !itc->itc_search_params[nth])
	{
	  itc->itc_search_params[nth] = dk_alloc_box (DT_LENGTH, DV_DATETIME);
	  ITC_OWNS_PARAM (itc, itc->itc_search_params[nth]);
	  if (itc->itc_search_par_fill <= nth)
	    itc->itc_search_par_fill = nth + 1;
	}
      break;
    default:
    boxes:
      itc->itc_search_params[nth] = NULL;
      if (itc->itc_search_par_fill <= nth)
	itc->itc_search_par_fill = nth + 1;
      break;
    }
}


void
ks_vec_itc (key_source_t * ks, it_cursor_t * itc, caddr_t * inst)
{
  /* Make boxes in the itc for the search params and attach the already cast column */
  search_spec_t *sp;
  int nth = 0;
  GPF_T1 ("this function is not in use");
  itc->itc_set = 0;
  itc->itc_set_first = 0;
  itc->itc_n_results = 0;
  for (sp = ks->ks_spec.ksp_spec_array; sp; sp = sp->sp_next)
    {
      if (sp->sp_min_op)
	{
	  if (SSL_IS_VEC (ks->ks_vec_cast[sp->sp_min]))
	    {
	      data_col_t *dc;
	      ITC_P_VEC (itc, sp->sp_min) = dc = QST_BOX (data_col_t *, inst, ks->ks_vec_cast[nth++]->ssl_index);
	      itc_vec_box (itc, sp->sp_cl.cl_sqt.sqt_col_dtp, sp->sp_min, dc);
	    }
	  else
	    {
	      ITC_P_VEC (itc, sp->sp_min) = NULL;
	      itc->itc_search_params[sp->sp_min] = QST_GET (inst, ks->ks_vec_cast[sp->sp_min]);
	    }
	}
      if (sp->sp_max_op)
	{
	  if (SSL_IS_VEC (ks->ks_vec_cast[sp->sp_max]))
	    {
	      data_col_t *dc;
	      ITC_P_VEC (itc, sp->sp_max) = dc = QST_BOX (data_col_t *, inst, ks->ks_vec_cast[nth++]->ssl_index);
	      itc_vec_box (itc, sp->sp_cl.cl_sqt.sqt_col_dtp, sp->sp_min, dc);
	    }
	  else
	    {
	      ITC_P_VEC (itc, sp->sp_min) = NULL;
	      itc->itc_search_params[sp->sp_min] = QST_GET (inst, ks->ks_vec_cast[sp->sp_min]);
	    }
	}
    }
}


int
cf_box_copy (caddr_t * inst, data_col_t * dc, data_col_t * src_dc, int inx)
{
  ((caddr_t *) dc->dc_values)[dc->dc_n_values++] = box_copy_tree (((caddr_t *) src_dc->dc_values)[inx]);
  return 1;
}


int
cf_any_iri_id_t (caddr_t * inst, data_col_t * dc, data_col_t * src_dc, int inx)
{
  /* dv string to iri */
  db_buf_t in = ((db_buf_t *) src_dc->dc_values)[inx];
  int64 ln;
  dtp_t dtp = in[0];
  switch (dtp)
    {
    case DV_DB_NULL:
      if (dc->dc_sqt.sqt_non_null)
	return 0;
      dc_set_null (dc, inx);
      return 1;
    case DV_IRI_ID:
      ln = (iri_id_t) (uint32) LONG_REF_NA (in + 1);
      ((int64 *) dc->dc_values)[dc->dc_n_values++] = ln;
      return 1;
    case DV_IRI_ID_8:
      ln = (iri_id_t) (uint32) INT64_REF_NA (in + 1);
      ((int64 *) dc->dc_values)[dc->dc_n_values++] = ln;
      return 1;
    default:
      return 0;
    }
}


int
cf_iri_id_t_any (caddr_t * inst, data_col_t * dc, data_col_t * src_dc, int inx)
{
  iri_id_t id = ((iri_id_t *) src_dc->dc_values)[inx];
  dtp_t bytes[10];
  if (src_dc->dc_nulls && (src_dc->dc_nulls[inx / 8] & (1 << (inx & 7))))
    {
      if (dc->dc_sqt.sqt_non_null)
	return 0;
      bytes[0] = DV_DB_NULL;
      dc_append_bytes (dc, bytes, 1, NULL, 0);
      return 1;
    }
  if (id > 0xffffffff)
    {
      bytes[0] = DV_IRI_ID;
      LONG_SET_NA (&bytes[1], id);
      dc_append_bytes (dc, bytes, 5, NULL, 0);
    }
  else
    {
      INT64_SET_NA (&bytes[1], id);
      dc_append_bytes (dc, bytes, 9, NULL, 0);
    }
  return 1;
}


int
cf_gen (caddr_t * inst, data_col_t * dc, data_col_t * src_dc, int inx)
{
  /* arbitrary cast */
  return 0;
}


int *
qi_extend_sets (caddr_t * inst, int place)
{
  QNCAST (query_instance_t, qi, inst);
  int *sets = QST_BOX (int *, inst, place);
  int len = box_length (sets) / sizeof (int);
  int new_len = MIN (dc_max_batch_sz, 2 * len);
  int *next;
  if (len >= dc_max_batch_sz)
    GPF_T1 ("extending sets past max batch sz");
  if (new_len < dc_batch_sz)
    new_len = dc_batch_sz;
  next = (int *) mp_alloc_box_ni (qi->qi_mp, new_len * sizeof (int), DV_BIN);
  memcpy (next, sets, len * sizeof (int));
  QST_BOX (int *, inst, place) = next;
  return next;
}


void
qn_result (data_source_t * qn, caddr_t * inst, int set_no)
{
  /* record that this output of the qn corresponds to the set_no'th input */
  int *sets = QST_BOX (int *, inst, qn->src_sets);
  int len = box_length (sets) / sizeof (int);
  int fill = QST_INT (inst, qn->src_out_fill)++;
  if (fill >= len)
    sets = qi_extend_sets (inst, qn->src_sets);
  sets[fill] = set_no;
}


void
ssl_result (state_slot_t * ssl, caddr_t * inst, int set_no)
{
  /* record that this output of the qn corresponds to the set_no'th input */
  int *sets = QST_BOX (int *, inst, ssl->ssl_sets);
  int fill = QST_INT (inst, ssl->ssl_n_values)++;
  if (fill >= box_length (sets) / sizeof (int))
    sets = qi_extend_sets (inst, ssl->ssl_sets);
  sets[fill] = set_no;
}


void
ssl_consec_results (state_slot_t * ssl, caddr_t * inst, int n_sets)
{
  QNCAST (query_instance_t, qi, inst);
  int *sets = QST_BOX (int *, inst, ssl->ssl_sets);
  if (n_sets > box_length (sets) / sizeof (int))
    sets = QST_BOX (int *, inst, ssl->ssl_sets) =
	(int *) mp_alloc_box_ni (qi->qi_mp, (n_sets + (n_sets / 8)) * sizeof (int), DV_BIN);
  int_asc_fill (sets, n_sets, 0);
  QST_INT (inst, ssl->ssl_n_values) = n_sets;
}

dc_cast_t
ssl_cast_func (state_slot_t * res, state_slot_t * source)
{
  dtp_t target_dtp = res->ssl_sqt.sqt_dtp;
  dtp_t source_dtp = source->ssl_sqt.sqt_dtp;
  if (source->ssl_dc_dtp == res->ssl_dc_dtp)
    return vec_box_dtps[res->ssl_dc_dtp] ? cf_box_copy : NULL;
  switch (target_dtp)
    {
    case DV_ANY:
      if (DV_IRI_ID == source_dtp)
	return cf_iri_id_t_any;
      return cf_gen;
    case DV_IRI_ID:
      if (DV_ANY == source_dtp)
	return cf_any_iri_id_t;
      return cf_gen;
    default:;
    }
  return cf_gen;
}


void
ssl_insert_cast (insert_node_t * ins, caddr_t * inst, int nth_col, caddr_t * err_ret, row_delta_t * rd, int from_row, int to_row)
{
  it_cursor_t *itc = rd->rd_itc;
  QNCAST (query_instance_t, qi, inst);
  state_slot_t *res = ins->ins_vec_cast[nth_col];
  state_slot_ref_t *source = ins->ins_vec_source[nth_col];
  caddr_t *source_boxes = NULL;
  dbe_col_loc_t *cl = ins->ins_vec_cast_cl[nth_col];
  int inx;
  data_col_t *from_dc = QST_BOX (data_col_t *, inst, source->sslr_index);
  data_col_t *target_dc = QST_BOX (data_col_t *, inst, res->ssl_index);
  int elt_sz = !cl ? dc_elt_size (from_dc) : 0;
  DC_CHECK_LEN (target_dc, to_row - 1);
  if (SSL_VEC == source->ssl_type && (DCT_BOXES & from_dc->dc_type))
    source_boxes = (caddr_t *) from_dc->dc_values;
  if (DCT_BOXES & target_dc->dc_type)
    {
      DC_FILL_TO (target_dc, caddr_t, from_row);
    }
  target_dc->dc_n_values = from_row;
  for (inx = from_row; inx < to_row; inx++)
    {
      int row = SSL_REF == source->ssl_type ? sslr_set_no (inst, (state_slot_t *) source, inx) : inx;
      if (elt_sz)
	{
	  if (8 == elt_sz)
	    ((int64 *) target_dc->dc_values)[target_dc->dc_n_values++] = ((int64 *) from_dc->dc_values)[row];
	  else if (4 == elt_sz)
	    ((int32 *) target_dc->dc_values)[target_dc->dc_n_values++] = ((int32 *) from_dc->dc_values)[row];
	  else
	    {
	      memcpy (target_dc->dc_values + elt_sz * target_dc->dc_n_values, from_dc->dc_values + row * elt_sz, elt_sz);
	      {
		target_dc->dc_n_values++;
	      }
	      if (from_dc->dc_any_null && from_dc->dc_nulls)
		if (DC_IS_NULL (from_dc, row))
		  {
		    dc_ensure_null_bits (target_dc);
		    DC_SET_NULL (target_dc, target_dc->dc_n_values - 1);
		  }
	    }
	}
      else
	{
	  dtp_t dtp;
	  caddr_t value;
	  if (source_boxes)
	    {
	      value = source_boxes[inx];
	      if (inx + 2 < to_row)
		__builtin_prefetch (source_boxes[inx + 2]);
	    }
	  else
	    {
	      qi->qi_set = inx;
	      value = qst_get (inst, (state_slot_t *) source);
	    }
	  dtp = DV_TYPE_OF (value);
	  if (dtp == dtp_canonical[cl->cl_sqt.sqt_col_dtp])
	    {
	      switch (dtp)
		{
		case DV_LONG_INT:
		  dc_append_box (target_dc, value);
		  break;
		case DV_SINGLE_FLOAT:
		case DV_DOUBLE_FLOAT:
		case DV_DATETIME:
		case DV_IRI_ID:
		  dc_append_box (target_dc, value);
		  break;
		case DV_STRING:
		  if ((cl->cl_sqt.sqt_precision && box_length (value) - 1 > cl->cl_sqt.sqt_precision) || box_length (value) > 4095)
		    goto general;
		  dc_append_box (target_dc, value);
		  break;
		default:
		  goto general;
		}
	    }
	  else
	    {
	    general:
	      rd->rd_non_comp_len = 0;
	      itc->itc_owned_search_par_fill = itc->itc_search_par_fill = 0;
	      row_insert_cast (rd, cl, value, err_ret, NULL);
	      if (*err_ret)
		{
		  return;
		}
	      if (DCT_BOXES & target_dc->dc_type)
		{
		  if (1 != itc->itc_owned_search_par_fill)
		    value = box_copy_tree (itc->itc_search_params[0]);
		  else
		    {
		      value = itc->itc_search_params[0];
		      itc->itc_owned_search_par_fill = 0;
		    }
		  ((caddr_t *) target_dc->dc_values)[target_dc->dc_n_values++] = value;
		}
	      else
		{
		  value = itc->itc_search_params[0];
		  if (DV_ANY == target_dc->dc_dtp && DV_ANY == cl->cl_sqt.sqt_col_dtp)
		    {
		      if (DV_DB_NULL == box_tag (value))
			{
			  dtp_t n = DV_DB_NULL;
			  dc_append_bytes (target_dc, &n, 1, NULL, 0);
			}
		      else
			dc_append_bytes (target_dc, (db_buf_t) value, box_length (value) - 1, NULL, 0);
		    }
		  else
		    dc_append_box (target_dc, value);
		}
	      if (itc->itc_owned_search_par_fill)
		itc_free_owned_params (itc);
	    }
	}
    }
}



#define MAX_PARAMS 16



void
ks_vec_params (key_source_t * ks, it_cursor_t * itc, caddr_t * inst)
{
  /* cast and align the search param columns */
  data_source_t *qn = (data_source_t *) ks->ks_ts;
  caddr_t err = NULL;
  QNCAST (query_instance_t, qi, inst);
  int row;
  int n_rows = qn->src_prev ? QST_INT (inst, qn->src_prev->src_out_fill) : 0;
  int n_cast = BOX_ELEMENTS (ks->ks_vec_cast), inx, n_cols = 0;
  data_col_t *target[MAX_PARAMS];
  data_col_t *source[MAX_PARAMS];
  dc_val_cast_t cf[MAX_PARAMS];
  state_slot_ref_t *sslr[MAX_PARAMS];
  dtp_t dc_elt_len[MAX_PARAMS];
  int cast_or_null = 0, is_row_spec = 0;
  if (!n_rows && !qn->src_prev)
    n_rows = 1;
  if (ks->ks_last_vec_param)
    QST_INT (inst, ks->ks_last_vec_param->ssl_n_values) = 0;
  if (itc)
    itc->itc_multistate_row_specs = 0;
  for (inx = 0; inx < n_cast; inx++)
    {
      state_slot_ref_t *source_ref;
      if (ks->ks_vec_cast[inx] && (source_ref = ks->ks_vec_source[inx]))
	{
	  data_col_t *source_dc = source[n_cols] = QST_BOX (data_col_t *, inst, ((state_slot_t *) source_ref)->ssl_index);
	  data_col_t *target_dc = target[n_cols] = QST_BOX (data_col_t *, inst, ks->ks_vec_cast[inx]->ssl_index);
	  sslr[n_cols] = SSL_REF == ks->ks_vec_source[inx]->ssl_type ? ks->ks_vec_source[inx] : NULL;
	  dc_elt_len[n_cols] = dc_elt_size (source_dc);
	  dc_reset (target_dc);
	  DC_CHECK_LEN (target_dc, n_rows - 1);
	  cf[n_cols] = ks->ks_dc_val_cast[inx];
	  if (target_dc->dc_dtp == source_dc->dc_dtp)
	    cf[n_cols] = NULL;
	  if (cf[n_cols] || source_dc->dc_any_null)
	    cast_or_null = 1;
	  if (!cf[n_cols] && DV_ANY == target_dc->dc_dtp && DV_ANY != source_dc->dc_dtp)
	    cf[n_cols] = vc_to_any (source_dc->dc_dtp);
	  if (ks->ks_first_row_vec_ssl && ks->ks_first_row_vec_ssl == (state_slot_t *) ks->ks_vec_cast[inx])
	    is_row_spec = 1;
	  if (itc && source_dc->dc_n_values > 1 && is_row_spec)
	    itc->itc_multistate_row_specs = 1;
	  n_cols++;
	  if (n_cols >= MAX_PARAMS)
	    sqlr_new_error ("37000", "VEC..", "Too many searchh parameters");
	}
    }
  if (!n_cols)
    return;
  if (!cast_or_null)
    {
      /* each col will be the same height after cast. No need to loop by row */
      for (inx = 0; inx < n_cols; inx++)
	{
	  data_col_t *source_dc = source[inx];
	  data_col_t *target_dc = target[inx];
	  if (!sslr[inx] && !cf[inx] && !(source_dc->dc_type & DCT_BOXES))
	    {
	      target_dc->dc_org_values = target_dc->dc_values;
	      target_dc->dc_org_places = target_dc->dc_n_places;
	      target_dc->dc_org_dtp = target_dc->dc_dtp;
	      target_dc->dc_values = source_dc->dc_values;
	      target_dc->dc_n_values = source_dc->dc_n_values;
	      target_dc->dc_n_places = source_dc->dc_n_places;
	      target_dc->dc_any_null = 0;
	    }
	  else if (!sslr[inx] && !cf[inx] && (source_dc->dc_type & DCT_BOXES))
	    dc_copy (target_dc, source_dc);
	  else if (cf[inx])
	    {
	      state_slot_ref_t *r = sslr[inx];
	      if (r)
		{
		  for (row = 0; row < n_rows; row++)
		    cf[inx] (target_dc, source_dc, sslr_set_no (inst, (state_slot_t *) r, row), &err);
		}
	      else
		{
		  for (row = 0; row < n_rows; row++)
		    cf[inx] (target_dc, source_dc, row, &err);
		}
	    }
	  else
	    {
	      sslr_dc_copy (inst, sslr[inx], target_dc, source_dc, n_rows, dc_elt_len[inx]);
	    }
	}
      if (ks->ks_last_vec_param && ks->ks_last_vec_param->ssl_sets)
	ssl_consec_results (ks->ks_last_vec_param, inst, n_rows);
      if (itc)
	itc->itc_max_rows = ks->ks_ts->ts_max_rows;
      return;
    }
  for (row = 0; row < n_rows; row++)
    {
      for (inx = 0; inx < n_cols; inx++)
	{
	  data_col_t *target_dc = target[inx];
	  data_col_t *source_dc = source[inx];
	  int source_row = row;
	  state_slot_ref_t *ref;
	  if ((ref = sslr[inx]))
	    {
	      int step;
	      for (step = 0; step < ref->sslr_distance; step++)
		{
		  int *set_nos = (int *) inst[ref->sslr_set_nos[step]];
		  source_row = set_nos[source_row];
		}
	    }
	  if (!cf[inx])
	    {
	      if (source_dc->dc_any_null)
		{
		  if (DV_ANY == source_dc->dc_dtp)
		    {
		      if (DV_DB_NULL == ((db_buf_t *) source_dc->dc_values)[source_row][0])
			goto was_null;
		      ((db_buf_t *) target_dc->dc_values)[target_dc->dc_n_values++] =
			  ((db_buf_t *) source_dc->dc_values)[source_row];
		    }
		  else if (source_dc->dc_type & DCT_BOXES)
		    {
		      if (DV_DB_NULL == DV_TYPE_OF (((db_buf_t *) source_dc->dc_values)[source_row]))
			goto was_null;
		      ((db_buf_t *) target_dc->dc_values)[target_dc->dc_n_values++] =
			  box_copy_tree (((db_buf_t *) source_dc->dc_values)[source_row]);
		    }
		  else
		    {
		      if (DC_IS_NULL (source_dc, source_row))
			goto was_null;
		      DC_ELT_CPY (target_dc, target_dc->dc_n_values, source_dc, source_row, dc_elt_len[inx]);
		      target_dc->dc_n_values++;
		    }
		}
	      else
		{
		  DC_ELT_CPY (target_dc, target_dc->dc_n_values, source_dc, source_row, dc_elt_len[inx]);
		  target_dc->dc_n_values++;
		}
	    }
	  else
	    {
	      if (!cf[inx] (target_dc, source_dc, source_row, &err))
		{
		  int inx2;
		  if (err && !qi->qi_no_cast_error)
		    sqlr_resignal (err);
		  dk_free_tree (err);
		  err = NULL;
		was_null:
		  for (inx2 = 0; inx2 < inx; inx2++)
		    {
		      /* check if it is DCT_BOXES & free the thing */
		      if (DCT_BOXES & target[inx2]->dc_type)
			dk_free_tree (((caddr_t *) target[inx2]->dc_values)[target[inx2]->dc_n_values - 1]);
		      target[inx2]->dc_n_values--;
		    }
		  goto next_row;	/* the row of input is skipped */
		}
	    }
	}
      if (ks->ks_last_vec_param && ks->ks_last_vec_param->ssl_sets)
	ssl_result (ks->ks_last_vec_param, inst, row);
    next_row:;
    }
  if (itc)
    itc->itc_max_rows = ks->ks_ts->ts_max_rows;
  qi->qi_set_mask = NULL;
}

int cmp_prefetch_d = 3;


int
itc_param_cmp (int r1, int r2, void *cd)
{
  QNCAST (it_cursor_t, itc, cd);
  int inx, r;
  int r_prefetch = r1 + cmp_prefetch_d < itc->itc_n_sets ? r1 + cmp_prefetch_d : r1;
  for (inx = 0; inx < itc->itc_n_vec_sort_cols; inx++)
    {
      data_col_t *dc = ITC_P_VEC (itc, inx);
      if (!dc)
	continue;
      r = dc->dc_sort_cmp (dc, r1, r2, r_prefetch);
      if (DVC_MATCH != r)
	return r;
    }
  return DVC_MATCH;
}


int
itc_int_param_cmp (int r1, int r2, void *cd)
{
  QNCAST (it_cursor_t, itc, cd);
  data_col_t *dc = ITC_P_VEC (itc, 0);
  int64 i1 = ((int64 *) dc->dc_values)[r1];
  int64 i2 = ((int64 *) dc->dc_values)[r2];
  return NUM_COMPARE (i1, i2);
}


int
itc_int_int_param_cmp (int r1, int r2, void *cd)
{
  QNCAST (it_cursor_t, itc, cd);
  data_col_t *dc = ITC_P_VEC (itc, 0);
  int64 i1 = ((int64 *) dc->dc_values)[r1];
  int64 i2 = ((int64 *) dc->dc_values)[r2];
  if (i1 == i2)
    {
      dc = ITC_P_VEC (itc, 1);
      i1 = ((int64 *) dc->dc_values)[r1];
      i2 = ((int64 *) dc->dc_values)[r2];
      return NUM_COMPARE (i1, i2);
    }
  return i1 < i2 ? DVC_LESS : DVC_GREATER;
}


int
itc_iri_iri_any_iri_param_cmp (int r1, int r2, void *cd)
{
  QNCAST (it_cursor_t, itc, cd);
  data_col_t *dc = ITC_P_VEC (itc, 0);
  iri_id_t i1 = ((iri_id_t *) dc->dc_values)[r1];
  iri_id_t i2 = ((iri_id_t *) dc->dc_values)[r2];
  db_buf_t dv1, dv2;
  int rc;
  int r_prefetch = r1 + cmp_prefetch_d < itc->itc_n_sets ? r1 + cmp_prefetch_d : r1;
  __builtin_prefetch (&((int64 *) dc->dc_values)[r_prefetch]);
  if (i1 == i2)
    {
      dc = ITC_P_VEC (itc, 1);
      i1 = ((iri_id_t *) dc->dc_values)[r1];
      i2 = ((iri_id_t *) dc->dc_values)[r2];
      __builtin_prefetch (&((int64 *) dc->dc_values)[r_prefetch]);
      if (i1 == i2)
	{
	  dc = ITC_P_VEC (itc, 2);
	  dv1 = ((db_buf_t *) dc->dc_values)[r1];
	  dv2 = ((db_buf_t *) dc->dc_values)[r2];
	  __builtin_prefetch (((caddr_t *) dc->dc_values)[r_prefetch]);
	  rc = dv_compare (dv1, dv2, NULL, 0);
	  if (DVC_MATCH == rc)
	    {
	      dc = ITC_P_VEC (itc, 3);
	      i1 = ((iri_id_t *) dc->dc_values)[r1];
	      i2 = ((iri_id_t *) dc->dc_values)[r2];
	      __builtin_prefetch (&((int64 *) dc->dc_values)[r_prefetch]);
	      return NUM_COMPARE (i1, i2);
	    }
	  return rc & ~DVC_NOORDER;
	}
    }
  return i1 < i2 ? DVC_LESS : DVC_GREATER;
}


int
itc_iri_any_iri_iri_param_cmp (int r1, int r2, void *cd)
{
  QNCAST (it_cursor_t, itc, cd);
  data_col_t *dc = ITC_P_VEC (itc, 0);
  iri_id_t i1 = ((iri_id_t *) dc->dc_values)[r1];
  iri_id_t i2 = ((iri_id_t *) dc->dc_values)[r2];
  db_buf_t dv1, dv2;
  int rc;
  int r_prefetch = r1 + cmp_prefetch_d < itc->itc_n_sets ? r1 + cmp_prefetch_d : r1;
  __builtin_prefetch (&((int64 *) dc->dc_values)[r_prefetch]);
  if (i1 == i2)
    {
      dc = ITC_P_VEC (itc, 1);
      dv1 = ((db_buf_t *) dc->dc_values)[r1];
      dv2 = ((db_buf_t *) dc->dc_values)[r2];
      __builtin_prefetch (((caddr_t *) dc->dc_values)[r_prefetch]);
      rc = dv_compare (dv1, dv2, NULL, 0);
      if (DVC_MATCH == rc)
	{
	  dc = ITC_P_VEC (itc, 2);
	  i1 = ((iri_id_t *) dc->dc_values)[r1];
	  i2 = ((iri_id_t *) dc->dc_values)[r2];
	  __builtin_prefetch (&((int64 *) dc->dc_values)[r_prefetch]);
	  if (i1 == i2)
	    {
	      dc = ITC_P_VEC (itc, 3);
	      i1 = ((iri_id_t *) dc->dc_values)[r1];
	      i2 = ((iri_id_t *) dc->dc_values)[r2];
	      __builtin_prefetch (&((int64 *) dc->dc_values)[r_prefetch]);
	      return NUM_COMPARE (i1, i2);
	    }
	  return i1 < i2 ? DVC_LESS : DVC_GREATER;
	}
      return rc & ~DVC_NOORDER;

    }
  return i1 < i2 ? DVC_LESS : DVC_GREATER;
}



caddr_t
itc_temp_any_box (it_cursor_t * itc, int inx, db_buf_t dv)
{
  caddr_t box = itc->itc_search_params[inx];
  caddr_t new_box = NULL;
  int len, hl, box_len;
  switch (dv[0])
    {
    case DV_SHORT_STRING_SERIAL:
      if (IS_BOX_POINTER (box) && DV_STRING == box_tag (box) && ALIGN_STR (box_length (box)) == ALIGN_STR ((dv[1] + 1)))
	{
	  int l = dv[1];
	  box_reuse (box, (caddr_t) dv + 2, l + 1, DV_STRING);
	  box[l] = 0;
	  return box;
	}
      goto general;

    case DV_WIDE:
      len = dv[1];
      hl = 2;
      goto wide;
    case DV_LONG_WIDE:
      len = LONG_REF_NA (dv + 1);
      hl = 5;
    wide:
      box_len = box ? box_length (box) : 0;
      if (box_len == len + 1 && DV_WIDE == box_tag (box))
	{
	  memcpy (box, dv + hl, len);
	  return box;
	}
      else
	{
	  new_box = dk_alloc_box (len + 1, DV_WIDE);
	  memcpy (new_box, dv + hl, len);
	  new_box[len] = 0;
	  goto replace_owned_par;
	}
    default:
    general:
      {
	new_box = box_deserialize_string ((caddr_t) dv, INT32_MAX, 0);
      replace_owned_par:
	for (inx = 0; inx < itc->itc_owned_search_par_fill; inx++)
	  {
	    if (box == itc->itc_owned_search_params[inx])
	      {
		dk_free_tree (box);
		itc->itc_owned_search_params[inx] = new_box;
		return new_box;
	      }
	  }
	ITC_OWNS_PARAM (itc, new_box);
	return new_box;
      }
    }
}


int
itc_vec_sp_copy (it_cursor_t * itc, int inx, int64 new_v)
{
  caddr_t b = itc->itc_search_params[inx];
  if (DV_DATETIME == DV_TYPE_OF (b))
    {
      memcpy (b, (caddr_t) (ptrlong) new_v, DT_LENGTH);
      return 1;
    }
  return 0;
}


void
itc_set_param_row (it_cursor_t * itc, int nth)
{
  /* set the search params to be the nth row according to sorted param order */
  int sp2nd = 0, is_row_sp = 0;
  search_spec_t *sp = itc->itc_key_spec.ksp_spec_array;
  int inx, pos = itc->itc_param_order[nth];
  if (!sp)
    {
      sp = itc->itc_row_specs;
      is_row_sp = 1;
    }
  for (inx = 0; inx < itc->itc_search_par_fill; inx++)
    {
      data_col_t *dc = ITC_P_VEC (itc, inx);
      int64 new_v;
      if (!dc)
	goto next;
      new_v = dc_any_value (dc, pos);
      if (DCT_NUM_INLINE & dc->dc_type)
	{
	  NEXT_SET_INL_NULL_ALWAYS (dc, pos, inx);
	  *(int64 *) itc->itc_search_params[inx] = new_v;
	}
      else if (DV_ANY == dc->dc_dtp)
	{
	  if ((sp->sp_cl.cl_sqt.sqt_dtp == DV_ANY || sp->sp_cl.cl_sqt.sqt_col_dtp == DV_ANY)
	      /*&& (!itc->itc_is_col || !is_row_sp) */ )
	    itc->itc_search_params[inx] = (caddr_t) (ptrlong) new_v;
	  else
	    itc->itc_search_params[inx] = itc_temp_any_box (itc, inx, (db_buf_t) new_v);
	}
      else if (DCT_BOXES & dc->dc_type)
	{
	  itc->itc_search_params[inx] = (caddr_t) (ptrlong) new_v;
	}
      else if (!itc_vec_sp_copy (itc, inx, new_v))
	itc->itc_search_params[inx] = (caddr_t) (ptrlong) new_v;
    next:
      NEXT_SP_COL;
      if (!sp)
	break;			/* in bm inx, can have in landed state less sps than params */
    }
}


void
itc_set_row_spec_param_row (it_cursor_t * itc, int nth)
{
  /* set the search params to be the nth row according to sorted param order */
  int sp2nd = 0, is_row_sp = 1;
  search_spec_t *sp = itc->itc_row_specs;
  int inx, pos;
  int first_row_param;
  if (!sp || !itc->itc_n_sets)
    return;
  pos = itc->itc_param_order[nth];
  first_row_param = (sp->sp_min_op != CMP_NONE) ? sp->sp_min : sp->sp_max;
  for (inx = first_row_param; inx < itc->itc_search_par_fill; inx++)
    {
      data_col_t *dc = ITC_P_VEC (itc, inx);
      int64 new_v;
      if (!dc)
	goto next;
      new_v = dc_any_value (dc, pos);
      if (DCT_NUM_INLINE & dc->dc_type)
	*(int64 *) itc->itc_search_params[inx] = new_v;
      else if (DV_ANY == dc->dc_dtp)
	{
	  if (sp->sp_cl.cl_sqt.sqt_dtp == DV_ANY || sp->sp_cl.cl_sqt.sqt_col_dtp == DV_ANY)
	    itc->itc_search_params[inx] = (caddr_t) (ptrlong) new_v;
	  else
	    itc->itc_search_params[inx] = itc_temp_any_box (itc, inx, (db_buf_t) new_v);
	}
      else if (!itc_vec_sp_copy (itc, inx, new_v))
	itc->itc_search_params[inx] = (caddr_t) (ptrlong) new_v;
    next:
      NEXT_SP_COL;
    }
}

void
itc_make_param_order (it_cursor_t * itc, query_instance_t * qi, int n_sets)
{
  int inx;
  if (!qi->qi_set_mask)
    {
      for (inx = 0; inx < n_sets; inx++)
	itc->itc_param_order[inx] = inx;
      itc->itc_n_sets = n_sets;
    }
  else
    {
      int fill = 0;
      for (inx = 0; inx < n_sets; inx++)
	{
	  if (QI_IS_SET (qi, inx))
	    itc->itc_param_order[fill++] = inx;
	}
      itc->itc_n_sets = fill;
    }
}


int enable_ext_param_cmp = 0;

sort_cmp_func_t
itc_param_cmp_func (it_cursor_t * itc)
{
  if (1 == itc->itc_n_vec_sort_cols)
    {
      data_col_t *dc = ITC_P_VEC (itc, 0);
      if (dc && dc_int_cmp == dc->dc_sort_cmp)
	return itc_int_param_cmp;
    }
  if (2 == itc->itc_n_vec_sort_cols)
    {
      data_col_t *dc = ITC_P_VEC (itc, 0);
      data_col_t *dc2 = ITC_P_VEC (itc, 1);
      if (dc && dc2 && dc_int_cmp == dc->dc_sort_cmp && dc_int_cmp == dc2->dc_sort_cmp)
	return itc_int_int_param_cmp;
    }
  if (4 == itc->itc_n_vec_sort_cols)
    {
      data_col_t *dc = ITC_P_VEC (itc, 0);
      data_col_t *dc2 = ITC_P_VEC (itc, 1);
      data_col_t *dc3 = ITC_P_VEC (itc, 2);
      data_col_t *dc4 = ITC_P_VEC (itc, 3);
      if (!dc || !dc2 || !dc3 || !dc4)
	return itc_param_cmp;
      if (dc_iri_id_cmp == dc->dc_sort_cmp && dc_iri_id_cmp == dc4->dc_sort_cmp)
	{
	  if (dc_any_cmp == dc2->dc_sort_cmp && dc_iri_id_cmp == dc3->dc_sort_cmp)
	    return itc_iri_any_iri_iri_param_cmp;
	  if (dc_iri_id_cmp == dc2->dc_sort_cmp && dc_any_cmp == dc3->dc_sort_cmp)
	    return itc_iri_iri_any_iri_param_cmp;
	}
    }
  return itc_param_cmp;
}


int enable_digit = 1;

int
itc_vec_digit_sort (it_cursor_t * itc)
{
  data_col_t *dcs[16];
  data_col_t *dc;
  int inx, fill = 0;
  if (itc->itc_n_sets < 40 || !enable_digit)
    return 0;
  for (inx = 0; inx < itc->itc_n_vec_sort_cols; inx++)
    {
      dc = ITC_P_VEC (itc, inx);
      if (dc)
	{
	  dcs[fill++] = dc;
	  if (!(DCT_NUM_INLINE & dc->dc_type))
	    return 0;
	}
    }
  dc_digit_sort (dcs, fill, itc->itc_param_order, itc->itc_n_sets);
  if (2 == enable_digit)
    {
      for (inx = 1; inx < dcs[0]->dc_n_values; inx++)
	{
	  int c;
	  for (c = 0; c < fill; c++)
	    {
	      int64 i1 = dc_int (dcs[c], itc->itc_param_order[inx - 1]);
	      int64 i2 = dc_int (dcs[c], itc->itc_param_order[inx]);
	      if (i1 < i2)
		break;
	      if (i1 > i2)
		GPF_T1 ("digit sort out of order");
	    }
	}
    }
  return 1;
}


void
itc_out_col_extend (it_cursor_t * itc)
{
  data_col_t *dc;
  key_source_t *ks = itc->itc_ks;
  v_out_map_t *om = ks->ks_v_out_map;
  int n = box_length (om) / sizeof (v_out_map_t), inx;
  for (inx = 0; inx < n; inx++)
    {
      if (!om[inx].om_ssl)
	continue;
      dc = QST_BOX (data_col_t *, itc->itc_out_state, om[inx].om_ssl->ssl_index);
      DC_CHECK_LEN (dc, itc->itc_batch_size - 1);
    }
}


void
itc_param_sort (key_source_t * ks, it_cursor_t * itc)
{
  /* the height isthe height of the last cast column */
  int *param_nos;
  caddr_t *inst = itc->itc_out_state;
  table_source_t *ts = ks->ks_ts;
  QNCAST (query_instance_t, qi, inst);
  data_col_t *last_dc = NULL;
  int n_params = 0, inx;
  if (ks->ks_from_temp_tree && SSL_TREE == ks->ks_from_temp_tree->ssl_type)
    n_params = 1;
  else if (ks->ks_last_vec_param)
    {
      last_dc = QST_BOX (data_col_t *, inst, ks->ks_last_vec_param->ssl_index);
      n_params = last_dc->dc_n_values;
    }
  else
    {
      if (ts->src_gen.src_prev)
	n_params = QST_INT (inst, ts->src_gen.src_prev->src_out_fill);
      else
	n_params = 1;
    }
  itc->itc_set = 0;
  itc->itc_n_sets = n_params;
  itc->itc_n_results = 0;
  if (!n_params)
    return;			/* can be if all casts failed in quietcast */
  if (n_params > 1)
    itc->itc_read_hook = itc_dive_read_hook;
  itc->itc_batch_size = MAX (itc->itc_n_sets, dc_batch_sz);
  QST_INT (inst, ts->src_gen.src_out_fill) = 0;
  param_nos = QST_BOX (int *, inst, ks->ks_param_nos);
  itc->itc_n_vec_sort_cols = ks->ks_n_vec_sort_cols;
  if (!param_nos || box_length (param_nos) / sizeof (int) < itc->itc_n_sets)
    {
      param_nos = (int *) mp_alloc_box_ni (qi->qi_mp, sizeof (int) * MAX (itc->itc_n_sets, dc_batch_sz), DV_BIN);
      QST_BOX (int *, inst, ks->ks_param_nos) = param_nos;
    }
  for (inx = 0; inx < n_params; inx++)
    param_nos[inx] = inx;
  itc->itc_param_order = param_nos;
  itc->itc_asc_eq = ks->ks_vec_asc_eq;
  if (itc->itc_n_vec_sort_cols && !ks->ks_oby_order)
    {
      if (!itc_vec_digit_sort (itc))
	{
	  int *left = QST_BOX (int *, inst, ts->src_gen.src_sets);
	  if (box_length (left) / sizeof (int) < itc->itc_n_sets)
	    left = QST_BOX (int *, inst, ts->src_gen.src_sets) =
		(int *) mp_alloc_box_ni (qi->qi_mp, itc->itc_batch_size * sizeof (int), DV_BIN);
	  gen_qmsort (itc->itc_param_order, left, itc->itc_n_sets, itc_param_cmp_func (itc), (void *) itc,
	      10 * itc->itc_n_vec_sort_cols);
	}
    }
  itc->itc_v_out_map = ks->ks_v_out_map;
  itc->itc_same_parent_hit = itc->itc_same_parent_miss = 0;
  itc_set_param_row (itc, 0);
  itc_out_col_extend (itc);
  if (ks->ks_from_temp_tree)
    {
      int tset;
      for (tset = 0; tset < n_params; tset++)
	{
	  qi->qi_set = tset;
	  if (itc_from_sort_temp (itc, qi, ks->ks_from_temp_tree))
	    return;
	  itc->itc_set++;
	}
    }
}


void
ks_vec_new_results (key_source_t * ks, caddr_t * inst, it_cursor_t * itc)
{
  /* new batch, reset the fills */
  search_spec_t *sp;
  table_source_t *ts = ks->ks_ts;
  int batch = QST_INT (inst, ts->src_gen.src_batch_size);
  if (!batch)
    batch = QST_INT (inst, ts->src_gen.src_batch_size) = dc_batch_sz;
  if (itc)
    itc->itc_batch_size = batch;
  QN_CHECK_SETS (ts, inst, batch);
  dc_reset_array (inst, (data_source_t *) ks->ks_ts, ts->src_gen.src_continue_reset, batch);
  QST_INT (inst, ks->ks_ts->src_gen.src_out_fill) = 0;
  if (itc)
    {
      for (sp = itc->itc_row_specs; sp; sp = sp->sp_next)
	{
	  if (CMP_HASH_RANGE == sp->sp_min_op && CMP_HASH_RANGE_ONLY != sp->sp_max_op)
	    {
	      QNCAST (hash_range_spec_t, hrng, sp->sp_min_ssl);
	      if (hrng->hrng_hs)
		dc_reset_array (inst, (data_source_t *) ks->ks_ts, hrng->hrng_hs->hs_out_slots, batch);
	    }
	}
    }
}

void
itc_vec_new_results (it_cursor_t * itc)
{
  itc->itc_n_results = 0;
  ks_vec_new_results (itc->itc_ks, itc->itc_out_state, itc);
}

/* adjustable batch size */

int
qn_batch_settable (data_source_t * qn)
{
  return IS_TS (qn) || IS_QN (qn, hash_source_input)
      || IS_QN (qn, end_node_input) || IS_QN (qn, set_ctr_input) || IS_QN (qn, txs_input) || IS_QN (qn, fun_ref_node_input);
}


long tc_adjust_batch_sz;
long tc_cum_batch_sz;


void
qi_set_batch_sz (caddr_t * inst, table_source_t * ts, int new_sz)
{
  data_source_t *pred;
  for (pred = (data_source_t *) ts; pred; pred = pred->src_prev)
    {
      if (SRC_IN_STATE (pred, inst))
	goto found;;
    }
  return;			/* there is nothing continuable */
found:
  TC (tc_adjust_batch_sz);
  tc_cum_batch_sz += new_sz;
  for (pred = (data_source_t *) ts; pred; pred = pred->src_prev)
    {
      if (qn_batch_settable (pred))
	{
	  if (pred->src_batch_size)
	    QST_INT (inst, pred->src_batch_size) = new_sz;
	}
      else
	break;
    }
}


int qi_mp_max_bytes = 100000000;


int
qn_nth_set (data_source_t * qn, caddr_t * inst, int *total_sets)
{
  if (IS_TS (qn))
    {
      QNCAST (table_source_t, ts, qn);
      it_cursor_t *itc = (it_cursor_t *) QST_GET_V (inst, ts->ts_order_cursor);
      if (itc)
	{
	  *total_sets = itc->itc_n_sets - itc->itc_first_set;
	  return itc->itc_set - itc->itc_first_set;
	}
      return *total_sets = QST_INT (inst, ts->src_gen.src_prev->src_out_fill);
    }
  if (IS_QN (qn, hash_source_input) || IS_QN (qn, txs_input))
    {
      QNCAST (hash_source_t, hs, qn);
      *total_sets = QST_INT (inst, qn->src_prev->src_out_fill);
      return QST_INT (inst, hs->clb.clb_nth_set);
    }
  if (IS_QN (qn, set_ctr_input))
    return *total_sets = QST_INT (inst, qn->src_out_fill);
  else
    return *total_sets = QST_INT (inst, qn->src_prev->src_out_fill);
}


int64
qn_n_results (data_source_t * qn, caddr_t * inst)
{
  if (IS_TS (qn))
    {
      QNCAST (table_source_t, ts, qn);
      it_cursor_t *itc = (it_cursor_t *) QST_GET_V (inst, ts->ts_order_cursor);
      if (itc)
	return itc->itc_rows_selected;
    }
  return QST_INT (inst, qn->src_out_fill);
}


float
qn_card_est (data_source_t * qn, caddr_t * inst)
{
  if (IS_TS (qn))
    {
      QNCAST (table_source_t, ts, qn);
      it_cursor_t *itc = (it_cursor_t *) QST_GET_V (inst, ts->ts_order_cursor);
      int n = itc ? itc->itc_n_branches : 1;
      return ((table_source_t *) qn)->ts_cardinality / MAX (1, n);
    }
  if (IS_QN (qn, txs_input))
    return ((text_node_t *) qn)->txs_card;
  return 0;
}


float
qn_fanout (data_source_t * qn, caddr_t * inst, float *rows_left)
{
  int total_sets;
  int nth_set = qn_nth_set (qn, inst, &total_sets);
  int64 n_res = qn_n_results (qn, inst);
  float fanout = (float) n_res / (float) MAX (nth_set, 1);
  if (nth_set == total_sets - 1 && SRC_IN_STATE (qn, inst))
    {
      float card_est = qn_card_est (qn, inst);
      fanout = MAX (card_est, n_res / (float) (total_sets - nth_set));
    }
  *rows_left = fanout * (total_sets - nth_set);
  return fanout;
}


float
qn_rows_to_expect (data_source_t * qn, caddr_t * inst)
{
  float cum_fanout = 1;
  float total_rows = 0;
  for (qn = qn; qn; qn = qn->src_prev)
    {
      float rows_left;
      float fanout = qn_fanout (qn, inst, &rows_left);
      total_rows += rows_left * cum_fanout;
      cum_fanout *= fanout;
    }
  return total_rows;
}

void
ts_check_batch_sz (table_source_t * ts, caddr_t * inst, it_cursor_t * itc)
{
  QNCAST (query_instance_t, qi, inst);
  data_source_t *prev = ts->src_gen.src_prev;
  float rows_to_expect;
  int prev_sz;
  float min_density = 5;
  dbe_key_t *key;
  if (!prev || !itc->itc_n_sets || !enable_dyn_batch_sz || dc_max_batch_sz == itc->itc_batch_size
      || qi->qi_mp->mp_bytes > qi_mp_max_bytes)
    return;
  if (itc->itc_set < itc->itc_first_set)
    {
      bing ();			/* anomalous to have first set above set, will /0 so return. */
      return;
    }
  if (itc->itc_rows_selected / (1 + itc->itc_set - itc->itc_first_set) > 30)
    return;			/* if getting over 30 rows per set do not bother to increase batch for more hit density */
  prev_sz = QST_INT (inst, prev->src_batch_size);
  rows_to_expect = qn_rows_to_expect (prev, inst);
  if (rows_to_expect < prev_sz)
    return;
  key = itc->itc_insert_key;
  if (key->key_is_col)
    {
      if (key->key_segs_sampled)
	min_density = (float) 5 / ((1 | key->key_rows_in_sampled_segs) / key->key_segs_sampled);
      else
	min_density = 1.0 / 800;
    }
  if ((float) itc->itc_rows_on_leaves / MAX (itc->itc_n_sets, itc->itc_rows_selected) > min_density)
    {
      int target_sz = MIN ((float) dc_max_batch_sz, rows_to_expect * 1.2);
      if (target_sz < prev_sz * 2)
	return;
      qi_set_batch_sz (inst, ts, target_sz);
    }
}


/* query parallelization */

int
box_is_qp_private (caddr_t data)
{
  int n, inx;
  dtp_t dtp = DV_TYPE_OF (data);
  if (DV_ASYNC_QUEUE == dtp)
    return 1;
  if (DV_ARRAY_OF_POINTER != dtp)
    return 0;
  n = BOX_ELEMENTS (data);
  for (inx = 0; inx < n; inx++)
    if (DV_QI == DV_TYPE_OF (((caddr_t *) data)[inx]))
      return 1;
  return 0;
}


void
qst_set_hash_part (caddr_t * copy, caddr_t * org)
{
  QNCAST (query_instance_t, qi, org);
  DO_SET (fun_ref_node_t *, fref, &qi->qi_query->qr_nodes)
  {
    if (IS_QN (fref, hash_fill_node_input) && fref->fnr_hash_part_min)
      {
	QST_INT (copy, fref->fnr_hash_part_min) = QST_INT (org, fref->fnr_hash_part_min);
	QST_INT (copy, fref->fnr_hash_part_max) = QST_INT (org, fref->fnr_hash_part_max);
      }
  }
  END_DO_SET ();
}

void
qst_sets_copy (caddr_t * inst, caddr_t * cp, ssl_index_t sets, ssl_index_t fill)
{
  int n;
  if (!sets || !fill)
    return;
  n = QST_INT (inst, fill);
  if (n)
    {
      QNCAST (query_instance_t, cp_qi, cp);
      int *tgt = QST_BOX (int *, cp, sets);
      int sz = box_length (tgt) / sizeof (int);
      if (sz < n)
	tgt = QST_BOX (int *, cp, sets) = (int *) mp_alloc_box_ni (cp_qi->qi_mp, sizeof (int) * n, DV_BIN);
      memcpy_16 (tgt, QST_BOX (int *, inst, sets), n * sizeof (int));
      QST_INT (cp, fill) = n;
    }
}


void
qi_vec_copy_nodes (query_instance_t * qi, caddr_t * cp_inst, query_t * qr, int reset_only)
{
  DO_SET (data_source_t *, qn, &qr->qr_nodes)
  {
    if (!reset_only && qn->src_sets)
      qst_sets_copy ((caddr_t *) qi, cp_inst, qn->src_sets, qn->src_out_fill);
    if (IS_QN (qn, subq_node_input))
      qi_vec_copy_nodes (qi, cp_inst, ((subq_source_t *) qn)->sqs_query, reset_only);
    if (IS_QN (qn, fun_ref_node_input))
      {
	QNCAST (query_instance_t, cp_qi, cp_inst);
	QNCAST (fun_ref_node_t, fref, qn);
	int n_sets = QST_INT (((caddr_t *) qi), fref->src_gen.src_prev->src_out_fill);
	cp_qi->qi_n_sets = n_sets;
	/*  this is done later, also depends on the set nos to be copied later *
	 * fun_ref_set_defaults_and_counts  (fref, cp_inst); */
      }
  }
  END_DO_SET ();
  DO_SET (query_t *, sq, &qr->qr_subq_queries) qi_vec_copy_nodes (qi, cp_inst, sq, reset_only);
  END_DO_SET ();
}


caddr_t *
qst_copy (caddr_t * inst, state_slot_t ** copy_ssls, ssl_index_t * cp_sets)
{
  caddr_t data;
  QNCAST (query_instance_t, qi, inst);
  caddr_t *cp = (caddr_t *) dk_alloc_box_zero (qi->qi_query->qr_instance_length, DV_QI);
  QNCAST (query_instance_t, cpqi, cp);
  query_t *qr = qi->qi_query;
  int sinx;
  IN_CLL;
  qr->qr_ref_count++;
  LEAVE_CLL;
  cpqi->qi_is_allocated = 1;
  cpqi->qi_is_branch = 1;
  cpqi->qi_root_id = qi->qi_root_id;
  cpqi->qi_query = qr;
  cpqi->qi_ref_count = 1;
  if (qi->qi_mp)
    qi_vec_init (cpqi, 0);
  qi_vec_copy_nodes (qi, (caddr_t *) cpqi, qi->qi_query, cp_sets ? 1 : 0);
  if (cp_sets)
    {
      int inx;
      for (inx = 0; inx < box_length (cp_sets) / sizeof (ssl_index_t); inx++)
	qst_sets_copy ((caddr_t *) qi, (caddr_t *) cpqi, cp_sets[inx], cp_sets[inx] + 1);
    }
  if (!copy_ssls)
    copy_ssls = qr->qr_qp_copy_ssls;
  DO_BOX (state_slot_t *, ssl, sinx, copy_ssls)
  {
    if (!ssl)
      continue;
    if (ssl->ssl_sets)
      qst_sets_copy (inst, cp, ssl->ssl_sets, ssl->ssl_n_values);
    switch (ssl->ssl_type)
      {
      case SSL_COLUMN:
      case SSL_VARIABLE:
      case SSL_PARAMETER:
      case SSL_TREE:
	data = qst_get (inst, ssl);
	/* do not copy a qi array because then nested qps get circular refs between these and ref counts do not work with this */
	if (box_is_qp_private (data))
	  break;
	qst_set (cp, ssl, box_mt_copy_tree (data));
	break;
      case SSL_REF_PARAMETER:
	cp[ssl->ssl_index] = inst[ssl->ssl_index];
	break;
      case SSL_VEC:
	dc_copy (QST_BOX (data_col_t *, cp, ssl->ssl_index), QST_BOX (data_col_t *, inst, ssl->ssl_index));
	break;
      default:
	break;
      }
  }
  END_DO_BOX;
  DO_SET (state_slot_t *, ssl, &qr->qr_temp_spaces)
  {
    switch (ssl->ssl_type)
      {
      case SSL_TREE:
	qst_set (cp, ssl, box_copy_tree (qst_get (inst, ssl)));
	break;
      }
  }
  END_DO_SET ();
  cpqi->qi_no_cast_error = qi->qi_no_cast_error;
  cpqi->qi_isolation = qi->qi_isolation;
  cpqi->qi_lock_mode = qi->qi_lock_mode;
  cpqi->qi_u_id = qi->qi_u_id;
  cpqi->qi_g_id = qi->qi_g_id;
  qst_set_hash_part (cp, inst);
  return cp;
}


it_cursor_t *
itc_copy (it_cursor_t * itc)
{
  int inx;
  it_cursor_t *cp = itc_create (NULL, itc->itc_ltrx);
  cp->itc_tree = itc->itc_tree;
  cp->itc_insert_key = itc->itc_insert_key;
  cp->itc_lock_mode = itc->itc_lock_mode;
  cp->itc_isolation = itc->itc_isolation;
  cp->itc_simple_ps = itc->itc_simple_ps;
  cp->itc_landed = 1;
  cp->itc_dive_mode = itc->itc_dive_mode;
  cp->itc_bp = itc->itc_bp;
  cp->itc_n_branches = itc->itc_n_branches;
  cp->itc_multistate_row_specs = itc->itc_multistate_row_specs;
  cp->itc_page = itc->itc_page;
  cp->itc_map_pos = itc->itc_map_pos;
  for (inx = 0; inx < itc->itc_search_par_fill; inx++)
    {
      caddr_t c = NULL;
      if (!itc->itc_n_sets || !ITC_P_VEC (itc, inx))
	c = box_copy_tree (itc->itc_search_params[inx]);
      ITC_SEARCH_PARAM (cp, c);
      if (c)
	ITC_OWNS_PARAM (cp, c);
    }
  cp->itc_ks = itc->itc_ks;
  cp->itc_key_spec = itc->itc_key_spec;
  if (RSP_CHANGED == itc->itc_hash_row_spec)
    {
      cp->itc_row_specs = sp_list_copy (itc->itc_row_specs);
      cp->itc_hash_row_spec = RSP_CHANGED;
    }
  else
    cp->itc_row_specs = itc->itc_row_specs;

  if (itc->itc_n_sets)
    {
      cp->itc_batch_size = itc->itc_batch_size;
      cp->itc_n_sets = itc->itc_n_sets;
      cp->itc_param_order = itc->itc_param_order;	/*copied in the branch qi's mp in ts_thread */
      cp->itc_n_vec_sort_cols = itc->itc_n_vec_sort_cols;
      cp->itc_asc_eq = itc->itc_asc_eq;
      cp->itc_v_out_map = itc->itc_v_out_map;
      cp->itc_same_parent_hit = cp->itc_same_parent_miss = 0;
    }
  cp->itc_n_reads = 0;
  cp->itc_ra_root_fill = 0;
  if (cp->itc_insert_key->key_is_col)
    {
      cp->itc_is_col = 0;
      itc_col_init (cp);
      itc_range (cp, 0, COL_NO_ROW);
      cp->itc_col_row = 0;
    }
  return cp;
}

#if 0
#define qp_printf(q) printf q
#else
#define qp_printf(q)
#endif

void ts_aq_result (table_source_t * ts, caddr_t * inst);



int
ts_handle_aq (table_source_t * ts, caddr_t * inst, buffer_desc_t ** order_buf_ret, int *order_buf_preset)
{
  /* called when continuing a ts with an aq.  Return 1 if ts should return. */
  it_cursor_t *itc = (it_cursor_t *) QST_GET_V (inst, ts->ts_order_cursor);
  int aq_state = QST_INT (inst, ts->ts_aq_state);
  QNCAST (query_instance_t, qi, inst);
  if (aq_state != TS_AQ_COORD_AQ_WAIT)
    {
      if (itc->itc_buf_registered && itc->itc_map_pos >= itc->itc_buf_registered->bd_content_map->pm_count)
	GPF_T1 ("itc regd after end of pagfe in ts_thread ");
      itc->itc_ltrx = qi->qi_trx;
      if (1 == itc->itc_n_sets && !itc->itc_param_order)
	{
	  itc->itc_param_order = (int *) mp_alloc_box (qi->qi_mp, sizeof (int), DV_BIN);
	  itc->itc_param_order[0] = 0;
	}
    }
  switch (aq_state)
    {
    case TS_AQ_PLACED:
      qp_printf (("itc from %d to %d\n", itc->itc_page, itc->itc_boundary->itc_page));
      QST_INT (inst, ts->ts_aq_state) = TS_AQ_SRV_RUN;
    case TS_AQ_SRV_RUN:
      *order_buf_ret = NULL;
      *order_buf_preset = 0;
      return 0;
    case TS_AQ_FIRST:
      {
	ITC_FAIL (itc)
	{
	  *order_buf_ret = itc_reset (itc);
	}
	ITC_FAILED
	{
	}
	END_FAIL (itc);
	qp_printf (("itc from start to %d\n", itc->itc_boundary->itc_page));
	*order_buf_preset = 1;
	QST_INT (inst, ts->ts_aq_state) = TS_AQ_SRV_RUN;
	return 0;
      }
    case TS_AQ_COORD:
      return 0;
    case TS_AQ_COORD_AQ_WAIT:
      {
	/* coordinating ts has done its part.  Wait for the aq branches */
	async_queue_t *aq = QST_BOX (async_queue_t *, inst, ts->ts_aq->ssl_index);
	caddr_t err1 = NULL, err2 = NULL;
	IO_SECT (inst);
	aq->aq_wait_qi = qi;
	aq_wait_all (aq, &err1);
	END_IO_SECT (&err2);
	if (err1)
	  {
	    dk_free_tree (err2);
	    sqlr_resignal (err1);
	  }
	if (err2)
	  sqlr_resignal (err2);
	SRC_IN_STATE (ts, inst) = NULL;
	ts_aq_result (ts, inst);
	return 1;
      }
    }
  return 0;
}


void
ts_aq_handle_end (table_source_t * ts, caddr_t * inst)
{
  /* if the ts is the coordinator of aq branches, wait for them */
  int aq_state = QST_INT (inst, ts->ts_aq_state);
  if (TS_AQ_COORD == aq_state)
    {
      SRC_IN_STATE (ts, inst) = inst;
      QST_INT (inst, ts->ts_aq_state) = TS_AQ_COORD_AQ_WAIT;
    }
}


void
ts_aq_final (table_source_t * ts, caddr_t * inst, it_cursor_t * itc)
{
  int aq_state;
  if (itc)
    ts_check_batch_sz (ts, inst, itc);
  if (!ts->ts_aq)
    return;
  aq_state = QST_INT (inst, ts->ts_aq_state);
  if (TS_AQ_COORD_AQ_WAIT == aq_state)
    {
      ts_handle_aq (ts, inst, NULL, NULL);
      QST_INT (inst, ts->ts_aq_state) = TS_AQ_NONE;
      SRC_IN_STATE (ts, inst) = NULL;
    }
}

caddr_t
aq_qr_func (caddr_t av, caddr_t * err_ret)
{
  caddr_t *args = (caddr_t *) av;
  caddr_t *inst = (caddr_t *) args[0];
  QNCAST (query_instance_t, qi, inst);
  client_connection_t *cli = GET_IMMEDIATE_CLIENT_OR_NULL;
  query_t *qr = (query_t *) (ptrlong) unbox (args[1]);
  qi->qi_trx = cli->cli_trx;
  qi->qi_trx->lt_rc_w_id = unbox (args[2]);
  dk_free_box (args[1]);
  dk_free_box (args[2]);
  dk_free_box (av);
  qi->qi_client = cli;
  if (!cli->cli_user)
    {
      *err_ret = srv_make_new_error ("42000", "SR186", "cli with no user in query branch");
      dk_free_box ((caddr_t) inst);
      return NULL;
    }
  qi->qi_thread = THREAD_CURRENT_THREAD;
  qi->qi_threads = 1;
  QR_RESET_CTX
  {
    qr_resume_pending_nodes (qr, inst);
  }
  QR_RESET_CODE
  {
    if (RST_GB_ENOUGH == reset_code)
      {
	return (caddr_t) qi;
      }
    if (RST_ENOUGH == reset_code)
      {
	return (caddr_t) qi;
      }
    if (RST_ERROR == reset_code)
      {
	*err_ret = thr_get_error_code (qi->qi_thread);
	return (caddr_t) qi;
      }
  }
  END_QR_RESET;
  qi_inc_branch_count (qi, 0, -1);	/* branch completed */
  return (caddr_t) qi;
}


extern long tc_qp_thread;


void
itc_copy_vec_params (it_cursor_t * itc, search_spec_t * sp)
{
  /* set vector params */
  caddr_t *inst = itc->itc_out_state;
  for (sp = sp; sp; sp = sp->sp_next)
    {
      if (sp->sp_min_ssl)
	{
	  if (SSL_VEC == sp->sp_min_ssl->ssl_type)
	    {
	      data_col_t *dc;
	      ITC_P_VEC (itc, sp->sp_min) = dc = QST_BOX (data_col_t *, inst, sp->sp_min_ssl->ssl_index);
	      itc_vec_box (itc, sp->sp_cl.cl_sqt.sqt_col_dtp, sp->sp_min, dc);
	    }
	  else
	    ITC_P_VEC (itc, sp->sp_min) = NULL;
	}
      if (sp->sp_max_ssl)
	{
	  if (SSL_VEC == sp->sp_max_ssl->ssl_type)
	    {
	      data_col_t *dc;
	      ITC_P_VEC (itc, sp->sp_max) = dc = QST_BOX (data_col_t *, inst, sp->sp_max_ssl->ssl_index);
	      itc_vec_box (itc, sp->sp_cl.cl_sqt.sqt_col_dtp, sp->sp_max, dc);
	    }
	  else
	    ITC_P_VEC (itc, sp->sp_max) = NULL;
	}
    }
}

void
fun_ref_reset_setps (fun_ref_node_t * fref, caddr_t * inst)
{
  DO_SET (setp_node_t *, setp, &fref->fnr_setps)
  {
    if (setp->setp_ha && HA_FILL != setp->setp_ha->ha_op)
      qst_set (inst, setp->setp_ha->ha_tree, NULL);
  }
  END_DO_SET ();
}


void
ts_thread (table_source_t * ts, caddr_t * inst, it_cursor_t * itc, int aq_state, int inx)
{
  /* add a thread to ts_aq running over itc */
  QNCAST (query_instance_t, qi, inst);
  caddr_t *cp_inst;
  query_instance_t *cp_qi;
  async_queue_t *aq = (async_queue_t *) QST_GET_V (inst, ts->ts_aq);
  caddr_t **qis = (caddr_t **) QST_GET_V (inst, ts->ts_aq_qis);
  TC (tc_qp_thread);
  qi->qi_client->cli_activity.da_qp_thread++;
  if (itc->itc_buf_registered && itc->itc_map_pos >= itc->itc_buf_registered->bd_content_map->pm_count)
    GPF_T1 ("itc regd after end of pagfe in ts_thread ");
  if (!aq)
    {
      aq = aq_allocate (qi->qi_client, 16);
      aq->aq_do_self_if_would_wait = 1;
      aq->aq_ts = get_msec_real_time ();
      qst_set (inst, ts->ts_aq, (caddr_t) aq);
    }
  if (!qis || BOX_ELEMENTS (qis) <= inx)
    {
      caddr_t **new_qis = (caddr_t **) dk_alloc_box_zero (sizeof (caddr_t) * (inx + 20), DV_ARRAY_OF_POINTER);
      if (qis)
	memcpy (new_qis, qis, box_length (qis));
      QST_BOX (caddr_t **, inst, ts->ts_aq_qis->ssl_index) = new_qis;
      dk_free_box ((caddr_t) qis);
      qis = new_qis;
    }
  cp_inst = qis[inx];
  if (!cp_inst)
    cp_inst = qst_copy (inst, ts->ts_branch_ssls, ts->ts_branch_sets);
  else
    qst_set_hash_part (cp_inst, inst);
  qis[inx] = cp_inst;
  cp_qi = (query_instance_t *) cp_inst;
  qst_set (cp_inst, ts->ts_order_cursor, (caddr_t) itc);
  itc->itc_out_state = cp_inst;
  itc_copy_vec_params (itc, ts->ts_order_ks->ks_spec.ksp_spec_array);
  itc_copy_vec_params (itc, ts->ts_order_ks->ks_row_spec);
  if (IS_QN (ts->ts_agg_node, select_node_input_subq))
    ((query_instance_t *) cp_inst)->qi_branched_select = (select_node_t *) ts->ts_agg_node;
  else
    {
      /* making a copy for aggregation, can be that agg is already inited in original, so reset it */
      QNCAST (fun_ref_node_t, fref, ts->ts_agg_node);
      int n_sets = QST_INT (inst, fref->src_gen.src_prev->src_out_fill);
      qn_init ((table_source_t *) ts->ts_agg_node, cp_inst);
      cp_qi->qi_n_sets = n_sets;
      fun_ref_set_defaults_and_counts (fref, cp_inst);
      fun_ref_reset_setps (fref, cp_inst);
    }
  if (itc->itc_param_order)
    {
      QNCAST (query_instance_t, cp_qi, cp_inst);
      int *order = itc->itc_param_order;
      itc->itc_param_order = (int *) mp_alloc_box_ni (cp_qi->qi_mp, sizeof (int) * itc->itc_n_sets, DV_BIN);
      memcpy_16_nt (itc->itc_param_order, order, sizeof (int) * itc->itc_n_sets);
      itc_set_param_row (itc, itc->itc_set);
    }
  if (ts->ts_branch_by_value)
    aq_state = TS_AQ_FIRST;
  QST_INT (cp_inst, ts->ts_aq_state) = aq_state;
  SRC_IN_STATE (ts, cp_inst) = cp_inst;
  aq_request (aq, aq_qr_func, list (3, box_copy ((caddr_t) cp_inst), box_num ((ptrlong) ts->src_gen.src_query),
	  box_num (qi->qi_trx->lt_rc_w_id ? qi->qi_trx->lt_rc_w_id : qi->qi_trx->lt_w_id)));
}


search_spec_t *
sp_list_copy (search_spec_t * sp)
{
  search_spec_t *copy_1 = NULL;
  search_spec_t **copy = &copy_1;
  for (sp = sp; sp; sp = sp->sp_next)
    {
      *copy = sp_copy (sp);
      copy = &(*copy)->sp_next;
    }
  return copy_1;
}


void
itc_add_value_range_spec (it_cursor_t * itc, caddr_t lower, caddr_t upper, int min_op, int max_op)
{
  search_spec_t *sp, *copy_1 = NULL;
  search_spec_t **copy = &copy_1;
  NEW_VARZ (search_spec_t, r_sp);
  itc->itc_local_key_spec = 1;
  itc->itc_key_spec.ksp_key_cmp = pg_key_compare;
  for (sp = itc->itc_row_specs; sp; sp = sp->sp_next)
    {
      *copy = sp_copy (sp);
      copy = &(*copy)->sp_next;
    }
  *copy = r_sp;
  itc->itc_key_spec.ksp_spec_array = copy_1;
  r_sp->sp_min_op = min_op;
  if (CMP_NONE != min_op)
    {
      r_sp->sp_min = itc->itc_search_par_fill++;
      itc->itc_search_params[r_sp->sp_min] = lower;
      ITC_OWNS_PARAM (itc, lower);
    }
  r_sp->sp_max_op = max_op;
  if (CMP_NONE != max_op)
    {
      r_sp->sp_max = itc->itc_search_par_fill++;
      itc->itc_search_params[r_sp->sp_max] = upper;
      ITC_OWNS_PARAM (itc, upper);
    }
  if (itc->itc_is_col)
    {
      if (RSP_CHANGED != itc->itc_hash_row_spec)
	itc->itc_row_specs = sp_list_copy (itc->itc_row_specs);
      ks_spec_add (&itc->itc_row_specs, sp_copy (r_sp));
      itc->itc_hash_row_spec = RSP_CHANGED;
    }
}


void
ts_reset_qis (table_source_t * ts, caddr_t * inst, caddr_t ** aq_qis)
{
  int inx;
  if (!aq_qis)
    return;
  qst_set (inst, ts->ts_aq_qis, NULL);
  return;
  DO_BOX (caddr_t *, branch, inx, aq_qis)
  {
    if (branch)
      qst_set (branch, ts->ts_order_cursor, NULL);
  }
  END_DO_BOX;
}



typedef struct ts_split_state_s
{
  int tsp_nth_call;
  int64 tsp_card_est;
  int tsp_n_parts;
  int tsp_org_n_parts;
  int tsp_nth_part;
  int tsp_rows_per_part;
  table_source_t *tsp_ts;
  caddr_t tsp_prev_value;
  caddr_t tsp_value;
} ts_split_state_t;

#define TSS_NO_SPLIT 0
#define TSS_NEXT 1
#define TSS_LAST 2
#define TSS_AT_END 3


int
itc_angle (it_cursor_t * itc, buffer_desc_t ** buf_ret, int angle, placeholder_t * prev, ts_split_state_t * tsp)
{

  /* if previous is given, check that the place is in fact to the right of previous.  Tree could have split in between.
   * Retry until can read both the place and the previous.  If landed to the left of previous, return NULL */
  table_source_t *ts = tsp->tsp_ts;
  QNCAST (query_instance_t, qi, itc->itc_out_state);
  int64 est;
  float cost;
  search_spec_t *rs = itc->itc_row_specs;
  int64 n_leaves;
  int rc;
  if (itc->itc_insert_key->key_is_col && !itc->itc_is_col)
    itc_col_init (itc);		/* init the column inx data while the row specs are in place, else gonna miss cr's */
  itc->itc_row_specs = NULL;
  itc->itc_random_search = RANDOM_SEARCH_ON;
  *buf_ret = itc_reset (itc);
  itc->itc_random_search = RANDOM_SEARCH_OFF;
  itc_clear_stats (itc);
  itc->itc_st.mode = ITC_STAT_ANGLE;
  est = itc_sample_1 (itc, buf_ret, &n_leaves, angle);
  if (!tsp->tsp_card_est)
    tsp->tsp_card_est = est;
  itc->itc_row_specs = rs;
  itc->itc_bp.bp_new_on_row = 1;
  itc->itc_bp.bp_just_landed = 1;
  itc->itc_bp.bp_is_pos_valid = 0;
  if (tsp->tsp_ts)
    {
      cost = (ts->ts_cost_after / ts->ts_cardinality) * est;
      cost *= compiler_unit_msecs * 1000;
      if (1 == tsp->tsp_nth_call)
	{
	  if (cost < qp_thread_min_usec || est < MAX (10, qp_range_split_min_rows))
	    {
	      itc_page_leave (itc, *buf_ret);
	      tsp->tsp_n_parts = 0;
	      return TSS_NO_SPLIT;
	    }
	  if (cost < tsp->tsp_n_parts * qp_thread_min_usec)
	    tsp->tsp_n_parts = MAX (2, (int) (cost / qp_thread_min_usec));
	  tsp->tsp_n_parts = 1 + qi_inc_branch_count (qi, enable_qp, tsp->tsp_n_parts - 1);
	}
    }
  if (tsp->tsp_ts->ts_branch_col)
    {
      dk_free_box (tsp->tsp_value);
      tsp->tsp_value = itc_box_column (itc, *buf_ret, tsp->tsp_ts->ts_branch_col->col_id, NULL);
    }
  itc->itc_landed = 1;
  itc->itc_is_on_row = 0;
  if (prev)
    {
      if (prev->itc_page == itc->itc_page)
	{
	  if (prev->itc_map_pos >= itc->itc_map_pos)
	    goto not_greater;
	}
      else
	{
	  /* the cursor has landed on a page different from the previous.  See that this is gt. If it moved or was being written, try no more and say it was not gt */
	  buffer_desc_t *reg_buf = prev->itc_buf_registered;
	  dp_addr_t org_page = reg_buf->bd_page;
	  ITC_IN_KNOWN_MAP (itc, reg_buf->bd_page);
	  if (prev->itc_buf_registered != reg_buf || reg_buf->bd_page != org_page || reg_buf->bd_is_write)
	    {
	      ITC_LEAVE_MAP_NC (itc);
	      goto not_greater;
	    }
	  rc = buf_row_compare (reg_buf, prev->itc_map_pos, *buf_ret, itc->itc_map_pos, 0);
	  ITC_LEAVE_MAP_NC (itc);
	  if (rc != DVC_LESS)
	    goto not_greater;
	}
    }
  return TSS_NEXT;
not_greater:
  itc_page_leave (itc, *buf_ret);
  return TSS_NO_SPLIT;
}


buffer_desc_t *
ts_split_sets (table_source_t * ts, caddr_t * inst, it_cursor_t * itc, int n_parts)
{
  QNCAST (query_instance_t, qi, itc->itc_out_state);
  float usecs;
  int n_sets = itc->itc_n_sets, chunk;
  int n_ways, ctr = 0, inx;
  qst_set (inst, ts->ts_aq, NULL);
  usecs = itc->itc_n_sets * ts->ts_cost_after * compiler_unit_msecs * 1000;
  if (!enable_split_sets || usecs < qp_thread_min_usec)
    return itc_reset (itc);
  n_ways = 1 + (usecs / qp_thread_min_usec);
  if (n_ways > enable_qp)
    n_ways = enable_qp;
  n_ways = 1 + qi_inc_branch_count (qi, enable_qp, n_ways - 1);
  if (n_ways < 2)
    return itc_reset (itc);
  if (n_ways > n_sets)
    n_ways = n_sets;
  chunk = n_sets / n_ways;
  for (inx = 0; inx < n_ways - 1; inx++)
    {
      it_cursor_t *cp_itc = itc_copy (itc);
      cp_itc->itc_set = cp_itc->itc_first_set = inx * chunk;
      cp_itc->itc_n_sets = chunk * (inx + 1);
      cp_itc->itc_param_order = itc->itc_param_order;
      ts_thread (ts, inst, cp_itc, TS_AQ_FIRST, ctr++);
    }
  QST_INT (inst, ts->ts_aq_state) = TS_AQ_COORD;
  itc->itc_set = itc->itc_first_set = chunk * inx;
  itc_set_param_row (itc, itc->itc_set);
  return itc_reset (itc);
}


int
tsp_next_col (ts_split_state_t * tsp, it_cursor_t * itc, buffer_desc_t ** buf_ret, it_cursor_t * prev)
{
  dbe_key_t *key = itc->itc_insert_key;
  jmp_buf_splice *save = itc->itc_fail_context;
  int sets_save = itc->itc_n_sets;
  int rows_per_seg = key->key_segs_sampled ? key->key_rows_in_sampled_segs / key->key_segs_sampled : 3000;
  int n_parts = tsp->tsp_n_parts;
  int rows_per_part, rows_to_go, rc;
  ITC_FAIL (itc)
  {
    if (!prev)
      {
	rc = itc_angle (itc, buf_ret, 0, NULL, tsp);
	itc->itc_n_branches = tsp->tsp_n_parts;
	if (TSS_NO_SPLIT == rc)
	  {
	    itc->itc_fail_context = save;
	    return rc;
	  }
	if (!itc->itc_key_spec.ksp_spec_array)
	  tsp->tsp_card_est = dbe_key_count (itc->itc_insert_key);
	else if (tsp->tsp_ts->ts_inx_cardinality)
	  tsp->tsp_card_est = tsp->tsp_ts->ts_inx_cardinality;
	tsp->tsp_org_n_parts = n_parts = tsp->tsp_n_parts;
      }
    else
      *buf_ret = itc_set_by_placeholder (itc, (placeholder_t *) prev);
    if (tsp->tsp_nth_call > tsp->tsp_n_parts + 1)
      {
	qi_inc_branch_count ((query_instance_t *) itc->itc_out_state, INT32_MAX, 1);
	tsp->tsp_n_parts++;
      }
    rows_per_part = (tsp->tsp_card_est / rows_per_seg) / tsp->tsp_org_n_parts;
    rows_to_go = MAX (1 + tsp->tsp_n_parts - tsp->tsp_org_n_parts, rows_per_part);
    /* rows to go is at leat 1 and if doing the range is taking more branches than predicted then it is that much more rows */
    for (;;)
      {
	int n_rows = (*buf_ret)->bd_content_map->pm_count, rc;
	if (rows_to_go < n_rows - itc->itc_map_pos)
	  {
	    itc->itc_map_pos += rows_to_go;
	    itc->itc_fail_context = save;
	    return TSS_NEXT;
	  }
	rows_to_go -= n_rows - itc->itc_map_pos;
	itc->itc_is_on_row = 1;
	itc->itc_map_pos = n_rows - 1;
	itc->itc_is_col = 0;
	itc->itc_n_sets = 0;
	rc = itc_next (itc, buf_ret);
	itc->itc_is_col = 1;
	itc->itc_n_sets = sets_save;
	if (DVC_GREATER == rc || DVC_INDEX_END == rc)
	  {
	    page_leave_outside_map (*buf_ret);
	    itc->itc_fail_context = save;
	    return TSS_AT_END;
	  }
      }
  }
  ITC_FAILED
  {
    itc->itc_is_col = 1;
  }
  END_FAIL (itc);
  itc->itc_fail_context = save;
}


int enable_col_split = 1;

int
tsp_next (ts_split_state_t * tsp, it_cursor_t * itc, buffer_desc_t ** buf_ret, it_cursor_t * prev)
{
  int nth = ++tsp->tsp_nth_call;
  if (itc->itc_insert_key->key_is_col && enable_col_split)
    return tsp_next_col (tsp, itc, buf_ret, prev);
  for (;;)
    {
      int angle = 999 * nth / tsp->tsp_n_parts;
      int rc = itc_angle (itc, buf_ret, angle, (placeholder_t *) prev, tsp);
      itc->itc_n_branches = tsp->tsp_n_parts;
      if (TSS_NEXT == rc)
	return nth == tsp->tsp_n_parts - 1 ? TSS_LAST : TSS_NEXT;
      if (!tsp->tsp_n_parts)
	return TSS_NO_SPLIT;
      if (nth == tsp->tsp_n_parts - 1)
	return TSS_NO_SPLIT == rc ? TSS_AT_END : TSS_LAST;
      nth++;
      tsp->tsp_nth_call++;
      if (nth > enable_qp)
	return TSS_AT_END;
    }
}


buffer_desc_t *
ts_split_range (table_source_t * ts, caddr_t * inst, it_cursor_t * itc, int n_parts)
{
  QNCAST (query_instance_t, qi, itc->itc_out_state);
  int ctr = 0, n_branches;
  it_cursor_t *cp_itc = NULL;
  buffer_desc_t *buf;
  it_cursor_t *prev = NULL;
  ts_split_state_t tsp;
  caddr_t aq_qis = qst_get (inst, ts->ts_aq_qis);
  ts_reset_qis (ts, inst, (caddr_t **) aq_qis);
  n_branches = qi_inc_branch_count (qi, 0, 0);
  if (n_branches >= enable_qp)
    return itc_reset (itc);
  if (-1 == n_branches)
    sqlr_new_error ("42000", "VEC..", "The root branch has terminated, so no point in branching more branch qis");
  if (itc->itc_n_sets > 1)
    return ts_split_sets (ts, inst, itc, n_parts);
  memset (&tsp, 0, sizeof (tsp));
  if (!enable_split_range)
    return itc_reset (itc);
  tsp.tsp_ts = ts;
  tsp.tsp_n_parts = n_parts;
  qst_set (inst, ts->ts_aq, NULL);
  for (;;)
    {
      int rc = tsp_next (&tsp, itc, &buf, prev);
      if (TSS_NO_SPLIT == rc)
	{
	  QST_INT (inst, ts->ts_aq_state) = 0;
	  return itc_reset (itc);
	}
      if (TSS_NEXT == rc)
	{
	  if (prev)
	    {
	      prev->itc_boundary = plh_landed_copy ((placeholder_t *) itc, buf);
	      if (itc->itc_map_pos >= buf->bd_content_map->pm_count)
		GPF_T1 ("ts reg after end");
	      ts_thread (ts, inst, prev, TS_AQ_PLACED, ctr++);
	      prev = itc_copy (itc);
	      if (prev->itc_map_pos >= buf->bd_content_map->pm_count)
		GPF_T1 ("ts reg after end");
	      itc_register_and_leave (prev, buf);
	    }
	  else
	    {
	      cp_itc = itc_copy (itc);
	      cp_itc->itc_boundary = plh_landed_copy ((placeholder_t *) itc, buf);
	      if (itc->itc_map_pos >= buf->bd_content_map->pm_count)
		GPF_T1 ("ts reg after end");
	      prev = itc_copy (itc);
	      if (prev->itc_map_pos >= buf->bd_content_map->pm_count)
		GPF_T1 ("ts reg after end");
	      itc_register_and_leave (prev, buf);
	      ts_thread (ts, inst, cp_itc, TS_AQ_FIRST, ctr++);
	    }
	}
      else if (TSS_LAST == rc)
	{
	  if (prev)
	    {
	      prev->itc_boundary = plh_landed_copy ((placeholder_t *) itc, buf);
	      ts_thread (ts, inst, prev, TS_AQ_PLACED, ctr++);
	      QST_INT (inst, ts->ts_aq_state) = TS_AQ_COORD;
	      if (ctr + 1 != tsp.tsp_n_parts)
		qi_inc_branch_count (qi, 10000, 1 + ctr - tsp.tsp_n_parts);
	      return buf;
	    }
	  else
	    {
	      cp_itc = itc_copy (itc);
	      cp_itc->itc_boundary = plh_landed_copy ((placeholder_t *) itc, buf);
	      ts_thread (ts, inst, cp_itc, TS_AQ_FIRST, ctr++);
	      QST_INT (inst, ts->ts_aq_state) = TS_AQ_COORD;
	      if (ctr + 1 != tsp.tsp_n_parts)
		qi_inc_branch_count (qi, 10000, 1 + ctr - tsp.tsp_n_parts);
	      return buf;
	    }
	}
      else if (TSS_AT_END == rc)
	{
	  if (!prev)
	    {
	      QST_INT (inst, ts->ts_aq_state) = 0;
	      if (ctr + 1 != tsp.tsp_n_parts)
		qi_inc_branch_count (qi, 10000, 1 + ctr - tsp.tsp_n_parts);
	      return itc_reset (itc);
	    }
	  else
	    {
	      buf = itc_set_by_placeholder (itc, (placeholder_t *) prev);
	      itc_unregister_inner (prev, buf, 0);
	      itc_free (prev);
	      QST_INT (inst, ts->ts_aq_state) = TS_AQ_COORD;
	      if (ctr + 1 != tsp.tsp_n_parts)
		qi_inc_branch_count (qi, 10000, 1 + ctr - tsp.tsp_n_parts);
	      return buf;
	    }
	}
    }
}


int qp_even_if_lock = 0;

buffer_desc_t *
ts_initial_itc (table_source_t * ts, caddr_t * inst, it_cursor_t * itc)
{
  QNCAST (query_instance_t, qi, inst);
  if (ts->ts_aq)
    QST_INT (inst, ts->ts_aq_state) = TS_AQ_NONE;
  if (!ts->ts_aq || itc->itc_isolation > ISO_COMMITTED
      || (itc->itc_lock_mode != PL_SHARED && !qp_even_if_lock)
      || KI_TEMP == itc->itc_insert_key->key_id
      || enable_qp < 2
      || itc->itc_n_sets * ts->ts_cost_after * compiler_unit_msecs * 1000 < 2 * qp_thread_min_usec || itc->itc_is_vacuum)
    return itc_reset (itc);
  if (!qi->qi_is_branch && !qi->qi_root_id)
    qi_assign_root_id (qi);
  return ts_split_range (ts, inst, itc, enable_qp);
}


dk_mutex_t *qi_ref_mtx;
dk_hash_t *qi_branch_count;
uint32 qi_root_id_ctr;

void
qi_assign_root_id (query_instance_t * qi)
{
  uint32 id;
  mutex_enter (qi_ref_mtx);
  do
    {
      id = qi_root_id_ctr++;
    }
  while (id == 0 || gethash ((void *) (ptrlong) id, qi_branch_count));
  sethash ((void *) (ptrlong) id, qi_branch_count, (void *) 1);
  qi->qi_root_id = id;
  mutex_leave (qi_ref_mtx);
}


int
qi_inc_branch_count (query_instance_t * qi, int max, int n)
{
  uint32 br, ret = 0;
  mutex_enter (qi_ref_mtx);
  br = (ptrlong) gethash ((void *) (ptrlong) qi->qi_root_id, qi_branch_count);
  if (!br)
    ret = !n ? -1 : 2;		/*if root has finished and asking thread count say -1, else give 2 branches because cannot signal error at this point */
  else if (!n)
    ret = br;
  else if (n < 0)
    sethash ((void *) (ptrlong) qi->qi_root_id, qi_branch_count, (void *) (ptrlong) (br + n));
  else
    {
      if (br + n > max)
	n = max - br;
      sethash ((void *) (ptrlong) qi->qi_root_id, qi_branch_count, (void *) (ptrlong) (br + n));
      ret = n;
    }
  mutex_leave (qi_ref_mtx);
  return ret;
}


void
qi_root_done (query_instance_t * qi)
{
  mutex_enter (qi_ref_mtx);
  remhash ((void *) (ptrlong) qi->qi_root_id, qi_branch_count);
  mutex_leave (qi_ref_mtx);
}


void
qi_qr_done (query_t * qr)
{
  IN_CLL;
  qr->qr_ref_count--;
  LEAVE_CLL;
}


int
qi_free_cb (caddr_t x)
{
  QNCAST (query_instance_t, qi, x);
  query_t *qr = qi->qi_query;
  int n;
  mutex_enter (qi_ref_mtx);
  n = --qi->qi_ref_count;
  if (n < 0)
    GPF_T1 ("qi ref count neg");
  mutex_leave (qi_ref_mtx);
  if (n)
    return 1;
  box_tag_modify (qi, DV_CUSTOM);
  qi_free ((caddr_t *) qi);
  qi_qr_done (qr);
  return 1;
}

caddr_t
qi_copy_cb (caddr_t x)
{
  QNCAST (query_instance_t, qi, x);
  mutex_enter (qi_ref_mtx);
  qi->qi_ref_count++;
  mutex_leave (qi_ref_mtx);
  return x;
}


void
ts_merge_subq_branches (table_source_t * ts, caddr_t * inst)
{
  int ign;
  QNCAST (query_instance_t, qi, inst);
  select_node_t *sel = (select_node_t *) ts->ts_agg_node;
  db_buf_t main_bits = QST_BOX (db_buf_t, inst, sel->sel_vec_set_mask);
  set_ctr_node_t *sctr = (set_ctr_node_t *) ts->src_gen.src_query->qr_head_node;
  data_col_t *set_nos;
  int n_sets;
  caddr_t ***qis = (caddr_t ***) QST_GET_V (inst, ts->ts_aq_qis);
  int qi_inx;
  if (IS_QN (ts->ts_agg_node, hash_fill_node_input))
    return;
  if (!qis)
    return;

  set_nos = QST_BOX (data_col_t *, inst, sctr->sctr_set_no->ssl_index);
  n_sets = ((int64 *) set_nos->dc_values)[set_nos->dc_n_values - 1] + 1;
  DO_BOX (caddr_t *, branch, qi_inx, qis)
  {
    db_buf_t branch_bits;
    int b;
    if (!branch)
      continue;
    branch_bits = QST_BOX (db_buf_t, branch, sel->sel_vec_set_mask);
    if (!branch_bits)
      continue;

    if (SEL_VEC_EXISTS == sel->sel_vec_role)
      {
	int n_bits = MIN (n_sets, box_length (branch_bits) * 8);
	int bits_bytes = ALIGN_8 (n_bits) / 8;
	if (!main_bits)
	  {
	    main_bits = QST_BOX (db_buf_t, inst, sel->sel_vec_set_mask) =
		(db_buf_t) mp_full_box_copy_tree (qi->qi_mp, (caddr_t) branch_bits);
	    continue;
	  }
	if (box_length (main_bits) < bits_bytes)
	  main_bits = sel_extend_bits (sel, inst, n_sets, &ign);
	for (b = 0; b < bits_bytes; b++)
	  main_bits[b] |= branch_bits[b];
      }
    else
      {
	QNCAST (query_instance_t, branch_qi, branch);
	int bit, n_bits = MIN (n_sets, box_length (branch_bits) * 8);
	int bits_bytes = ALIGN_8 (n_bits) / 8;
	if (!sel->sel_vec_set_mask)
	  GPF_T1 ("parallel ts result merge expects to have a sets mask");
	if (!main_bits)
	  {
	    main_bits = QST_BOX (db_buf_t, inst, sel->sel_vec_set_mask) = (db_buf_t) mp_alloc_box (qi->qi_mp, bits_bytes, DV_BIN);
	    memzero (main_bits, box_length (main_bits));
	  }
	if (box_length (main_bits) * 8 < n_bits)
	  main_bits = sel_extend_bits (sel, inst, n_sets, &ign);
	for (bit = 0; bit < n_bits; bit++)
	  {
	    if (!BIT_IS_SET (main_bits, bit) && BIT_IS_SET (branch_bits, bit))
	      {
		BIT_SET (main_bits, bit);
		qi->qi_set = bit;
		branch_qi->qi_set = bit;
		qst_set (inst, sel->sel_scalar_ret, box_copy_tree (qst_get (branch, sel->sel_scalar_ret)));
	      }
	  }
      }
  }
  END_DO_BOX;
}


void
vec_fref_single_result (fun_ref_node_t * fref, table_source_t * ts, caddr_t * inst, int n_sets)
{
  QNCAST (query_instance_t, qi, inst);
  set_ctr_node_t *sctr = (set_ctr_node_t *) fref->src_gen.src_query->qr_head_node;
  data_col_t *set_nos;
  caddr_t ***qis = (caddr_t ***) QST_GET_V (inst, ts->ts_aq_qis);
  int qi_inx, set;
  if (!qis)
    return;
  if (!IS_QN (sctr, set_ctr_input))
    {
      select_node_t *sel = fref->src_gen.src_query->qr_select_node;
      if (!sel)
	sqlr_new_error ("42000", "VEC..", "Internal error, aggregation subq is supposed to start with sctr");
      set_nos = QST_BOX (data_col_t *, inst, sel->sel_set_no->ssl_index);
      if (!set_nos)
	sqlr_new_error ("42000", "VEC..", "Internal error, aggregation subq does not have a set no in select");
    }
  else
    set_nos = QST_BOX (data_col_t *, inst, sctr->sctr_set_no->ssl_index);
  DO_BOX (caddr_t *, branch, qi_inx, qis)
  {
    int inx = 0;
    if (!branch)
      continue;
    for (set = 0; set < n_sets; set++)
      {
	int agg_set = ((int64 *) set_nos->dc_values)[set];
	qi->qi_set = agg_set;
	((query_instance_t *) branch)->qi_set = agg_set;
	DO_SET (state_slot_t *, ssl, &fref->fnr_default_ssls)
	{
	  caddr_t new_val = qst_get (branch, ssl);
	  if (DV_DB_NULL == DV_TYPE_OF (new_val))
	    {
	      inx++;
	      continue;
	    }
	  if (ssl->ssl_name[0] == 'b' || ssl->ssl_name[0] == 'm')
	    {
	      int rc, is_max = ssl->ssl_name[0] == 'b';
	      rc = cmp_boxes (new_val, qst_get (inst, ssl), NULL, NULL);
	      if (DVC_UNKNOWN == rc || (rc == DVC_LESS && !is_max) || (rc == DVC_GREATER && is_max))
		{
		  qst_set (inst, ssl, box_copy_tree (new_val));
		}
	    }
	  else
	    {
	      caddr_t old = qst_get (inst, ssl);
	      if (DV_DB_NULL == DV_TYPE_OF (old))
		qst_set (inst, ssl, box_copy (new_val));
	      else
		box_add (old, new_val, inst, ssl);
	    }
	  inx++;
	}
	END_DO_SET ();
      }
  }
  END_DO_BOX;
}

state_slot_t *
fref_agg_set_no (fun_ref_node_t * fref)
{
  table_source_t *read_node = (table_source_t *) qn_next ((data_source_t *) fref);
  if (IS_QN (read_node, chash_read_input) || IS_QN (read_node, sort_read_input) || IS_TS (read_node))
    {
      return read_node->ts_order_ks->ks_set_no;
    }
  sqlr_new_error ("42000", "VEC..", "cube ks set no not yet done ");
  return NULL;
}



#if 0
void
ks_ha_out (key_source_t * ks, it_cursor_t * itc, setp_node_t * setp)
{
  /* set the ks_out_* so it gets stuff from the setp */
  mem_pool_t *mp = NULL;
  hash_area_t *ha = setp->setp_ha;
  int n = BOX_ELEMENTS (ha->ha_slots), inx;
  out_map_t *om = (out_map_t *) dk_alloc_box (sizeof (out_map_t) * n, DV_BIN);
  for (inx = 0; inx < n; inx++)
    {
      om[inx].om_cl = ha->ha_key_cols[inx];
      dk_set_push (&itcl->itcl_out_slots, (void *) ha->ha_slots[inx]);
    }
  ks->ks_out_map = om;
  itc->itc_out_map = om;
  ks->ks_out_slots = dk_set_nreverse (itcl->itcl_out_slots);
}
#endif


void
vec_merge_setp (setp_node_t ** setp_ret, hash_area_t ** ha_ret, setp_node_t * tmp_setp, hash_area_t * tmp_ha,
    state_slot_t * tmp_ssls)
{
  /* make identical setp/ha with single state ssls in the place of vector or ref ssls */
  int inx;
  *tmp_setp = **setp_ret;
  tmp_setp->setp_ssa.ssa_set_no = NULL;
  tmp_setp->setp_ha = tmp_ha;
  tmp_setp->setp_dependent_box = (state_slot_t **) box_copy ((caddr_t) tmp_setp->setp_dependent_box);
  if (tmp_ha)
    {
      *tmp_ha = **ha_ret;
      tmp_ha->ha_set_no = NULL;
      tmp_ha->ha_slots = (state_slot_t **) box_copy ((caddr_t) tmp_ha->ha_slots);
      DO_BOX (state_slot_t *, ssl, inx, tmp_ha->ha_slots)
      {
	tmp_ha->ha_slots[inx] = ssl_single_state_shadow (ssl, &tmp_ssls[inx]);
	if (inx >= tmp_ha->ha_n_keys)
	  tmp_setp->setp_dependent_box[inx - tmp_ha->ha_n_keys] = tmp_ha->ha_slots[inx];
      }
      END_DO_BOX;
      *ha_ret = tmp_ha;
    }
  else
    {
      int fill = 0;
      tmp_setp->setp_keys_box = (state_slot_t **) box_copy ((caddr_t) tmp_setp->setp_keys_box);
      DO_BOX (state_slot_t *, ssl, inx, tmp_setp->setp_keys_box)
      {
	tmp_setp->setp_keys_box[inx] = ssl_single_state_shadow (ssl, &tmp_ssls[fill++]);
      }
      END_DO_BOX;
      DO_BOX (state_slot_t *, ssl, inx, tmp_setp->setp_dependent_box)
      {
	tmp_setp->setp_dependent_box[inx] = ssl_single_state_shadow (ssl, &tmp_ssls[fill++]);
      }
      END_DO_BOX;

    }
  *setp_ret = tmp_setp;
}


void
vec_top_merge (setp_node_t * setp, fun_ref_node_t * fref, caddr_t * inst, caddr_t * branch, state_slot_t * tmp_ssl, int n_ssl)
{
  int nth;
  caddr_t **arr = (caddr_t **) qst_get (branch, setp->setp_sorted);
  ptrlong top = unbox (qst_get (inst, setp->setp_top));
  ptrlong skip = setp->setp_top_skip ? unbox (qst_get (inst, setp->setp_top_skip)) : 0;
  ptrlong fill = unbox (qst_get (branch, setp->setp_row_ctr));
  QNCAST (query_instance_t, qi, inst);
  setp_node_t tmp_setp;
  top += skip;
  skip = 0;
  if (!arr)
    return;
  if (fref->src_gen.src_prev)
    {
      if (BOX_ELEMENTS (setp->setp_keys_box) + BOX_ELEMENTS (setp->setp_dependent_box) > n_ssl)
	sqlr_new_error ("42000", "VEC..", "Too many order by or group by columns in parallel query branch merge");
      vec_merge_setp (&setp, NULL, &tmp_setp, NULL, &tmp_ssl[0]);
    }
  for (nth = 0; nth < fill; nth++)
    {
      int k_inx = 0, inx;
      DO_BOX (state_slot_t *, ssl, inx, setp->setp_keys_box)
      {
	qst_set (inst, ssl, arr[nth][k_inx]);
	arr[nth][k_inx] = NULL;
	k_inx++;
      }
      END_DO_BOX;
      DO_BOX (state_slot_t *, ssl, inx, setp->setp_dependent_box)
      {
	qst_set (inst, ssl, arr[nth][k_inx]);
	arr[nth][k_inx] = NULL;
	k_inx++;
      }
      END_DO_BOX;
      setp_mem_sort (setp, inst, 1, qi->qi_set);
    }
  if (&tmp_setp == setp)
    {
      dk_free_box ((caddr_t) setp->setp_keys_box);
      dk_free_box ((caddr_t) setp->setp_dependent_box);
    }
}


void
it_print_wired (index_tree_t * it)
{
  int inx;
  for (inx = 0; inx < it_n_maps; inx++)
    {
      DO_HT (ptrlong, dp, buffer_desc_t *, buf, &it->it_maps[inx].itm_dp_to_buf)
      {
	if (buf->bd_is_write || buf->bd_readers)
	  {
	    printf ("%d %s\n", (int) dp, buf->bd_is_write ? "w" : "r");
	  }
      }
      END_DO_HT;
    }
}


int
vec_fref_chash_result (fun_ref_node_t * fref, table_source_t * ts, caddr_t * inst, int n_sets)
{
  /* true if chash only */
  int set;
  QNCAST (query_instance_t, qi, inst);
  caddr_t ***qis = (caddr_t ***) QST_GET_V (inst, ts->ts_aq_qis);
  int qi_inx;
  state_slot_t *agg_set_no = fref_agg_set_no (fref);
  if (!qis)
    return 1;
  qi->qi_set = 0;
  DO_BOX (caddr_t *, branch, qi_inx, qis)
  {
    if (!branch)
      continue;
    for (set = 0; set < n_sets; set++)
      {
	int set_in_sctr = 1 == n_sets ? 0 : qst_vec_get_int64 (inst, agg_set_no, set);
	((query_instance_t *) branch)->qi_set = set_in_sctr;
	qi->qi_set = set_in_sctr;
	DO_SET (setp_node_t *, setp, &fref->fnr_setps)
	{
	  hash_area_t *ha = setp->setp_ha;
	  index_tree_t *local_tree = (index_tree_t *) qst_get (inst, ha->ha_tree);
	  index_tree_t *tree = (index_tree_t *) qst_get (branch, ha->ha_tree);
	  if (!tree)
	    continue;
	  if (tree->it_hi && !tree->it_hi->hi_chash)
	    return 0;
	}
	END_DO_SET ();
      }
  }
  END_DO_BOX;

  DO_BOX (caddr_t *, branch, qi_inx, qis)
  {
    if (!branch)
      continue;
    for (set = 0; set < n_sets; set++)
      {
	int set_in_sctr = qst_vec_get_int64 (inst, agg_set_no, set);
	((query_instance_t *) branch)->qi_set = set_in_sctr;
	qi->qi_set = set_in_sctr;
	DO_SET (setp_node_t *, setp, &fref->fnr_setps)
	{
	  hash_area_t *ha = setp->setp_ha;
	  index_tree_t *local_tree = (index_tree_t *) qst_get (inst, ha->ha_tree);
	  index_tree_t *tree = (index_tree_t *) qst_get (branch, ha->ha_tree);
	  if (!tree)
	    continue;
	  if (!local_tree)
	    qst_set (inst, ha->ha_tree, box_copy_tree ((caddr_t) tree));
	  else
	    chash_merge (setp, local_tree->it_hi->hi_chash, tree->it_hi->hi_chash);
	}
	END_DO_SET ();
      }
  }
  END_DO_BOX;
  return 1;
}


void
vec_fref_group_result (fun_ref_node_t * fref, table_source_t * ts, caddr_t * inst, int n_sets)
{
  int set;
  QNCAST (query_instance_t, qi, inst);
  state_slot_t *agg_set_no;
  it_cursor_t itc_auto;
  it_cursor_t *itc = &itc_auto;
  index_tree_t *tree;
  caddr_t ***qis = (caddr_t ***) QST_GET_V (inst, ts->ts_aq_qis);
  int qi_inx;
  ITC_INIT (itc, NULL, NULL);
  if (!qis)
    return;
  if (HA_GROUP == fref->fnr_setp->setp_ha->ha_op && fref->fnr_setp->setp_set_no_in_key)
    n_sets = 1;
  qi->qi_set = 0;
  if (HA_GROUP == fref->fnr_setp->setp_ha->ha_op && enable_chash_gb)
    {
      if (vec_fref_chash_result (fref, ts, inst, n_sets))
	return;
    }
  DO_SET (setp_node_t *, setp, &fref->fnr_setps)
  {
    hash_area_t *ha = setp->setp_ha;
    if (HA_GROUP != ha->ha_op)
      continue;
    if (1 == n_sets && (tree = (index_tree_t *) (SSL_REF == ha->ha_tree->ssl_type
		|| SSL_VEC == ha->ha_tree->ssl_type ? sslr_qst_get (inst, (state_slot_ref_t *) ha->ha_tree, 0) : qst_get (inst,
		    ha->ha_tree))))
      {
	if (tree->it_hi && tree->it_hi->hi_chash)
	  chash_to_memcache (inst, tree, ha);
      }
  }
  END_DO_SET ();
  agg_set_no = fref_agg_set_no (fref);
  DO_BOX (caddr_t *, branch, qi_inx, qis)
  {
    if (!branch)
      continue;
    for (set = 0; set < n_sets; set++)
      {
	int set_in_sctr = qst_vec_get_int64 (inst, agg_set_no, set);
	((query_instance_t *) branch)->qi_set = set_in_sctr;
	fref_setp_flush (fref, branch);
	qi->qi_set = set_in_sctr;
	DO_SET (setp_node_t *, setp, &fref->fnr_setps)
	{
	  buffer_desc_t *buf;
	  hash_area_t *ha = setp->setp_ha;
	  index_tree_t *tree = (index_tree_t *) qst_get (branch, ha->ha_tree);
	  setp_node_t tmp_setp;
	  hash_area_t tmp_ha;
	  state_slot_t tmp_ssl[200];
	  if (setp->setp_top)
	    {
	      vec_top_merge (setp, fref, inst, branch, tmp_ssl, sizeof (tmp_ssl) / sizeof (state_slot_t));
	      continue;
	    }
	  if (!tree)
	    continue;
	  itc_from_it (itc, tree);
	  if (fref->src_gen.src_prev)
	    {
	      if (BOX_ELEMENTS (ha->ha_slots) > sizeof (tmp_ssl) / sizeof (state_slot_t))
		sqlr_new_error ("42000", "VEC..", "Too many order by or group by columns in parallel query branch merge");
	      vec_merge_setp (&setp, &ha, &tmp_setp, &tmp_ha, &tmp_ssl[0]);
	    }
	  ITC_FAIL (itc)
	  {
	    buf = itc_reset (itc);
	    while (DVC_MATCH == itc_next (itc, &buf))
	      {
		int col_inx;
		for (col_inx = 0; ha->ha_key_cols[col_inx].cl_col_id; col_inx++)
		  {
		    itc_qst_set_column (itc, buf, &ha->ha_key_cols[col_inx], inst, ha->ha_slots[col_inx]);
		  }
		if (setp->setp_ha->ha_op != HA_GROUP)
		  setp_order_row (setp, inst);
		else
		  setp_group_row (setp, inst);
	      }
	    itc_page_leave (itc, buf);
	  }
	  ITC_FAILED
	  {
	  }
	  END_FAIL (itc);
	  if (ha == &tmp_ha)
	    {
	      dk_free_box ((caddr_t) ha->ha_slots);
	      dk_free_box ((caddr_t) setp->setp_dependent_box);
	    }
	}
	END_DO_SET ();
      }
  }
  END_DO_BOX;
}


data_source_t *
fnr_skip_hash_fillers (fun_ref_node_t * fref)
{
  /* a fref can be preceded by hash fillers which do not count as producing sets and their result counts are therefore not copied when making qi branch copies.  So skip them when looking for a fref input set count */
  data_source_t *prev = fref->src_gen.src_prev;
  for (prev = prev; prev; prev = prev->src_prev)
    {
      if (!IS_QN (prev, hash_fill_node_input))
	return prev;
    }
  GPF_T1 ("it is anomalous to have a fref preceded by hash fillers only");
  return NULL;
}


void
ts_aq_result (table_source_t * ts, caddr_t * inst)
{
  /* a ts completed itts branches.  Add up the results.  Can be in a fref or a scalar/exists subq. */
  if (IS_QN (ts->ts_agg_node, fun_ref_node_input))
    {
      QNCAST (fun_ref_node_t, fref, ts->ts_agg_node);
      int n_sets = QST_INT (inst, fnr_skip_hash_fillers (fref)->src_out_fill);
      if (!n_sets)
	GPF_T1
	    ("A fref with 0 sets of input ought not to execute in the first place.  Probably missed copy of out fills in qi branchj copy");
      if (fref->fnr_default_ssls)
	{
	  vec_fref_single_result (fref, ts, inst, n_sets);
	}
      else
	{
	  setp_node_t *setp = fref->fnr_setp;
	  if ((!setp && fref->fnr_setps)
	      || (setp->setp_ha && (HA_ORDER == setp->setp_ha->ha_op || HA_GROUP == setp->setp_ha->ha_op)))
	    vec_fref_group_result (fref, ts, inst, n_sets);
	}
      qst_set (inst, ts->ts_aq_qis, NULL);
    }
  else
    ts_merge_subq_branches (ts, inst);
}


int
qi_fref_continuable (caddr_t * inst, dk_set_t nodes)
{
  DO_SET (table_source_t *, ts, &nodes)
  {
    if (IS_TS (ts) && ts->ts_aq)
      {
	int state = QST_INT (inst, ts->ts_aq_state);
	if (TS_AQ_COORD_AQ_WAIT == state)
	  return 0;
      }
    if (SRC_IN_STATE (ts, inst))
      return 1;
  }
  END_DO_SET ();
  return 0;
}


#define FST_INIT 0
#define FST_LOCAL 1
#define FST_LOCAL_DONE 2


void
fun_ref_streaming_input (fun_ref_node_t * fref, caddr_t * inst, caddr_t * state)
{
  int n_sets, req_no;
  table_source_t *ts = fref->fnr_stream_ts;
  QNCAST (query_instance_t, qi, inst);
  QN_N_SETS (fref, inst);
  n_sets = MAX (1, qi->qi_n_sets);
  if (fref->src_gen.src_out_fill)
    QST_INT (inst, fref->src_gen.src_out_fill) = n_sets;

  for (;;)
    {
      int fs_state = QST_INT (inst, fref->fnr_stream_state);
      if (state)
	{
	  if (ts)
	    {
	      qst_set (inst, ts->ts_aq, NULL);
	      qst_set (inst, ts->ts_aq_qis, NULL);
	    }
	  fs_state = QST_INT (inst, fref->fnr_stream_state) = FST_INIT;
	}
      else
	{
	  caddr_t err = NULL;
	  async_queue_t *aq = ts ? (async_queue_t *) qst_get (inst, ts->ts_aq) : NULL;
	  caddr_t *branch = QST_BOX (caddr_t *, inst, fref->fnr_current_branch);
	  if (aq)
	    {
	      if (branch != inst)
		{
		  if (qi_fref_continuable (branch, fref->fnr_select_nodes))
		    aq_request (aq, aq_qr_func, list (3, box_copy ((caddr_t) branch), box_num ((ptrlong) ts->src_gen.src_query),
			    box_num (qi->qi_trx->lt_rc_w_id ? qi->qi_trx->lt_rc_w_id : qi->qi_trx->lt_w_id)));
		  else
		    {
		      /*fnr_branch_done (fref, inst, branch) */ ;
		    }
		}
	      if (!aq->aq_requests->ht_count && FST_LOCAL_DONE == fs_state)
		{
		  qst_set (inst, ts->ts_aq_qis, NULL);
		  qst_set (inst, ts->ts_aq, NULL);
		  SRC_IN_STATE (fref, inst) = NULL;
		  return;
		}
	      branch = (caddr_t *) aq_wait_any (aq, &err, FST_LOCAL_DONE == fs_state, &req_no);
	      dk_free_box ((caddr_t) branch);	/* drop the ref count, to compensate for the copy in the aq_request */
	      if (AQR_RUNNING == (ptrlong) err)
		QST_BOX (caddr_t *, inst, fref->fnr_current_branch) = inst;
	      else if (err)
		sqlr_resignal (err);
	      else
		{
		  QST_BOX (caddr_t *, inst, fref->fnr_current_branch) = branch;
		  goto result;
		}
	    }
	  else if (FST_LOCAL_DONE == fs_state)
	    {
	      SRC_IN_STATE (fref, inst) = NULL;
	      return;
	    }
	}
      QR_RESET_CTX
      {
	if (FST_INIT == fs_state)
	  {
	    fs_state = QST_INT (inst, fref->fnr_stream_state) = FST_LOCAL;
	    qn_input (fref->fnr_select, inst, state);
	  }
	else
	  {
	    if (qi_fref_continuable (inst, fref->fnr_select_nodes))
	      cl_fref_resume (fref, inst);
	  }
	/* the continue returned, meaning locaal at end.  Set the branching ts so it will not try to sync since the fref does that */
	QST_INT (inst, fref->fnr_stream_state) = FST_LOCAL_DONE;
	QST_BOX (caddr_t *, inst, fref->fnr_current_branch) = inst;
	if (ts)
	  {
	    SRC_IN_STATE (ts, inst) = NULL;
	    QST_INT (inst, ts->ts_aq_state) = 0;
	  }
      }
      QR_RESET_CODE
      {
	POP_QR_RESET;
	QST_BOX (caddr_t *, inst, fref->fnr_current_branch) = inst;
	if (RST_GB_ENOUGH == reset_code)
	  goto result;
	longjmp_splice (THREAD_CURRENT_THREAD->thr_reset_ctx, RST_ERROR);
      }
      END_QR_RESET;

    result:
      SRC_IN_STATE (fref, inst) = inst;
      qn_send_output ((data_source_t *) fref, inst);
      state = NULL;
    }
}