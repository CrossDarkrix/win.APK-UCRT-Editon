// android.net.SSLCertificateSocketFactory -- host-JSSE Android compatibility layer.
package android.net;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.Socket;
import java.net.SocketAddress;
import java.net.UnknownHostException;
import java.security.Principal;
import java.security.cert.Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import javax.net.SocketFactory;
import javax.net.ssl.HandshakeCompletedListener;
import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.KeyManager;
import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSessionContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.security.cert.X509Certificate;

@Deprecated
public class SSLCertificateSocketFactory extends SSLSocketFactory {
    private volatile SSLSocketFactory mDelegate;
    private volatile KeyManager[] mKeyManagers;
    private volatile TrustManager[] mTrustManagers;
    private volatile String[] mApplicationProtocols;
    private final int mHandshakeTimeoutMillis;

    @Deprecated
    public SSLCertificateSocketFactory(int handshakeTimeoutMillis) {
        mHandshakeTimeoutMillis = Math.max(0, handshakeTimeoutMillis);
        rebuildDelegate();
    }
    public static SocketFactory getDefault(int timeout) { return new SSLCertificateSocketFactory(timeout); }
    public static SSLSocketFactory getDefault(int timeout, SSLSessionCache cache) { return new SSLCertificateSocketFactory(timeout); }
    public static SSLSocketFactory getInsecure(int timeout, SSLSessionCache cache) {
        // Deliberately still verifies certificates. This method does not weaken trust policy.
        return new SSLCertificateSocketFactory(timeout);
    }

    private synchronized void rebuildDelegate() {
        try {
            SSLContext context = SSLContext.getInstance("TLS");
            context.init(mKeyManagers, mTrustManagers, null);
            mDelegate = context.getSocketFactory();
        } catch (Exception e) { throw new IllegalStateException("Unable to initialize TLS context", e); }
    }
    private static String[] decodeProtocols(byte[][] protocols) {
        if (protocols == null) return null;
        List<String> out = new ArrayList<String>();
        for (byte[] p : protocols) if (p != null && p.length != 0) try { out.add(new String(p, "UTF-8")); } catch (Exception ignored) { }
        return out.isEmpty() ? null : out.toArray(new String[out.size()]);
    }
    private SSLSocket configure(SSLSocket ssl, String host) throws IOException {
        if (mHandshakeTimeoutMillis > 0) ssl.setSoTimeout(mHandshakeTimeoutMillis);
        SSLParameters p = ssl.getSSLParameters();
        if (host != null && !host.isEmpty()) {
            try { p.setServerNames(java.util.Collections.singletonList(new SNIHostName(host))); } catch (IllegalArgumentException ignored) { }
            p.setEndpointIdentificationAlgorithm("HTTPS");
        }
        String[] ap = mApplicationProtocols;
        if (ap != null) try { p.setApplicationProtocols(ap); } catch (UnsupportedOperationException ignored) { }
        ssl.setSSLParameters(p);
        return ssl;
    }
    private SSLSocket fallback(String host, int port) throws IOException {
        SSLSocket ssl = configure((SSLSocket)mDelegate.createSocket(host, port), host);
        List<String> protocols = Arrays.asList(ssl.getSupportedProtocols());
        if (!protocols.contains("TLSv1.2")) throw new SSLException("TLSv1.2 is not supported by host JSSE");
        ssl.setEnabledProtocols(new String[]{"TLSv1.2"});
        List<String> suites = new ArrayList<String>();
        for (String suite : ssl.getSupportedCipherSuites()) {
            String u = suite.toUpperCase(java.util.Locale.ROOT);
            if (!u.contains("_ECDH_") && !u.contains("_ECDHE_") && !u.contains("_ANON_") && !u.contains("_NULL_") && !u.contains("RC4") && !u.contains("3DES") && !u.contains("DES_")) suites.add(suite);
        }
        if (suites.isEmpty()) throw new SSLException("No TLS 1.2 non-ECDH cipher suite is supported");
        ssl.setEnabledCipherSuites(suites.toArray(new String[suites.size()]));
        return ssl;
    }
    private static boolean ecdhFailure(Throwable error) {
        for (Throwable t=error; t!=null; t=t.getCause()) {
            String m=t.getMessage();
            if (m != null) { String u=m.toUpperCase(java.util.Locale.ROOT); if (u.contains("ECDH") && (u.contains("KEYPAIR") || u.contains("KEY PAIR"))) return true; }
        }
        return false;
    }

    public void setHostname(Socket socket, String hostname) { if (socket instanceof SSLSocket) try { configure((SSLSocket)socket, hostname); } catch(IOException e){ throw new IllegalArgumentException(e); } }
    public void setUseSessionTickets(Socket socket, boolean enabled) { }
    public void setNpnProtocols(byte[][] protocols) { mApplicationProtocols=decodeProtocols(protocols); }
    public void setKeyManagers(KeyManager[] managers) { mKeyManagers=managers==null?null:managers.clone(); rebuildDelegate(); }
    public void setTrustManagers(TrustManager[] managers) { mTrustManagers=managers==null?null:managers.clone(); rebuildDelegate(); }
    public byte[] getNpnSelectedProtocol(Socket socket) { return getAlpnSelectedProtocol(socket); }
    public byte[] getAlpnSelectedProtocol(Socket socket) { if(!(socket instanceof SSLSocket))return null; try { String p=((SSLSocket)socket).getApplicationProtocol(); return p==null||p.isEmpty()?null:p.getBytes("UTF-8"); } catch(Exception e){return null;} }
    public void setHostnameVerifier(HostnameVerifier verifier) { }
    @Override public String[] getDefaultCipherSuites(){return mDelegate.getDefaultCipherSuites();}
    @Override public String[] getSupportedCipherSuites(){return mDelegate.getSupportedCipherSuites();}

