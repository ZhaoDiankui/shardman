/* ------------------------------------------------------------------------
 *
 * exchange.c
 *		This module contains the EXHCANGE custom node implementation
 *
 *
 * Copyright (c) 2018, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#include "postgres.h"
#include "unistd.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opclass.h"
#include "commands/defrem.h"
#include "dmq.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/readfuncs.h"
#include "nodes/relation.h"
#include "nodeDistPlanExec.h"
#include "optimizer/cost.h"
#include "partitioning/partbounds.h"
#include "postgres_fdw.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "common.h"
#include "exchange.h"
#include "partutils.h"
#include "stream.h"

/*
 * Startup cost of EXCHANGE node is smaller, than DistExec node, because cost
 * of DistExec node contains cost of query transfer and localization at each
 * instance. Startup cost of EXCHANGE node includes only connection channel
 * establishing between instances.
 */
#define DEFAULT_EXCHANGE_STARTUP_COST	10.0
#define DEFAULT_TRANSFER_TUPLE_COST		0.01


static CustomPathMethods		exchange_path_methods;
static CustomScanMethods		exchange_plan_methods;
static CustomExecMethods		exchange_exec_methods;
static ExtensibleNodeMethods	exchange_plan_private;

/* XXX: Need to be placed in shared memory */
uint32 exchange_counter = 0;

#define ZPS \
{ \
		.strategy = PARTITION_STRATEGY_HASH, \
		.partnatts = 0, \
		.partopfamily = NULL, \
		.partopcintype = NULL, \
		.partcollation = NULL, \
		.parttyplen = NULL, \
		.parttypbyval = NULL, \
		.partsupfunc = NULL \
};


#define ZBI \
{ \
		.strategy = PARTITION_STRATEGY_HASH, \
		.ndatums = 0, \
		.datums = NULL, \
		.kind = NULL, \
		.indexes = NULL, \
		.null_index = -1, \
		.default_index = -1 \
};

PartitionSchemeData ZERO_PART_SCHEME = ZPS;
PartitionBoundInfoData ZERO_BOUND_INFO = ZBI;
PartitionSchemeData BCAST_PART_SCHEME = ZPS;
PartitionBoundInfoData BCAST_BOUND_INFO = ZBI;
PartitionSchemeData STEALTH_PART_SCHEME = ZPS;
PartitionBoundInfoData STEALTH_BOUND_INFO = ZBI;

static void nodeOutExchangePrivate(struct StringInfoData *str,
								   const struct ExtensibleNode *node);
static void nodeReadExchangePrivate(struct ExtensibleNode *node);
static struct Plan * ExchangeCreateCustomPlan(PlannerInfo *root,
					   RelOptInfo *rel,
					   struct CustomPath *best_path,
					   List *tlist,
					   List *clauses,
					   List *custom_plans);

static Node *EXCHANGE_Create_state(CustomScan *node);
static void EXCHANGE_Begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *EXCHANGE_Execute(CustomScanState *node);
static void EXCHANGE_End(CustomScanState *node);
static void EXCHANGE_Rescan(CustomScanState *node);
static void EXCHANGE_ReInitializeDSM(CustomScanState *node,
									 ParallelContext *pcxt,
									 void *coordinate);
static void EXCHANGE_Explain(CustomScanState *node, List *ancestors,
							 ExplainState *es);
static Size EXCHANGE_EstimateDSM(CustomScanState *node, ParallelContext *pcxt);
static void EXCHANGE_InitializeDSM(CustomScanState *node, ParallelContext *pcxt,
								   void *coordinate);
static void EXCHANGE_InitializeWorker(CustomScanState *node,
									  shm_toc *toc,
									  void *coordinate);

static void create_gather_dfn(const Distribution dist, EPPNode *epp);
static void create_stealth_dfn(const PlannerInfo *root, const Distribution dist,
							   EPPNode *epp);
static void create_shuffle_dfn(const PlannerInfo *root, const Distribution dist,
							   EPPNode *epp);
