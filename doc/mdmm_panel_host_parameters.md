# MD/MM: Front-Panel als Host-Parameter

Status: Konzept, noch nicht umgesetzt.

## Ziel

Jedes Bedienelement des emulierten Front-Panels (Tasten, Data-Entry-Encoder,
Encoder-Push, LEVEL, SOUND SELECTION) soll als **Host-Parameter** erscheinen,
damit ein DAW-Nutzer es mit Bordmitteln der DAW an MIDI-Controller binden kann.
In Ableton Live heißt das:

1. Plugin-Device → **Configure** aktivieren.
2. Im Plugin-Fenster auf eine Taste klicken oder an einem Encoder drehen.
   Live nimmt den berührten Parameter in die Device-Ansicht auf.
3. **MIDI Map** (Cmd+M) → Parameter anklicken → Controller bewegen.

Das funktioniert nur, wenn die GUI-Interaktion über einen Host-Parameter läuft,
der seine Änderung per `beginChangeGesture` / `setValueNotifyingHost` /
`endChangeGesture` meldet. Live lernt in Configure ausschließlich Parameter,
die der Host sich ändern sieht.

## Entscheidungen (2026-10-26)

- **Umfang:** alle Controls, also alle verifizierten `PanelControl`-Tasten,
  alle `PanelEncoder` und Encoder-Push A–H.
- **Ziel-Hosts:** Ableton Live 12, **VST3 und AU**. Beide Formate werden
  getrennt verifiziert, weil die Wrapper Schrittweiten und vom Plugin
  ausgelöste Änderungen unterschiedlich weitergeben können.
- **Controller:** Endlos-Encoder in einem relativen Modus von Live.
- **Standardannahmen** (bis auf Widerruf):
  - Panel-Parameter sind automatisierbar, weil Live nicht automatisierbare
    Parameter in MIDI Map nicht anbietet.
  - Mausklicks laufen über die Parameter.
  - Eigener Branch auf `release/md-mm-alpha`. Ideen aus PR #93 werden übernommen,
    Code nicht.
  - Plattformunabhängig, also auch Windows.

## Offene Host-Hypothesen (vor der Umsetzung zu prüfen)

| # | Hypothese | Prüfung |
|---|---|---|
| H1 | Eine auf eine Note gemappte Bool-Taste sendet bei Note-On 1 und bei Note-Off 0 (momentan), schaltet also nicht um. | Probe-Plugin, Parameter „Probe Button" |
| H2 | Ein relativer Encoder-Tick bewegt einen Parameter mit 128 Stufen um genau eine Stufe. | „Probe Encoder 128" / „16k" / „Continuous" |
| H3 | Live übernimmt ein vom Plugin ausgelöstes Re-Zentrieren als neue Basis für den nächsten relativen Tick, ohne Automation zu schreiben. | „Probe Recentre Mode" in beiden Notify-Varianten |

| H4 | Wird eine aufgenommene Encoder-Automation abgespielt, darf der Rücksprung zur Mitte keine Gegen-Ticks auslösen. | Automation mit Probe aufnehmen und abspielen, Log prüfen |

### Ergebnisse Live 12 AU (2026-10-26)

- **H1 bestätigt (AU):** Note-On ergibt 1, Note-Off ergibt 0. Die Haltedauer
  war im Log sichtbar, zwischen 0,3 s und 8 s. Die Taste verhält sich also
  momentan, P-Locks sind möglich.
- **H2:** Live bewegt pro relativem Tick um **1/127 des normalisierten
  Bereichs**, unabhängig von der Stufenzahl des Parameters (16k-Parameter: 129
  Stufen pro Tick). Bei schnellem Drehen kommen Sprünge von 2–5 Ticks pro
  Nachricht. → **Parameter mit 128 Stufen (0–127) verwenden**: ein Tick
  entspricht dann genau einer Stufe. Größere Bereiche bringen keine
  Auflösung, nur Rundungsfehler.
- **H3 überwiegend bestätigt:** Zurückgesetzt wurde frühestens nach 301 ms
  Ruhe. In 42 von 54 Fällen hat Live ab der Mitte weitergezählt, mit und ohne
  Gesture gleichermaßen. **Problem am Anschlag:** Stand Lives Wert bei 0 oder
  127, wurde das Zurücksetzen in 10 von 23 Fällen ignoriert, und Live zählte
  vom alten Randwert weiter. Meist kam der nächste Tick 7–60 ms nach dem
  Zurücksetzen, es sieht also nach einem Wettlauf aus. Gegenmaßnahme ist noch
  zu prüfen: früher zurücksetzen, bevor der Rand erreicht ist.
