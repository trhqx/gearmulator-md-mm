# MD/MM: OS aus einer Upgrade-.syx installieren

Status: umgesetzt (2026-10-27). Getestet mit MD OS 1.63 und MM OS 1.32b.

## Ziel

Ein Elektron-OS-Upgrade (`.syx`) soll sich wie am Gerät installieren lassen,
ohne dass für jedes OS ein eigenes 8-MiB-`.bin` beschafft werden muss.

Das stock `.bin` bleibt als Basis nötig. Ein OS-Upgrade enthält nur das
Betriebssystem, nicht den Bootloader. Ein Basis-`.bin` reicht aber für alle
weiteren OS-Versionen.

## Ablauf (`md::OsInstaller`, mdLib)

Der Installer macht, was man am Gerät von Hand macht, in einer unsichtbaren
Emulator-Instanz:

1. Basis-Abbild laden, den OS-Bereich `0x004000–0x0FFFFF` mit Nullen füllen.
   So kann nichts vom alten OS im Ergebnis übrig bleiben.
2. Mit gehaltener FUNCTION-Taste booten. Die Firmware liest den Tastenzustand
   einmal aus der Startantwort des Panels (MD: Zeile `0x24`, Maske `0x02`;
   MM: Deskriptorwert `0x02`). Tastendrücke während des Boots kommen zu spät.
   `Hardware::setBootHoldFunction()` muss vor dem ersten `advance()` gesetzt
   sein.
3. Im EARLY STARTUP MENU „5.. MIDI UPGRADE“ wählen.
4. Die Nachrichten der `.syx` unverändert über den emulierten MIDI-Eingang
   schicken, jede mindestens in ihrer Leitungszeit (31250 Baud) plus 5 ms
   Pause. Alles auf einmal eingereiht bricht der Bootloader mit
   „UPGRADE FAILED“ ab. Ohne Pause (nur Leitungszeit) ging es auf der MM auch;
   die 5 ms sind Reserve.
5. Der Bootloader puffert alles und programmiert den Flash nach der
   End-Nachricht (`7F`). Er löscht dabei jeden Sektor in
   `[0x004000, 0x100000)` und nichts darunter.
6. Sobald der Flash ruhig ist, wird der OS-Bereich übernommen. Der Rest des
   Abbilds bleibt wie im Basis-Abbild.
7. Prüf-Boot: Ein frischer Emulator bootet das Ergebnis. Nur wenn das OS dort
   bereit wird (`isFirmwareMidiReady`), gilt die Installation als gelungen.

Das Upgrade-Format wird nicht dekodiert. Der Installer prüft nur die Rahmen
(`F0 00 20 3C <Produkt> 00 7E/7F … F7`, Produkt MD = 2, MM = 3).

### Gefundene Emulationslücken

- **MM-Flash:** Das Modell kannte nur 64-KiB-Sektoren. Der Baustein ist wie
  der der MD ein Bottom-Boot-Typ mit acht 8-KiB-Blöcken unter 64 KiB. Das
  Upgrade löscht ab `0x4000` und blieb bei „PREPARING FLASH“ hängen.
  Behoben in `monomachineFlashSectorSize()`.
- **DSP-Reset (offen, MD):** Nach dem Programmieren startet der Bootloader
  die Maschine neu. Der emulierte DSP kann dem nicht folgen: Er springt auf
  `$FF0000` (Boot-ROM, nicht emuliert) und hält an. Bisher hing dann
  `Hardware::advance()` für immer, weil der Scheduler den angehaltenen DSP als
  Nachzügler endlos weiterschalten wollte. Jetzt parkt der Scheduler einen
  DSP, dessen PC außerhalb des Programmspeichers liegt, wie einen noch nicht
  gebooteten. Die Maschine bleibt dann stumm, aber die Emulation läuft weiter.
  Der Installer hört auf, sobald der Flash ruhig ist, und bootet das Ergebnis
  frisch. Ein Update per „Send SysEx File“ im laufenden Plugin bliebe
  dagegen nach dem Neustart stumm; das bleibt gesperrt.