static void create_broadcast_dfn(const PlannerInfo *root,
								 const Distribution dist,
								 EPPNode *epp);


/*
 * Serialize extensible node with EXCHANGE distribution data.
 */
static void
nodeOutExchangePrivate(struct StringInfoData *str,
					   const struct ExtensibleNode *node)
{
	EPPNode *planNode = (EPPNode *) node;
	int i;

	Assert(str && node);

	appendStringInfo(str, " :nnodes %d", planNode->nnodes);
	appendStringInfoString(str, " :nodenames");
	for (i = 0; i < planNode->nnodes; i++)
		appendStringInfo(str, " %s", planNode->nodes[i]);

	appendStringInfo(str, " :natts %d", planNode->natts);

	appendStringInfoString(str, " :funcids");
	for (i = 0; i < planNode->natts; i++)
		appendStringInfo(str, " %u", planNode->funcid[i]);

	appendStringInfoString(str, " :att_exprs ");
	outNode(str, planNode->exprs);

	appendStringInfo(str, " :mode %d", planNode->mode);
}

/*
 * Deserialize extensible node with EXCHANGE distribution data.
 */
static void
nodeReadExchangePrivate(struct ExtensibleNode *node)
{
	EPPNode *planNode = (EPPNode *) node;
	char *token;
	int length;
	int i;

	Assert(planNode);

	/* nnodes field */
	token = pg_strtok(&length);		/* skip :fldname */
	token = pg_strtok(&length);
	planNode->nnodes = atoi(token);

	/* array of node names */
	planNode->nodes = palloc0(sizeof(NodeName) * planNode->nnodes);
	token = pg_strtok(&length);
	for (i = 0; i < planNode->nnodes; i++)
	{
		char *str;

		token = pg_strtok(&length);
		str = debackslash(token, length);
		memcpy(planNode->nodes[i], str, strlen(str));
	}

	/* natts field */
	token = pg_strtok(&length);		/* skip :fldname */
	token = pg_strtok(&length);
	planNode->natts = atoi(token);

	/* funcid array */
	token = pg_strtok(&length);		/* skip :fldname */
	planNode->funcid = readOidCols(planNode->natts);

	/* att_exprs field */
	token = pg_strtok(&length);		/* skip :fldname */
	planNode->exprs = nodeRead(NULL, 0);

	/* mode field */
	token = pg_strtok(&length);		/* skip :fldname */
	token = pg_strtok(&length);
	planNode->mode = atoi(token);
}

void
EXCHANGE_Init_methods(void)
{
	/* Initialize path generator methods */
	exchange_path_methods.CustomName = EXCHANGE_PATH_NAME;
	exchange_path_methods.PlanCustomPath = ExchangeCreateCustomPlan;
	exchange_path_methods.ReparameterizeCustomPathByChild = NULL;

	exchange_plan_methods.CustomName = EXCHANGE_PLAN_NAME;
	exchange_plan_methods.CreateCustomScanState	= EXCHANGE_Create_state;
	RegisterCustomScanMethods(&exchange_plan_methods);

	exchange_plan_private.extnodename = EXCHANGE_PRIVATE_NAME;
	exchange_plan_private.node_size = sizeof(EPPNode);
	exchange_plan_private.nodeCopy = NULL;
	exchange_plan_private.nodeEqual = NULL;
	exchange_plan_private.nodeOut = nodeOutExchangePrivate;
	exchange_plan_private.nodeRead = nodeReadExchangePrivate;
	RegisterExtensibleNodeMethods(&exchange_plan_private);

	/* setup exec methods */
	exchange_exec_methods.CustomName				= EXCHANGE_STATE_NAME;
	exchange_exec_methods.BeginCustomScan			= EXCHANGE_Begin;
	exchange_exec_methods.ExecCustomScan			= EXCHANGE_Execute;
	exchange_exec_methods.EndCustomScan				= EXCHANGE_End;
	exchange_exec_methods.ReScanCustomScan			= EXCHANGE_Rescan;
	exchange_exec_methods.MarkPosCustomScan			= NULL;
	exchange_exec_methods.RestrPosCustomScan		= NULL;
	exchange_exec_methods.EstimateDSMCustomScan  	= EXCHANGE_EstimateDSM;
	exchange_exec_methods.InitializeDSMCustomScan 	= EXCHANGE_InitializeDSM;
	exchange_exec_methods.InitializeWorkerCustomScan= EXCHANGE_InitializeWorker;
	exchange_exec_methods.ReInitializeDSMCustomScan = EXCHANGE_ReInitializeDSM;
	exchange_exec_methods.ShutdownCustomScan		= NULL;
	exchange_exec_methods.ExplainCustomScan			= EXCHANGE_Explain;

	DistExec_Init_methods();
}