- **Automation:** Die Aufnahme enthält die Rücksprünge zur Mitte (Beobachtung
  des Nutzers). Siehe H4. Ohne Gegenmaßnahme würde jeder Rücksprung beim
  Abspielen als Gegendrehung an die Firmware gehen.
- **VST3:** noch nicht getestet.

**Entscheidung zu H4 (Nutzer, 2026-10-26): Variante (a)** – in Live überholt, siehe „Ergebnisse Probe v2". Encoder-Parameter
sind nur per MIDI steuerbar und werden nicht automatisiert
(`isAutomatable() == false`). Tasten bleiben automatisierbar. Offen ist, ob
Live nicht automatisierbare Parameter in Configure und MIDI Map anbietet und
ob das Zurücksetzen dort genauso übernommen wird (H5). Die Probe v2 misst das,
außerdem den Zeitpunkt des Zurücksetzens (0–1000 ms Ruhe, immer oder nur
außerhalb des mittleren Drittels) für das Problem am Anschlag.

### Ergebnisse Probe v2, Live 12 AU + VST3 (2026-10-26)

- **H5 noch offen:** Für „Probe Encoder 128 No Automation" steht in beiden
  Formaten kein Ereignis aus Live im Log. Alle Drehungen des Nutzers kamen
  beim automatisierbaren „Probe Encoder 128" an. Dass das Logging den
  Parameter erfasst, ist per `auval` belegt (13 Ereignisse). Vermutlich waren
  die beiden Parameter in Live wegen gekürzter Namen nicht zu unterscheiden.
  Der Parameter heißt jetzt „NOAUTO Encoder", gleiche ID; der Test wird
  wiederholt.
  **Ursache gefunden:** Die Probe verwendete JUCEs
  `GenericAudioProcessorEditor`, und der blendet nicht automatisierbare
  Parameter aus. Seit die Probe einen eigenen Editor hat, gilt:
- **H5 (AU) widerlegt im Sinne von Variante (a):** „NOAUTO Encoder" erscheint
  in Configure, ist per MIDI Map steuerbar, und **Live zeichnet trotzdem
  Automation auf** (Beobachtung des Nutzers). Das AU-Flag `NonRealTime`, über
  das JUCE „nicht automatisierbar" meldet, hält Live also nicht davon ab.
  Variante (a) schützt in Live daher nicht vor abgespielten Rücksprüngen.
  Am Anschlag wurde das Zurücksetzen nach 300 ms (*Always*) 3-mal übernommen
  und 3-mal ignoriert.

**Folgerung:** Encoder-Parameter bleiben automatisierbar. Abgespielte
Rücksprünge werden im Plugin erkannt und verworfen (Re-Basis-Regel, siehe
unten). Zurückgesetzt wird nach 300 ms Pause und nur außerhalb des mittleren
Drittels, damit der Wert den Rand möglichst nicht erreicht.
- **Zeitpunkt des Zurücksetzens:**
  - Nach 0 ms Pause wurde es **in 51 von 51 Fällen ignoriert**. Live
    überschreibt den Wert, solange gedreht wird. Sofortiges Zurücksetzen
    scheidet damit aus.
  - Nach 300 ms Pause wurde es übernommen: AU in der Mitte 15/15 und am
    Anschlag 7/8, VST3 4/4.
  - Mit „nur außerhalb des mittleren Drittels" kam es nur einmal zum Anschlag
    (statt 9-mal bei *Always*), bei kleiner Datenbasis.
- **VST3 liefert jede Änderung doppelt:** einmal auf dem Message-Thread und
  einmal auf dem Audio-Thread, mit identischem Wert. Rückwärtssprünge durch
  Verschränkung traten nicht auf. → Der Mapper muss Änderungen mit Delta 0
  ignorieren und darf den Wert pro Parameter nur an **einer** Stelle
  auswerten.
- **Taste unter VST3:** Statt sauberer 0/1-Wechsel kamen kontinuierliche
  Werte (0,05 … 1,0 im 16-ms-Takt), also eine Bedienung mit Maus oder Regler
  statt per Note. Momentan-Verhalten mit Note-Mapping ist unter VST3 damit
  noch nicht nachgewiesen. Der Mapper wertet Bool-Werte über die Schwelle 0,5 aus.

