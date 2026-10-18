/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unity's web requests, whose Java side is com.unity3d.player.UnityWebRequest: a Runnable over HttpURLConnection that the engine
 * builds (with a java.util.HashMap of request headers), sets up, runs on a thread of its own, and which reports back through
 * native callbacks -- the status, each header, the length, the body in pieces, or an error. Here the request itself is the host's
 * (husk-tl-http.m), and the Java object is this: it keeps what the engine gave it and, on run(), does what the Java would have done,
 * in the same order, so the engine sees the same calls.
 *
 * Also the java.util.HashMap that carries the headers, since the engine reads nothing back from it but this code does.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-bionic.h"
#include "husk-tl-http.h"
#include "husk-tl-netcfg.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <resolv.h>

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static void Noop(tl_jcall *c) { (void)c; }
static const char *S(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }

/* ------------------------------------------------------------------ HashMap */

typedef struct { jobj **k, **v; int n, cap; } hmap;
static hmap *HM(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(hmap)); return o->native; }

static void Map_init(tl_jcall *c) { (void)HM(c->self); }
static void Map_put(tl_jcall *c)
{
    hmap *m = HM(c->self);
    const char *key = S(c->args[0].l);
    for (int i = 0; i < m->n; i++) {
        if (!strcmp(S(m->k[i]), key)) { jobj *old = m->v[i]; m->v[i] = c->args[1].l ? tl_jni_ref(c->args[1].l) : NULL; c->ret = vl(old); return; }
    }
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 8; m->k = realloc(m->k, (size_t)m->cap * sizeof(jobj *)); m->v = realloc(m->v, (size_t)m->cap * sizeof(jobj *)); }
    m->k[m->n] = c->args[0].l ? tl_jni_ref(c->args[0].l) : NULL;
    m->v[m->n] = c->args[1].l ? tl_jni_ref(c->args[1].l) : NULL;
    m->n++;
    c->ret = vl(NULL);
}
static void Map_get(tl_jcall *c)
{
    hmap *m = HM(c->self);
    const char *key = S(c->args[0].l);
    for (int i = 0; i < m->n; i++) if (!strcmp(S(m->k[i]), key)) { c->ret = vl(m->v[i] ? tl_jni_ref(m->v[i]) : NULL); return; }
    c->ret = vl(NULL);
}
static void Map_containsKey(tl_jcall *c)
{
    hmap *m = HM(c->self);
    const char *key = S(c->args[0].l);
    for (int i = 0; i < m->n; i++) if (!strcmp(S(m->k[i]), key)) { c->ret = vz(1); return; }
    c->ret = vz(0);
}
static void Map_size(tl_jcall *c) { c->ret = vi(HM(c->self)->n); }
static void Map_isEmpty(tl_jcall *c) { c->ret = vz(HM(c->self)->n == 0); }
static void Map_entrySet(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("java/util/HashSet"))); }

/* ------------------------------------------------------------ UnityWebRequest */

#define CLS "com/unity3d/player/UnityWebRequest"

typedef struct { int64_t ptr; char *url, *method; jobj *headers; int timeout_ms; int64_t upload_len; bool expect, chunked; } uwr;
static uwr *U(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(uwr)); return o->native; }

static void UWR_init(tl_jcall *c)
{
    /* (long nativeRequest, String method, Map headers, String url, boolean certificateHandler, int timeoutMs) */
    uwr *u = U(c->self);
    u->ptr = c->args[0].j;
    u->method = strdup(S(c->args[1].l));
    u->headers = c->args[2].l ? tl_jni_ref(c->args[2].l) : NULL;
    u->url = strdup(S(c->args[3].l));
    u->timeout_ms = c->args[5].i;
}
static void UWR_setup(tl_jcall *c)
{
    uwr *u = U(c->self);
    u->upload_len = c->args[0].j; u->expect = c->args[1].z; u->chunked = c->args[2].z;
}

typedef int (*upload_fn)(void *env, void *cls, int64_t ptr, void *buf);
typedef uint8_t (*download_fn)(void *env, void *cls, int64_t ptr, void *buf, int n);
typedef void (*header_fn)(void *env, void *cls, int64_t ptr, void *name, void *value);
typedef void (*int_fn)(void *env, void *cls, int64_t ptr, int v);
typedef void (*error_fn)(void *env, void *cls, int64_t ptr, int code, void *msg);

static jobj *direct_buffer(void *mem, int64_t cap)
{
    jobj *o = tl_jni_new_object(tl_jni_class("java/nio/DirectByteBuffer"));
    jvalue a, n; a.j = 0; a.l = mem; n.j = cap;
    tl_jni_set_field(o, "address", "J", a);
    tl_jni_set_field(o, "capacity", "J", n);
    return o;
}

