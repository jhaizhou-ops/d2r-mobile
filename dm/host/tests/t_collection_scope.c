#include "../dm_textbox.c"
static int checks,failures;
#define CHECK(c,msg) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",msg);}}while(0)
static unsigned char widgets[7][0x100];
static uint64_t setup(const char *panel){memset(widgets,0,sizeof widgets);const char *names[]={"Root",panel,"ItemScrollViewContainer","Container","ListItem0","ItemSlotBackground_Locked","ItemSlotText"};for(int i=0;i<7;i++){uint64_t w=(uint64_t)widgets[i];*(const char **)(w+8)=names[i];*(uint64_t *)(w+0x30)=i?(uint64_t)widgets[i-1]:0;*(uint8_t *)(w+0x50)=1;}return(uint64_t)widgets[6];}
int main(void){
    uint64_t root=(uint64_t)widgets[0],text;
    const char *panels[]={"ChronicleUniquesListPanel","ChronicleSetsListPanel","ChronicleRunewordsListPanel"};
    for(int i=0;i<3;i++){text=setup(panels[i]);CHECK(collection_scope(text,root,1,1),"Admite sólo fila viva bajo cada una de las tres colecciones PC españolas");}
    CHECK(!collection_scope(text,root,0,1),"Mando/iOS permanecen intactos");CHECK(!collection_scope(text,root,1,0),"Idiomas ajenos a español permanecen intactos");
    *(const char **)(text+8)="ItemSlotTimestamp";CHECK(!collection_scope(text,root,1,1),"No modifica otros campos de la fila");
    text=setup("TouchVault");CHECK(!collection_scope(text,root,1,1),"No modifica el alijo personalizado");
    text=setup("TooltipsPanel");CHECK(!collection_scope(text,root,1,1),"No modifica tooltips/notas");
    text=setup("ChronicleUniquesListPanelExtra");CHECK(!collection_scope(text,root,1,1),"El nombre de panel debe ser exacto");
    for(int i=1;i<7;i++){text=setup(panels[0]);widgets[i][0x50]=0;CHECK(!collection_scope(text,root,1,1),"Rechaza campo o ancestro oculto");}
    text=setup(panels[0]);CHECK(!collection_scope(text,(uint64_t)widgets[3],1,1),"Rechaza árbol ajeno al root vigente");
    *(uint64_t *)(widgets[1]+0x30)=0;CHECK(!collection_scope(text,root,1,1),"Rechaza panel separado del árbol vivo");
    text=setup(panels[0]);*(uint64_t *)(widgets[2]+0x30)=(uint64_t)widgets[4];CHECK(!collection_scope(text,root,1,1),"Un ciclo de padres no se recorre indefinidamente");
    CHECK(!collection_scope(0,root,1,1)&&!collection_scope(text,0,1,1),"Root y widget nulos no activan contexto");
    printf("COLLECTION SCOPE %d comprobaciones, %d fallos\n",checks,failures);return failures!=0;
}
