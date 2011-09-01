/*
 * The contents of this file are subject to the MonetDB Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.monetdb.org/Legal/MonetDBLicense
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the MonetDB Database System.
 *
 * The Initial Developer of the Original Code is CWI.
 * Portions created by CWI are Copyright (C) 1997-July 2008 CWI.
 * Copyright August 2008-2011 MonetDB B.V.
 * All Rights Reserved.
 */

/*
 * @f sql_gencode
 * @t SQL to MAL code generation.
 * @a N. Nes, M. Kersten
 * @+ MAL Code generation
 * This module contains the actions to construct a MAL program, ready for
 * optimization and execution by the Monet V5 kernel.
 *
 * The code base is modeled directly after its MIL variant, replacing
 * each IO request by instructions to initialize the corresponding MAL data
 * structure.
 * To speed up the compilation, we may consider keeping a cache of pre-compiled
 * statements.
 *
 * MAL extensions needed. A temporary variable used as an argument
 * should be printed (done). Consider replacing modname/fcnname by
 * an integer constant and a global lookup table. This should
 * reduce the cost to prepare MAL statements significantly.
 *
 * A dummy module is needed to load properly.
 * @-
 */
#include "monetdb_config.h"
#include "sql_gencode.h"
#include "sql_optimizer.h"
#include "sql_scenario.h"
#include "mal_namespace.h"
#include "opt_prelude.h"
#include "mal_builder.h"

#include <sql_rel2bin.h>
#include <rel_optimizer.h>
#include <rel_subquery.h>
#include <rel_bin.h>

static int _dumpstmt(backend *sql, MalBlkPtr mb, stmt *s);

/*
 * @+ MAL code support
 * To simplify construction of the MAL program use the following
 * macros
 *
 * @+ MAL initialization
 * Many instructions have a more or less fixed structure, therefore
 * they can be assembled in a pre-compiled block. Each time we need it,
 * a copy can be extracted and included in the MAL block
 *
 * The catalog relations should be maintained in a MAL box, which
 * provides the handle for transaction management.
 * @-
 * The atoms produced by the parser should be converted back into
 * MAL constants. Ideally, this should not be necessary when the
 * SQL parser keeps the string representation around.
 * This involves regeneration of their string as well and
 * trimming the enclosing string quotes.
 */
static int
constantAtom(backend *sql, MalBlkPtr mb, atom *a)
{
	int idx;
	ValPtr vr = (ValPtr) &a->data;
	ValRecord cst;

	(void) sql;
	cst.vtype = 0;
	VALcopy(&cst,vr);
	idx = defConstant(mb, vr->vtype, &cst);
	return idx;
}

static int
argumentZero(MalBlkPtr mb, int tpe)
{
	ValRecord cst;

	cst.vtype =TYPE_int;
	cst.val.ival= 0;
	convertConstant(tpe, &cst);
	return defConstant(mb,tpe,&cst);
}

/*
 * @-
 * To speedup code generation we freeze the references to the major modules.
 * This safes table lookups.
 */
static str exportValueRef;
static str exportResultRef;

void initSQLreferences(void){
	optimizerInit();
	if ( exportValueRef == NULL ) {
		exportValueRef = putName("exportValue",11);
		exportResultRef= putName("exportResult",12);
	}
	if( algebraRef==NULL || exportValueRef==NULL || exportResultRef==NULL )
			GDKfatal("error initSQLreferences");
}

/*
 * @-
 * The dump_header produces a sequence of instructions for
 * the front-end to prepare presentation of a result table.
 */
static void
dump_header(mvc *sql, MalBlkPtr mb, stmt *s, list *l)
{
	node *n;
	InstrPtr q;

	for (n = l->h; n; n = n->next) {
		stmt *c = n->data;
		sql_subtype *t = tail_type(c);
		char *tname = table_name(sql->sa, c);
		char *sname = schema_name(sql->sa, c);
		char *_empty = "";
		char *tn = (tname) ? tname : _empty;
		char *sn = (sname) ? sname : _empty;
		char *cn = column_name(sql->sa, c);
		char *ntn = sql_escape_ident(tn);
		char *nsn = sql_escape_ident(sn);
		size_t fqtnl = strlen(ntn) + 1 + strlen(nsn) + 1;
		char *fqtn = NEW_ARRAY(char, fqtnl);

		snprintf(fqtn, fqtnl, "%s.%s", nsn, ntn);

		q = newStmt1(mb, sqlRef, "rsColumn");
		q = pushArgument(mb, q, s->nr);
		q = pushStr(mb, q, fqtn);
		q = pushStr(mb, q, cn);
		q = pushStr(mb, q, t->type->sqlname);
		q = pushInt(mb, q, t->digits);
		q = pushInt(mb, q, t->scale);
		(void) pushArgument(mb, q, c->nr);
		_DELETE(ntn);
		_DELETE(nsn);
		_DELETE(fqtn);
	}
}

static int
dump_table(MalBlkPtr mb, sql_table *t)
{
	int nr;
	node *n;
	InstrPtr k = newStmt1(mb, sqlRef, "declaredTable");

	nr = getDestVar(k);
	(void) pushStr(mb, k, t->base.name );
	for (n = t->columns.set->h; n; n = n->next) {
		sql_column *c = n->data;
		char *tname = c->t->base.name;
		char *tn = sql_escape_ident(tname);
		char *cn = c->base.name;
		InstrPtr q = newStmt1(mb, sqlRef, "dtColumn");

		q = pushArgument(mb, q, nr);
		q = pushStr(mb, q, tn);
		q = pushStr(mb, q, cn);
		q = pushStr(mb, q, c->type.type->sqlname);
		q = pushInt(mb, q, c->type.digits);
		(void) pushInt(mb, q, c->type.scale);
		_DELETE(tn);
	}
	return nr;
}

static int
drop_table(MalBlkPtr mb, str n)
{
	InstrPtr k = newStmt1(mb, sqlRef, "dropDeclaredTable");
	int nr = getDestVar(k);

	(void) pushStr(mb, k, n);
	return nr;
}

static InstrPtr
dump_cols(MalBlkPtr mb, list *l, InstrPtr q)
{
	int i;
	node *n;

	q->retc=q->argc=0;
	for (i = 0, n = l->h; n; n = n->next, i++) {
		stmt *c = n->data;

		q = pushArgument(mb, q, c->nr);
	}
	q->retc = q->argc;
	return q;
}

static InstrPtr
table_func_create_result( MalBlkPtr mb, InstrPtr q, sql_table *f)
{
	node *n;
	int i;

	for (i = 0, n = f->columns.set->h; n; n = n->next, i++ ) {
		sql_column *c = n->data;
		int type = c->type.type->localtype;

		type = newBatType(TYPE_oid,type);
		if (i)
			q = pushReturn(mb, q, newTmpVariable(mb, type));
		else
			setVarType(mb,getArg(q,0), type);
	}
	return q;
}

/*
 * @-
 * Some utility routines to generate code
 * The equality operator in MAL is '==' instead of '='.
 */
static str
convertMultiplexMod(str mod, str op)
{
	if (strcmp(op, "=") == 0)
		return "calc";
	return mod;
}
static str
convertMultiplexFcn(str op)
{
	if (strcmp(op, "=") == 0)
		return "==";
	return op;
}

static str
convertOperator(str op)
{
	if (strcmp(op, "=") == 0)
		return "==";
	return op;
}

static int
range_join_convertable(stmt *s, stmt **base, stmt **L, stmt **H)
{
	int ls = 0, hs = 0;
	stmt *l = NULL, *h = NULL;
	stmt *bl = s->op2, *bh = s->op3;
	int tt = tail_type(s->op2)->type->localtype;

	if (tt > TYPE_lng)
		return 0;
	if (s->op2->type == st_binop) {
		bl = s->op2->op1;
		l  = s->op2->op2;
	} else if (s->op2->type == st_Nop &&
	    list_length(s->op2->op1->op4.lval) == 2) {
		bl = s->op2->op1->op4.lval->h->data;
		l  = s->op2->op1->op4.lval->t->data;
	}
	if (s->op3->type == st_binop) {
		bh = s->op3->op1;
		h  = s->op3->op2;
	}
	else if (s->op3->type == st_Nop &&
	    list_length(s->op3->op1->op4.lval) == 2) {
		bh = s->op3->op1->op4.lval->h->data;
		h  = s->op3->op1->op4.lval->t->data;
	}

	if ((ls = (l &&
	    strcmp(s->op2->op4.funcval->func->base.name, "sql_sub")==0 &&
	    l->nrcols == 0) ||

	    (hs = (h &&
	    strcmp(s->op3->op4.funcval->func->base.name, "sql_add")==0 &&
	    h->nrcols == 0))) && (ls || hs) && bl == bh) {
		*base = bl;
		*L = l;
		*H = h;
		return 1;
	}
	return 0;
}

static int
_dump_1(MalBlkPtr mb, char *mod, char *name, int o1)
{
	InstrPtr q;

	q = newStmt2(mb, mod, name);
	q = pushArgument(mb, q, o1);
	return getDestVar(q);
}

static void
dump_1(backend *sql, MalBlkPtr mb, stmt *s, char *mod, char *name)
{
	int o1 = _dumpstmt(sql, mb, s->op1);

	s->nr = _dump_1(mb, mod, name, o1);
}

static int
_dump_2( MalBlkPtr mb, char *mod, char *name, int o1, int o2)
{
	InstrPtr q;

	q = newStmt2(mb, mod, name);
	q = pushArgument(mb, q, o1);
	q = pushArgument(mb, q, o2);
	return getDestVar(q);
}

static void
dump_2(backend *sql, MalBlkPtr mb, stmt *s, char *mod, char *name)
{
	int o1 = _dumpstmt(sql, mb, s->op1);
	int o2 = _dumpstmt(sql, mb, s->op2);

	s->nr = _dump_2(mb, mod, name, o1, o2);
}

static InstrPtr
multiplex2(MalBlkPtr mb, char *mod, char *name /* should be eaten */, int o1, int o2, int rtype)
{
	InstrPtr q;

	q = newStmt(mb, "mal","multiplex");
	setVarType(mb,getArg(q,0), newBatType(TYPE_oid,rtype));
	setVarUDFtype(mb,getArg(q,0));
	q = pushStr(mb, q, convertMultiplexMod(mod,name));
	q = pushStr(mb, q, convertMultiplexFcn(name));
	q = pushArgument(mb, q, o1);
	q = pushArgument(mb, q, o2);
	return q;
}

