"""Exercise the actual host capture path; ROMs stay outside this repository.

Usage: python tests/sample-rates.py HOST ROM_ROOT [--baseline OLD_HOST]
ROM_ROOT contains SC-55-v1.21 and SC-55mk2-v1.01 directories.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import wave

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('host', type=Path)
p.add_argument('rom_root', type=Path)
p.add_argument('--baseline', type=Path)
args = p.parse_args()
out = Path(__file__).resolve().parents[1] / 'build/sample-rate-results'
out.mkdir(parents=True, exist_ok=True)
host = out / 'vsc55.exe'
shutil.copy2(args.host, host)
ini = out / 'VSC55.INI'

def capture(exe, model, folder, name, rate, buffer_ms=20):
    wav = out / (name + '.wav')
    result = subprocess.run([str(exe), model, str(args.rom_root.resolve() / folder),
        '--no-midi', '--demo', '--seconds', '4', '--buffer-ms', str(buffer_ms),
        '--capture', str(wav)], cwd=out, capture_output=True, timeout=60)
    (out / (name + '.stdout')).write_bytes(result.stdout + result.stderr)
    log = (exe.parent / 'VSC55.LOG').read_text()
    (out / (name + '.log')).write_text(log)
    assert result.returncode == 0 and 'RESULT=PASS' in log, (name, result.returncode, log)
    with wave.open(str(wav)) as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()) == (rate, 2, 2, rate*4), name
        pcm = w.readframes(w.getnframes())
        assert any(pcm), name
    print('PASS', name, 'rate', rate, flush=True)
    return pcm, log

for model, folder in [('mk1-v1.21', 'SC-55-v1.21'), ('mk2-v1.01', 'SC-55mk2-v1.01')]:
    reference = None
    if args.baseline:
        old_dir = out / 'baseline'
        old_dir.mkdir(exist_ok=True)
        old = old_dir / 'vsc55.exe'
        shutil.copy2(args.baseline, old)
        reference, _ = capture(old, model, folder, model+'-baseline', 48000)
    for setting, rate in [(None,48000), ('48000',48000), ('44100',44100),
                          ('32000',32000), ('22050',22050), ('16000',16000),
                          ('11025',11025), ('12345',48000), ('0',48000)]:
        ini.write_text('[Audio]\n' + ('' if setting is None else 'SampleRate='+setting+'\n'))
        pcm, log = capture(host, model, folder, model+'-'+str(setting), rate,
                           10 if rate==11025 else 20)
        assert 'OUTPUT_RATE='+str(rate) in log
        assert 'NATIVE_RATE='+('64000' if model.startswith('mk1') else '66207') in log
        if rate == 48000 and reference is not None:
            assert pcm == reference, '48 kHz PCM changed: '+model+' '+str(setting)
print('PASS all rates, fallback, WAV durations and baseline PCM comparisons', flush=True)
