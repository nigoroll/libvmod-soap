/*
 * Copyright (c) 2019, Refinitiv
 * Copyright 2022 UPLEX - Nils Goroll Systemoptimierung
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <cache/cache.h>
#include <vcl.h>
#include <vsb.h>

#include "vmod_soap.h"
#include "vcc_soap_if.h"

#define POOL_KEY "VRN_IH_PK"

static int		refcount = 0;
static apr_pool_t	*apr_pool = NULL;

enum soap_state {
	NONE = 0,
	INIT,
	HEADER_DONE,      // Header element completely read
	ACTION_AVAILABLE, // Body parsing is started and action name and namespace available
	BODY_DONE,        // Body element completely read
	FAILED
};

/* -------------------------------------------------------------------------------------/
   init module
*/
static void init_apr()
{
	AZ(apr_pool);
	XXXAZ(apr_initialize());
	XXXAZ(apr_pool_create(&apr_pool, NULL));
}

static void clean_apr()
{
	apr_pool = NULL;
	apr_terminate();
}

/* -------------------------------------------------------------------------------------/
   init vcl
*/
static void clean_vcl(VRT_CTX, void *priv)
{
	struct priv_soap_vcl *priv_soap_vcl;
	struct soap_namespace *ns, *ns2;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CAST_OBJ_NOTNULL(priv_soap_vcl, priv, PRIV_SOAP_VCL_MAGIC);

	VSLIST_FOREACH_SAFE(ns, &priv_soap_vcl->namespaces, list, ns2) {
		VSLIST_REMOVE_HEAD(&priv_soap_vcl->namespaces, list);
		FREE_OBJ(ns);
	}

	FREE_OBJ(priv_soap_vcl);
}

static struct priv_soap_vcl* init_vcl()
{
	struct priv_soap_vcl *priv_soap_vcl;

	ALLOC_OBJ(priv_soap_vcl, PRIV_SOAP_VCL_MAGIC);
	XXXAN(priv_soap_vcl);

	VSLIST_INIT(&priv_soap_vcl->namespaces);
	return priv_soap_vcl;
}

/* ------------------------------------------------------------------/
   initialize session
*/

#define TASK_ALLOC_OBJ(ctx, ptr, magic) do {			\
	assert(sizeof *(ptr) <= UINT_MAX);			\
	ptr = WS_Alloc((ctx)->ws, sizeof *(ptr));		\
	if ((ptr) == NULL)					\
		VRT_fail(ctx, "Out of workspace for " #magic);	\
	else							\
		INIT_OBJ(ptr, magic);				\
} while(0)

static struct priv_soap_task* init_task(VRT_CTX)
{
	struct priv_soap_task *soap_task;

	TASK_ALLOC_OBJ(ctx, soap_task, PRIV_SOAP_TASK_MAGIC);
	if (! soap_task)
		return (NULL);

	soap_task->ctx = ctx;

	TASK_ALLOC_OBJ(ctx, soap_task->req_xml, SOAP_REQ_XML_MAGIC);
	if (! soap_task->req_xml)
		return (NULL);

	XXXAZ(apr_pool_create(&soap_task->pool, apr_pool));

	VSLb(soap_task->ctx->vsl, SLT_Debug, "init_task");
	return soap_task;
}

/* -----------------------------------------------------------------/
   destroy session
*/
static void clean_task(VRT_CTX, void *priv)
{
	struct priv_soap_task *soap_task;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CAST_OBJ_NOTNULL(soap_task, priv, PRIV_SOAP_TASK_MAGIC);

	clean_req_xml(soap_task->req_xml);
	INIT_OBJ(soap_task->req_xml, SOAP_REQ_XML_MAGIC);

	INIT_OBJ(soap_task->req_xml, SOAP_REQ_XML_MAGIC);

	AN(soap_task->pool);
	apr_pool_destroy(soap_task->pool);

	INIT_OBJ(soap_task, PRIV_SOAP_TASK_MAGIC);
}

static int
process_init_read(struct priv_soap_task *task)
{

	AN(task);
	assert(task->state == NONE);

	task->req_xml->pool = task->pool;
	task->req_xml->ctx = task->ctx;
	init_req_xml(task->req_xml);

	task->state = INIT;
	task->vrb_what = VRB_CACHED;
	return (0);
}

