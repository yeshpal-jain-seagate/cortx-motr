/* -*- C -*- */
/*
 * Copyright (c) 2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


/**
 * Mapping for m0net-->libfabric API
 * xo_dom_init                    =  fi_getinfo, fi_fabric, fi_domain
 * xo_dom_fini                    =  fi_close, // free resources
 * xo_tm_init                     =  // Empty function ? fi_send
 * xo_tm_confine                  =
 * xo_tm_start                    =  // fi_send
 * xo_tm_stop                     =  fi_cancel
 * xo_tm_fini                     =  // Empty function
 * xo_end_point_create            =  fi_endpoint, fi_pep, fi_av, fi_cq, fi_cntr, fi_eq, fi_bind(av/cq/cntr/eq), fi_pep_bind
 * xo_buf_register                =  fi_mr_reg, fi_mr_desc, fi_mr_key, fi_mr_bind, fi_mr_enable
 * xo_buf_deregister              =  fi_close
 * xo_buf_add                     =  fi_send/fi_recv
 * xo_buf_del                     =  fi_cancel 
 * xo_bev_deliver_sync            =  Is it needed?
 * xo_bev_deliver_all             =  Is it needed?
 * xo_bev_pending                 =  Is it needed?
 * xo_bev_notify                  =  Is it needed?
 * xo_get_max_buffer_size         =  // need to define new API
 * xo_get_max_buffer_segment_size =  // need to define new functions
 * xo_get_max_buffer_segments     =  // need to define new functions
 * xo_get_max_buffer_desc_size    =  
 * 
 */

/**
 * @addtogroup netlibfab
 *
 * @{
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_NET
#include "lib/trace.h"          /* M0_ENTRY() */
#include <netinet/in.h>         /* INET_ADDRSTRLEN */
#include <arpa/inet.h>          /* inet_pton, htons */
#include "net/net.h"            /* struct m0_net_domain */
#include "lib/memory.h"         /* M0_ALLOC_PTR()*/
#include "libfab_internal.h"    /* struct m0_fab__dom_param */

/** Parameters required for libfabric configuration */
enum m0_fab__mr_params {
	/** Fabric memory access. */
	FAB_MR_ACCESS  = (FI_READ | FI_WRITE | FI_RECV | FI_SEND | \
			FI_REMOTE_READ | FI_REMOTE_WRITE),
	/** Fabric memory offset. */
	FAB_MR_OFFSET  = 0,
	/** Fabric memory flag. */
	FAB_MR_FLAG    = 0,
	/** Key used for memory registration. */
	FAB_MR_KEY     = 0XABCD,
};

enum PORT_SOCK_TYPE {
        PORTFAMILYMAX = 3,
        SOCKTYPEMAX   = 2
};

static const char portf[PORTFAMILYMAX][10]  = { "unix", "inet", "inet6" };
static const char socktype[SOCKTYPEMAX][10] = { "stream", "dgram" };

/* libfab init and fini() : initialized in motr init */
M0_INTERNAL int m0_net_libfab_init(void)
{
	int result = 0;

	
	/*  TODO: Uncomment it when all the changes are intergated
	*  commnet to avoid compilation ERROR 
	*  m0_net_xprt_register(&m0_net_libfab_xprt);
	*  m0_net_xprt_default_set(&m0_net_libfab_xprt);
	*/
	return M0_RC(result);
}

M0_INTERNAL void m0_net_libfab_fini(void)
{
	/*  TODO: Uncomment it when all the changes are intergated
	*  commnet to avoid compilation ERROR 
	*	m0_net_xprt_deregister(&m0_net_libfab_xprt);
	*/
}

/**
 * Bitmap of used transfer machine identifiers.
 *
 * This is used to allocate unique transfer machine identifiers for LNet network
 * addresses with wildcard transfer machine identifier (like
 * "192.168.96.128@tcp1:12345:31:*").
 *
 */
static char fab_autotm[1024] = {};

