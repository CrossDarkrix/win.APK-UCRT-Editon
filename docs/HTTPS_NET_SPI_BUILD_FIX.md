# HTTPS net_spi build dependency fix

- Removes the illegal compile-time dependency from runtime/net_spi to android.net.
- Adds a self-contained com.macapk.net.EcdhFallbackSSLSocketFactory compiled in the same net_spi javac pass.
- AndroidHttpHandlerProvider now instantiates the local net_spi factory.
- The normal TLS first attempt, ECDH-keypair detection, TLS 1.2 non-ECDH retry, certificate validation, SNI and hostname verification are preserved.
- Punch default 3 and CPU priority +2 are preserved.