Bitmapset *
accumulate_part_servers(RelOptInfo *rel)
{
	Bitmapset *servers = NULL;
	Oid serverid;
	int i;

	for (i = 0; i < rel->nparts; i++)
	{
		serverid = rel->part_rels[i]->serverid;
		if (OidIsValid(serverid) && !bms_is_member((int) serverid, servers))
			servers = bms_add_member(servers, (int) serverid);
		else if (!bms_is_member(0, servers))
			servers = bms_add_member(servers, 0);
	}
	return servers;
}

#include "optimizer/cost.h"

void
cost_exchange(PlannerInfo *root, RelOptInfo *rel, ExchangePath *expath)
{
	Path *subpath;
	int instances;

	/* Estimate baserel size as best we can with local statistics. */
	subpath = cstmSubPath1(expath);
	expath->cp.path.rows = subpath->rows;
	expath->cp.path.startup_cost = subpath->startup_cost;
	expath->cp.path.total_cost = subpath->total_cost;

	instances = (expath->dist) ? expath->dist->nparts : 0;
	switch (expath->mode)
	{
	case EXCH_GATHER:
		expath->cp.path.startup_cost += DEFAULT_EXCHANGE_STARTUP_COST;
		expath->cp.path.total_cost += cpu_tuple_cost * expath->cp.path.rows;
		break;

	case EXCH_STEALTH:
		expath->cp.path.startup_cost += 0.;
		expath->cp.path.total_cost += 0.;
		break;

	case EXCH_BROADCAST:
		expath->cp.path.startup_cost += DEFAULT_EXCHANGE_STARTUP_COST;
		expath->cp.path.total_cost += cpu_tuple_cost * expath->cp.path.rows;
		break;

	case EXCH_SHUFFLE:
	{
		double local_rows;
		double received_rows;
		double send_rows;
		Path *path = &expath->cp.path;

		path->startup_cost += DEFAULT_EXCHANGE_STARTUP_COST;

		/*
		 * We assume perfect balance of tuple distribution:
		 * If we have N instances, M tuples from subtree, than we send up by
		 * subtree M/N local tuples, send to network [M-M/N] tuples and same to
		 * receive.
		 */
		Assert(instances > 0);
		send_rows = path->rows - (path->rows/instances);
		received_rows = send_rows;
		local_rows = path->rows/instances;
		path->total_cost += (send_rows + local_rows) * cpu_tuple_cost;
		path->total_cost += (received_rows) * cpu_tuple_cost * 10.;
	}
		break;
	default:
		elog(FATAL, "Unknown EXCHANGE mode.");
	}
	expath->cp.path.total_cost += expath->cp.path.startup_cost;
}

/*
 * Create and Initialize plan structure of EXCHANGE-node. It will serialized
 * and deserialized at some instances and convert to an exchange state.
 */
