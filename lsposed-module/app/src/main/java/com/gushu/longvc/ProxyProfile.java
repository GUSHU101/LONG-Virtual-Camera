package com.gushu.longvc;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.net.IDN;
import java.net.URI;
import java.util.Locale;
import java.util.UUID;

final class ProxyProfile {
    enum Protocol {
        SOCKS5("socks", false), HTTP("http", false), HTTPS("http", true),
        VLESS("vless", true), VMESS("vmess", true), HYSTERIA2("hysteria2", true);

        final String outbound;
        final boolean tls;

        Protocol(String outbound, boolean tls) {
            this.outbound = outbound;
            this.tls = tls;
        }
    }

    private final Protocol protocol;
    private final String server;
    private final int port;
    private final String user;
    private final String secret;
    private final String serverName;

    ProxyProfile(Protocol protocol, String host, String port, String user, String secret, String serverName) {
        this.protocol = protocol;
        this.server = hostName(host);
        try {
            this.port = Integer.parseInt(port.trim());
        } catch (NumberFormatException invalid) {
            throw new IllegalArgumentException("port");
        }
        if (this.port < 1 || this.port > 65535) throw new IllegalArgumentException("port");
        this.user = user.trim();
        this.secret = secret;
        this.serverName = serverName.isBlank() ? server : hostName(serverName);
        if (secret.length() > 512 || hasControls(secret) || this.user.length() > 255 || hasControls(this.user)) {
            throw new IllegalArgumentException("credentials");
        }
        switch (protocol) {
            case VLESS, VMESS -> {
                try {
                    if (!UUID.fromString(this.user).toString().equalsIgnoreCase(this.user)) {
                        throw new IllegalArgumentException();
                    }
                } catch (IllegalArgumentException invalid) {
                    throw new IllegalArgumentException("uuid");
                }
            }
            case HYSTERIA2 -> {
                if (secret.isEmpty()) throw new IllegalArgumentException("password");
            }
            case SOCKS5, HTTP, HTTPS -> {
                if (this.user.isEmpty() && !secret.isEmpty()) throw new IllegalArgumentException("username");
                if (protocol == Protocol.SOCKS5 && (this.user.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 255
                        || secret.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 255)) {
                    throw new IllegalArgumentException("credentials");
                }
            }
        }
    }

    JSONObject config(boolean system) throws JSONException {
        JSONObject upstream = new JSONObject().put("type", protocol.outbound).put("tag", "upstream")
                .put("server", server).put("server_port", port);
        if (protocol.tls) {
            upstream.put("tls", new JSONObject().put("enabled", true).put("server_name", serverName));
        }
        switch (protocol) {
            case SOCKS5, HTTP, HTTPS -> {
                if (protocol == Protocol.SOCKS5) upstream.put("version", "5");
                if (!user.isEmpty()) upstream.put("username", user).put("password", secret);
            }
            case VLESS -> upstream.put("uuid", user);
            case VMESS -> upstream.put("uuid", user).put("security", "auto");
            case HYSTERIA2 -> upstream.put("password", secret);
        }
        JSONArray dnsServers = new JSONArray().put(new JSONObject().put("type", "local").put("tag", "bootstrap"));
        JSONObject route = new JSONObject().put("final", "upstream")
                .put("auto_detect_interface", true).put("default_domain_resolver", "bootstrap");
        JSONArray outbounds = new JSONArray().put(upstream);
        JSONObject config = new JSONObject().put("log", new JSONObject().put("disabled", true))
                .put("outbounds", outbounds).put("route", route);
        if (system) {
            dnsServers.put(new JSONObject().put("type", "https").put("tag", "tunnel-dns")
                    .put("server", "1.1.1.1").put("detour", "upstream"));
            outbounds.put(new JSONObject().put("type", "direct").put("tag", "lan"));
            JSONArray rules = new JSONArray()
                    .put(new JSONObject().put("port", 53).put("action", "hijack-dns"))
                    .put(new JSONObject().put("ip_is_private", true).put("outbound", "lan"));
            if (protocol == Protocol.HTTP || protocol == Protocol.HTTPS) {
                rules.put(new JSONObject().put("network", "udp").put("action", "reject"));
            }
            route.put("rules", rules);
            config.put("inbounds", new JSONArray().put(new JSONObject().put("type", "tun")
                    .put("interface_name", "lvcproxy0")
                    .put("address", new JSONArray().put("172.30.93.1/30").put("fd00:3976:1::1/126"))
                    .put("mtu", 1400).put("auto_route", true).put("strict_route", true)
                    .put("iproute2_table_index", 39761).put("iproute2_rule_index", 8850)));
        }
        return config.put("dns", new JSONObject().put("servers", dnsServers)
                .put("final", system ? "tunnel-dns" : "bootstrap"));
    }

    static String testUrl(String value) {
        String url = value.trim();
        if (url.length() > 2048 || hasControls(url)) throw new IllegalArgumentException("url");
        URI parsed = URI.create(url);
        if (!("http".equals(parsed.getScheme()) || "https".equals(parsed.getScheme()))
                || parsed.getHost() == null || parsed.getRawUserInfo() != null || parsed.getPort() > 65535) {
            throw new IllegalArgumentException("url");
        }
        return parsed.toASCIIString();
    }

    private static String hostName(String value) {
        String host = value.trim();
        if (host.startsWith("[") && host.endsWith("]")) host = host.substring(1, host.length() - 1);
        if (host.indexOf(':') >= 0) {
            if (!host.matches("[a-fA-F0-9:.]+") || URI.create("http://[" + host + "]").getHost() == null) {
                throw new IllegalArgumentException("host");
            }
            return host;
        }
        String ascii = IDN.toASCII(host, IDN.USE_STD3_ASCII_RULES).toLowerCase(Locale.ROOT);
        if (ascii.isEmpty() || ascii.length() > 253 || !ascii.matches("[a-z0-9.-]+")) {
            throw new IllegalArgumentException("host");
        }
        return ascii;
    }

    private static boolean hasControls(String value) {
        return value.chars().anyMatch(ch -> ch <= 31 || ch == 127);
    }
}
