"""Run the freshly native-built FlexCat on real ADT catalog inputs."""
from pathlib import Path
import subprocess
import sys
import tempfile

assert sys.platform.startswith('aros'), sys.platform
source = Path('/SYS/Sources/AROS')
tool = '/SYS/NativeBuild/bin/aros-x86_64/tools/flexcat'
inputs = source / 'workbench/prefs/input/catalogs'
with tempfile.TemporaryDirectory(prefix='native-flexcat-') as directory:
    header = Path(directory) / 'input.h'
    catalog = Path(directory) / 'input.catalog'
    subprocess.run([tool, str(inputs / 'input.cd'),
                    str(header) + '=' + str(source / 'tools/flexcat/src/sd/C_h_aros.sd')], check=True)
    text = header.read_text()
    assert 'MSG_NAME' in text and 'Input Preferences' in text, text
    result = subprocess.run([tool, str(inputs / 'input.cd'), str(inputs / 'german.ct'),
                             'CATALOG=' + str(catalog)])
    # FlexCat uses 5 for translation warnings, >=10 for errors.
    assert result.returncode in (0, 5), result.returncode
    data = catalog.read_bytes()
    assert data[:4] == b'FORM' and data[8:12] == b'CTLG', data[:12]
    assert int.from_bytes(data[4:8], 'big') + 8 == len(data)
    assert b'Eingabe-Einstellungen' in data
print('NATIVE-FLEXCAT-PASS', flush=True)