Die Ergebnisse werden hier eingetragen, bevor der Parameterbereich und das
Tastenmodell festgelegt werden. Fällt H1 durch, sind P-Locks per MIDI Map nur
mit einem eigenen Halte-Modus möglich.

## Ausgangslage im Code

| Thema | Heute | Fundstelle |
|---|---|---|
| Host-Parameter | Klang-Parameter pro Track: MD 26 × 16 Tracks, MM 58 × 6 Tracks. Sie gehen als CC an die Firmware. | `parameterDescriptions_{md,mm}.json`, `mdController.cpp:397` |
| Parameter-IDs | Seitennummern sind Teil der JUCE-IDs. Neue Seiten werden angehängt, nie umnummeriert. Später hinzugefügte Parameter landen in eigenen Versionsgruppen. | `mdautomation.h`, `jucePluginLib/controller.cpp:60` |
| Globale Parameter | `class: NonPartSensitive` registriert nur auf Part 0. | `jucePluginLib/controller.cpp:100` |
| Logische Panel-Controls | `PanelControl` (47 Tasten), `PanelEncoder` (10 Rotaries), `panelPacket()`, `panelEncoderCommand()`, `panelEncoderPressPacket()` | `mdLib/mdpanel.h` |
| Panel-Eingang | `PanelInputQueue`: MPSC, lock-frei, darf von jedem Thread befüllt werden | `mdLib/mdpanel.h`, `Hardware::trySendPanelEvent` |
| **Panel-Zustand** | `PanelRowState m_panelRows`, Shift-Latch, Encoder-Press, Navigations-Queue (`m_panelSteps`, 33-ms-Timer) – **alles im Editor** | `mdEditor.cpp:168, 640–680, 912, 1837` |
| Realtime-Hook | `Controller::processRealtimeParameterChanges()` wird im `processBlock` aufgerufen | `jucePluginLib/processor.cpp:869`, `mdController.cpp:715` |

Konsequenz: Panel-Eingaben existieren heute nur, solange das Plugin-Fenster
offen ist. Eine per MIDI gemappte PLAY-Taste muss aber auch bei eingeklapptem
Device funktionieren.

## Kernproblem: Gesten vs. absolute Werte

Host-Parameter sind absolute Werte (0…1). Das Panel kennt nur Gesten:

- **Tasten** sind Press/Release-Bits in einer Scan-Zeile (`row`, `mask`).
  Gleichzeitig gehaltene Tasten in derselben Zeile müssen zusammengeführt werden.
- **Encoder** liefern vorzeichenbehaftete Ticks (`0x01` / `0xff`), keine Position.

Die Brücke pro Control-Typ:

### Tasten und Encoder-Push → Bool-Parameter (momentan)

- 0 → 1: Press, 1 → 0: Release.
- Gleicher Wert wiederholt: keine Aktion (idempotent).
- Für Live gilt: Eine auf eine Note gemappte Bool-Taste kann Live als
  **Toggle** behandeln. Dann kommt das Release erst beim nächsten Notenanschlag.
  Für „Trig halten + Encoder drehen" (P-Lock) braucht es echtes Momentary-Verhalten.
  **Das ist in Live zu verifizieren** (siehe offene Fragen). Als Fallback gibt es
  einen optionalen Modus „Pulse": Jede Änderung auf 1 erzeugt Press, nach einem
  festen Intervall folgt automatisch das Release.

### Encoder → Relativ-Parameter

Mit Endlos-Encodern im relativen Modus von Live erhöht oder verringert Live
**seinen eigenen** Parameterwert und schickt dem Plugin den absoluten Wert.
Am Rand des Wertebereichs bleibt Lives Wert stehen. Variante B hilft dagegen
nicht. **Gesetzt ist deshalb Variante A mit für den Host sichtbarem
Re-Zentrieren** (Hypothese H3).

- **A. Virtuelle Position (gesetzt):** Der Parameter ist eine Position 0…127.
  Delta = neu − alt wird in Ticks umgesetzt, begrenzt durch `g_encoderBurstCap`.
  Das funktioniert direkt mit Potis und mit Lives Takeover-Modi. Nachteil:
  Am Anschlag (0 oder 127) geht es nicht weiter. Gegenmaßnahme: Nach einer
  Ruhephase (ca. 300 ms) setzt das Plugin den Parameter auf die Mitte zurück
  und meldet das dem Host (`setValueNotifyingHost`, mit oder ohne Gesture,
  siehe H3). Ein stilles internes Zurücksetzen funktioniert nicht, weil Live
  dann mit seinem alten Wert weiterzählt.
