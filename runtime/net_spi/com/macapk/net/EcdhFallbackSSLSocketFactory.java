package com.macapk.net;

import java.io.IOException;
import java.net.InetAddress;
import java.net.Socket;
import java.net.UnknownHostException;
import java.security.KeyPairGenerator;
import java.security.Provider;
import java.security.Security;
import java.util.ArrayList;
import java.util.List;
import javax.crypto.KeyAgreement;
import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/** HTTPS factory pinned to the host JDK JSSE and host EC/XDH providers. */
public final class EcdhFallbackSSLSocketFactory extends SSLSocketFactory {
    private final SSLSocketFactory delegate;
    private final int timeoutMillis;

    public EcdhFallbackSSLSocketFactory(int timeoutMillis) {
        this.timeoutMillis = Math.max(0, timeoutMillis);
        this.delegate = createHostFactory();
        diagnoseHostEc();
    }

    private static SSLSocketFactory createHostFactory() {
        try {
            Provider sunJsse = Security.getProvider("SunJSSE");
            if (sunJsse == null) throw new IllegalStateException("Host JDK SunJSSE provider is unavailable");
            SSLContext context = SSLContext.getInstance("TLS", sunJsse);
            context.init(null, null, null);
            System.err.println("[I] Win.apk.TLS: host JSSE provider=" + context.getProvider().getName());
            return context.getSocketFactory();
        } catch (Exception e) {
            throw new IllegalStateException("Unable to initialize host JDK SunJSSE", e);
        }
    }

    private static void diagnoseHostEc() {
        probeKeyPair("X25519");
        probeKeyPair("XDH");
        probeKeyPair("EC");
        probeAgreement("X25519");
        probeAgreement("XDH");
        probeAgreement("ECDH");
    }
    private static void probeKeyPair(String algorithm) {
        try {
            KeyPairGenerator kpg = KeyPairGenerator.getInstance(algorithm);
            String provider = kpg.getProvider().getName();
            if ("Conscrypt".equals(provider) || "GmsCore_OpenSSL".equals(provider))
                throw new IllegalStateException("Synthetic provider selected for " + algorithm);
            System.err.println("[I] Win.apk.TLS: host KeyPairGenerator." + algorithm + " provider=" + provider);
        } catch (Exception e) {
            System.err.println("[W] Win.apk.TLS: host KeyPairGenerator." + algorithm + " unavailable: " + e);
        }
    }
    private static void probeAgreement(String algorithm) {
        try {
            KeyAgreement ka = KeyAgreement.getInstance(algorithm);
            String provider = ka.getProvider().getName();
            if ("Conscrypt".equals(provider) || "GmsCore_OpenSSL".equals(provider))
                throw new IllegalStateException("Synthetic provider selected for " + algorithm);
            System.err.println("[I] Win.apk.TLS: host KeyAgreement." + algorithm + " provider=" + provider);
        } catch (Exception e) {
            System.err.println("[W] Win.apk.TLS: host KeyAgreement." + algorithm + " unavailable: " + e);
        }
    }

    private Socket configure(Socket socket, String host) throws IOException {
        if (!(socket instanceof SSLSocket)) return socket;
        SSLSocket ssl=(SSLSocket)socket;
        if (timeoutMillis>0) ssl.setSoTimeout(timeoutMillis);
        SSLParameters p=ssl.getSSLParameters();
        if(host!=null && !host.isEmpty()) {
            try { p.setServerNames(java.util.Collections.singletonList(new SNIHostName(host))); }
            catch(IllegalArgumentException ignored) { }
            p.setEndpointIdentificationAlgorithm("HTTPS");
        }
        ssl.setSSLParameters(p);
        return ssl;
    }
    @Override public String[] getDefaultCipherSuites(){return delegate.getDefaultCipherSuites();}
    @Override public String[] getSupportedCipherSuites(){return delegate.getSupportedCipherSuites();}
    @Override public Socket createSocket(Socket s,String h,int p,boolean c)throws IOException{return configure(delegate.createSocket(s,h,p,c),h);}
    @Override public Socket createSocket(String h,int p)throws IOException,UnknownHostException{return configure(delegate.createSocket(h,p),h);}
    @Override public Socket createSocket(String h,int p,InetAddress l,int lp)throws IOException,UnknownHostException{return configure(delegate.createSocket(h,p,l,lp),h);}
    @Override public Socket createSocket(InetAddress h,int p)throws IOException{return configure(delegate.createSocket(h,p),h.getHostName());}
    @Override public Socket createSocket(InetAddress h,int p,InetAddress l,int lp)throws IOException{return configure(delegate.createSocket(h,p,l,lp),h.getHostName());}
}
