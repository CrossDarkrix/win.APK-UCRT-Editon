# HTTPS real-path ECDH fallback

- Installs `android.net.SSLCertificateSocketFactory` on every real JDK `HttpsURLConnection` created by `AndroidHttpHandlerProvider`.
- Guest `HttpURLConnection` requests now use the existing one-shot ECDH-keypair failure retry.
- The first attempt retains normal JSSE configuration.
- Only an explicit ECDH keypair-generation failure retries with TLS 1.2 and non-ECDH/non-ECDHE cipher suites.
- Redirect-created HTTPS connections preserve the socket factory through `HucFollow`.
- Certificate-chain, SNI and HTTPS hostname verification remain enabled.
- Punch default 3 and CPU priority +2 remain unchanged.