- **B. Snap-back-Delta:** 64 = Ruhe, darüber und darunter Ticks pro Wert.
  Danach meldet das Plugin selbst 64 an den Host zurück. Passt zu Endlos-Encodern
  im Relative-Modus von Live. Bei Automation-Aufnahme erzeugt es aber Rauschen
  in der Hüllkurve.

Für PR #93 (`mdPanelMidiMap`) gibt es schon Logik für Absolute / Relative-64 /
Relative-2's. Die sollte wiederverwendet werden.

### Re-Basis-Regel für Encoder

Ein Encoder-Parameter hat 128 Stufen (0–127), Mitte 64. Für jede eingehende
Änderung gilt:

1. Ist der Wert gleich dem zuletzt ausgewerteten Wert, passiert nichts. Das
   deckt die doppelte Zustellung unter VST3 ab.
2. Landet der Wert **genau auf 64**, lag die letzte Änderung **mindestens
   300 ms** zurück und beträgt der Sprung **mehr als eine Stufe**, ist das ein
   Rücksprung. Das kann ein eigener sein oder ein aus der Automation
   abgespielter. Es wird nur die Basis neu gesetzt, ohne Ticks.
3. Sonst gilt: Delta = neu − alt, als Ticks an die Firmware, begrenzt auf
   `g_encoderBurstCap`.

Verloren geht nur eine schnelle Drehung von mindestens zwei Rasten nach einer
Pause, die exakt auf 64 endet. Eine abgespielte Encoder-Automation gibt
Bewegungen wieder, keine Zielwerte. Das Ergebnis hängt also vom Zustand der
Maschine beim Start ab. Das kommt in die Nutzerdoku.

## Architektur

```
 Maus (Editor) ─┐                         ┌─> Hardware::trySendPanelEvent
                ├─> PanelHostParameters ──┤      (PanelInputQueue, MPSC)
 Host / MIDI ───┘    (Controller-Ebene)   └─> Rückmeldung an Editor (Taste gedrückt)
 Map / Automation      PanelRowState
                       Shift-Latch
                       Encoder-Delta
```

1. **`md::PanelGestureMapper` (neu, `mdLib`, JUCE-frei):** Übersetzt
   `(PanelParameterId, alter Wert, neuer Wert)` in Panel-Pakete. Er besitzt den
   einzigen `PanelRowState` pro Plugin-Instanz. Rein deterministisch und damit
   ohne JUCE testbar (Vorbild: `mdShiftPanelLatchTest`).
2. **Neue Parameterseiten** in `parameterDescriptions_{md,mm}.json`, angehängt
   hinter `Mute`, alle mit `class: NonPartSensitive`:
   - Seite „Panel Buttons": ein Bool pro verifiziertem `PanelControl`
   - Seite „Panel Encoders": ein Parameter pro `PanelEncoder`
   - Seite „Encoder Push": ein Bool pro Data-Entry-Encoder A–H

   Controls, für die `panelPacket()` beim jeweiligen Modell `nullopt` liefert,
   bekommen keinen Parameter.
3. **Controller (`mdController`)** fängt die neuen Seiten in
   `sendParameterChange` ab und reicht sie an den Mapper weiter. Sie gehen
   nicht in den CC-Automationspfad. Auf dem Audio-Thread läuft das allokationsfrei
   bis zur `PanelInputQueue`.
4. **Editor** schickt Maus-Interaktion nicht mehr direkt an `sendPanelEvent`,
   sondern setzt die Parameter (`Origin::Ui`, mit Change-Gesture). Damit sieht
   Live in Configure jeden Klick. Der Editor zeigt den Zustand an, er besitzt
   ihn nicht mehr.
5. **Navigationslogik** (Track per LED anwählen, `servicePanelNavigation`) bleibt
   vorerst im Editor. Ein späterer Schritt kann semantische Aktionen wie
   „Select Track N" als eigene Parameter anbieten. Dafür muss die
   LED-Rückkopplung ebenfalls aus dem Editor heraus.

### Regeln

- **Keine Persistenz von Gesten:** Panel-Parameter werden nicht in den
  Projekt-State geschrieben. Ein `setState` oder Preset-Load darf keine
  Tastendrücke auslösen. Beim Laden werden alle Panel-Parameter auf 0
  bzw. Ruhe zurückgesetzt, ohne Pakete zu erzeugen.
