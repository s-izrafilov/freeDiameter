/*********************************************************************************************************
* Software License Agreement (BSD License)                                                               *
* Author: Sebastien Decugis <sdecugis@nict.go.jp>							 *
*													 *
* Copyright (c) 2011, WIDE Project and NICT								 *
* All rights reserved.											 *
* 													 *
* Redistribution and use of this software in source and binary forms, with or without modification, are  *
* permitted provided that the following conditions are met:						 *
* 													 *
* * Redistributions of source code must retain the above 						 *
*   copyright notice, this list of conditions and the 							 *
*   following disclaimer.										 *
*    													 *
* * Redistributions in binary form must reproduce the above 						 *
*   copyright notice, this list of conditions and the 							 *
*   following disclaimer in the documentation and/or other						 *
*   materials provided with the distribution.								 *
* 													 *
* * Neither the name of the WIDE Project or NICT nor the 						 *
*   names of its contributors may be used to endorse or 						 *
*   promote products derived from this software without 						 *
*   specific prior written permission of WIDE Project and 						 *
*   NICT.												 *
* 													 *
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED *
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A *
* PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR *
* ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 	 *
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 	 *
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR *
* TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF   *
* ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.								 *
*********************************************************************************************************/

/* Messages module.
 * 
 * This module allows to manipulate the msg and avp structures that represents a Diameter message in memory.
 */

#include "fdproto-internal.h"

#include <sys/param.h>

/* Type of object */
enum msg_objtype {
	MSG_MSG = 1, 
	MSG_AVP
};

/* Chaining of elements as a free hierarchy */
struct msg_avp_chain {
	struct fd_list		chaining;	/* Chaining information at this level. */
	struct fd_list		children;	/* sentinel for the children of this object */
	enum msg_objtype 	type;		/* Type of this object, _MSG_MSG or _MSG_AVP */
};

/* Return the chain information from an AVP or MSG. Since it's the first field, we just cast */
#define _C(_x) ((struct msg_avp_chain *)(_x))

/* Some details about chaining:
 *
 *  A message is made of a header ( msg ) and 0 or more AVPs ( avp ).
 * The structure is a kind of tree, where some AVPs (grouped AVPs) can contain other AVPs.
 * Exemple:
 * msg
 *  |-avp
 *  |-gavp
 *  |   |-avp
 *  |   |-avp
 *  |   \-avp
 *  |-avp
 *  \-avp
 *
 * Each item (msg or avp) structure begins with a msg_avp_chain structure.
 * The element at the top of the hierarchy (msg in our example) has all the fields of its "chaining" equal to the same value.
 *
 * All elements at the same level are linked by their "chaining" list.
 * The "children" list is the sentinel for the lists of children of this element.
 */

/* The following definitions are used to recognize objects in memory. */
#define MSG_MSG_EYEC	(0x11355463)
#define MSG_AVP_EYEC	(0x11355467)

/* The following structure represents an AVP instance. */
struct avp {
	struct msg_avp_chain	 avp_chain;		/* Chaining information of this AVP */
	int			 avp_eyec;		/* Must be equal to MSG_AVP_EYEC */
	struct dict_object	*avp_model;		/* If not NULL, pointer to the dictionary object of this avp */
	struct avp_hdr		 avp_public;		/* AVP data that can be managed by other modules */
	
	uint8_t			*avp_source;		/* If the message was parsed from a buffer, pointer to the AVP data start in the buffer. */
	uint8_t			*avp_rawdata;		/* when the data can not be interpreted, the raw data is copied here. The header is not part of it. */
	size_t			 avp_rawlen;		/* The length of the raw buffer. */
	union avp_value		 avp_storage;		/* To avoid many alloc/free, store the integer values here and set avp_public.avp_data to &storage */
	int			 avp_mustfreeos;	/* 1 if an octetstring is malloc'd in avp_storage and must be freed. */
};

/* Macro to compute the AVP header size */
#define AVPHDRSZ_NOVEND	8
#define AVPHDRSZ_VENDOR	12
#define GETAVPHDRSZ( _flag ) ((_flag & AVP_FLAG_VENDOR) ? AVPHDRSZ_VENDOR : AVPHDRSZ_NOVEND)

/* Macro to cast a msg_avp_t */
#define _A(_x) ((struct avp *)(_x))
/* Check the type and eyecatcher */
#define CHECK_AVP(_x) ((_C(_x)->type == MSG_AVP) && (_A(_x)->avp_eyec == MSG_AVP_EYEC))

/* The following structure represents an instance of a message (command and children AVPs). */
struct msg {
	struct msg_avp_chain	 msg_chain;		/* List of the AVPs in the message */
	int			 msg_eyec;		/* Must be equal to MSG_MSG_EYEC */
	struct dict_object	*msg_model;		/* If not NULL, pointer to the dictionary object of this message */
	struct msg_hdr		 msg_public;		/* Message data that can be managed by extensions. */
	
	uint8_t			*msg_rawbuffer;		/* data buffer that was received, saved during fd_msg_parse_buffer and freed in fd_msg_parse_dict */
	int			 msg_routable;		/* Is this a routable message? (0: undef, 1: routable, 2: non routable) */
	struct msg		*msg_query;		/* the associated query if the message is a received answer */
	int			 msg_associated;	/* and the counter part information in the query, to avoid double free */
	struct rt_data		*msg_rtdata;		/* Routing list for the query */
	struct session		*msg_sess;		/* Cached message session if any */
	struct {
			void (*fct)(void *, struct msg **);
			void * data;
			struct timespec timeout;
		}		 msg_cb;		/* Callback to be called when an answer is received, if not NULL */
	char *			 msg_src_id;		/* Diameter Id of the peer this message was received from. This string is malloc'd and must be freed */
};

/* Macro to compute the message header size */
#define GETMSGHDRSZ() 	20

/* Macro to cast a msg_avp_t */
#define _M(_x) ((struct msg *)(_x))
/* Check the type and eyecatcher */
#define CHECK_MSG(_x) ((_C(_x)->type == MSG_MSG) && (_M(_x)->msg_eyec == MSG_MSG_EYEC))

#define VALIDATE_OBJ(_x) ( (CHECK_MSG(_x)) || (CHECK_AVP(_x)) )


/* Macro to validate a MSGFL_ value */
#define CHECK_AVPFL(_fl) ( ((_fl) & (- (AVPFL_MAX << 1) )) == 0 )
#define CHECK_MSGFL(_fl) ( ((_fl) & (- (MSGFL_MAX << 1) )) == 0 )


/* initial sizes of AVP from their types, in bytes. */
static int avp_value_sizes[] = { 
	 0,	/* AVP_TYPE_GROUPED: size is dynamic */
	 0,	/* AVP_TYPE_OCTETSTRING: size is dynamic */
	 4,	/* AVP_TYPE_INTEGER32: size is 32 bits */
	 8,	/* AVP_TYPE_INTEGER64: size is 64 bits */
	 4,	/* AVP_TYPE_UNSIGNED32: size is 32 bits */
	 8,	/* AVP_TYPE_UNSIGNED64: size is 64 bits */
	 4,	/* AVP_TYPE_FLOAT32: size is 32 bits */
	 8	/* AVP_TYPE_FLOAT64: size is 64 bits */
};
#define CHECK_BASETYPE( _type ) ( ((_type) <= AVP_TYPE_MAX) && ((_type) >= 0) )
#define GETINITIALSIZE( _type, _vend ) (avp_value_sizes[ CHECK_BASETYPE(_type) ? (_type) : 0] + GETAVPHDRSZ(_vend))

/* Forward declaration */
static int parsedict_do_msg(struct dictionary * dict, struct msg * msg, int only_hdr, struct fd_pei *error_info);

/***************************************************************************************************************/
/* Creating objects */

/* Initialize a msg_avp_chain structure */
static void init_chain(struct msg_avp_chain * chain, int type)
{
	fd_list_init( &chain->chaining, (void *)chain);
	fd_list_init( &chain->children, (void *)chain);
	chain->type = type;
}

/* Initialize a new AVP object */
static void init_avp ( struct avp * avp )
{
	TRACE_ENTRY("%p", avp);
	
	memset(avp, 0, sizeof(struct avp));
	init_chain( &avp->avp_chain, MSG_AVP);
	avp->avp_eyec = MSG_AVP_EYEC;
}
	
/* Initialize a new MSG object */
static void init_msg ( struct msg * msg )
{
	TRACE_ENTRY("%p", msg);
	
	memset(msg, 0, sizeof(struct msg));
	init_chain( &msg->msg_chain, MSG_MSG);
	msg->msg_eyec = MSG_MSG_EYEC;
}


/* Create a new AVP instance */
int fd_msg_avp_new ( struct dict_object * model, int flags, struct avp ** avp )
{
	struct avp *new = NULL;
	
	TRACE_ENTRY("%p %x %p", model, flags, avp);
	
	/* Check the parameters */
	CHECK_PARAMS(  avp && CHECK_AVPFL(flags)  );
	
	if (model) {
		enum dict_object_type 	 dicttype;
		CHECK_PARAMS( (fd_dict_gettype(model, &dicttype) == 0) && (dicttype == DICT_AVP) );
	}
	
	/* Create a new object */
	CHECK_MALLOC(  new = malloc (sizeof(struct avp))  );
	
	/* Initialize the fields */
	init_avp(new);
	
	if (model) {
		struct dict_avp_data dictdata;
		
		CHECK_FCT(  fd_dict_getval(model, &dictdata)  );
	
		new->avp_model = model;
		new->avp_public.avp_code    = dictdata.avp_code;
		new->avp_public.avp_flags   = dictdata.avp_flag_val;
		new->avp_public.avp_len = GETINITIALSIZE(dictdata.avp_basetype, dictdata.avp_flag_val );
		new->avp_public.avp_vendor  = dictdata.avp_vendor;
	}
	
	if (flags & AVPFL_SET_BLANK_VALUE) {
		new->avp_public.avp_value = &new->avp_storage;
	}
	
	/* The new object is ready, return */
	*avp = new;
	return 0;
}

/* Create a new message instance */
int fd_msg_new ( struct dict_object * model, int flags, struct msg ** msg )
{
	struct msg * new = NULL;
	
	TRACE_ENTRY("%p %x %p", model, flags, msg);
	
	/* Check the parameters */
	CHECK_PARAMS(  msg && CHECK_MSGFL(flags)  );
	
	if (model) {
		enum dict_object_type 	 dicttype;
		CHECK_PARAMS( (fd_dict_gettype(model, &dicttype) == 0) && (dicttype == DICT_COMMAND) );
	}
	
	/* Create a new object */
	CHECK_MALLOC(  new = malloc (sizeof(struct msg))  );
	
	/* Initialize the fields */
	init_msg(new);
	new->msg_public.msg_version	= DIAMETER_VERSION;
	new->msg_public.msg_length	= GETMSGHDRSZ(); /* This will be updated later */

	if (model) {
		struct dictionary 	*dict;
		struct dict_cmd_data     dictdata;
		struct dict_object     	*dictappl;
		
		CHECK_FCT( fd_dict_getdict(model, &dict) );
		CHECK_FCT( fd_dict_getval(model, &dictdata)  );
		
		new->msg_model = model;
		new->msg_public.msg_flags	= dictdata.cmd_flag_val;
		new->msg_public.msg_code	= dictdata.cmd_code;

		/* Initialize application from the parent, if any */
		CHECK_FCT(  fd_dict_search( dict, DICT_APPLICATION, APPLICATION_OF_COMMAND, model, &dictappl, 0)  );
		if (dictappl != NULL) {
			struct dict_application_data appdata;
			CHECK_FCT(  fd_dict_getval(dictappl, &appdata)  );
			new->msg_public.msg_appl = appdata.application_id;
		}
	}
	
	if (flags & MSGFL_ALLOC_ETEID) {
		new->msg_public.msg_eteid = fd_msg_eteid_get();
	}
	
	/* The new object is ready, return */
	*msg = new;
	return 0;
}	

