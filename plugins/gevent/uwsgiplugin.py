import os
from distutils import sysconfig
import subprocess

def splitted_output_of(cmdline):
    process = subprocess.Popen(cmdline.split(), stdout=subprocess.PIPE)
    if process.wait():
        raise Exception("Command %s returned non-zero" % cmdline)
    return process.stdout.read().strip().split()

NAME='gevent'

python_config_script = os.environ.get('UWSGICONFIG_PYTHONCONFIG')
if python_config_script:
    CFLAGS = splitted_output_of(python_config_script + " --includes")
else:
    CFLAGS = ['-I' + sysconfig.get_python_inc(), '-I' + sysconfig.get_python_inc(plat_specific=True)]
LDFLAGS = []
LIBS = []

GCC_LIST = ['gevent', 'hooks']
