/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ap_config.h"
#include "ap_mmn.h"
#include "httpd.h"
#include "http_config.h"
#include "http_connection.h"
#include "http_protocol.h"
#include "http_log.h"
#include "apr_strings.h"
#include "apr_lib.h"
#define APR_WANT_BYTEFUNC
#include "apr_want.h"
#include "apr_network_io.h"

module AP_MODULE_DECLARE_DATA remoteip_module;

typedef struct {
    /** A proxy IP mask to match */
    apr_ipsubnet_t *ip;
    /** Flagged if internal, otherwise an external trusted proxy */
    int internal;
} remoteip_proxymatch_t;

typedef struct {
    /** The header to retrieve a proxy-via ip list */
    const char *header_name;
    /** A header to record the proxied IP's 
     * (removed as the physical connection and 
     * from the proxy-via ip header value list) 
     */
    const char *proxies_header_name;
    /** A list of trusted proxies, ideally configured
     *  with the most commonly encountered listed first
     */
    apr_array_header_t *proxymatch_ip;
} remoteip_config_t;

typedef struct {
    /** The previous proxy-via request header value */
    const char *prior_remote;
    /** The unmodified original ip and address */
    const char *orig_ip;
    apr_sockaddr_t *orig_addr;
    /** The list of proxy ip's ignored as remote ip's */
    const char *proxy_ips;
    /** The remaining list of untrusted proxied remote ip's */
    const char *proxied_remote;
    /** The most recetly modified ip and address record */
    const char *proxied_ip;
    apr_sockaddr_t proxied_addr;
} remoteip_conn_t;

static void *create_remoteip_server_config(apr_pool_t *p, server_rec *s)
{
    remoteip_config_t *config = apr_pcalloc(p, sizeof *config);
    /* config->header_name = NULL;
     * config->proxies_header_name = NULL;
     */
    return config;
}

static void *merge_remoteip_server_config(apr_pool_t *p, void *globalv,
                                          void *serverv)
{
    remoteip_config_t *global = (remoteip_config_t *) globalv;
    remoteip_config_t *server = (remoteip_config_t *) serverv;
    remoteip_config_t *config;

    config = (remoteip_config_t *) apr_palloc(p, sizeof(*config));
    config->header_name = server->header_name
                        ? server->header_name
                        : global->header_name;
    config->proxies_header_name = server->proxies_header_name
                                ? server->proxies_header_name
                                : global->proxies_header_name;
    config->proxymatch_ip = server->proxymatch_ip
                          ? server->proxymatch_ip
                          : global->proxymatch_ip;
    return config;
}