## Ablage und Auswahl (Plugin)

- Installierte Abbilder liegen als normale 8-MiB-`.bin` unter
  `<Datenordner>/os/<Name der .syx>.bin`, z. B.
  `~/Documents/Gearmulator Preview/Machinedrum/os/`. Sie lassen sich kopieren
  und sichern wie das Basis-`.bin`.
- Settings → Operating System: „Install OS from .syx…“ (mit Fortschritt und
  Abbrechen), „Use Installed OS…“, „Use Stock OS“. Nach einer erfolgreichen
  Installation fragt das Plugin, ob es gleich damit neu starten soll.
- Die Auswahl steht in der Plugin-Konfiguration (`osImage`). Beim Wechsel
  startet die Maschine neu und behält den Patch-RAM (Kits, Patterns, Songs),
  wie das batteriegepufferte RAM am Gerät.
- ROM-Prüfung: `RomLoader::findROM` akzeptiert weiterhin nur die stock
  Abbilder, damit ein installiertes Abbild nie das Basis-Abbild ersetzt.
  Explizit gewählte Abbilder prüft `isImageForModel` über den
  Bootloader-Bereich `[0, 0x4000)`. Ein gewähltes, aber unbrauchbares Abbild
  führt sichtbar (Settings: „Running: …“) auf das stock OS zurück, nicht still.
- Der MD-Factory-Flash-Cache bekommt pro Firmware eine eigene Datei, damit
  ein OS-Wechsel den Cache des stock OS nicht überschreibt.

## Nicht getestet / Einschränkungen

Beide vorhandenen `.syx` erzeugen exakt die stock Abbilder. Was sich mit einem
wirklich anderen OS ändert, ließ sich deshalb nicht prüfen:

- Teile der Emulation sind auf OS 1.63 / 1.32b abgestimmt (z. B. die
  RAM-Recording-Korrektur, Panel-Handshake, TurboMIDI-Zeiten).
- Ein neues OS kann beim ersten Start den Kit-Speicher neu formatieren.
- Projekt-Zustände sind an den Fingerabdruck der Firmware gebunden. Ein mit
  einem anderen OS gespeichertes Projekt lädt nicht, solange dieses OS läuft.
- Beim OS-Wechsel wird nur der Patch-RAM übernommen, nicht der übrige Flash
  (MD-UW-Samples, MM-DigiPRO-Wellen).

## Tests

- `mdFlashTest`: MM-Boot-Blöcke, 64-KiB-Sektoren, falsch ausgerichtete
  Löschbefehle.
- `mdOsUpgradeFileTest`: Rahmenprüfung mit synthetischen Nachrichten,
  Abweisung fremder Abbilder, kein stiller Ersatz.
- `mdOsSelectionFirmwareTest` (FirmwareTest): Wechsel über den Prozessor mit
  einer Kopie des stock Abbilds, Patch-RAM bleibt erhalten, fremdes Abbild
  wird abgewiesen, ohne die Maschine zu stören, eine verschwundene Datei wird
  gemeldet, Rückkehr zum stock OS.
- `mdOsInstallFirmwareTest` (FirmwareTest, dauert Minuten): braucht
  `GEARMULATOR_MD_FIRMWARE_BIN` + `GEARMULATOR_MD_OS_SYX` und/oder
  `GEARMULATOR_MM_FIRMWARE_BIN` + `GEARMULATOR_MM_OS_SYX`. Prüft, dass sich
  außerhalb des OS-Bereichs nichts ändert und dass das Ergebnis bootet. Mit
  dem passenden `.syx` muss das stock Abbild Byte für Byte herauskommen; weil
  der OS-Bereich vorher genullt wurde, belegt das, dass der Bootloader ihn
  vollständig geschrieben hat. Die Dateien sind nicht im Repository.
