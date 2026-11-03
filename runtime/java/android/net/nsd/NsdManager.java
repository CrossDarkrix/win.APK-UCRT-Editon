package android.net.nsd;

import android.content.Context;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.net.NetworkInterface;
import java.net.SocketAddress;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * In-process DNS-SD/mDNS implementation used by the Windows runtime.
 * Browsing is continuous until stopServiceDiscovery() is called. Resolution
 * reads SRV, TXT, A and AAAA records and returns a populated NsdServiceInfo.
 */
public final class NsdManager {
    public static final int PROTOCOL_DNS_SD = 1;
    public static final int FAILURE_INTERNAL_ERROR = 0;
    public static final int FAILURE_ALREADY_ACTIVE = 3;
    public static final int FAILURE_MAX_LIMIT = 4;

    private static final String MDNS_V4 = "224.0.0.251";
    private static final int MDNS_PORT = 5353;
    private static final int TYPE_A = 1;
    private static final int TYPE_PTR = 12;
    private static final int TYPE_TXT = 16;
    private static final int TYPE_AAAA = 28;
    private static final int TYPE_SRV = 33;
    private static final int TYPE_ANY = 255;
    private static final int CLASS_IN = 1;

    public interface RegistrationListener {
        void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onServiceRegistered(NsdServiceInfo serviceInfo);
        void onServiceUnregistered(NsdServiceInfo serviceInfo);
    }
    public interface DiscoveryListener {
        void onStartDiscoveryFailed(String serviceType, int errorCode);
        void onStopDiscoveryFailed(String serviceType, int errorCode);
        void onDiscoveryStarted(String serviceType);
        void onDiscoveryStopped(String serviceType);
        void onServiceFound(NsdServiceInfo serviceInfo);
        void onServiceLost(NsdServiceInfo serviceInfo);
    }
    public interface ResolveListener {
        void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onServiceResolved(NsdServiceInfo serviceInfo);
    }

    private final Map<DiscoveryListener, DiscoverySession> sessions =
            Collections.synchronizedMap(new IdentityHashMap<DiscoveryListener, DiscoverySession>());

    public NsdManager() { }
    public NsdManager(Context context) { }

    public void registerService(NsdServiceInfo serviceInfo, int protocolType,
            RegistrationListener listener) {
        if (listener != null) listener.onRegistrationFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
    }

    public void unregisterService(RegistrationListener listener) {
        if (listener != null) listener.onUnregistrationFailed(null, FAILURE_INTERNAL_ERROR);
    }

    public void discoverServices(String serviceType, int protocolType,
            DiscoveryListener listener) {
        if (listener == null || serviceType == null || serviceType.trim().isEmpty()) {
            throw new IllegalArgumentException("serviceType and listener must be non-null");
        }
        if (protocolType != PROTOCOL_DNS_SD) {
            listener.onStartDiscoveryFailed(serviceType, FAILURE_INTERNAL_ERROR);
            return;
        }
        synchronized (sessions) {
            if (sessions.containsKey(listener)) {
                listener.onStartDiscoveryFailed(serviceType, FAILURE_ALREADY_ACTIVE);
                return;
            }
            DiscoverySession session = new DiscoverySession(serviceType, listener);
            sessions.put(listener, session);
            session.start();
        }
    }

    public void stopServiceDiscovery(DiscoveryListener listener) {
        if (listener == null) throw new IllegalArgumentException("listener == null");
        DiscoverySession session;
        synchronized (sessions) { session = sessions.remove(listener); }
        if (session == null) {
            listener.onStopDiscoveryFailed(null, FAILURE_INTERNAL_ERROR);
            return;
        }
        session.stop();
    }

    public void resolveService(final NsdServiceInfo serviceInfo, final ResolveListener listener) {
        if (serviceInfo == null || listener == null)
            throw new IllegalArgumentException("serviceInfo and listener must be non-null");
        Thread worker = new Thread(new Runnable() {
            @Override public void run() { resolveWorker(serviceInfo, listener); }
        }, "winapk-nsd-resolve");
        worker.setDaemon(true);
        worker.start();
    }

    private final class DiscoverySession implements Runnable {
        final String requestedType;
        final String serviceFqdn;
        final DiscoveryListener listener;
        final Map<String, Long> instances = new LinkedHashMap<String, Long>();
        volatile boolean running = true;
        volatile MulticastSocket socket;
        Thread thread;

        DiscoverySession(String serviceType, DiscoveryListener listener) {
            this.requestedType = serviceType;
            this.serviceFqdn = serviceFqdn(serviceType);
            this.listener = listener;
        }