/* Create answer from a request */
int fd_msg_new_answer_from_req ( struct dictionary * dict, struct msg ** msg, int flags )
{
	struct dict_object * model = NULL;
	struct msg *qry, *ans;
	struct session * sess = NULL;
	
	TRACE_ENTRY("%p %x", msg, flags);
	
	/* Check the parameters */
	CHECK_PARAMS(  msg );
	qry = *msg;
	CHECK_PARAMS( CHECK_MSG(qry) && (qry->msg_public.msg_flags & CMD_FLAG_REQUEST) );
	
	if (! (flags & MSGFL_ANSW_NOSID)) {
		/* Get the session of the message */
		CHECK_FCT_DO( fd_msg_sess_get(dict, qry, &sess, NULL), /* ignore an error */ );
	}
	
	/* Find the model for the answer */
	if (flags & MSGFL_ANSW_ERROR) {
		/* The model is the generic error format */
		CHECK_FCT( fd_dict_get_error_cmd(dict, &model) );
	} else {
		/* The model is the answer corresponding to the query. It supposes that these are defined in the dictionary */
		CHECK_FCT_DO(  parsedict_do_msg( dict, qry, 1, NULL), /* continue */  );
		if (qry->msg_model) {
			CHECK_FCT(  fd_dict_search ( dict, DICT_COMMAND, CMD_ANSWER, qry->msg_model, &model, EINVAL )  );
		}
	}
	
	/* Create the answer */
	CHECK_FCT(  fd_msg_new( model, flags, &ans )  );
	
	/* Set informations in the answer as in the query */
	ans->msg_public.msg_code = qry->msg_public.msg_code; /* useful for MSGFL_ANSW_ERROR */
	ans->msg_public.msg_appl = qry->msg_public.msg_appl;
	ans->msg_public.msg_eteid = qry->msg_public.msg_eteid;
	ans->msg_public.msg_hbhid = qry->msg_public.msg_hbhid;
	
	/* Add the Session-Id AVP if session is known */
	if (sess && dict) {
		struct dict_object * sess_id_avp;
		char * sid;
		struct avp * avp;
		union avp_value val;
		
		CHECK_FCT( fd_dict_search( dict, DICT_AVP, AVP_BY_NAME, "Session-Id", &sess_id_avp, ENOENT) );
		CHECK_FCT( fd_sess_getsid ( sess, &sid ) );
		CHECK_FCT( fd_msg_avp_new ( sess_id_avp, 0, &avp ) );
		val.os.data = (unsigned char *)sid;
		val.os.len  = strlen(sid);
		CHECK_FCT( fd_msg_avp_setvalue( avp, &val ) );
		CHECK_FCT( fd_msg_avp_add( ans, MSG_BRW_FIRST_CHILD, avp ) );
		ans->msg_sess = sess;
		CHECK_FCT( fd_sess_ref_msg(sess) );
	}
	
	/* associate with query */
	ans->msg_query = qry;
	qry->msg_associated = 1;
	
	/* Done */
	*msg = ans;
	return 0;
}

/***************************************************************************************************************/

/* Explore a message */
int fd_msg_browse_internal ( msg_or_avp * reference, enum msg_brw_dir dir, msg_or_avp ** found, int * depth )
{
	struct msg_avp_chain *result = NULL;
	int diff = 0;
	struct fd_list *li = NULL;
	
	TRACE_ENTRY("%p %d %p %p", reference, dir, found, depth);
	
	/* Initialize the "found" result if any */
	if (found)
		*found = NULL;
	
	/* Check the parameters */
	CHECK_PARAMS(  VALIDATE_OBJ(reference)  );
	
	TRACE_DEBUG(FCTS, "chaining(%p): nxt:%p prv:%p hea:%p top:%p", 
			&_C(reference)->chaining,
			_C(reference)->chaining.next,
			_C(reference)->chaining.prev,
			_C(reference)->chaining.head,
			_C(reference)->chaining.o);
	TRACE_DEBUG(FCTS, "children(%p): nxt:%p prv:%p hea:%p top:%p", 
			&_C(reference)->children,
			_C(reference)->children.next,
			_C(reference)->children.prev,
			_C(reference)->children.head,
			_C(reference)->children.o);

	/* Now search */
	switch (dir) {
		case MSG_BRW_NEXT:
			/* Check the reference is an AVP */
			CHECK_PARAMS(  _C(reference)->type == MSG_AVP  );

			li = &_C(reference)->chaining;
			
			/* Check if the next element is not the sentinel ( ==> the parent) */
			if (li->next != li->head)
				result = _C(li->next->o);
			break;

		case MSG_BRW_PREV:
			/* Check the reference is an AVP */
			CHECK_PARAMS(  _C(reference)->type == MSG_AVP  );

			li = &_C(reference)->chaining;
			
			/* Check if the prev element is not the sentinel ( ==> the parent) */
			if (li->prev != li->head)
				result = _C(li->prev->o);
			break;

		case MSG_BRW_FIRST_CHILD:
			li = &_C(reference)->children;
			if (! FD_IS_LIST_EMPTY(li)) {
				result = _C(li->next->o);
				diff = 1;
			}
			break;

		case MSG_BRW_LAST_CHILD:
			li = &_C(reference)->children;
			if (! FD_IS_LIST_EMPTY(li)) {
				result = _C(li->prev->o);
				diff = 1;
			}
			break;

		case MSG_BRW_PARENT:
			/* If the object is not chained, it has no parent */
			li = &_C(reference)->chaining;
			if (li != li->head) {
				/* The sentinel is the parent's children list */
				result = _C(li->head->o);
				diff = -1;
			}
			break;

		case MSG_BRW_WALK:
			/* First, try to find a child */
			li = &_C(reference)->children;
			if ( ! FD_IS_LIST_EMPTY(li) ) {
				result = _C(li->next->o);
				diff = 1;
				break;
			}
			
			/* Then try to find a "next" at this level or one of the parent's */
			li = &_C(reference)->chaining;
			do {
				/* If this element has a "next" element, return it */
				if (li->next != li->head) {
					result = _C(li->next->o);
					break;
				}
				/* otherwise, check if we have a parent */
				if (li == li->head) {
					/* no parent */
					break;
				}
				/* Go to the parent's chaining information and loop */
				diff -= 1;
				li = &_C(li->head->o)->chaining;
			} while (1); 
			break;
			
		default:
			/* Other directions are invalid */
			CHECK_PARAMS( dir = 0 );
	}
	
	/* Save the found object, if any */
	if (found && result)
		*found = (void *)result;
	
	/* Modify the depth according to the walk direction */
	if (depth && diff)
		(*depth) += diff;
	
	/* Return ENOENT if found was NULL */
	if ((!found) && (!result))
		return ENOENT;
	else
		return 0;
}

/* Add an AVP into a tree */
int fd_msg_avp_add ( msg_or_avp * reference, enum msg_brw_dir dir, struct avp *avp)
{
	TRACE_ENTRY("%p %d %p", reference, dir, avp);
	
	/* Check the parameters */
	CHECK_PARAMS(  VALIDATE_OBJ(reference)  &&  CHECK_AVP(avp)  &&  FD_IS_LIST_EMPTY(&avp->avp_chain.chaining)  );
	
	/* Now insert */
	switch (dir) {
		case MSG_BRW_NEXT:
			/* Check the reference is an AVP -- we do not chain AVPs at same level as msgs. */
			CHECK_PARAMS(  _C(reference)->type == MSG_AVP  );
			
			/* Insert the new avp after the reference */
			fd_list_insert_after( &_A(reference)->avp_chain.chaining, &avp->avp_chain.chaining );
			break;

		case MSG_BRW_PREV:
			/* Check the reference is an AVP */
			CHECK_PARAMS(  _C(reference)->type == MSG_AVP  );
			
			/* Insert the new avp before the reference */
			fd_list_insert_before( &_A(reference)->avp_chain.chaining, &avp->avp_chain.chaining );
			break;

		case MSG_BRW_FIRST_CHILD:
			/* Insert the new avp after the children sentinel */
			fd_list_insert_after( &_C(reference)->children, &avp->avp_chain.chaining );
			break;

		case MSG_BRW_LAST_CHILD:
			/* Insert the new avp before the children sentinel */
			fd_list_insert_before( &_C(reference)->children, &avp->avp_chain.chaining );
			break;

		default:
			/* Other directions are invalid */
			CHECK_PARAMS( dir = 0 );
	}
			
	return 0;
}

/* Search a given AVP model in a message */
int fd_msg_search_avp ( struct msg * msg, struct dict_object * what, struct avp ** avp )
{
	struct avp * nextavp;
	struct dict_avp_data 	dictdata;
	enum dict_object_type 	dicttype;
	
	TRACE_ENTRY("%p %p %p", msg, what, avp);
	
	CHECK_PARAMS( CHECK_MSG(msg) && what );
	
	CHECK_PARAMS( (fd_dict_gettype(what, &dicttype) == 0) && (dicttype == DICT_AVP) );
	CHECK_FCT(  fd_dict_getval(what, &dictdata)  );
	
	/* Loop on all top AVPs */
	CHECK_FCT(  fd_msg_browse(msg, MSG_BRW_FIRST_CHILD, (void *)&nextavp, NULL)  );
	while (nextavp) {
		
		if ( (nextavp->avp_public.avp_code   == dictdata.avp_code)
		  && (nextavp->avp_public.avp_vendor == dictdata.avp_vendor) ) /* always 0 if no V flag */
			break;
		
		/* Otherwise move to next AVP in the message */
		CHECK_FCT( fd_msg_browse(nextavp, MSG_BRW_NEXT, (void *)&nextavp, NULL) );
	}
	
	if (avp)
		*avp = nextavp;
	
	if (avp && nextavp) {
		struct dictionary * dict;
		CHECK_FCT( fd_dict_getdict( what, &dict) );
		CHECK_FCT_DO( fd_msg_parse_dict( nextavp, dict, NULL ), /* nothing */ );
	}
	
	if (avp || nextavp)
		return 0;
	else
		return ENOENT;
}


/***************************************************************************************************************/
/* Deleting objects */