static void UWR_run(tl_jcall *c)
{
    uwr *u = U(c->self);
    void *env = tl_jni_env(), *cls = tl_jni_class_object(CLS);
    upload_fn upload = tl_jni_native(CLS, "uploadCallback", "(JLjava/nio/ByteBuffer;)I");
    download_fn download = tl_jni_native(CLS, "downloadCallback", "(JLjava/nio/ByteBuffer;I)Z");
    header_fn header = tl_jni_native(CLS, "headerCallback", "(JLjava/lang/String;Ljava/lang/String;)V");
    int_fn length = tl_jni_native(CLS, "contentLengthCallback", "(JI)V");
    int_fn status = tl_jni_native(CLS, "responseCodeCallback", "(JI)V");
    error_fn error = tl_jni_native(CLS, "errorCallback", "(JILjava/lang/String;)V");
    if (!upload || !download || !header || !length || !status || !error) return;

    /* the request body, if any: the engine says how much with a null buffer, then hands it over in pieces */
    const size_t CHUNK = 128 * 1024;
    uint8_t *chunk = malloc(CHUNK), *body = NULL; size_t body_len = 0;
    jobj *buf = direct_buffer(chunk, (int64_t)CHUNK);
    if (upload(env, cls, u->ptr, NULL) > 0) {
        for (;;) {
            int n = upload(env, cls, u->ptr, buf);
            if (n <= 0) break;
            body = realloc(body, body_len + (size_t)n);
            memcpy(body + body_len, chunk, (size_t)n);
            body_len += (size_t)n;
        }
    }

    /* the headers it set */
    hmap *hm = u->headers ? HM(u->headers) : NULL;
    int nh = hm ? hm->n : 0;
    const char **names = calloc((size_t)nh + 1, sizeof(char *)), **values = calloc((size_t)nh + 1, sizeof(char *));
    for (int i = 0; i < nh; i++) { names[i] = S(hm->k[i]); values[i] = S(hm->v[i]); }

    tl_http_request rq = { .url = u->url, .method = u->method, .nheaders = nh, .header_names = names, .header_values = values,
                           .body = body, .body_len = body_len, .timeout_ms = u->timeout_ms };
    tl_http_response rs;
    tl_log_line("http: %s %s (%zu bytes sent)", u->method, u->url, body_len);
    if (!tl_http_perform(&rq, &rs)) {
        tl_log_line("http: %s failed: %s", u->url, rs.message);
        error(env, cls, u->ptr, rs.error, tl_jni_new_string(rs.message));
    } else {
        long content_length = -1;
        for (int i = 0; i < rs.nheaders; i++) {
            header(env, cls, u->ptr, tl_jni_new_string(rs.header_names[i]), tl_jni_new_string(rs.header_values[i]));
            if (!strcasecmp(rs.header_names[i], "content-length")) content_length = atol(rs.header_values[i]);
        }
        length(env, cls, u->ptr, (int)(content_length >= 0 ? content_length : (long)rs.body_len));
        status(env, cls, u->ptr, rs.status);
        for (size_t off = 0; off < rs.body_len;) {
            size_t n = rs.body_len - off < CHUNK ? rs.body_len - off : CHUNK;
            memcpy(chunk, rs.body + off, n);
            if (!download(env, cls, u->ptr, buf, (int)n)) break;
            off += n;
        }
        tl_log_line("http: %s -> %d (%zu bytes)", u->url, rs.status, rs.body_len);
        tl_http_response_free(&rs);
    }
    free(names); free(values); free(body);
    /* chunk stays: the buffer object may still be referenced by the engine */
}

#define M_(c, n, s, f) { c, n, s, f }
static const tl_jhle k_hle[] = {
    M_("java/util/HashMap", "<init>", "()V", Map_init), M_("java/util/HashMap", "<init>", "(I)V", Map_init),
    M_("java/util/HashMap", "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", Map_put),
    M_("java/util/HashMap", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", Map_get),
    M_("java/util/HashMap", "containsKey", "(Ljava/lang/Object;)Z", Map_containsKey),
    M_("java/util/HashMap", "size", "()I", Map_size), M_("java/util/HashMap", "isEmpty", "()Z", Map_isEmpty),
    M_("java/util/HashMap", "entrySet", "()Ljava/util/Set;", Map_entrySet),
    M_("java/util/Map", "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", Map_put),
    M_("java/util/Map", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", Map_get),
    M_("java/util/Map", "containsKey", "(Ljava/lang/Object;)Z", Map_containsKey),
    M_("java/util/Map", "size", "()I", Map_size), M_("java/util/Map", "isEmpty", "()Z", Map_isEmpty),
    M_("java/util/Map", "entrySet", "()Ljava/util/Set;", Map_entrySet),
    M_(CLS, "<init>", "(JLjava/lang/String;Ljava/util/Map;Ljava/lang/String;ZI)V", UWR_init),
    M_(CLS, "setupTransferSettings", "(JZZ)V", UWR_setup),
    M_(CLS, "run", "()V", UWR_run),
    M_(CLS, "clearCookieCache", "(Ljava/lang/String;Ljava/lang/String;)V", Noop),
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------ connectivity */

static void CM_activeNetworkInfo(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("android/net/NetworkInfo"))); }
static void CM_activeNetwork(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("android/net/Network"))); }
static void CM_capabilities(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("android/net/NetworkCapabilities"))); }
static void True_(tl_jcall *c) { c->ret = vz(1); }
static void NI_type(tl_jcall *c) { c->ret = vi(1); }                          /* TYPE_WIFI */
static void NI_typeName(tl_jcall *c) { c->ret = vl(tl_jni_new_string("WIFI")); }
static void NI_subtype(tl_jcall *c) { c->ret = vi(0); }

/*
 * The DNS servers the phone uses come from husk-tl-netcfg.c (tl_dns_servers): code that resolves names itself rather than
 * through getaddrinfo -- c-ares in Geode 5's curl, and others -- asks Android for them here through
 * ConnectivityManager -> LinkProperties.getDnsServers(), and failing that reads net.dns1/net.dns2. Without an answer it
 * has no servers and every request fails with "couldn't resolve host".
 */

static void CM_linkProperties(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("android/net/LinkProperties"))); }
typedef struct { jobj **items; uint32_t n; } alist;      /* the ArrayList the gamepad code answers size()/get() for */
static jobj *Net_list(jobj **items, uint32_t n)
{
    jobj *l = tl_jni_new_object(tl_jni_class("java/util/ArrayList"));
    alist *a = calloc(1, sizeof(*a));
    a->items = items; a->n = n;
    l->native = a;
    return l;
}
static void LP_dnsServers(tl_jcall *c)
{
    char servers[6][64];
    int n = tl_dns_servers(servers, 6);
    jobj **items = calloc((size_t)(n > 0 ? n : 1), sizeof(jobj *));
    uint32_t have = 0;
    for (int i = 0; i < n; i++) {
        jobj *ia = tl_jni_new_object(tl_jni_class("java/net/InetAddress"));
        jvalue v; v.j = 0; v.l = tl_jni_new_string(servers[i]);
        tl_jni_set_field(ia, "host", "Ljava/lang/String;", v);
        items[have++] = ia;
    }
    c->ret = vl(Net_list(items, have));
}
static void IA_hostAddress(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "host", "Ljava/lang/String;"); }

/* ---------------------------------------------------------------- the bridge */