static InstrPtr
dump_crossproduct(MalBlkPtr mb, int l, int r)
{
	int z;
	InstrPtr q;

	q = newStmt1(mb, calcRef, "int");
	q = pushInt(mb, q, 0);
	z = getDestVar(q);

	q = newStmt2(mb, algebraRef, projectRef);
	q = pushArgument(mb, q, r);
	q = pushArgument(mb, q, z);
	r = getDestVar(q);

	q = newStmt2(mb, batRef, reverseRef);
	q = pushArgument(mb, q, r);
	r = getDestVar(q);

	q = newStmt2(mb, algebraRef, projectRef);
	q = pushArgument(mb, q, l);
	q = pushArgument(mb, q, z);
	l = getDestVar(q);

	q = newStmt2(mb, algebraRef, joinRef);
	q = pushArgument(mb, q, l);
	q = pushArgument(mb, q, r);
	return q;
}

static InstrPtr
multiplexN(MalBlkPtr mb, char *mod, char *name)
{
	InstrPtr q = NULL;

	if (strcmp(name,"rotate_xor_hash") == 0 && strcmp(mod, "calc") == 0)
		q = newStmt(mb, "mkey", "bulk_rotate_xor_hash");
	return q;
}

static int
dump_joinN(backend *sql, MalBlkPtr mb, stmt *s)
{
	char *mod = sql_func_mod(s->op4.funcval->func);
	char *fimp = sql_func_imp(s->op4.funcval->func);
	node *n;
	InstrPtr q;
	int l, r, k;
	int need_not = (s->flag & ANTI);

	/* dump left and right operands */
	(void)_dumpstmt(sql, mb, s->op1);
	(void)_dumpstmt(sql, mb, s->op2);

	/* find left and right columns (need more work) */
	l = ((stmt*)s->op1->op4.lval->h->data)->nr;
	r = ((stmt*)s->op2->op4.lval->h->data)->nr;

	q = dump_crossproduct(mb, l, r);
	k = getDestVar(q);

	/* split */
	q = newStmt2(mb, algebraRef, markHRef);
	q = pushArgument(mb, q, k);
	q = pushOid(mb, q, 0);
	r = getDestVar(q);

	q = newStmt2(mb, algebraRef, markTRef);
	q = pushArgument(mb, q, k);
	q = pushOid(mb, q, 0);
	l = getDestVar(q);

	l = _dump_1(mb, batRef, reverseRef, l );

	/* join left columns */
	for (n = s->op1->op4.lval->h; n; n = n->next) {
		stmt *op = n->data;

		if (op->nrcols)
			op->nr = _dump_2(mb, algebraRef, joinRef, l, op->nr);
	}
	/* join right columns */
	for (n = s->op2->op4.lval->h; n; n = n->next) {
		stmt *op = n->data;

		if (op->nrcols)
			op->nr = _dump_2(mb, algebraRef, joinRef, r, op->nr);
	}

	/* execute multiplexed function */
	q = newStmt(mb, "mal","multiplex");
	setVarType(mb,getArg(q,0), newBatType(TYPE_oid, TYPE_bit));
	setVarUDFtype(mb,getArg(q,0));
	q = pushStr(mb, q, mod);
	q = pushStr(mb, q, fimp);
	for (n = s->op1->op4.lval->h; n; n = n->next) {
		stmt *op = n->data;
		q = pushArgument(mb, q, op->nr);
	}
	for (n = s->op2->op4.lval->h; n; n = n->next) {
		stmt *op = n->data;
		q = pushArgument(mb, q, op->nr);
	}
	k = getDestVar(q);

	/* filter qualifying tuples, return oids of h and tail */
	q = newStmt2(mb, algebraRef, uselectRef);
	q = pushArgument(mb, q, k);
	q = pushBit(mb, q, !need_not);
	k = getDestVar(q);

	k = _dump_1(mb, batRef, mirrorRef, k);
	k = _dump_2(mb, algebraRef, joinRef, k, l);
	k = _dump_1(mb, batRef, reverseRef, k);
	k = _dump_2(mb, algebraRef, joinRef, k, r);
	return k;
}

static InstrPtr
pushSchema(MalBlkPtr mb, InstrPtr q, sql_table *t)
{
	if (t->s)
		return pushStr(mb, q, t->s->base.name);
	else
		return pushNil(mb, q, TYPE_str);
}

static char*
reconnect(MalBlkPtr mb, list *l)
{
	InstrPtr q = NULL;
	char *db_alias = NULL;

	node* n = l->h;
	int i = 0;

	n = n->next;

	/*create the mserver reconnect*/
	q = newStmt1(mb,mapiRef,reconnectRef);
	setVarUDFtype(mb,getArg(q,0));
	setVarType(mb, getArg(q, 0), TYPE_int);

	while (n) {
		if (i == 1)
			q = pushInt(mb, q, *(int *) n->data);
		else {
			if (i == 3) {
				db_alias = (char *) n->data;
				q = pushStr(mb, q, (char *) n->data);
			}
			else
				if(i != 2)
					q = pushStr(mb, q, (char *) n->data);
		}
		n = n->next;
		i++;
	}

	return db_alias;
}


static void
disconnect(MalBlkPtr mb, list *l)
{
	InstrPtr q = NULL;

	node* n = l->h;


	/*create the mserver disconnect*/
	q = newStmt1(mb,mapiRef,disconnectRef);
	setVarUDFtype(mb,getArg(q,0));
	setVarType(mb, getArg(q, 0), TYPE_int);

	if (list_length(l) == 2) {
		(void) pushStr(mb, q, (char *) n->next->data);
	}

}

/*
 * @-
 * The big code generation switch.
 */
