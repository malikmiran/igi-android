# IGI Port - Roadmap

Goal: play Project I.G.I. (2000) natively on Android from the user's own game files, using a new engine
written from public format research (https://project-igi-research.vercel.app). No game data or original
code is bundled; the app copies `.res` archives from a folder the user picks.

Each stage ends in an APK that can be tested.

| Stage | Deliverable | Status |
|-------|-------------|--------|
| 0 | Project + CI builds APK. Folder import. ILFF/RES/TEX/MEF parsers (unit-tested on synthetic data). GLES3 viewer with touch orbit. On-screen diagnostics. | **Done (this commit)** |
| 1 | **Validate against real files.** Run on a phone, send the diagnostic text, fix layouts that differ from the notes. Handle compressed RES entries, name tables, type-1/3 vertex strides, MEF materials. | Next |
| 2 | Textures: TEX mip chains, MTP model-texture packages, textured model viewer, asset browser. | |
| 3 | Terrain: HMP heightmaps, LMP lightmaps, CTR/CMD terrain meshes, OLM object lightmaps. First full level rendered. | |
| 4 | Player: first-person camera, touch controls (virtual stick + look), collision (BIT mask, model collision), level load from `level/N` data. | |
| 5 | Script VM: QVM bytecode interpreter and the native-function table (largest risk: ~mission logic, objectives, triggers). | |
| 6 | AI + weapons: graphs.dat navigation, enemy behaviours from AI scripts, weapons, damage, IFF skeletal animation. | |
| 7 | Audio (WAV/ADPCM), HUD/menus (FNT, PIC, SPR), video (AFP/AVI), save games. | |
| 8 | Polish: performance, controller support, settings, release signing. | |

## Known risks
- The research notes are partly unverified against retail files (the MEF page says so itself). Stage 1 exists
  because of this.
- QVM opcode semantics are "version- and source-dependent" per the notes. Stage 5 may need disassembly of the
  retail script set and trial and error.
- This is a very large project. Realistic scope per stage is weeks, not hours.
