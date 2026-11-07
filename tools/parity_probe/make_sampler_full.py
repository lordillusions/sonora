#!/usr/bin/env python3
"""DEV-14b: writes SamplerFull.adv for tools/parity_probe/suites/sampler_laws2.txt.

The reference's default Sampler (Defaults/Instruments/Sampler.adv) leaves the Osc, pitch envelope, Shaper, Aux envelope,
LFO and aux LFO slots empty; switching one on in the XML alone made the reference crash. This copies each empty slot's
content from the first installed factory Sampler preset that has it and switches every optional section off, so each
suite case can switch one on. Reads the local reference installation at run time; the output stays outside the repo.

usage: make_sampler_full.py <out.adv> [--library <Core Library folder>]
"""
import copy, glob, gzip, sys
import xml.etree.ElementTree as ET

LIBRARY = '/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Core Library/'
HOLDERS = ['Player/SubOsc', 'Pitch/Envelope', 'Filter', 'Shaper', 'AuxEnv', 'Lfo', 'AuxLfos.0', 'AuxLfos.1']


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    library = sys.argv[sys.argv.index('--library') + 1].rstrip('/') + '/' if '--library' in sys.argv else LIBRARY
    root = ET.fromstring(gzip.open(library + 'Defaults/Instruments/Sampler.adv').read())
    device = root.find('MultiSampler')
    presets = sorted(glob.glob(library + 'Devices/Instruments/Sampler/**/*.adv', recursive=True))
    for holder in HOLDERS:
        value = device.find(holder + '/Slot/Value')
        if len(list(value)) == 0:
            for path in presets:
                source = ET.fromstring(gzip.open(path).read()).find('MultiSampler')
                filled = source.find(holder + '/Slot/Value') if source is not None else None
                if filled is not None and len(list(filled)) > 0:
                    value.append(copy.deepcopy(list(filled)[0]))
                    break
        device.find(holder + '/IsOn/Manual').set('Value', 'false')
    with gzip.GzipFile(sys.argv[1], 'wb', mtime=0) as out:  # deterministic: no timestamp in the gzip header
        out.write(b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root))


if __name__ == '__main__':
    main()