/*
 * Settings > Network Bridge (husk-tl-netcfg.c): with it on, a game that asks Android about its network is told what the
 * phone's own Wi-Fi really is -- the address, netmask, router and DNS the iPhone's DHCP lease gave it. The game's sockets
 * already go out on the host's stack, so this is the address its packets come from, and a LAN game told anything else
 * announces itself somewhere nothing can reach it. With the bridge off these answer the placeholders they always did, and
 * the log says once which question was asked, so a game whose LAN mode does not work can be pointed at the switch.
 */
static bool net_bridge(const char *what, tl_netcfg *cfg)
{
    if (!tl_netcfg_bridge()) {
        char n[192]; snprintf(n, sizeof(n), "net: %s was asked, but the bridge is off (Settings > Network Bridge)", what);
        tl_note_once(n);
        return false;
    }
    if (!tl_netcfg_read(cfg)) {
        char n[192]; snprintf(n, sizeof(n), "net: %s was asked, but the phone has no network (%s)", what, cfg->why);
        tl_note_once(n);
        return false;
    }
    tl_netcfg_note();
    return true;
}
static void nset_i(jobj *o, const char *f, int v) { jvalue x; x.j = 0; x.i = v; tl_jni_set_field(o, f, "I", x); }
static void nset_s(jobj *o, const char *f, const char *v)
{
    jvalue x; x.j = 0; x.l = v ? tl_jni_new_string(v) : NULL;
    tl_jni_set_field(o, f, "Ljava/lang/String;", x);
}
static jobj *Net_inet(const char *ipv4)
{
    jobj *o = tl_jni_new_object(tl_jni_class("java/net/InetAddress"));
    nset_s(o, "host", ipv4);
    return o;
}

/* WifiManager/WifiInfo: what Android's Wi-Fi connection info would say. Off, the questions answer null, exactly as they
 * did before the bridge (there was no implementation); on, they answer with the phone's own Wi-Fi. */
static void WM_getConnectionInfo(tl_jcall *c)
{
    tl_netcfg cfg;
    if (!net_bridge("WifiManager.getConnectionInfo", &cfg)) { c->ret = vl(NULL); return; }
    jobj *w = tl_jni_new_object(tl_jni_class("android/net/wifi/WifiInfo"));
    /* What an app without the location permission sees on Android: no SSID, and a MAC Android no longer gives out. */
    nset_s(w, "SSID", "<unknown ssid>"); nset_i(w, "NetworkId", -1); nset_i(w, "Rssi", -55); nset_i(w, "LinkSpeed", 433);
    nset_i(w, "Frequency", 2412); nset_i(w, "IpAddress", 0);
    nset_s(w, "BSSID", "02:00:00:00:00:00"); nset_s(w, "MacAddress", "02:00:00:00:00:00");
    nset_i(w, "IpAddress", (int)cfg.ip_int);
    if (cfg.mac[0]) { nset_s(w, "MacAddress", cfg.mac); nset_s(w, "BSSID", cfg.mac); }
    /* the private names, for a game that reads the fields rather than the getters */
    nset_i(w, "mIpAddress", (int)cfg.ip_int);
    if (cfg.mac[0]) nset_s(w, "mMacAddress", cfg.mac);
    c->ret = vl(w);
}
static void WM_getDhcpInfo(tl_jcall *c)
{
    tl_netcfg cfg;
    if (!net_bridge("WifiManager.getDhcpInfo", &cfg)) { c->ret = vl(NULL); return; }
    jobj *d = tl_jni_new_object(tl_jni_class("android/net/DhcpInfo"));
    nset_i(d, "ipAddress", (int)cfg.ip_int);
    nset_i(d, "gateway", (int)cfg.gateway_int);
    nset_i(d, "netmask", (int)cfg.netmask_int);
    nset_i(d, "dns1", (int)(cfg.ndns > 0 ? cfg.dns_int[0] : 0));
    nset_i(d, "dns2", (int)(cfg.ndns > 1 ? cfg.dns_int[1] : 0));
    nset_i(d, "serverAddress", (int)cfg.gateway_int);            /* the router is the DHCP server on any home LAN */
    nset_i(d, "leaseDuration", 86400);
    c->ret = vl(d);
}
static void WM_isWifiEnabled(tl_jcall *c)
{
    tl_netcfg cfg;
    c->ret = vz(net_bridge("WifiManager.isWifiEnabled", &cfg));
}
static void WM_getWifiState(tl_jcall *c)
{
    tl_netcfg cfg;
    c->ret = vi(net_bridge("WifiManager.getWifiState", &cfg) ? 3 : 0);      /* 3: WIFI_STATE_ENABLED */
}
static void WI_getIpAddress(tl_jcall *c)
{
    tl_netcfg cfg;
    c->ret = vi(net_bridge("WifiInfo.getIpAddress", &cfg) ? (int)cfg.ip_int : 0);
}
static void WI_getMacAddress(tl_jcall *c)
{
    tl_netcfg cfg;
    c->ret = vl(tl_jni_new_string(net_bridge("WifiInfo.getMacAddress", &cfg) && cfg.mac[0] ? cfg.mac : "02:00:00:00:00:00"));
}
static void WI_getBSSID(tl_jcall *c)
{
    tl_netcfg cfg;
    c->ret = vl(tl_jni_new_string(net_bridge("WifiInfo.getBSSID", &cfg) && cfg.mac[0] ? cfg.mac : "02:00:00:00:00:00"));
}
static void WI_getSSID(tl_jcall *c) { c->ret = vl(tl_jni_new_string("<unknown ssid>")); }
static void WI_getLinkSpeed(tl_jcall *c) { c->ret = vi(433); }
static void WI_getFrequency(tl_jcall *c) { c->ret = vi(2412); }
static void WI_getNetworkId(tl_jcall *c) { c->ret = vi(-1); }
static void WI_getRssi(tl_jcall *c) { c->ret = vi(-55); }

