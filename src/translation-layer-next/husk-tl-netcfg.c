/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The phone's own network configuration (husk-tl-netcfg.h): read with getifaddrs, the kernel's routing table (where the
 * SDK has one -- iOS's has none, and no routing table is given to a sandboxed app, so the router comes from the resolvers
 * or from the network itself there) and the system resolver, and handed back as the IPv4 addresses Android's APIs use.
 * Nothing here needs an entitlement that a sideloaded app does not have: the interface addresses and the resolvers are
 * the app's own.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-netcfg.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
/* The routing table, where the SDK has one: iOS does not ship net/route.h, and a sandboxed app is not given the table
   there, so the router comes from the rest of the reading (fill_gateway). -DHUSK_HAVE_ROUTE_H=0 compiles the way iOS
   builds, which the workflow does, so that half of the file is compiled on every push and not only on a device. */
#ifndef HUSK_HAVE_ROUTE_H
#if __has_include(<net/route.h>)
#include <net/route.h>
#define HUSK_HAVE_ROUTE_H 1
#else
#define HUSK_HAVE_ROUTE_H 0
#endif
#endif
#include <pthread.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <time.h>

#include "husk-tl-bionic.h"      /* tl_log_line */

/* ------------------------------------------------------------------ the switch */

static int g_bridge = -1;        /* -1: nobody has said; 0/1: the app's setting (or TL_NET_BRIDGE, before that) */

bool tl_netcfg_bridge(void)
{
    if (g_bridge < 0) {
        const char *e = getenv("TL_NET_BRIDGE");
        g_bridge = (e && e[0] == '1') ? 1 : 0;
    }
    return g_bridge != 0;
}

void tl_netcfg_bridge_set(bool on) { g_bridge = on ? 1 : 0; }

bool husk_tl_net_bridge(void) { return tl_netcfg_bridge(); }
void husk_tl_net_bridge_set(bool on) { tl_netcfg_bridge_set(on); }

/* ------------------------------------------------------------- addresses */

uint32_t tl_netcfg_addr_int(const char *ipv4)
{
    struct in_addr a;
    if (!ipv4 || !*ipv4 || inet_pton(AF_INET, ipv4, &a) != 1) return 0;
    /* Android's own layout for these ints: the first octet of the dotted quad in the low byte (WifiInfo.getIpAddress,
       DhcpInfo's fields, InetAddress.inetAddressToInt). Network order in the bytes, native order in the value. */
    const uint8_t *b = (const uint8_t *)&a.s_addr;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int mask_prefix(const char *mask)
{
    struct in_addr a;
    if (!mask || !*mask || inet_pton(AF_INET, mask, &a) != 1) return 0;
    uint32_t m = ntohl(a.s_addr);
    int n = 0;
    while (m) { if (!(m & 0x80000000u)) return 0; n++; m <<= 1; }    /* a mask is ones then zeros; anything else is not one */
    return n;
}

/* ------------------------------------------------------------------ interfaces */

/* Apple's own interfaces -- AWDL, the low-latency WLAN, the tunnels of a VPN, the personal hotspot bridge. No Android
   device has ever had one, and a game that walks the interface list should not see them. */
bool tl_netcfg_if_hidden(const char *n)
{
    if (!n) return false;
    static const char *skip[] = { "awdl", "llw", "utun", "ipsec", "anpi", "bridge", "gif", "stf", "ap", "p2p", "vmenet" };
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) if (!strncmp(n, skip[i], strlen(skip[i]))) return true;
    return false;
}

/* Which interface a guest should see as "its own": Wi-Fi first, then any other Ethernet, then cellular. */
static int if_rank(const char *n)
{
    if (!strcmp(n, "en0")) return 0;               /* Wi-Fi on every iPhone */
    if (!strncmp(n, "en", 2)) return 10;
    if (!strcmp(n, "pdp_ip0")) return 20;          /* cellular data */
    if (!strncmp(n, "pdp_ip", 6)) return 21;
    return 50;
}