static struct Plan *
ExchangeCreateCustomPlan(PlannerInfo *root,
					   RelOptInfo *rel,
					   struct CustomPath *best_path,
					   List *tlist,
					   List *clauses,
					   List *custom_plans)
{
	CustomScan *exchange;
	char *host;
	int port;
	char *streamName = palloc(DMQ_NAME_MAXLEN);
	ExchangePath *path = (ExchangePath *) best_path;
	EPPNode *private = (EPPNode *) newNode(sizeof(EPPNode), T_ExtensibleNode);

	exchange = make_exchange(custom_plans, tlist);
	private->node.extnodename = EXCHANGE_PRIVATE_NAME;

	/* Add stream name into private field*/
	host = GetMyServerName(&port);
	sprintf(streamName, "%s-%d-%d", host, port, exchange_counter++);
	exchange->custom_private = lappend(exchange->custom_private, makeString(streamName));

	/*
	 * No redistribution activities needed. Init distribution function from
	 * relation partitioning info.
	 * We do it for partitions and servers initialization.
	 */
	if (path->dist == NULL)
	{
		Assert(path->mode == EXCH_GATHER || path->mode == EXCH_STEALTH);
		path->dist = InitDistribution(rel);
	}
	Assert(path->dist->nparts >= 0);

	/*
	 * Compute and fill private data structure. It is used connection
	 * info and a tuple distribution rules. Now we define partitioning scheme,
	 * made by exchange and foreign servers are involved.
	 */
	switch (path->mode)
	{
	case EXCH_GATHER:
		create_gather_dfn(path->dist, private);
		break;
	case EXCH_STEALTH:
		create_stealth_dfn(root, path->dist, private);
		break;
	case EXCH_SHUFFLE:
		create_shuffle_dfn(root, path->dist, private);
		break;
	case EXCH_BROADCAST:
		create_broadcast_dfn(root, path->dist, private);
		break;
	default:
		Assert(0);
	}

	exchange->custom_exprs = private->exprs;
	exchange->custom_private = lappend(exchange->custom_private, private);
	exchange->custom_private = list_concat(exchange->custom_private, path->cp.custom_private);
	return &exchange->scan.plan;
}

/*
 * Create distribution data for EXCHANGE plan node. It is based on altrel
 * RelOptInfo structure of exchange path node.
 */
static void
create_gather_dfn(const Distribution dist, EPPNode *epp)
{
	char *hostname;
	int port;
	int partno;
	int serverid = -1;

	Assert(dist != NULL);

	/*
	 * Do not create partition scheme. In the GATHER mode all tuples will
	 * transfer to the coordinator node. None distribution computations needed.
	 */
	epp->mode = EXCH_GATHER;
	epp->natts = 0;
	epp->funcid = NULL;
	epp->exprs = NIL;

	epp->nnodes = dist->nparts;
	epp->nodes = (NodeName *) palloc(sizeof(NodeName) * epp->nnodes);

	partno = 0;
	while ((serverid = bms_next_member(dist->servers, serverid)) >= 0)
	{
		if (OidIsValid(serverid))
			/* Foreign relation */
			FSExtractServerName(serverid, &hostname, &port);
		else
			hostname = GetMyServerName(&port);

		createNodeName(epp->nodes[partno++], hostname, port);
	}
	Assert(partno == epp->nnodes);
}

/*
 * Do the distribution function of EXCHANGE node in accordance with current
 * RelOptInfo relation partitioning. In this case all tuples will go up.
 */
static void
create_stealth_dfn(const PlannerInfo *root, const Distribution dist, EPPNode *epp)
{
	int partno;
	char *hostname;
	int port;
	int serverid = -1;

	/*
	 * Do not create partition scheme. In the STEALTH mode all tuples will
	 * transfer up to next node. None distribution computations needed.
	 */
	epp->mode = EXCH_STEALTH;
	epp->natts = 0;
	epp->funcid = NULL;
	epp->exprs = NIL;

	/* nparts of altrel represents real or virtual partitions (after JOIN) */
	epp->nnodes = dist->nparts;
	epp->nodes = (NodeName *) palloc(sizeof(NodeName) * epp->nnodes);

	partno = 0;
	while ((serverid = bms_next_member(dist->servers, serverid)) >= 0)
	{
		if (OidIsValid(serverid))
			/* Foreign relation */
			FSExtractServerName(serverid, &hostname, &port);
		else
			hostname = GetMyServerName(&port);

		createNodeName(epp->nodes[partno++], hostname, port);
	}
	Assert(partno == epp->nnodes);
}

