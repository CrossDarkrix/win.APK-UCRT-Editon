# Audio Punch + SSL compatibility fix

## Audio
- USB Direct に、低域の厚みを補う軽量な低域シェルフ相当処理を追加。
- `HKCU\Software\Win.apk\UsbDirectPunchLevel` で 0..3 を選択可能。既定値は 3。
- 既存の 0..18 dB USB Direct gain ramp とソフトリミッターは維持。
- Punch 有効時は Gain 0 dB でもリミッターを通し、追加低域によるクリップを防止。
- サンプルレートに追従する係数と、左右独立の状態を使用。

## SSL/TLS
- `SSLCertificateSocketFactory.setTrustManagers()` と `setKeyManagers()` を実働化し、SSLContext を再構築。
- SNI、HTTPS hostname verification、ALPN/NPN互換設定、handshake timeout を JSSE socket に反映。
- `getAlpnSelectedProtocol()` / `getNpnSelectedProtocol()` が選択済みプロトコルを返すよう修正。
- 証明書検証を無効化する回避策は導入していない。
