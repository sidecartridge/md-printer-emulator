MD PRINTER EMULATOR: TEST DISK
==============================

Every program the printer's emulated tests print with, to print the same
thing on a real ST through the Multi-device. Before each print, the setup
menu's [F]irmware must have started the printer (once per boot is enough).
A debug build logs each job's size and MD5 when the job closes, 10 s after
the last byte:

    printer: job 0000000n closed, BYTES bytes, MD5 ...

Compare it with the expected line below (the emulated job, TOS 1.62).
Press each key once, briefly: Alt-Help pressed again during a dump is
ignored while the printer is on.

C:\ESCP          PRFILES.PRG: a key prints TEXT.PRN, then GRAPH.PRN (our
                 ESC/P test pages); a key at the end. Low resolution.
                 Expected: 2962 bytes,
                 MD5 5571886388a18dcfa368d775ba0d3404
                 (each file prints through BIOS, byte by byte)

C:\PCL           PRFILES.PRG again, with our PCL test streams as TEXT.PRN
                 and GRAPH.PRN: one job the RP prints as the HP LaserJet II
                 (it starts in PCL). Low resolution. Expected: 19163 bytes,
                 MD5 2ae5138a9ddf6ca8b9e647f55abf9cb1; 8 pages at 300 dpi, PDF 68103 bytes
                 on A4, the first page drawn band by band

C:\PCLTEXT       PRFILES.PRG again, with our PCL text streams as TEXT.PRN
                 and GRAPH.PRN: the LaserJet's fonts, symbol sets, underline,
                 wrap and cursor moves. Low resolution. Expected: 6234 bytes,
                 MD5 29d7abd1584f8dc4199dd44f9e35a3b1; 3 pages at 300 dpi, PDF
                 135943 bytes on A4

C:\PRPATHS       PRPATHS.PRG: a key before each of its 5 print paths, a key
                 at the end. Path 5 is a screen dump of what is on screen,
                 so the job differs from the emulated one there.

C:\DESKPRT       PRINT.TXT: click it, File > Open, then Print in the
                 alert. Low resolution.
                 Expected: 446 bytes,
                 MD5 c6ad7cfa3f7582a603f00a6ba931a367

C:\WORDPLUS      WORDPLUS.PRG, medium resolution: Cancel the Open dialog,
                 File > Print file..., DOCS\TEST_C.DOC, the print options'
                 defaults: OK. Its own "standard backspace printer" table.
                 Expected: 3086 bytes,
                 MD5 08a94f48ad7510c6887515d8454d0350

C:\WORDPLFX      The same with its Epson FX table as PRINTER.CFG.
                 Expected: 19204 bytes,
                 MD5 21aea7c60902961293ec34ec5d901ca2

C:\STWRITER      STWRITER.PRG: L, TEMPLATE.TXT, Return; P, the print
                 dialog's defaults (Printer, Draft).
                 Expected: 1324 bytes,
                 MD5 0c7d4593d46e09bd9c55c7735d8b0aa7

C:\GFA           GFABASIC\GFABASIC.PRG: type the two lines
                     lprint "gfa basic lprint test"
                     hardcopy
                 and run them (Shift F10). HARDCOPY dumps GFA's screen,
                 mouse pointer included: the job matches only with the
                 pointer where the emulated run had it (mid-screen).
                 Expected: 3485 bytes,
                 MD5 ef5d9450e7335285d610b601168f5b49

C:\DEGAS         DEGELITE.PRG, low resolution: File > Load pic,
                 PICTURES\MAGICMTN.PC1; File > Print type, PRINTERS\EPSON.PRT;
                 File > Print pic. Its original disk has copy protection on
                 tracks 80-81: if it asks for its disk, note it.
                 Expected: 64487 bytes,
                 MD5 3d4b095cbf2082059e5f117ab1f0e7a8

C:\EASYDRAW      EASYDRAW.PRG, medium resolution, with GDOS (C:\AUTO,
                 C:\ASSIGN.SYS): File > OUTPRINT..., don't save; Edit > Add
                 Name, DRAWINGS\HOUSE.GEM; File > Start Outprint; Device:
                 Printer, OK.
                 Expected: 94225 bytes,
                 MD5 5d860e06c1c3af1537214b6503dc30e3

C:\XCONTROL.ACC  Desk > Control Panel, Printer Setup, Quality: Final, OK,
                 close; then Alt-Help. The dump is of the screen, so its
                 MD5 is not the emulated one; Final doubles a Draft dump.

Alt-Help         The desktop's screen dump: compare Draft (default) against
                 Final, nothing else to compare.

C:\CALAMUS       CALAMUS.PRG, high resolution (a monochrome monitor), 4 MB:
                 Datei > Neu anlegen, Abbruch (no fonts), Datei > Drucken
                 (einstellen), DRUCKEN. Calamus 1.x drives the printer port
                 itself: no job is expected (not caught).

C:\SIGNUM3       SIGNUM3.PRG, high resolution, 4 MB: Datei > Dokument laden,
                 DEMO\HAUS.SDK; Datei > ins Druckprogramm, Lade!, TREIBER\
                 LQ850.TRB, OK, Drucke, Starte Ausdruck!. Its default output
                 channel "Centronics schnell" drives the port itself: no job.
                 With channel "Centronics, BIOS" it prints through TOS, with
                 a 24-pin driver (Signum 3 has no 9-pin one).
                 Expected (BIOS): 63654 bytes,
                 MD5 e1346f15dcd7d14c4ae6bff2be38ec98