/* Destroy and free an AVP or message */
static int destroy_obj (struct msg_avp_chain * obj )
{
	TRACE_ENTRY("%p", obj);
	
	/* Check the parameter is a valid object */
	CHECK_PARAMS(  VALIDATE_OBJ(obj) && FD_IS_LIST_EMPTY( &obj->children ) );

	/* Unlink this object if needed */
	fd_list_unlink( &obj->chaining );
	
	/* Free the octetstring if needed */
	if ((obj->type == MSG_AVP) && (_A(obj)->avp_mustfreeos == 1)) {
		free(_A(obj)->avp_storage.os.data);
	}
	/* Free the rawdata if needed */
	if ((obj->type == MSG_AVP) && (_A(obj)->avp_rawdata != NULL)) {
		free(_A(obj)->avp_rawdata);
	}
	if ((obj->type == MSG_MSG) && (_M(obj)->msg_rawbuffer != NULL)) {
		free(_M(obj)->msg_rawbuffer);
	}
	
	if ((obj->type == MSG_MSG) && (_M(obj)->msg_src_id != NULL)) {
		free(_M(obj)->msg_src_id);
	}
	
	if ((obj->type == MSG_MSG) && (_M(obj)->msg_rtdata != NULL)) {
		fd_rtd_free(&_M(obj)->msg_rtdata);
	}
	
	if ((obj->type == MSG_MSG) && (_M(obj)->msg_sess != NULL)) {
		CHECK_FCT_DO( fd_sess_reclaim_msg ( &_M(obj)->msg_sess ), /* continue */);
	}
	
	/* free the object */
	free(obj);
	
	return 0;
}

/* Destroy an object and all its children */
static void destroy_tree(struct msg_avp_chain * obj)
{
	struct fd_list *rem;
	
	TRACE_ENTRY("%p", obj);
	
	/* Destroy any subtree */
	while ( (rem = obj->children.next) != &obj->children)
		destroy_tree(_C(rem->o));
	
	/* Then unlink and destroy the object */
	CHECK_FCT_DO(  destroy_obj(obj),  /* nothing */  );
}

/* Free an object and its tree */
int fd_msg_free ( msg_or_avp * object )
{
	TRACE_ENTRY("%p", object);
	
	if (CHECK_MSG(object)) {
		if (_M(object)->msg_query) {
			_M(_M(object)->msg_query)->msg_associated = 0;
			CHECK_FCT(  fd_msg_free( _M(object)->msg_query )  );
			_M(object)->msg_query = NULL;
		} else {
			if (_M(object)->msg_associated) {
				TRACE_DEBUG(INFO, "Not freeing query %p referenced in an answer (will be freed along the answer).", object);
				return 0;
			}
		}
	}
	
	destroy_tree(_C(object));
	return 0;
}


/***************************************************************************************************************/
/* Debug functions: dumping */

/* indent inside an object */
#define INOBJHDR 	"%*s   "
#define INOBJHDRVAL 	indent<0 ? 1 : indent, indent<0 ? "-" : "|"

/* Dump a msg_t object */
static void obj_dump_msg (struct msg * msg, int indent, FILE * fstr )
{
	int ret = 0;
	
	fd_log_debug_fstr(fstr, "%*sMSG: %p\n", INOBJHDRVAL, msg);
	
	if (!CHECK_MSG(msg)) {
		fd_log_debug_fstr(fstr, INOBJHDR "INVALID!\n", INOBJHDRVAL);
		return;
	}
	
	if (!msg->msg_model) {
		
		fd_log_debug_fstr(fstr, INOBJHDR "(no model)\n", INOBJHDRVAL);
		
	} else {
		
		enum dict_object_type dicttype;
		struct dict_cmd_data  dictdata;
		ret = fd_dict_gettype(msg->msg_model, &dicttype);
		if (ret || (dicttype != DICT_COMMAND)) {
			fd_log_debug_fstr(fstr, INOBJHDR "(invalid model: %d %d)\n", INOBJHDRVAL, ret, dicttype);
			goto public;
		}
		ret = fd_dict_getval(msg->msg_model, &dictdata);
		if (ret != 0) {
			fd_log_debug_fstr(fstr, INOBJHDR "(error getting model data: %s)\n", INOBJHDRVAL, strerror(ret));
			goto public;
		}
		fd_log_debug_fstr(fstr, INOBJHDR "model : v/m:" DUMP_CMDFL_str "/" DUMP_CMDFL_str ", %u \"%s\"\n", INOBJHDRVAL, 
			DUMP_CMDFL_val(dictdata.cmd_flag_val), DUMP_CMDFL_val(dictdata.cmd_flag_mask), dictdata.cmd_code, dictdata.cmd_name);
	}
public:	
	fd_log_debug_fstr(fstr, INOBJHDR "public: V:%d L:%d fl:" DUMP_CMDFL_str " CC:%u A:%d hi:%x ei:%x\n", INOBJHDRVAL, 
		msg->msg_public.msg_version,
		msg->msg_public.msg_length,
		DUMP_CMDFL_val(msg->msg_public.msg_flags),
		msg->msg_public.msg_code,
		msg->msg_public.msg_appl,
		msg->msg_public.msg_hbhid,
		msg->msg_public.msg_eteid
		);
	fd_log_debug_fstr(fstr, INOBJHDR "intern: rwb:%p rt:%d cb:%p(%p) qry:%p asso:%d sess:%p src:%s\n", 
			INOBJHDRVAL, msg->msg_rawbuffer, msg->msg_routable, msg->msg_cb.fct, msg->msg_cb.data, msg->msg_query, msg->msg_associated, msg->msg_sess, msg->msg_src_id?:"(nil)");
}

/* Dump an avp object */
static void obj_dump_avp ( struct avp * avp, int indent, FILE * fstr )
{
	int ret = 0;
	enum dict_avp_basetype type = -1;
	
	if (!CHECK_AVP(avp)) {
		fd_log_debug_fstr(fstr, INOBJHDR "INVALID!\n", INOBJHDRVAL);
		return;
	}
	
	if (!avp->avp_model) {
		
		fd_log_debug_fstr(fstr, INOBJHDR "(no model)\n", INOBJHDRVAL);
		
	} else {
		
		enum dict_object_type dicttype;
		struct dict_avp_data dictdata;
		ret = fd_dict_gettype(avp->avp_model, &dicttype);
		if (ret || (dicttype != DICT_AVP)) {
			fd_log_debug_fstr(fstr, INOBJHDR "(invalid model: %d %d)\n", INOBJHDRVAL, ret, dicttype);
			goto public;
		}
		ret = fd_dict_getval(avp->avp_model, &dictdata);
		if (ret != 0) {
			fd_log_debug_fstr(fstr, INOBJHDR "(error getting model data: %s)\n", INOBJHDRVAL, strerror(ret));
			goto public;
		}
		fd_log_debug_fstr(fstr, INOBJHDR "model : v/m:" DUMP_AVPFL_str "/" DUMP_AVPFL_str ", %12s, %u \"%s\"\n", INOBJHDRVAL, 
			DUMP_AVPFL_val(dictdata.avp_flag_val), 
			DUMP_AVPFL_val(dictdata.avp_flag_mask), 
			type_base_name[dictdata.avp_basetype], 
			dictdata.avp_code, 
			dictdata.avp_name );
		type = dictdata.avp_basetype;
	}
public:	
	fd_log_debug_fstr(fstr, INOBJHDR "public: C:%u fl:" DUMP_AVPFL_str " L:%d V:%u  data:@%p\n", INOBJHDRVAL, 
		avp->avp_public.avp_code,
		DUMP_AVPFL_val(avp->avp_public.avp_flags),
		avp->avp_public.avp_len,
		avp->avp_public.avp_vendor,
		avp->avp_public.avp_value
		);
	/* Dump the value if set */
	if (avp->avp_public.avp_value) {
		if (!avp->avp_model) {
			fd_log_debug_fstr(fstr, INOBJHDR "(data set but no model: ERROR)\n", INOBJHDRVAL);
		} else {
			fd_dict_dump_avp_value(avp->avp_public.avp_value, avp->avp_model, indent, fstr);
		}
	}

	fd_log_debug_fstr(fstr, INOBJHDR "intern: src:%p mf:%d raw:%p(%d)\n", INOBJHDRVAL, avp->avp_source, avp->avp_mustfreeos, avp->avp_rawdata, avp->avp_rawlen);
}

/* Dump a single object content */
static void msg_dump_intern ( int level, msg_or_avp * obj, int indent, FILE * fstr )
{
	/* Log only if we are at least at level */
	if ( ! TRACE_BOOL(level) )
		return;
	
	/* Check the object */
	if (!VALIDATE_OBJ(obj)) {
		fd_log_debug_fstr(fstr, ">>> invalid object (%p)!.\n", obj);
		return;
	}
	
	/* Dump the object */
	switch (_C(obj)->type) {
		case MSG_AVP:
			obj_dump_avp ( _A(obj), indent, fstr );
			break;
		
		case MSG_MSG:
			obj_dump_msg ( _M(obj), indent, fstr );
			break;
		
		default:
			ASSERT(0);
	}
}

/* Dump a message to a specified file stream */
void fd_msg_dump_fstr ( struct msg * msg, FILE * fstr )
{
	msg_or_avp * ref = msg;
	int indent = 2;
	do {
		msg_dump_intern ( NONE, ref, indent, fstr );
		
		/* Now find the next object */
		CHECK_FCT_DO(  fd_msg_browse ( ref, MSG_BRW_WALK, &ref, &indent ), break  );
		
		/* dump next object */
	} while (ref);
}

/* Dump a message content -- for debug mostly */
void fd_msg_dump_walk ( int level, msg_or_avp *obj )
{
	msg_or_avp * ref = obj;
	int indent = 1;
	
	TRACE_DEBUG(level, "------ Dumping object %p (w)-------", obj);
	do {
		msg_dump_intern ( level, ref, indent, fd_g_debug_fstr );
		
		/* Now find the next object */
		CHECK_FCT_DO(  fd_msg_browse ( ref, MSG_BRW_WALK, &ref, &indent ), break  );
		
		/* dump next object */
	} while (ref);
	
	TRACE_DEBUG(level, "------ /end of object %p -------", obj);
}

/* Dump a single object content -- for debug mostly */
void fd_msg_dump_one ( int level, msg_or_avp * obj )
{
	TRACE_DEBUG(level, "------ Dumping object %p (s)-------", obj);
	msg_dump_intern ( level, obj, 1, fd_g_debug_fstr );
	TRACE_DEBUG(level, "------ /end of object %p -------", obj);
}


/***************************************************************************************************************/
/* Simple meta-data management */

/* Retrieve the model of an object */
int fd_msg_model ( msg_or_avp * reference, struct dict_object ** model )
{
	TRACE_ENTRY("%p %p", reference, model);
	
	/* Check the parameters */
	CHECK_PARAMS(  model && VALIDATE_OBJ(reference)  );
	
	/* copy the model reference */
	switch (_C(reference)->type) {
		case MSG_AVP:
			*model = _A(reference)->avp_model;
			break;
		
		case MSG_MSG:
			*model = _M(reference)->msg_model;
			break;
		
		default:
			CHECK_PARAMS(0);
	}
	
	return 0;
}

/* Retrieve the address of the msg_public field of a message */
int fd_msg_hdr ( struct msg *msg, struct msg_hdr **pdata )
{
	TRACE_ENTRY("%p %p", msg, pdata);
	CHECK_PARAMS(  CHECK_MSG(msg) && pdata  );
	
	*pdata = &msg->msg_public;
	return 0;
}

