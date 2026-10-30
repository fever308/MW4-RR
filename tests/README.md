# CPU regression tests

After building, run `test.cmd "path\to\NGX\include"` from the project root.
The tests use fake COM objects and mock NGX functions. They do not create a
GPU device, launch the game, or validate visual results.

The runtime-shader test always checks rejection of missing shader input and
the structural normalization on synthetic text. For the optional CPU-only
checks, pass privately supplied files: the supported native composite, its
independently prepared expected guide shader, the player-captured composite
variant, and the full path to the pinned DXC DLL, in that order, to
`build/tests/runtime-shader.exe`. This verifies both pinned guides, that each
adds only the guide output to its own instructions, mixed concurrent cache
reuse, per-input failure isolation, structural acceptance of an equivalent
in-memory rebuild, and refusal of altered inputs. `--fingerprint <shader> <dll>`
prints the fingerprints used to pin a newly captured variant. These private
fixture files are not distributed.

Coverage includes default settings, variable resource extents, configuration
validation, preset selection and retirement, unfinished command recordings,
fence completion, exactly-once hook forwarding, private transition lock
ordering, native Super Resolution replacement and history recovery, and
spatial fallback coordinates, weapon depth, the water responsivity mask and the
transparency albedo blend. The private-transition test includes an optional
`RR_RELEASE_NEGATIVE_CONTROL` compile definition that deliberately bypasses
the internal-call guard and is expected to fail. Never use that definition
for an addon build.