/* LinkProperties: the address on the link, its netmask as a prefix, and the default route through the router. */
static void LP_getInterfaceName(tl_jcall *c)
{
    tl_netcfg cfg;
    c->ret = vl(net_bridge("LinkProperties.getInterfaceName", &cfg) ? tl_jni_new_string(cfg.android_if) : NULL);
}
static void LP_getLinkAddresses(tl_jcall *c)
{
    tl_netcfg cfg;
    if (!net_bridge("LinkProperties.getLinkAddresses", &cfg)) { c->ret = vl(Net_list(NULL, 0)); return; }
    jobj **items = calloc(1, sizeof(jobj *));
    jobj *la = tl_jni_new_object(tl_jni_class("android/net/LinkAddress"));
    nset_i(la, "prefixLength", cfg.prefix);
    tl_jni_set_field(la, "address", "Ljava/net/InetAddress;", vl(Net_inet(cfg.ip)));
    items[0] = la;
    c->ret = vl(Net_list(items, 1));
}
static void LA_getAddress(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "address", "Ljava/net/InetAddress;"); }
static void LA_getPrefixLength(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "prefixLength", "I"); }
static void LP_getRoutes(tl_jcall *c)
{
    tl_netcfg cfg;
    if (!net_bridge("LinkProperties.getRoutes", &cfg)) { c->ret = vl(Net_list(NULL, 0)); return; }
    jobj **items = calloc(1, sizeof(jobj *));
    jobj *r = tl_jni_new_object(tl_jni_class("android/net/RouteInfo"));
    nset_s(r, "iface", cfg.android_if);
    if (cfg.gateway[0]) tl_jni_set_field(r, "gateway", "Ljava/net/InetAddress;", vl(Net_inet(cfg.gateway)));
    items[0] = r;
    c->ret = vl(Net_list(items, 1));
}
static void RI_getGateway(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "gateway", "Ljava/net/InetAddress;"); }
static void RI_getInterface(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "iface", "Ljava/lang/String;"); }
/* java.net.NetworkInterface: the same interface once more, for a game that walks the list rather than reading
 * LinkProperties, and for the InetAddress java.net code asks the local address from. Android's loopback is not among
 * them here (a game that skips it finds the phone's own interface first). */
static jobj *NetIF_one(const tl_netcfg *cfg)
{
    jobj *n = tl_jni_new_object(tl_jni_class("java/net/NetworkInterface"));
    nset_s(n, "name", cfg->android_if);
    nset_s(n, "displayName", cfg->android_if);
    tl_jni_set_field(n, "addr", "Ljava/net/InetAddress;", vl(Net_inet(cfg->ip)));
    tl_jni_set_field(n, "bcast", "Ljava/net/InetAddress;", vl(Net_inet(cfg->broadcast)));
    nset_i(n, "prefix", cfg->prefix);
    return n;
}
static void NetIF_getNetworkInterfaces(tl_jcall *c)
{
    tl_netcfg cfg;
    if (!net_bridge("NetworkInterface.getNetworkInterfaces", &cfg) || !cfg.ip[0]) { c->ret = vl(Net_list(NULL, 0)); return; }
    jobj **items = calloc(1, sizeof(jobj *));
    items[0] = NetIF_one(&cfg);
    c->ret = vl(Net_list(items, 1));
}
static void NetIF_getByName(tl_jcall *c)      /* also getByIndex / getByInetAddress: the same one */
{
    tl_netcfg cfg;
    if (!net_bridge("NetworkInterface.getByName", &cfg) || !cfg.ip[0]) { c->ret = vl(NULL); return; }
    c->ret = vl(NetIF_one(&cfg));
}
static void NetIF_getName(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "name", "Ljava/lang/String;"); }
static void NetIF_getDisplayName(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "displayName", "Ljava/lang/String;"); }
static void NetIF_getInetAddresses(tl_jcall *c)
{
    tl_netcfg cfg;
    if (!net_bridge("NetworkInterface.getInetAddresses", &cfg) || !cfg.ip[0]) { c->ret = vl(Net_list(NULL, 0)); return; }
    jobj **items = calloc(1, sizeof(jobj *));
    items[0] = Net_inet(cfg.ip);
    c->ret = vl(Net_list(items, 1));
}
static void NetIF_getInterfaceAddresses(tl_jcall *c)
{
    tl_netcfg cfg;
    if (!net_bridge("NetworkInterface.getInterfaceAddresses", &cfg) || !cfg.ip[0]) { c->ret = vl(Net_list(NULL, 0)); return; }
    jobj **items = calloc(1, sizeof(jobj *));
    jobj *a = tl_jni_new_object(tl_jni_class("java/net/InterfaceAddress"));
    tl_jni_set_field(a, "address", "Ljava/net/InetAddress;", vl(Net_inet(cfg.ip)));
    if (cfg.broadcast[0]) tl_jni_set_field(a, "broadcast", "Ljava/net/InetAddress;", vl(Net_inet(cfg.broadcast)));
    nset_i(a, "prefix", cfg.prefix);
    items[0] = a;
    c->ret = vl(Net_list(items, 1));
}
static void NetIF_getHardwareAddress(tl_jcall *c)
{
    tl_netcfg cfg;
    if (!net_bridge("NetworkInterface.getHardwareAddress", &cfg) || !cfg.mac[0]) { c->ret = vl(NULL); return; }
    unsigned b[6];
    if (sscanf(cfg.mac, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) { c->ret = vl(NULL); return; }
    jobj *arr = tl_jni_new_prim_array('B', 6);
    arr->refs = 1u << 30;
    for (int i = 0; i < 6; i++) ((uint8_t *)arr->arr.data)[i] = (uint8_t)b[i];
    c->ret = vl(arr);
}
static void NetIF_isUp(tl_jcall *c) { tl_netcfg cfg; c->ret = vz(net_bridge("NetworkInterface.isUp", &cfg)); }
static void NetIF_isLoopback(tl_jcall *c) { (void)c; c->ret = vz(0); }
static void NetIF_isPointToPoint(tl_jcall *c) { (void)c; c->ret = vz(0); }
static void NetIF_isVirtual(tl_jcall *c) { (void)c; c->ret = vz(0); }
static void NetIF_supportsMulticast(tl_jcall *c) { (void)c; c->ret = vz(1); }
static void NetIF_getMTU(tl_jcall *c) { (void)c; c->ret = vi(1500); }
static void NetIF_getIndex(tl_jcall *c) { (void)c; c->ret = vi(1); }
static void IA2_getAddress(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "address", "Ljava/net/InetAddress;"); }
static void IA2_getBroadcast(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "broadcast", "Ljava/net/InetAddress;"); }
static void IA2_getNetworkPrefixLength(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "prefix", "I"); }
/* InetAddress: the address we handed out is a literal, so its bytes (and its name) are its own text. */
static void Inet_getAddress(tl_jcall *c)
{
    jvalue s = tl_jni_get_field(c->self, "host", "Ljava/lang/String;");
    const char *ip = s.l ? tl_jni_string(s.l) : NULL;
    unsigned b[4];
    if (!ip || sscanf(ip, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) { c->ret = vl(NULL); return; }
    jobj *arr = tl_jni_new_prim_array('B', 4);
    arr->refs = 1u << 30;
    for (int i = 0; i < 4; i++) ((uint8_t *)arr->arr.data)[i] = (uint8_t)b[i];
    c->ret = vl(arr);
}
static void Inet_getHostName(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "host", "Ljava/lang/String;"); }
static void Inet_getLocalHost(tl_jcall *c)      /* static: the address a game should announce is the phone's own */
{
    (void)c;
    tl_netcfg cfg;
    c->ret = vl(net_bridge("InetAddress.getLocalHost", &cfg) && cfg.ip[0] ? Net_inet(cfg.ip) : NULL);
}
static void Inet_isLoopbackAddress(tl_jcall *c)
{
    jvalue s = tl_jni_get_field(c->self, "host", "Ljava/lang/String;");
    const char *ip = s.l ? tl_jni_string(s.l) : NULL;
    c->ret = vz(ip && !strncmp(ip, "127.", 4));
}
static void Inet_isAnyLocalAddress(tl_jcall *c)
{
    jvalue s = tl_jni_get_field(c->self, "host", "Ljava/lang/String;");
    const char *ip = s.l ? tl_jni_string(s.l) : NULL;
    c->ret = vz(ip && !strcmp(ip, "0.0.0.0"));
}
static void Inet_getLoopbackAddress(tl_jcall *c) { (void)c; c->ret = vl(Net_inet("127.0.0.1")); }