/* Retrieve the address of the avp_public field of an avp */
int fd_msg_avp_hdr ( struct avp *avp, struct avp_hdr **pdata )
{
	TRACE_ENTRY("%p %p", avp, pdata);
	CHECK_PARAMS(  CHECK_AVP(avp) && pdata  );
	
	*pdata = &avp->avp_public;
	return 0;
}

/* Associate answers and queries */
int fd_msg_answ_associate( struct msg * answer, struct msg * query )
{
	TRACE_ENTRY( "%p %p", answer, query );
	
	CHECK_PARAMS(  CHECK_MSG(answer) && CHECK_MSG(query) && (answer->msg_query == NULL )  );
	
	answer->msg_query = query;
	query->msg_associated = 1;
	
	return 0;
}	

int fd_msg_answ_getq( struct msg * answer, struct msg ** query )
{
	TRACE_ENTRY( "%p %p", answer, query );
	
	CHECK_PARAMS(  CHECK_MSG(answer) && query  );
	
	*query = answer->msg_query;
	
	return 0;
}	

int fd_msg_answ_detach( struct msg * answer )
{
	TRACE_ENTRY( "%p", answer );
	
	CHECK_PARAMS(  CHECK_MSG(answer) );
	
	answer->msg_query->msg_associated = 0;
	answer->msg_query = NULL;
	
	return 0;
}

/* Associate / get answer callbacks */
int fd_msg_anscb_associate( struct msg * msg, void ( *anscb)(void *, struct msg **), void  * data, const struct timespec *timeout )
{
	TRACE_ENTRY("%p %p %p", msg, anscb, data);
	
	/* Check the parameters */
	CHECK_PARAMS( CHECK_MSG(msg) && anscb );
	CHECK_PARAMS( msg->msg_public.msg_flags & CMD_FLAG_REQUEST ); /* we associate with requests only */
	CHECK_PARAMS( msg->msg_cb.fct == NULL ); /* No cb is already registered */
	
	/* Associate callback and data with the message, if any */
	msg->msg_cb.fct = anscb;
	msg->msg_cb.data = data;
	if (timeout) {
		memcpy(&msg->msg_cb.timeout, timeout, sizeof(struct timespec));
	}
	
	return 0;
}	

int fd_msg_anscb_get( struct msg * msg, void (**anscb)(void *, struct msg **), void ** data )
{
	TRACE_ENTRY("%p %p %p", msg, anscb, data);
	
	/* Check the parameters */
	CHECK_PARAMS( CHECK_MSG(msg) && anscb && data );
	
	/* Copy the result */
	*anscb = msg->msg_cb.fct;
	*data  = msg->msg_cb.data;
	
	return 0;
}

struct timespec *fd_msg_anscb_gettimeout( struct msg * msg )
{
	TRACE_ENTRY("%p", msg);
	
	/* Check the parameters */
	CHECK_PARAMS_DO( CHECK_MSG(msg), return NULL );
	
	if (!msg->msg_cb.timeout.tv_sec) {
		return NULL;
	}
	
	return &msg->msg_cb.timeout;
}

/* Associate routing lists */
int fd_msg_rt_associate( struct msg * msg, struct rt_data ** rtd )
{
	TRACE_ENTRY( "%p %p", msg, rtd );
	
	CHECK_PARAMS(  CHECK_MSG(msg) && rtd  );
	
	msg->msg_rtdata = *rtd;
	*rtd = NULL;
	
	return 0;
}

int fd_msg_rt_get( struct msg * msg, struct rt_data ** rtd )
{
	TRACE_ENTRY( "%p %p", msg, rtd );
	
	CHECK_PARAMS(  CHECK_MSG(msg) && rtd  );
	
	*rtd = msg->msg_rtdata;
	msg->msg_rtdata = NULL;
	
	return 0;
}	

/* Find if a message is routable */
int fd_msg_is_routable ( struct msg * msg )
{
	TRACE_ENTRY("%p", msg);
	
	CHECK_PARAMS_DO(  CHECK_MSG(msg),  return 0 /* pretend the message is not routable */ );
	
	if ( ! msg->msg_routable ) {
		/* To define if a message is routable, we rely on the "PXY" flag (for application 0). */
		msg->msg_routable = ((msg->msg_public.msg_appl != 0) || (msg->msg_public.msg_flags & CMD_FLAG_PROXIABLE)) ? 1 : 2;
		
		/* Note : the 'real' criteria according to the Diameter I-D is that the message is 
		 routable if and only if the "Destination-Realm" AVP is required by the command ABNF.
		 We could make a test for this here, but it's more computational work and our test
		 seems accurate (until proven otherwise...) */
	}
	
	return (msg->msg_routable == 1) ? 1 : 0;
}

/* Associate source peer */
int fd_msg_source_set( struct msg * msg, char * diamid, int add_rr, struct dictionary * dict )
{
	TRACE_ENTRY( "%p %p %d %p", msg, diamid, add_rr, dict);
	
	/* Check we received a valid message */
	CHECK_PARAMS( CHECK_MSG(msg) && ( (! add_rr) || dict ) );
	
	/* Cleanup any previous source */
	free(msg->msg_src_id); msg->msg_src_id = NULL;
	
	/* If the request is to cleanup the source, we are done */
	if (diamid == NULL) {
		return 0;
	}
	
	/* Otherwise save the new informations */
	CHECK_MALLOC( msg->msg_src_id = strdup(diamid) );
	
	if (add_rr) {
		struct dict_object 	*avp_rr_model;
		avp_code_t 		 code = AC_ROUTE_RECORD;
		struct avp 		*avp;
		union avp_value		 val;
		
		/* Find the model for Route-Record in the dictionary */
		CHECK_FCT( fd_dict_search ( dict, DICT_AVP, AVP_BY_CODE, &code, &avp_rr_model, ENOENT) );
		
		/* Create the AVP with this model */
		CHECK_FCT( fd_msg_avp_new ( avp_rr_model, 0, &avp ) );
		
		/* Set the AVP value with the diameter id */
		memset(&val, 0, sizeof(val));
		val.os.data = (unsigned char *)diamid;
		val.os.len  = strlen(diamid);
		CHECK_FCT( fd_msg_avp_setvalue( avp, &val ) );

		/* Add the AVP in the message */
		CHECK_FCT( fd_msg_avp_add( msg, MSG_BRW_LAST_CHILD, avp ) );
	}
	
	/* done */
	return 0;
}

int fd_msg_source_get( struct msg * msg, char ** diamid )
{
	TRACE_ENTRY( "%p %p", msg, diamid);
	
	/* Check we received valid parameters */
	CHECK_PARAMS( CHECK_MSG(msg) );
	CHECK_PARAMS( diamid );
	
	/* Copy the informations */
	*diamid = msg->msg_src_id;
	
	/* done */
	return 0;
}

/* Retrieve the session of the message */
int fd_msg_sess_get(struct dictionary * dict, struct msg * msg, struct session ** session, int * new)
{
	struct avp * avp;
	
	TRACE_ENTRY("%p %p %p", msg, session, new);
	
	/* Check we received valid parameters */
	CHECK_PARAMS( CHECK_MSG(msg) );
	CHECK_PARAMS( session );
	
	/* If we already resolved the session, just send it back */
	if (msg->msg_sess) {
		*session = msg->msg_sess;
		if (new)
			*new = 0;
		return 0;
	}
	
	/* OK, we have to search for Session-Id AVP -- it is usually the first AVP, but let's be permissive here */
	/* -- note: we accept messages that have not yet been dictionary parsed... */
	CHECK_FCT(  fd_msg_browse(msg, MSG_BRW_FIRST_CHILD, &avp, NULL)  );
	while (avp) {
		if ( (avp->avp_public.avp_code   == AC_SESSION_ID)
		  && (avp->avp_public.avp_vendor == 0) )
			break;
		
		/* Otherwise move to next AVP in the message */
		CHECK_FCT( fd_msg_browse(avp, MSG_BRW_NEXT, &avp, NULL) );
	}
	
	if (!avp) {
		TRACE_DEBUG(FULL, "No Session-Id AVP found in message %p", msg);
		*session = NULL;
		return 0;
	}
	
	if (!avp->avp_model) {
		CHECK_FCT( fd_msg_parse_dict ( avp, dict, NULL ) );
	}
	
	ASSERT( avp->avp_public.avp_value );
	
	/* Resolve the session and we are done */
	CHECK_FCT( fd_sess_fromsid_msg ( avp->avp_public.avp_value->os.data, avp->avp_public.avp_value->os.len, &msg->msg_sess, new) );
	*session = msg->msg_sess;
	
	return 0;
}


/******************* End-to-end counter *********************/
static uint32_t fd_eteid;
static pthread_mutex_t fd_eteid_lck = PTHREAD_MUTEX_INITIALIZER;

void fd_msg_eteid_init(void)
{
	fd_eteid = ((uint32_t)time(NULL) << 20) | ((uint32_t)lrand48() & ( (1 << 20) - 1 ));
}

uint32_t fd_msg_eteid_get ( void )
{
	uint32_t ret;
	
	CHECK_POSIX_DO( pthread_mutex_lock(&fd_eteid_lck), /* continue */ );
	
	ret = fd_eteid ++;
	
	CHECK_POSIX_DO( pthread_mutex_unlock(&fd_eteid_lck), /* continue */ );
	
	return ret;
}

/***************************************************************************************************************/
/* Manage AVPs values */

/* Set the value of an AVP */
int fd_msg_avp_setvalue ( struct avp *avp, union avp_value *value )
{
	enum dict_avp_basetype type = -1;
	
	TRACE_ENTRY("%p %p", avp, value);
	
	/* Check parameter */
	CHECK_PARAMS(  CHECK_AVP(avp) && avp->avp_model  );
	
	/* Retrieve information from the AVP model */
	{
		enum dict_object_type dicttype;
		struct dict_avp_data  dictdata;
		
		CHECK_PARAMS( (fd_dict_gettype(avp->avp_model, &dicttype) == 0) && (dicttype == DICT_AVP) );
		CHECK_FCT(  fd_dict_getval(avp->avp_model, &dictdata)  );
		type = dictdata.avp_basetype;
		CHECK_PARAMS(  type != AVP_TYPE_GROUPED  );
	}
	
	/* First, clean any previous value */
	if (avp->avp_mustfreeos != 0) {
		free(avp->avp_storage.os.data);
		avp->avp_mustfreeos = 0;
	}
	
	memset(&avp->avp_storage, 0, sizeof(union avp_value));
	
	/* If the request was to delete a value: */
	if (!value) {
		avp->avp_public.avp_value = NULL;
		return 0;
	}
	
	/* Now we have to set the value */
	memcpy(&avp->avp_storage, value, sizeof(union avp_value));
	
	/* Copy an octetstring if needed. */
	if (type == AVP_TYPE_OCTETSTRING) {
		if (value->os.len) {
			CHECK_MALLOC(  avp->avp_storage.os.data = malloc(value->os.len)  );
			avp->avp_mustfreeos = 1;
			memcpy(avp->avp_storage.os.data, value->os.data, value->os.len);
		} else {
			avp->avp_storage.os.data = NULL;
		}
	}
	
	/* Set the data pointer of the public part */
	avp->avp_public.avp_value = &avp->avp_storage;
	
	return 0;		
}

