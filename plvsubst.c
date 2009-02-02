/*
  This code implements one part of functonality of
  free available library PL/Vision. Please look www.quest.com

  Original author: Steven Feuerstein, 1996 - 2002
  PostgreSQL implementation author: Pavel Stehule, 2006

  This module is under BSD Licence

  History:
    1.0. first public version 22. September 2006

*/

#include "postgres.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "string.h"
#include "stdlib.h"
#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"
#include "lib/stringinfo.h"

#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "access/tupmacs.h"
#include "orafunc.h"

#include "plvstr.h"

Datum plvsubst_string_array(PG_FUNCTION_ARGS);
Datum plvsubst_string_string(PG_FUNCTION_ARGS);
Datum plvsubst_setsubst(PG_FUNCTION_ARGS);
Datum plvsubst_setsubst_default(PG_FUNCTION_ARGS);
Datum plvsubst_subst(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plvsubst_string_array);
PG_FUNCTION_INFO_V1(plvsubst_string_string);
PG_FUNCTION_INFO_V1(plvsubst_setsubst);
PG_FUNCTION_INFO_V1(plvsubst_setsubst_default);
PG_FUNCTION_INFO_V1(plvsubst_subst);

#define PARAMETER_ERROR(detail) \
	ereport(ERROR, \
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE), \
			 errmsg("invalid parameter"), \
			 errdetail(detail)));

#define C_SUBST  "%s"


text *c_subst = NULL;

static void
init_c_subst()
{
	if (!c_subst)
	{
		MemoryContext oldctx;

		oldctx = MemoryContextSwitchTo(TopMemoryContext);
		c_subst = CStringGetTextP(C_SUBST);
		MemoryContextSwitchTo(oldctx);
	}
}

static void
set_c_subst(text *sc)
{
	MemoryContext oldctx;

	if (c_subst)
		pfree(c_subst);

	oldctx = MemoryContextSwitchTo(TopMemoryContext);
	c_subst = sc ? TextPCopy(sc) : CStringGetTextP(C_SUBST);
	MemoryContextSwitchTo(oldctx);
}

static text*
plvsubst_string(text *template_in, ArrayType *vals_in, text *c_subst, FunctionCallInfo fcinfo)
{
	ArrayType	   *v = vals_in;
	int				nitems,
				   *dims,
					ndims;
	char		   *p;
	int16			typlen;
	bool			typbyval;
	char			typalign;
	char			typdelim;
	Oid				typelem;
	Oid				typiofunc;
	FmgrInfo		proc;
	int				i = 0, items = 0;
	StringInfo		sinfo;
	const char	   *template_str;
	int				template_len;
	char		   *sizes;
	int			   *positions;
	int				subst_mb_len;
	int				subst_len;
	const bits8	   *bitmap;
	int				bitmask;

	if (v != NULL)
	{
		ndims = ARR_NDIM(v);
		if (ndims != 1)
			PARAMETER_ERROR("Array of arguments has wrong dimension.");

		p = ARR_DATA_PTR(v);
		dims = ARR_DIMS(v);
		nitems = ArrayGetNItems(ndims, dims);
		bitmap = ARR_NULLBITMAP(v);
		get_type_io_data(ARR_ELEMTYPE(v), IOFunc_output,
						 &typlen, &typbyval,
						 &typalign, &typdelim,
						 &typelem, &typiofunc);
		fmgr_info_cxt(typiofunc, &proc, fcinfo->flinfo->fn_mcxt);
	}
	else
	{
		nitems = 0;
		p = NULL;
		bitmap = NULL;
	}

	template_str = VARDATA(template_in);
	template_len = ora_mb_strlen(template_in, &sizes, &positions);
	subst_mb_len = ora_mb_strlen1(c_subst);
	subst_len = VARSIZE_ANY_EXHDR(c_subst);
	sinfo = makeStringInfo();

	bitmask = 1;
	for (i = 0; i < template_len; i++)
	{
		if (strncmp(&template_str[positions[i]], VARDATA(c_subst), subst_len) == 0)
		{
			Datum    itemvalue;
			char     *value;

			if (items++ < nitems)
			{
				if (bitmap && (*bitmap & bitmask) == 0)
					value = pstrdup("NULL");
				else
				{
					itemvalue = fetch_att(p, typbyval, typlen);
					value = DatumGetCString(FunctionCall3(&proc,
								itemvalue,
								ObjectIdGetDatum(typelem),
								Int32GetDatum(-1)));

					p = att_addlength_pointer(p, typlen, p);
					p = (char *) att_align_nominal(p, typalign);
				}
				appendStringInfoString(sinfo, value);
				pfree(value);

				if (bitmap)
				{
					bitmask <<= 1;
					if (bitmask == 0x100)
					{
						bitmap++;
						bitmask = 1;
					}
				}
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("too few parameters specified for template string")));

			i += subst_mb_len - 1;
		}
		else
			appendBinaryStringInfo(sinfo, &template_str[positions[i]], sizes[i]);
	}

	return CStringGetTextP(sinfo->data);
}


Datum
plvsubst_string_array(PG_FUNCTION_ARGS)
{
	init_c_subst();

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(plvsubst_string(PG_GETARG_TEXT_P(0),
					 PG_GETARG_ARRAYTYPE_P(1),
					 PG_ARGISNULL(2) ? c_subst : PG_GETARG_TEXT_P(2),
					 fcinfo));
}

Datum
plvsubst_string_string(PG_FUNCTION_ARGS)
{
	Datum		r;
	ArrayType  *array;
	FunctionCallInfoData locfcinfo;

	init_c_subst();

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	/*
	 * I can't use DirectFunctionCall2
	 */

	InitFunctionCallInfoData(locfcinfo, fcinfo->flinfo, 2, NULL, NULL);
	locfcinfo.arg[0] = PG_GETARG_DATUM(1);
	locfcinfo.arg[1] = (PG_NARGS() <= 2 || PG_ARGISNULL(2)) ? CStringGetTextDatum(",") : PG_GETARG_DATUM(2);
	locfcinfo.argnull[0] = false;
	locfcinfo.argnull[1] = false;
	r = text_to_array(&locfcinfo);

	if (locfcinfo.isnull || r == (Datum) 0)
		array = NULL;
	else
		array = DatumGetArrayTypeP(r);

	PG_RETURN_TEXT_P(plvsubst_string(PG_GETARG_TEXT_P(0),
					 array,
					 (PG_NARGS() <= 3 || PG_ARGISNULL(3)) ? c_subst : PG_GETARG_TEXT_P(3),
					 fcinfo));
}

Datum
plvsubst_setsubst(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("substition is NULL"),
				 errdetail("Substitution keyword may not be NULL.")));

	set_c_subst(PG_GETARG_TEXT_P(0));
	PG_RETURN_VOID();
}

Datum
plvsubst_setsubst_default(PG_FUNCTION_ARGS)
{
	set_c_subst(NULL);
	PG_RETURN_VOID();
}


Datum
plvsubst_subst(PG_FUNCTION_ARGS)
{
	init_c_subst();
	PG_RETURN_TEXT_P(TextPCopy(c_subst));
}