        void start() {
            thread = new Thread(this, "winapk-nsd-browse");
            thread.setDaemon(true);
            thread.start();
        }

        void stop() {
            running = false;
            MulticastSocket s = socket;
            if (s != null) s.close();
            listener.onDiscoveryStopped(requestedType);
        }

        @Override public void run() {
            try {
                socket = openMulticastSocket(true);
                listener.onDiscoveryStarted(requestedType);
                long nextQuery = 0;
                byte[] receive = new byte[9000];
                while (running) {
                    long now = System.currentTimeMillis();
                    if (now >= nextQuery) {
                        sendQuery(socket, serviceFqdn, TYPE_PTR, false);
                        nextQuery = now + 3000;
                    }
                    try {
                        DatagramPacket packet = new DatagramPacket(receive, receive.length);
                        socket.receive(packet);
                        handleDiscoveryPacket(packet.getData(), packet.getLength(), now);
                    } catch (SocketTimeoutException ignored) { }
                    expire(now);
                }
            } catch (Throwable failure) {
                if (running) {
                    synchronized (sessions) { sessions.remove(listener); }
                    listener.onStartDiscoveryFailed(requestedType, FAILURE_INTERNAL_ERROR);
                }
            } finally {
                MulticastSocket s = socket;
                if (s != null) s.close();
            }
        }

        void handleDiscoveryPacket(byte[] data, int length, long now) throws IOException {
            DnsMessage message = DnsMessage.parse(data, length);
            for (DnsRecord record : message.records) {
                if (record.type != TYPE_PTR || !sameName(record.name, serviceFqdn)) continue;
                String instance = record.domainValue;
                if (instance == null || instance.isEmpty()) continue;
                if (record.ttl == 0) {
                    if (instances.remove(canonical(instance)) != null)
                        listener.onServiceLost(serviceInfo(instance, requestedType));
                    continue;
                }
                long expiry = now + Math.max(1000L, Math.min(record.ttl * 1000L, 4500000L));
                String key = canonical(instance);
                boolean added = !instances.containsKey(key);
                instances.put(key, expiry);
                if (added) listener.onServiceFound(serviceInfo(instance, requestedType));
            }
        }

        void expire(long now) {
            List<String> lost = new ArrayList<String>();
            for (Map.Entry<String, Long> entry : instances.entrySet())
                if (entry.getValue() <= now) lost.add(entry.getKey());
            for (String key : lost) {
                instances.remove(key);
                listener.onServiceLost(serviceInfo(key, requestedType));
            }
        }
    }

    private static void resolveWorker(NsdServiceInfo request, ResolveListener listener) {
        String type = request.getServiceType();
        String instance = instanceFqdn(request.getServiceName(), type);
        MulticastSocket socket = null;
        try {
            socket = openMulticastSocket(false);
            socket.setSoTimeout(400);
            ResolveData accumulated = new ResolveData(instance);
            long deadline = System.currentTimeMillis() + 6000L;
            long nextQuery = 0L;
            byte[] receive = new byte[9000];
            while (System.currentTimeMillis() < deadline) {
                long now = System.currentTimeMillis();
                if (now >= nextQuery) {
                    sendQuery(socket, instance, TYPE_SRV, true);
                    sendQuery(socket, instance, TYPE_TXT, true);
                    if (accumulated.target != null) {
                        sendQuery(socket, accumulated.target, TYPE_A, true);
                        sendQuery(socket, accumulated.target, TYPE_AAAA, true);
                    }
                    nextQuery = now + 750L;
                }
                try {
                    DatagramPacket packet = new DatagramPacket(receive, receive.length);
                    socket.receive(packet);
                    accumulated.add(DnsMessage.parse(packet.getData(), packet.getLength()));
                    if (accumulated.complete()) {
                        NsdServiceInfo resolved = new NsdServiceInfo();
                        resolved.setServiceName(displayName(instance, type));
                        resolved.setServiceType(type);
                        resolved.setPort(accumulated.port);
                        resolved.setHost(accumulated.preferredAddress());
                        for (Map.Entry<String, String> item : accumulated.txt.entrySet())
                            resolved.setAttribute(item.getKey(), item.getValue());
                        listener.onServiceResolved(resolved);
                        return;
                    }
                } catch (SocketTimeoutException ignored) { }
            }
            listener.onResolveFailed(request, FAILURE_INTERNAL_ERROR);
        } catch (Throwable failure) {
            listener.onResolveFailed(request, FAILURE_INTERNAL_ERROR);
        } finally {
            if (socket != null) socket.close();
        }
    }

