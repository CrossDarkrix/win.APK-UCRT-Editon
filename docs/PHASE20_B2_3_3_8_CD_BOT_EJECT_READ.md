# Phase 20-B2.3.3.8 CD BOT transport, read and eject

The optical UsbDevice exposed Bulk-Only endpoints, but UsbDeviceConnection returned -1 for every optical bulk transfer. Consequently the application could enumerate and open the drive but could not deliver SCSI CDBs, read payloads, receive CSWs, or eject.

Implemented:
- USB Mass Storage Bulk-Only Transport CBW parsing on the optical OUT endpoint.
- SCSI CDB forwarding to HostUsbCdBridge.execute.
- Data-in delivery followed by a 13-byte CSW with the original tag, residue and pass/fail status.
- Support for synchronous bulkTransfer and asynchronous UsbRequest because both use the same bulkTransfer backend.
- READ TOC, INQUIRY, REQUEST SENSE and READ CD now travel through the existing Windows SPTI bridge.
- Eject START STOP UNIT first sends PREVENT ALLOW MEDIUM REMOVAL with Prevent=0, then sends LoEj=1/Start=0.
- BOT and eject diagnostics were added.

USB Direct audio, virtual DAC, URB recovery, gain and frame pacing are unchanged.