/* Enumeration: how java.net.NetworkInterface hands its list out (getNetworkInterfaces/getInetAddresses), and how the
 * usual loop over them asks for each one. The same list objects answer the List API as well. */
static void Enum_hasMoreElements(tl_jcall *c)
{
    alist *a = c->self ? c->self->native : NULL;
    c->ret = vz(a && tl_jni_get_field(c->self, "cursor", "I").i < (int)a->n);
}
static void Enum_nextElement(tl_jcall *c)
{
    alist *a = c->self ? c->self->native : NULL;
    int i = c->self ? tl_jni_get_field(c->self, "cursor", "I").i : 0;
    if (!a || i < 0 || i >= (int)a->n) { tl_jni_throw("java/util/NoSuchElementException", ""); c->ret = vl(NULL); return; }
    nset_i(c->self, "cursor", i + 1);
    c->ret = vl(a->items[i]);
}

static void LP_getDomains(tl_jcall *c) { c->ret = vl(NULL); }
static void LP_getMtu(tl_jcall *c) { c->ret = vi(1500); }
/* "Is there a network?", asked of the link itself rather than of ConnectivityManager -- what a game checks before it
 * offers its online mode. The answer is the phone's own link, with the bridge on. */
static void LP_hasIPv4Address(tl_jcall *c) { (void)c; tl_netcfg g; c->ret = vz(net_bridge("LinkProperties.hasIPv4Address", &g) && g.ip[0]); }
static void LP_hasIPv4DefaultRoute(tl_jcall *c) { (void)c; tl_netcfg g; c->ret = vz(net_bridge("LinkProperties.hasIPv4DefaultRoute", &g) && g.gateway[0]); }
static void LP_hasIPv4DnsServer(tl_jcall *c) { (void)c; tl_netcfg g; c->ret = vz(net_bridge("LinkProperties.hasIPv4DnsServer", &g) && g.ndns > 0); }
static void LP_isProvisioned(tl_jcall *c)
{
    (void)c;
    tl_netcfg g;
    c->ret = vz(net_bridge("LinkProperties.isProvisioned", &g) && g.ip[0] && g.gateway[0] && g.ndns > 0);
}