static void
create_shuffle_dfn(const PlannerInfo *root, const Distribution dist, EPPNode *epp)
{
	int partno;
	int attno;
	char *hostname;
	int port;
	int serverid = -1;

	Assert(dist != NULL);
	Assert(dist->nparts == bms_num_members(dist->servers));
	Assert(dist->part_scheme->strategy == PARTITION_STRATEGY_HASH);

	epp->mode = EXCH_SHUFFLE;
	epp->nnodes = dist->nparts;
	epp->nodes = (NodeName *) palloc(sizeof(NodeName) * epp->nnodes);

	partno = 0;
	while ((serverid = bms_next_member(dist->servers, serverid)) >= 0)
	{
		if (OidIsValid(serverid))
			/* Foreign relation */
			FSExtractServerName(serverid, &hostname, &port);
		else
			hostname = GetMyServerName(&port);

		createNodeName(epp->nodes[partno++], hostname, port);
	}
	Assert(partno == epp->nnodes);

	epp->natts = dist->part_scheme->partnatts;
	epp->funcid = (Oid *) palloc(sizeof(Oid) * epp->natts);
	for (attno = 0; attno < epp->natts; attno++)
	{
		Oid funcid;

		funcid = get_opfamily_proc(dist->part_scheme->partopfamily[attno],
								   dist->part_scheme->partopcintype[attno],
								   dist->part_scheme->partopcintype[attno],
								   HASHEXTENDED_PROC);
		epp->funcid[attno] = funcid;
		epp->exprs = lappend(epp->exprs, linitial(dist->partexprs[attno]));
	}
}

static void
create_broadcast_dfn(const PlannerInfo *root, const Distribution dist, EPPNode *epp)
{
	int partno;
	char *hostname;
	int port;
	int serverid = -1;

	/*
	 * Do not create partition scheme. In the STEALTH mode all tuples will
	 * transfer up to next node. None distribution computations needed.
	 */
	epp->mode = EXCH_BROADCAST;
	epp->natts = 0;
	epp->funcid = NULL;
	epp->exprs = NIL;

	/* nparts of altrel represents real or virtual partitions (after JOIN) */
	epp->nnodes = dist->nparts;
	epp->nodes = (NodeName *) palloc(sizeof(NodeName) * epp->nnodes);

	partno = 0;
	while ((serverid = bms_next_member(dist->servers, serverid)) >= 0)
	{
		if (OidIsValid(serverid))
			/* Foreign relation */
			FSExtractServerName(serverid, &hostname, &port);
		else
			hostname = GetMyServerName(&port);

		createNodeName(epp->nodes[partno++], hostname, port);
	}
	Assert(partno == epp->nnodes);
}

/*
 * Create EXCHANGE path.
 * custom_private != NIL - exchange node is a leaf and it is needed to create
 * subtree for scanning of base relations.
 */
ExchangePath *
create_exchange_path(PlannerInfo *root, RelOptInfo *rel, Path *children,
		ExchangeMode mode, Distribution dist)
{
	ExchangePath *epath = (ExchangePath *) newNode(sizeof(ExchangePath), T_CustomPath);
	CustomPath *path = &epath->cp;
	Path *pathnode = &path->path;

	pathnode->pathtype = T_CustomScan;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = NULL;
	pathnode->parent = rel;
	pathnode->parallel_aware = false; /* permanently */
	pathnode->parallel_safe = false; /* permanently */
	pathnode->parallel_workers = 0; /* permanently */
	pathnode->pathkeys = NIL;

	path->flags = 0;
	path->custom_paths = lappend(path->custom_paths, children);

	path->custom_private = NIL;
	path->methods = &exchange_path_methods;

	epath->dist = dist;
	epath->mode = mode;
	cost_exchange(root, rel, epath); /* Change at next step*/

	return epath;
}

