"""用合成玩家数据检查生产技能有效性判断，不需要游戏文件。
Exercise the production skill-ownership predicate with synthetic player data, without game files.
"""
from pathlib import Path
import os
import subprocess
import tempfile

src = Path(__file__).resolve().parents[1] / 'Sources/dm_touch_game_bridge.c'
text = src.read_text()
start = text.index('static int skill_live(')
opening = text.index('{', start)
depth = 1
end = opening + 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
predicate = text[start:end]
# 只替换两个客体函数入口，保留生产分支、链表遍历及等级判断。
# Substitute only the two guest call targets; keep production branches, list traversal and level checks.
for guest, stub in (
    ('((uint64_t(*)(uint32_t))(b+0x45f9d0))', 'player_lookup'),
    ('((int(*)(uint64_t,uint64_t,int))(b+0x5b1710))', 'equip_bonus'),
):
    assert predicate.count(guest) == 1, 'guest call seam changed: ' + guest
    predicate = predicate.replace(guest, stub)

code = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Alignas(uint64_t) static unsigned char player[0x200], list[16], node[0x80], row[8], config[0x1508];
static int cap=20, equipped;
static uint64_t player_lookup(uint32_t id) { (void)id; return (uint64_t)player; }
static int equip_bonus(uint64_t pl,uint64_t n,int x) { (void)pl;(void)n;(void)x;return 0; }
int dm_swapskill_equipped(int32_t item) { return item==42 && equipped; }
static void putptr(void *p,void *v) { memcpy(p,&v,8); }
''' + '\n' + predicate + r'''
static int count, bad;
static void check(const char *name,unsigned char *base,uint32_t id,int32_t item,int expected) {
    int actual=skill_live(base,id,item);
    count++;
    if(actual!=expected) { fprintf(stderr,"FAIL %s: got %d expected %d\n",name,actual,expected);bad++; }
}
int main(void) {
    size_t size=0x2d64a18+64;
    unsigned char *base=calloc(1,size);
    if(!base) {perror("calloc");return 2;}
    player[0x1bd]=1;putptr(player+0x100,list);putptr(base+0x2d64a18+16,config);putptr(config+0x1500,&cap);
    const char *names[]={"Interact","Loot","TownPortal","EmoteWheel","SwapWeapons","Map","ShowItems","RunToggle","CubeLoot"};
    const uint32_t ids[]={357,358,359,360,361,362,363,364,370};
    for(int i=0;i<9;i++) check(names[i],base,ids[i],-1,1);
    check("empty slot",base,UINT32_MAX,-1,1);
    check("missing learned skill",base,36,-1,0);
    check("removed item skill",base,36,42,0);
    equipped=1;check("still equipped offhand skill",base,36,42,1);equipped=0;
    check("unrelated monster skill",base,365,-1,0);
    check("unknown id",base,999,-1,0);
    putptr(list,node);putptr(node,row);*(int16_t *)row=36;*(int32_t *)(node+0x38)=1;*(int32_t *)(node+0x44)=-1;
    check("owned learned skill",base,36,-1,1);
    *(int32_t *)(node+0x38)=0;check("zero level learned skill",base,36,-1,0);
    free(base);
    printf("skill_live: %d/%d checks passed\n",count-bad,count);
    return bad?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='dm-skill-live-') as temp:
    source = Path(temp) / 't_skill_live.c'
    binary = Path(temp) / 't_skill_live'
    source.write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra',
                    '-Werror', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
