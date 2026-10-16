"""Native no-follow metadata semantics for regular, dangling and directory links."""
from pathlib import Path
import subprocess
import tempfile
import sys
assert sys.platform.startswith('aros'), sys.platform
source = r'''
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d errno %d\n",__LINE__,errno); return 1; } } while (0)
int main(void) {
 struct stat s; struct stat64 t; ino64_t ino; mode_t before; int fd;
 CHECK(lstat("dangling",&s)==0 && S_ISLNK(s.st_mode));
 CHECK(lstat64("dangling",&t)==0 && S_ISLNK(t.st_mode)); ino=t.st_ino;
 CHECK(lstat64("link",&t)==0 && S_ISLNK(t.st_mode) && ino!=t.st_ino);
 CHECK(stat("dangling",&s)==-1 && errno==ENOENT);
 CHECK(stat("link",&s)==0 && S_ISREG(s.st_mode)); before=s.st_mode;
 CHECK(lstat("dirlink/child",&s)==0 && S_ISREG(s.st_mode));
 CHECK(lstat64("dirlink/child",&t)==0 && S_ISREG(t.st_mode)); ino=t.st_ino;
 /* AROS inode values are path-derived: compare the same alias. */
 CHECK(stat64("dirlink/child",&t)==0 && ino==t.st_ino);
 CHECK(lstat("dirlink/",&s)==0 && S_ISDIR(s.st_mode));
 CHECK(lstat(".",&s)==0 && S_ISDIR(s.st_mode));
 CHECK(lstat("/SYS/",&s)==0 && S_ISDIR(s.st_mode));
 CHECK(lstat("/",&s)==0 && S_ISDIR(s.st_mode));
 CHECK(lstat64("///",&t)==0 && S_ISDIR(t.st_mode));
 CHECK(lstat("target/",&s)==-1 && errno==ENOTDIR);
 fd=open(".",O_RDONLY); CHECK(fd>=0);
 CHECK(fstatat(fd,"dangling",&s,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(s.st_mode));
 CHECK(fstatat64(fd,"dangling",&t,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(t.st_mode));
 CHECK(fchmodat(fd,"dangling",0600,AT_SYMLINK_NOFOLLOW)==-1 && errno==ENOTSUP);
 CHECK(fchmodat(fd,"link",0600,AT_SYMLINK_NOFOLLOW)==-1 && errno==ENOTSUP);
 CHECK(stat("target",&s)==0 && s.st_mode==before);
 CHECK(fchmodat(fd,"target",0600,AT_SYMLINK_NOFOLLOW)==0);
 CHECK(lstat("missing",&s)==-1 && errno==ENOENT);
 CHECK(lstat("target/child",&s)==-1);
 CHECK(close(fd)==0);
 puts("NATIVE-LSTAT-NOFOLLOW-PASS"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='native-lstat-') as directory:
    p=Path(directory)
    subprocess.run(['/SYS/Developer/bin/mkdir','-p',str(p/'parents/deep')],check=True)
    assert (p/'parents/deep').is_dir()
    (p/'target').write_text('data\n')
    (p/'dir').mkdir(); (p/'dir/child').write_text('child\n')
    (p/'dangling').symlink_to('missing')
    (p/'link').symlink_to('target')
    (p/'dirlink').symlink_to('dir')
    (p/'test.c').write_text(source)
    subprocess.run(['/SYS/Developer/bin/clang','-mcmodel=large','-mno-red-zone','-nix',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],cwd=p,check=True)
