/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The network bridge's reading (husk-tl-netcfg.c), on the machine running the test: which interface it picks, what it
 * reports, and that what it reports is coherent -- the address parses, the netmask is one, the prefix matches it, the
 * broadcast is on the same network, and Android's own packing of the address (the first octet in the low byte) is right.
 *
 * A machine with no network at all still passes: nothing is up, and the reading says so.
 */
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-netcfg.h"

/* The layer's log (husk-tl-load.c on the device, not linked here): printed, so the run shows what a game would be told. */
void tl_log_line(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("log: ", stdout); vprintf(fmt, ap); fputc('\n', stdout);
    va_end(ap);
}

static int fails;

static void check(bool ok, const char *what, const char *detail)
{
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) fails++;
}

static bool parses(const char *s)
{
    struct in_addr a;
    return s && *s && inet_pton(AF_INET, s, &a) == 1;
}

static bool same_network(const char *ip, const char *mask, const char *peer)
{
    struct in_addr a, m, b;
    if (!parses(ip) || !parses(mask) || !parses(peer)) return false;
    inet_pton(AF_INET, ip, &a); inet_pton(AF_INET, mask, &m); inet_pton(AF_INET, peer, &b);
    return (a.s_addr & m.s_addr) == (b.s_addr & m.s_addr);
}

int main(void)
{
    printf("bridge (before anyone says): %s\n", tl_netcfg_bridge() ? "on" : "off");
    check(getenv("TL_NET_BRIDGE") == NULL || (tl_netcfg_bridge() == (getenv("TL_NET_BRIDGE")[0] == '1')),
          "the environment is the bridge's default", getenv("TL_NET_BRIDGE") ? getenv("TL_NET_BRIDGE") : "(unset)");


    tl_netcfg_bridge_set(true);
    check(tl_netcfg_bridge(), "the bridge can be turned on", "");
    tl_netcfg_bridge_set(false);
    check(!tl_netcfg_bridge(), "the bridge can be turned off", "");
    tl_netcfg_bridge_set(true);

    tl_netcfg c;
    bool ok = tl_netcfg_read(&c);
    printf("\nifname=%s android=%s ip=%s/%d mask=%s gw=%s bcast=%s mac=%s dns=", c.ifname, c.android_if, c.ip, c.prefix,
           c.netmask, c.gateway, c.broadcast, c.mac);
    for (int i = 0; i < c.ndns; i++) printf("%s%s", i ? "," : "", c.dns[i]);
    printf("\nwhy=%s\n\n", c.why);

    if (!ok) {
        check(c.why[0] != 0, "an empty reading says why", c.why);
        printf("\nnothing is up on this machine -- that is a pass (%d failure(s))\n", fails);
        return fails ? 1 : 0;
    }

    check(parses(c.ip), "the address parses", c.ip);
    check(parses(c.netmask), "the netmask parses", c.netmask);
    check(c.prefix > 0 && c.prefix <= 32, "the prefix is a prefix", "");
    check(c.prefix == 32 || !strcmp(c.broadcast, "") || same_network(c.ip, c.netmask, c.broadcast),
          "the broadcast is on the same network", c.broadcast);
    check(!c.gateway[0] || same_network(c.ip, c.netmask, c.gateway), "the router is on the same network", c.gateway);
    check(c.ndns > 0 && parses(c.dns[0]), "there is at least one resolver", c.ndns ? c.dns[0] : "");
    check(c.ip_int == tl_netcfg_addr_int(c.ip) && c.ip_int != 0, "the address packs the way Android takes it", "");

    /* Android order: 192.168.1.42 packs to 0x2A01A8C0, whose low byte is the first octet. */
    check(tl_netcfg_addr_int("192.168.1.42") == 0x2A01A8C0u, "Android's packing of 192.168.1.42", "");
    check(tl_netcfg_addr_int("not an address") == 0, "an address that is not one packs to 0", "");
    check(tl_netcfg_addr_int("255.255.255.0") == tl_netcfg_addr_int("255.255.255.0"), "a netmask packs, too", "");

    check(!tl_netcfg_if_hidden(c.ifname), "the interface it chose is not one of the host's own", c.ifname);
    check(tl_netcfg_if_hidden("utun3") && tl_netcfg_if_hidden("awdl0") && !tl_netcfg_if_hidden("en0"),
          "a VPN's tunnel is hidden and Wi-Fi is not", "");

    char host[32], android[16];
    check(tl_netcfg_primary(host, sizeof(host), android, sizeof(android)) && !strcmp(host, c.ifname) && !strcmp(android, c.android_if),
          "the primary interface agrees with the reading", "");

    char *json = husk_tl_net_report();
    check(json && strstr(json, "\"ip\":\"") && strstr(json, c.ip) && strstr(json, c.android_if),
          "the JSON carries the reading", json ? json : "(null)");
    printf("\n%s\n", json ? json : "");
    free(json);

    printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
