// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Settings › Network Bridge (husk-tl-netcfg.c): what the Translation Layer tells a game about the iPhone's network.
///
/// A game's sockets are the iPhone's own — Husk hands the host's network stack straight to the guest — so the address a LAN
/// game should announce is the phone's address, as its DHCP lease left it. With the bridge on, the Android APIs a game asks
/// (`WifiManager`, `DhcpInfo`, `LinkProperties`, `getifaddrs`) answer with that address, its netmask, the router and the
/// resolvers; with it off they answer the placeholders they always did. The switch is a global of the layer, read every
/// time, so it can be turned off and on again — a game has to be started again to see the change.
enum NetworkBridge {
    /// Where the switch is kept (`husk.` keys are Husk's own settings).
    static let key = "husk.networkBridge"

    /// What the layer reports, as husk_tl_net_report() writes it.
    struct Info: Codable {
        var bridge = false
        var ok = false
        var why = ""
        var ifname = ""
        var android_if = ""
        var ip = ""
        var netmask = ""
        var gateway = ""
        var broadcast = ""
        var mac = ""
        var prefix = 0
        var dns: [String] = []
    }

    static var isOn: Bool { UserDefaults.standard.bool(forKey: key) }

    /// Tell the layer where the switch stands: at launch, when it is turned over, and before a game starts.
    static func apply() { husk_tl_net_bridge_set(isOn) }

    /// The iPhone's network as the layer sees it right now.
    static func read() -> Info {
        guard let p = husk_tl_net_report() else { return Info() }
        defer { husk_tl_free(p) }
        return (try? JSONDecoder().decode(Info.self, from: Data(String(cString: p).utf8))) ?? Info()
    }
}

/// The page: the switch, and what the layer would hand a game right now.
struct NetworkBridgeSettings: View {
    @AppStorage(NetworkBridge.key) private var bridge = false
    @State private var info = NetworkBridge.Info()

    var body: some View {
        Form {
            Section {
                Toggle("Bridge", isOn: $bridge)
                if bridge {
                    DetailRow(label: "Status", value: info.ok ? "On" : "No network", mono: false)
                    if info.ok {
                        DetailRow(label: "Address", value: info.prefix > 0 ? "\(info.ip)/\(info.prefix)" : info.ip)
                        if !info.gateway.isEmpty { DetailRow(label: "Router", value: info.gateway) }
                        if !info.dns.isEmpty { DetailRow(label: "DNS", value: info.dns.joined(separator: ", ")) }
                        if !info.mac.isEmpty { DetailRow(label: "Wi-Fi MAC", value: info.mac) }
                        DetailRow(label: "Interface",
                                  value: info.android_if.isEmpty ? info.ifname : "\(info.ifname) · games see \(info.android_if)")
                    } else if !info.why.isEmpty {
                        Text(info.why).font(.footnote).foregroundStyle(.secondary)
                    }
                    Button {
                        info = NetworkBridge.read()
                    } label: {
                        Label("Read the network again", systemImage: "arrow.clockwise")
                    }
                }
            } header: {
                Text("Network Bridge")
            } footer: {
                Text(bridge
                     ? "Games are told the iPhone's own address, netmask, router and DNS instead of empty placeholders, "
                       + "which is what LAN multiplayer needs to find them. Start the game again for it to take effect."
                     : "Off: games are told nothing about the iPhone's network, as before. Turn this on to play over the "
                       + "LAN (then start the game again).")
            }
        }
        .navigationTitle("Network Bridge")
        .huskForm()
        .onAppear {
            NetworkBridge.apply()
            info = NetworkBridge.read()
        }
        .onChange(of: bridge) { _ in
            NetworkBridge.apply()
            info = NetworkBridge.read()
        }
    }
}