/* The phone has its network, as far as a game asking Android about it can tell: one active connection, over Wi-Fi, that is up. */
static const tl_jhle k_net[] = {
    M_("android/net/ConnectivityManager", "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;", CM_activeNetworkInfo),
    M_("android/net/ConnectivityManager", "getNetworkInfo", "(I)Landroid/net/NetworkInfo;", CM_activeNetworkInfo),
    M_("android/net/ConnectivityManager", "getActiveNetwork", "()Landroid/net/Network;", CM_activeNetwork),
    M_("android/net/ConnectivityManager", "getNetworkCapabilities", "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;", CM_capabilities),
    M_("android/net/NetworkInfo", "isConnected", "()Z", True_), M_("android/net/NetworkInfo", "isConnectedOrConnecting", "()Z", True_),
    M_("android/net/NetworkInfo", "isAvailable", "()Z", True_), M_("android/net/NetworkInfo", "getType", "()I", NI_type),
    M_("android/net/NetworkInfo", "getTypeName", "()Ljava/lang/String;", NI_typeName), M_("android/net/NetworkInfo", "getSubtype", "()I", NI_subtype),
    M_("android/net/NetworkCapabilities", "hasCapability", "(I)Z", True_), M_("android/net/NetworkCapabilities", "hasTransport", "(I)Z", True_),
    M_("android/net/ConnectivityManager", "getLinkProperties", "(Landroid/net/Network;)Landroid/net/LinkProperties;", CM_linkProperties),
    M_("android/net/LinkProperties", "getDnsServers", "()Ljava/util/List;", LP_dnsServers),
    M_("android/net/LinkProperties", "getInterfaceName", "()Ljava/lang/String;", LP_getInterfaceName),
    M_("android/net/LinkProperties", "getLinkAddresses", "()Ljava/util/List;", LP_getLinkAddresses),
    M_("android/net/LinkProperties", "getRoutes", "()Ljava/util/List;", LP_getRoutes),
    M_("android/net/LinkProperties", "getDomains", "()Ljava/lang/String;", LP_getDomains),
    M_("android/net/LinkProperties", "getMtu", "()I", LP_getMtu),
    M_("android/net/LinkProperties", "hasIPv4Address", "()Z", LP_hasIPv4Address),
    M_("android/net/LinkProperties", "hasIPv4DefaultRoute", "()Z", LP_hasIPv4DefaultRoute),
    M_("android/net/LinkProperties", "hasIPv4DnsServer", "()Z", LP_hasIPv4DnsServer),
    M_("android/net/LinkProperties", "isProvisioned", "()Z", LP_isProvisioned),
    M_("android/net/LinkProperties", "isIpv4Provisioned", "()Z", LP_isProvisioned),
    M_("android/net/LinkProperties", "hasIpv4Address", "()Z", LP_hasIPv4Address),
    M_("android/net/LinkProperties", "hasIpv4DefaultRoute", "()Z", LP_hasIPv4DefaultRoute),
    M_("android/net/LinkProperties", "hasIpv4DnsServer", "()Z", LP_hasIPv4DnsServer),
    M_("android/net/LinkAddress", "getAddress", "()Ljava/net/InetAddress;", LA_getAddress),
    M_("android/net/LinkAddress", "getPrefixLength", "()I", LA_getPrefixLength),
    M_("android/net/RouteInfo", "getGateway", "()Ljava/net/InetAddress;", RI_getGateway),
    M_("android/net/RouteInfo", "getInterface", "()Ljava/lang/String;", RI_getInterface),
    M_("android/net/RouteInfo", "isDefaultRoute", "()Z", True_),
    M_("android/net/wifi/WifiManager", "getConnectionInfo", "()Landroid/net/wifi/WifiInfo;", WM_getConnectionInfo),
    M_("android/net/wifi/WifiManager", "getDhcpInfo", "()Landroid/net/DhcpInfo;", WM_getDhcpInfo),
    M_("android/net/wifi/WifiManager", "isWifiEnabled", "()Z", WM_isWifiEnabled),
    M_("android/net/wifi/WifiManager", "getWifiState", "()I", WM_getWifiState),
    M_("android/net/wifi/WifiInfo", "getIpAddress", "()I", WI_getIpAddress),
    M_("android/net/wifi/WifiInfo", "getMacAddress", "()Ljava/lang/String;", WI_getMacAddress),
    M_("android/net/wifi/WifiInfo", "getBSSID", "()Ljava/lang/String;", WI_getBSSID),
    M_("android/net/wifi/WifiInfo", "getSSID", "()Ljava/lang/String;", WI_getSSID),
    M_("android/net/wifi/WifiInfo", "getLinkSpeed", "()I", WI_getLinkSpeed),
    M_("android/net/wifi/WifiInfo", "getFrequency", "()I", WI_getFrequency),
    M_("android/net/wifi/WifiInfo", "getNetworkId", "()I", WI_getNetworkId),
    M_("android/net/wifi/WifiInfo", "getRssi", "()I", WI_getRssi),
    M_("java/net/InetAddress", "getHostAddress", "()Ljava/lang/String;", IA_hostAddress),
    M_("java/net/InetAddress", "getAddress", "()[B", Inet_getAddress),
    M_("java/net/InetAddress", "getHostName", "()Ljava/lang/String;", Inet_getHostName),
    M_("java/net/InetAddress", "getCanonicalHostName", "()Ljava/lang/String;", Inet_getHostName),
    M_("java/net/InetAddress", "isLoopbackAddress", "()Z", Inet_isLoopbackAddress),
    M_("java/net/InetAddress", "isAnyLocalAddress", "()Z", Inet_isAnyLocalAddress),
    M_("java/net/InetAddress", "getLoopbackAddress", "()Ljava/net/InetAddress;", Inet_getLoopbackAddress),
    M_("java/net/InetAddress", "getLocalHost", "()Ljava/net/InetAddress;", Inet_getLocalHost),
    M_("java/net/NetworkInterface", "getNetworkInterfaces", "()Ljava/util/Enumeration;", NetIF_getNetworkInterfaces),
    M_("java/net/NetworkInterface", "getByName", "(Ljava/lang/String;)Ljava/net/NetworkInterface;", NetIF_getByName),
    M_("java/net/NetworkInterface", "getByIndex", "(I)Ljava/net/NetworkInterface;", NetIF_getByName),
    M_("java/net/NetworkInterface", "getByInetAddress", "(Ljava/net/InetAddress;)Ljava/net/NetworkInterface;", NetIF_getByName),
    M_("java/net/NetworkInterface", "getName", "()Ljava/lang/String;", NetIF_getName),
    M_("java/net/NetworkInterface", "getDisplayName", "()Ljava/lang/String;", NetIF_getDisplayName),
    M_("java/net/NetworkInterface", "getInetAddresses", "()Ljava/util/Enumeration;", NetIF_getInetAddresses),
    M_("java/net/NetworkInterface", "getInterfaceAddresses", "()Ljava/util/List;", NetIF_getInterfaceAddresses),
    M_("java/net/NetworkInterface", "getHardwareAddress", "()[B", NetIF_getHardwareAddress),
    M_("java/net/NetworkInterface", "isUp", "()Z", NetIF_isUp),
    M_("java/net/NetworkInterface", "isLoopback", "()Z", NetIF_isLoopback),
    M_("java/net/NetworkInterface", "isPointToPoint", "()Z", NetIF_isPointToPoint),
    M_("java/net/NetworkInterface", "isVirtual", "()Z", NetIF_isVirtual),
    M_("java/net/NetworkInterface", "supportsMulticast", "()Z", NetIF_supportsMulticast),
    M_("java/net/NetworkInterface", "getMTU", "()I", NetIF_getMTU),
    M_("java/net/NetworkInterface", "getIndex", "()I", NetIF_getIndex),
    M_("java/net/InterfaceAddress", "getAddress", "()Ljava/net/InetAddress;", IA2_getAddress),
    M_("java/net/InterfaceAddress", "getBroadcast", "()Ljava/net/InetAddress;", IA2_getBroadcast),
    M_("java/net/InterfaceAddress", "getNetworkPrefixLength", "()S", IA2_getNetworkPrefixLength),
    M_("java/util/Enumeration", "hasMoreElements", "()Z", Enum_hasMoreElements),
    M_("java/util/Enumeration", "nextElement", "()Ljava/lang/Object;", Enum_nextElement),
    { NULL, NULL, NULL, NULL }
};