/*
 * Compute foreign server ID and return its dmq representation.
 */
static DmqDestinationId
computeTupleDestination(TupleTableSlot *slot, ExchangeState *state)
{
	EState *estate = state->estate;
	int		greatest_modulus;
	uint64	rowHash;
	int16	part_index;
	Datum	values[PARTITION_MAX_KEYS];
	bool	isnull[PARTITION_MAX_KEYS];

	ListCell   *partexpr_item;
	int			i;
	ExprContext *econtext;
	int index;

	ExecMaterializeSlot(slot);
	econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = slot;

	partexpr_item = list_head(state->keystate);
	for (i = 0; i < state->partnatts; i++)
	{
		Datum	datum;
		bool	isNull;

		if (partexpr_item == NULL)
			elog(ERROR, "wrong number of partition key expressions");

		datum = ExecEvalExprSwitchContext((ExprState *) lfirst(partexpr_item),
										  GetPerTupleExprContext(estate),
										  &isNull);
		partexpr_item = lnext(partexpr_item);
		values[i] = datum;
		isnull[i] = isNull;
	}

	if (partexpr_item != NULL)
		elog(ERROR, "wrong number of partition key expressions. keystate list size: %d, partnatts: %d",
				list_length(state->keystate), state->partnatts);

	greatest_modulus = state->greatest_modulus;
	rowHash = compute_partition_hash_value(state->partnatts, state->partsupfunc,
			values, isnull);
	part_index = rowHash % greatest_modulus;
	index = state->indexes[part_index];

	if (index < 0)
		return -1;

	return state->dests->dests[index].dest_id;
}

/*
 * Make Exchange plan node from path, generated by create_exchange_path() routine
 */
CustomScan *
make_exchange(List *custom_plans, List *tlist)
{
	CustomScan	*node = makeNode(CustomScan);
	Plan		*plan = &node->scan.plan;
	List *child_tlist;

	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->targetlist = tlist;

	/* Setup methods and child plan */
	node->methods = &exchange_plan_methods;
	node->scan.scanrelid = 0;
	node->custom_plans = custom_plans;
	child_tlist = ((Plan *)linitial(node->custom_plans))->targetlist;
	node->custom_scan_tlist = child_tlist;
	node->custom_exprs = NIL;
	node->custom_private = NIL;

	return node;
}

/*
 * Create state of exchange node.
 */
static Node *
EXCHANGE_Create_state(CustomScan *node)
{
	ExchangeState	*state;
	ListCell		*lc;
	List			*private_data;
	int16			partno;
	EPPNode			*epp;

	state = (ExchangeState *) palloc0(sizeof(ExchangeState));
	NodeSetTag(state, T_CustomScanState);

	state->css.flags = node->flags;
	state->css.methods = &exchange_exec_methods;
	state->estate = NULL;

	private_data = node->custom_private;
	lc = list_head(private_data);

	/* Name of DMQ-connection stream */
	strcpy(state->stream, strVal(lfirst(lc)));

	/* Load partition info */
	Assert(list_length(private_data) <= 3);
	epp = (EPPNode *) lsecond(private_data);
	state->mode = epp->mode;
	if (state->mode == EXCH_SHUFFLE)
	{
		state->partnatts = epp->natts;

		foreach(lc, node->custom_exprs)
			state->partexprs = lappend(state->partexprs, copyObject(lfirst(lc)));

		state->partsupfunc = (FmgrInfo *) palloc(sizeof(FmgrInfo) * state->partnatts);
		state->greatest_modulus = epp->nnodes;
		for (partno = 0; partno < state->partnatts; partno++)
			fmgr_info(epp->funcid[partno], &state->partsupfunc[partno]);
		state->keystate = NIL;
	}
	Assert(epp->nnodes > 0);
	state->nodes = epp->nodes;
	state->nnodes = epp->nnodes;
	state->indexes = (int *) palloc(sizeof(int) * state->nnodes);

	if (lnext(lnext(list_head(private_data))) != NULL)
		state->indexinfo = (IndexOptInfo *) lthird(private_data);
	else
		state->indexinfo = NULL;

	return (Node *) state;
}

