"""Exercise the status protocol, launch feedback, cooldown and output geometry."""
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    mock = root / "swaymsg"
    mock.write_text('#!' + shutil.which('bash') + '''
if [[ $* == '-p -t get_outputs' ]]; then
    printf "Output HDMI-A-1 'Monitor'\\n  Current mode: 1280x720 @ 60 Hz\\n  Scale factor: 1.000000\\n  Transform: normal\\n"
    printf "Output DSI-1 'Panel'\\n  Current mode: 368x552 @ 75 Hz\\n  Scale factor: 1.000000\\n  Transform: 90\\n"
else
    printf '%s\\n' "$*" >> "$CALLS"
fi
''')
    mock.chmod(0o755)
    entries = root / "entries"
    label = 'Demo "quoted" \\ path'
    entries.write_text(f'{label}=env VALUE=a=b badge-demo\nClose=swaymsg kill\n')
    log = root / "calls"
    env = dict(os.environ, PATH=f'{root}:{os.environ["PATH"]}', CALLS=str(log))
    p = subprocess.Popen(['bash', '-euo', 'pipefail', sys.argv[1], 'DSI-1', str(entries)],
                         env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert json.loads(p.stdout.readline()) == {'version': 1, 'click_events': True}
        assert p.stdout.readline() == '[\n'
        def frame():
            return json.loads(p.stdout.readline().rstrip(',\n'))
        initial = frame()
        assert [b['full_text'] for b in initial] == [label, 'Close']
        assert [b['min_width'] for b in initial] == [273, 273]
        def tap(i):
            p.stdin.write(',' + json.dumps({'name': 'touch', 'instance': str(i), 'button': 1}) + '\n')
            p.stdin.flush()
        tap(0)
        assert frame()[0]['full_text'] == 'Starting…'
        tap(0)  # A duplicate must neither launch nor redraw.
        tap(1)  # Close remains available while Demo is starting.
        assert [b['full_text'] for b in frame()] == ['Starting…', 'Starting…']
        restored = frame()
        while any(b['full_text'] == 'Starting…' for b in restored):
            restored = frame()
        assert restored == initial
        tap(0)
        assert frame()[0]['full_text'] == 'Starting…'
        p.stdin.close()
        assert p.wait(timeout=3) == 0
        assert log.read_text().splitlines() == [
            'exec -- env VALUE=a=b badge-demo', 'exec -- swaymsg kill',
            'exec -- env VALUE=a=b badge-demo',
        ]
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
print('touch bar: protocol, escaping, geometry and cooldown passed')