static const char *header_name_set(cmd_parms *cmd, void *dummy,
                                   const char *arg)
{
    remoteip_config_t *config = ap_get_module_config(cmd->server->module_config,
                                                     &remoteip_module);
    config->header_name = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *proxies_header_name_set(cmd_parms *cmd, void *dummy,
                                           const char *arg)
{
    remoteip_config_t *config = ap_get_module_config(cmd->server->module_config,
                                                     &remoteip_module);
    config->proxies_header_name = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

/* Would be quite nice if APR exported this */
static int looks_like_ip(const char *ipstr)
{
    if (ap_strchr_c(ipstr, ':')) {
        /* definitely not a hostname; assume it is intended to be an IPv6 address */
        return 1;
    }

    /* simple IPv4 address string check */
    while ((*ipstr == '.') || apr_isdigit(*ipstr))
        ipstr++;
    return (*ipstr == '\0');
}

static const char *proxies_set(cmd_parms *cmd, void *internal,
                               const char *arg)
{
    remoteip_config_t *config = ap_get_module_config(cmd->server->module_config,
                                                     &remoteip_module);
    remoteip_proxymatch_t *match;
    apr_status_t rv;
    char *ip = apr_pstrdup(cmd->temp_pool, arg);
    char *s = ap_strchr(ip, '/');
    if (s)
        *s++ = '\0';

    if (!config->proxymatch_ip)
        config->proxymatch_ip = apr_array_make(cmd->pool, 1, sizeof(*match));
    match = (remoteip_proxymatch_t *) apr_array_push(config->proxymatch_ip);
    match->internal = (int)internal;

    if (looks_like_ip(ip)) {
        /* Note s may be null, that's fine (explicit host) */
        rv = apr_ipsubnet_create(&match->ip, ip, s, cmd->pool);
    }
    else
    {
        apr_sockaddr_t *temp_sa;

        if (s) {
            return apr_pstrcat(cmd->pool, "RemoteIP: Error parsing IP ", arg, 
                               " the subnet /", s, " is invalid for ", 
                               cmd->cmd->name, NULL);
        }

        rv = apr_sockaddr_info_get(&temp_sa,  ip, APR_UNSPEC, 0,
                                   APR_IPV4_ADDR_OK, cmd->temp_pool);
        while (rv == APR_SUCCESS)
        {
            apr_sockaddr_ip_get(&ip, temp_sa);
            rv = apr_ipsubnet_create(&match->ip, ip, NULL, cmd->pool);
            if (!(temp_sa = temp_sa->next))
                break;
            match = (remoteip_proxymatch_t *) 
                    apr_array_push(config->proxymatch_ip);
            match->internal = (int)internal;
        }
    }

    if (rv != APR_SUCCESS) {
        char msgbuf[128];
        apr_strerror(rv, msgbuf, sizeof(msgbuf));
        return apr_pstrcat(cmd->pool, "RemoteIP: Error parsing IP ", arg, 
                           " (", msgbuf, " error) for ", cmd->cmd->name, NULL);
    }

    return NULL;
}

static const char *proxylist_read(cmd_parms *cmd, void *internal,
                                  const char *filename)
{
    char lbuf[MAX_STRING_LEN];
    char *arg;
    const char *args;
    const char *errmsg;
    ap_configfile_t *cfp;
    apr_status_t rv;

    filename = ap_server_root_relative(cmd->temp_pool, filename);
    rv = ap_pcfg_openfile(&cfp, cmd->temp_pool, filename);
    if (rv != APR_SUCCESS) {
        return apr_psprintf(cmd->pool, "%s: Could not open file %s: %s",
                            cmd->cmd->name, filename, 
                            apr_strerror(rv, lbuf, sizeof(lbuf)));
    }

    while (!(ap_cfg_getline(lbuf, MAX_STRING_LEN, cfp))) {
        args = lbuf;
        while (*(arg = ap_getword_conf(cmd->temp_pool, &args)) != '\0') {
            if (*arg == '#' || *arg == '\0')
                break;
            errmsg = proxies_set(cmd, internal, arg);
            if (errmsg) {
                errmsg = apr_psprintf(cmd->pool, "%s at line %d of %s", 
                                      errmsg, cfp->line_number, filename);
                return errmsg;
            }
        }
    }

    ap_cfg_closefile(cfp);
    return NULL;
}

static int remoteip_modify_connection(request_rec *r)
{
    conn_rec *c = r->connection;
    remoteip_config_t *config = (remoteip_config_t *)
        ap_get_module_config(r->server->module_config, &remoteip_module);
    remoteip_conn_t *conn;
#ifdef REMOTEIP_OPTIMIZED
    apr_sockaddr_t temp_sa_buff;
    apr_sockaddr_t *temp_sa = &temp_sa_buff;
#else
    apr_sockaddr_t *temp_sa;
#endif
    apr_status_t rv;
    char *remote = (char *) apr_table_get(r->headers_in, config->header_name);
    char *proxy_ips = NULL;
    char *parse_remote;
    char *eos;
    unsigned char *addrbyte;
    int internal = 0;

    apr_pool_userdata_get((void*)&conn, "mod_remoteip-conn", c->pool);

    if (conn) {
        if (remote && (strcmp(remote, conn->prior_remote) == 0)) {
            /* TODO: Recycle r-> overrides from previous request
             */
            goto ditto_request_rec;
        }
        else {
            /* TODO: Revert connection from previous request
             */
            c->remote_addr = conn->orig_addr;
            c->remote_ip = (char *) conn->orig_ip;
        }
    }

    if (!remote)
        return OK;

    remote = apr_pstrdup(r->pool, remote);

#ifdef REMOTEIP_OPTIMIZED
    memcpy(&temp_sa, c->remote_addr, sizeof(temp_sa));
    temp_sa->pool = r->pool;
#else
    temp_sa = c->remote_addr;
#endif

    while (remote) {

        /* verify c->remote_addr is trusted if there is a trusted proxy list
         */
        if (config->proxymatch_ip) {
            int i;
            remoteip_proxymatch_t *match;
            match = (remoteip_proxymatch_t *)config->proxymatch_ip->elts;
            for (i = 0; i < config->proxymatch_ip->nelts; ++i) {
                if (apr_ipsubnet_test(match[i].ip, c->remote_addr)) {
                    internal = match[i].internal;
                    break;
                }
            }
            if (i && i >= config->proxymatch_ip->nelts)
                break;
        }

        if ((parse_remote = strrchr(remote, ',')) == NULL) {
            parse_remote = remote;
            remote = NULL;
        }
        else {
            *(parse_remote++) = '\0';
        }

        while (*parse_remote == ' ') 
            ++parse_remote;

        eos = parse_remote + strlen(parse_remote) - 1;
        while (eos >= parse_remote && *eos == ' ')
            *(eos--) = '\0';

        if (eos < parse_remote) {
            if (remote)
                *(remote + strlen(remote)) = ',';
            else
                remote = parse_remote;
            break;
        }

#ifdef REMOTEIP_OPTIMIZED
        /* Decode remote_addr - sucks; apr_sockaddr_vars_set isn't 'public' */
        if (inet_pton(AF_INET, parse_remote, 
                      &temp_sa_buff->sa.sin.sin_addr) > 0) {
            apr_sockaddr_vars_set(temp_sa, APR_INET, temp_sa.port);
        }
#if APR_HAVE_IPV6
        else if (inet_pton(AF_INET6, parse_remote, 
                           &temp_sa->sa.sin6.sin6_addr) > 0) {
            apr_sockaddr_vars_set(temp_sa, APR_INET6, temp_sa.port);
        }
#endif
        else {
            rv = apr_get_netos_error();
#else /* !REMOTEIP_OPTIMIZED */
        /* We map as IPv4 rather than IPv6 for equivilant host names
         * or IPV4OVERIPV6 
         */
        rv = apr_sockaddr_info_get(&temp_sa,  parse_remote, 
                                   APR_UNSPEC, temp_sa->port,
                                   APR_IPV4_ADDR_OK, r->pool);
        if (rv != APR_SUCCESS) {
#endif
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG,  rv, r,
                          "RemoteIP: Header %s value of %s cannot be parsed "
                          "as a client IP",
                          config->header_name, parse_remote);
            if (remote)
                *(remote + strlen(remote)) = ',';
            else
                remote = parse_remote;
            break;
        }

        addrbyte = (unsigned char *) &temp_sa->sa.sin.sin_addr;

        /* For intranet (Internal proxies) ignore all restrictions below */
        if (!internal
              && ((temp_sa->family == APR_INET
                   /* For internet (non-Internal proxies) deny all
                    * RFC3330 designated local/private subnets:
                    * 10.0.0.0/8   169.254.0.0/16  192.168.0.0/16 
                    * 127.0.0.0/8  172.16.0.0/12
                    */
                      && (addrbyte[0] == 10
                       || addrbyte[0] == 127
                       || (addrbyte[0] == 169 && addrbyte[1] == 254)
                       || (addrbyte[0] == 172 && (addrbyte[1] & 0xf0) == 16)
                       || (addrbyte[0] == 192 && addrbyte[1] == 168)))
#if APR_HAVE_IPV6
               || (temp_sa->family == APR_INET6
                   /* For internet (non-Internal proxies) we translated 
                    * IPv4-over-IPv6-mapped addresses as IPv4, above.
                    * Accept only Global Unicast 2000::/3 defined by RFC4291
                    */
                      && ((temp_sa->sa.sin6.sin6_addr.s6_addr[0] & 0xe0) != 0x20))
#endif
        )) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG,  rv, r,
                          "RemoteIP: Header %s value of %s appears to be "
                          "a private IP or nonsensical.  Ignored",
                          config->header_name, parse_remote);
            if (remote)
                *(remote + strlen(remote)) = ',';
            else
                remote = parse_remote;
            break;
        }

        if (!conn) {
            conn = (remoteip_conn_t *) apr_palloc(c->pool, sizeof(*conn));
            apr_pool_userdata_set(conn, "mod_remoteip-conn", NULL, c->pool);
            conn->orig_addr = c->remote_addr;
            conn->orig_ip = c->remote_ip;
        }

        /* Set remote_ip string */
        if (!internal) {
            if (proxy_ips)
                proxy_ips = apr_pstrcat(r->pool, proxy_ips, ", ", 
                                        c->remote_ip, NULL);
            else
                proxy_ips = c->remote_ip;
        }

        c->remote_addr = temp_sa;
        apr_sockaddr_ip_get(&c->remote_ip, c->remote_addr);
    }

    /* Nothing happened? */
    if (!conn || (c->remote_addr == conn->orig_addr))
        return OK;

    /* Fixups here, remote becomes the new Via header value, etc 
     * In the heavy operations above we used request scope, to limit
     * conn pool memory growth on keepalives, so here we must scope
     * the final results to the connection pool lifetime.
     * To limit memory growth, we keep recycling the same buffer
     * for the final apr_sockaddr_t in the remoteip conn rec.
     */
    c->remote_ip = apr_pstrdup(c->pool, c->remote_ip);
    conn->proxied_ip = c->remote_ip;
    memcpy(&conn->proxied_addr, &temp_sa, sizeof(temp_sa));
    conn->proxied_addr.pool = c->pool;
    c->remote_addr = &conn->proxied_addr;

    if (remote)
        remote = apr_pstrdup(c->pool, remote);
    conn->proxied_remote = remote;
    conn->prior_remote = apr_pstrdup(c->pool, apr_table_get(r->headers_in, 
                                                      config->header_name));
    if (proxy_ips)
        proxy_ips = apr_pstrdup(c->pool, proxy_ips);
    conn->proxy_ips = proxy_ips;

    /* Unset remote_host string DNS lookups */
    c->remote_host = NULL;
    c->remote_logname = NULL;

ditto_request_rec:
    if (conn->proxied_remote)
        apr_table_setn(r->headers_in, config->header_name,
                       conn->proxied_remote);
    else
        apr_table_unset(r->headers_in, config->header_name);
    if (conn->proxy_ips) {
        apr_table_setn(r->notes, "remoteip-proxy-ip-list", conn->proxy_ips);
        if (config->proxies_header_name)
            apr_table_setn(r->headers_in, config->proxies_header_name,
                           conn->proxy_ips);
    }

    ap_log_rerror(APLOG_MARK, APLOG_INFO|APLOG_NOERRNO, 0, r,
                  conn->proxy_ips 
                      ? "Using %s as client's IP by proxies %s"
                      : "Using %s as client's IP by internal proxies",
                  conn->proxied_ip, conn->proxy_ips);
    return OK;
}

