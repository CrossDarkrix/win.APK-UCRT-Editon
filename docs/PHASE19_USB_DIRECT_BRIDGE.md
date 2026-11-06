# Phase 19 A-H integrated USB Direct Bridge

Implements Windows SetupAPI/WinUSB enumeration, descriptors, permission persistence, open/close, interface claim/release and alternate setting, control, bulk and interrupt transfers, asynchronous UsbRequest queueing, timeout/cancel, and disconnect failure handling. Optical-drive BOT compatibility is retained. Isochronous audio remains outside this phase. WinUSB access requires the target function/interface to be bound to winusb.sys.