static int libfab_ep_addr_decode_lnet(const char *name, char *node,
                                      size_t nodeSize, char *port,
                                      size_t portSize)
{
	char               *at       = strchr(name, '@');
	int                 nr;
	unsigned            pid;
	unsigned            portal;
	unsigned            portnum;
	unsigned            tmid;
	char		   *lp       = "127.0.0.1";

	if (strncmp(name, "0@lo", 4) == 0) {
		M0_PRE(nodeSize >= ((strlen(lp)+1)) );
		memcpy(node, lp, (strlen(lp)+1));
	} else {
		if (at == NULL || at - name >= nodeSize)
			return M0_ERR(-EPROTO);

		M0_PRE(nodeSize >= (at-name)+1);
		memcpy(node, name, at - name);
	}
	if ((at = strchr(at, ':')) == NULL) /* Skip 'tcp...:' bit. */
		return M0_ERR(-EPROTO);
	nr = sscanf(at + 1, "%u:%u:%u", &pid, &portal, &tmid);
	if (nr != 3) {
		nr = sscanf(at + 1, "%u:%u:*", &pid, &portal);
		if (nr != 2)
			return M0_ERR(-EPROTO);
		for (nr = 0; nr < ARRAY_SIZE(fab_autotm); ++nr) {
			if (fab_autotm[nr] == 0) {
				tmid = nr;
				break;
			}
		}
		if (nr == ARRAY_SIZE(fab_autotm))
			return M0_ERR(-EADDRNOTAVAIL);
	}
	/*
	* Hard-code LUSTRE_SRV_LNET_PID to avoid dependencies on the Lustre
	* headers.
	*/
	if (pid != 12345)
		return M0_ERR(-EPROTO);
	/*
	* Deterministically combine portal and tmid into a unique 16-bit port
	* number (greater than 1024). Tricky.
	*
	* Port number is, in binary: tttttttttt1ppppp, that is, 10 bits of tmid
	* (which must be less than 1024), followed by a set bit (guaranteeing
	* that the port is not reserved), followed by 5 bits of (portal - 30),
	* so that portal must be in the range 30..61.
	*/
	if (tmid >= 1024 || (portal - 30) >= 32)
		return M0_ERR_INFO(-EPROTO,
			"portal: %u, tmid: %u", portal, tmid);

	portnum  = htons(tmid | (1 << 10) | ((portal - 30) << 11));
	sprintf(port, "%d", portnum);
	fab_autotm[tmid] = 1;
	return M0_RC(0);
}

static int libfab_ep_addr_decode_sock(const char *ep_name, char *node,
                                      size_t nodeSize, char *port,
                                      size_t portSize)
{
        int   shift;
        int   f;
        int   s;
        char *at;

        for (f = 0; f < PORTFAMILYMAX ; ++f) {
                if (portf[f]!= NULL) {
                        shift = strlen(portf[f]);
                        if (strncmp(ep_name, portf[f], shift) == 0)
                                break;
                }
        }
        if (ep_name[shift] != ':')
                return M0_ERR(-EINVAL);
        ep_name += shift + 1;
        for (s = 0; s < SOCKTYPEMAX; ++s) {
                if (socktype[s] != NULL) {
                        shift = strlen(socktype[s]);
                        if (strncmp(ep_name, socktype[s], shift) == 0)
                                break;
                }
        }
        if (ep_name[shift] != ':')
                return M0_ERR(-EINVAL);
        ep_name += shift + 1;
        at = strchr(ep_name, '@');
        if (at == NULL) {
                /* XXX @todo: default port? */
                return M0_ERR(-EINVAL);
        } else {
                at++;
                if (at == NULL)
                        return M0_ERR(-EINVAL);
                M0_PRE(portSize >= (strlen(at)+1));
                memcpy(port,at,(strlen(at)+1));
        }
        M0_PRE(nodeSize >= (at - ep_name));
        memcpy(node, ep_name, ((at - ep_name)-1));
        return 0;
}