/* ------------------------------------------------- Xbox's HttpClientRequest */

/*
 * Minecraft's web calls (Xbox services, the store) go through Microsoft's libHttpClient, whose Android side is a Java class over OkHttp.
 * The library builds a com.xbox.httpclient.HttpClientRequest, fills it in, calls doRequestAsync(call) and waits to be told, through
 * native methods on that class, how it went: OnRequestCompleted(call, response) -- after which it asks the response for its status, its headers
 * and (getResponseBodyBytes) its body, which the Java streams back through another native -- or OnRequestFailed. Here the request goes to the
 * host's network stack. It is carried out on the thread that asked, which is one of the library's workers, so the guest code that reports the
 * outcome runs where it expects to.
 */
#include "husk-tl-ld.h"

#define XREQ "com/xbox/httpclient/HttpClientRequest"
#define XRES "com/xbox/httpclient/HttpClientResponse"
#define XIN  "com/xbox/httpclient/HttpClientRequestBody$NativeInputStream"
#define XOUT "com/xbox/httpclient/HttpClientResponse$NativeOutputStream"

static void *x_native(const char *cls, const char *name, const char *sig, const char *mangled)
{
    void *fn = tl_jni_native(cls, name, sig);
    if (!fn) { tl_lib *lib = tl_ld_find_lib("libHttpClient.Android.so"); if (lib) fn = tl_ld_sym(lib, mangled); }
    if (!fn) tl_log_line("http: native %s.%s%s is not provided by libHttpClient", cls, name, sig);
    return fn;
}

typedef struct { char *url, *method, *ctype; char **hn, **hv; int nh, cap; int64_t call, clen; } xreq;
typedef struct { int64_t call; tl_http_response rs; } xres;

static xreq *XQ(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(xreq)); return o->native; }

static void XReq_init(tl_jcall *c) { (void)XQ(c->self); }
static void XReq_url(tl_jcall *c) { xreq *q = XQ(c->self); free(q->url); q->url = strdup(S(c->args[0].l)); }
static void XReq_header(tl_jcall *c)
{
    xreq *q = XQ(c->self);
    if (q->nh == q->cap) { q->cap = q->cap ? q->cap * 2 : 8; q->hn = realloc(q->hn, (size_t)q->cap * sizeof(char *)); q->hv = realloc(q->hv, (size_t)q->cap * sizeof(char *)); }
    q->hn[q->nh] = strdup(S(c->args[0].l)); q->hv[q->nh] = strdup(S(c->args[1].l)); q->nh++;
}
static void XReq_methodAndBody(tl_jcall *c)
{
    /* (String method, long call, String contentType, long contentLength) */
    xreq *q = XQ(c->self);
    free(q->method); q->method = strdup(S(c->args[0].l));
    q->call = c->args[1].j;
    free(q->ctype); q->ctype = c->args[2].l ? strdup(S(c->args[2].l)) : NULL;
    q->clen = c->args[3].j;
}