static void if_android_name(const char *host, int rank, char *out, size_t n)
{
    if (rank >= 20) snprintf(out, n, "rmnet%d", rank - 20 > 0 ? rank - 20 : 0);
    else if (rank < 10) snprintf(out, n, "wlan0");
    else snprintf(out, n, "eth0");
    (void)host;
}

typedef struct { char name[IFNAMSIZ + 1]; struct sockaddr_in ip, mask; bool have_ip, have_mask; unsigned flags; int rank; } cand;

static bool read_config(tl_netcfg *c)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) {
        snprintf(c->why, sizeof(c->why), "getifaddrs: %s", strerror(errno));
        return false;
    }

    cand cs[24];
    int n = 0;
    for (struct ifaddrs *i = list; i && n < (int)(sizeof(cs) / sizeof(cs[0])); i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!i->ifa_name || tl_netcfg_if_hidden(i->ifa_name)) continue;
        if (i->ifa_flags & IFF_LOOPBACK) continue;
        cand *k = &cs[n++];
        memset(k, 0, sizeof(*k));
        snprintf(k->name, sizeof(k->name), "%s", i->ifa_name);
        memcpy(&k->ip, i->ifa_addr, i->ifa_addr->sa_len < sizeof(k->ip) ? i->ifa_addr->sa_len : sizeof(k->ip));
        k->have_ip = true;
        if (i->ifa_netmask) memcpy(&k->mask, i->ifa_netmask, i->ifa_netmask->sa_len < sizeof(k->mask) ? i->ifa_netmask->sa_len : sizeof(k->mask));
        k->have_mask = i->ifa_netmask != NULL;
        k->flags = i->ifa_flags;
        k->rank = if_rank(i->ifa_name);
    }

    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!(cs[i].flags & IFF_UP) || !(cs[i].flags & IFF_RUNNING)) continue;
        if (best < 0 || cs[i].rank < cs[best].rank) best = i;
    }
    if (best < 0) {                                  /* nothing up with an address: the phone is offline */
        freeifaddrs(list);
        snprintf(c->why, sizeof(c->why), "no interface is up with an IPv4 address");
        return false;
    }

    inet_ntop(AF_INET, &cs[best].ip.sin_addr, c->ip, sizeof(c->ip));
    c->ip_int = tl_netcfg_addr_int(c->ip);
    if (cs[best].have_mask) inet_ntop(AF_INET, &cs[best].mask.sin_addr, c->netmask, sizeof(c->netmask));
    c->netmask_int = tl_netcfg_addr_int(c->netmask);
    c->prefix = mask_prefix(c->netmask);
    snprintf(c->ifname, sizeof(c->ifname), "%s", cs[best].name);
    if_android_name(cs[best].name, cs[best].rank, c->android_if, sizeof(c->android_if));

    /* The broadcast address and the MAC are in the same list: the first by name, the second its AF_LINK entry. */
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_name || strcmp(i->ifa_name, c->ifname)) continue;
        if (i->ifa_dstaddr && (i->ifa_flags & IFF_BROADCAST) && i->ifa_dstaddr->sa_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_dstaddr)->sin_addr, c->broadcast, sizeof(c->broadcast));
            c->broadcast_int = tl_netcfg_addr_int(c->broadcast);
        } else if (i->ifa_addr && i->ifa_addr->sa_family == AF_LINK) {
            struct sockaddr_dl *dl = (struct sockaddr_dl *)i->ifa_addr;
            const uint8_t *a = (const uint8_t *)LLADDR(dl);
            if (dl->sdl_alen == 6) snprintf(c->mac, sizeof(c->mac), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    }
    /* An interface that reports a broadcast address sometimes reports 0.0.0.0 early in the lease. */
    if (!c->broadcast[0] || !strcmp(c->broadcast, "0.0.0.0")) {
        if (c->prefix > 0 && c->prefix < 32) {
            uint32_t host = ntohl((uint32_t)cs[best].ip.sin_addr.s_addr) | (c->prefix ? ~((uint32_t)0xffffffffu << (32 - c->prefix)) : 0);
            struct in_addr b; b.s_addr = htonl(host);
            inet_ntop(AF_INET, &b, c->broadcast, sizeof(c->broadcast));
            c->broadcast_int = tl_netcfg_addr_int(c->broadcast);
        }
    }
    freeifaddrs(list);
    return true;
}