/**
 * Used to take the ip and port from the given end point
 * ep_name : endpoint address from domain
 * node    : copy ip address from ep_name
 * port    : copy port number from ep_name
 * Example of ep_name IPV4 192.168.0.1:4235
 *                    IPV6 [4002:db1::1]:4235
 */
static int libfab_ep_addr_decode_native(const char *ep_name, char *node,
			                size_t nodeSize, char *port,
                                        size_t portSize)
{
	char     *cp;
	size_t    n;
	int       rc = 0;

	M0_PRE(ep_name != NULL);

	M0_ENTRY("ep_name=%s", ep_name);

	if( ep_name[0] == '[' ) {
		/* IPV6 pattern */
		cp = strchr(ep_name, ']');
		if (cp == NULL)
			return M0_ERR(-EINVAL);

		ep_name++;
		n = cp - ep_name;
		if (n == 0 )
			return M0_ERR(-EINVAL);
		cp++;
		if (*cp != ':')
		return M0_ERR(-EINVAL);
		cp++;
	}
	else {
		/* IPV4 pattern */
		cp = strchr(ep_name, ':');
		if (cp == NULL)
			return M0_ERR(-EINVAL);

		n = cp - ep_name;
		if (n == 0 )
			return M0_ERR(-EINVAL);

		++cp;
	}

	M0_PRE(nodeSize >= (n+1));
	M0_PRE(portSize >= (strlen(cp)+1));

	memcpy(node, ep_name, n);
	node[n] = 0;

	n=strlen(cp);
	memcpy(port, cp, n);
	port[n] = 0;

	return M0_RC(rc);
}

/**
 * Parses network address.
 *
 * The following address formats are supported:
 *
 *     - lnet compatible, see nlx_core_ep_addr_decode():
 *
 *           nid:pid:portal:tmid
 *
 *       for example: "10.0.2.15@tcp:12345:34:123" or
 *       "192.168.96.128@tcp1:12345:31:*"
 *
 *     - sock format, see socket(2):
 *           family:type:ipaddr[@port]
 *
 *
 *     - libfab compatible format
 *       for example IPV4 libfab:192.168.0.1:4235
 *                   IPV6 libfab:[4002:db1::1]:4235
 *
 */
static int libfab_ep_addr_decode(const char *name, char *node,
			size_t nodeSize, char *port, size_t portSize)
{
	int result;

	if( name != NULL || name[0] == 0)
		result =  M0_ERR(-EPROTO);
	else if((strncmp(name,"libfab",6))==0)
		result = libfab_ep_addr_decode_native(name, node,
                                                      nodeSize, port, portSize);
	else if (name[0] < '0' || name[0] > '9')
                /* sock format */
		result = libfab_ep_addr_decode_sock(name, node,
						    nodeSize, port, portSize);
	else
		/* Lnet format. */
		result = libfab_ep_addr_decode_lnet(name, node,
						    nodeSize, port, portSize);
	return M0_RC(result);
}


/** Used as m0_net_xprt_ops::xo_dom_init(). */
static int libfab_dom_init(struct m0_net_xprt *xprt, struct m0_net_domain *dom)
{
	struct m0_fab__dom_param *fab_dom;
	struct fi_info           *fab_hints;
	int 			  rc;
	char                      node[INET6_ADDRSTRLEN];
	char                      port[8];
	char                      *ep_name = "127.0.0.1:42365";

	M0_ENTRY();
	
	M0_ALLOC_PTR(fab_dom);
	if (fab_dom == NULL)
		return M0_ERR(-ENOMEM);

	fab_hints = fi_allocinfo();
	if (fab_hints == NULL) {
		 m0_free(fab_dom);
		return M0_ERR(-ENOMEM); 
	}

	/*
	* TODO: Added for future use
	* fab_hints->ep_attr->type = FI_EP_RDM;
	* fab_hints->caps = FI_MSG;
	* fab_hints->fabric_attr->prov_name = "verbs";
	*/
	/* TODO - adding this decode API until actual args are
	 * passsed from tm
	 */
	libfab_ep_addr_decode(ep_name, node, ARRAY_SIZE(node), 
			      port,ARRAY_SIZE(port));

	rc = fi_getinfo(FI_VERSION(FI_MAJOR_VERSION,FI_MINOR_VERSION),
			NULL, NULL, 0, fab_hints,
			&fab_dom->fdp_fi);
	if (rc == FI_SUCCESS) {
		rc = fi_fabric(fab_dom->fdp_fi->fabric_attr,
			       &fab_dom->fdp_fabric, NULL);
		if (rc == FI_SUCCESS) {
			rc = fi_domain(fab_dom->fdp_fabric,
				       fab_dom->fdp_fi,
				       &fab_dom->fdp_domain, NULL);
			if (rc == FI_SUCCESS)
				dom->nd_xprt_private = fab_dom;
		}
	}

	if ( rc != FI_SUCCESS)
	{
		m0_free(fab_dom);
	}
	fi_freeinfo(fab_hints);

	return M0_RC(rc);
}