static void
EXCHANGE_Begin(CustomScanState *node, EState *estate, int eflags)
{
	CustomScan	*cscan = (CustomScan *) node->ss.ps.plan;
	Plan		*scan_plan;
	PlanState	*planState;
	ExchangeState *state = (ExchangeState *) node;
	TupleDesc	scan_tupdesc;

	Stream_subscribe(state->stream);

	scan_plan = linitial(cscan->custom_plans);
	planState = (PlanState *) ExecInitNode(scan_plan, estate, eflags);
	node->custom_ps = lappend(node->custom_ps, planState);

	state->init = false;
	state->ltuples = 0;
	state->rtuples = 0;
	state->stuples = 0;

	/* Need to access to QueryEnv which will initialized later. */
	state->estate = estate;

	scan_tupdesc = ExecTypeFromTL(scan_plan->targetlist, false);
	ExecInitScanTupleSlot(estate, &node->ss, scan_tupdesc);

	/* First time through, set up expression evaluation state */
	if (state->mode == EXCH_SHUFFLE)
		state->keystate = ExecPrepareExprList(state->partexprs, estate);
}

static void
init_state_ifany(ExchangeState *state)
{
	EphemeralNamedRelation enr;

	if (state->init)
		return;

	/* In the STEALTH mode we do not needed to init any communications. */
	if (state->mode == EXCH_STEALTH)
		state->activeRemotes = 0;
	else
	{
		enr = get_ENR(state->estate->es_queryEnv, destsName);
		Assert(enr != NULL && enr->reldata != NULL);
		state->dests = (DMQDestCont *) enr->reldata;
		state->activeRemotes = state->dests->nservers;
	}

	state->hasLocal = true;
	state->init = true;
}

#include "postmaster/postmaster.h"
static TupleTableSlot *
EXCHANGE_Execute(CustomScanState *node)
{
	ScanState	*subPlanState = linitial(node->custom_ps);
	ExchangeState *state = (ExchangeState *) node;
	bool readRemote = false;

	init_state_ifany(state);

	for (;;)
	{
		TupleTableSlot *slot = NULL;
		DmqDestinationId dest;

		readRemote = !readRemote;

		if ((state->activeRemotes > 0) /*&& readRemote */)
		{
			int status;
			status = RecvTuple(state->stream, node->ss.ss_ScanTupleSlot);

			switch (status)
			{
			case -1:
				/* No tuples currently */
				break;
			case 0:
				Assert(!TupIsNull(node->ss.ss_ScanTupleSlot));
				state->rtuples++;
				return node->ss.ss_ScanTupleSlot;
			case 1:
				state->activeRemotes--;
				elog(LOG, "[%s %d] GOT NULL. activeRemotes: %d, lt=%d, rt=%d hasLocal=%hhu st=%d",
						state->stream, state->mode, state->activeRemotes,
						state->ltuples,
						state->rtuples, state->hasLocal, state->stuples);
				break;
			default:
				/* Any system message */
				break;
			}
			slot = NULL;
		}

		if ((state->hasLocal) && (!readRemote))
		{
			slot = ExecProcNode(&subPlanState->ps);

			if (TupIsNull(slot))
			{
				elog(LOG, "[%s] FINISH Local store: l=%d, r=%d s=%d, activeRemotes=%d",
						state->stream, state->ltuples,
						state->rtuples, state->stuples, state->activeRemotes);
				if (state->mode != EXCH_STEALTH)
				{
					int i;

					for (i = 0; i < state->dests->nservers; i++)
						SendByteMessage(state->dests->dests[i].dest_id,
										state->stream, END_OF_TUPLES, false);
				}
				state->hasLocal = false;
				continue;
			}
			else
				state->ltuples++;
		}

		if ((state->activeRemotes == 0) && (!state->hasLocal))
		{
			elog(LOG, "[%s] Exchange returns NULL. Tuples: local=%d, remote=%d, send=%d",
					state->stream, state->ltuples, state->rtuples, state->stuples);
			return NULL;
		}

		if (TupIsNull(slot))
			continue;

		if (state->mode == EXCH_GATHER)
		{
			int cnum = state->dests->coordinator_num;
			if (cnum < 0)
				dest = -1;
			else
				dest = state->dests->dests[cnum].dest_id;
		}
		else if (state->mode == EXCH_STEALTH)
			dest = -1;
		else if (state->mode == EXCH_SHUFFLE)
			dest = computeTupleDestination(slot, state);
		else if (state->mode == EXCH_BROADCAST)
		{
			int i;
			state->stuples++;

			/* Send tuple to each server that involved. Himself too. */
			for (i = 0; i < state->dests->nservers; i++)
				SendTuple(state->dests->dests[i].dest_id, state->stream, slot);

			return slot;
		}
		else
			Assert(0);

		if (dest < 0)
			return slot;
		else
		{
			state->stuples++;
			SendTuple(dest, state->stream, slot);
		}
	}
	return NULL;
}

