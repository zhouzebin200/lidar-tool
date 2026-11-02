# Windows x64 portable release

Use the existing licensed Nuitka standalone pipeline and retain machine licensing. Deliver an audited ZIP containing EXE, bundled runtime, web assets, calibration data and five x64 native decoders. Preserve existing customer artifacts.

1. Add regression coverage for missing/wrong-architecture decoders and native build failures.
2. Validate required PE x64 DLLs before compilation; stop the alternate native builder on failure. Reuse existing application code.
3. Prepare a Windows x64 build interpreter in PD, install fixed build dependencies, copy the current source snapshot without private signing material, run all tests, compile the licensed application.
4. Audit the final customer directory and ZIP; smoke test the actual packaged EXE and bundled DLLs in PD; copy the deliverable and SHA-256 back to the workspace.
5. Report real-hardware validation limits. No removal of licensing or modification of physical NIC settings.

User update: deliver an unlicensed single EXE using existing run.py, preserving the licensed entry point. Use Nuitka onefile with explicit native DLL configuration and embedded calibration .bin files.

Offline Npcap follow-up: user explicitly requests bundling an offline installer. Embed the official free Npcap 1.90 GUI installer; no silent switch or automatic EULA acceptance. Check uninstall DisplayVersion (both registry views), System32 DLL presence and driver running state before capture initialization. Do not invoke the older bundled installer over an installed same/newer version with health problems. Hash-pin the embedded installer, recheck after installer exits, explain reboot/cancel/failure without forced restart. Offline PCAP tests and --no-elevation bypass installation. Verify embedded payload hash, final EXE smoke and the supplied 111.pcap. Full GUI installation/EULA acceptance remains user-operated.