static void libfab_fab_param_free(struct m0_fab__dom_param *fab_dom)
{
	int    ret;

	if (fab_dom->fdp_domain != NULL) {
		ret = fi_close(&(fab_dom->fdp_domain)->fid);
		if (ret != FI_SUCCESS)
			M0_LOG(M0_ERROR, "fdp_domain fi_close ret=%d fid=%d",
			       ret, (int)(fab_dom->fdp_domain)->fid.fclass);
	}

	if (fab_dom->fdp_fabric != NULL) {
		ret = fi_close(&(fab_dom->fdp_fabric)->fid);
		if (ret != FI_SUCCESS)
			M0_LOG(M0_ERROR, "fdp_fabric fi_close ret=%d fid=%d",
			       ret, (int)(fab_dom->fdp_fabric)->fid.fclass);
		fab_dom->fdp_fabric = NULL;
	}

	if (fab_dom->fdp_fi != NULL) {
		fi_freeinfo(fab_dom->fdp_fi);
		fab_dom->fdp_fi = NULL;
	}
}

/** Used as m0_net_xprt_ops::xo_dom_fini(). */
static void libfab_dom_fini(struct m0_net_domain *dom)
{
	struct m0_fab__dom_param *fab_dom = dom->nd_xprt_private;

	M0_ENTRY();

	libfab_fab_param_free(fab_dom);
	dom->nd_xprt_private = NULL;

	M0_LEAVE();
}

/**
 * Initialises transport-specific part of the transfer machine.
 *
 * Used as m0_net_xprt_ops::xo_tm_init().
 */
static int libfab_ma_init(struct m0_net_transfer_mc *net)
{
	int result = 0;
   /* 
      approach 2 - Recommended
      Poller thread should be started here
      completion queue and counters can be initialised
      here based on list of endpoints
   */
	return M0_RC(result);
}


/**
 * Starts initialised ma.
 *
 * Initialises everything that libfab_ma_init() didn't. Note that ma is in
 * M0_NET_TM_STARTING state after this returns. Switch to M0_NET_TM_STARTED
 * happens when the poller thread posts special event.
 *
 * Used as m0_net_xprt_ops::xo_tm_start().
 */
static int libfab_ma_start(struct m0_net_transfer_mc *net, const char *name)
{
	/*
	* TODO:
	* poller thread needs to be added to check completion queue, 
	* refer nlx_xo_tm_start() LNet 
	*/
	return 0;
}

/**
 * Stops a ma that has been started or is being started.
 *
 *
 * Used as m0_net_xprt_ops::xo_tm_stop().
 */
static int libfab_ma_stop(struct m0_net_transfer_mc *net, bool cancel)
{
	/* TODO: fi_cancel () */

	return 0;
}

/**
 * Used as m0_net_xprt_ops::xo_ma_fini().
 */
static void libfab_ma_fini(struct m0_net_transfer_mc *net)
{
	/* TODO: 
	 * Reverse the actions of libfab_ma_init() 
	 * */
}

/**
 * Returns an end-point with the given name.
 *
 * Used as m0_net_xprt_ops::xo_end_point_create().
 *
 * @see m0_net_end_point_create().
 */
