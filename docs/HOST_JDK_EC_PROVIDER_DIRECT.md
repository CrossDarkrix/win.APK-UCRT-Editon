# Host JDK EC provider direct

- HTTPS SSLContext is instantiated explicitly from the host `SunJSSE` provider.
- EC/XDH KeyPairGenerator, KeyAgreement, KeyFactory and AlgorithmParameters services are not cloned into synthetic Conscrypt aliases.
- X25519, XDH, EC and ECDH provider selection is logged.
- TLS protocol and cipher-suite defaults are left to host SunJSSE, allowing TLS 1.3 and TLS 1.2 ECDHE.
- The rejected TLS 1.2 non-ECDH retry has been removed from the active HTTPS path.
- Certificate validation, SNI and HTTPS endpoint identification remain enabled.
- Punch 3 and CPU priority +2 are preserved.
