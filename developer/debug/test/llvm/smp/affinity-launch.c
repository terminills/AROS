#include <stdio.h>
#include <stdlib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <dos/dostags.h>
#include <exec/tasks.h>
#include <utility/tagitem.h>
APTR KernelBase;
int main(int argc,char **argv) {
 KernelBase=OpenResource("kernel.resource");
 if(!KernelBase)return 20;
 if(argc==2 && argv[1][0]=='?') {printf("CPU=%lu\n",(unsigned long)KrnGetCPUNumber());return 0;}
 if(argc!=3)return 21;
 unsigned cpu=(unsigned)strtoul(argv[1],NULL,10);
 if(cpu>=KrnGetCPUCount())return 22;
 void *mask=NULL;
 if(KrnGetCPUCount()>1) {mask=KrnAllocCPUMask();if(!mask)return 23;
 KrnClearCPUMask(mask);KrnGetCPUMask(cpu,mask);}
 printf("AFFINITY launch cpu=%u command=%s\n",cpu,argv[2]);fflush(stdout);
 LONG rc=SystemTags(argv[2],mask?NP_Affinity:TAG_IGNORE,(IPTR)mask,NP_StackSize,67108864,SYS_Asynch,FALSE,SYS_Output,Output(),SYS_Error,Output(),TAG_END);
 printf("AFFINITY return cpu=%u rc=%ld\n",cpu,(long)rc);fflush(stdout);
 return rc<0?24:rc;
}
