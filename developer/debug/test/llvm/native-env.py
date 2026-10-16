"""Check edited environ vectors survive native env/exec, including PATH."""
import os
import subprocess
import sys

assert sys.platform.startswith('aros'), sys.platform
native_env = '/SYS/Developer/bin/env'
python = '/SYS/Developer/bin/python3'
parent = dict(os.environ, ADT_ENV_KEEP='keep', ADT_ENV_DROP='drop')
checks = [
    (['ADT_ENV_NEW=new', 'ADT_ENV_KEEP=replaced', python],
     "assert os.environ['ADT_ENV_NEW']=='new'; assert os.environ['ADT_ENV_KEEP']=='replaced'"),
    (['-u', 'ADT_ENV_DROP', 'ADT_ENV_NEW=new', python],
     "assert 'ADT_ENV_DROP' not in os.environ; assert os.environ['ADT_ENV_NEW']=='new'"),
    (['-i', 'PYTHONHOME=/SYS/Developer', 'PATH=/SYS/Developer/bin:/SYS/C',
      'ADT_ENV_NEW=isolated', 'python3'],
     "assert 'ADT_ENV_KEEP' not in os.environ; assert os.environ['ADT_ENV_NEW']=='isolated'"),
]
for arguments, check in checks:
    subprocess.run([native_env] + arguments + ['-c', 'import os; '+check],
                   env=parent, check=True)
print('NATIVE-ENV-PASS', flush=True)