static int libfab_end_point_create(struct m0_net_end_point **epp,
			    struct m0_net_transfer_mc *net,
			    const char *name)
{
	/*
 	* TODO:
	* fi_endpoint, fi_pep, fi_av, fi_cq, fi_cntr, fi_eq, fi_bind(av/cq/cntr/eq), fi_pep_bind
 	* */
	int        result = 0;

	return M0_RC(result);
}


/**
 * Deregister a network buffer.
 *
 * Used as m0_net_xprt_ops::xo_buf_deregister().
 *
 * @see m0_net_buffer_deregister().
 */
static void libfab_buf_deregister(struct m0_net_buffer *nb)
{
	struct m0_fab__buf_params *fbp = nb->nb_xprt_private;
	int			   ret;

	ret = fi_close(&fbp->fbp_mr->fid);
	M0_PRE(ret == FI_SUCCESS);
	m0_free(fbp);
	nb->nb_xprt_private = NULL;
}

/**
 * Register a network buffer.
 *
 * Used as m0_net_xprt_ops::xo_buf_register().
 *
 * @see m0_net_buffer_register().
 */
static int libfab_buf_register(struct m0_net_buffer *nb)
{
	int			      ret;
	struct m0_fab__dom_param     *fab_dom = nb->nb_dom->nd_xprt_private;
	struct fid_domain	     *domain = fab_dom->fdp_domain;
	struct m0_fab__buf_params    *fbp;

	M0_PRE(nb->nb_xprt_private == NULL);

	M0_ALLOC_PTR(fbp);
	if (fbp == NULL)
		return M0_ERR(-ENOMEM);

	nb->nb_xprt_private = fbp;
	fbp->fbp_nb = nb;
	/* Registers buffer that can be used for send/recv and local/remote RMA. */
	ret = fi_mr_reg(domain, nb->nb_buffer.ov_buf[0], nb->nb_length,
			FAB_MR_ACCESS, FAB_MR_OFFSET, FAB_MR_KEY,
			FAB_MR_FLAG, &fbp->fbp_mr, NULL);	
	if (ret != FI_SUCCESS)
	{
		nb->nb_xprt_private = NULL;
		m0_free(fbp);
		return M0_ERR(ret);
	}
	
	ret = fi_mr_enable(fbp->fbp_mr);
	if (ret != FI_SUCCESS)
		libfab_buf_deregister(nb); /* Failed to enable memory region */

	return M0_RC(ret);
}

/**
 * Adds a network buffer to a ma queue.
 *
 * Used as m0_net_xprt_ops::xo_buf_add().
 *
 * @see m0_net_buffer_add().
 */
static int libfab_buf_add(struct m0_net_buffer *nb)
{
	int        result = 0;
	/*
 	* TODO:
 	*   fi_send/fi_recv
 	* */
	return M0_RC(result);
}



/**
 * Cancels a buffer operation..
 *
 * Used as m0_net_xprt_ops::xo_buf_del().
 *
 * @see m0_net_buffer_del().
 */
static void libfab_buf_del(struct m0_net_buffer *nb)
{

	/*
 	* TODO:
 	*   fi_cancel
 	* */
}

static int libfab_ma_confine(struct m0_net_transfer_mc *ma,
		      const struct m0_bitmap *processors)
{
	return -ENOSYS;
}

static int libfab_bev_deliver_sync(struct m0_net_transfer_mc *ma)
{
	/*
 	* TODO:
 	* Check if it is required ?
 	* */
	return 0;
}

static void libfab_bev_deliver_all(struct m0_net_transfer_mc *ma)
{
	/*
 	* TODO:
 	* Check if it is required ?
 	* */
}

static bool libfab_bev_pending(struct m0_net_transfer_mc *ma)
{
	/*
 	* TODO:
 	* Check if it is required ?
 	* */
	return false;
}

