#include "../dm_cursor.c"
#include <stdio.h>
static int checks,failures,calls;
static unsigned char root[256],sound[512],manager[512],container[512],selected[256],other[256];
static uint64_t roots[3],sound_children[1],items[1];
static uint64_t ptr(void*p){return (uint64_t)(uintptr_t)p;}
static module_t module;
static void widget(unsigned char*w,const char*n){*(const char**)(w+8)=n;w[0x50]=w[0x51]=1;}
static void native_focus(uint64_t pair,uint64_t current,uint64_t target){calls++;if(pair!=ptr(manager)+0x180||current!=ptr(container)||target)abort();*(uint64_t*)(manager+0x190)=target;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}else printf("PASS %s\n",t);}while(0)
int main(void){
 module.base=(uint8_t*)0x100000;widget(root,"Root");widget(sound,"SoundOptionsPanel");widget(manager,"FocusManager");widget(container,"AudioRows");widget(selected,"SoundSlider");widget(other,"OtherPanel");
 roots[0]=ptr(sound);roots[1]=ptr(manager);roots[2]=ptr(other);*(uint64_t*)(root+0x58)=ptr(roots);*(uint64_t*)(root+0x60)=3;sound_children[0]=ptr(container);*(uint64_t*)(sound+0x58)=ptr(sound_children);*(uint64_t*)(sound+0x60)=1;items[0]=ptr(selected);*(uint64_t*)(container+0x58)=ptr(items);*(uint64_t*)(container+0x60)=1;
 *(uint64_t*)manager=(uint64_t)module.base+0x20b7f88;manager[0x180]=1;*(uint64_t*)(manager+0x188)=ptr(container);*(uint64_t*)(manager+0x190)=ptr(selected);*(uint64_t*)(manager+0x198)=ptr(selected);*(uint64_t*)(container+0x160)=ptr(selected);
 CHECK(audio_focus_clear(&module,ptr(root),native_focus)&&calls==1,"Audio visible cede selección mediante función original y contenedor actual");CHECK(!*(uint64_t*)(manager+0x190)&&*(uint64_t*)(manager+0x198)==ptr(selected)&&*(uint64_t*)(container+0x160)==ptr(selected),"Ceder foco conserva memorias de navegación de mando");
 CHECK(!audio_focus_clear(&module,ptr(root),native_focus)&&calls==1,"Foco ya liberado no vuelve a notificarse");*(uint64_t*)(manager+0x190)=ptr(selected);sound[0x50]=0;CHECK(!audio_focus_clear(&module,ptr(root),native_focus),"Audio oculto no cambia foco");sound[0x50]=1;
 *(uint64_t*)manager=0;CHECK(!audio_focus_clear(&module,ptr(root),native_focus),"Clase FocusManager desconocida se rechaza");*(uint64_t*)manager=(uint64_t)module.base+0x20b7f88;
 *(uint64_t*)(manager+0x190)=ptr(other);CHECK(!audio_focus_clear(&module,ptr(root),native_focus),"Selección de otra página no se modifica");*(uint64_t*)(manager+0x190)=ptr(selected);*(uint64_t*)(manager+0x188)=0xdead;CHECK(!audio_focus_clear(&module,ptr(root),native_focus),"Contenedor retirado de árbol no se desreferencia");*(uint64_t*)(manager+0x188)=ptr(container);manager[0x180]=0;CHECK(!audio_focus_clear(&module,ptr(root),native_focus),"Gestor sin par de mando habilitado conserva estado");
 printf("AUDIO FOCUS %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
}
