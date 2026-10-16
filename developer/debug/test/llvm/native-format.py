"""Exercise C99 j/t formats and the GNU patch messages that exposed them."""
from pathlib import Path
import subprocess
import tempfile
import sys
assert sys.platform.startswith('aros'), sys.platform
source = r'''
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d\n",__LINE__); return 1; } } while (0)
int main(void) {
 char b[512]; wchar_t w[512]; intmax_t jn=-1; ptrdiff_t tn=-1;
 CHECK(snprintf(b,sizeof b,"Hunk #%jd %s at %td%s.",(intmax_t)7,"FAILED",(ptrdiff_t)-3,"!")==22);
 CHECK(!strcmp(b,"Hunk #7 FAILED at -3!."));
 snprintf(b,sizeof b,"%jd %ju %jx %td %tu %tx %s",(intmax_t)-12,(uintmax_t)42,(uintmax_t)42,(ptrdiff_t)-13,(uintptr_t)43,(uintptr_t)43,"tail");
 CHECK(!strcmp(b,"-12 42 2a -13 43 2b tail"));
 snprintf(b,sizeof b,"%jd %ju",INTMAX_MIN,UINTMAX_MAX);
 CHECK(!strcmp(b,"-9223372036854775808 18446744073709551615"));
 snprintf(b,sizeof b,"%td",PTRDIFF_MIN);
 CHECK(!strcmp(b,"-9223372036854775808"));
 snprintf(b,sizeof b,"%+08jd|%.5td|%#jx",(intmax_t)-42,(ptrdiff_t)7,(uintmax_t)42);
 CHECK(!strcmp(b,"-0000042|00007|0x2a"));
 snprintf(b,sizeof b,"abc%jnDEF%tn!",&jn,&tn); CHECK(jn==3 && tn==6);
 snprintf(b,sizeof b,"%d %u %ld %lu %lld %llu %zu %.2f %s",-1,2U,-3L,4UL,-5LL,6ULL,(size_t)7,1.25,"tail");
 CHECK(!strcmp(b,"-1 2 -3 4 -5 6 7 1.25 tail"));
 CHECK(swprintf(w,512,L"%jd %td %ju",(intmax_t)-12,(ptrdiff_t)-13,(uintmax_t)42)>0);
 CHECK(!wcscmp(w,L"-12 -13 42"));
 printf("Hunk #%jd succeeded at %td (offset %td line%s).\n",(intmax_t)1,(ptrdiff_t)2,(ptrdiff_t)1,"");
 puts("NATIVE-FORMAT-JT-PASS"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='native-format-') as d:
 p=Path(d); (p/'test.c').write_text(source)
 subprocess.run(['/SYS/Developer/bin/clang','-mcmodel=large','-mno-red-zone','-nix',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 (p/'input').write_text('extra\na\nb\nc\n')
 (p/'change.diff').write_text('--- input\n+++ input\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n')
 result=subprocess.run(['/SYS/Developer/bin/patch','--verbose','-p0','-i','change.diff'],cwd=d,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 print(result.stdout,flush=True); assert result.returncode==0
 assert (p/'input').read_text()=='extra\na\nB\nc\n'
 assert 'offset 1 line' in result.stdout and '%td' not in result.stdout
 (p/'input').write_text('unrelated\n')
 result=subprocess.run(['/SYS/Developer/bin/patch','--batch','-p0','-i','change.diff'],cwd=d,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 print(result.stdout,flush=True); assert result.returncode==1
 assert 'Hunk #1 FAILED' in result.stdout and '%jd' not in result.stdout
print('NATIVE-PATCH-FORMAT-PASS',flush=True)