int v_matchproto_(objiterate_f)
read_iter_f(void *priv, unsigned flush, const void *ptr, ssize_t len)
{
	struct priv_soap_task *task;
	int err;

	CAST_OBJ_NOTNULL(task, priv, PRIV_SOAP_TASK_MAGIC);

	if (task->state == FAILED)
		return (0);

	assert(task->state == INIT ||
	    task->state == HEADER_DONE ||
	    task->state == ACTION_AVAILABLE);

	err = soap_iter_f(task->req_xml, flush, ptr, len);

	// we never fail the iterator for custom error handling from vcl
	if (err < 0)
		task->state = FAILED;
	else if (task->req_xml->body)
		task->state = BODY_DONE;
	else if (task->req_xml->action_namespace && task->req_xml->action_name)
		task->state = ACTION_AVAILABLE;
	else if (task->req_xml->header)
		task->state = HEADER_DONE;

	return (0);
}

int process_request(struct priv_soap_task *task, enum soap_state state,
    unsigned can_eat)
{
	enum soap_state old;
	int r;

	if (task->ctx->req->req_body_status == BS_NONE)
		return (-1);
	while (task->state < state) {
		old = task->state;
		VSLb(task->ctx->vsl, SLT_Debug,
		    "process_request 0: %d/%d", old, state);
		switch (old) {
		case NONE:  // init
			r = process_init_read(task);
			if (r)
				return (r);
			break;
		case INIT:
		case HEADER_DONE:
		case ACTION_AVAILABLE:
			r = VRB_Iterate(task->ctx->req->wrk, task->ctx->vsl,
			    task->ctx->req, read_iter_f, task, task->vrb_what);
			if (task->vrb_what == VRB_CACHED && can_eat) {
				task->vrb_what = VRB_REMAIN;
				continue;
			}
			/* error or no progress */
			if (r == 0 && old == task->state)
				r = -1;
			if (r) {
				task->state = FAILED;
				return (r);
			}
			break;
		case BODY_DONE:  // read from memory
			VSLb(task->ctx->vsl, SLT_Debug, "process_request 8: %d/%d", old, state);
			break;
		case FAILED:
			break;
		default:
			WRONG("task->state");
		}
	}
	VSLb(task->ctx->vsl, SLT_Debug, "process_request .: %d/%d", task->state, state);
	return (task->state == FAILED);
}

static const struct vmod_priv_methods priv_soap_vcl_methods[1] = {{
	.magic = VMOD_PRIV_METHODS_MAGIC,
	.type = "soap priv_vcl",
	.fini = clean_vcl
}};

/*
 * handle vmod internal state, vmod init/fini and/or varnish callback
 * (un)registration here.
 *
 */
int v_matchproto_(vmod_event_f)
	VPFX(event_function)(VRT_CTX, struct vmod_priv *priv /* PRIV_VCL */,
	    enum vcl_event_e e)
{
	struct priv_soap_vcl *priv_soap_vcl;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	switch (e) {
	case VCL_EVENT_LOAD:
		if (! xmlHasFeature(XML_WITH_THREAD)) {
			VRT_fail(ctx, "Need libxml2 with threads support");
			return (1);
		}
		if(0 == refcount++) {
			init_xml();
			init_apr();
		}

		priv_soap_vcl = init_vcl();
		priv->priv = priv_soap_vcl;
		priv->methods = priv_soap_vcl_methods;
		break;
	case VCL_EVENT_WARM:
		break;
	case VCL_EVENT_COLD:
		break;
	case VCL_EVENT_DISCARD:
		if(0 == --refcount) {
			clean_xml();
			clean_apr();
		}
		break;
	default:
		return (0);
	}
	return (0);
}

static const struct vmod_priv_methods priv_soap_task_methods[1] = {{
	.magic = VMOD_PRIV_METHODS_MAGIC,
	.type = "soap priv_task",
	.fini = clean_task
}};

sess_record* priv_soap_get(VRT_CTX, struct vmod_priv *priv /* PRIV_TASK */)
{
	struct priv_soap_task *soap_task;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(priv);
	if(priv->priv == NULL) {
		priv->priv = init_task(ctx);
		priv->methods = priv_soap_task_methods;
	}
	CAST_OBJ_NOTNULL(soap_task, priv->priv, PRIV_SOAP_TASK_MAGIC);
	if(soap_task->ctx != ctx) {
		soap_task->ctx = ctx;
		if(soap_task->req_xml) {
			soap_task->req_xml->ctx = ctx;
		}
	}
	return (soap_task);
}

VCL_BOOL v_matchproto_(td_soap_is_valid)
	vmod_is_valid(VRT_CTX, struct vmod_priv *priv /* PRIV_TASK */)
{
	struct priv_soap_task *soap_task = priv_soap_get(ctx, priv);

	return (process_request(soap_task, ACTION_AVAILABLE, 1) == 0);
}

VCL_STRING v_matchproto_(td_soap_action)
	vmod_action(VRT_CTX, struct vmod_priv *priv /* PRIV_TASK */)
{
	struct priv_soap_task *soap_task = priv_soap_get(ctx, priv);
	if(process_request(soap_task, ACTION_AVAILABLE, 1) == 0) {
		return (soap_task->req_xml->action_name);
	}
	return ("");
}

