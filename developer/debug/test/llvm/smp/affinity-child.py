import subprocess,sys
expected=int(sys.argv[1])
result=subprocess.run(['/SYS/Parallel/affinity-launch','?'],stdout=subprocess.PIPE,text=True,check=True)
print('AFFINITY-CHILD',repr(result.stdout),'expected',expected,flush=True)
assert result.stdout.strip()=='CPU='+str(expected),result.stdout
sys.exit(subprocess.run(sys.argv[2:]).returncode)