#define SMALLBUFSIZ 64
static int
_dumpstmt(backend *sql, MalBlkPtr mb, stmt *s)
{
	InstrPtr q = NULL;
	node *n;

 	if (THRhighwater()) {
		if (sql->client->exception_buf_initialized)
			longjmp(sql->client->exception_buf, -1);
		showException(SQL, "sql", "too many nested operators");
		assert(0);
	}

	if (s) {
		if (s->nr > 0)
			return s->nr;	/* stmt already handled */

		switch (s->type) {
		case st_none: {
			q = newAssignment(mb);
			s->nr = getDestVar(q);
			(void) pushInt(mb, q, 1);
		}	break;
		case st_connection: {
			if (list_length(s->op4.lval) < 3)
				disconnect(mb, s->op4.lval);
			else
				reconnect(mb, s->op4.lval);
			s->nr = 1;
		}	break;
		case st_var:{
			if (s->op1) {
				if (VAR_GLOBAL(s->flag)) { /* globals */
					int tt = tail_type(s)->type->localtype;

					q = newStmt1(mb, sqlRef, "getVariable");
					q = pushArgument(mb, q, sql->mvc_var);
					q = pushStr(mb, q, s->op1->op4.aval->data.val.sval);
					setVarType(mb, getArg(q, 0), tt);
					setVarUDFtype(mb,getArg(q,0));
				} else if ((s->flag & VAR_DECLARE) == 0) {
					char *buf = GDKmalloc(MAXIDENTLEN);

					(void) snprintf(buf, MAXIDENTLEN, "A%s", s->op1->op4.aval->data.val.sval);
					q = newAssignment(mb);
					q = pushArgumentId(mb, q, buf);
				} else {
					int tt = tail_type(s)->type->localtype;
					char *buf = GDKmalloc(MAXIDENTLEN);

					if (tt == TYPE_bat) {
						/* declared table */
						s->nr = dump_table(mb, tail_type(s)->comp_type);
						break;
					}
					(void) snprintf(buf, MAXIDENTLEN, "A%s", s->op1->op4.aval->data.val.sval);
					q = newInstruction(mb,ASSIGNsymbol);
					q->argc = q->retc = 0;
					q = pushArgumentId(mb, q, buf);
					q = pushNil(mb, q, tt);
					pushInstruction(mb, q);
					q->retc++;
				}
			} else {
				char *buf = GDKmalloc(SMALLBUFSIZ);

				(void) snprintf(buf, SMALLBUFSIZ, "A%d", s->flag);
				q = newAssignment(mb);
				q = pushArgumentId(mb, q, buf);
			}
			s->nr = getDestVar(q);
		} break;
		case st_single: {
			int ht = TYPE_oid;
			int tt = s->op4.typeval.type->localtype;
			int val = _dumpstmt(sql, mb, s->op1);
/*
			int temp, val = _dumpstmt(sql, mb, s->op1);

			q = newStmt1(mb, batRef, "new");
			setVarType(mb, getArg(q, 0), newBatType(ht, tt));
			setVarUDFtype(mb,getArg(q,0));
			q = pushType(mb, q, ht);
			q = pushType(mb, q, tt);

			temp = getDestVar(q);
			q = newStmt2(mb, batRef, appendRef);
			q = pushArgument(mb, q, temp);
			q = pushArgument(mb, q, val);
			q = pushBit(mb, q, TRUE);
			s->nr = getDestVar(q);
*/
			q = newStmt1(mb, sqlRef, "single");
			setVarType(mb, getArg(q, 0), newBatType(ht, tt));
			q = pushArgument(mb, q, val);
			s->nr = getDestVar(q);
		} break;
		case st_temp:{
			int ht = TYPE_oid;
			int tt = s->op4.typeval.type->localtype;

			q = newStmt1(mb, batRef, "new");
			setVarType(mb, getArg(q, 0), newBatType(ht, tt));
			setVarUDFtype(mb,getArg(q,0));
			q = pushType(mb, q, ht);
			q = pushType(mb, q, tt);

			s->nr = getDestVar(q);
		} break;
		case st_bat: {
			int ht = TYPE_oid;
			int tt = s->op4.cval->type.type->localtype;
			sql_table *t = s->op4.cval->t;
			str mod = isRemote(t)?remoteRef:sqlRef;

			q = newStmt2(mb, mod, bindRef);
			setVarType(mb, getArg(q, 0), newBatType(ht, tt));
			setVarUDFtype(mb,getArg(q,0));
			if (isRemote(t))
				q = pushStr(mb, q, t->query);
			else
				q = pushArgument(mb, q, sql->mvc_var);
			q = pushSchema(mb, q, t);
			q = pushStr(mb, q, t->base.name);
			q = pushStr(mb, q, s->op4.cval->base.name);
			q = pushInt(mb, q, s->flag);
			/* dummy version */
			if (isRemote(t))
				q = pushInt(mb, q, s->flag);
			s->nr = getDestVar(q);
		}
			break;
		case st_dbat:{
			int ht = TYPE_oid;
			sql_table *t = s->op4.tval;
			str mod = isRemote(t)?remoteRef:sqlRef;

			q = newStmt2(mb, mod, binddbatRef);
			setVarType(mb, getArg(q,0), newBatType(ht,TYPE_oid));
			setVarUDFtype(mb,getArg(q,0));
			if (isRemote(t))
				q = pushStr(mb, q, t->query);
			else
				q = pushArgument(mb, q, sql->mvc_var);
			q = pushSchema(mb, q, t);
			q = pushStr(mb, q, t->base.name);
			q = pushInt(mb, q, s->flag);
			/* dummy version */
			if (isRemote(t))
				q = pushInt(mb, q, s->flag);
			s->nr = getDestVar(q);
		}
			break;
		case st_idxbat:{
			int tt;
			int ht = TYPE_oid;
			sql_table *t = s->op4.idxval->t;
			str mod = isRemote(t)?remoteRef:sqlRef;

			q = newStmt2(mb, mod, bindidxRef);
			tt = tail_type(s)->type->localtype;
			setVarType(mb, getArg(q, 0), newBatType(ht, tt));
			setVarUDFtype(mb,getArg(q,0));
			if (isRemote(t))
				q = pushStr(mb, q, t->query);
			else
				q = pushArgument(mb, q, sql->mvc_var);
			q = pushSchema(mb, q, t);
			q = pushStr(mb, q, t->base.name);
			q = pushStr(mb, q, s->op4.idxval->base.name);
			q = pushInt(mb, q, s->flag);
			/* dummy version */
			if (isRemote(t))
				q = pushInt(mb, q, s->flag);
			s->nr = getDestVar(q);
		}
			break;
		case st_const:{
			if (s->op2)
				dump_2(sql, mb, s, algebraRef, projectRef);
			else
				dump_1(sql, mb, s, algebraRef, projectRef);
		}
			break;
		case st_mark:{
			dump_2(sql, mb, s, algebraRef, markTRef);
		}
			break;
		case st_gen_group:{
			dump_1(sql, mb, s, algebraRef, groupbyRef);
		}
			break;
		case st_reverse:{
			dump_1(sql, mb, s, batRef, reverseRef);
		}
			break;
		case st_mirror:{
			dump_1(sql, mb, s, batRef, mirrorRef);
		}
			break;
		case st_limit2:
		case st_limit:{
			int l = _dumpstmt(sql, mb, s->op1);
			stmt *l1 = (s->type == st_limit2)?s->op1->op4.lval->h->data:s->op1;
			stmt *l2 = (s->type == st_limit2)?s->op1->op4.lval->t->data:NULL;
			int offset = _dumpstmt(sql, mb, s->op2);
			int len = _dumpstmt(sql, mb, s->op3);
			int la = (l2)?l2->nr:0;

			l = l1->nr;
			/* first insert single value into a bat */
			assert(s->nrcols);
			if (s->nrcols == 0) {
				int k;
				int ht = TYPE_oid;
				int tt = tail_type(s->op1)->type->localtype;

				q = newStmt1(mb, batRef, "new");
				setVarType(mb, getArg(q, 0), newBatType(ht,tt));
				setVarUDFtype(mb,getArg(q,0));
				q = pushType(mb, q, ht);
				q = pushType(mb, q, tt);
				k = getDestVar(q);

				q = newStmt2(mb, batRef, appendRef);
				q = pushArgument(mb, q, k);
				(void) pushArgument(mb, q, l);
				l = k;
			}
			if (s->flag) {
				int topn = 0, flag = s->flag, utopn = flag&2;
				char *name = "utopn_min";

				flag >>= 2;
				if (flag)
					name = "utopn_max";

				if (!utopn)
					name = name+1;
				q = newStmt1(mb, calcRef, "+");
				q = pushArgument(mb,q, offset);
				q = pushArgument(mb,q, len);
				topn = getDestVar(q);

				q = newStmt(mb, "pqueue", name);
				if (la)
					q = pushArgument(mb, q, la);
				q = pushArgument(mb,q, l);
				q = pushArgument(mb, q, topn);
				l = getDestVar(q);

				/* since both arguments of algebra.slice are
				   inclusive correct the LIMIT value by
				   substracting 1 */
				if (s->op2->op4.aval->data.val.wval) {
					q = newStmt1(mb, calcRef, "-");
					q = pushArgument(mb, q, topn);
					q = pushInt(mb, q, 1);
					len = getDestVar(q);

					q = newStmt1(mb, algebraRef, "slice");
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, offset);
					q = pushArgument(mb, q, len);
					l = getDestVar(q);
				}
			} else {
				q = newStmt1(mb, calcRef, "+");
				q = pushArgument(mb, q, offset);
				q = pushArgument(mb, q, len);
				len = getDestVar(q);

				/* since both arguments of algebra.slice are
				   inclusive correct the LIMIT value by
				   substracting 1 */
				q = newStmt1(mb, calcRef, "-");
				q = pushArgument(mb, q, len);
				q = pushInt(mb, q, 1);
				len = getDestVar(q);

				q = newStmt1(mb, algebraRef, "slice");
				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, offset);
				q = pushArgument(mb, q, len);
				l = getDestVar(q);
			}
			/* retrieve the single values again */
			if (s->nrcols == 0) {
				q = newStmt1(mb, algebraRef, "find");
				q = pushArgument(mb, q, l);
				q = pushOid(mb,q,0);
				l = getDestVar(q);
			}
			s->nr = l;
		} break;
		case st_sample:{
			int l = _dumpstmt(sql, mb, s->op1);
			int r = _dumpstmt(sql, mb, s->op2);
			q = newStmt(mb, "sample", "uniform");
			q = pushArgument(mb, q, l);
			q = pushArgument(mb, q, r);
			s->nr = getDestVar(q);
		} break;
		case st_order:{
			int l = _dumpstmt(sql, mb, s->op1);

			if( s->flag > 0){
				q = newStmt2(mb, algebraRef, sortTailRef);
			} else {
				q = newStmt2(mb, algebraRef, sortReverseTailRef);
			}
			q = pushArgument(mb, q, l);
			s->nr = getDestVar(q);
		} break;
		case st_reorder:{
			int l = _dumpstmt(sql, mb, s->op1);
			int r = _dumpstmt(sql, mb, s->op2);

			if (s->flag) {
				q = newStmt2(mb, groupRef, refineRef);
			} else {
				q = newStmt2(mb, groupRef, refine_reverseRef);
			}
			q = pushArgument(mb, q, l);
			q = pushArgument(mb, q, r);
			s->nr = getDestVar(q);
		}
			break;

		case st_uselect:
		case st_select: {
			bit need_not = FALSE;
			int l = _dumpstmt(sql, mb, s->op1);
			int r = _dumpstmt(sql, mb, s->op2);

			if (s->op2->nrcols >= 1) {
				char *mod = calcRef;
				char *op = "=";
				int k;
				int j,hml,tmr,mhj,mtj;

				switch (s->flag) {
				case cmp_equal:
					op = "=";
					break;
				case cmp_notequal:
					op = "!=";
					break;
				case cmp_lt:
					op = "<";
					break;
				case cmp_lte:
					op = "<=";
					break;
				case cmp_gt:
					op = ">";
					break;
				case cmp_gte:
					op = ">=";
					break;
				case cmp_like:
					op = "like";
					mod = strRef;
					break;
				case cmp_ilike:
					op = "ilike";
					mod = strRef;
					break;
				case cmp_notlike:
					need_not = TRUE;
					op = "like";
					mod = strRef;
					break;
				case cmp_notilike:
					need_not = TRUE;
					op = "ilike";
					mod = strRef;
					break;
				default:
					showException(SQL,"sql","Unknown operator");
				}

				/* select on join */
				q = newStmt2(mb, batRef, mirrorRef );
				q = pushArgument(mb, q, l);
				hml = getDestVar(q);

				q = newStmt2(mb, batRef, mirrorRef );
				q = pushArgument(mb, q, r);
				tmr = getDestVar(q);

				q = newStmt2(mb, algebraRef, joinRef);
				q = pushArgument(mb, q, hml);
				q = pushArgument(mb, q, tmr);
				j = getDestVar(q);

				q = newStmt2(mb, algebraRef, markHRef);
				q = pushArgument(mb, q, j);
				q = pushOid(mb, q, 0);
				mhj = getDestVar(q);

				q = newStmt2(mb, algebraRef, markTRef);
				q = pushArgument(mb, q, j);
				q = pushOid(mb, q, 0);
				mtj = getDestVar(q);

				q = newStmt2(mb, batRef, reverseRef );
				q = pushArgument(mb, q, mtj);
				mtj = getDestVar(q);

				q = newStmt2(mb, algebraRef, joinRef);
				q = pushArgument(mb, q, mtj);
				q = pushArgument(mb, q, l);
				l = getDestVar(q);

				q = newStmt2(mb, algebraRef, joinRef);
				q = pushArgument(mb, q, mhj);
				q = pushArgument(mb, q, r);
				r = getDestVar(q);

				q = multiplex2(mb,mod,convertOperator(op),l,r,TYPE_bit);
				k = getDestVar(q);

				q = newStmt2(mb, algebraRef, uselectRef);
				q = pushArgument(mb, q, k);
				q = pushBit(mb, q, !need_not);
				k = getDestVar(q);

				q = newStmt2(mb, batRef, reverseRef );
				q = pushArgument(mb, q, k);
				k = getDestVar(q);

				q = newStmt2(mb, algebraRef, joinRef);
				q = pushArgument(mb, q, k);
				q = pushArgument(mb, q, mhj);
				k = getDestVar(q);

				q = newStmt2(mb, batRef, reverseRef );
				q = pushArgument(mb, q, k);
				s->nr = getDestVar(q);
/*
				assert(s->type != st_select);
				q = multiplex2(mb,mod,convertOperator(op),l,r,TYPE_bit);
				k = getDestVar(q);

				q = newStmt2(mb, algebraRef, uselectRef);
				q = pushArgument(mb, q, k);
				q = pushBit(mb, q, !need_not);
				s->nr = getDestVar(q);
*/
			} else {
				char *cmd = s->type == st_select ?
						"select" : "uselect";

				if (s->flag != cmp_equal)
					cmd = s->type == st_select ?
						"thetaselect" : "thetauselect";

				switch (s->flag) {
				case cmp_like:
				case cmp_ilike:
				{
					int e = _dumpstmt(sql, mb, s->op3);
					q = newStmt1(mb, pcreRef,
							(s->flag == cmp_like ? "like_uselect" : "ilike_uselect"));
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, r);
					q = pushArgument(mb, q, e);
					break;
				}
				case cmp_notlike:
				case cmp_notilike:
				{
					int e = _dumpstmt(sql, mb, s->op3);
					int k;

					q = newStmt1(mb, pcreRef,
							(s->flag == cmp_notlike ? "like_uselect" : "ilike_uselect"));
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, r);
					q = pushArgument(mb, q, e);
					k = getDestVar(q);

					q = newStmt2(mb, algebraRef, projectRef);
					q = pushArgument(mb, q, l);
					q = pushNil(mb, q, TYPE_void);
					l = getDestVar(q);
					q = newStmt2(mb, algebraRef, kdifferenceRef);
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, k);
					break;
				}
				case cmp_equal:{
					q = newStmt1(mb, algebraRef, cmd);
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, r);
					break;
				}
				case cmp_notequal:{
					q = newStmt1(mb, algebraRef, "antiuselect");
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, r);
					break;
				}
				case cmp_lt:
					q = newStmt1(mb, algebraRef, cmd);
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, r);
					q = pushStr(mb, q, "<");
					break;
				case cmp_lte:
					q = newStmt1(mb, algebraRef, cmd);
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, r);
					q = pushStr(mb, q, "<=");
					break;
				case cmp_gt:
					q = newStmt1(mb, algebraRef, cmd);
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, r);
					q = pushStr(mb, q, ">");
					break;
				case cmp_gte:
					q = newStmt1(mb, algebraRef, cmd);
					q = pushArgument(mb, q, l);
					q = pushArgument(mb, q, r);
					q = pushStr(mb, q, ">=");
					break;
				default:
					showException(SQL,"sql","SQL2MAL: error impossible\n");
				}
			}
			if ( q )
				s->nr = getDestVar(q);
			else s->nr = newTmpVariable(mb, TYPE_any);
		}
			break;
		case st_uselect2:
		case st_select2:
		case st_join2:{
			InstrPtr r,p;
			int l = _dumpstmt(sql, mb, s->op1);
			stmt *base, *low = NULL, *high = NULL;
			int r1 = -1, r2 = -1, rs = 0, k;

			char *cmd =
				(s->type == st_select2) ? selectRef :
				(s->type == st_uselect2) ?
				(s->flag&ANTI ? antiuselectRef : uselectRef) :
				joinRef;

			if ((s->op2->nrcols > 0 || s->op3->nrcols) &&
			   (s->type == st_select2 || s->type == st_uselect2)) {
				char *mod = calcRef;
				char *op1 = "<", *op2 = "<";

				r1 = _dumpstmt(sql, mb, s->op2);
				r2 = _dumpstmt(sql, mb, s->op3);

				if (s->flag&1)
					op1 = "<=";
				if (s->flag&2)
					op2 = "<=";

				q = multiplex2(mb,mod,convertOperator(op1),l,r1,TYPE_bit);

				r = multiplex2(mb,mod,convertOperator(op2),l,r2,TYPE_bit);
				p = newStmt1(mb, batcalcRef, "and");
				p = pushArgument(mb, p, getDestVar(q));
				p = pushArgument(mb, p, getDestVar(r));
				k = getDestVar(p);

				q = newStmt2(mb, algebraRef, uselectRef);
				q = pushArgument(mb, q, k);
				q = pushBit(mb, q, TRUE);
				s->nr = getDestVar(q);
				break;
			}
			/* if st_join2 try to convert to bandjoin */
			/* ie check if we substract/add a constant, to the
			   same column */
			if (s->type == st_join2 &&
			    range_join_convertable(s, &base, &low, &high)) {
				int tt = tail_type(base)->type->localtype;
				rs = _dumpstmt(sql, mb, base);
				q = newStmt2(mb, batRef, reverseRef);
				q = pushArgument(mb, q, rs);
				rs = getDestVar(q);
				if (low)
					r1 = _dumpstmt(sql, mb, low);
				else
					r1 = argumentZero(mb, tt);
				if (high)
					r2 = _dumpstmt(sql, mb, high);
				else
					r2 = argumentZero(mb, tt);
				cmd = bandjoinRef;
			}

			if (s->op2->type == st_atom &&
			    s->op3->type == st_atom &&
			    atom_null(s->op2->op4.aval) &&
			    atom_null(s->op3->op4.aval)
			) {
				q = newStmt2(mb, algebraRef, selectNotNilRef);
				q = pushArgument(mb, q, l);
				s->nr = getDestVar(q);
				break;
			}
			if (!rs) {
				r1 = _dumpstmt(sql, mb, s->op2);
				r2 = _dumpstmt(sql, mb, s->op3);
			}
			q = newStmt2(mb, algebraRef, cmd);
			q = pushArgument(mb, q, l);
			if (rs)
				q = pushArgument(mb, q, rs);
			q = pushArgument(mb, q, r1);
			q = pushArgument(mb, q, r2);

			switch (s->flag&3) {
			case 0:
				q = pushBit(mb, q, FALSE);
				q = pushBit(mb, q, FALSE);
				break;
			case 1:
				q = pushBit(mb, q, TRUE);
				q = pushBit(mb, q, FALSE);
				break;
			case 2:
				q = pushBit(mb, q, FALSE);
				q = pushBit(mb, q, TRUE);
				break;
			case 3:
				q = pushBit(mb, q, TRUE);
				q = pushBit(mb, q, TRUE);
				break;
			}
			s->nr = getDestVar(q);
		}
			break;
		case st_uselectN:
		case st_selectN: {
			assert(0);
			break;
		}
		case st_joinN:
			s->nr = dump_joinN(sql, mb, s);
			break;
		case st_semijoin:{
			dump_2(sql, mb, s, algebraRef, semijoinRef);
		}
			break;
		case st_diff:{
			dump_2(sql, mb, s, algebraRef, kdifferenceRef);
		}
			break;
		case st_union:{
			dump_2(sql, mb, s, algebraRef, kunionRef);
		}
			break;
		case st_outerjoin:
		case st_join:{
			int l = _dumpstmt(sql, mb, s->op1);
			int r = _dumpstmt(sql, mb, s->op2);
			char *jt = "join";

			assert(l >= 0 && r >= 0);
			if (s->type == st_outerjoin) {
				jt = "outerjoin";
			}

			switch (s->flag) {
			case cmp_equal:
				q = newStmt1(mb, algebraRef, jt);

				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, r);
				break;
			case cmp_notequal:
				q = newStmt1(mb, algebraRef, "antijoin");

				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, r);
				break;
			case cmp_lt:
				q = newStmt1(mb, algebraRef, "thetajoin");

				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, r);
				q = pushInt(mb, q, -1);
				break;
			case cmp_lte:
				q = newStmt1(mb, algebraRef, "thetajoin");

				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, r);
				q = pushInt(mb, q, -2);
				break;
			case cmp_gt:
				q = newStmt1(mb, algebraRef, "thetajoin");

				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, r);
				q = pushInt(mb, q, 1);
				break;
			case cmp_gte:
				q = newStmt1(mb, algebraRef, "thetajoin");

				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, r);
				q = pushInt(mb, q, 2);
				break;
			case cmp_all:	/* aka cross table */
			{
				q = newStmt2(mb, batRef, reverseRef);
				q = pushArgument(mb, q, r);
				r = getDestVar(q);

				q = dump_crossproduct(mb, l, r);
				break;
			}
			case cmp_project:
				/* projections, ie left is void headed */
				q = newStmt2(mb, algebraRef, leftjoinRef);

				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, r);
				break;
			default:
				showException(SQL,"sql","SQL2MAL: error impossible\n");
			}
			if (q)
				s->nr = getDestVar(q);
			break;
		}
		case st_group:{
			char *nme = GDKmalloc(SMALLBUFSIZ);
			int ext, grp, o1 = _dumpstmt(sql, mb, s->op1);

			q = newStmt2(mb, groupRef, s->flag&GRP_DONE?doneRef:newRef);
			ext = getDestVar(q);
			snprintf(nme, SMALLBUFSIZ, "grp%d", getDestVar(q));
			q = pushReturn(mb, q, newVariable(mb, nme, TYPE_any));
			grp = getArg(q, 1);
			(void) pushArgument(mb, q, o1);

			q = newAssignment(mb);
			q = pushArgument(mb, q, grp);
			s->nr = getDestVar(q);

			nme = GDKmalloc(SMALLBUFSIZ);
			snprintf(nme, SMALLBUFSIZ, "ext%d", s->nr);
			renameVariable(mb, ext, nme);

		} 	break;
		case st_group_ext:{
			char ext[SMALLBUFSIZ];
			int e = -1, g = _dumpstmt(sql, mb, s->op1);

			snprintf(ext, SMALLBUFSIZ, "ext%d", g);
			e = findVariable(mb, ext);
			assert(e >= 0);

			q = newStmt2(mb, batRef, mirrorRef);
			q = pushArgument(mb, q, e);
			s->nr = getDestVar(q);
		} break;
		case st_derive:{
			char *nme = GDKmalloc(SMALLBUFSIZ);
			char *buf = GDKmalloc(SMALLBUFSIZ);
			int ext, grp;
			int g = _dumpstmt(sql, mb, s->op1);
			int l = _dumpstmt(sql, mb, s->op2);

			q = newStmt2(mb, groupRef, s->flag&GRP_DONE?doneRef:deriveRef);
			ext = getDestVar(q);
			snprintf(nme, SMALLBUFSIZ, "grp%d", getDestVar(q));
			q = pushReturn(mb, q, newVariable(mb, nme, TYPE_any));
			grp = getArg(q, 1);
			(void) snprintf(buf, SMALLBUFSIZ, "ext%d", g);
			q = pushArgumentId(mb, q, buf);
			q = pushArgument(mb, q, g);
			(void) pushArgument(mb, q, l);

			q = newAssignment(mb);
			q = pushArgument(mb, q, grp);
			s->nr = getDestVar(q);

			nme = GDKmalloc(SMALLBUFSIZ);
			snprintf(nme, SMALLBUFSIZ, "ext%d", s->nr);
			renameVariable(mb, ext, nme);

		} 	break;
		case st_unique:{
			int l = _dumpstmt(sql, mb, s->op1);

			if (s->op2) {
				char *nme = GDKmalloc(SMALLBUFSIZ);
				char *buf = GDKmalloc(SMALLBUFSIZ);
				int e, g = _dumpstmt(sql, mb, s->op2);

				q = newStmt2(mb, groupRef, deriveRef);
				e = getDestVar(q);
				snprintf(nme, SMALLBUFSIZ, "grp%d", getDestVar(q));
				q = pushReturn(mb, q, newVariable(mb, nme, TYPE_any));
				(void) snprintf(buf, SMALLBUFSIZ, "ext%d", g);
				q = pushArgumentId(mb, q, buf);
				q = pushArgument(mb, q, g);
				(void) pushArgument(mb, q, l);

				q = newStmt2(mb, batRef, mirrorRef);
				q = pushArgument(mb, q, e);
				e = getDestVar(q);

/*
				q = newStmt1(mb, algebraRef, "semijoin");
				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, e);
*/
				q = newStmt2(mb, algebraRef, joinRef);
				q = pushArgument(mb, q, e);
				q = pushArgument(mb, q, l);
			} else {
				int k;

				q = newStmt2(mb, batRef, reverseRef);
				q = pushArgument(mb, q, l);
				k = getDestVar(q);
				q = newStmt1(mb, algebraRef, "kunique");
				q = pushArgument(mb, q, k);
				k = getDestVar(q);
				q = newStmt2(mb, batRef, reverseRef);
				q = pushArgument(mb, q, k);
			}
			s->nr = getDestVar(q);
			break;
		}
		case st_convert: {
			list *types = s->op4.lval;
			sql_subtype *f = types->h->data;
			sql_subtype *t = types->t->data;
			char *convert = t->type->base.name;
			/* convert types and make sure they are rounded up correctly */
			int l = _dumpstmt(sql, mb, s->op1);

			if (t->type->localtype == f->type->localtype &&
			    t->type->eclass == f->type->eclass &&
			    f->type->eclass != EC_INTERVAL &&
			    f->type->eclass != EC_DEC &&
			    (t->digits == 0 ||
			     f->digits == t->digits)
			   ) {
				s->nr = l;
				break;
			}

			/* external types have sqlname convert functions,
			   these can generate errors (fromstr cannot) */
			if (t->type->eclass == EC_EXTERNAL)
				convert = t->type->sqlname;

			if (t->type->eclass == EC_INTERVAL) {
				if (t->type->localtype == TYPE_int)
					convert = "month_interval";
				else
					convert = "second_interval";
			}

			/* Lookup the sql convert function, there is no need
			 * for single value vs bat, this is handled by the
			 * mal function resolution */
			if (s->nrcols == 0) { /* simple calc */
				q = newStmt1(mb, calcRef, convert);
			} else if (s->nrcols > 0 &&
			 	(t->type->localtype > TYPE_str ||
				 f->type->eclass == EC_DEC ||
				 t->type->eclass == EC_DEC ||
				 t->type->eclass == EC_INTERVAL ||
				 EC_TEMP(t->type->eclass) ||
				(EC_VARCHAR(t->type->eclass) &&
			    	!(f->type->eclass == EC_STRING &&
			    	t->digits == 0) ) ) ){
				int type = t->type->localtype;

				q = newStmt(mb, "mal","multiplex");
				setVarType(mb,getArg(q,0), newBatType(TYPE_oid,type));
				setVarUDFtype(mb,getArg(q,0));
				q = pushStr(mb, q, convertMultiplexMod("calc",convert));
				q = pushStr(mb, q, convertMultiplexFcn(convert));
			} else
				q = newStmt1(mb, batcalcRef, convert);

			/* convert to string is complex, we need full type info
			   and mvc for the timezone */
			if (EC_VARCHAR(t->type->eclass) &&
			    !(f->type->eclass == EC_STRING &&
			    t->digits == 0) ) {
				q = pushInt(mb, q, f->type->eclass);
				q = pushInt(mb, q, f->digits);
				q = pushInt(mb, q, f->scale);
				q = pushInt(mb, q, type_has_tz(f));
			} else if (f->type->eclass == EC_DEC)
				/* scale of the current decimal */
				q = pushInt(mb, q, f->scale);
			q = pushArgument(mb, q, l);

			if (t->type->eclass == EC_DEC ||
			    EC_TEMP_FRAC(t->type->eclass) ||
			    t->type->eclass == EC_INTERVAL) {
				/* digits, scale of the result decimal */
				q = pushInt(mb, q, t->digits);
				if (!EC_TEMP_FRAC(t->type->eclass))
					q = pushInt(mb, q, t->scale);
			}
			/* convert to string, give error on to large strings */
			if (EC_VARCHAR(t->type->eclass) &&
			    !(f->type->eclass == EC_STRING &&
			    t->digits == 0) )
				q = pushInt(mb, q, t->digits);
			s->nr = getDestVar(q);
			break;
		}
		case st_unop:{
			char *mod, *fimp;
			int l = _dumpstmt(sql, mb, s->op1);

			backend_create_func(sql, s->op4.funcval->func);
			mod = sql_func_mod(s->op4.funcval->func);
			fimp = sql_func_imp(s->op4.funcval->func);
			if (s->op1->nrcols && strcmp(fimp, "not_uniques") == 0) {
				int rtype = s->op4.funcval->res.type->localtype;

				q = newStmt(mb, mod, fimp);
				setVarType(mb,getArg(q,0), newBatType(TYPE_oid,rtype));
				setVarUDFtype(mb,getArg(q,0));
				q = pushArgument(mb, q, l);
			} else if (s->op1->nrcols) {
				int rtype = s->op4.funcval->res.type->localtype;

				q = newStmt(mb, "mal","multiplex");
				setVarType(mb,getArg(q,0), newBatType(TYPE_oid,rtype));
				setVarUDFtype(mb,getArg(q,0));
				q = pushStr(mb, q, convertMultiplexMod(mod,fimp));
				q = pushStr(mb, q, convertMultiplexFcn(fimp));
				q = pushArgument(mb, q, l);
			} else {
				q = newStmt(mb, mod, fimp);
				q = pushArgument(mb, q, l);
			}
			s->nr = getDestVar(q);
		}
			break;
		case st_binop:{
			/* TODO use the rewriter to fix the 'round' function */
			sql_subtype *tpe = tail_type(s->op1);
			int special = 0;
			char *mod, *fimp;
			int l = _dumpstmt(sql, mb, s->op1);
			int r = _dumpstmt(sql, mb, s->op2);

			backend_create_func(sql, s->op4.funcval->func);
			mod  = sql_func_mod(s->op4.funcval->func);
			fimp = sql_func_imp(s->op4.funcval->func);

			if (strcmp(fimp, "round")==0 &&
			    tpe->type->eclass == EC_DEC)
				special = 1;

			if (s->op1->nrcols || s->op2->nrcols) {
				if (!special) {
					q = multiplex2(mb,mod,convertOperator(fimp),l,r, s->op4.funcval->res.type->localtype);
				} else {
					mod= convertMultiplexMod(mod,fimp);
					fimp= convertMultiplexFcn(fimp);
					q = newStmt(mb, "mal","multiplex");
					setVarType(mb,getArg(q,0),
						newBatType(TYPE_oid,s->op4.funcval->res.type->localtype));
					setVarUDFtype(mb,getArg(q,0));
					q = pushStr(mb, q, mod);
					q = pushStr(mb, q, fimp);
					q = pushArgument(mb, q, l);
					q = pushInt(mb, q, tpe->digits);
					q = pushInt(mb, q, tpe->scale);
					q = pushArgument(mb, q, r);
				}
				s->nr = getDestVar(q);
			} else {
				q = newStmt(mb, mod, convertOperator(fimp));
				q = pushArgument(mb, q, l);
				if (special) {
					q = pushInt(mb, q, tpe->digits);
					q = pushInt(mb, q, tpe->scale);
				}
				q = pushArgument(mb, q, r);
			}
			s->nr = getDestVar(q);
		}
			break;
		case st_Nop:{
			char *mod, *fimp;
			sql_subtype *tpe = NULL;
			int special = 0;
			sql_subfunc *f = s->op4.funcval;
			node *n;
			/* dump operands */
			_dumpstmt(sql, mb, s->op1);

			backend_create_func(sql, f->func);
			mod = sql_func_mod(f->func);
			fimp = sql_func_imp(f->func);
			if (s->nrcols) {
				fimp = convertMultiplexFcn(fimp);
				q = multiplexN(mb,mod,fimp);
				if (!q) {
					q = newStmt(mb, "mal","multiplex");
					setVarType(mb,getArg(q,0),
						newBatType(TYPE_oid,f->res.type->localtype));
					setVarUDFtype(mb,getArg(q,0));
					q = pushStr(mb, q, mod);
					q = pushStr(mb, q, fimp);
				} else {
					setVarType(mb,getArg(q,0),
						newBatType(TYPE_any,f->res.type->localtype));
					setVarUDFtype(mb,getArg(q,0));
				}
			} else {
				fimp = convertOperator(fimp);
				q = newStmt(mb, mod, fimp);
				/* first dynamic output of copy* functions */
				if (f->res.comp_type) 
					q = table_func_create_result(mb, q, f->res.comp_type);
				else if (f->func->res.comp_type) 
					q = table_func_create_result(mb, q, f->func->res.comp_type);
			}
			if (list_length(s->op1->op4.lval))
				tpe = tail_type(s->op1->op4.lval->h->data);
			if (strcmp(fimp, "round")==0 && tpe &&
			    tpe->type->eclass == EC_DEC)
				special = 1;

			for (n = s->op1->op4.lval->h; n; n = n->next) {
				stmt *op = n->data;

				q = pushArgument(mb, q, op->nr);
				if (special) {
					q = pushInt(mb, q, tpe->digits);
					q = pushInt(mb, q, tpe->scale);
				}
				special = 0;
			}
			s->nr = getDestVar(q);
			/* keep reference to instruction */
			s->rewritten = (void*)q;
		} break;
		case st_aggr:{
			int l = _dumpstmt(sql, mb, s->op1);
			char *mod, *aggrfunc;
			int restype = s->op4.aggrval->res.type->localtype;
			int output_type_needed = 0;

			backend_create_func(sql, s->op4.aggrval->aggr);
			mod = s->op4.aggrval->aggr->mod;
			aggrfunc = s->op4.aggrval->aggr->imp;
			if (strcmp(aggrfunc, "sum") == 0 ||
					strcmp(aggrfunc, "prod") == 0)
				output_type_needed = 1;

			if (s->flag) {
				int l2 = _dumpstmt(sql, mb, s->op2);

				q = newStmt(mb, mod, aggrfunc);
				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, l2);

			} else if (s->op3) {
				int g = _dumpstmt(sql, mb, s->op2);
				int e = _dumpstmt(sql, mb, s->op3);

				q = newStmt(mb, mod, aggrfunc);
				setVarType(mb, getArg(q, 0), newBatType(TYPE_any, restype));
				setVarUDFtype(mb, getArg(q, 0));
				q = pushArgument(mb, q, l);
				q = pushArgument(mb, q, g);
				q = pushArgument(mb, q, e);
			} else {
				q = newStmt(mb, mod, aggrfunc);
				if (output_type_needed){
					setVarType(mb, getArg(q, 0), restype);
					setVarUDFtype(mb, getArg(q, 0));
				}
				q = pushArgument(mb, q, l);
			}
			s->nr = getDestVar(q);
		}
			break;
		case st_atom: {
			atom *a = s->op4.aval;
			q = newStmt1(mb, calcRef, atom_type(a)->type->base.name);
			if (atom_null(a)) {
				q = pushNil(mb, q, atom_type(a)->type->localtype);
			} else {
				int k;
				k = constantAtom(sql, mb, a);
				q = pushArgument(mb, q, k);
			}
			/* digits of the result timestamp/daytime */
			if (EC_TEMP_FRAC(atom_type(a)->type->eclass)) 
				q = pushInt(mb, q, atom_type(a)->digits);
			s->nr = getDestVar(q);
		}
			break;
		case st_append:{
			int l = 0;
			int r = _dumpstmt(sql, mb, s->op2);

			l = _dumpstmt(sql, mb, s->op1);
			q = newStmt2(mb, batRef, appendRef);
			q = pushArgument(mb, q, l);
			q = pushArgument(mb, q, r);
			q = pushBit(mb, q, TRUE);
			s->nr = getDestVar(q);
		} break;
		case st_update_col:
		case st_append_col:{
			int r = _dumpstmt(sql, mb, s->op1);
			sql_column *c = s->op4.cval;
			char *n = (s->type==st_append_col)?appendRef:updateRef;

			if (s->type == st_append_col && s->flag) { /* fake append */
				s->nr = r;
			} else {
				q = newStmt2(mb, sqlRef, n);
				q = pushArgument(mb, q, sql->mvc_var);
				getArg(q, 0) = sql->mvc_var= newTmpVariable(mb,TYPE_int);
				q = pushSchema(mb, q, c->t);
				q = pushStr(mb, q, c->t->base.name);
				q = pushStr(mb, q, c->base.name);
				q = pushArgument(mb, q, r);
				sql->mvc_var = s->nr = getDestVar(q);
			}
		} break;

		case st_update_idx:
		case st_append_idx:{
			int r = _dumpstmt(sql, mb, s->op1);
			sql_idx *i = s->op4.idxval;
			char *n = (s->type==st_append_idx)?appendRef:updateRef;

			q = newStmt2(mb, sqlRef, n);
			q = pushArgument(mb, q, sql->mvc_var);
			getArg(q, 0) = sql->mvc_var= newTmpVariable(mb,TYPE_int);
			q = pushSchema(mb, q, i->t);
			q = pushStr(mb, q, i->t->base.name);
			q = pushStr(mb, q, sa_strconcat(sql->mvc->sa, "%", i->base.name));
			q = pushArgument(mb, q, r);
			sql->mvc_var = s->nr = getDestVar(q);
		} break;
		case st_delete:{
			int r = _dumpstmt(sql, mb, s->op1);
			sql_table *t = s->op4.tval;
			str mod = isRemote(t)?remoteRef:sqlRef;

			q = newStmt1(mb, mod, "delete");
			q = pushArgument(mb, q, sql->mvc_var);
			getArg(q, 0) = sql->mvc_var= newTmpVariable(mb,TYPE_int);
			q = pushSchema(mb, q, t);
			q = pushStr(mb, q, t->base.name);
			q = pushArgument(mb, q, r);
			sql->mvc_var = s->nr = getDestVar(q);
		} break;
		case st_table_clear:{
			sql_table *t = s->op4.tval;
			str mod = isRemote(t)?remoteRef:sqlRef;

			q = newStmt1(mb, mod, "clear_table");
			q = pushSchema(mb, q, t);
			q = pushStr(mb, q, t->base.name);
			s->nr = getDestVar(q);
		} break;
		case st_exception:{
			int l,r;

			l = _dumpstmt(sql, mb, s->op1);
			r = _dumpstmt(sql, mb, s->op2);

			/* if(bit(l)) { error(r);}  ==raising an exception */
			q = newStmt1(mb, sqlRef, "assert");
			q = pushArgument(mb, q, l);
			q = pushArgument(mb, q, r);
			s->nr = getDestVar(q);
			break;
		}
		case st_trans:{
			int l,r = -1;

			l = _dumpstmt(sql, mb, s->op1);
			if (s->op2)
				r = _dumpstmt(sql, mb, s->op2);
			q = newStmt1(mb, sqlRef, "trans");
			q = pushInt(mb, q, s->flag);
			q = pushArgument(mb, q, l);
			if (r > 0)
				q = pushArgument(mb, q, r);
			else
				q = pushNil(mb, q, TYPE_str);
			s->nr = getDestVar(q);
			break;
		}
		case st_catalog: {
			_dumpstmt(sql, mb, s->op1);

			q = newStmt1(mb, sqlRef, "catalog");
			q = pushInt(mb, q, s->flag);
			for (n = s->op1->op4.lval->h; n; n = n->next) {
				stmt *c = n->data;
				q = pushArgument(mb, q, c->nr);
			}
			s->nr = getDestVar(q);
			break;
		}
		case st_alias:
			s->nr = _dumpstmt(sql, mb, s->op1);
			break;
		case st_list:
			for (n = s->op4.lval->h; n; n = n->next) {
				_dumpstmt(sql, mb, n->data);
			}
			s->nr = 1;
			break;
		case st_rs_column:{
			_dumpstmt(sql, mb, s->op1);
			q = (void*)s->op1->rewritten;
			s->nr = getArg(q,s->flag);
		} break;
		case st_ordered:{
			int l = _dumpstmt(sql, mb, s->op1);

			_dumpstmt(sql, mb, s->op2);
			s->nr = l;
		} break;
		case st_affected_rows:{
			InstrPtr q;
			int o1 = _dumpstmt(sql, mb, s->op1);

			q = newStmt1(mb, sqlRef, "affectedRows");
			q = pushArgument(mb, q, sql->mvc_var);
			getArg(q, 0) = sql->mvc_var= newTmpVariable(mb,TYPE_int);
			q = pushArgument(mb, q, o1);
			q = pushStr(mb, q, ""); /* warning */
			sql->mvc_var = s->nr = getDestVar(q);
		} break;
		case st_output:
		case st_export:{
			stmt *order = NULL;
			stmt *lst = s->op1;

			_dumpstmt(sql, mb, lst);

			if (lst->type == st_ordered) {
				order = lst->op1;
				lst = lst->op2;
			}
			if (lst->type == st_list) {
				list *l = lst->op4.lval;
				int file, cnt = list_length(l);
				stmt *f;
				InstrPtr k;

				n = l->h;
				f = n->data;

				/* single value result, has a fast exit */
				if (cnt == 1 && !order && f->nrcols <= 0 &&
						s->type != st_export)
				{
					stmt *c = n->data;
					sql_subtype *t = tail_type(c);
					char *tname = table_name(sql->mvc->sa, c);
					char *sname = schema_name(sql->mvc->sa, c);
					char *_empty = "";
					char *tn = (tname) ? tname : _empty;
					char *sn = (sname) ? sname : _empty;
					char *cn = column_name(sql->mvc->sa, c);
					char *ntn = sql_escape_ident(tn);
					char *nsn = sql_escape_ident(sn);
					size_t fqtnl = strlen(ntn) + 1 + strlen(nsn) + 1;
					char *fqtn = NEW_ARRAY(char, fqtnl);

					snprintf(fqtn, fqtnl, "%s.%s", nsn, ntn);

					q = newStmt2(mb, sqlRef, exportValueRef);
					s->nr = getDestVar(q);
					q = pushInt(mb, q, sql->mvc->type);
					q = pushStr(mb, q, fqtn);
					q = pushStr(mb, q, cn);
					q = pushStr(mb, q, t->type->sqlname);
					q = pushInt(mb, q, t->digits);
					q = pushInt(mb, q, t->scale);
					q = pushInt(mb, q, t->type->eclass);
					q = pushArgument(mb, q, c->nr);
					(void) pushStr(mb, q, "");   /* warning */
					_DELETE(ntn);
					_DELETE(nsn);
					_DELETE(fqtn);
					break;
				}
				if (n) {
					if (!order) {
						order = n->data;
					}
				}
				k = newStmt2(mb, sqlRef, resultSetRef);
				s->nr = getDestVar(k);
				k = pushInt(mb, k, cnt);
				if (s->type == st_export) {
					node *n = s->op4.lval->h;
					char *sep = n->data;
					char *rsep = n->next->data;
					char *ssep = n->next->next->data;
					char *ns = n->next->next->next->data;

					k = pushStr(mb, k, sep);
					k = pushStr(mb, k, rsep);
					k = pushStr(mb, k, ssep);
					k = pushStr(mb, k, ns);
				} else {
					k = pushInt(mb, k, sql->mvc->type);
				}
				(void) pushArgument(mb, k, order->nr);
				dump_header(sql->mvc, mb, s, l);

				if (s->type == st_export && s->op2) {
					int codeset;

					q = newStmt(mb, "str", "codeset");
					codeset = getDestVar(q);
					file = _dumpstmt(sql, mb, s->op2);

					q = newStmt(mb, "str", "iconv");
					q = pushArgument(mb, q, file);
					q = pushStr(mb, q, "UTF-8");
					q = pushArgument(mb, q, codeset);
					file = getDestVar(q);

					q = newStmt(mb, "streams", "openWrite");
					q = pushArgument(mb, q, file);
					file = getDestVar(q);
				} else {
					q = newStmt(mb, "io", "stdout");
					file = getDestVar(q);
				}
/*

				q = newStmt2(mb, sqlRef, putName("exportHead",10));
				q = pushArgument(mb, q, file);
				q = pushArgument(mb, q, s->nr);
				q = newStmt2(mb, sqlRef, putName("exportChunk",11));
				q = pushArgument(mb, q, file);
				q = pushArgument(mb, q, s->nr);
				q = pushInt(mb,q,0);
				q = pushInt(mb,q,10);
				q = newStmt2(mb, sqlRef, putName("exportChunk",11));
				q = pushArgument(mb, q, file);
				q = pushArgument(mb, q, s->nr);
				q = pushInt(mb,q,10);
				q = pushInt(mb,q,0);
*/
				q = newStmt2(mb, sqlRef, exportResultRef);
				q = pushArgument(mb, q, file);
				(void) pushArgument(mb, q, s->nr);
				if (s->type == st_export && s->op2) {
					q = newStmt(mb, "streams", "close");
					(void) pushArgument(mb, q, file);
				}
			} else {
				q = newStmt1(mb, sqlRef, "print");
				(void) pushStr(mb, q, "not a valid output list\n");
				s->nr = 1;
			}
		}
			break;

		case st_table:{
			stmt *lst = s->op1;

			_dumpstmt(sql, mb, lst);

			if (lst->type != st_list) {
				q = newStmt1(mb, sqlRef, "print");
				(void) pushStr(mb, q, "not a valid output list\n");
			}
			s->nr = 1;
		}
			break;


		case st_cond: {
			int c = _dumpstmt(sql,mb, s->op1);

			if (!s->flag) { /* if */
				q= newAssignment(mb);
				q->barrier = BARRIERsymbol;
				pushArgument(mb,q,c);
				s->nr = getArg(q,0);
			} else { /* while */
				int outer = _dumpstmt(sql,mb, s->op2);

				/* leave barrier */
				q = newStmt1(mb, calcRef, "not");
				q = pushArgument(mb, q, c);
				c = getArg(q,0);

				q = newAssignment(mb);
				getArg(q,0) = outer;
				q->barrier= LEAVEsymbol;
				pushArgument(mb,q,c);
				s->nr = outer;
			}
		}	break;
		case st_control_end: {
			int c= _dumpstmt(sql,mb, s->op1);

			if (s->op1->flag) { /* while */
			        /* redo barrier */
				q = newAssignment(mb);
				getArg(q,0) = c;
				q->argc = q->retc = 1;
				q->barrier = REDOsymbol;
				(void) pushBit(mb, q, TRUE);
			} else {
				q = newAssignment(mb);
				getArg(q,0) = c;
				q->argc = q->retc = 1;
				q->barrier = EXITsymbol;
			}
			q = newStmt1(mb, sqlRef, "mvc");
			sql->mvc_var = getDestVar(q);
			s->nr = getArg(q,0);
		} 	break;
		case st_return: {
			int c = _dumpstmt(sql,mb, s->op1);

			if (s->flag) { /* drop declared tables */
				InstrPtr k = newStmt1(mb, sqlRef, "dropDeclaredTables");
				(void) pushInt(mb, k, s->flag);
			}
			q = newInstruction(mb,RETURNsymbol);
			if (s->op1->type == st_table) {
				list *l = s->op1->op1->op4.lval;

				q = dump_cols(mb, l, q);
			} else {
				getArg(q,0) = getArg(getInstrPtr(mb,0),0);
				q = pushArgument(mb,q,c);
			}
			pushInstruction(mb, q);
			s->nr = 1;
		}	break;
		case st_assign: {
			int r = -1;

			if (s->op2)
				r = _dumpstmt(sql,mb, s->op2);
			if (!VAR_GLOBAL(s->flag)) { /* globals */
				char *buf = GDKmalloc(MAXIDENTLEN);
				char *vn = atom2string(sql->mvc->sa, s->op1->op4.aval);

				if (!s->op2) {
					/* drop declared table */
					s->nr = drop_table(mb, vn);
					break;
				}
				(void) snprintf(buf, MAXIDENTLEN, "A%s", vn);
				q = newInstruction(mb,ASSIGNsymbol);
				q->argc = q->retc = 0;
				q = pushArgumentId(mb, q, buf);
				pushInstruction(mb, q);
				q->retc++;
				s->nr = 1;
			} else {
				int vn = _dumpstmt(sql, mb, s->op1);
				q = newStmt1(mb, sqlRef, "setVariable");
				q = pushArgument(mb, q, sql->mvc_var);
				q = pushArgument(mb, q, vn);
				getArg(q, 0) = sql->mvc_var= newTmpVariable(mb,TYPE_int);
				sql->mvc_var = s->nr = getDestVar(q);
			}
			(void) pushArgument(mb, q, r);
		} 	break;

			/* todo */
		case st_basetable:
		case st_relselect:
		case st_releqjoin:
		case st_reljoin:
			mnstr_printf(GDKout, "not implemented stmt\n");
			assert(0);


		}

		return s->nr;
	}

	return(0);
}
/*
 * @-
 * The kernel uses two calls to procedures defined in SQL.
 * They have to be initialized, which is currently hacked
 * by using the SQLstatment.
 */