/* Set the value of an AVP, using formatted data */
int fd_msg_avp_value_encode ( void *data, struct avp *avp )
{
	enum dict_avp_basetype type = -1;
	struct dict_type_data type_data;
	
	TRACE_ENTRY("%p %p", data, avp);
	
	/* Check parameter */
	CHECK_PARAMS(  CHECK_AVP(avp) && avp->avp_model  );
	
	/* Retrieve information from the AVP model and it's parent type */
	{
		enum dict_object_type dicttype;
		struct dict_avp_data  dictdata;
		struct dictionary   * dict;
		struct dict_object  * parenttype = NULL;
		
		/* First check the base type of the AVP */
		CHECK_PARAMS( (fd_dict_gettype(avp->avp_model, &dicttype) == 0) && (dicttype == DICT_AVP) );
		CHECK_FCT(  fd_dict_getval(avp->avp_model, &dictdata)  );
		type = dictdata.avp_basetype;
		CHECK_PARAMS(  type != AVP_TYPE_GROUPED  );
		
		/* Then retrieve information about the parent's type (= derived type) */
		CHECK_FCT(  fd_dict_getdict( avp->avp_model, &dict )  );
		CHECK_FCT(  fd_dict_search( dict, DICT_TYPE, TYPE_OF_AVP, avp->avp_model, &parenttype, EINVAL)  );
		CHECK_FCT(  fd_dict_getval(parenttype, &type_data)  );
		if (type_data.type_encode == NULL) {
			TRACE_DEBUG(INFO, "This AVP type does not provide a callback to encode formatted data. ENOTSUP.");
			return ENOTSUP;
		}
	}
	
	/* Ok, now we can encode the value */
	
	/* First, clean any previous value */
	if (avp->avp_mustfreeos != 0) {
		free(avp->avp_storage.os.data);
		avp->avp_mustfreeos = 0;
	}
	avp->avp_public.avp_value = NULL;
	memset(&avp->avp_storage, 0, sizeof(union avp_value));
	
	/* Now call the type's callback to encode the data */
	CHECK_FCT(  (*type_data.type_encode)(data, &avp->avp_storage)  );
	
	/* If an octetstring has been allocated, let's mark it to be freed */
	if (type == AVP_TYPE_OCTETSTRING)
		avp->avp_mustfreeos = 1;
	
	/* Set the data pointer of the public part */
	avp->avp_public.avp_value = &avp->avp_storage;
	
	return 0;		
}

/* Interpret the value of an AVP into formatted data */
int fd_msg_avp_value_interpret ( struct avp *avp, void *data )
{
	struct dict_type_data type_data;
	
	TRACE_ENTRY("%p %p", avp, data);
	
	/* Check parameter */
	CHECK_PARAMS(  CHECK_AVP(avp) && avp->avp_model && avp->avp_public.avp_value  );
	
	/* Retrieve information about the AVP parent type */
	{
		struct dictionary   * dict;
		struct dict_object  * parenttype = NULL;
		
		CHECK_FCT(  fd_dict_getdict( avp->avp_model, &dict )  );
		CHECK_FCT(  fd_dict_search( dict, DICT_TYPE, TYPE_OF_AVP, avp->avp_model, &parenttype, EINVAL)  );
		CHECK_FCT(  fd_dict_getval(parenttype, &type_data)  );
		if (type_data.type_interpret == NULL) {
			TRACE_DEBUG(INFO, "This AVP type does not provide a callback to interpret value in formatted data. ENOTSUP.");
			return ENOTSUP;
		}
	}
	
	/* Ok, now we can interpret the value */
	
	CHECK_FCT(  (*type_data.type_interpret)(avp->avp_public.avp_value, data)  );
	
	return 0;		
}

/***************************************************************************************************************/
/* Creating a buffer from memory objects (bufferize a struct msg) */

/* Following macros are used to store 32 and 64 bit fields into a buffer in network byte order */
#define PUT_in_buf_32( _u32data, _bufptr ) {							\
	*(uint32_t *)(_bufptr) = htonl((uint32_t)(_u32data));					\
}
#define PUT_in_buf_64( _u64data, _bufptr ) {							\
	*(uint64_t *)(_bufptr) = htonll((uint64_t)(_u64data));					\
}

/* Write a message header in the buffer */
static int bufferize_msg(unsigned char * buffer, size_t buflen, size_t * offset, struct msg * msg)
{
	TRACE_ENTRY("%p %d %p %p", buffer, buflen, offset, msg);
	
	if ((buflen - *offset) < GETMSGHDRSZ())
		return ENOSPC;
	
	if (*offset & 0x3)
		return EFAULT;	/* We are supposed to start on 32 bit boundaries */
	
	PUT_in_buf_32(msg->msg_public.msg_length, buffer + *offset);
	buffer[*offset] = msg->msg_public.msg_version;
	*offset += 4;
	
	PUT_in_buf_32(msg->msg_public.msg_code, buffer + *offset);
	buffer[*offset] = msg->msg_public.msg_flags;
	*offset += 4;
	
	PUT_in_buf_32(msg->msg_public.msg_appl, buffer + *offset);
	*offset += 4;
	
	PUT_in_buf_32(msg->msg_public.msg_hbhid, buffer + *offset);
	*offset += 4;
	
	PUT_in_buf_32(msg->msg_public.msg_eteid, buffer + *offset);
	*offset += 4;
	
	return 0;
}

static int bufferize_chain(unsigned char * buffer, size_t buflen, size_t * offset, struct fd_list * list);

/* Write an AVP in the buffer */
static int bufferize_avp(unsigned char * buffer, size_t buflen, size_t * offset,  struct avp * avp)
{
	struct dict_avp_data dictdata;
	
	TRACE_ENTRY("%p %d %p %p", buffer, buflen, offset, avp);
	
	if ((buflen - *offset) < avp->avp_public.avp_len)
		return ENOSPC;
	
	/* Write the header */
	PUT_in_buf_32(avp->avp_public.avp_code, buffer + *offset);
	*offset += 4;
	
	PUT_in_buf_32(avp->avp_public.avp_len, buffer + *offset);
	buffer[*offset] = avp->avp_public.avp_flags;
	*offset += 4;
	
	if (avp->avp_public.avp_flags & AVP_FLAG_VENDOR) {
		PUT_in_buf_32(avp->avp_public.avp_vendor, buffer + *offset);
		*offset += 4;
	}
	
	/* Then we must write the AVP value */
	
	if (avp->avp_model == NULL) {
		/* In the case where we don't know the type of AVP, just copy the raw data or source */
		CHECK_PARAMS( avp->avp_source || avp->avp_rawdata );
		
		if ( avp->avp_source != NULL ) {
			/* the message was not parsed completely */
			size_t datalen = avp->avp_public.avp_len - GETAVPHDRSZ(avp->avp_public.avp_flags);
			memcpy(&buffer[*offset], avp->avp_source, datalen);
			*offset += PAD4(datalen);
		} else {
			/* the content was stored in rawdata */
			memcpy(&buffer[*offset], avp->avp_rawdata, avp->avp_rawlen);
			*offset += PAD4(avp->avp_rawlen);
		}
		
	} else {
		/* The AVP is defined in the dictionary */
		CHECK_FCT(  fd_dict_getval(avp->avp_model, &dictdata)  );

		CHECK_PARAMS( ( dictdata.avp_basetype == AVP_TYPE_GROUPED ) || avp->avp_public.avp_value );
		
		switch (dictdata.avp_basetype) {
			case AVP_TYPE_GROUPED:
				return bufferize_chain(buffer, buflen, offset, &avp->avp_chain.children);

			case AVP_TYPE_OCTETSTRING:
				if (avp->avp_public.avp_value->os.len)
					memcpy(&buffer[*offset], avp->avp_public.avp_value->os.data, avp->avp_public.avp_value->os.len);
				*offset += PAD4(avp->avp_public.avp_value->os.len);
				break;

			case AVP_TYPE_INTEGER32:
				PUT_in_buf_32(avp->avp_public.avp_value->i32, buffer + *offset);
				*offset += 4;
				break;

			case AVP_TYPE_INTEGER64:
				PUT_in_buf_64(avp->avp_public.avp_value->i64, buffer + *offset);
				*offset += 8;
				break;

			case AVP_TYPE_UNSIGNED32:
				PUT_in_buf_32(avp->avp_public.avp_value->u32, buffer + *offset);
				*offset += 4;
				break;

			case AVP_TYPE_UNSIGNED64:
				PUT_in_buf_64(avp->avp_public.avp_value->u64, buffer + *offset);
				*offset += 8;
				break;

			case AVP_TYPE_FLOAT32:
				/* We read the f32 as "u32" here to avoid casting to uint make decimals go away. 
				 The alternative would be something like "*(uint32_t *)(& f32)" but
				 then the compiler complains about strict-aliasing rules. */
				PUT_in_buf_32(avp->avp_public.avp_value->u32, buffer + *offset);
				*offset += 4;
				break;

			case AVP_TYPE_FLOAT64:
				/* Same remark as previously */
				PUT_in_buf_64(avp->avp_public.avp_value->u64, buffer + *offset);
				*offset += 8;
				break;

			default:
				ASSERT(0);
		}
	}
	return 0;
}
			
/* Write a chain of AVPs in the buffer */
static int bufferize_chain(unsigned char * buffer, size_t buflen, size_t * offset, struct fd_list * list)
{
	struct fd_list * avpch;
	
	TRACE_ENTRY("%p %d %p %p", buffer, buflen, offset, list);
	
	for (avpch = list->next; avpch != list; avpch = avpch->next) {
		/* Bufferize the AVP */
		CHECK_FCT( bufferize_avp(buffer, buflen, offset, _A(avpch->o))  );
	}
	return 0;
}

/* Create the message buffer, in network-byte order. We browse the tree twice, this could be probably improved if needed */
int fd_msg_bufferize ( struct msg * msg, unsigned char ** buffer, size_t * len )
{
	int ret = 0;
	unsigned char * buf = NULL;
	size_t offset = 0;
	
	TRACE_ENTRY("%p %p %p", msg, buffer, len);
	
	/* Check the parameters */
	CHECK_PARAMS(  buffer && CHECK_MSG(msg)  );
	
	/* Update the length. This also checks that all AVP have their values set */
	CHECK_FCT(  fd_msg_update_length(msg)  );
	
	/* Now allocate a buffer to store the message */
	CHECK_MALLOC(  buf = malloc(msg->msg_public.msg_length)  );
	
	/* Clear the memory, so that the padding is always 0 (should not matter) */
	memset(buf, 0, msg->msg_public.msg_length);
	
	/* Write the message header in the buffer */
	CHECK_FCT_DO( ret = bufferize_msg(buf, msg->msg_public.msg_length, &offset, msg), 
		{
			free(buf);
			return ret;
		}  );
	
	/* Write the list of AVPs */
	CHECK_FCT_DO( ret = bufferize_chain(buf, msg->msg_public.msg_length, &offset, &msg->msg_chain.children),
		{
			free(buf);
			return ret;
		}  );
	
	ASSERT(offset == msg->msg_public.msg_length); /* or the msg_update_length is buggy */
		
	if (len) {
		*len = offset;
	}
	
	*buffer = buf;
	return 0;
}


/***************************************************************************************************************/
/* Parsing buffers and building AVP objects lists (not parsing the AVP values which requires dictionary knowledge) */

