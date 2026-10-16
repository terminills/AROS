from pathlib import Path
import subprocess, tempfile
temporary = tempfile.TemporaryDirectory(prefix='aros-raw-paste-')
p=Path(temporary.name)
s=(Path(__file__).resolve().parent.parent / 'con_handler.c').read_text();s=s[s.index('static void pump_raw_input('):s.index('\nstatic void stopwait')]
pre=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef unsigned char UBYTE; typedef long LONG; typedef int BOOL;
#define INPUTBUFFER_SIZE 256
#define PASTEBUFSIZE 16384
#define FHFLG_DEVICEMODE 1
#define CopyMem(s,d,n) memcpy(d,s,n)
#define FreeMem(p,n) free(p)
struct filehandle {int flags,conbufferpos,conbuffersize,inputsize,inputpos,inputstart,pastebuffersize,pastebufferpos; UBYTE rawpasteprefix; char devname[64]; UBYTE consolebuffer[1024],inputbuffer[258]; char *pastebuffer;};
static unsigned char output[100000], clip[16384]; static size_t used,cliplen; static int pending,limit,pastes;
static void HandlePendingReads(struct filehandle *f){ if(pending && f->inputsize){int n=f->inputsize<limit?f->inputsize:limit; memcpy(output+used,f->inputbuffer,n);used+=n;memmove(f->inputbuffer,f->inputbuffer+n,f->inputsize-n);f->inputsize-=n;pending=0;}}
static void do_paste(struct filehandle *f){assert(!f->pastebuffer);f->pastebuffer=malloc(16384);memcpy(f->pastebuffer,clip,cliplen);f->pastebuffersize=cliplen;f->pastebufferpos=0;pastes++;}
'''
post=r'''
static void drain(struct filehandle *f){int guard=0;while(f->inputsize||f->pastebuffer||f->conbufferpos<f->conbuffersize){assert(++guard<100000);pending=1;HandlePendingReads(f);pump_raw_input(f);}}
static void run(int native,int chunk,int readsize,int clipsize){struct filehandle f={0};f.flags=1;strcpy(f.devname,native?"consoleng.device":"serial.device");used=pastes=0;limit=readsize;cliplen=clipsize;for(int i=0;i<clipsize;i++)clip[i]=(unsigned char)(i%256);unsigned char in[]={3,27,'[','A',0x9b,'A',0x9b,'0','X',0x9b,'0',' ','v','z',0x9b,'0',' ','v',4}; unsigned char expected[40000];size_t n=0;for(size_t i=0;i<sizeof(in);){if(native&&i+4<=sizeof(in)&&!memcmp(in+i,"\x9b" "0 v",4)){memcpy(expected+n,clip,cliplen);n+=cliplen;i+=4;}else expected[n++]=in[i++];}for(size_t i=0;i<sizeof(in);){size_t k=sizeof(in)-i;if(k>(size_t)chunk)k=chunk;memcpy(f.consolebuffer,in+i,k);f.conbufferpos=0;f.conbuffersize=k;i+=k;pending=0;pump_raw_input(&f);drain(&f);}assert(used==n);assert(!memcmp(output,expected,n));assert(pastes==(native?2:0));assert(!f.rawpasteprefix);}
int main(void){int count=0;int sizes[]={0,1,2,255,256,257,4096,16384};for(int native=0;native<2;native++)for(int chunk=1;chunk<20;chunk++)for(int r=1;r<=256;r*=2)for(int c=0;c<8;c++){run(native,chunk,r,sizes[c]);count++;}printf("PASS %d fragmentation/backpressure cases\n",count);}
'''
(p/'pump-test.c').write_text(pre+s+post)
subprocess.run(['cc','-fsanitize=address,undefined','-g','-Wall','-Wextra',str(p/'pump-test.c'),'-o',str(p/'pump-test')],check=True)
subprocess.run([str(p/'pump-test')],check=True)