/* Whether this build reads the kernel's routing table for the router here: false when the SDK ships none (iOS), and on
   TL_NETCFG_NO_ROUTES=1, which is how the reading is tested where the table does exist. */
bool tl_netcfg_has_routes(void)
{
#if HUSK_HAVE_ROUTE_H
    const char *e = getenv("TL_NETCFG_NO_ROUTES");
    return !(e && e[0] == '1');
#else
    return false;
#endif
}

/* The router: the gateway of the IPv4 default route out of this interface, from the kernel's routing table.
 *
 * Only where the platform has one to read. The iOS SDK has no net/route.h at all -- a sandboxed app is not given the
 * routing table -- so there the router is worked out from the rest of the reading instead (fill_gateway). The rest of the
 * configuration (the address, the netmask, the broadcast address, the MAC, the resolvers) is the app's own on either. */
static bool gateway_from_routes(const char *ifname, char *out, size_t n)
{
    if (!tl_netcfg_has_routes()) { (void)ifname; (void)out; (void)n; return false; }
#if HUSK_HAVE_ROUTE_H
    int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0 };
    size_t len = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len < sizeof(struct rt_msghdr)) return false;
    char *buf = malloc(len);
    if (!buf) return false;
    if (sysctl(mib, 6, buf, &len, NULL, 0) != 0) { free(buf); return false; }

    bool found = false;
    for (char *p = buf; p + sizeof(struct rt_msghdr) <= buf + len; ) {
        struct rt_msghdr *rtm = (struct rt_msghdr *)p;
        if (rtm->rtm_msglen < sizeof(struct rt_msghdr) || p + rtm->rtm_msglen > buf + len) break;
        p += rtm->rtm_msglen;
        if (!(rtm->rtm_flags & RTF_UP) || !(rtm->rtm_flags & RTF_GATEWAY) || !(rtm->rtm_addrs & RTA_DST) || !(rtm->rtm_addrs & RTA_GATEWAY)) continue;

        char this[IFNAMSIZ + 1] = "";
        if (ifname && *ifname && (rtm->rtm_index == 0 || !if_indextoname(rtm->rtm_index, this) || strcmp(this, ifname))) continue;

        struct sockaddr *dst = NULL, *gw = NULL;
        int slot = 0;
        for (char *q = p - rtm->rtm_msglen + sizeof(struct rt_msghdr); slot < RTAX_MAX; slot++) {
            if (!(rtm->rtm_addrs & (1 << slot))) continue;
            struct sockaddr *sa = (struct sockaddr *)q;
            size_t step = sa->sa_len ? ((sa->sa_len + 3) & ~(size_t)3) : sizeof(uint32_t);
            if (slot == RTAX_DST) dst = sa;
            else if (slot == RTAX_GATEWAY) gw = sa;
            if ((char *)sa + step > buf + len) break;
            q += step;
        }
        if (!gw || gw->sa_family != AF_INET) continue;
        /* the default route: the destination is 0.0.0.0 (a dump may leave the slot empty for it) */
        if (dst && dst->sa_len >= (socklen_t)sizeof(struct sockaddr_in) && dst->sa_family == AF_INET
            && ((struct sockaddr_in *)dst)->sin_addr.s_addr != 0) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)gw)->sin_addr, out, (socklen_t)n);
        found = true;
        break;
    }
    free(buf);
    return found;