/* Parse a buffer containing a supposed list of AVPs */
static int parsebuf_list(unsigned char * buf, size_t buflen, struct fd_list * head)
{
	size_t offset = 0;
	
	TRACE_ENTRY("%p %d %p", buf, buflen, head);
	
	while (offset < buflen) {
		struct avp * avp;
		
		if (buflen - offset <= AVPHDRSZ_NOVEND) {
			TRACE_DEBUG(INFO, "truncated buffer: remaining only %d bytes", buflen - offset);
			return EBADMSG;
		}
		
		/* Create a new AVP object */
		CHECK_MALLOC(  avp = malloc (sizeof(struct avp))  );
		
		init_avp(avp);
		
		/* Initialize the header */
		avp->avp_public.avp_code    = ntohl(*(uint32_t *)(buf + offset));
		avp->avp_public.avp_flags   = buf[offset + 4];
		avp->avp_public.avp_len     = ((uint32_t)buf[offset+5]) << 16 |  ((uint32_t)buf[offset+6]) << 8 |  ((uint32_t)buf[offset+7]) ;
		
		offset += 8;
		
		if (avp->avp_public.avp_flags & AVP_FLAG_VENDOR) {
			if (buflen - offset <= 4) {
				TRACE_DEBUG(INFO, "truncated buffer: remaining only %d bytes for vendor and data", buflen - offset);
				free(avp);
				return EBADMSG;
			}
			avp->avp_public.avp_vendor  = ntohl(*(uint32_t *)(buf + offset));
			offset += 4;
		}
		
		/* Check there is enough remaining data in the buffer */
		if (buflen - offset < avp->avp_public.avp_len - GETAVPHDRSZ(avp->avp_public.avp_flags)) {
			TRACE_DEBUG(INFO, "truncated buffer: remaining only %d bytes for data, and avp data size is %d", 
					buflen - offset, 
					avp->avp_public.avp_len - GETAVPHDRSZ(avp->avp_public.avp_flags));
			free(avp);
			return EBADMSG;
		}
		
		/* buf[offset] is now the beginning of the data */
		avp->avp_source = &buf[offset];
		
		/* Now eat the data and eventual padding */
		offset += PAD4(avp->avp_public.avp_len - GETAVPHDRSZ(avp->avp_public.avp_flags));
		
		/* And insert this avp in the list, at the end */
		fd_list_insert_before( head, &avp->avp_chain.chaining );
	}
	
	return 0;
}

/* Create a message object from a buffer. Dictionary objects are not resolved, AVP contents are not interpreted, buffer is saved in msg */
int fd_msg_parse_buffer ( unsigned char ** buffer, size_t buflen, struct msg ** msg )
{
	struct msg * new = NULL;
	int ret = 0;
	uint32_t msglen = 0;
	unsigned char * buf;
	
	TRACE_ENTRY("%p %d %p", buffer, buflen, msg);
	
	CHECK_PARAMS(  buffer &&  *buffer  &&  msg  &&  (buflen >= GETMSGHDRSZ())  );
	buf = *buffer;
	*buffer = NULL;
	
	if ( buf[0] != DIAMETER_VERSION) {
		TRACE_DEBUG(INFO, "Invalid version in message: %d (supported: %d)", buf[0], DIAMETER_VERSION);
		free(buf);
		return EBADMSG;
	}
	
	msglen = ntohl(*(uint32_t *)buf) & 0x00ffffff;
	if ( buflen < msglen ) {  
		TRACE_DEBUG(INFO, "Truncated message (%d / %d)", buflen, msglen );
		free(buf);
		return EBADMSG; 
	}
	
	/* Create a new object */
	CHECK_MALLOC_DO( new = malloc (sizeof(struct msg)),  { free(buf); return ENOMEM; }  );
	
	/* Initialize the fields */
	init_msg(new);
	
	/* Now read from the buffer */
	new->msg_public.msg_version = buf[0];
	new->msg_public.msg_length = msglen;

	new->msg_public.msg_flags = buf[4];
	new->msg_public.msg_code = ntohl(*(uint32_t *)(buf+4)) & 0x00ffffff;
	
	new->msg_public.msg_appl = ntohl(*(uint32_t *)(buf+8));
	new->msg_public.msg_hbhid = ntohl(*(uint32_t *)(buf+12));
	new->msg_public.msg_eteid = ntohl(*(uint32_t *)(buf+16));
	
	new->msg_rawbuffer = buf;
	
	/* Parse the AVP list */
	CHECK_FCT_DO( ret = parsebuf_list(buf + GETMSGHDRSZ(), buflen - GETMSGHDRSZ(), &new->msg_chain.children), { destroy_tree(_C(new)); return ret; }  );
	
	*msg = new;
	return 0;
}

		
/***************************************************************************************************************/
/* Parsing messages and AVP with dictionary information */

/* Resolve dictionary objects of the cmd and avp instances, from their headers.
 * When the model is found, the data is interpreted from the avp_source buffer and copied to avp_storage.
 * When the model is not found, the data is copied as rawdata and saved (in case we FW the message).
 * Therefore, after this function has been called, the source buffer can be freed.
 * For command, if the dictionary model is not found, an error is returned.
 */

static int parsedict_do_chain(struct dictionary * dict, struct fd_list * head, int mandatory, struct fd_pei *error_info);

/* Process an AVP. If we are not in recheck, the avp_source must be set. */
static int parsedict_do_avp(struct dictionary * dict, struct avp * avp, int mandatory, struct fd_pei *error_info)
{
	struct dict_avp_data dictdata;
	
	TRACE_ENTRY("%p %p %d %p", dict, avp, mandatory, error_info);
	
	/* First check we received an AVP as input */
	CHECK_PARAMS(  CHECK_AVP(avp) );
	
	if (avp->avp_model != NULL) {
		/* the model has already been resolved. we do check it is still valid */

		CHECK_FCT(  fd_dict_getval(avp->avp_model, &dictdata)  );

		if ( avp->avp_public.avp_code == dictdata.avp_code  ) {
			/* Ok then just process the children if any */
			return parsedict_do_chain(dict, &avp->avp_chain.children, mandatory && (avp->avp_public.avp_flags & AVP_FLAG_MANDATORY), error_info);
		} else {
			/* We just erase the old model */
			avp->avp_model = NULL;
		}
	}
	
	/* Now try and resolve the model from the avp code and vendor */
	if (avp->avp_public.avp_flags & AVP_FLAG_VENDOR) {
		struct dict_avp_request avpreq;
		avpreq.avp_vendor = avp->avp_public.avp_vendor;
		avpreq.avp_code = avp->avp_public.avp_code;
		CHECK_FCT( fd_dict_search ( dict, DICT_AVP, AVP_BY_CODE_AND_VENDOR, &avpreq, &avp->avp_model, 0));
	} else {
		/* no vendor */
		CHECK_FCT( fd_dict_search ( dict, DICT_AVP, AVP_BY_CODE, &avp->avp_public.avp_code, &avp->avp_model, 0));
	}
	
	/* First handle the case where we have not found this AVP in the dictionary */
	if (!avp->avp_model) {
		
		if (mandatory && (avp->avp_public.avp_flags & AVP_FLAG_MANDATORY)) {
			TRACE_DEBUG(INFO, "Unsupported mandatory AVP found:");
			msg_dump_intern(INFO, avp, 2, fd_g_debug_fstr);
			if (error_info) {
				error_info->pei_errcode = "DIAMETER_AVP_UNSUPPORTED";
				error_info->pei_avp = avp;
			}
			return ENOTSUP;
		}
		
		if (avp->avp_source) {
			/* we must copy the data from the source to the internal buffer area */
			CHECK_PARAMS( !avp->avp_rawdata  );
			
			avp->avp_rawlen = avp->avp_public.avp_len - GETAVPHDRSZ( avp->avp_public.avp_flags );
			
			if (avp->avp_rawlen) {
				CHECK_MALLOC(  avp->avp_rawdata = malloc(avp->avp_rawlen)  );
			
				memcpy(avp->avp_rawdata, avp->avp_source, avp->avp_rawlen);
			}
			
			avp->avp_source = NULL;
			
			TRACE_DEBUG(FULL, "Unsupported optional AVP found, raw source data saved in avp_rawdata.");
		}
		
		return 0;
	}
	
	/* Ok we have resolved the object. Now we need to interpret its content. */
	
	CHECK_FCT(  fd_dict_getval(avp->avp_model, &dictdata)  );
	
	if (avp->avp_rawdata) {
		/* This happens if the dictionary object was defined after the first check */
		avp->avp_source = avp->avp_rawdata;
	}
	
	/* A bit of sanity here... */
	ASSERT(CHECK_BASETYPE(dictdata.avp_basetype));
	
	/* Check the size is valid */
	if ((avp_value_sizes[dictdata.avp_basetype] != 0) &&
	    (avp->avp_public.avp_len - GETAVPHDRSZ( avp->avp_public.avp_flags ) != avp_value_sizes[dictdata.avp_basetype])) {
		TRACE_DEBUG(INFO, "The AVP size is not suitable for the type.");
		if (error_info) {
			error_info->pei_errcode = "DIAMETER_INVALID_AVP_LENGTH";
			error_info->pei_avp = avp;
		}
		return EBADMSG;
	}
	
	/* Now get the value inside */
	switch (dictdata.avp_basetype) {
		case AVP_TYPE_GROUPED: {
			int ret;
			
			/* This is a grouped AVP, so let's parse the list of AVPs inside */
			CHECK_FCT_DO(  ret = parsebuf_list(avp->avp_source, avp->avp_public.avp_len - GETAVPHDRSZ( avp->avp_public.avp_flags ), &avp->avp_chain.children),
				{
					if ((ret == EBADMSG) && (error_info)) {
						error_info->pei_errcode = "DIAMETER_INVALID_AVP_VALUE";
						error_info->pei_avp = avp;
					}
					return ret;
				}  );
			
			return parsedict_do_chain(dict, &avp->avp_chain.children, mandatory && (avp->avp_public.avp_flags & AVP_FLAG_MANDATORY), error_info);
		}
			
		case AVP_TYPE_OCTETSTRING:
			/* We just have to copy the string into the storage area */
			CHECK_PARAMS_DO( avp->avp_public.avp_len >= GETAVPHDRSZ( avp->avp_public.avp_flags ),
				{
					if (error_info) {
						error_info->pei_errcode = "DIAMETER_INVALID_AVP_LENGTH";
						error_info->pei_avp = avp;
					}
					return EBADMSG;
				} );
			avp->avp_storage.os.len = avp->avp_public.avp_len - GETAVPHDRSZ( avp->avp_public.avp_flags );
			if (avp->avp_storage.os.len) {
				CHECK_MALLOC(  avp->avp_storage.os.data = malloc(avp->avp_storage.os.len)  );
				avp->avp_mustfreeos = 1;
				memcpy(avp->avp_storage.os.data, avp->avp_source, avp->avp_storage.os.len);
			} else {
				avp->avp_storage.os.data = NULL;
			}
			break;
		
		case AVP_TYPE_INTEGER32:
			avp->avp_storage.i32 = (int32_t)ntohl(*(uint32_t *)avp->avp_source);
			break;
	
		case AVP_TYPE_INTEGER64:
			avp->avp_storage.i64 = (int64_t)ntohll(*(uint64_t *)avp->avp_source);
			break;
	
		case AVP_TYPE_UNSIGNED32:
		case AVP_TYPE_FLOAT32: /* For float, we must not cast, or the value is changed. Instead we use implicit cast by changing the member of the union */
			avp->avp_storage.u32 = (uint32_t)ntohl(*(uint32_t *)avp->avp_source);
			break;
	
		case AVP_TYPE_UNSIGNED64:
		case AVP_TYPE_FLOAT64: /* same as 32 bits */
			avp->avp_storage.u64 = (uint64_t)ntohll(*(uint64_t *)avp->avp_source);
			break;
	
	}
	
	/* The value is now set, so set the data pointer and return 0 */
	avp->avp_public.avp_value = &avp->avp_storage;
	return 0;
}