    private Socket wrap(SSLSocket socket,String host,int port) throws IOException { return new RetrySocket(configure(socket,host),host,port,this); }
    @Override public Socket createSocket(Socket s,String host,int port,boolean close)throws IOException{return wrap((SSLSocket)mDelegate.createSocket(s,host,port,close),host,port);}
    @Override public Socket createSocket(String host,int port)throws IOException,UnknownHostException{return wrap((SSLSocket)mDelegate.createSocket(host,port),host,port);}
    @Override public Socket createSocket(String host,int port,InetAddress local,int localPort)throws IOException,UnknownHostException{return wrap((SSLSocket)mDelegate.createSocket(host,port,local,localPort),host,port);}
    @Override public Socket createSocket(InetAddress host,int port)throws IOException{return wrap((SSLSocket)mDelegate.createSocket(host,port),host.getHostName(),port);}
    @Override public Socket createSocket(InetAddress host,int port,InetAddress local,int localPort)throws IOException{return wrap((SSLSocket)mDelegate.createSocket(host,port,local,localPort),host.getHostName(),port);}

    private static final class RetrySocket extends SSLSocket {
        private SSLSocket d; private final String host; private final int port; private final SSLCertificateSocketFactory owner; private boolean retried;
        RetrySocket(SSLSocket d,String host,int port,SSLCertificateSocketFactory owner){this.d=d;this.host=host;this.port=port;this.owner=owner;}
        private synchronized void ensureHandshake() throws IOException {
            try { d.startHandshake(); }
            catch (IOException first) {
                if (retried || !ecdhFailure(first)) throw first;
                retried=true; try{d.close();}catch(IOException ignored){}
                d=owner.fallback(host,port);
                System.err.println("[W] Win.apk.TLS: ECDH keypair generation failed; retrying with TLSv1.2 non-ECDH cipher suites host="+host);
                d.startHandshake();
            }
        }
        @Override public void startHandshake()throws IOException{ensureHandshake();}
        @Override public InputStream getInputStream()throws IOException{ensureHandshake();return d.getInputStream();}
        @Override public OutputStream getOutputStream()throws IOException{ensureHandshake();return d.getOutputStream();}
        @Override public String[] getSupportedCipherSuites(){return d.getSupportedCipherSuites();}
        @Override public String[] getEnabledCipherSuites(){return d.getEnabledCipherSuites();}
        @Override public void setEnabledCipherSuites(String[] s){d.setEnabledCipherSuites(s);}
        @Override public String[] getSupportedProtocols(){return d.getSupportedProtocols();}
        @Override public String[] getEnabledProtocols(){return d.getEnabledProtocols();}
        @Override public void setEnabledProtocols(String[] p){d.setEnabledProtocols(p);}
        @Override public SSLSession getSession(){return d.getSession();}
        @Override public void addHandshakeCompletedListener(HandshakeCompletedListener l){d.addHandshakeCompletedListener(l);}
        @Override public void removeHandshakeCompletedListener(HandshakeCompletedListener l){d.removeHandshakeCompletedListener(l);}
        @Override public void setUseClientMode(boolean m){d.setUseClientMode(m);}
        @Override public boolean getUseClientMode(){return d.getUseClientMode();}
        @Override public void setNeedClientAuth(boolean n){d.setNeedClientAuth(n);}
        @Override public boolean getNeedClientAuth(){return d.getNeedClientAuth();}
        @Override public void setWantClientAuth(boolean w){d.setWantClientAuth(w);}
        @Override public boolean getWantClientAuth(){return d.getWantClientAuth();}
        @Override public void setEnableSessionCreation(boolean f){d.setEnableSessionCreation(f);}
        @Override public boolean getEnableSessionCreation(){return d.getEnableSessionCreation();}
        @Override public SSLParameters getSSLParameters(){return d.getSSLParameters();}
        @Override public void setSSLParameters(SSLParameters p){d.setSSLParameters(p);}
        @Override public String getApplicationProtocol(){return d.getApplicationProtocol();}
        @Override public String getHandshakeApplicationProtocol(){return d.getHandshakeApplicationProtocol();}
        @Override public synchronized void close()throws IOException{d.close();}
        @Override public boolean isClosed(){return d.isClosed();}
        @Override public boolean isConnected(){return d.isConnected();}
        @Override public InetAddress getInetAddress(){return d.getInetAddress();}
        @Override public InetAddress getLocalAddress(){return d.getLocalAddress();}
        @Override public int getPort(){return d.getPort();}
        @Override public int getLocalPort(){return d.getLocalPort();}
        @Override public SocketAddress getRemoteSocketAddress(){return d.getRemoteSocketAddress();}
        @Override public SocketAddress getLocalSocketAddress(){return d.getLocalSocketAddress();}
        @Override public void setSoTimeout(int timeout)throws java.net.SocketException{d.setSoTimeout(timeout);}
        @Override public int getSoTimeout()throws java.net.SocketException{return d.getSoTimeout();}
        @Override public void setTcpNoDelay(boolean on)throws java.net.SocketException{d.setTcpNoDelay(on);}
        @Override public boolean getTcpNoDelay()throws java.net.SocketException{return d.getTcpNoDelay();}
        @Override public void setKeepAlive(boolean on)throws java.net.SocketException{d.setKeepAlive(on);}
        @Override public boolean getKeepAlive()throws java.net.SocketException{return d.getKeepAlive();}
        @Override public void shutdownInput()throws IOException{d.shutdownInput();}
        @Override public void shutdownOutput()throws IOException{d.shutdownOutput();}
        @Override public boolean isInputShutdown(){return d.isInputShutdown();}
        @Override public boolean isOutputShutdown(){return d.isOutputShutdown();}
    }
}