#else
    (void)ifname; (void)out; (void)n;
    return false;
#endif
}

/* Whether two addresses are on the network a netmask leaves. */
static bool same_net(const char *a, const char *b, const char *mask)
{
    struct in_addr x, y, m;
    if (inet_pton(AF_INET, a, &x) != 1 || inet_pton(AF_INET, b, &y) != 1 || inet_pton(AF_INET, mask, &m) != 1) return false;
    return (x.s_addr & m.s_addr) == (y.s_addr & m.s_addr);
}

/* The first address of our network (192.168.1.1 for 192.168.1.42/24): where a router almost always is. */
static bool subnet_router(tl_netcfg *c)
{
    struct in_addr ip, mask, g;
    if (c->prefix <= 0 || c->prefix >= 31) return false;
    if (inet_pton(AF_INET, c->ip, &ip) != 1 || inet_pton(AF_INET, c->netmask, &mask) != 1) return false;
    g.s_addr = htonl((ntohl(ip.s_addr) & ntohl(mask.s_addr)) | 1u);
    return inet_ntop(AF_INET, &g, c->gateway, sizeof(c->gateway)) != NULL;
}

/* The router of the network we are on, from what can be read here. */
static void fill_gateway(tl_netcfg *c)
{
    if (gateway_from_routes(c->ifname, c->gateway, sizeof(c->gateway))) return;
    /* No routing table to read (iOS), or no default route in it. On a home network the router is the resolver, so a
       resolver on our own network is taken as it; otherwise the first address of the network is, which is where a router
       almost always is, and which keeps the answer on the network either way. */
    if (c->ndns > 0 && same_net(c->dns[0], c->ip, c->netmask)) { snprintf(c->gateway, sizeof(c->gateway), "%s", c->dns[0]); return; }
    if (subnet_router(c)) return;
    if (c->ndns > 0) { snprintf(c->gateway, sizeof(c->gateway), "%s", c->dns[0]); return; }
    snprintf(c->why, sizeof(c->why), "no default route to report");
}

/* ------------------------------------------------------------------ resolvers */

int tl_dns_servers(char out[][64], int max)
{
    int n = 0;
    struct __res_state st;
    memset(&st, 0, sizeof(st));
    if (res_ninit(&st) == 0) {
        union res_sockaddr_union addrs[8];
        int got = res_getservers(&st, addrs, 8);
        for (int i = 0; i < got && n < max; i++) {
            char one[64] = "";
            if (addrs[i].sin.sin_family == AF_INET) inet_ntop(AF_INET, &addrs[i].sin.sin_addr, one, sizeof(one));
            else if (addrs[i].sin6.sin6_family == AF_INET6) inet_ntop(AF_INET6, &addrs[i].sin6.sin6_addr, one, sizeof(one));
            else continue;
            if (!one[0]) continue;
            bool seen = false;                                     /* the resolver state often lists a server twice */
            for (int j = 0; j < n; j++) if (!strcmp(out[j], one)) seen = true;
            if (!seen) snprintf(out[n++], 64, "%s", one);
        }
        res_ndestroy(&st);
    }
    static const char *fallback[] = { "1.1.1.1", "8.8.8.8" };
    for (int i = 0; i < 2 && n < max; i++) {
        bool seen = false;
        for (int j = 0; j < n; j++) if (!strcmp(out[j], fallback[i])) seen = true;
        if (!seen) snprintf(out[n++], 64, "%s", fallback[i]);
    }
    return n;
}

/* ------------------------------------------------------------------ the reading */

static bool fill(tl_netcfg *c)
{
    memset(c, 0, sizeof(*c));
    if (!read_config(c)) return false;
    c->ndns = tl_dns_servers(c->dns, TL_NETCFG_MAX_DNS);
    for (int i = 0; i < c->ndns; i++) c->dns_int[i] = tl_netcfg_addr_int(c->dns[i]);
    /* After the resolvers: where the routing table cannot be read, one of them is the router. */
    fill_gateway(c);
    c->gateway_int = tl_netcfg_addr_int(c->gateway);
    c->ok = true;
    return true;
}