static const command_rec remoteip_cmds[] =
{
    AP_INIT_TAKE1("RemoteIPHeader", header_name_set, NULL, RSRC_CONF,
                  "Specifies a request header to trust as the client IP, "
                  "e.g. X-Forwarded-For"),
    AP_INIT_TAKE1("RemoteIPProxiesHeader", proxies_header_name_set,
                  NULL, RSRC_CONF,
                  "Specifies a request header to record proxy IP's, "
                  "e.g. X-Forwarded-By; if not given then do not record"),
    AP_INIT_ITERATE("RemoteIPTrustedProxy", proxies_set, 0, RSRC_CONF,
                    "Specifies one or more proxies which are trusted "
                    "to present IP headers"),
    AP_INIT_ITERATE("RemoteIPInternalProxy", proxies_set, (void*)1, RSRC_CONF,
                    "Specifies one or more internal (transparent) proxies "
                    "which are trusted to present IP headers"),
    AP_INIT_TAKE1("RemoteIPTrustedProxyList", proxylist_read, 0,
                  RSRC_CONF | EXEC_ON_READ,
                  "The filename to read the list of trusted proxies, "
                  "see the RemoteIPTrustedProxy directive"),
    AP_INIT_TAKE1("RemoteIPInternalProxyList", proxylist_read, (void*)1,
                  RSRC_CONF | EXEC_ON_READ,
                  "The filename to read the list of internal proxies, "
                  "see the RemoteIPInternalProxy directive"),
    { NULL }
};

static void register_hooks(apr_pool_t *p)
{
    ap_hook_post_read_request(remoteip_modify_connection, NULL, NULL, APR_HOOK_FIRST);
}

module AP_MODULE_DECLARE_DATA remoteip_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                          /* create per-directory config structure */
    NULL,                          /* merge per-directory config structures */
    create_remoteip_server_config, /* create per-server config structure */
    merge_remoteip_server_config,  /* merge per-server config structures */
    remoteip_cmds,                 /* command apr_table_t */
    register_hooks                 /* register hooks */
};