VCL_STRING v_matchproto_(td_soap_action_namespace)
	vmod_action_namespace(VRT_CTX, struct vmod_priv *priv /* PRIV_TASK */)
{
	struct priv_soap_task *soap_task = priv_soap_get(ctx, priv);
	if(process_request(soap_task, ACTION_AVAILABLE, 1) == 0) {
		return (soap_task->req_xml->action_namespace);
	}
	return ("");
}

VCL_VOID v_matchproto_(td_soap_add_namespace)
	vmod_add_namespace(VRT_CTX, struct vmod_priv *priv /* PRIV_VCL */, VCL_STRING prefix, VCL_STRING uri)
{
	struct priv_soap_vcl	    *priv_soap_vcl;
	struct soap_namespace	    *namespace;

	AN(priv);
	CAST_OBJ_NOTNULL(priv_soap_vcl, priv->priv, PRIV_SOAP_VCL_MAGIC);
	ALLOC_OBJ(namespace, PRIV_SOAP_NAMESPACE_MAGIC);
	AN(namespace);

	namespace->prefix = prefix;
	namespace->uri = uri;
	VSLIST_INSERT_HEAD(&priv_soap_vcl->namespaces, namespace, list);
}

VCL_STRING v_matchproto_(td_soap_xpath_header)
	vmod_xpath_header(VRT_CTX, struct vmod_priv *priv_vcl /* PRIV_VCL */, struct vmod_priv *priv_task /* PRIV_TASK */, VCL_STRING xpath)
{
	struct priv_soap_vcl *soap_vcl;;
	struct priv_soap_task *soap_task;

	AN(priv_vcl);
	CAST_OBJ_NOTNULL(soap_vcl, priv_vcl->priv, PRIV_SOAP_VCL_MAGIC);

	AN(priv_task);
	soap_task = priv_soap_get(ctx, priv_task);

	if(process_request(soap_task, HEADER_DONE, 1) == 0) {
		return (evaluate_xpath(soap_vcl, soap_task, soap_task->req_xml->header, xpath));
	}
	return ("");
}

VCL_STRING v_matchproto_(td_soap_xpath_body)
	vmod_xpath_body(VRT_CTX, struct vmod_priv *priv_vcl /* PRIV_VCL */, struct vmod_priv *priv_task /* PRIV_TASK */, VCL_STRING xpath)
{
	struct priv_soap_vcl *soap_vcl;;
	struct priv_soap_task *soap_task;

	AN(priv_vcl);
	CAST_OBJ_NOTNULL(soap_vcl, priv_vcl->priv, PRIV_SOAP_VCL_MAGIC);

	AN(priv_task);
	soap_task = priv_soap_get(ctx, priv_task);

	if(process_request(soap_task, BODY_DONE, 1) == 0) {
		return (evaluate_xpath(soap_vcl, soap_task, soap_task->req_xml->body, xpath));
	}
	return ("");
}

VCL_VOID v_matchproto_(td_soap_synthetic)
	vmod_synthetic(VRT_CTX, struct vmod_priv *priv_task /* PRIV_TASK */, VCL_INT soap_code, VCL_STRING soap_message)
{
	struct priv_soap_task *soap_task;

	AN(priv_task);
	soap_task = priv_soap_get(ctx, priv_task);

	synth_soap_fault(soap_task->req_xml, soap_code, soap_message);
}

/*
 * ============================================================
 *
 * Object Interface (rework)
 */

static void
soap_vsl_generic_error(void *priv, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	VSLbv(priv, SLT_Error, fmt, ap);
	va_end(ap);
}

static void
soap_vsl_structured_error(void *priv, xmlErrorPtr error)
{

	AN(error);
	VSLb(priv, SLT_Error, "xml: domain=%d, code=%d, msg=%s",
	    error->domain, error->code, error->message);
}

static void
soap_vsb_generic_error(void *priv, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	VSB_vprintf(priv, fmt, ap);
	va_end(ap);
}

static void
soap_vsb_structured_error(void *priv, xmlErrorPtr error)
{

	AN(error);
	VSB_printf(priv, "xml: domain=%d, code=%d, msg=%s",
	    error->domain, error->code, error->message);
}

static void
soap_init_thread(VRT_CTX)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (ctx->vsl != NULL) {
		xmlSetGenericErrorFunc(ctx->vsl, soap_vsl_generic_error);
		xmlSetStructuredErrorFunc(ctx->vsl, soap_vsl_structured_error);
	} else {
		AN(ctx->msg);
		xmlSetGenericErrorFunc(ctx->msg, soap_vsb_generic_error);
		xmlSetStructuredErrorFunc(ctx->msg, soap_vsb_structured_error);
	}
}

