"""Compile project-owned HLSL; no native game shader inputs are required."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value



def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxc', required=True, type=Path, help='DXC 1.8.2505.1 bin/x64')
    args = parser.parse_args()
    dxc = args.dxc.resolve() / 'dxc.exe'
    compiler = args.dxc.resolve() / 'dxcompiler.dll'
    out = ROOT / 'build'
    out.mkdir(exist_ok=True)
    require(hashlib.sha256(dxc.read_bytes()).hexdigest() ==
            'ce11bb02f6027055e0b9f8c062d13b14f373c876a16557a4fa744314117d31e7',
            'Use the x64 DXC 1.8.2505.1 executable')
    require(hashlib.sha256(compiler.read_bytes()).hexdigest() ==
            '5888d3e590f5bfd8743484e7f31313996734adec5741506271a5c6368cbe8ba9',
            'Use the x64 DXC 1.8.2505.1 compiler package')
    results = {}

    def header(path, shaders, extra=''):
        parts = ['// Generated locally; see tools/prepare.py for source and native-input provenance.\n#pragma once\n']
        for symbol, target, hashed in shaders:
            data = target.read_bytes()
            parts.append('inline constexpr unsigned char ' + symbol + '[]={' + ','.join(map(str, data)) + '};\n')
            if hashed:
                parts.append(f'inline constexpr unsigned long long {symbol}Hash=0x{fnv(data):016x}ull;\n')
            results[symbol] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(''.join(parts) + extra, encoding='utf-8')

    def compile_hlsl(source, leaf, profile='cs_6_0', entry='main', flags=()):
        target = out / (leaf + '.cso')
        subprocess.run([str(dxc), '-T', profile, '-E', entry, '-O3', *flags,
                        '-Fo', str(target), str(ROOT / 'shaders' / source)], check=True)
        return target

    header(out / 'rr_sparse.generated.h', [
        ('RRSpatialPassthrough', compile_hlsl('rr_spatial_passthrough.hlsl', 'rr_spatial_passthrough'), True)])
    header(out / 'rr_transparency_blend.generated.h', [
        ('RRTransparencyBlend', compile_hlsl('rr_transparency_blend.hlsl', 'rr_transparency_blend'), False)])
    header(out / 'rr_vrrt_native.generated.h', [
        ('RRVrrtProbability', compile_hlsl('rr_vrrt_probability.hlsl', 'rr_vrrt_probability', flags=('-Ges',)), False),
        ('RRVrrtScale', compile_hlsl('rr_sparse_scale_vrrt.hlsl', 'rr_sparse_scale_vrrt', flags=('-Ges',)), False),
        ('RRVrrtScaleFloat', compile_hlsl('rr_sparse_scale_vrrt.hlsl', 'rr_sparse_scale_vrrt_float', flags=('-Ges', '-D', 'RR_PACKED_FLOAT=1')), False),
        # SSS lighting (src/rr_lighting_source.h SssHistory2): on SSS pixels lobe 2 from native
        # temporal wherever it has history, else fresh unboosted, else this frame's spatial mean.
        *[(f'RRVrrtScaleSssHistory2{suffix}',
           compile_hlsl('rr_sparse_scale_vrrt.hlsl', f'rr_sparse_scale_vrrt{leaf}_ssshistory2',
                        flags=('-Ges', '-D', 'RR_NATIVE_LOBE=2', '-D', 'RR_NATIVE_FILL=1',
                               '-D', 'RR_SSS_MASK=1', '-D', 'RR_FILL_HISTORY=1',
                               '-D', 'RR_FILL_SPATIAL=1', '-D', 'RR_FILL_PREFER_NATIVE=1',
                               *extra)), False)
          for suffix, leaf, extra in (('', '', ()), ('Float', '_float', ('-D', 'RR_PACKED_FLOAT=1')))],
        # The same with the panel experiments: the edge-aware no-history fill (src/rr_edge_fill.h,
        # variant bit 0, EdgeBit) and the "SSS pixel" decision from reprojected Subsurface
        # Scattering pass coverage (src/rr_sss_coverage.h, bit 1, CoverageBit), alone and together.
        *[(f'RRVrrtScaleSssHistory2{name}{suffix}',
           compile_hlsl('rr_sparse_scale_vrrt.hlsl', f'rr_sparse_scale_vrrt{leaf}_ssshistory2{tag}',
                        flags=('-Ges', '-D', 'RR_NATIVE_LOBE=2', '-D', 'RR_NATIVE_FILL=1',
                               '-D', 'RR_SSS_MASK=1', '-D', 'RR_FILL_HISTORY=1',
                               '-D', 'RR_FILL_SPATIAL=1', '-D', 'RR_FILL_PREFER_NATIVE=1',
                               *defines, *extra)), False)
          for name, tag, defines in (('Edge', '_edge', ('-D', 'RR_FILL_EDGE=1')),
                                     ('Coverage', '_coverage', ('-D', 'RR_SSS_COVERAGE=1')),
                                     ('EdgeCoverage', '_edge_coverage',
                                      ('-D', 'RR_FILL_EDGE=1', '-D', 'RR_SSS_COVERAGE=1')))
          for suffix, leaf, extra in (('', '', ()), ('Float', '_float', ('-D', 'RR_PACKED_FLOAT=1')))]])
    header(out / 'sss-owned-shader/rr_sss_deinterleave.generated.h', [
        ('RRSSSDeinterleave', compile_hlsl('rr_sss_deinterleave.hlsl', 'rr_sss_deinterleave'), False),
        ('RRSSSDeinterleaveCoverage', compile_hlsl('rr_sss_deinterleave.hlsl', 'rr_sss_deinterleave_coverage',
                                                   flags=('-D', 'RR_SSS_COVERAGE_OUT=1')), False)])
    header(out / 'sss-owned-shader/rr_sss_stencil.generated.h', [
        ('RRSSSStencilVS', compile_hlsl('rr_sss_stencil.hlsl', 'rr_sss_stencil_vs', 'vs_6_0', 'vs'), False),
        ('RRSSSStencilPS', compile_hlsl('rr_sss_stencil.hlsl', 'rr_sss_stencil_ps', 'ps_6_0', 'ps'), False),
        ('RRSSSStencilMarkerPS', compile_hlsl('rr_sss_stencil.hlsl', 'rr_sss_stencil_marker_ps', 'ps_6_0', 'ps_marker'), False)])
    header(out / 'rr_sss_coverage.generated.h', [
        ('RRSSSCoverageKey', compile_hlsl('rr_sss_coverage_key.hlsl', 'rr_sss_coverage_key'), False)])
    # Water responsivity mask (src/rr_water.h).
    header(out / 'rr_water_masks.generated.h', [
        ('RRWaterMasks', compile_hlsl('rr_water_masks.hlsl', 'rr_water_masks'), False)])
    for source, entry, symbol, leaf in [
        ('rr_convert_guides.hlsl', 'main', 'RRGuideShader', 'rr_convert_guides'),
        ('rr_live_gate.hlsl', 'BeginFrame', 'RRLiveBegin', 'rr_live_begin'),
        ('rr_live_gate.hlsl', 'Validate', 'RRLiveValidate', 'rr_live_validate'),
        ('rr_live_gate.hlsl', 'Display', 'RRLiveDisplay', 'rr_live_display'),
        ('rr_weapon_depth.hlsl', 'main', 'RRWeaponDepth', 'rr_weapon_depth')]:
        subprocess.run(['fxc', '/nologo', '/T', 'cs_5_0', '/E', entry,
                        '/Fh', str(out / (leaf + '.generated.h')), '/Vn', symbol,
                        str(ROOT / 'shaders' / source)], check=True)
    (out / 'shader-build.json').write_text(json.dumps({'outputs': results}, indent=2) + '\n')
    print('Project-owned shaders compiled; runtime guide preparation uses the game-supplied shader.')


if __name__ == '__main__':
    main()

