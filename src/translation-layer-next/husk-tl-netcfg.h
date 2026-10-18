/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The network bridge: the phone's own IPv4 configuration -- the address, netmask, broadcast, router and DNS its Wi-Fi (or
 * cellular) really has, as the iPhone's own DHCP client obtained them -- read from the host so a game can be told it.
 *
 * Why it is needed: a game's sockets are the host's sockets (husk-tl-bionic-net.c passes socket/bind/connect straight
 * through), so the guest is already on the phone's network with the phone's address. What was missing is what the game is
 * *told*: Android's network APIs answered with placeholders (WifiInfo.getIpAddress() 0, no DNS servers, no link addresses),
 * and a game that advertises itself on a LAN -- or binds to "its" address -- announced an address that is not the one its
 * packets come out of. With Settings > Network Bridge on, those APIs answer with this instead.
 *
 * There is no DHCP server and no virtual adapter here, on purpose: a guest address of its own (SLIRP's 10.0.2.15, say)
 * would be unreachable from the rest of the LAN, which is the opposite of what LAN play needs.
 */
#ifndef HUSK_TL_NETCFG_H
#define HUSK_TL_NETCFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many resolvers the bridge reports at most. */
#define TL_NETCFG_MAX_DNS 4
/* How long a reading is reused before the host is asked again. */
#define TL_NETCFG_TTL_S 3

typedef struct tl_netcfg {
    bool ok;
    char why[96];                       /* ok: empty; otherwise why there is nothing to report */
    char ifname[24];                    /* the host's own interface, e.g. "en0", "pdp_ip0" */
    char android_if[16];                /* what the guest should call it: "wlan0", "rmnet0", "eth0" */
    char ip[64], netmask[64], gateway[64], broadcast[64], mac[32];
    int prefix;                         /* the netmask as a prefix length (0 when unknown) */
    /* The same addresses the way Android stores them: the dotted quad's first octet in the low byte, which is what
       WifiInfo.getIpAddress(), DhcpInfo's fields and InetAddress take. */
    uint32_t ip_int, netmask_int, gateway_int, broadcast_int;
    char dns[TL_NETCFG_MAX_DNS][64];
    uint32_t dns_int[TL_NETCFG_MAX_DNS];
    int ndns;
} tl_netcfg;

/* The host's configuration right now, cached for TL_NETCFG_TTL_S. False (with `why` filled) when there is nothing to
   report -- no interface is up. `out` may be NULL to ask only whether it is available. */
bool tl_netcfg_read(tl_netcfg *out);

/* The phone's resolvers, as text, for code that resolves names itself (net.dns1, c-ares, ...). Falls back to public
   resolvers when the system has none. Returns how many were written. */
int tl_dns_servers(char out[][64], int max);

/* The bridge switch: what the guest is told. The host is read either way (the Settings screen shows it before the bridge
   is on). Until the app says (husk_tl_net_bridge_set), TL_NET_BRIDGE=1 in the environment turns it on. */
bool tl_netcfg_bridge(void);
void tl_netcfg_bridge_set(bool on);

/* One line in the log the first time a bridged API answers (the configuration the guest was told). */
void tl_netcfg_note(void);

/* The app's side of the same: the switch, and the configuration as JSON for the Settings screen (caller frees). */
bool husk_tl_net_bridge(void);
void husk_tl_net_bridge_set(bool on);
char *husk_tl_net_report(void);

/* "192.168.1.42" -> the uint32 Android's WifiInfo/DhcpInfo use; 0 when it does not parse. */
uint32_t tl_netcfg_addr_int(const char *ipv4);

/* For getifaddrs: whether an interface is one of the host's own, which no Android device has ever had (AWDL, the low
   latency WLAN, a VPN's tunnels, the hotspot bridge); and the interface a guest should see as its own, with the name
   Android would give it ("wlan0", "rmnet0", "eth0"). Both answer false/zero when nothing is up. */
bool tl_netcfg_if_hidden(const char *name);
bool tl_netcfg_primary(char *host_name, size_t n, char *android_name, size_t m);

#ifdef __cplusplus
}
#endif

#endif