enum soap_source {
	SOAPS_INVALID = 0,
	SOAPS_REQ_BODY,
	SOAPS_RESP_BODY
};

struct VPFX(soap_parser) {
	unsigned			magic;
#define SOAP_PARSER_MAGIC		0x017ce81e
	unsigned			can_VRB_REMAIN:1;
	enum soap_source		source;
	char				*vcl_name;
	VSLIST_HEAD(, soap_namespace)	namespaces;
};

VCL_VOID
vmod_parser__init(VRT_CTX, struct VPFX(soap_parser) **soapp,
    const char *vcl_name, struct VARGS(parser__init)*args)
{
	struct VPFX(soap_parser) *soap;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(soapp);
	AZ(*soapp);
	AN(vcl_name);
	AN(args);

	soap_init_thread(ctx);

	ALLOC_OBJ(soap, SOAP_PARSER_MAGIC);
	AN(soap);

	if (args->source == VENUM(req_body)) {
		soap->source = SOAPS_REQ_BODY;
		if (! args->valid_req_body) {
			VRT_fail(ctx, "new %s: req_body argument "
			    "is required with source=req_body",
			    vcl_name);
			vmod_parser__fini(&soap);
			return;
		}
		soap->can_VRB_REMAIN = (args->req_body == VENUM(all));
	} else if (args->source == VENUM(resp_body)) {
		soap->source = SOAPS_RESP_BODY;
	} else {
		WRONG("source argument");
	}
	REPLACE(soap->vcl_name, vcl_name);
	VSLIST_INIT(&soap->namespaces);
	*soapp = soap;
}

VCL_VOID
vmod_parser__fini(struct VPFX(soap_parser) **soapp)
{
	struct VPFX(soap_parser) *soap;
	struct soap_namespace *ns, *ns2;

	TAKE_OBJ_NOTNULL(soap, soapp, SOAP_PARSER_MAGIC);
	REPLACE(soap->vcl_name, NULL);

	// ref clean_vcl
	VSLIST_FOREACH_SAFE(ns, &soap->namespaces, list, ns2) {
		VSLIST_REMOVE_HEAD(&soap->namespaces, list);
		FREE_OBJ(ns);
	}

	FREE_OBJ(soap);
}

VCL_VOID
vmod_parser_add_namespace(VRT_CTX,
    struct VPFX(soap_parser) *soap, VCL_STRING prefix, VCL_STRING uri)
{
	struct soap_namespace *ns;

	CHECK_OBJ_NOTNULL(soap, SOAP_PARSER_MAGIC);

	if (ctx->method != VCL_MET_INIT) {
		VRT_fail(ctx, "%s.add_namespace() may only be called "
		    "from vcl_init{}", soap->vcl_name);
		return;
	}

	if (prefix == NULL || *prefix == '\0' ||
	    uri == NULL || *uri == '\0') {
		VRT_fail(ctx, "%s.add_namespace: prefix or uri "
		    "empty", soap->vcl_name);
		return;
	}

	//// called in _init()
	// soap_init_thread(ctx->vsl);

	if (test_ns(prefix, uri)) {
		// XXX no idea if this can ever fail
		VRT_fail(ctx, "%s.add_namespace: validation failed",
		    soap->vcl_name);
		return;
	}

	// ref vmod_add_namespace
	ALLOC_OBJ(ns, PRIV_SOAP_NAMESPACE_MAGIC);
	AN(ns);

	ns->prefix = prefix;
	ns->uri = uri;
	VSLIST_INSERT_HEAD(&soap->namespaces, ns, list);
}

sess_record *
obj_priv_soap_get(VRT_CTX, const struct VPFX(soap_parser) *soap)
{
	struct vmod_priv *priv;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(soap, SOAP_PARSER_MAGIC);

	soap_init_thread(ctx);

	priv = VRT_priv_task(ctx, soap);
	if (priv == NULL) {
		VRT_fail(ctx, "No priv_task");
		return (NULL);
	}

	return (priv_soap_get(ctx, priv));
}

VCL_STRING
vmod_parser_header_xpath(VRT_CTX,
    struct VPFX(soap_parser) *soap, VCL_STRING xpath)
{
	struct priv_soap_task *soap_task = obj_priv_soap_get(ctx, soap);

	if (soap_task == NULL)
		return (NULL);
	AN(xpath);
	return (NULL);
}

VCL_STRING
vmod_parser_body_xpath(VRT_CTX,
    struct VPFX(soap_parser) *soap, VCL_STRING xpath)
{
	struct priv_soap_task *soap_task = obj_priv_soap_get(ctx, soap);

	if (soap_task == NULL)
		return (NULL);
	AN(xpath);
	return (NULL);
}