static void setCommitProperty(MalBlkPtr mb){
	ValRecord cst;

	if ( varGetProp(mb, getArg(mb->stmt[0],0), PropertyIndex("autoCommit")) )
		return; /* already set */
	cst.vtype= TYPE_bit;
	cst.val.cval[0]= TRUE;
	cst.val.cval[1]= 0;
	cst.val.cval[2]= 0;
	cst.val.cval[3]= 0;
	varSetProperty(mb, getArg(getInstrPtr(mb,0),0), "autoCommit","=", &cst);
}

static void backend_dumpstmt(backend *be, MalBlkPtr mb, stmt *s)
{
	mvc *c = be->mvc;
	stmt **stmts = stmt_array(c->sa, s);
	InstrPtr q;
	int old_mv = be->mvc_var, nr = 0;

	/* announce the transaction mode */
	if ( c->session->auto_commit)
		setCommitProperty(mb);
	q = newStmt1(mb, sqlRef, "mvc");
	be->mvc_var = getDestVar(q);

	/*print_stmts(c->sa, stmts);*/
	clear_stmts(stmts);
	while(stmts[nr]) {
		stmt *s = stmts[nr++];
		(void)_dumpstmt(be, mb, s);
	}
	(void)_dumpstmt(be, mb, s);

	be->mvc_var = old_mv;
	if (c->caching && (c->type == Q_SCHEMA || c->type == Q_TRANS)) {
		q = newStmt2(mb, sqlRef, exportOperationRef);
		(void) pushStr(mb, q, ""); /* warning */
	}
	/* generate a dummy return assignment for functions */
	if (getArgType(mb,getInstrPtr(mb,0),0) != TYPE_void &&
	    getInstrPtr(mb,mb->stop-1)->barrier != RETURNsymbol) {
		q = newAssignment(mb);
		getArg(q,0) = getArg(getInstrPtr(mb,0),0);
		q->barrier = RETURNsymbol;
	}
	pushEndInstruction(mb);
}

