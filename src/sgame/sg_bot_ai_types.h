#ifndef BOT_AI_TYPES_H_
#define BOT_AI_TYPES_H_

#include "sg_local.h"

enum AINodeStatus_t
{
	STATUS_FAILURE = 0,
	STATUS_SUCCESS,
	STATUS_RUNNING
};

enum AINode_t
{
	SPAWN_NODE,
	SELECTOR_NODE,
	ACTION_NODE,
	CONDITION_NODE,
	BEHAVIOR_NODE,
	DECORATOR_NODE,
	LUA_BEHAVIOR_NODE,
	LUA_ACTION_NODE,
};

struct AIGenericNode_t;
using AINodeRunner = AINodeStatus_t (*)( gentity_t *self, AIGenericNode_t *node );

struct AIGenericNode_t
{
	AINode_t type;
	AINodeRunner run;
};

#define MAX_NODE_LIST 20
struct AINodeList_t
{
	AINode_t type;
	AINodeRunner run;
	AIGenericNode_t *list[ MAX_NODE_LIST ];
	int numNodes;
};

struct AIBehaviorTree_t
{
	AINode_t     type;
	AINodeRunner run;
	char name[ MAX_QPATH ];
	AIGenericNode_t *root;
	AIGenericNode_t *classSelectionTree;
};

enum AIOpType_t
{
	OP_NOT,
	OP_LESSTHAN,
	OP_LESSTHANEQUAL,
	OP_GREATERTHAN,
	OP_GREATERTHANEQUAL,
	OP_EQUAL,
	OP_NEQUAL,
	OP_AND,
	OP_OR,
	OP_NONE
};

enum AIExpType_t
{
	EX_OP,
	EX_VALUE,
	EX_FUNC
};

enum AIValueType_t
{
	VALUE_FLOAT,
	VALUE_INT,
	VALUE_STRING
};

struct AIValue_t
{
	AIExpType_t             expType;
	AIValueType_t           valType;

	union
	{
		float floatValue;
		int   intValue;
		char  *stringValue;
	} l;
};

using AIFunc = AIValue_t (*)( gentity_t *self, const AIValue_t *params );

struct AIValueFunc_t
{
	AIExpType_t   expType;
	AIFunc        func;
	AIValue_t     *params;
	int           nparams;
};

struct AIOp_t
{
	AIExpType_t expType;
	AIOpType_t  opType;
};

struct AIBinaryOp_t
{
	AIExpType_t expType;
	AIOpType_t  opType;
	AIExpType_t *exp1;
	AIExpType_t *exp2;
};

struct AIUnaryOp_t
{
	AIExpType_t expType;
	AIOpType_t  opType;
	AIExpType_t *exp;
};

struct AISpawnNode_t
{
	AINode_t type;
	AINodeRunner run;
	int selection;
};

struct AIConditionNode_t
{
	AINode_t        type;
	AINodeRunner    run;
	AIGenericNode_t *child;
	AIExpType_t     *exp;
};

struct AIDecoratorNode_t
{
	AINode_t        type;
	AINodeRunner    run;
	AIGenericNode_t *child;
	AIValue_t       *params;
	int             nparams;
	int             data[ MAX_CLIENTS ];
};

struct AIActionNode_t
{
	AINode_t     type;
	AINodeRunner run;
	AIValue_t    *params;
	int          nparams;
	int lineNum;
	const char *name;
};

#endif  // BOT_AI_TYPES_H_