- **Keine Rückkopplungsschleife:** Eine Parameteränderung vom Host aktualisiert
  die Tastenanzeige, die wiederum keinen neuen Parameterwert erzeugt
  (`Parameter::Origin` unterscheiden).
- **Firmware-Bereitschaft:** Vor `isFirmwareMidiReady()` und abgeschlossenem
  Panel-Handshake werden Eingaben verworfen. Der Mapper-Zustand wird dabei
  zurückgesetzt, sodass keine Taste „hängen" bleibt.
- **Gehaltene Tasten beim Stop oder Bypass:** Bei Transport-Stop,
  Plugin-Bypass, `releaseResources` und Editor-Schließen bleibt der Zustand
  erhalten, weil der Host ihn besitzt. Bei Device-Neustart (State-Restore)
  wird er komplett freigegeben.
- **Parameter-IDs stabil:** nur neue Seiten anhängen, bestehende nie umnummerieren.

## Abgrenzung zu PR #93 (Panel MIDI)

| | PR #93 | Dieses Konzept |
|---|---|---|
| Mapping gehört | dem Plugin (eigenes Learn, Textdatei) | dem Host (Live MIDI Map, Configure) |
| Transport | eigener virtueller MIDI-Port | normale Host-Parameter |
| Windows | nein (JUCE hat dort keine virtuellen Ports) | ja |
| Automation-Lanes | nein | möglich |
| Semantik-Grenze | volle Kontrolle über Note-On/Off | abhängig davon, was Live sendet |

Beide Ansätze ergänzen sich. Die gemeinsame Basis (Mapper, Encoder-Modi,
Zustand außerhalb des Editors) sollte nur einmal existieren.

## Testgetriebene Umsetzung

Reihenfolge: Jeder Schritt beginnt mit einem fehlschlagenden Test.

1. **Mapper-Unit-Test** (`mdPanelGestureMapperTest`, JUCE-frei, wird in der
   schnellen Headless-Konfiguration gebaut):
   - Press → Release erzeugt die Pakete `(row, mask)` und `(row, 0)`.
   - Zwei Tasten in derselben Zeile: Die Masken werden zusammengeführt,
     beim Release bleibt nur das Bit der anderen Taste.
   - Release ohne Press und wiederholter gleicher Wert erzeugen keine Pakete.
   - Encoder: Delta +3 ergibt 3 × `0x01`, Delta −200 wird auf `g_encoderBurstCap` begrenzt.
   - Für nicht verifizierte Controls je Modell gibt es keinen Parameter und kein Paket.
   - Reset gibt alle gehaltenen Bits frei.
2. **Parameter-Registrierungstest:** Pro Modell hat die Anzahl der neuen
   Parameter den erwarteten Wert. Alle sind `NonPartSensitive`, also nur
   einmal vorhanden, und die IDs bestehender Parameter bleiben unverändert
   (Snapshot der alten ID-Liste).
3. **Processor-Test ohne Editor** (Vorbild: `mdAutomationParameterTest`):
   Wird ein Host-Parameter gesetzt, steigt `getPanelInputStatus().pendingPackets`.
   Ein State-Restore erzeugt dagegen keine Pakete.
4. **Firmware-Test mit ROM** (Vorbild: `mdEncoderPressFirmwareTest`,
   `mdPanelReadinessFirmwareTest`; gated über `GEARMULATOR_*_FIRMWARE_BIN`,
   `SKIP_RETURN_CODE 77`):
   - Parameter „Play" 0→1→0 lässt die Play-LED über den `FrontPanelPublisher` leuchten.
   - Trig halten + Encoder-Parameter drehen erzeugt einen P-Lock. Prüfbar über
     LCD bzw. Pattern-Dump.
5. **Editor-Test** (Vorbild: `mdLcdEditorPointerTest`): Ein Mausklick auf eine
   Taste erzeugt eine Change-Gesture am passenden Parameter.
6. **`pluginTester -automation-smoke`** über alle neuen Parameter.
7. **Manuelle Abnahme in Live** (Checkliste): Configure lernt eine Taste per
   Klick, MIDI Map auf Note und CC, Momentary-Verhalten, Encoder mit Poti und
   mit Endlos-Encoder, eingeklapptes Device, Projekt speichern und neu laden
   ohne Phantomdrücke.