void
backend_callinline(backend *be, Client c, stmt *s )
{
	mvc *m = be->mvc;
	InstrPtr curInstr = 0;
	MalBlkPtr curBlk = c->curprg->def;

	curInstr = getInstrPtr(curBlk, 0);

	if (m->argc) { /* we shouldn't come here as we aren't caching statements */
		int argc=0;
		char arg[SMALLBUFSIZ];

		for (; argc < m->argc; argc++) {
			atom *a = m->args[argc];
			int type = atom_type(a)->type->localtype;
			int varid = 0;

			curInstr = newAssignment(curBlk);
			snprintf(arg, SMALLBUFSIZ, "A%d", argc);
			varid = getDestVar(curInstr);
			renameVariable(curBlk, varid, _strdup(arg));
			setVarType(curBlk, varid, type);
			setVarUDFtype(curBlk,varid);

			if (atom_null(a)) {
				sql_subtype *t = atom_type(a);
				(void) pushNil(curBlk, curInstr, t->type->localtype);
			} else {
				int _t = constantAtom(be, curBlk, a);
				(void) pushArgument(curBlk, curInstr, _t);
			}
		}
	}
	backend_dumpstmt(be, curBlk, s);
	c->curprg->def = curBlk;
}

Symbol
backend_dumpproc(backend *be, Client c, cq *cq, stmt *s)
{
	mvc *m = be->mvc;
	MalBlkPtr mb = 0;
	Symbol curPrg = 0, backup = NULL;
	InstrPtr curInstr = 0;
	int argc = 0;
	char arg[SMALLBUFSIZ];
	node *n;
	lng Toptimize = 0, Tparse = 0;

	backup = c->curprg;

	if (m->history == 1) {
		sql_schema *sys = mvc_bind_schema(m, "sys");
		sql_subfunc *kq = sql_find_func(m->sa, sys, "keepquery", NR_KEEPQUERY_ARGS);
		sql_subfunc *cq = sql_find_func(m->sa, sys, "keepcall", NR_KEEPCALL_ARGS);

		assert(kq && cq);
		backend_create_func(be, kq->func);
		backend_create_func(be, cq->func);
		/* only needed once */
		m->history = 2;
	}
	/* later we change this to a factory ? */
	if (cq)
		c->curprg = newFunction(userRef,putName(cq->name,strlen(cq->name)), FUNCTIONsymbol);
	else
		c->curprg = newFunction(userRef,"tmp", FUNCTIONsymbol);

	curPrg = c->curprg;
	curPrg->def->keephistory= backup->def->keephistory;
	mb = curPrg->def;
	curInstr = getInstrPtr(mb, 0);
	/* we do not return anything */
	setVarType(mb, 0, TYPE_void);
	setVarUDFtype(mb,0);
	setModuleId(curInstr, putName("user",4));

	if (m->argc) {
		for (argc = 0; argc < m->argc; argc++) {
			atom *a = m->args[argc];
			int type = atom_type(a)->type->localtype;
			int varid = 0;

			snprintf(arg, SMALLBUFSIZ, "A%d", argc);
			varid = newVariable(mb, _strdup(arg), type);
			curInstr = pushArgument(mb, curInstr, varid);
			setVarType(mb, varid, type);
			setVarUDFtype(mb,0);
		}
	} else if (m->params) { /* needed for prepare statements */

		for (n = m->params->h; n; n = n->next, argc++) {
			sql_arg *a = n->data;
			int type = a->type.type->localtype;
			int varid = 0;

			snprintf(arg, SMALLBUFSIZ, "A%d", argc);
			varid = newVariable(mb, _strdup(arg), type);
			curInstr = pushArgument(mb, curInstr, varid);
			setVarType(mb, varid, type);
			setVarUDFtype(mb,varid);
		}
	}

	backend_dumpstmt(be, mb, s);
	Toptimize = GDKusec();
	Tparse = Toptimize - m->Tparse;

	if (m->history) {
		char *t;
		oid queryid = OIDnew(1);
		InstrPtr q;

		if ( be->q && be->q->codestring) {
			t = GDKstrdup(  be->q->codestring);
			while( t && isspace((int) *t) )
				t++;
		} else
			t = GDKstrdup("-- no query");

		q = newStmt1(mb, userRef, "keepquery");
		q->token = REMsymbol;
		q = pushWrd(mb, q, queryid);
		q = pushStr(mb, q, t);
		q = pushLng(mb, q, Tparse );
		(void) pushLng(mb, q, Toptimize);
		m->Tparse = 0;
	}
	if (cq)
		addQueryToCache(c);
	Toptimize = GDKusec() - Toptimize;

	curPrg = c->curprg;
	if (backup)
		c->curprg = backup;
	return curPrg;
}

