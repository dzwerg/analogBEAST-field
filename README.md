# analogBEAST-field 1.0.0

A standalone synthesizer engine for **Teenage Engineering OP-1 Field** running **open-field** on OP-1 Field firmware **1.7.3**. Built and maintained by the `dzwerg` project. This is an independent project, not affiliated with Teenage Engineering or the open-field maintainers.

## Features

- Two oscillators, configurable waveforms and oscillator modulation
- Noise source, wavefolding, filter modes, distortion and filter envelope
- Three control pages with four colour-coded encoders and SHIFT controls
- Five built-in factory sounds: `analogbeast_init`, `ab_bass`, `ab_soft_pad`, `ab_fold_lead`, `ab_noisy_keys`
- Saved extra synthesizer parameters via the `abfield_extra` keep
- Direct access to OP-1 Field keyboard, synth presets and built-in signal chain

## Compatibility

- OP-1 Field OS **1.7.3** with **open-field**, supporting base level 10
- No standalone MIDI sequencer or arpeggiator is included
- Requires an existing open-field installation (do **not** flash this package as firmware)

## Installation

Once the release has a prebuilt `analogbeast.pack` file, open the [open-field installer](https://lap0573.github.io/open-field/), connect the OP-1 Field and use the pack section's **+** control to add the pack. Follow the on-device confirmation prompts. Alternatively install the `.pack` through the documented disk-mode workflow. Do not install the source ZIP as a pack.

## Build from source (Windows PowerShell)

1. Clone [LaP0573/open-field](https://github.com/LaP0573/open-field) and install its required Blackfin toolchain.
2. Extract the source ZIP **directly into the root** of that checkout, preserving `analogbeast_field/` and `BUILD-analogBEAST-field.ps1`.
3. Run `& "$HOME\open-field\BUILD-analogBEAST-field.ps1"` (adjust the path if needed).
4. The script writes **`analogbeast.pack`**, checks the pack and prints its result. Do not publish a failed check.

This v1.0.0 recipe uses **pack header version 100**, higher than the development series ending at version 25, so catalog/pack upgrades stay monotonic.

## Build status

The v1.0.0 Windows Blackfin cross-build has been confirmed by the author: pack header version 100 and `pack_build.py check` passed. Hardware validation of the **exact v1.0.0 binary** is required before publishing; the release script asks the author to confirm this.

## Licensing and attribution

Source code is distributed under **GPL-3.0-only**. See `LICENSE`. open-field is a separate project licensed under MIT; its tooling and headers are required only to build and use this extension and are not bundled here. This repo does not include proprietary OP-1 Field firmware.

## Security

A `.pack` is executable code on your device. Install only from a trusted source. This community project is unofficial and provided without warranty.
