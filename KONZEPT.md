# MusicEngine – Konzept

## Ziel
1. **Editor**: Noten in einer Piano-Roll zeichnen und abspielen.
2. **Generator**: Aus Regeln + Einstellungen jedes Mal einen neuen Song erzeugen, der trotzdem gut klingt.
3. **Erweiterbar**: Instrumente als Plugins (Start: Piano).

## Was ein guter Generator braucht
| Baustein | Inhalt |
|---|---|
| Tonart & Skala | Dur, Moll, Dorisch, Pentatonik … – alle Noten bleiben "in key" |
| Akkordfolgen | Funktionsharmonik (I–IV–V–vi), Markov-Kette über Akkorde mit Gewichten |
| Songstruktur | Intro, Strophe (Nebenteil), Refrain (Hauptteil), Bridge, Outro – als Grammatik, z. B. `I V R V R B R O` |
| Motive | Kurze Melodie-Idee wird wiederholt + variiert (transponieren, umkehren, Rhythmus ändern) → Wiedererkennung |
| Melodie-Regeln | Akkordtöne auf betonten Zählzeiten, meist Schrittbewegung, Sprünge danach gegenläufig auflösen, Phrasen enden auf Grundton/Terz |
| Rhythmus | Taktart, Rhythmus-Patterns, Synkopen-Wahrscheinlichkeit |
| Begleitung | Bass (Grundton), Akkorde/Arpeggios, später Drums |
| Dynamik | Lautstärke-/Dichte-Kurve über den Song (Refrain voller) |
| Zufall | Seed → reproduzierbar; "Kreativität"-Regler steuert, wie stark Regeln gebrochen werden dürfen |

## Einstellungen (Beispiele)
Tempo (BPM), Länge, Tonart, Skala, Taktart, Stimmung (fröhlich/traurig → Dur/Moll, Tempo), Struktur, Kreativität, Dichte, Instrumente, Seed.

## Architektur (C++)
```
core/       Note, Pattern, Track, Song, Zeit (Ticks/PPQ)
theory/     Scale, Chord, Progression
generator/  StructureGen, HarmonyGen, MelodyGen, BassGen, RhythmGen
audio/      Mixer, Ausgabe (miniaudio), Sequencer
instruments/ IInstrument-Interface -> PianoSynth (später Sampler, Strings, Drums …)
ui/         Piano-Roll (SDL2 oder Dear ImGui), Einstellungs-Panel
io/         MIDI-Export, Song speichern/laden (JSON)
```
Neues Instrument = neue Klasse, die `IInstrument` implementiert (`noteOn`, `noteOff`, `render`) und sich in einer Registry anmeldet.

## Vorgeschlagene Libraries
- **miniaudio** – Audio-Ausgabe (single header)
- **Dear ImGui + SDL2** – UI / Piano-Roll
- **nlohmann/json** – Speichern
- Build: **CMake**

## Schritte
1. CMake-Gerüst, Audio-Ausgabe, einfacher Piano-Synth
2. Sequencer + Song-Datenmodell
3. Piano-Roll-Editor
4. Generator: Struktur → Harmonie → Melodie → Bass
5. Einstellungs-UI, Seed, MIDI-Export
6. Weitere Instrumente