void
backend_call(backend *be, Client c, cq *cq)
{
	mvc *m = be->mvc;
	InstrPtr q;
	MalBlkPtr mb = c->curprg->def;

	q = newStmt1(mb, userRef, cq->name);
	/* cached (factorized queries return bit??) */
	if (getInstrPtr(((Symbol)cq->code)->def, 0)->token == FACTORYsymbol ) {
		setVarType(mb, getArg(q, 0), TYPE_bit);
		setVarUDFtype(mb,getArg(q,0));
	} else {
		setVarType(mb, getArg(q, 0), TYPE_void);
		setVarUDFtype(mb,getArg(q,0));
	}
	if (m->argc) {
		int i;

		for (i=0; i<m->argc; i++){
			atom *a = m->args[i];
			sql_subtype *pt = cq->params+i;

			if (!atom_cast(a, pt)) {
				sql_error(m, 003,
					  "wrong type for argument %d of "
					  "function call: %s, expected %s\n",
					  i + 1, atom_type(a)->type->sqlname,
					  pt->type->sqlname);
				break;
			}
			if (atom_null(a)) {
				sql_subtype *t = cq->params+i;
				/* need type from the prepared argument */
				q = pushNil(mb, q, t->type->localtype);
			} else {
				int _t = constantAtom(be, mb, a);
				q = pushArgument(mb, q, _t);
			}
		}
	}
}