static void XReq_do(tl_jcall *c)
{
    xreq *q = XQ(c->self);
    int64_t call = c->args[0].j;
    void *env = tl_jni_env(), *self = c->self;
    typedef void (*done_fn)(void *env, void *self, int64_t call, void *response);
    typedef void (*fail_fn)(void *env, void *self, int64_t call, void *cls, void *trace, void *net, uint8_t unknown_host);
    typedef int (*read_fn)(void *env, void *stream, int64_t call, int64_t offset, void *buf, int64_t buf_off, int64_t len);
    done_fn done = x_native(XREQ, "OnRequestCompleted", "(JLcom/xbox/httpclient/HttpClientResponse;)V", "Java_com_xbox_httpclient_HttpClientRequest_OnRequestCompleted");
    fail_fn fail = x_native(XREQ, "OnRequestFailed", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V", "Java_com_xbox_httpclient_HttpClientRequest_OnRequestFailed");
    if (!done || !fail) return;

    /* the body, which the library holds and hands over in pieces on request */
    uint8_t *body = NULL; size_t body_len = 0;
    if (q->clen > 0) {
        read_fn rd = x_native(XIN, "nativeRead", "(JJ[BJJ)I", "Java_com_xbox_httpclient_HttpClientRequestBody_00024NativeInputStream_nativeRead");
        if (rd) {
            const int64_t CH = 64 * 1024;
            jobj *buf = tl_jni_new_prim_array('B', (uint32_t)CH);
            jobj *stream = tl_jni_new_object(tl_jni_class(XIN));
            for (;;) {
                int n = rd(env, stream, call, (int64_t)body_len, buf, 0, CH);
                if (n <= 0) break;
                body = realloc(body, body_len + (size_t)n);
                memcpy(body + body_len, buf->arr.data, (size_t)n);
                body_len += (size_t)n;
            }
            tl_jni_unref(buf); tl_jni_unref(stream);
        }
    }

    tl_http_request rq = { .url = q->url, .method = q->method, .nheaders = q->nh, .header_names = (const char *const *)q->hn,
                           .header_values = (const char *const *)q->hv, .body = body, .body_len = body_len, .timeout_ms = 60000, .follow_redirects = true };
    xres *res = calloc(1, sizeof(*res));
    res->call = call;
    tl_log_line("http: %s %s (%zu bytes sent)", q->method ? q->method : "GET", q->url ? q->url : "", body_len);
    if (!tl_http_perform(&rq, &res->rs)) {
        tl_log_line("http: %s failed: %s", q->url ? q->url : "", res->rs.message);
        const char *cls = res->rs.error == TL_HTTP_UNKNOWN_HOST ? "java.net.UnknownHostException" : res->rs.error == TL_HTTP_TIMEOUT ? "java.net.SocketTimeoutException"
                        : (res->rs.error == TL_HTTP_SSL || res->rs.error == TL_HTTP_SSL_UNTRUSTED) ? "javax.net.ssl.SSLHandshakeException" : "java.io.IOException";
        jobj *a = tl_jni_new_string(cls), *b = tl_jni_new_string(res->rs.message), *n = tl_jni_new_string("Has active network: true");
        fail(env, self, call, a, b, n, res->rs.error == TL_HTTP_UNKNOWN_HOST);
        tl_jni_unref(a); tl_jni_unref(b); tl_jni_unref(n);
        tl_http_response_free(&res->rs); free(res);
    } else {
        tl_log_line("http: %s -> %d (%zu bytes)", q->url ? q->url : "", res->rs.status, res->rs.body_len);
        jobj *r = tl_jni_new_object(tl_jni_class(XRES));
        r->native = res;
        done(env, self, call, r);                 /* the library reads the response back, through the methods below */
        tl_jni_unref(r);
    }
    free(body);
}

static xres *XR(const tl_jcall *c) { return c->self ? c->self->native : NULL; }
static void XRes_code(tl_jcall *c) { const xres *r = XR(c); c->ret = vi(r ? r->rs.status : 0); }
static void XRes_numHeaders(tl_jcall *c) { const xres *r = XR(c); c->ret = vi(r && r->rs.nheaders > 1 ? r->rs.nheaders - 1 : 0); }   /* the first is the status line */
static void XRes_headerName(tl_jcall *c) { const xres *r = XR(c); int i = c->args[0].i + 1; c->ret = vl(r && i > 0 && i < r->rs.nheaders ? tl_jni_new_string(r->rs.header_names[i]) : NULL); }
static void XRes_headerValue(tl_jcall *c) { const xres *r = XR(c); int i = c->args[0].i + 1; c->ret = vl(r && i > 0 && i < r->rs.nheaders ? tl_jni_new_string(r->rs.header_values[i]) : NULL); }
static void XRes_body(tl_jcall *c)
{
    xres *r = XR(c);
    typedef void (*write_fn)(void *env, void *stream, int64_t call, void *bytes, int off, int len);
    write_fn wr = x_native(XOUT, "nativeWrite", "(J[BII)V", "Java_com_xbox_httpclient_HttpClientResponse_00024NativeOutputStream_nativeWrite");
    if (!r || !wr) return;
    void *env = tl_jni_env();
    jobj *stream = tl_jni_new_object(tl_jni_class(XOUT));
    const size_t CH = 64 * 1024;
    jobj *buf = tl_jni_new_prim_array('B', (uint32_t)CH);
    for (size_t off = 0; off < r->rs.body_len;) {
        size_t n = r->rs.body_len - off < CH ? r->rs.body_len - off : CH;
        memcpy(buf->arr.data, r->rs.body + off, n);
        wr(env, stream, r->call, buf, 0, (int)n);
        off += n;
    }
    tl_jni_unref(buf); tl_jni_unref(stream);
    free(r->rs.body); r->rs.body = NULL; r->rs.body_len = 0;
}

static const tl_jhle k_xbox[] = {
    M_(XREQ, "<init>", "(Landroid/content/Context;)V", XReq_init), M_(XREQ, "setHttpUrl", "(Ljava/lang/String;)V", XReq_url),
    M_(XREQ, "setHttpHeader", "(Ljava/lang/String;Ljava/lang/String;)V", XReq_header),
    M_(XREQ, "setHttpMethodAndBody", "(Ljava/lang/String;JLjava/lang/String;J)V", XReq_methodAndBody), M_(XREQ, "doRequestAsync", "(J)V", XReq_do),
    M_(XRES, "getResponseCode", "()I", XRes_code), M_(XRES, "getNumHeaders", "()I", XRes_numHeaders),
    M_(XRES, "getHeaderNameAtIndex", "(I)Ljava/lang/String;", XRes_headerName), M_(XRES, "getHeaderValueAtIndex", "(I)Ljava/lang/String;", XRes_headerValue),
    M_(XRES, "getResponseBodyBytes", "()V", XRes_body),
    M_("com/xbox/httpclient/NetworkObserver", "Initialize", "(Landroid/content/Context;)V", Noop), M_("com/xbox/httpclient/NetworkObserver", "Cleanup", "(Landroid/content/Context;)V", Noop),
    { NULL, NULL, NULL, NULL }
};

void tl_http_install(void)
{
    tl_jni_declare("android/net/NetworkInfo", "java/lang/Object"); tl_jni_declare("android/net/Network", "java/lang/Object");
    tl_jni_declare("android/net/LinkProperties", "java/lang/Object"); tl_jni_declare("java/net/InetAddress", "java/lang/Object");
    tl_jni_declare("android/net/NetworkCapabilities", "java/lang/Object");
    /* The bridge's own classes: what Settings > Network Bridge answers with (husk-tl-netcfg.c). */
    tl_jni_declare("android/net/LinkAddress", "java/lang/Object"); tl_jni_declare("android/net/RouteInfo", "java/lang/Object");
    tl_jni_declare("android/net/wifi/WifiManager", "java/lang/Object"); tl_jni_declare("android/net/wifi/WifiInfo", "java/lang/Object");
    tl_jni_declare("android/net/DhcpInfo", "java/lang/Object");
    tl_jni_register_hle(k_net);
    tl_jni_declare("com/xbox/httpclient/HttpClientRequest", "java/lang/Object"); tl_jni_declare("com/xbox/httpclient/HttpClientResponse", "java/lang/Object");
    tl_jni_declare("com/xbox/httpclient/HttpClientRequestBody$NativeInputStream", "java/lang/Object"); tl_jni_declare("com/xbox/httpclient/HttpClientResponse$NativeOutputStream", "java/lang/Object");
    tl_jni_declare("com/xbox/httpclient/NetworkObserver", "java/lang/Object");
    tl_jni_register_hle(k_xbox);
    tl_jni_declare("java/util/HashMap", "java/lang/Object");
    tl_jni_declare(CLS, "java/lang/Object");
    tl_jni_register_hle(k_hle);
}