/* Process a list of AVPs */
static int parsedict_do_chain(struct dictionary * dict, struct fd_list * head, int mandatory, struct fd_pei *error_info)
{
	struct fd_list * avpch;
	
	TRACE_ENTRY("%p %p %d %p", dict, head, mandatory, error_info);
	
	/* Sanity check */
	ASSERT ( head == head->head );
	
	/* Now process the list */
	for (avpch=head->next; avpch != head; avpch = avpch->next) {
		CHECK_FCT(  parsedict_do_avp(dict, _A(avpch->o), mandatory, error_info)  );
	}
	
	/* Done */
	return 0;
}

/* Process a msg header. */
static int parsedict_do_msg(struct dictionary * dict, struct msg * msg, int only_hdr, struct fd_pei *error_info)
{
	int ret = 0;
	
	TRACE_ENTRY("%p %p %d %p", dict, msg, only_hdr, error_info);
	
	CHECK_PARAMS(  CHECK_MSG(msg)  );
	
	/* Look for the model from the header */
	CHECK_FCT_DO( ret = fd_dict_search ( dict, DICT_COMMAND, 
			(msg->msg_public.msg_flags & CMD_FLAG_REQUEST) ? CMD_BY_CODE_R : CMD_BY_CODE_A,
			&msg->msg_public.msg_code,
			&msg->msg_model, ENOTSUP),
		{
			if ((ret == ENOTSUP) && (error_info)) {
				error_info->pei_errcode = "DIAMETER_COMMAND_UNSUPPORTED";
				error_info->pei_protoerr = 1;
			}
			return ret;
		} );
	
	if (!only_hdr) {
		/* Then process the children */
		ret = parsedict_do_chain(dict, &msg->msg_chain.children, 1, error_info);

		/* Free the raw buffer if any */
		if ((ret == 0) && (msg->msg_rawbuffer != NULL)) {
			free(msg->msg_rawbuffer);
			msg->msg_rawbuffer=NULL;
		}
	}
	
	return ret;
}

int fd_msg_parse_dict ( msg_or_avp * object, struct dictionary * dict, struct fd_pei *error_info )
{
	TRACE_ENTRY("%p %p %p", dict, object, error_info);
	
	CHECK_PARAMS(  VALIDATE_OBJ(object)  );
	
	if (error_info)
		memset(error_info, 0, sizeof(struct fd_pei));
	
	switch (_C(object)->type) {
		case MSG_MSG:
			return parsedict_do_msg(dict, _M(object), 0, error_info);
		
		case MSG_AVP:
			return parsedict_do_avp(dict, _A(object), 0, error_info);
		
		default:
			ASSERT(0);
	}
	return EINVAL;
}

/***************************************************************************************************************/
/* Parsing messages and AVP for rules (ABNF) compliance */

/* This function is used to get stats (first occurence position, last occurence position, number of occurences) 
   of AVP instances of a given model in a chain of AVP */
static void parserules_stat_avps( struct dict_object * model_avp, struct fd_list *list, int * count, int * firstpos, int * lastpos) 
{
	struct fd_list * li;
	int curpos = 0; /* The current position in the list */
	
	TRACE_ENTRY("%p %p %p %p %p", model_avp, list, count, firstpos, lastpos);
	
	*count = 0;	/* number of instances found */
	*firstpos = 0;	/* position of the first instance */
	*lastpos = 0;	/* position of the last instance, starting from the end */
	
	for (li = list->next; li != list; li = li->next) {
		/* Increment the current position counter */
		curpos++;
		
		/* If we previously saved a "lastpos" information, increment it */
		if (*lastpos != 0)
			(*lastpos)++;
		
		/* Check the type of the next AVP. We can compare the references directly, it is safe. */
		if (_A(li->o)->avp_model == model_avp) {
			
			/* This AVP is of the type we are searching */
			(*count)++;
			
			/* If we don't have yet a "firstpos", save it */
			if (*firstpos == 0)
				*firstpos = curpos;
			
			/* Reset the lastpos */
			(*lastpos) = 1;
		}
	}
}

/* We use this structure as parameter for the next function */
struct parserules_data {
	struct fd_list  * sentinel;  	/* Sentinel of the list of children AVP */
	struct fd_pei 	* pei;   	/* If the rule conflicts, save the error here */
};

/* Create an empty AVP of a given model (to use in Failed-AVP) */
static struct avp * empty_avp(struct dict_object * model_avp)
{
	struct avp * avp = NULL;
	struct dict_avp_data avp_info;
	union avp_value val;
	unsigned char os[1] = { '\0' };
	
	/* Create an instance */
	CHECK_FCT_DO( fd_msg_avp_new(model_avp, 0, &avp ), return NULL );
	
	/* Type of the AVP */
	CHECK_FCT_DO( fd_dict_getval(model_avp, &avp_info), return NULL );
	
	/* Prepare the empty value */
	memset(&val, 0, sizeof(val));
	switch (avp_info.avp_basetype) {
		case AVP_TYPE_OCTETSTRING:
			val.os.data = os;
			val.os.len  = sizeof(os);
		case AVP_TYPE_INTEGER32:
		case AVP_TYPE_INTEGER64:
		case AVP_TYPE_UNSIGNED32:
		case AVP_TYPE_UNSIGNED64:
		case AVP_TYPE_FLOAT32:
		case AVP_TYPE_FLOAT64:
			CHECK_FCT_DO( fd_msg_avp_setvalue(avp, &val), return NULL );
		case AVP_TYPE_GROUPED:
			/* For AVP_TYPE_GROUPED we don't do anything */
			break;
		default:
			ASSERT(0); /* not handled */
	}
	
	return avp;
}

/* Check that a list of AVPs is compliant with a given rule -- will be iterated on the list of rules */
static int parserules_check_one_rule(void * data, struct dict_rule_data *rule)
{
	int count, first, last, min;
	struct parserules_data * pr_data = data;
	char * avp_name = "<unresolved name>";
	
	TRACE_ENTRY("%p %p", data, rule);
	
	/* Get statistics of the AVP concerned by this rule in the parent instance */
	parserules_stat_avps( rule->rule_avp, pr_data->sentinel, &count, &first, &last);
	
	if (TRACE_BOOL(INFO))
	{
		struct dict_avp_data avpdata;
		int ret;
		ret = fd_dict_getval(rule->rule_avp, &avpdata);
		if (ret == 0)
			avp_name = avpdata.avp_name;
		
		TRACE_DEBUG(ANNOYING, "Checking rule: p:%d(%d) m/M:%2d/%2d. Counted %d (first: %d, last:%d) of AVP '%s'", 
					rule->rule_position,
					rule->rule_order,
					rule->rule_min,
					rule->rule_max,
					count, 
					first, 
					last,
					avp_name
				);
	}
	
	/* Now check the rule is not conflicting */
	
	/* Check the "min" value */
	if ((min = rule->rule_min) == -1) {
		if (rule->rule_position == RULE_OPTIONAL)
			min = 0;
		else
			min = 1;
	}
	if (count < min) {
		TRACE_DEBUG(INFO, "Conflicting rule: the number of occurences (%d) is < the rule min (%d) for '%s'.", count, min, avp_name);
		if (pr_data->pei) {
			pr_data->pei->pei_errcode = "DIAMETER_MISSING_AVP";
			pr_data->pei->pei_avp = empty_avp(rule->rule_avp);
		}
		return EBADMSG;
	}
	
	/* Check the "max" value */
	if ((rule->rule_max != -1) && (count > rule->rule_max)) {
		TRACE_DEBUG(INFO, "Conflicting rule: the number of occurences (%d) is > the rule max (%d) for '%s'.", count, rule->rule_max, avp_name);
		if (pr_data->pei) {
			if (rule->rule_max == 0)
				pr_data->pei->pei_errcode = "DIAMETER_AVP_NOT_ALLOWED";
			else
				pr_data->pei->pei_errcode = "DIAMETER_AVP_OCCURS_TOO_MANY_TIMES";
			pr_data->pei->pei_avp = empty_avp(rule->rule_avp); /* Well we are supposed to return the (max + 1)th instance of the AVP instead... Pfff... */ TODO("Improve...");
		}
		return EBADMSG;
	}
		
	/* Check the position and order (if relevant) */
	switch (rule->rule_position) {
		case RULE_OPTIONAL:
		case RULE_REQUIRED:
			/* No special position constraints */
			break;
		
		case RULE_FIXED_HEAD:
			/* Since "0*1<fixed>" is a valid rule specifier, we only reject cases where the AVP appears *after* its fixed position */
			if (first > rule->rule_order) {
				TRACE_DEBUG(INFO, "Conflicting rule: the FIXED_HEAD AVP appears first in (%d) position, the rule requires (%d) for '%s'.", first, rule->rule_order, avp_name);
				if (pr_data->pei) {
					pr_data->pei->pei_errcode = "DIAMETER_MISSING_AVP";
					pr_data->pei->pei_message = "AVP was not in its fixed position";
					pr_data->pei->pei_avp = empty_avp(rule->rule_avp);
				}
				return EBADMSG;
			}
			break;
	
		case RULE_FIXED_TAIL:
			/* Since "0*1<fixed>" is a valid rule specifier, we only reject cases where the AVP appears *before* its fixed position */
			if (last > rule->rule_order) {	/* We have a ">" here because we count in reverse order (i.e. from the end) */
				TRACE_DEBUG(INFO, "Conflicting rule: the FIXED_TAIL AVP appears last in (%d) position, the rule requires (%d) for '%s'.", last, rule->rule_order, avp_name);
				if (pr_data->pei) {
					pr_data->pei->pei_errcode = "DIAMETER_MISSING_AVP";
					pr_data->pei->pei_message = "AVP was not in its fixed position";
					pr_data->pei->pei_avp = empty_avp(rule->rule_avp);
				}
				return EBADMSG;
			}
			break;
		
		default:
			/* What is this position ??? */
			ASSERT(0);
			return ENOTSUP;
	}
	
	/* We've checked all the parameters */
	return 0;
}