void
monet5_create_table_function(ptr M, char *name, sql_rel *rel, sql_table *t)
{
	sql_rel *r;
	mvc *m = (mvc*)M;
	Client c = MCgetClient(m->clientid);
	backend *be = ((backend *) c->state[MAL_SCENARIO_PARSER]);
	MalBlkPtr curBlk = 0;
	InstrPtr curInstr = 0;
	Symbol backup = NULL;
	stmt *s, *opt;

	(void)t;
	r = rel_optimizer(m, rel);
	s = rel_bin(m, r);
/*
	if (s && s->type == st_ordered) {
		stmt *order = s->op1;
		stmt *ns = s->op2;

		s = sql_reorder(order, ns);
	}
*/
	if (s->type == st_list && s->nrcols == 0 && s->key) {
		/* row to columns */
		node *n;
		list *l = list_new(m->sa);

		for(n=s->op4.lval->h; n; n = n->next)
			list_append(l, const_column(m->sa, n->data));
		s = stmt_list(m->sa, l);
	}
	s = stmt_table(m->sa, s, 1);
	s = stmt_return(m->sa, s, 0);
	opt = rel2bin(m, s);
	s = bin_optimizer(m, opt);

	backup = c->curprg;
	c->curprg = newFunction(userRef,putName(name,strlen(name)), FUNCTIONsymbol);

	curBlk = c->curprg->def;
	curInstr = getInstrPtr(curBlk, 0);

	curInstr = table_func_create_result(curBlk, curInstr, t);
	setVarUDFtype(curBlk,0);

	/* no ops */

	backend_dumpstmt(be, curBlk, s);
	/* SQL function definitions meant for inlineing should not be optimized before */
	varSetProp(curBlk, getArg(curInstr, 0), sqlfunctionProp, op_eq, NULL);
	addQueryToCache(c);
	if (backup)
		c->curprg = backup;
}

