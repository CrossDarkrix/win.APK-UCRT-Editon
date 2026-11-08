# Bundled runtime crypto guard

1. The generated runtime is rejected before publication unless SunJSSE, SunEC, TLS, X25519/XDH/EC key generation, X25519/XDH/ECDH agreement and an HTTPS request all pass.
2. jdeps output is augmented with Java networking/security modules required by service-loaded TLS providers.
3. jlink always uses --bind-services.
4. Runtime publication remains atomic and occurs only after all checks pass.
5. Punch 3, CPU priority +2 and host-JDK EC routing are preserved.
