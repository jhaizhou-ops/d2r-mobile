#!/usr/bin/env python3
"""职业头像（桌面服务卡片用）：从玩家自己的游戏文件取原版 HIREABLES/<职业>icon.sprite（60×60，自带金框），放大到 120×120 写成
class_<职业序号>.png（序号同存档：0 亚马逊 … 7 术士）。产物不进仓库，由 dm/tools/d2rm_android.py 放进运行包 ui-art/。
用法：gen_class_portraits.py <原版 RomFS（Update）> <输出目录>
Class portraits for the home-screen service card: the original HIREABLES/<class>icon.sprite (60×60, with its own gold frame) from the
player's own game files, enlarged to 120×120 and written as class_<class index>.png (index as in the save: 0 Amazon … 7 Warlock). The
output is not kept in the repository; dm/tools/d2rm_android.py puts it into the runtime pack's ui-art/.
Usage: gen_class_portraits.py <original RomFS (Update)> <output directory>"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
import romfs_art

CLASSES = ('amazon', 'sorceress', 'necromancer', 'paladin', 'barbarian', 'druid', 'assassin', 'warlock')
SIZE = 120


def main(romfs, out):
    os.makedirs(out, exist_ok=True)
    for i, name in enumerate(CLASSES):
        im = romfs_art.sprite(romfs, 'hd/global/ui/HIREABLES/%sicon.sprite' % name).convert('RGBA')
        assert im.size == (60, 60), '%sicon 不是 60×60（原版资源变了？）| %sicon is not 60×60 (game data changed?)' % (name, name)
        im.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(out, 'class_%d.png' % i), optimize=True)
    print('class portraits: %d → %s' % (len(CLASSES), out))


if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
