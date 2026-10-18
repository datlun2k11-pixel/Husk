package android.net.wifi;

/**
 * Husk's Wi-Fi, for apps run on the Dalvik runtime: Android's generated WifiManager answers with a phone-less WifiInfo
 * (getIpAddress() 0) and a null DhcpInfo, which is nothing like a phone on a network.
 *
 * With Settings &gt; Network Bridge on, this one answers with the phone's own Wi-Fi as its DHCP lease left it -- the address,
 * netmask, router and resolvers husk.Native.networkConfig() reports (husk-tl-netcfg.c) -- and that address is the same one
 * a socket from here goes out on, because Husk hands the host's network stack to the guest whole. A LAN game that
 * announces itself needs exactly that. The switch itself is in Husk's Settings; here it is read every time.
 */
public final class HuskWifi extends WifiManager {
    public HuskWifi() { super(); }

    /** {host interface, Android's name for it, address, netmask, prefix, router, broadcast, MAC, up to four resolvers}, or null. */
    private static String[] host() {
        try { return husk.Native.networkConfig(); } catch (Throwable t) { return null; }
    }
    private static boolean up() { String[] c = host(); return c != null && c.length > 2 && !c[2].isEmpty(); }
    /** "192.168.1.42" the way Android stores it: the first octet in the low byte. */
    private static int packed(String s) {
        if (s == null || s.isEmpty()) return 0;
        String[] q = s.split("\\.");
        if (q.length != 4) return 0;
        int v = 0;
        for (int i = 0; i < 4; i++) {
            int o;
            try { o = Integer.parseInt(q[i]); } catch (NumberFormatException e) { return 0; }
            if (o < 0 || o > 255) return 0;
            v |= (o & 0xff) << (8 * i);
        }
        return v;
    }

    @Override public WifiInfo getConnectionInfo() {
        WifiInfo w = new WifiInfo();
        /* What Android shows an app without the location permission, and what Husk answered before the bridge. */
        w.huskProps.put("SSID", "<unknown ssid>");
        w.huskProps.put("BSSID", "02:00:00:00:00:00");
        w.huskProps.put("MacAddress", "02:00:00:00:00:00");
        w.huskProps.put("NetworkId", Integer.valueOf(-1));
        w.huskProps.put("Rssi", Integer.valueOf(-55));
        w.huskProps.put("LinkSpeed", Integer.valueOf(433));
        w.huskProps.put("Frequency", Integer.valueOf(2412));
        w.huskProps.put("IpAddress", Integer.valueOf(0));
        String[] c = host();
        if (c != null && c.length > 7) {
            if (!c[7].isEmpty()) { w.huskProps.put("MacAddress", c[7]); w.huskProps.put("BSSID", c[7]); }
            if (!c[2].isEmpty()) w.huskProps.put("IpAddress", Integer.valueOf(packed(c[2])));
        }
        return w;
    }

    @Override public android.net.DhcpInfo getDhcpInfo() {
        android.net.DhcpInfo d = new android.net.DhcpInfo();
        String[] c = host();
        if (c != null && c.length > 7 && !c[2].isEmpty()) {
            d.ipAddress = packed(c[2]);
            d.netmask = packed(c[3]);
            d.gateway = packed(c[5]);
            d.dns1 = c.length > 8 ? packed(c[8]) : 0;
            d.dns2 = c.length > 9 ? packed(c[9]) : 0;
            d.serverAddress = d.gateway;              /* on any home LAN the router is the DHCP server */
            d.leaseDuration = 86400;
        }
        return d;
    }

    @Override public boolean isWifiEnabled() { return up(); }
    @Override public int getWifiState() { return up() ? 3 : 0; }    /* 3: WIFI_STATE_ENABLED */
}