bool tl_netcfg_read(tl_netcfg *out)
{
    static tl_netcfg cached;
    static bool have;
    static time_t at;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);                      /* a game thread and a getifaddrs on another: one reading at a time */
    time_t now = time(NULL);
    if (!have || now - at >= TL_NETCFG_TTL_S) {
        fill(&cached);
        at = now;
        have = true;
    }
    if (out) *out = cached;
    bool ok = cached.ok;
    pthread_mutex_unlock(&lock);
    return ok;
}

/* The interface a guest should see as its own, and what Android would call it. */
bool tl_netcfg_primary(char *host_name, size_t n, char *android_name, size_t m)
{
    tl_netcfg c;
    if (!tl_netcfg_read(&c)) return false;
    if (host_name) snprintf(host_name, n, "%s", c.ifname);
    if (android_name) snprintf(android_name, m, "%s", c.android_if);
    return true;
}

/* One line, the first time a bridged API answers: what the game was told, in the log the app shows. */
void tl_netcfg_note(void)
{
    static bool said;
    if (said) return;
    said = true;
    tl_netcfg c;
    if (!tl_netcfg_read(&c)) { tl_log_line("net: bridge on, but nothing to report (%s)", c.why); return; }
    char dns[TL_NETCFG_MAX_DNS * 68 + 1]; dns[0] = 0;
    for (int i = 0; i < c.ndns; i++) { strncat(dns, c.dns[i], sizeof(dns) - strlen(dns) - 2); if (i + 1 < c.ndns) strncat(dns, " ", sizeof(dns) - strlen(dns) - 2); }
    tl_log_line("net: bridge on: %s is %s/%d (broadcast %s, mac %s) gw %s dns %s%s%s", c.ifname, c.ip, c.prefix,
                c.broadcast[0] ? c.broadcast : "?", c.mac[0] ? c.mac : "?", c.gateway[0] ? c.gateway : "?",
                dns[0] ? dns : "?", c.why[0] ? " (" : "", c.why[0] ? c.why : "");
}

/* ------------------------------------------------------------------ the app's JSON */

static void json_put(char *dst, size_t n, const char *src)
{
    size_t k = 0;
    for (const char *p = src; p && *p && k + 1 < n; p++) {
        if (*p == '"' || *p == '\\') continue;                     /* nothing here should have either; drop them if so */
        dst[k++] = *p;
    }
    dst[k] = 0;
}

char *husk_tl_net_report(void)
{
    tl_netcfg c;
    bool ok = tl_netcfg_read(&c);
    size_t cap = 512 + (size_t)TL_NETCFG_MAX_DNS * 80;
    char *s = malloc(cap);
    if (!s) return NULL;
    char why[96]; json_put(why, sizeof(why), c.why);
    int k = snprintf(s, cap, "{\"bridge\":%s,\"ok\":%s,\"why\":\"%s\",\"ifname\":\"%s\",\"android_if\":\"%s\","
                             "\"ip\":\"%s\",\"netmask\":\"%s\",\"prefix\":%d,\"gateway\":\"%s\",\"broadcast\":\"%s\",\"mac\":\"%s\",\"dns\":[",
                     tl_netcfg_bridge() ? "true" : "false", ok ? "true" : "false", why, c.ifname, c.android_if,
                     c.ip, c.netmask, c.prefix, c.gateway, c.broadcast, c.mac);
    for (int i = 0; i < c.ndns && k > 0 && (size_t)k < cap; i++)
        k += snprintf(s + k, cap - (size_t)k, "%s\"%s\"", i ? "," : "", c.dns[i]);
    if (k > 0 && (size_t)k < cap) snprintf(s + k, cap - (size_t)k, "]}");
    return s;
}
