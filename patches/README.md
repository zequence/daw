# Local JUCE patches

`build.cmd` applies every `*.patch` here to `external/JUCE` (skipping ones
already applied), so a fresh clone builds with them.

- **juce-vst3-event-bus.patch** - multiport MIDI for VST3 plugins. Stock JUCE
  sends every MIDI event to a plugin's event bus 0 (port 1). Our route
  processor wraps traffic for port N as a private SysEx
  (`F0 7D 50 <bus> <status hi> <status lo> <data..> F7`, bus = N-1); the patch
  unwraps it at the VST3 boundary and sets `busIndex`, so plugins with several
  MIDI ports (Vienna Ensemble Pro: up to 16) are addressed directly, like
  Cubase does. Plugins never see the wrapper.

  The same patch delivers **program changes** and makes **CC mappings per
  port**. VST3 has no program change event: a plugin takes it as the
  program-change parameter (`kIsProgramChange`) of the unit its event bus and
  channel belong to. Stock JUCE sends a LegacyMIDICCOut event instead, which
  plugins ignore (VE Pro never switched Synchron articulations), and looks up
  CC mappings on bus 0 only. The host now finds the program-change parameter
  per bus and channel (`IUnitInfo::getUnitByBus`; plugins that don't report
  units but have one per port and channel, like VE Pro's "PrgCh 01-01" ...
  "PrgCh 16-16", get them in order) and sets it sample-accurately.

Re-create after editing JUCE:
`git -C external/JUCE diff -- <file> > patches/<name>.patch`