int
monet5_resolve_function(ptr M, sql_func *f)
{
	mvc *sql = (mvc*)M;
	Client c = MCgetClient(sql->clientid);
	Module m;

	/*
 fails to search outer modules!
	if (!findSymbol(c->nspace, f->mod, f->imp))
		return 0;
	*/

	for (m = findModule(c->nspace, f->mod); m; m= m->outer) {
		if (strcmp(m->name, f->mod) == 0) {
			Symbol s = m->subscope[(int)(getSubScope(f->imp))];
			for(; s; s = s->peer) {
				InstrPtr sig = getSignature(s);
				int argc = sig->argc - sig->retc;

				if (strcmp(s->name, f->imp) == 0 &&
						((!f->ops && argc == 0) ||
						 list_length(f->ops) == argc ||
						 (sig->varargs & VARARGS) == VARARGS))
					return 1;

			}
		}
	}
	return 0;
/*
	node *n;
	newFcnCall(f->mod, f->imp);
	for (n = f->ops->h; n; n = n->next) {
		sql_arg *a = n->data;

		q = push ?type? (mb, q, a->);
	}
*/
}

/* TODO handle aggr */
void
backend_create_func(backend *be, sql_func *f)
{
	mvc *m = be->mvc;
	sql_schema *schema = m->session->schema;
	MalBlkPtr curBlk = 0;
	InstrPtr curInstr = 0;
	Client c = be->client;
	Symbol backup = NULL;
	stmt *s;
	int i, retseen =0, sideeffects =0;
	sql_allocator *sa, *osa = m->sa;

	/* nothing to do for internal and ready (not recompiling) functions */
	if (!f->sql || f->sql > 1)
		return;
	f->sql++;
 	sa = sa_create();
	m->session->schema = f->s;
	s = sql_parse(m, sa, f->query, m_instantiate);
	m->session->schema = schema;
	if (s && !f->sql) /* native function */
		return;

	if (!s) {
		fputs(m->errstr, stderr);
		sa_destroy(sa);
		return;
	} else {
		stmt *opt;

		m->sa = sa;
		opt = rel2bin(m, s);
		s = bin_optimizer(m, opt);
	}
	assert(s);

	backup = c->curprg;
	c->curprg = newFunction(userRef,putName(f->base.name,strlen(f->base.name)), FUNCTIONsymbol);

	curBlk = c->curprg->def;
	curInstr = getInstrPtr(curBlk, 0);

	if (f->res.type) {
		if (f->res.comp_type)
			curInstr = table_func_create_result(curBlk, curInstr, f->res.comp_type);
		else
			setVarType(curBlk, 0, f->res.type->localtype);
	} else {
		setVarType(curBlk, 0, TYPE_void);
	}
	setVarUDFtype(curBlk,0);

	if (f->ops) {
		int argc = 0;
		node *n;

		for (n = f->ops->h; n; n = n->next, argc++) {
			sql_arg *a = n->data;
			int type = a->type.type->localtype;
			int varid = 0;
			char *buf = GDKmalloc(MAXIDENTLEN);

			if (a->name)
				(void) snprintf(buf, MAXIDENTLEN, "A%s", a->name);
			else
				(void) snprintf(buf, MAXIDENTLEN, "A%d", argc);
			varid = newVariable(curBlk, buf, type);
			curInstr = pushArgument(curBlk, curInstr, varid);
			setVarType(curBlk, varid, type);
			setVarUDFtype(curBlk,varid);
		}
	}
	/* announce the transaction mode */
	if ( m->session->auto_commit)
		setCommitProperty(curBlk);

	backend_dumpstmt(be, curBlk, s);
	/* selectively make functions available for inlineing */
	/* for the time being we only inline scalar functions */
	/* and only if we see a single return value */
	/* check the function for side effects and make that explicit */
	sideeffects =0;
	for( i= 1; i<curBlk->stop; i++){
		InstrPtr p = getInstrPtr(curBlk,i);
		if ( getFunctionId(p)== bindRef || getFunctionId(p)== bindidxRef)
			continue;
		sideeffects = sideeffects || hasSideEffects(p,FALSE) || (getModuleId(p) != sqlRef && isUpdateInstruction(p));
		if ( p->token == RETURNsymbol || p->token == YIELDsymbol ||
		     p->barrier == RETURNsymbol || p->barrier == YIELDsymbol)
			retseen++;
	}
	if (i == curBlk->stop && retseen == 1)
		varSetProp(curBlk, getArg(curInstr, 0), inlineProp, op_eq, NULL);
	if ( sideeffects)
		varSetProp(curBlk, getArg(curInstr, 0), unsafeProp, op_eq, NULL);
	/* SQL function definitions meant for inlineing should not be optimized before */
	varSetProp(curBlk, getArg(curInstr, 0), sqlfunctionProp, op_eq, NULL);
	m->sa = osa;
	addQueryToCache(c);
	if (backup)
		c->curprg = backup;
}
