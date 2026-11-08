# ECDH keypair failure fallback

- Default connection keeps the host JSSE defaults.
- Only when the exception chain explicitly contains an ECDH keypair-generation failure, the socket is recreated once.
- Retry is restricted to TLS 1.2 and supported non-ECDH/non-ECDHE cipher suites.
- Anonymous, NULL, RC4, 3DES and DES suites are excluded.
- Certificate-chain and HTTPS hostname verification stay enabled.
- Punch default remains 3.