    private static final class ResolveData {
        final String instance;
        String target;
        int port;
        final Map<String, String> txt = new LinkedHashMap<String, String>();
        final Map<String, InetAddress> addresses = new LinkedHashMap<String, InetAddress>();
        ResolveData(String instance) { this.instance = canonical(instance); }
        void add(DnsMessage message) {
            for (DnsRecord record : message.records) {
                if (record.type == TYPE_SRV && sameName(record.name, instance)) {
                    target = canonical(record.srvTarget);
                    port = record.srvPort;
                } else if (record.type == TYPE_TXT && sameName(record.name, instance)) {
                    txt.putAll(record.txtValues);
                } else if ((record.type == TYPE_A || record.type == TYPE_AAAA)
                        && record.address != null) {
                    addresses.put(canonical(record.name), record.address);
                }
            }
        }
        boolean complete() { return target != null && port > 0 && preferredAddress() != null; }
        InetAddress preferredAddress() {
            InetAddress fallback = null;
            for (Map.Entry<String, InetAddress> entry : addresses.entrySet()) {
                if (!sameName(entry.getKey(), target)) continue;
                if (entry.getValue() instanceof Inet4Address) return entry.getValue();
                if (fallback == null) fallback = entry.getValue();
            }
            return fallback;
        }
    }

    private static NsdServiceInfo serviceInfo(String instance, String serviceType) {
        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName(displayName(instance, serviceType));
        info.setServiceType(serviceType);
        return info;
    }

    private static String displayName(String instance, String serviceType) {
        String full = trimDot(instance);
        String suffix = "." + trimDot(serviceFqdn(serviceType));
        if (full.toLowerCase(Locale.ROOT).endsWith(suffix.toLowerCase(Locale.ROOT)))
            return full.substring(0, full.length() - suffix.length());
        return full;
    }

    private static String instanceFqdn(String name, String serviceType) {
        String n = trimDot(name);
        String service = trimDot(serviceFqdn(serviceType));
        if (n.toLowerCase(Locale.ROOT).endsWith(("." + service).toLowerCase(Locale.ROOT)))
            return n + ".";
        return n + "." + service + ".";
    }

    private static String serviceFqdn(String serviceType) {
        String value = trimDot(serviceType);
        if (!value.toLowerCase(Locale.ROOT).endsWith(".local")) value += ".local";
        return value + ".";
    }

    private static String trimDot(String value) {
        if (value == null) return "";
        String out = value.trim();
        while (out.endsWith(".")) out = out.substring(0, out.length() - 1);
        return out;
    }

    private static String canonical(String value) { return trimDot(value).toLowerCase(Locale.ROOT) + "."; }
    private static boolean sameName(String a, String b) { return canonical(a).equals(canonical(b)); }

    private static MulticastSocket openMulticastSocket(boolean continuous) throws IOException {
        MulticastSocket socket = new MulticastSocket(null);
        socket.setReuseAddress(true);
        socket.bind(new InetSocketAddress(continuous ? MDNS_PORT : 0));
        socket.setTimeToLive(255);
        socket.setSoTimeout(350);
        if (continuous) {
            InetAddress group = InetAddress.getByName(MDNS_V4);
            boolean joined = false;
            Enumeration<NetworkInterface> all = NetworkInterface.getNetworkInterfaces();
            while (all != null && all.hasMoreElements()) {
                NetworkInterface net = all.nextElement();
                try {
                    if (!net.isUp() || net.isLoopback() || !net.supportsMulticast()) continue;
                    socket.joinGroup(new InetSocketAddress(group, MDNS_PORT), net);
                    joined = true;
                } catch (Throwable ignored) { }
            }
            if (!joined) socket.joinGroup(group);
        }
        return socket;
    }

