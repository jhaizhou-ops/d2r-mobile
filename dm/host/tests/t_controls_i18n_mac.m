// Synthetic EN/zh checks for macOS binding labels and native UI formats; no game assets or windows.
#import <Foundation/Foundation.h>
#include "dm_controls_mac.h"
#include "dm_i18n.h"
#include <stdio.h>
#include <string.h>
static int checks,failed;
#define CHECK(c) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d\n",__FILE__,__LINE__);}}while(0)
int main(void){@autoreleasepool{
 for(int lang=0;lang<=8;lang+=8){
  atomic_store(&g_dm_lang,lang);char text[512],error[512];
  for(int i=0;i<DM_CTL_COUNT;i++){dm_controls_action_label(i,text,sizeof text);CHECK(*text&&strncmp(text,"dm",2));}
  dm_controls_label(-1,text,sizeof text);CHECK(!strcmp(text,lang?"未分配":"Unbound"));
  dm_controls_label(256,text,sizeof text);CHECK(!strcmp(text,lang?"鼠标左键":"Left Mouse"));
  dm_controls_label(512,text,sizeof text);CHECK(!strcmp(text,lang?"滚轮向上":"Wheel Up"));
  dm_controls_label(49,text,sizeof text);CHECK(!strcmp(text,lang?"空格":"Space"));
  int bindings[DM_CTL_COUNT][2];dm_controls_defaults(bindings);CHECK(dm_controls_validate(bindings,error,sizeof error));
  bindings[0][0]=54;CHECK(!dm_controls_validate(bindings,error,sizeof error));CHECK(strstr(error,lang?"无效":"Invalid binding"));
  dm_controls_defaults(bindings);bindings[0][0]=bindings[DM_CTL_A][0];CHECK(!dm_controls_validate(bindings,error,sizeof error));CHECK(strstr(error,lang?"已分配":"already assigned"));
  int mouse[2][2]={{256,-1},{257,-1}};CHECK(dm_controls_mouse_validate(mouse,NULL,error,sizeof error));mouse[0][0]=42;CHECK(!dm_controls_mouse_validate(mouse,NULL,error,sizeof error));CHECK(strstr(error,lang?"鼠标":"mouse"));
  CHECK(!strcmp(dm_ui("dmMacCtlCapturing"),lang?"正在分配“%@”。按下一个输入，或取消捕获。":"Assigning \"%@\". Press an input or cancel capture."));
  CHECK(!strcmp(dm_ui("dmMacGameControls"),lang?"游戏控制":"Game Controls"));
  CHECK(!strcmp(dm_ui("dmMacTextTitle"),lang?"文本":"Text"));
  NSString *resolution=[NSString stringWithFormat:@(dm_ui("dmMacResolutionInUse")),1920u,1080u,@(dm_ui("dmMacResolutionPending"))];
  CHECK([resolution isEqualToString:lang?@"当前使用：1920 × 1080 像素；更改已保存，重启后生效":@"In use: 1920 × 1080 pixels; change saved for restart"]);
  NSString *quit=[NSString stringWithFormat:@(dm_ui("dmMacQuitFormat")),@"D2R Mobile"];
  CHECK([quit isEqualToString:lang?@"退出 D2R Mobile":@"Quit D2R Mobile"]);
 }
 printf("CONTROLS I18N %d checks, %d failures\n",checks,failed);return failed!=0;
}}