static void libfab_bev_notify(struct m0_net_transfer_mc *ma, struct m0_chan *chan)
{
	/*
 	* TODO:
 	* Check if it is required ?
 	* */
}

/**
 * Maximal number of bytes in a buffer.
 *
 * Used as m0_net_xprt_ops::xo_get_max_buffer_size()
 *
 * @see m0_net_domain_get_max_buffer_size()
 */
static m0_bcount_t libfab_get_max_buffer_size(const struct m0_net_domain *dom)
{
	/*
 	* TODO:
 	* Explore libfab code and return approriate value based on
 	* underlying protocol used i.e. tcp/udp/verbs
 	* Might have to add switch case based on protocol used 
 	* */
	return M0_BCOUNT_MAX / 2;
}

/**
 * Maximal number of bytes in a buffer segment.
 *
 * Used as m0_net_xprt_ops::xo_get_max_buffer_segment_size()
 *
 * @see m0_net_domain_get_max_buffer_segment_size()
 */
static m0_bcount_t libfab_get_max_buffer_segment_size(const struct m0_net_domain *dom)
{
	/*
 	* TODO:
 	* same as get_max_buffer_size()
	* This is maximum size of buffer segment size
 	* */
	return M0_BCOUNT_MAX / 2;
}

/**
 * Maximal number of segments in a buffer
 *
 * Used as m0_net_xprt_ops::xo_get_max_buffer_segments()
 *
 * @see m0_net_domain_get_max_buffer_segments()
 */
static int32_t libfab_get_max_buffer_segments(const struct m0_net_domain *dom)
{
	/*
 	* TODO:
 	* same as libfab_get_max_buffer_size()
	* This is maximum number of segments supported 
	* */
	return INT32_MAX / 2; /* Beat this, LNet! */
}

/**
 * Maximal number of bytes in a buffer descriptor.
 *
 * Used as m0_net_xprt_ops::xo_get_max_buffer_desc_size()
 *
 * @see m0_net_domain_get_max_buffer_desc_size()
 */
static m0_bcount_t libfab_get_max_buffer_desc_size(const struct m0_net_domain *dom)
{
	/*
 	* TODO:
 	* same as libfab_get_max_buffer_size()
	* This is size of buffer descriptor structure size, refer fi_mr_desc() 
	* */
	//return sizeof(struct bdesc);
	return 0;
}


static const struct m0_net_xprt_ops libfab_xprt_ops = {
	.xo_dom_init                    = &libfab_dom_init,
	.xo_dom_fini                    = &libfab_dom_fini,
	.xo_tm_init                     = &libfab_ma_init,
	.xo_tm_confine                  = &libfab_ma_confine,
	.xo_tm_start                    = &libfab_ma_start,
	.xo_tm_stop                     = &libfab_ma_stop,
	.xo_tm_fini                     = &libfab_ma_fini,
	.xo_end_point_create            = &libfab_end_point_create,
	.xo_buf_register                = &libfab_buf_register,
	.xo_buf_deregister              = &libfab_buf_deregister,
	.xo_buf_add                     = &libfab_buf_add,
	.xo_buf_del                     = &libfab_buf_del,
	.xo_bev_deliver_sync            = &libfab_bev_deliver_sync,
	.xo_bev_deliver_all             = &libfab_bev_deliver_all,
	.xo_bev_pending                 = &libfab_bev_pending,
	.xo_bev_notify                  = &libfab_bev_notify,
	.xo_get_max_buffer_size         = &libfab_get_max_buffer_size,
	.xo_get_max_buffer_segment_size = &libfab_get_max_buffer_segment_size,
	.xo_get_max_buffer_segments     = &libfab_get_max_buffer_segments,
	.xo_get_max_buffer_desc_size    = &libfab_get_max_buffer_desc_size
};

struct m0_net_xprt m0_net_libfab_xprt = {
	.nx_name = "libfab",
	.nx_ops  = &libfab_xprt_ops
};
M0_EXPORTED(m0_net_libfab_xprt);

#undef M0_TRACE_SUBSYSTEM

/** @} end of netlibfab group */

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */
