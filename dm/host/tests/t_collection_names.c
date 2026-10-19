#include "../dm_textbox_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int checks,failures;
#define CHECK(c,msg) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",msg);}}while(0)
static void expect(const char *s,int expected,const char *text){char *out=NULL;size_t n=0;int k=dm_collection_names_rewrite(s,strlen(s),&out,&n);CHECK(k==expected,"Coincidencia exacta de presentación");CHECK(text?(out&&n==strlen(text)&&!memcmp(out,text,n)):!out,"Salida oficial o ausencia de cambios");free(out);}
int main(void){
    char path[]="/tmp/t_collection_names_XXXXXX";int fd=mkstemp(path);FILE *f=fdopen(fd,"w");
    fputs("# mapa contextual\n[Artesanía: golpe triturador]\\n[mp]Guantes pesados[normal]\tGuantes pesados\n[Defensa 225-301]\\n★[fs]Coraza de mago[excep.·ligera]\tCoraza de mago\n",f);fclose(f);
    expect("[mp]Guantes pesados[normal]",0,NULL);
    CHECK(dm_collection_names_load(path)==2,"Carga separada de mapa contextual");
    expect("[Artesanía: golpe triturador]\n[mp]Guantes pesados[normal]",1,"Guantes pesados");
    expect("[Defensa 225-301]\n★[fs]Coraza de mago[excep.·ligera]",1,"Coraza de mago");
    expect("Guantes pesados",0,NULL);expect("[Artesanía: otro dato]\n[mp]Guantes pesados[normal]",0,NULL);
    expect("Texto antes [Artesanía: golpe triturador]\n[mp]Guantes pesados[normal] después",0,NULL);
    char *out=NULL;size_t n=0;CHECK(!dm_textbox_rewrite("[Artesanía: golpe triturador]\n[mp]Guantes pesados[normal]",strlen("[Artesanía: golpe triturador]\n[mp]Guantes pesados[normal]"),&out,&n)&&!out,"Mapa contextual no altera reescritura general");
    dm_textbox_reset();expect("[Defensa 225-301]\n★[fs]Coraza de mago[excep.·ligera]",1,"Coraza de mago");
    dm_collection_names_reset();expect("[Defensa 225-301]\n★[fs]Coraza de mago[excep.·ligera]",0,NULL);
    CHECK(dm_collection_names_load("/tmp/no-existe-d2r-collection-map")==-1,"Ausencia de mapa deja presentación intacta");
    unlink(path);printf("COLLECTION NAMES %d comprobaciones, %d fallos\n",checks,failures);return failures!=0;
}