static void
EXCHANGE_End(CustomScanState *node)
{
	ExchangeState *state = (ExchangeState *) node;

	Assert(list_length(node->custom_ps) == 1);
	ExecEndNode(linitial(node->custom_ps));

	if (state->mode != EXCH_STEALTH)
		Stream_unsubscribe(state->stream);
}

static void
EXCHANGE_Rescan(CustomScanState *node)
{
	ExchangeState *state = (ExchangeState *) node;
	PlanState *subPlan = (PlanState *) linitial(node->custom_ps);

	init_state_ifany(state);

	if (subPlan->chgParam == NULL)
		ExecReScan(subPlan);
	if (state->mode != EXCH_STEALTH)
		state->activeRemotes = state->dests->nservers;
	state->ltuples = 0;
	state->rtuples = 0;
	state->hasLocal = true;
}

static void
EXCHANGE_ReInitializeDSM(CustomScanState *node, ParallelContext *pcxt,
		  	  	  	  	 void *coordinate)
{
	/* ToDo */
	Assert(0);
}

static void
EXCHANGE_Explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	StringInfoData		str;
	ExchangeState *state = (ExchangeState *) node;
	const char *mode;

	initStringInfo(&str);

	switch (state->mode)
	{
	case EXCH_GATHER:
		mode = "GATHER";
		break;
	case EXCH_STEALTH:
		mode = "STEALTH";
		break;
	case EXCH_SHUFFLE:
		mode = "SHUFFLE";
		break;
	case EXCH_BROADCAST:
		mode = "BROADCAST";
		break;
	default:
		Assert(0);
	}

	appendStringInfo(&str, "mode: %s, stream: %s. ", mode, state->stream);

	ExplainPropertyText("Exchange", str.data, es);
}

static Size
EXCHANGE_EstimateDSM(CustomScanState *node, ParallelContext *pcxt)
{
	return 0;
}

static void
EXCHANGE_InitializeDSM(CustomScanState *node, ParallelContext *pcxt,
					   void *coordinate)
{
	/*
	 * coordinate - pointer to shared memory segment.
	 * node->pscan_len - size of the coordinate - is defined by
	 * EstimateDSMCustomScan() function.
	 */
}

static void
EXCHANGE_InitializeWorker(CustomScanState *node,
						  shm_toc *toc,
						  void *coordinate)
{

}

void
createNodeName(char *nodeName, const char *hostname, int port)
{
	sprintf(nodeName, "%s-%d", hostname, port);
}