    private static void sendQuery(MulticastSocket socket, String name, int type,
            boolean requestUnicast) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream(256);
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeShort(0);      // transaction ID
        out.writeShort(0);      // standard query
        out.writeShort(1);      // one question
        out.writeShort(0); out.writeShort(0); out.writeShort(0);
        writeName(out, name);
        out.writeShort(type);
        out.writeShort(CLASS_IN | (requestUnicast ? 0x8000 : 0));
        out.flush();
        byte[] payload = bytes.toByteArray();
        DatagramPacket packet = new DatagramPacket(payload, payload.length,
                InetAddress.getByName(MDNS_V4), MDNS_PORT);
        socket.send(packet);
    }

    private static void writeName(DataOutputStream out, String name) throws IOException {
        String value = trimDot(name);
        if (!value.isEmpty()) {
            for (String label : value.split("\\.")) {
                byte[] encoded = label.getBytes(StandardCharsets.UTF_8);
                if (encoded.length > 63) throw new IOException("DNS label longer than 63 bytes");
                out.writeByte(encoded.length);
                out.write(encoded);
            }
        }
        out.writeByte(0);
    }

    private static final class DnsMessage {
        final List<DnsRecord> records = new ArrayList<DnsRecord>();
        static DnsMessage parse(byte[] data, int length) throws IOException {
            if (length < 12) throw new IOException("short DNS packet");
            DnsMessage message = new DnsMessage();
            int questions = u16(data, 4, length);
            int count = u16(data, 6, length) + u16(data, 8, length) + u16(data, 10, length);
            int[] offset = new int[] { 12 };
            for (int i = 0; i < questions; i++) {
                readName(data, length, offset);
                require(offset[0] + 4 <= length);
                offset[0] += 4;
            }
            for (int i = 0; i < count; i++) {
                String name = readName(data, length, offset);
                require(offset[0] + 10 <= length);
                int type = u16(data, offset[0], length);
                long ttl = u32(data, offset[0] + 4, length);
                int dataLength = u16(data, offset[0] + 8, length);
                int dataOffset = offset[0] + 10;
                require(dataOffset + dataLength <= length);
                DnsRecord record = new DnsRecord(name, type, ttl);
                if (type == TYPE_PTR) {
                    int[] p = new int[] { dataOffset };
                    record.domainValue = readName(data, length, p);
                } else if (type == TYPE_SRV && dataLength >= 6) {
                    record.srvPort = u16(data, dataOffset + 4, length);
                    int[] p = new int[] { dataOffset + 6 };
                    record.srvTarget = readName(data, length, p);
                } else if (type == TYPE_TXT) {
                    int p = dataOffset, end = dataOffset + dataLength;
                    while (p < end) {
                        int size = data[p++] & 0xff;
                        if (p + size > end) break;
                        String item = new String(data, p, size, StandardCharsets.UTF_8);
                        p += size;
                        int equals = item.indexOf('=');
                        if (equals >= 0) record.txtValues.put(item.substring(0, equals), item.substring(equals + 1));
                        else if (!item.isEmpty()) record.txtValues.put(item, "");
                    }
                } else if (type == TYPE_A && dataLength == 4) {
                    byte[] address = new byte[4];
                    System.arraycopy(data, dataOffset, address, 0, 4);
                    record.address = InetAddress.getByAddress(address);
                } else if (type == TYPE_AAAA && dataLength == 16) {
                    byte[] address = new byte[16];
                    System.arraycopy(data, dataOffset, address, 0, 16);
                    record.address = InetAddress.getByAddress(address);
                }
                message.records.add(record);
                offset[0] = dataOffset + dataLength;
            }
            return message;
        }
    }

    private static final class DnsRecord {
        final String name;
        final int type;
        final long ttl;
        String domainValue;
        String srvTarget;
        int srvPort;
        InetAddress address;
        final Map<String, String> txtValues = new LinkedHashMap<String, String>();
        DnsRecord(String name, int type, long ttl) { this.name = name; this.type = type; this.ttl = ttl; }
    }

    private static String readName(byte[] data, int length, int[] offset) throws IOException {
        StringBuilder out = new StringBuilder();
        int position = offset[0];
        int resume = -1;
        int jumps = 0;
        while (true) {
            require(position >= 0 && position < length);
            int size = data[position] & 0xff;
            if (size == 0) {
                position++;
                if (resume < 0) offset[0] = position; else offset[0] = resume;
                return out.length() == 0 ? "." : out.append('.').toString();
            }
            if ((size & 0xc0) == 0xc0) {
                require(position + 1 < length);
                int pointer = ((size & 0x3f) << 8) | (data[position + 1] & 0xff);
                if (resume < 0) resume = position + 2;
                position = pointer;
                if (++jumps > 32) throw new IOException("DNS compression loop");
                continue;
            }
            if ((size & 0xc0) != 0 || size > 63) throw new IOException("invalid DNS label");
            position++;
            require(position + size <= length);
            if (out.length() > 0) out.append('.');
            out.append(new String(data, position, size, StandardCharsets.UTF_8));
            position += size;
            if (resume < 0) offset[0] = position;
        }
    }

    private static int u16(byte[] data, int offset, int length) throws IOException {
        require(offset >= 0 && offset + 2 <= length);
        return ((data[offset] & 0xff) << 8) | (data[offset + 1] & 0xff);
    }
    private static long u32(byte[] data, int offset, int length) throws IOException {
        return ((long)u16(data, offset, length) << 16) | (long)u16(data, offset + 2, length);
    }
    private static void require(boolean condition) throws IOException {
        if (!condition) throw new IOException("malformed DNS packet");
    }
}