/* Check the rules recursively */
static int parserules_do ( struct dictionary * dict, msg_or_avp * object, struct fd_pei *error_info, int mandatory)
{
	struct parserules_data data;
	struct dict_object * model = NULL;
	
	TRACE_ENTRY("%p %p %p %d", dict, object, error_info, mandatory);
	
	/* object has already been checked and dict-parsed when we are called. */
	
	/* First, handle the cases where there is no model */
	{
		if (CHECK_MSG(object)) {
			if ( _M(object)->msg_public.msg_flags & CMD_FLAG_ERROR ) {
				/* The case of error messages: the ABNF is different */
				CHECK_FCT( fd_dict_get_error_cmd(dict, &model) );
			} else {
				model = _M(object)->msg_model;
			}
			/* Commands MUST be supported in the dictionary */
			if (model == NULL) {
				TRACE_DEBUG(INFO, "Message with no dictionary model. EBADMSG");
				if (error_info) {
					error_info->pei_errcode = "DIAMETER_COMMAND_UNSUPPORTED";
					error_info->pei_protoerr = 1;
				}
				return EBADMSG;
			}
		}

		/* AVP with the 'M' flag must also be recognized in the dictionary -- except inside an optional grouped AVP */
		if (CHECK_AVP(object) && ((model = _A(object)->avp_model) == NULL)) {
			if ( mandatory && (_A(object)->avp_public.avp_flags & AVP_FLAG_MANDATORY)) {
				/* Return an error in this case */
				TRACE_DEBUG(INFO, "Mandatory AVP with no dictionary model. EBADMSG");
				if (error_info) {
					error_info->pei_errcode = "DIAMETER_AVP_UNSUPPORTED";
					error_info->pei_avp = object;
				}
				return EBADMSG;
			} else {
				/* We don't know any rule for this object, so assume OK */
				TRACE_DEBUG(FULL, "Unknown informational AVP, ignoring...");
				return 0;
			}
		}
	}
	
	/* At this point we know "model" is set and points to the object's model */
	
	/* If we are an AVP with no children, just return OK */
	if (CHECK_AVP(object)) {
		struct dict_avp_data	dictdata;
		CHECK_FCT(  fd_dict_getval(model, &dictdata)  );
		if (dictdata.avp_basetype != AVP_TYPE_GROUPED) {
			/* This object has no children and no rules */
			return 0;
		}
	}
	
	/* If this object has children, first check the rules for all its children */
	{
		int is_child_mand = 0;
		struct fd_list * ch = NULL;
		if (  CHECK_MSG(object) 
		   || (mandatory && (_A(object)->avp_public.avp_flags & AVP_FLAG_MANDATORY)) )
			is_child_mand = 1;
		for (ch = _C(object)->children.next; ch != &_C(object)->children; ch = ch->next) {
			CHECK_FCT(  parserules_do ( dict, _C(ch->o), error_info, is_child_mand )  );
		}
	}

	/* Now check all rules of this object */
	data.sentinel = &_C(object)->children;
	data.pei  = error_info;
	CHECK_FCT( fd_dict_iterate_rules ( model, &data, parserules_check_one_rule ) );
	
	return 0;
}

int fd_msg_parse_rules ( msg_or_avp * object, struct dictionary * dict, struct fd_pei *error_info)
{
	TRACE_ENTRY("%p %p %p", object, dict, error_info);
	
	if (error_info)
		memset(error_info, 0, sizeof(struct fd_pei));
	
	/* Resolve the dictionary objects when missing. This also validates the object. */
	CHECK_FCT(  fd_msg_parse_dict ( object, dict, error_info )  );
	
	/* Call the recursive function */
	return parserules_do ( dict, object, error_info, 1 ) ;
}

/***************************************************************************************************************/

/* Compute the lengh of an object and its subtree. */
int fd_msg_update_length ( msg_or_avp * object )
{
	size_t sz = 0;
	struct dict_object * model;
	union {
		struct dict_cmd_data   cmddata;
		struct dict_avp_data   avpdata;
	} dictdata;
	
	TRACE_ENTRY("%p", object);
	
	/* Get the model of the object. This also validates the object */
	CHECK_FCT( fd_msg_model ( object, &model ) );
	
	/* Get the information of the model */
	if (model) {
		CHECK_FCT(  fd_dict_getval(model, &dictdata)  );
	} else {
		/* For unknown AVP, just don't change the size */
		if (_C(object)->type == MSG_AVP)
			return 0;
	}
	
	/* Deal with easy cases: AVPs without children */
	if ((_C(object)->type == MSG_AVP) && (dictdata.avpdata.avp_basetype != AVP_TYPE_GROUPED)) {
		/* Sanity check */
		ASSERT(FD_IS_LIST_EMPTY(&_A(object)->avp_chain.children));
		
		/* Now check that the data is set in the AVP */
		CHECK_PARAMS(  _A(object)->avp_public.avp_value  );
		
		sz = GETAVPHDRSZ( _A(object)->avp_public.avp_flags );
		
		switch (dictdata.avpdata.avp_basetype) {
			case AVP_TYPE_OCTETSTRING:
				sz += _A(object)->avp_public.avp_value->os.len;
				break;
			
			case AVP_TYPE_INTEGER32:
			case AVP_TYPE_INTEGER64:
			case AVP_TYPE_UNSIGNED32:
			case AVP_TYPE_UNSIGNED64:
			case AVP_TYPE_FLOAT32:
			case AVP_TYPE_FLOAT64:
				sz += avp_value_sizes[dictdata.avpdata.avp_basetype];
				break;
			
			default:
				/* Something went wrong... */
				ASSERT(0);
		}
	}
	else  /* message or grouped AVP */
	{
		struct fd_list * ch = NULL;
		
		/* First, compute the header size */
		if (_C(object)->type == MSG_AVP) {
			sz = GETAVPHDRSZ( _A(object)->avp_public.avp_flags );
		} else {
			sz = GETMSGHDRSZ( );
		}
		
		/* Recurse in all children and update the sz information */
		for (ch = _C(object)->children.next; ch != &_C(object)->children; ch = ch->next) {
			CHECK_FCT(  fd_msg_update_length ( ch->o )  );
			
			/* Add the padded size to the parent */
			sz += PAD4( _A(ch->o)->avp_public.avp_len );
		}
	}
	
	/* When we arrive here, the "sz" variable contains the size to write in the object */
	if (_C(object)->type == MSG_AVP) 
		_A(object)->avp_public.avp_len = sz;
	else
		_M(object)->msg_public.msg_length = sz;
	
	return 0;
}

/***************************************************************************************************************/
/* Macro to check if further callbacks must be called */
#define TEST_ACTION_STOP()					\
	if ((*msg == NULL) || (*action != DISP_ACT_CONT))	\
		goto out;

/* Call all dispatch callbacks for a given message */
int fd_msg_dispatch ( struct msg ** msg, struct session * session, enum disp_action *action, const char ** error_code)
{
	struct dictionary  * dict;
	struct dict_object * app;
	struct dict_object * cmd;
	struct avp * avp;
	struct fd_list * cb_list;
	int ret = 0, r2;
	
	TRACE_ENTRY("%p %p %p %p", msg, session, action, error_code);
	CHECK_PARAMS( msg && CHECK_MSG(*msg) && action);
	
	if (error_code)
		*error_code = NULL;
	*action = DISP_ACT_CONT;
	
	/* Take the dispatch lock */
	CHECK_FCT( pthread_rwlock_rdlock(&fd_disp_lock) );
	pthread_cleanup_push( fd_cleanup_rwlock, &fd_disp_lock );
	
	/* First, call the DISP_HOW_ANY callbacks */
	CHECK_FCT_DO( ret = fd_disp_call_cb_int( NULL, msg, NULL, session, action, NULL, NULL, NULL, NULL ), goto out );

	TEST_ACTION_STOP();
	
	/* If we don't know the model at this point, we stop cause we cannot get the dictionary. It's invalid: an error should already have been trigged by ANY callbacks */
	CHECK_PARAMS_DO(cmd = (*msg)->msg_model, { ret = EINVAL; goto out; } );
	
	/* Now resolve message application */
	CHECK_FCT_DO( ret = fd_dict_getdict( cmd, &dict ), goto out );
	CHECK_FCT_DO( ret = fd_dict_search( dict, DICT_APPLICATION, APPLICATION_BY_ID, &(*msg)->msg_public.msg_appl, &app, 0 ), goto out );
	
	if (app == NULL) {
		if ((*msg)->msg_public.msg_flags & CMD_FLAG_REQUEST) {
			if (error_code)
				*error_code = "DIAMETER_APPLICATION_UNSUPPORTED";
			*action = DISP_ACT_ERROR;
		} else {
			fd_msg_log( FD_MSG_LOG_DROPPED, *msg, "Internal error: Received this answer to a local query with an unsupported application %d", (*msg)->msg_public.msg_appl);
			fd_msg_free(*msg);
			*msg = NULL;
		}
		goto out;
	}
	
	/* So start browsing the message */
	CHECK_FCT_DO( ret = fd_msg_browse( *msg, MSG_BRW_FIRST_CHILD, &avp, NULL ), goto out );
	while (avp != NULL) {
		/* For unknown AVP, we don't have a callback registered, so just skip */
		if (avp->avp_model) {
			struct dict_object * enumval = NULL;
			
			/* Get the list of callback for this AVP */
			CHECK_FCT_DO( ret = fd_dict_disp_cb(DICT_AVP, avp->avp_model, &cb_list), goto out );
			
			/* We search enumerated values only in case of non-grouped AVP */
			if ( avp->avp_public.avp_value ) {
				struct dict_object * type;
				/* Check if the AVP has a constant value */
				CHECK_FCT_DO( ret = fd_dict_search(dict, DICT_TYPE, TYPE_OF_AVP, avp->avp_model, &type, 0), goto out );
				if (type) {
					struct dict_enumval_request req;
					memset(&req, 0, sizeof(struct dict_enumval_request));
					req.type_obj = type;
					memcpy( &req.search.enum_value, avp->avp_public.avp_value, sizeof(union avp_value) );
					CHECK_FCT_DO( ret = fd_dict_search(dict, DICT_ENUMVAL, ENUMVAL_BY_STRUCT, &req, &enumval, 0), goto out );
				}
			}
			
			/* Call the callbacks */
			CHECK_FCT_DO( ret = fd_disp_call_cb_int( cb_list, msg, avp, session, action, app, cmd, avp->avp_model, enumval ), goto out );
			TEST_ACTION_STOP();
		}
		/* Go to next AVP */
		CHECK_FCT_DO(  ret = fd_msg_browse( avp, MSG_BRW_WALK, &avp, NULL ), goto out );
	}
		
	/* Now call command and application callbacks */
	CHECK_FCT_DO( ret = fd_dict_disp_cb(DICT_COMMAND, cmd, &cb_list), goto out );
	CHECK_FCT_DO( ret = fd_disp_call_cb_int( cb_list, msg, NULL, session, action, app, cmd, NULL, NULL ), goto out );
	TEST_ACTION_STOP();
	
	if (app) {
		CHECK_FCT_DO( ret = fd_dict_disp_cb(DICT_APPLICATION, app, &cb_list), goto out );
		CHECK_FCT_DO( ret = fd_disp_call_cb_int( cb_list, msg, NULL, session, action, app, cmd, NULL, NULL ), goto out );
		TEST_ACTION_STOP();
	}
out:
	; /* some systems would complain without this */	
	pthread_cleanup_pop(0);
	
	CHECK_POSIX_DO(r2 = pthread_rwlock_unlock(&fd_disp_lock), /* ignore */ );
	return ret ?: r2;
}

