#!/usr/bin/env python3
# 触摸界面的固定回归：逐界面逐项点一遍，每项一行「期望 → 真结果」。用法见同目录「usage.md」。
#   uireg.py <iphone|ipad|ipadfull> [局名…]      局 = a–k（ipadfull 另有 top）；不写局名 = 全部
# 起局由 UIREG_RUNNER 指的脚本负责（缺省同目录 run_mac.sh）：参数「<标签> <上限秒>」，触点脚本走标准输入，环境变量 PW_ENV（空格分隔的 K=V）、
#   PW_SAVE（换上的存档目录名，只有 d 局用）、UIREG_MODE（画面档，起局脚本据此挑包）。产物放到 $UIREG_OUT/pw/<标签>/：boot.txt、截图、timing.txt、c2s.jsonl。
# 点位：基准是 667×375 的 16:9 画面，按画面宽等比换算；裸坐标是「游戏视图内的点」（不是窗口点，整窗截图上量的位置要先减游戏视图原点）；
#   贴屏幕边的控件用 @名字 / tap_ui <控件名>，世界里的单位用 tap_unit，走位用 walk_to。
# 判据只读 boot.txt 里该项记号（note 项N）之后的日志；「未判」= 只留截图的项，写结论前要先看图。
# Fixed regression for the touch interface: tap through every item of every screen, one line of "expected → actual result" per item. Usage is in usage.md in the same directory.
#   uireg.py <iphone|ipad|ipadfull> [run names…]      run = a–k (ipadfull also has top); no run name = all
# Launching a run is done by the script UIREG_RUNNER points to (default run_mac.sh in the same directory): arguments "<tag> <limit seconds>", the touch point script goes through standard input, environment variables PW_ENV (space-separated K=V),
#   PW_SAVE (name of the save directory to swap in, used only by run d), UIREG_MODE (display mode, by which the run launcher script picks the app). Run output goes to $UIREG_OUT/pw/<tag>/: boot.txt, screenshots, timing.txt, c2s.jsonl.
# Tap positions: the base is a 667×375 16:9 display, converted proportionally by display width; bare coordinates are "points inside the game view" (not window points; positions measured on a whole-window screenshot must first have the game view origin subtracted);
#   widgets anchored to the screen edge use @name / tap_ui <widget name>, units in the world use tap_unit, movement uses walk_to.
# Criteria read only the log after that item's marker 「note 项N」(note item N) in boot.txt; 「未判」(not judged) = items that only keep a screenshot, look at the image before writing a conclusion.
import sys,os,re,subprocess,time
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # 工作区根目录（游戏文件、产物、运行现场都在它下面） | Workspace root (game files, build outputs and run artifacts are all under it)
MODE=sys.argv[1]; ONLY=sys.argv[2:]
VIEW={'iphone':(667,375,0,0),'ipad':(1366,768,0,0),'ipadfull':(1366,768,0,0)}[MODE]   # ipadfull = iPad 包加 DM_IPAD_FULL=1：窗口 1366×1024，游戏视图是贴底的 1366×768。脚本裸坐标是游戏视图内的点（不是窗口点），所以不加偏移；截图是整窗的，对截图量位置要减 256 点 | ipadfull = the iPad app plus DM_IPAD_FULL=1: window 1366×1024, the game view is a bottom-anchored 1366×768. Bare script coordinates are points inside the game view (not window points), so no offset is added; screenshots are of the whole window, so positions measured on a screenshot need 256 points subtracted
SCREEN=None   # UIREG_SCREEN=<宽>x<高>（点，只对 iPad 两档）：测试包的 DM_TEST_SCREEN 模拟屏——1366×1024 的窗口里居中摆一块这么大的屏，游戏视图在它里面（留边居中 / 真全屏贴底） | UIREG_SCREEN=<width>x<height> (points, only for the two iPad modes): the simulated screen of the test build's DM_TEST_SCREEN - a screen of this size is placed centered in the 1366×1024 window, with the game view inside it (letterbox centered / true fullscreen bottom-anchored)
if os.environ.get("UIREG_SCREEN") and MODE!="iphone": SCREEN=tuple(float(v) for v in os.environ["UIREG_SCREEN"].split("x")); VIEW=(SCREEN[0],SCREEN[0]*9/16,0,0)
SHOT_Y={}   # 截图名 → (拍这张时游戏视图的 y, 窗口宽)（窗口点）：iPad 真全屏开大面板时整块界面居中，视图 y 随面板开关在贴底与居中之间跳，按日志里拍图前最近一行「画面：游戏视图摆到 y N」取 | screenshot name → (the game view's y when this one was taken, window width) (window points): in iPad true fullscreen opening a large panel centers the whole interface block, and the view y jumps between bottom-anchored and centered as panels open and close; taken from the nearest log line 「画面：游戏视图摆到 y N」(display: game view placed at y N) before the shot
SHOT_BAD=[]   # 日志里「已截 X.png」但认不出局号前缀的截图名：这些图拿不到上面的纵向偏移，iPad 真全屏下读截图取格会取错位置（起局脚本把局号写成 p<时分秒>s<槽>，37 项静默错判） | screenshot names that appear in the log as 「已截 X.png」(captured X.png) but whose run-id prefix is not recognized: these images cannot get the vertical offset above, and in iPad true fullscreen reading cells from the screenshot takes the wrong position (the run launcher script wrote the run id as p<HHMMSS>s<slot>, 37 items silently misjudged)
def shot_ys(log):
    SHOT_Y.clear(); cur=None
    SHOT_BAD[:]=[n for n in re.findall(r"已截 (\S+?\.png)",log) if not re.match(r"[a-z]\d+_\S",n)]
    for m in re.finditer(r"游戏视图摆到 y (\d+) 高 \d+（窗口 (\d+)x|已截 [a-z]\d+_(\S+?\.png)",log):
        if m.group(1): cur=(int(m.group(1)),int(m.group(2)))
        elif cur: SHOT_Y[m.group(3)]=cur
def shot_geom(W,H,path=None):   # 整窗截图上：每个 667 基准点合多少像素、游戏视图原点的像素偏移 | on the whole-window screenshot: how many pixels each 667-base point is, and the pixel offset of the game view origin
    if MODE=="iphone": k=min(W/667.0,H/375.0); return k,(W-667*k)/2,0   # 真机屏比 16:9 宽：界面按高度撑满、左右居中（mini 上的 16:9 窗口偏移为 0） | the real device screen is wider than 16:9: the interface fills the height and is centered horizontally (the 16:9 window on the second Mac has offset 0)
    if not SCREEN: k,ox,oy=W/667.0,0,(H-W*9/16)*(1 if MODE=="ipadfull" else .5)
    else: pp=W/1366.0; dm,sh=SCREEN; k,ox,oy=pp*dm/667.0, pp*(1366-dm)/2, pp*((1024-sh)/2+(sh-dm*9/16)*(1 if MODE=="ipadfull" else .5))
    v=SHOT_Y.get(os.path.basename(path)) if path else None
    return k,ox,(v[0]*W/v[1] if v else oy)
K=VIEW[0]/667.0
# 暂停页按钮一律按控件名点（Settings / Exit / Chronicle @PauseLayoutGarden）：加了「一键重开」后五个按钮等距，原坐标 (333,185) 落在「一键重开」上
# Pause page buttons are always tapped by widget name (Settings / Exit / Chronicle @PauseLayoutGarden): after 「一键重开」(Quick Restart) was added the five buttons are evenly spaced, and the old coordinate (333,185) lands on Quick Restart
def P(x,y): return f"{x*K+VIEW[2]:.1f},{y*K+VIEW[3]:.1f}"
def O(dx,dy): return f"{dx*K:.0f},{dy*K:.0f}"
BASE="DM_SAVE_FRESH=warlock DM_GAME_SEED=0x12345678 DM_FIXED_TIME=1790000000 DM_C2S_LOG=pwc2s.jsonl DM_WORLD_DUMP=1 DM_LAT_LOG=1"+(" DM_IPAD_FULL=1" if MODE=="ipadfull" else " DM_IPAD_FULL=0" if MODE=="ipad" else "")+(f" DM_TEST_SCREEN={SCREEN[0]:.0f}x{SCREEN[1]:.0f}" if SCREEN else "")   #  iPad 包缺省真全屏，16:9 留边档要显式关 | the iPad app defaults to true fullscreen, the 16:9 letterbox mode has to turn it off explicitly
# 进局落在小站旁（产品缺省，dm_townwp.c）：整表各局缺省开着走产品落点（玩家缺省开着的功能回归局不许缺省关）。
# 第一批已改成走产品落点：d t u s sb vf2（起局带 DM_TEST_WORLD_GO=near2:2:c267@200 把人带到箱子旁，点箱子前 g.arrive(5808,4443,…)——从小站旁的新落点看不到箱子，直接点单位会「画面里没有这类单位」）。
# OLD_SPAWN = 暂时还关着它的局，【待改成产品落点】——不是豁免：这些局的起手按原版出生点写死了屏幕坐标，逐批改成「走到跟前再按单位点」后从表里拿掉，
# 最后只留专门验「关掉这个功能」的那一局。原因是两档实跑出来的（落点开着时这些局的第一步点空，后面连带不通）。
# Spawn next to the waypoint on joining a run (product default, dm_townwp.c): every run in the full table keeps it on by default and uses the
# product spawn point (a feature that is on by default for players must not be off by default in regression runs).
# The first batch already converted to the product spawn point: d t u s sb vf2 (launched with DM_TEST_WORLD_GO=near2:2:c267@200 to bring the
# character next to the chest, with g.arrive(5808,4443,...) before tapping the chest -- the chest is not visible from the new spawn point next
# to the waypoint, and tapping the unit directly gives "no unit of this kind on screen").
# OLD_SPAWN = runs that still have it turned off for now, [to be converted to the product spawn point] -- not an exemption: the opening steps
# of these runs hard-code screen coordinates based on the original spawn point; they are converted batch by batch to "walk up to it, then tap
# by unit" and removed from the table,
# until only the one run that specifically verifies "this feature turned off" remains. The reason comes from real runs in both profiles (with
# the spawn point on, the first tap of these runs hits nothing and everything after it fails in turn).
OLD_SPAWN={
 "n":"点阿卡拉的屏幕坐标","p":"点阿卡拉的屏幕坐标","b":"点瓦瑞夫 / 恰西的屏幕坐标","top":"iPad 档：开箱子、走到阿卡拉旁的点位","i":"iPad 档：点卡夏雇佣菜单的点位","pg2":"点箱子","rw2":"点箱子","vt1":"点箱子","vf1":"开箱","v":"建角后点箱子",
 "rw1":"长按物品栏里的软帽后清单落点","j":"背包拖放的落点（物品给在脚下的次序）","k":"背包拖放的落点","pk":"箱子选择框边缘的屏幕坐标","g4":"点凯恩的屏幕坐标","kbb":"主钮连点要怪在跟前",
}
if os.environ.get("UIREG_EXTRA_ENV"): BASE+=" "+os.environ["UIREG_EXTRA_ENV"].strip()   # 每局都加的环境变量（空格分隔的 K=V），如某条分支的核对开关 | environment variables added to every run (space-separated K=V), such as the verification switch of some branch
SAVE_TIME_MIN=1800000000   # 手柄选人的局若要固定时钟，取不早于它的值（标准存档里角色的时间戳都早于它） | If a controller-selection run wants a fixed clock, use a value no earlier than this (the timestamps of the characters in the standard save are all earlier than it)
WHO_ROW={1:"joe",2:"Warlock",3:"joebao",4:"法师"}   # 标准存档选角页自上而下四行的角色名 | Character names of the four rows, top to bottom, on the standard save's character select page
FORBID=os.environ.get("UIREG_FORBID")   # 正则：整局日志里出现就算不通（表里多一行「（整局）不该出现的日志」） | regex: appearing in the whole-run log counts as fail (one more row in the table: 「（整局）不该出现的日志」((whole run) log that should not appear))
REQUIRE=os.environ.get("UIREG_REQUIRE")   # 正则：整局日志里必须出现，结果里写出现了几次（核对开关真的生效了才算数） | regex: must appear in the whole-run log, and the result states how many times it appeared (it counts only when the verification switch has really taken effect)
# 红行按合入闸门的红线表数（dm/verify/logred.sh 同一张）：不只字面【红】，还有 FATAL、看门狗、命令缓冲错误、GPU 挂起 / 故障、管线失败—— 之前这里只数字面【红】，渲染层的红行不在口径里
# Failure lines are counted by the merge gate check's failure line table (the same one as dm/verify/logred.sh): not only the literal 【红】(red), but also FATAL, watchdog, command buffer errors, GPU hang / fault, pipeline failure - previously only the literal 【红】(red) was counted here, and the render layer's failure lines were not in scope
LOGRED=r"【红】|FATAL|看门狗|MTLCommandBufferStatus(Error|Fault)|CommandBufferCallbackError|Discarded \(victim|GPU (hang|error|fault|recovery)|GPU Hang|管线.*失败|[Pp]ipeline.*fail"
HERE=os.path.dirname(os.path.abspath(__file__))
RUNNER=os.environ.get("UIREG_RUNNER") or os.path.join(HERE,"run_mac.sh")
OUTBASE=(os.environ.get("UIREG_OUT") or os.path.join(DM_ROOT, "run/uireg"))
OUT=f"{OUTBASE}/tables/{MODE}_{time.strftime('%m%d_%H%M')}"; os.makedirs(OUT,exist_ok=True)
# NPC 对话菜单的行 = tap_unit 的单位中心 + 这里的偏移（视图点）。16:9 两种画面按宽等比；真全屏下世界铺满整窗、界面在贴底那块，菜单回到 NPC 上，但被游戏夹在界面矩形顶边，比 16:9 的相对位置低约 55 点，偏移单独量
# NPC dialog menu rows = the unit center from tap_unit + the offsets here (view points). The two 16:9 displays scale proportionally by width; in true fullscreen the world fills the whole window and the interface is in the bottom-anchored block, the menu goes back onto the NPC but is clamped by the game to the top edge of the interface rectangle, about 55 points lower than the 16:9 relative position, so the offsets are measured separately
MENU={"talk2":O(0,-24),"cancel2":O(0,13),"trade3":O(8,12),"kashya2":O(8,12),"akara2":O(8,12),"trade3m":O(8,12)} if MODE!="ipadfull" else {"akara2":"0,18","trade3m":"16,38","talk2":"6,6","cancel2":"6,83","trade3":"16,80","kashya2":"0,-46"}   # 真全屏下菜单贴着界面顶边排，行的位置不跟 NPC 走：卡夏比恰西离顶边远，她菜单的第二行在她身体中心上方约 46 点（四行、三行菜单的第二行位置相同） | in true fullscreen the menu is laid out against the top edge of the interface, and row positions do not follow the NPC: Kashya is farther from the top edge than Charsi, and the second row of her menu is about 46 points above her body center (the second row is at the same position in four-row and three-row menus)
IPAD=MODE.startswith("ipad")
LETTERBOX=MODE=="ipad"   # iPad 16:9 留边档：不是产品缺省，只跑与比例有关的子集。iPad 真全屏是缺省、有真玩家，跑全套 | iPad 16:9 letterbox mode: not the product default, runs only the aspect-ratio-related subset. iPad true fullscreen is the default and has real players, runs the full set
class G:
    def __init__(s,name,env="",save=""): s.name=name; s.run=name; s.env=env; s.save=save; s.who=None; s.padsel=False; s.t=3.5; s.L=[]; s.items=[]; s.n=0; s.cut=None
    def sect(s,name,gap=1.0,skip=False):
        if s.cut: s.L=s.L[:s.cut[0]]; s.items=s.items[:s.cut[1]]; s.t=s.cut[2]; s.cut=None   # 上一段标了 skip：整段撤掉（不能是一局的最后一段） | the previous section was marked skip: remove the whole section (it cannot be the last section of a run)
        if skip: s.cut=(len(s.L),len(s.items),s.t)
        s.name=name; s.n=0; s.t+=gap   # 同一局里接着走下一组（并局） | continue with the next group in the same run (merged run)
    def at(s,line,dt=0.0): s.L.append(f"{s.t:.2f} {line}"); s.t+=dt
    # 点完等 1.0 秒再截图 / 下一步
    # After a tap wait 1.0 second before the screenshot / next step
    def tap(s,spec,dt=1.0): s.at(f"down 0 {spec}"); s.t+=.12; s.at("up 0"); s.t+=dt-.12
    def drag(s,a,b,hold=.3): s.at(f"down 0 {a}"); s.t+=.04; s.at(f"move 0 {b}"); s.t+=hold; s.at("up 0"); s.t+=1.2
    def wait(s,dt): s.t+=dt
    def ui(s,name,dt=1.0): s.at(f"tap_ui {name} 3",dt)   # 按界面控件名点（贴屏幕边的控件：各种宽高比的屏位置不同，裸坐标换设备就偏） | tap by UI widget name (widgets anchored to the screen edge: their position differs across screen aspect ratios, and bare coordinates go off when the device changes)
    def dragp(s,a,b,pause=.3):   # 面板里的拖放：按下 → 中点 → 落点 → 松手 | drag-and-drop inside a panel: press → midpoint → drop point → release
        ax,ay=a; bx,by=b; s.at(f"down 0 {P(ax,ay)}",pause); s.at(f"move 0 {P((ax+bx)/2,(ay+by)/2)}",.3); s.at(f"move 0 {P(bx,by)}",.4); s.at("up 0",1.0)
    def clr(s): s.tap(P(502.5 if s.run in WIDE else 464,246.5),.7)   # 储藏箱开着的局：物品栏右移了 34.5 点（储藏箱加宽前后之别）   # 点一下背包里的空格（第 5 列第 2 行）：把光标挪开，物品说明框就不盖住要读的格子 | runs with the stash open: the inventory panel is shifted right by 34.5 points (the difference before and after the stash widening)   # tap an empty inventory cell (column 5, row 2): moves the cursor away so the item tooltip does not cover the cell to be read
    def dbl(s,x,y): s.tap(P(x,y),.25); s.tap(P(x,y),1.0)
    def fast(s,a,b):   # 快松：按下 40 毫秒后一步移到落点、再 10 毫秒松手（抬手时宿主的落点答案多半还没回，前端要挂着等它） | quick release: 40 milliseconds after press move to the drop point in one step, then release 10 milliseconds later (at lift-off the host's drop point answer has most likely not come back yet, and the front end has to hold and wait for it)
        s.at(f"down 0 {P(*a)}",.04); s.at(f"move 0 {P(*b)}",.01); s.at("up 0",1.6)
    def hold(s,x,y,sec=1.3): s.at(f"down 0 {P(x,y)}",sec); s.at("up 0",1.0)
    def item(s,title,expect,pat,neg=None):   # 在此之后做动作；pat = 期望在日志里出现的正则 | actions are done after this; pat = the regex expected to appear in the log
        s.at(f"note 项{len(s.items)}"); s.items.append([title,expect,pat,neg,"",s.name]); s.t+=.05
    def shot(s): n=f"{s.name}_{s.n:02d}.png"; s.n+=1; s.at(f"shot {n}",.3); s.items[-1][4]=n
    def enter(s,row=2):   # 标准存档：第 2 行 1 级术士；第 1 行 76 级圣骑（要点难度） | standard save: row 2 level 1 warlock; row 1 level 76 paladin (needs a difficulty tap)
        if not s.save and not s.who: s.who=WHO_ROW.get(row)   # 这一局该进的角色（整局判据：进局日志里的角色名必须是它） | The character this run should enter (whole-run criterion: the character name in the run-entry log must be this one)
        if row!=1: s.ui(f"ListItem{row-1}@CharacterSelectPanel",1.3)
        s.tap(P(333,336),1.6)
        if row!=2: s.tap(P(333,157),1.0)
        s.at("await pos"); s.t=max(s.t,13.0); s.at("note 进局",1.5)   # 玩家状态可读就把时钟拨到这里（上限第 13 秒），再留 1.5 秒站定 | once the player state is readable advance the clock to here (limit: second 13), then leave 1.5 seconds to settle
    def arrive(s,x,y,r,upto): s.at(f"await pos {x} {y} {r}"); s.t=max(s.t,upto); s.at("note 到位",1.0)   # 等测试开关把人带到 (x,y) 附近；upto = 最晚等到脚本第几秒 | wait for the test switch to bring the player near (x,y); upto = the latest script second to wait until
# 页签类的判据一律认「界面页签：切 ±N 页」（真的切了），不认「界面命中 → 页签」（只是命中）：真机上有过命中了没切、后面连带错位却报通的
# 绑定框（重新绑定技能）：两排五列各格的中心与框顶那个大图标（= 长按的技能）的位置，667 宽基准、原样档位的画面。
# 读「长按的这个技能现在在哪一格」：把每格的图标和框顶大图标都缩成 12×12 灰度、去均值归一后求相关——同一个技能 0.73 以上，别的技能 0.43 以下，阈值取 0.6，空格子对比度不够直接不算。
# 为什么不用日志：宿主打的「绑定框里点了第 R 排第 C 列 / 已绑到键」是我们自己的过程日志，技能实际落进别的格子时它照样打（第 3、4 列与手柄键对反那次就是这样漏掉的）。
# 这条规则在真坏例上验过：旧包点 1 排 3 列后读出技能在 1 排 4 列、再点 4 列读出在 3 列；修后的包读出的都是点的那一格。
# Tab criteria always accept 「界面页签：切 ±N 页」(UI tab: switch ±N pages) (really switched), not 「界面命中 → 页签」(UI hit → tab) (only a hit): on a real device there was a case that hit without switching, misaligned everything after it, and still reported pass
# Binding box (rebind skill): the centers of the cells in two rows of five columns and the position of the big icon at the top of the box (= the long-pressed skill), 667-wide base, display in the as-is profile.
# Reading "which cell is the long-pressed skill in now": shrink each cell's icon and the big icon at the top of the box to 12×12 grayscale, remove the mean and normalize, then correlate - the same skill is 0.73 or above, other skills 0.43 or below, the threshold is 0.6, and empty cells without enough contrast are simply not counted.
# Why the log is not used: the host's 「绑定框里点了第 R 排第 C 列 / 已绑到键」(tapped row R column C in the binding box / bound to key) is our own process log, and it is printed all the same when the skill actually lands in another cell (that is how the time columns 3 and 4 were swapped against the controller keys slipped through).
# This rule was verified on a real bad case: on the old build, after tapping row 1 column 3 the skill read as in row 1 column 4, and after tapping column 4 it read as in column 3; on the fixed build every reading is the tapped cell.
BIND_COLS=(275,298,321,344,368); BIND_ROWS=(202,235); BIND_TOP=(333,167)
def bind_cells(path):   # 返回与框顶大图标是同一个技能的格子集合；读不了返回 None | returns the set of cells holding the same skill as the big icon at the top of the box; returns None when unreadable
    try: from PIL import Image
    except ImportError: return None
    import math
    im=Image.open(path).convert("L"); W,H=im.size; k,ox,oy=shot_geom(W,H,path)   # 截图是整窗的：留边档游戏视图居中，真全屏贴底 | the screenshot is of the whole window: in letterbox mode the game view is centered, in true fullscreen it is anchored to the bottom
    def sig(x,y):
        cx,cy,h=x*k+ox,y*k+oy,7*k; v=list(im.crop((int(cx-h),int(cy-h),int(cx+h),int(cy+h))).resize((12,12)).tobytes())
        m=sum(v)/len(v); v=[a-m for a in v]; n=math.sqrt(sum(a*a for a in v)); return ([a/(n or 1) for a in v], n/12)
    ts,tn=sig(*BIND_TOP)
    if tn<20: return None   # 框顶没有图标：框没开，或者画面档位不是这套坐标 | no icon at the top of the box: the box is not open, or the display profile is not this set of coordinates
    on=set()
    for r,y in enumerate(BIND_ROWS,1):
        for c,x in enumerate(BIND_COLS,1):
            v,n=sig(x,y)
            if n>20 and sum(a*b for a,b in zip(ts,v))>0.6: on.add((r,c))   # 实测：同一个技能 0.73–0.98（第 2 排第 1 列旁边有个换组小图标，拉低到 0.73），别的技能最高 0.43 | measured: the same skill 0.73–0.98 (next to row 2 column 1 there is a small set-switch icon that pulls it down to 0.73), other skills at most 0.43
    return on
def bind_check(d,prev,cur,cell):   # 点了 cell 之后再开框：这个技能要在 cell 上，且没有落到别的新格子 | open the box again after tapping cell: this skill must be on cell, and must not have landed on any other new cell
    a=bind_cells(f"{d}/{prev}"); b=bind_cells(f"{d}/{cur}")
    if a is None or b is None: return "未判：读不出绑定框（本机没有 PIL，或框没开 / 画面档位不是这套坐标）"
    fmt=lambda S: "、".join(f"{r} 排 {c} 列" for r,c in sorted(S)) or "没有"
    if cell not in b: return f"不通：点的是 {cell[0]} 排 {cell[1]} 列，技能却在 {fmt(b)}（点之前在 {fmt(a)}）"
    extra=b-a-{cell}
    if extra: return f"不通：技能到了点的那格，但 {fmt(extra)} 也新出现了它"
    return f"通：技能在 {fmt(b)}（点之前在 {fmt(a)}）"
def spot_filled(path,x,y):   # 截图上以 (x,y)（667 宽基准的视图点）为中心 ±6 点的灰度标准差：空的背包格是一片深色（实测 2 上下），有物品图标在 40 以上；(x,y) 必须是格子正中心 | grayscale standard deviation over ±6 points centered on (x,y) (view points on the 667-wide base) in the screenshot: an empty inventory cell is a dark patch (measured around 2), with an item icon it is above 40; (x,y) must be the exact cell center
    try: from PIL import Image, ImageStat
    except ImportError: return None
    im=Image.open(path).convert("L"); W,H=im.size; k,ox,oy=shot_geom(W,H,path); cx,cy,h=x*k+ox,y*k+oy,6*k
    return ImageStat.Stat(im.crop((int(cx-h),int(cy-h),int(cx+h),int(cy+h)))).stddev[0]>15
INV_X0,INV_DX,INV_Y0,INV_DY=386.0,19.55,227.0,19.5   # 箱子开着时右边物品栏的背包格（10 列 4 行）中心，667 宽基准 | centers of the inventory cells (10 columns, 4 rows) of the inventory panel on the right when the stash is open, 667-wide base
def inv_count(path):   # 背包里有东西的格子数；读不了返回 None | number of occupied inventory cells; returns None when unreadable
    n=0
    for r in range(4):
        for k in range(10):
            f=spot_filled(path,INV_X0+INV_DX*k,INV_Y0+INV_DY*r)
            if f is None: return None
            n+=f
    return n
def item_changes(d,cls):   # 物品表状态日志里，类号 cls 那件「开局后先出现在背包里的」物品的全部状态变化 [(拍, (mode,page,x,y))]；物品没了记 mode −1。没有日志返回 None | in the item table state log, all state changes [(tick, (mode,page,x,y))] of the item of class number cls that "first appears in the inventory after the start"; an item that is gone is recorded as mode −1. Returns None when there is no log
    import json
    try: rows=[json.loads(l) for l in open(f"{d}/state.jsonl") if l.strip()]
    except (OSError,ValueError): return None
    iid=None; seq=[]
    for r in rows:
        its={i["id"]:i for i in r.get("items",[])}
        if iid is None:
            c=[i for i in r.get("items",[]) if i["cls"]==cls and i["mode"]==0 and i["page"]==0]
            if not c: continue
            iid=c[0]["id"]
        i=its.get(iid); st=(i["mode"],i["page"],i["x"],i["y"]) if i else (-1,0,0,0)
        if not seq or seq[-1][1]!=st: seq.append((r["tick"],st))
    return seq
def stable_segs(d,cls,thr=50):   # 上面那串变化压成停留段 [(mode,page,x,y,停留拍数,起始拍)]，只留停留 ≥ thr 拍的段与最后一段。mode 0 = 在容器里（page 0 背包），4 = 在手上——物品放进单件槽在原版里就是「在手上」； | compress the series of changes above into dwell segments [(mode,page,x,y,dwell ticks,start tick)], keeping only segments that dwell ≥ thr ticks and the last segment. mode 0 = in a container (page 0 inventory), 4 = in hand - an item placed into the single-item slot is "in hand" in the original game;
    # 拖动途中也是 4，所以不看瞬时值：每个动作做完停 3 秒，动作途中那不到 2 秒的 4 不算一段
    # It is also 4 while being dragged, so the instantaneous value is not used: after each action pause 3 seconds, and the under-2-second 4 during the action does not count as a segment
    seq=item_changes(d,cls)
    if seq is None: return None
    out=[]
    for k,(t,st) in enumerate(seq):
        dur=(seq[k+1][0]-t) if k+1<len(seq) else 10**6
        if dur>=thr: out.append(st+(dur,t))
    return out
def held_segs(d,thr=50):   # 物品表里「某件物品稳定地在手上（= 在单件槽里）」的各段 [(物品 id, 类号, 起始拍, 停留拍数)]，按起始拍排；不指定是哪件物品（货架上现买的那种，事先不知道类号） | the segments [(item id, class number, start tick, dwell ticks)] in the item table where "some item is stably in hand (= in the single-item slot)", sorted by start tick; which item is not specified (the kind bought off the shelf on the spot, whose class number is not known in advance)
    import json
    try: rows=[json.loads(l) for l in open(f"{d}/state.jsonl") if l.strip()]
    except (OSError,ValueError): return None
    cur={}; out=[]
    for r in rows:
        now={i["id"]:i["cls"] for i in r.get("items",[]) if i["mode"]==4}
        for k in list(cur):
            if k not in now: c,t0=cur.pop(k); (r["tick"]-t0>=thr) and out.append((k,c,t0,r["tick"]-t0))
        for k,c in now.items(): cur.setdefault(k,(c,r["tick"]))
    for k,(c,t0) in cur.items(): out.append((k,c,t0,10**6))
    return sorted(out,key=lambda x:x[2])
def seg_txt(sg): return ({0:"背包",3:"魔盒",4:"箱子"}.get(sg[1],f"页 {sg[1]}") if sg[0]==0 else {4:"手上 / 槽里",-1:"没了",1:"身上",2:"腰带"}.get(sg[0],f"mode {sg[0]}"))+(f" 格 ({sg[2]},{sg[3]})" if sg[0]==0 else "")
def mana(txt):
    m=re.findall(r"法力 (\d+)/\d+",txt); return [int(x) for x in m]
def cast(s,before):   # 施放：本段内法力读数低于动作前最后一次读数 | cast: within this segment the mana reading is lower than the last reading before the action
    b=mana(before); a=mana(s)
    if not b or not a: return "未判：没有法力读数"
    return f"通：法力 {b[-1]} → {min(a)}" if min(a)<b[-1] else f"不通：法力没降（{b[-1]} → {min(a)}）"
def nocast(s,before):
    b=mana(before); a=mana(s)
    if "点空技能位" in s: return "通：开技能树（点空技能位）"
    if not b: return "未判：没有法力读数"
    return f"不通：法力降了（{b[-1]} → {min(a)}）" if a and min(a)<b[-1] else "通：没有施放（法力没降）"
STATPAT=r"DM_TEST_STAT_LOG：拍 \d+[^\n]*? 14=(\d+)"
def gold(chg,neg=None):   # 身上金币（属性 14，DM_TEST_STAT_LOG=14 记的游戏值）：chg = '+' 增 / '-' 减 / 整数 = 差正好是它（0 不变）。卖出时另核金币差 = 游戏给的卖价 | gold carried (stat 14, the game value recorded by DM_TEST_STAT_LOG=14): chg = '+' increase / '-' decrease / integer = the difference is exactly that (0 unchanged). On a sale also check that the gold difference = the sell price given by the game
    def f(s,before):
        b=re.findall(STATPAT,before); a=re.findall(STATPAT,s)
        if not b: return "未判：动作前没有金币读数"
        g0=int(b[-1]); g1=int(a[-1]) if a else g0; d=g1-g0
        r=("通：" if ((d>0) if chg=='+' else (d<0) if chg=='-' else d==chg) else "不通：")+f"金币 {g0} → {g1}"
        m=[int(x) for x in re.findall(r"卖手上的物品[^\n]*卖价 (-?\d+)[^\n]*→ 1（?",s)]
        if chg=='+' and m and d!=sum(m): r=f"不通：金币差 {d} 不等于游戏给的卖价 {m}；"+r.split("：",1)[1]
        if neg and re.search(neg,s): r="不通：出现「"+re.search(neg,s).group(0)[:20]+"」；"+r.split("：",1)[1]
        return r
    return f
def php(chg):   # 玩家生命（dm_hud 行）：'+' 涨 / 0 不变。药是逐拍回的，取本段最后一个读数 | player life (dm_hud line): '+' rises / 0 unchanged. Potions restore tick by tick, so take the last reading in this segment
    def f(s,before):
        pat=r"dm_hud：区域 [1-9]\d* [^\n]*生命 (\d+)/"; b=re.findall(pat,before); a=re.findall(pat,s)
        if not b: return "未判：动作前没有生命读数"
        h0=int(b[-1]); h1=int(a[-1]) if a else h0
        return ("通：" if ((h1>h0) if chg=='+' else h1==h0) else "不通：")+f"玩家生命 {h0} → {h1}"
    return f
def dur(want,cap=86):   # 身上那件耐久上限为 cap 的装备（88 级法师标准档里只有盾是 86）现在的耐久 = want。读 DM_TEST_DUR 的记录行（更早的包没有这行，报未判） | the current durability of the equipped item whose durability cap is cap (in the level 88 sorceress standard save only the shield is 86) = want. Reads the DM_TEST_DUR record line (earlier builds do not have this line, reports not judged)
    def f(s,before):
        a=re.findall(r"DM_TEST_DUR：[^\n]*耐久 (\d+)/%d\b"%cap,before+s)
        if not a: return "未判：没有耐久读数（包里没有 DM_TEST_DUR，看截图说明框）"
        return ("通：" if int(a[-1])==want else "不通：")+f"盾耐久 {a[-1]}/{cap}"
    return f
def merc(chg):   # 佣兵生命（DM_TEST_MERC_HP 的记录行）：'+' 涨 / 0 不变 | mercenary life (the DM_TEST_MERC_HP record line): '+' rises / 0 unchanged
    def f(s,before):
        pat=r"DM_TEST_MERC_HP：[^\n]*生命 (\d+)/(\d+)"; b=re.findall(pat,before); a=re.findall(pat,s)
        if not b: return "未判：没有佣兵生命读数（包里没有 DM_TEST_MERC_HP）"
        h0=int(b[-1][0]); h1=int(a[-1][0]) if a else h0
        return ("通：" if ((h1>h0) if chg=='+' else h1==h0) else "不通：")+f"佣兵生命 {h0} → {h1}（上限 {b[-1][1]}）"
    return f
def belt(chg):   # 腰带里的瓶数（dm_hud 行四列之和）：'+' 增 / '-' 减 / 0 不变 | number of potions in the belt (sum of the four columns on the dm_hud line): '+' increase / '-' decrease / 0 unchanged
    def f(s,before):
        pat=r"腰带 (\d+)/(\d+)/(\d+)/(\d+)"; b=re.findall(pat,before); a=re.findall(pat,s)
        if not b: return "未判：动作前没有腰带读数"
        n0=sum(map(int,b[-1])); last=a[-1] if a else b[-1]; n1=sum(map(int,last))
        return ("通：" if ((n1>n0) if chg=='+' else (n1<n0) if chg=='-' else n1==n0) else "不通：")+f"腰带 {'/'.join(b[-1])} → {'/'.join(last)}"
    return f
def merge(*rs):   # 几条判据并成一条：有不通算不通，否则有未判算未判 | merge several criteria into one: any fail counts as fail, otherwise any not judged counts as not judged
    st="不通" if any(r.startswith("不通") for r in rs) else "未判" if any(r.startswith("未判") for r in rs) else "通"
    return st+"："+"；".join(r.split("：",1)[1] for r in rs)
def both(*fs): return lambda s,before: merge(*[f(s,before) for f in fs])
def two_names(s,before):   # 连建两个留空名字的角色：两次最终填进名字框的随机名不同（撞名时宿主再取，那几行带「再取一个」，不算最终名） | create two characters in a row with the name left blank: the random names finally filled into the name box the two times differ (on a name collision the host takes another; those lines carry 「再取一个」(take another one) and do not count as the final name)
    L=re.findall(r"随机名 → 名字框「([^」]*)」([^\n]*)",before+s); fin=[n for n,t in L if "再取一个" not in t]
    if any("【红】" in t for n,t in L): return "不通：试满次数仍重名"
    if len(fin)!=2: return f"不通：最终名字应有 2 个，读到 {fin}"
    return ("通：" if fin[0]!=fin[1] and all(fin) else "不通：")+f"两个名字「{fin[0]}」「{fin[1]}」"+("（第二次先取到重名、再取过）" if len(L)>2 else "")
def menu(k,res=None):   # 点 NPC 对话菜单第 k 行（0 起）：宿主记的「N 行，点中第 K 行（ListItemK），选中行 M（菜单自己记的）」里 K 得是 k，且出了期望的结果消息；出现补挪 / 结构对不上 / 不按确认算不通。 | tap row k (0-based) of the NPC dialog menu: in the host's log 「N 行，点中第 K 行（ListItemK），选中行 M（菜单自己记的）」(N rows, hit row K (ListItemK), selected row M (recorded by the menu itself)), K must be k, and the expected result message appeared; an extra move / structure mismatch / confirm not pressed counts as fail.
    def f(s,before):                # 没有菜单改动的旧包不打这行：那时只看结果消息，并在结果里写明 | old builds without the menu change do not print this line: then only the result message is checked, and this is stated in the result
        bad=re.search(r"补挪|列表结构对不上|不按确认",s)
        if bad: return "不通：出现「"+bad.group(0)+"」"
        if "tap_ui 超时" in s or re.search(r"tap_ui[^\n]*(没找到|超时)",s): return "不通：按控件名没点到这一行（菜单没开？）"
        m=re.search(r"界面对话菜单：(\d+) 行，点中第 (\d+) 行[^\n]*选中行 (\d+)",s); got=bool(res is None or re.search(res,s))
        if not m: return ("通：" if got and res else "未判：" if got else "不通：")+"没有「点中第 N 行」日志（旧包不打）"+("，结果消息对" if got and res else "" if got else "，也没有期望的结果消息")
        ok=int(m.group(2))==k   # 「选中行 M」是点之前菜单停在哪一行（宿主据此挪 K − M 行再按确认），不是结果 | 「选中行 M」(selected row M) is the row the menu was on before the tap (from which the host moves K − M rows and then presses confirm), not the result
        return ("通：" if ok and got else "不通：")+f"{m.group(1)} 行菜单，点中第 {m.group(2)} 行（点之前停在第 {m.group(3)} 行）"+("" if got else "；没有期望的结果消息")
    return f
def edge_idle(s,before):   # 底边那一条起手：整次触摸都不当摇杆——move 那行轴为 0，且前后两次 pos 相同 | touch starting in the bottom-edge strip: the whole touch is not treated as the stick - the axes on the move line are 0, and the pos before and after are the same
    if not re.search(r"\[script\] move slot=1[^\n]*lx=0 ly=0",s): return "不通：move 那行的轴不是 0"
    return stay(s,before)
def moved(s,before):   # 人走动了：本段第一条与最后一条 pos 不同 | the player moved: the first and last pos of this segment differ
    p=re.findall(r"pos 区域 \d+ \((\d+),(\d+)\)",s)
    if len(p)<2: return "未判：没有两条坐标读数"
    return f"通：{p[0]} → {p[-1]}" if p[0]!=p[-1] else f"不通：坐标没变 {p[0]}"
def stay(s,before):   # 人不离开原地：本段第一条与最后一条 pos 相同 | the player does not leave the spot: the first and last pos of this segment are the same
    p=re.findall(r"pos 区域 (\d+ \(\d+,\d+\))",s)
    if len(p)<2: return "未判：没有两条坐标读数"
    return f"通：坐标不变 {p[0]}" if p[0]==p[-1] else f"不通：{p[0]} → {p[-1]}"
# 储藏箱加宽（10 → 14 列，挪面板乘面板缩放）后储藏箱开着时：储藏箱一侧比原版 x −39（个人页签 62.5、共享 106.5、个人页第 0 列 60）、
# 物品栏一侧 +38.5（背包第 0 列 424.5）、关闭钮 654.5、取金币钮 187.5；宝石 / 材料 / 符文 / 魔盒四页回到原版位置。这几局（与 top / v 局开箱那几步）的点位都按这个量过
# After the stash widening (10 → 14 columns, the panel shift multiplied by the panel scale), with the stash open: the stash side is x −39 from the original game (personal tab 62.5, shared 106.5, personal page column 0 at 60),
# the inventory panel side is +38.5 (inventory column 0 at 424.5), close button 654.5, withdraw gold button 187.5; the four pages gems / materials / runes / cube are back at the original game's positions. The tap positions of these runs (and of the stash-opening steps of the top / v runs) were all measured this way
WIDE=("c","d","t","u","s","sa")
def run(g):
    global INV_X0; INV_X0=424.5 if g.run in WIDE else 386.0   # 背包占格的读法跟着物品栏右移 | the inventory occupancy reading follows the inventory panel's shift to the right
    script="\n".join(g.L)+f"\n{g.t+1.0:.2f} note 结束\n"; cap=int(g.t)+6; tag=f"{os.environ.get('UIREG_TAG') or 'ur'}_{MODE}_{g.run}"   # 起局脚本见到「note 结束」就收，cap 只是上限 | the run launcher script stops as soon as it sees 「note 结束」(note end); cap is only an upper limit
    # 手柄在角色列表里选人的局（g.padsel）：原版确认选人时给角色打「现在」的时间戳、列表按时间戳重排后取第一行；固定时钟早于存档时间时被选的反而排到后面、进的不是高亮的角色
    # ——是量具造出来的假缺陷（2026-10-12）。这类局起局环境里的固定时钟自动去掉（用真实时钟）；局自己又带了早于存档时间的固定时钟就直接报错不跑。
    # Runs where the controller selects a character in the character list (g.padsel): on confirming a selection the original game stamps the character with the "now" timestamp, re-sorts the list by timestamp and takes the first row; when a fixed clock is earlier than the save times the selected one sorts further back instead, and the character entered is not the highlighted one
    # -- a false defect created by the measuring tool (2026-10-12). For such runs the fixed clock is automatically removed from the run's start environment (the real clock is used); if the run itself also sets a fixed clock earlier than the save times, it errors out directly without running.
    base=BASE if not g.save else BASE.replace('DM_SAVE_FRESH=warlock ','')
    if g.padsel:
        base=re.sub(r" ?DM_FIXED_TIME=\d+","",base); ft=re.search(r"DM_FIXED_TIME=(\d+)",g.env)
        if ft and int(ft.group(1))<SAVE_TIME_MIN: raise SystemExit(f"局 {g.run}：手柄选人的局不许带早于存档时间的固定时钟（DM_FIXED_TIME={ft.group(1)} < {SAVE_TIME_MIN}）")
    env=dict(os.environ,UIREG_MODE=MODE,UIREG_OUT=OUTBASE,PW_SAVE=g.save,PW_ENV=f"{base}{' DM_TOWNWP_OFF=1' if g.run in OLD_SPAWN else ''} {g.env}".strip())
    t0=time.time(); subprocess.run(["bash",RUNNER,tag,str(cap)],input=script.encode(),env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    d=f"{OUTBASE}/pw/{tag}"; wall=time.time()-t0
    try: log=open(f"{d}/boot.txt",errors='replace').read()
    except OSError: log=""
    shot_ys(log)
    parts=re.split(r"\[script\] note 项(\d+)",log); seg={int(parts[i]):parts[i+1] for i in range(1,len(parts)-1,2)}
    rows=[]
    for i,it in enumerate(g.items):
        title,expect,pat,neg=it[:4]; shot=it[4]; s=seg.get(i,"")
        if neg=='late': s=s+seg.get(i+1,''); neg=None; it[3]=None
        before="".join(seg.get(j,"") for j in range(i)) if i else parts[0]
        if isinstance(pat,tuple) and pat and pat[0] in ("seg","segnot","segpage","bag","still"):   # 物品写的是物品码（lgl）时，从日志里测试开关那行换成类号 | when the item is written as an item code (lgl), convert it to the class number using the test switch line in the log
            def cls_of(c):
                if isinstance(c,int): return c
                m=re.search(r"DM_TEST_GIVE：[^\n]*生成「"+c+r"」[^\n]*物品类号 (\d+)",log); return int(m.group(1)) if m else -1
            pat=tuple(cls_of(x) if k in (1,3) and isinstance(x,str) and pat[0]=="still" or (k==1 and isinstance(x,str)) else x for k,x in enumerate(pat))
        if i not in seg: res="没执行到"
        elif callable(pat): res=pat(s,parts[0]+before)
        elif isinstance(pat,tuple) and pat[0]=="spot":   # ("spot","empty"|"filled",(x,y))：这一项的截图里那个格子空 / 不空 | ("spot","empty"|"filled",(x,y)): in this item's screenshot that cell is empty / not empty
            try: f=spot_filled(f"{d}/{shot}",*pat[2])
            except OSError: f=None
            res="未判：读不了截图" if f is None else ("通：" if f==(pat[1]=="filled") else "不通：")+f"({pat[2][0]},{pat[2][1]}) 那一格{'有东西' if f else '是空的'}"
        elif isinstance(pat,tuple) and pat[0]=="inv":   # ("inv",±N)：背包里有东西的格子数比上一项的截图多 / 少 N 格（东西真到了背包 / 真离开了背包，不认「发了包」） | ("inv",±N): the number of occupied inventory cells is N more / fewer than in the previous item's screenshot (the thing really reached / really left the inventory, "a packet was sent" is not accepted)
            try: n0=inv_count(f"{d}/{g.items[i-1][4]}"); n1=inv_count(f"{d}/{shot}")
            except OSError: n0=n1=None
            res="未判：读不了截图" if n0 is None or n1 is None else ("通：" if (n1<n0 if pat[1]=='-' else n1>n0 if pat[1]=='+' else n1-n0==pat[1]) else "不通：")+f"背包占格 {n0} → {n1}"   # '-' = 只判减少：多格物品（软帽 2×2）有的格图案暗、读不成「有东西」，格数差不准 | '-' = judge decrease only: for multi-cell items (cap 2×2) some cells have a dark image and do not read as "has something", so the cell count difference is inaccurate
            if len(pat)>2 and callable(pat[2]): res=merge(res,pat[2](s,parts[0]+before))
            elif len(pat)>2 and not re.search(pat[2],s): res="不通：这一步的触摸没有走到物品手势（日志里没有「"+pat[2]+"」"+("，走的是「界面鼠标：点击」" if "界面鼠标：点击" in s else "")+"）；"+res   # 11 寸 iPad 上有过：点成叠格全走了鼠标路，一次命中都没有。注意宿主的「界面命中」行有限频（同一答案 1 秒内至多 4 条；更早的包是每进程只打前 400 条）：这条判据只用在每步点按不超过 4 下的短局里，没有这行不一定代表没命中 | this happened on the 11-inch iPad: taps on stack cells all went down the mouse path, without a single hit. Note that the host's 「界面命中」(UI hit) line is rate-limited (at most 4 lines within 1 second for the same answer; earlier builds printed only the first 400 per process): this criterion is used only in short runs with no more than 4 taps per step, and the absence of this line does not necessarily mean no hit
        elif isinstance(pat,tuple) and pat and pat[0]=="still":   # ("still",A,n,B[,日志正则])：A 的第 n 个停留段期间，B 那件物品在物品表里没有任何变化（连短暂拿起都没有） | ("still",A,n,B[,log regex]): during A's n-th dwell segment, item B has no change at all in the item table (not even a brief pickup)
            sa=stable_segs(d,pat[1]); cb=item_changes(d,pat[3])
            if sa is None or cb is None: res="未判：没有物品表状态日志"
            elif len(sa)<=pat[2]: res=f"不通：前一件物品没有第 {pat[2]} 个停留段"
            else:
                t0=sa[pat[2]][5]; t1=t0+sa[pat[2]][4]; hit=[(t,st) for t,st in cb if t0<t<t1]
                res=("通：" if not hit and sa[pat[2]][0]==4 else "不通：")+(f"槽里那件拍 {t0} 起一直在槽里；另一件这期间没动过" if not hit else f"另一件在拍 {hit[0][0]} 动了（→ {seg_txt(hit[0][1]+(0,0))}）")
                if len(pat)>4 and not re.search(pat[4],s): res="不通：没有期望的日志「"+pat[4]+"」；"+res.split("：",1)[1]
        elif isinstance(pat,tuple) and pat[0]=="held":   # ("held",n[,"ended"][,日志正则])：物品表里第 n 次有物品稳定地在槽里；"ended" = 这一段已经结束（物品离开了槽：成交换成新物品，或回了背包） | ("held",n[,"ended"][,log regex]): the n-th time the item table has an item stably in the slot; "ended" = this segment has ended (the item left the slot: the deal replaced it with a new item, or it went back to the inventory)
            hs=held_segs(d); txt="、".join(f"类号 {c} 自拍 {t0} 起{'一直在' if du>=10**6 else f'在了 {du} 拍'}" for k,c,t0,du in hs) if hs else "没有"
            if hs is None: res="未判：没有物品表状态日志"
            elif len(hs)<=pat[1]: res=f"不通：没有第 {pat[1]} 次进槽；进过槽的：{txt}"
            else:
                k,c,t0,du=hs[pat[1]]; ended="ended" in pat; ok=(du<10**6) if ended else True
                res=("通：" if ok else "不通：")+f"类号 {c} 的物品自拍 {t0} 起在槽里"+(f"，{du} 拍后离开" if du<10**6 else "，到局末还在")
            extra=[x for x in pat[2:] if isinstance(x,str) and x!="ended"]
            if extra and not re.search(extra[0],s): res="不通：没有期望的日志「"+extra[0]+"」；"+res.split("：",1)[1]
        elif isinstance(pat,tuple) and pat[0]=="segpage":
            sg=stable_segs(d,pat[1],thr=30); allt="、".join(seg_txt(x) for x in sg) if sg else "没有"
            if sg is None: res="未判：没有物品表状态日志"
            elif len(sg)<=pat[2]: res=f"不通：这一步之后物品没有新的停留段（应是第 {pat[2]} 段）；各停留段：{allt}"
            else: x=sg[pat[2]]; res=("通：" if x[0]==0 and x[1]==pat[3] else "不通：")+f"第 {pat[2]} 段 {seg_txt(x)}"+("" if x[0]==0 and x[1]==pat[3] else f"；各停留段：{allt}")
        elif isinstance(pat,tuple) and pat[0] in ("seg","segnot"):   # ("seg",类号,n,mode[,"same"])：那件物品第 n 个停留段（0 = 开局在背包）的 mode；"same" = 与上一段同格。("segnot",类号,mode)：整局没有这种 mode 的停留段 | ("seg",class number,n,mode[,"same"]): the mode of that item's n-th dwell segment (0 = in the inventory at the start); "same" = same cell as the previous segment. ("segnot",class number,mode): the whole run has no dwell segment of this mode
            sg=stable_segs(d,pat[1]); allt="、".join(seg_txt(x) for x in sg) if sg else "没有"
            if sg is None: res="未判：没有物品表状态日志（局里要带 DM_STATE=pwstate.jsonl）"
            elif pat[0]=="segnot": res=("通：" if all(x[0]!=pat[2] for x in sg) and sg else "不通：")+f"各停留段：{allt}"
            elif len(sg)<=pat[2]: res=f"不通：这一步之后物品没有新的停留段（应是第 {pat[2]} 段）；各停留段：{allt}"
            else:
                same=len(pat)>4 and pat[4]=="same"; mv=len(pat)>4 and pat[4]=="moved"; x=sg[pat[2]]; ok=x[0]==pat[3] and (not same or x[:4]==sg[pat[2]-1][:4]) and (not mv or x[:4]!=sg[pat[2]-1][:4])
                res=("通：" if ok else "不通：")+f"第 {pat[2]} 段 {seg_txt(x)}"+("（与上一段同格）" if same and ok else "")+("" if ok else f"；各停留段：{allt}")
            if len(pat)>5-(pat[0]=="segnot")*2 and isinstance(pat[-1],str) and pat[-1] not in ("same","moved") and not re.search(pat[-1],s): res="不通：没有期望的日志「"+pat[-1]+"」；"+res.split("：",1)[1]
        elif isinstance(pat,tuple) and pat[0]=="bag":   # ("bag",类号)：物品表最后一行里，背包（mode 0、page 0）有这个类号的物品 | ("bag",class number): in the last row of the item table, the inventory (mode 0, page 0) has an item of this class number
            import json
            try: last=[json.loads(l) for l in open(f"{d}/state.jsonl") if l.strip()][-1]; hit=[i for i in last["items"] if i["cls"]==pat[1] and i["mode"]==0 and i["page"]==0]
            except (OSError,ValueError,IndexError): hit=None
            res="未判：没有物品表状态日志" if hit is None else ("通：" if hit else "不通：")+(f"背包里有类号 {pat[1]} 的物品 id {hit[0]['id']}（格 {hit[0]['x']},{hit[0]['y']}）" if hit else f"局末背包里没有类号 {pat[1]} 的物品")
        elif isinstance(pat,tuple) and pat[0]=="cellhas":   # 开着的绑定框里，框顶那个技能要在指定的格子上 | in the open binding box, the skill at the top of the box must be on the specified cell
            try: b=bind_cells(f"{d}/{shot}")
            except OSError: b=None
            res="未判：读不出绑定框" if b is None else (f"通：技能在 {pat[1][0]} 排 {pat[1][1]} 列" if pat[1] in b else "不通：技能不在 %d 排 %d 列，读到的是 %s"%(pat[1][0],pat[1][1],sorted(b)))
        elif isinstance(pat,tuple) and pat[0]=="cells":
            try: res=bind_check(d,pat[1],shot,pat[2])
            except OSError as e: res=f"不通：读不到截图（{e.filename}）"
        elif pat is None: res="未判：只留截图，没设判据"
        else:
            m=re.search(pat,s); bad=re.search(neg,s) if neg else None
            res=("通：" + m.group(0)[:60]) if (m and not bad) else ("不通：" + ("没等到期望的日志" if not m else "出现 "+bad.group(0)[:40]))
        blocked=re.search(r"确认 / 丢弃键不放行[^\n]*",s)   # 数量框开着的项除外：框是真模态，底下的拖放被挡是对的   # 宿主拦了这一下的确认 / 丢弃键：途中的落点日志照样会出，只看落点会误报通（真机上有过戒指没戴上却报通）。这行日志每个进程只打前 20 次，不能只靠它——换装段末尾另有双击戒指位的反证 | items with the quantity box open are excluded: the box is a true modal, and the drag-and-drop underneath being blocked is correct   # the host blocked the confirm / drop key for this one: the drop point logs along the way still appear, so looking only at the drop point would falsely report pass (on a real device a ring was not equipped yet pass was reported). This log line is printed only the first 20 times per process, so it cannot be relied on alone - the end of the equipment-swap section has a separate counter-proof by double-tapping the ring slot
        if blocked and "数量框" not in title: res="不通：宿主拦了这一下——"+blocked.group(0)[:30]+("（正则本来判通）" if res.startswith("通") else "")
        rows.append((it[5],title,expect,res,f"{tag}/{shot}" if shot else ""))
    red=len(re.findall(LOGRED,log))
    if SHOT_BAD: rows.append((g.run,"（整局）不该出现认不出局号的截图名","日志里每张截图都认得出局号前缀（字母 + 数字 + 下划线）",f"不通：【红】认不出局号的截图 {len(SHOT_BAD)} 张（如 {SHOT_BAD[0]}）——取不到纵向偏移，这一局读截图的判据不可信",""))
    # 通用判据：点地 / 点物件的规划「一步都展不开」= 起点被自己或跟随单位的占位当成障碍（dm_world.c 修过的线上缺陷）。
    # 和「目标确实到不了、搜完几千格才取消」不同；vf1 局修前一直撞它，因为后面还有第二次点、箱子最终开了，原判据拦不住
    # General criterion: planning for tap-ground / tap-object "cannot expand a single step" = the start point is treated as an obstacle by the occupancy of the player or a following unit (a defect in released builds, fixed in dm_world.c).
    # This differs from "the target really cannot be reached, cancelled only after searching several thousand cells"; before the fix run vf1 kept hitting it, and because a second tap followed and the stash did open in the end, the original criterion could not catch it
    n0=len(re.findall(r"世界目标：路线 0 段（找到 0、搜过 1 格",log))
    if n0: rows.append((g.run,"（整局）不该出现一步都展不开的规划","日志里没有「世界目标：路线 0 段（找到 0、搜过 1 格」",f"不通：出现 {n0} 次——起点被当成障碍，点了走不了",""))
    if FORBID:
        m=re.search(FORBID,log); rows.append((g.run,"（整局）不该出现的日志",f"没有「{FORBID}」",("不通：出现 "+m.group(0)[:60]) if m else "通：没有",""))
    if REQUIRE:
        n=len(re.findall(REQUIRE,log)); rows.append((g.run,"（整局）该出现的日志",f"有「{REQUIRE}」",(f"通：出现 {n} 次" if n else "不通：一次都没有"),""))
    try:
        import json; ops=[json.loads(l)["pkt"][:2] for l in open(f"{d}/c2s.jsonl") if '"unit": 1' in l or '"unit":1' in l]
        casts=sum(1 for o in ops if o in ("05","06","07","08","09","0a","0c","0d","0e","0f","10","11"))
        rows.append((g.run,"（整局）技能 / 攻击类命令包","见各项",f"{casts} 条（操作码 05–11；没按项对齐）",""))
    except Exception as e: rows.append((g.run,"（整局）技能 / 攻击类命令包","见各项","未判：没取到命令包日志",""))
    try: tm=open(f"{d}/timing.txt").read().strip()
    except OSError: tm="计时：没取到"
    # 进局的角色必须是脚本选的那一行（2026-10-12：手柄选人进的不是高亮的角色，各局判据只数了「选了一次、进了局」，没核进的是谁）。
    # 宿主每次进局都打「刷怪积分牌：「<角色名>」…」；只核这一局第一次进局。
    # The character that enters the run must be the row the script selected (2026-10-12: selecting with the controller entered a character other than the highlighted one, and each run's criterion only counted "selected once, entered a run" without checking who entered).
    # The host prints 「刷怪积分牌：「<角色名>」…」(kill board: "<character name>"...) on every run entry; only the first run entry of this run is checked.
    if g.who:
        m=re.findall(r"刷怪积分牌：「([^」]+)」",log)
        rows.append((g.run,"（整局）该出现进局角色",f"进局的是选的那一行的角色「{g.who}」",(f"通：{m[0]}" if m and m[0]==g.who else f"不通：进的是「{m[0]}」" if m else "不通：日志里读不到进局角色"),""))
    return rows,wall,cap,red,tm
def groups():
    gs=[]
    # 装备位换装：测试开关生成软帽、骷髅帽、短剑、两枚魔法戒指、一张鉴定卷轴并当场进背包（第一件软帽会自动戴上）。
    # 捡完的背包布局（667 宽基准）：短剑 (245,240)、戒指 A (245,285)、骷髅帽 (412,235)、戒指 B (403,285)、新鉴定卷轴 (402,264)、原有鉴定卷轴 (423,265)、回城卷轴 (423,285)。
    # 装备位：头 (333,67)、左戒指 (294,167)、右戒指 (373,167)；拖放松手点 = 目标下方 40 点（落点算物品图位置）。
    # Equipment slot swap: the test switch generates a cap, a skull cap, a short sword, two magic rings and an identify scroll, straight into the inventory (the first cap is auto-equipped).
    # Inventory layout after pickup (667-wide base): short sword (245,240), ring A (245,285), skull cap (412,235), ring B (403,285), new identify scroll (402,264), existing identify scroll (423,265), town portal scroll (423,285).
    # Equipment slots: head (333,67), left ring (294,167), right ring (373,167); drag-and-drop release point = 40 points below the target (the drop point counts the item image position).
    g=G("a","DM_TEST_GIVE=cap:2:1,skp:2:1,ssd:2:1,rin:4:2,isc:2:1@60 DM_TEST_GIVE_INV=1 DM_UI_TAPS_AT=OpenPanel:PlayerInventoryExpansionLayout DM_DBG_FE_SEND=60:PanelManager:OpenPanel:SaveIndicatorPanel;75:PanelManager:OpenPanel:SaveIndicatorPanel;90:PanelManager:OpenPanel:SaveIndicatorPanel;105:PanelManager:OpenPanel:SaveIndicatorPanel;120:PanelManager:OpenPanel:SaveIndicatorPanel;135:PanelManager:OpenPanel:SaveIndicatorPanel;150:PanelManager:OpenPanel:SaveIndicatorPanel;185:PanelManager:ClosePanel:SaveIndicatorPanel"+(" DM_TEST_EDGE_B=20" if MODE=="ipadfull" else "")); g.sect("equip",0); g.enter(); g.wait(2.0)   # 换装段并在 a 局开头（同是术士档，省一次起停与进局等待）；它要原始的背包布局，所以排在最前 | the equipment-swap section is merged into the start of run a (same warlock save, saving one start/stop and one wait for entering the run); it needs the original inventory layout, so it goes first
    # 物品由 DM_TEST_GIVE_INV=1 当场进背包（布局与逐件走过去捡完全相同），不再逐件走过去捡，省 15 秒
    # 存盘指示（SaveIndicatorPanel）在最上层时拖物品：真机上鉴定 / 存盘那一刻它在最上层，宿主把手势层的确认键扣了，物品拿不起来。
    # Mac 上存盘太快撞不上，背包第一次打开后第 60–150 帧每 15 帧发一条 OpenPanel SaveIndicatorPanel、第 185 帧发 ClosePanel 把它收掉（不收的话它一直压在最上层，后面各项全被连带）（DM_DBG_FE_SEND，帧号从 DM_UI_TAPS_AT 那条消息起算），拖放正好落在这段里
    # Items go into the inventory on the spot via DM_TEST_GIVE_INV=1 (the layout is exactly the same as walking over and picking them up one by one), no longer walking over to pick up each one, saving 15 seconds
    # Dragging an item while the save indicator (SaveIndicatorPanel) is topmost: on a real device it is topmost at the moment of identify / save, the host withholds the gesture layer's confirm key, and the item cannot be picked up.
    # On Mac the save is too fast to hit, so in frames 60–150 after the inventory first opens one OpenPanel SaveIndicatorPanel is sent every 15 frames, and at frame 185 a ClosePanel is sent to dismiss it (if not dismissed it stays pressed on the topmost layer and every later item is dragged down with it) (DM_DBG_FE_SEND, frame numbers counted from the DM_UI_TAPS_AT message), and the drag-and-drop falls exactly in this span
    g.tap("@menu",1.1)
    g.item("存盘指示在最上层时把骷髅帽拖到头部位","拿得起、换上，软帽落到骷髅帽原来的格子；日志没有「不放行」",r"OpenPanel SaveIndicatorPanel[\s\S]*换下来的那件[^\n]*放到被拖物品腾出的原位|换下来的那件[^\n]*放到被拖物品腾出的原位[\s\S]*OpenPanel SaveIndicatorPanel",r"原版判定 0"); g.dragp((412,235),(333,107)); g.wait(1.0); g.shot()
    g.item("单击背包里的软帽（头上戴着骷髅帽）","出说明，和身上那件并排对比（看截图）",r"物品区 grid"); g.tap(P(412,235),1.5); g.shot()
    g.item("短剑拖到头部位（戴不了）","回到原来的格子",r"原版判定 0[\s\S]*没放下[^\n]*放到被拖物品腾出的原位"); g.dragp((245,240),(333,107)); g.shot()
    # 鉴定后不等、立刻拖（间隔 0.15 秒）：真机上鉴定会触发存盘，存盘指示在最上层的那一刻确认键被扣、戒指拿不起来；等 1.8 秒再拖就撞不上
    # Drag immediately after identifying without waiting (0.15 second gap): on a real device identifying triggers a save, and at the moment the save indicator is topmost the confirm key is withheld and the ring cannot be picked up; waiting 1.8 seconds before dragging would not hit it
    g.item("双击鉴定卷轴后点戒指 A","鉴定",r"界面鉴定：单击 rin"); g.dbl(423,265); g.tap(P(245,285),0.15)
    g.item("鉴定后立刻把戒指 A 拖到左戒指位（空）","戴上（是否真戴上由后面换装与双击戒指位两项反证）",r"拖放落点类 5",r"原版判定 0|没放下"); g.dragp((245,285),(294,207)); g.shot()
    g.item("双击另一张鉴定卷轴后点戒指 B","鉴定",r"界面鉴定：单击 rin"); g.dbl(402,264); g.tap(P(403,285),0.15)
    g.item("鉴定后立刻把戒指 B 拖到左戒指位（戴着 A）","换上 B，A 落到 B 原来的格子",r"原版判定 5|换下来的那件[^\n]*原位",r"原版判定 0"); g.dragp((403,285),(294,207)); g.shot()
    g.item("换下来的戒指 A 拖到右戒指位（空）","戴上",r"拖放落点类 5",r"原版判定 0|没放下"); g.dragp((403,285),(373,207)); g.shot()
    g.item("双击右戒指位","卸下那枚戒指（反证上一步确实戴上了）",r"界面双击：身上的 rin"); g.dbl(373,167); g.shot()
    g.item("双击左戒指位","卸下那枚戒指（反证左戒指位确实戴着）",r"界面双击：身上的 rin"); g.dbl(294,167); g.shot()
    g.tap(P(615,17),1.2); g.sect("hud")
    g.item("进局","玩家状态可读、在营地",r"pos 区域 1 \(\d+,\d+\) 生命 \d+/\d+"); g.at("pos",.3); g.shot()
    g.item("点右上角区域名那一条","切全屏地图",r"点小地图那一块.*→ 全屏地图"); g.tap("@mmname"); g.shot()   # 点击区只是小地图矩形顶部区域名那一条（线上「小地图消失」）；@mmname = 点击区中心，各档位置由前端算。「点主体不切地图」在专项固定用例里验（营地里主体中心附近有储藏箱，放进整表会点开它）   # 真全屏下小地图在屏幕真顶部 = 游戏视图上方 256 点，脚本 y 是负数 | the tap area is only the area-name strip at the top of the minimap rectangle (the "minimap disappears" issue in released builds); @mmname = tap area center, with the position in each profile computed by the front end. "Tapping the body does not switch the map" is verified in a dedicated fixed case (in the camp there is a stash near the body center, and putting it in the full table would tap it open)   # in true fullscreen the minimap is at the true top of the screen = 256 points above the game view, so the script y is negative
    g.item("全屏地图点空处","仍是全屏地图（点地面走路不再切回小地图，#186）",r"pos 区域 \d+",r"→ 回小地图|记住小地图"); g.tap(P(300,150)); g.at("pos",.3); g.shot()
    g.item("再点区域名那一条","回小地图",r"点小地图那一块.*→ 回小地图"); g.tap("@mmname"); g.shot()
    g.item("点底栏十字","开物品栏",r"OpenPanel PlayerInventoryExpansionLayout"); g.tap("@menu"); g.shot()
    for nm,x,pat in [("任务日志",114,"OpenPanel QuestLogPanelExpansion"),("角色",223,"OpenPanel CharacterStatsPanel"),("技能树",442,"OpenPanel SkillsTreePanel"),("选项",551,"OpenPanel PauseLayout"),("物品栏",333,"OpenPanel PlayerInventoryExpansionLayout")]:
        g.item(f"点顶部页签「{nm}」",f"切到{nm}",pat); g.tap(P(x,17)); g.shot()
    g.item("点右上角 X","面板关闭",r"ClosePanel PlayerInventoryExpansionLayout"); g.tap(P(615,17)); g.shot()
    g.item("点腰带第 1 格","喝掉一瓶",r"腰带 0/1/1/1"); g.tap("@belt1",2.0); g.shot()
    # 摇杆输入行程：全设备 60 点满偏（位移是屏幕点，不乘控件缩放）；线性带死区：幅度 = (位移 − 6) / 54。只量纯水平，斜向另有预修正
    # Stick input travel: 60 points for full deflection on all devices (displacement is in screen points, not multiplied by widget scale); linear with a dead zone: magnitude = (displacement − 6) / 54. Only pure horizontal is measured, diagonal has a separate pre-correction
    g.item("摇杆向右推 60 点","满偏：lx = 32767",r"\[script\] move slot=1 spec=@joy\+60,0[^\n]*lx=32767 ly=0"); g.at("down 1 @joy",.3); g.at("move 1 @joy+60,0",.5); g.at("up 1",.6)
    g.item("摇杆向右推 20 点","lx = 8495（(20 − 6) / 54 × 32767）",r"\[script\] move slot=1 spec=@joy\+20,0[^\n]*lx=8495 ly=0"); g.at("down 1 @joy",.3); g.at("move 1 @joy+20,0",.5); g.at("up 1",.6)
    if MODE=="ipadfull":   # iPad 底边那一条（底部安全区高，带横条的 iPad 是 20 点；Mac 上没有，用 DM_TEST_EDGE_B=20 给）：在里面起手不当摇杆，系统手势留给系统 | the strip at the iPad bottom edge (bottom safe area height, 20 points on iPads with the home bar; absent on Mac, given with DM_TEST_EDGE_B=20): a touch starting inside it is not treated as the stick, system gestures are left to the system
        g.item("底边那一条里起手往上拖","整次触摸都不当摇杆：轴为 0、人不动",edge_idle); g.at("pos",.2); g.at("down 1 200,760",.3); g.at("move 1 200,690",.8); g.at("up 1",.5); g.at("pos",.3)
        g.item("从摇杆底座起手往上拖","摇杆照常：纵轴满偏（底座固定后，只有落在底座附近才起摇杆）",r"\[script\] move slot=1 spec=@joy\+0,-60[^\n]*ly=-?32767"); g.at("down 1 @joy",.3); g.at("move 1 @joy+0,-60",.5); g.at("up 1",.6)
    g.sect("skill",skip=LETTERBOX)   # 留边档不跑野外施放；iPhone 比例与 iPad 真全屏都跑 | letterbox mode does not run casting in the wilderness; both the iPhone ratio and iPad true fullscreen run it
    for x,y,r,to in [(5788,4475,4,10),(5812,4503,4,9),(5826,4522,4,8),(5840,4560,5,8)]: g.at(f"walk_to {x} {y} {r} {to} skip",to+.5)
    for n in (4,3,2,1):
        bound=(n==3)   # 1 级术士标准存档：绑着技能的是 @skill3，其余是空位 | level 1 warlock standard save: the one with a skill bound is @skill3, the rest are empty slots
        g.item(f"野外点技能钮 @skill{n}","施放：法力下降" if bound else "空位：开技能树、不施放",cast if bound else nocast); g.tap(f"@skill{n}",2.2); g.shot(); g.tap(P(615,17),1.0)
        g.item(f"@skill{n} 按住向右拖","施放：法力下降" if bound else "空位：不施放",cast if bound else nocast); g.drag(f"@skill{n}",f"@skill{n}+{O(70,0)}"); g.wait(1.0); g.shot(); g.tap(P(615,17),1.0)
    g.item("按住主钮 1.5 秒","人不离开原地",stay); g.at("pos",.2); g.at("down 0 @main",1.5); g.at("up 0",.8); g.at("pos",.3); g.shot()
    g.sect("pause"); g.tap("@menu"); g.tap(P(551,17))
    # 留边档不跑设置改值 / 应用 / 确认框这四项（与比例无关的逻辑）
    # Letterbox mode does not run these four items: settings value change / apply / confirmation box (logic unrelated to aspect ratio)
    g.item("暂停菜单点「编年史」","进编年史",r"OpenPanel ChroniclePanel"); g.at("tap_ui Chronicle@PauseLayoutGarden 3",2.0); g.shot()
    g.item("编年史点右上角 X","回暂停菜单",r"ClosePanel ChroniclePanel"); g.tap(P(615,17)); g.shot()
    g.item("暂停菜单点「选项」","进设置页",r"OpenPanel SettingsPanel"); g.at("tap_ui Settings@PauseLayoutGarden 3",1.8); g.shot()
    for nm,x in [("游戏",333),("游戏设定",388),("音频",277)]:
        g.item(f"设置页点子页签「{nm}」","切页",r"界面页签：切 [+-]\d+ 页"); g.tap(P(x,67),1.3); g.shot()
    if not LETTERBOX: g.item("点 NPC 语音右箭头","改值",r"OptionsPanelMessage SetNPCSpeech"); g.tap(P(485,195),1.2); g.shot()
    g.item("点「放弃」","退出设置页、不保存",r"ClosePanel SettingsPanel"); g.tap(P(420,345),1.8); g.shot()
    if not LETTERBOX: g.item("再进设置改一项后点「应用」","保存并退出设置页",r"AcceptChanges"); g.at("tap_ui Settings@PauseLayoutGarden 3",1.8); g.tap(P(485,195),1.2); g.tap(P(245,345),1.8); g.shot()
    if not LETTERBOX: g.item("有改动时点顶部「角色」页签","弹确认框",r"OpenPanel ConfirmationModal"); g.at("tap_ui Settings@PauseLayoutGarden 3",1.8); g.tap(P(485,195),1.2); g.tap(P(223,17),1.5); g.shot()
    if not LETTERBOX: g.item("确认框点「放弃」","关设置页、不保存",r"CancelChanges"); g.tap(P(378,200),1.8); g.shot()
    g.item("暂停菜单点「保存并退出」","回选角页",r"FrontEndNavigation ToFrontEnd"); g.tap("@menu"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayoutGarden 3",4.0); g.shot()
    gs.append(g)
    # e 局：选角页与建角（从选角页起步，不进别的角色；原先接在 a 局暂停段的「保存并退出」后面，a 局超过单局 3 分钟后拆出来）
    # e run: character select page and character creation (starts from the character select page, does not enter any other character; it used to follow 「保存并退出」(Save and Exit) in the pause section of run a, and was split out after run a exceeded 3 minutes for a single run)
    g=G("e"); g.sect("front",0); g.t=3.5
    g.item("选角页点第 3 行","选中该角色",r"tap_ui ListItem2@CharacterSelectPanel：控件在[\s\S]*UpdateCharacterInfo"); g.ui("ListItem2@CharacterSelectPanel",1.3); g.shot()
    g.item("点垃圾桶","弹删除确认",r"ShowDeleteCharacterModal"); g.ui("DeleteCharacter@CharacterSelectPanel",1.5); g.shot()
    g.item("删除确认点「否」","弹窗关闭、不删",r"ClosePanel DeleteCharacterModal",r"CharacterSelect DeleteCharacter "); g.tap(P(380,203),1.5); g.shot()
    g.item("点「创建新角色」","进选职业页",r"ToCharacterCreate"); g.ui("CreateCharacter@CharacterSelectPanel",2.5); g.shot()
    g.item("点一个职业两下","选中职业",r"CharacterCreate Select"); g.tap(P(488,209),.8); g.tap(P(433,189),1.5); g.shot()
    g.item("点「创建」","进选择游戏模式页",r"OpenPanel CharacterOptionsPanel"); g.tap(P(334,343),2.8); g.shot()
    g.item("点「极乐世界」勾选框","勾上",r"极乐世界 勾上","late"); g.tap(P(70,236),1.2); g.shot()   # 勾选状态到下一次触摸才记日志 | the checked state is only logged at the next touch
    g.item("再点一次勾选框","取消勾选",r"极乐世界 取消","late"); g.tap(P(70,236),1.2); g.shot()
    g.item("点「标准」","弹名字输入框",r"OpenPanel CharacterCreateNamePanel"); g.tap(P(167,193),2.5); g.shot()
    g.item("名字框留空点确定","填入随机名",r"随机名 → 名字框"); g.at("alert ok",2.0); g.shot()
    g.item("起名页点「创建」","进局；建的是普通角色，不补共享仓库",r"FrontEndNavigation ToGame",r"共享仓库补齐"); g.tap(P(407,257),7.0); g.at("pos",.3); g.shot()
    # 连建两个留空名字的角色（随机名与已有角色重名时再取一个）：两个都建成、名字不同；选角列表里的两个名字看最后一张截图
    # Create two characters in a row with the name left blank (when the random name collides with an existing character, take another one): both are created and the names differ; the two names in the character list are checked in the last screenshot
    g.item("暂停菜单点「保存并退出」","回选角页",r"FrontEndNavigation ToFrontEnd"); g.tap("@menu"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayoutGarden 3",4.0); g.shot()
    g.item("再建一个：名字框同样留空","建成进局",r"随机名 → 名字框[\s\S]*FrontEndNavigation ToGame"); g.ui("CreateCharacter@CharacterSelectPanel",2.5); g.tap(P(488,209),.8); g.tap(P(433,189),1.5); g.tap(P(334,343),2.8); g.tap(P(167,193),2.5); g.at("alert ok",2.0); g.tap(P(407,257),7.0); g.at("pos",.3); g.shot()
    g.item("再保存并退出回选角页","两次填进名字框的随机名不同；选角列表里有这两个名字（看截图）",two_names); g.tap("@menu"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayoutGarden 3",4.5); g.shot()
    gs.append(g)
    # v 局：勾着「极乐世界」建角（勾选框只点一次）。进局约 1 秒后宿主把共享仓库金币补到合计 250 万、往这个角色背包里发 2 枚赦免之证并请游戏存盘；再开箱看共享页（金币框，看截图；赦免之证在背包）
    # v run: create a character with "Elysium" checked (the checkbox is tapped only once). About 1 second after entering the run the host tops
    # the shared stash gold up to a total of 2.5 million, grants 2 Tokens of Absolution into this character's backpack and asks the game to save;
    # then open the stash and look at the shared page (gold box, check the screenshot; the Tokens of Absolution are in the backpack)
    g=G("v"); g.sect("ely",0); g.t=3.5
    g.item("点「创建新角色」","进选职业页",r"ToCharacterCreate"); g.ui("CreateCharacter@CharacterSelectPanel",2.5); g.shot()
    g.item("点一个职业两下","选中职业",r"CharacterCreate Select"); g.tap(P(488,209),.8); g.tap(P(433,189),1.5); g.shot()
    g.item("点「创建」","进选择游戏模式页",r"OpenPanel CharacterOptionsPanel"); g.tap(P(334,343),2.8); g.shot()
    g.item("点「极乐世界」勾选框（只点一次）","勾上",r"极乐世界 勾上","late"); g.tap(P(70,236),1.2); g.shot()   # 勾选状态到下一次触摸才记日志 | the checked state is only logged at the next touch
    g.item("点「标准」","弹名字输入框",r"OpenPanel CharacterCreateNamePanel"); g.tap(P(167,193),2.5); g.shot()
    g.item("名字框留空点确定","填入随机名",r"随机名 → 名字框"); g.at("alert ok",2.0); g.shot()
    g.item("起名页点「创建」，进局后等 12 秒","记为极乐角色 → 共享仓库补齐 → 补齐后金币到数、角色身上赦免之证 2 枚 → 随存档落盘，四行依次出现、不带红",r"记为极乐世界角色[\s\S]*共享仓库补齐——[\s\S]*共享仓库补齐后——[^\n]*角色身上 2 枚[\s\S]*共享仓库补齐已随存档提交落盘",r"共享仓库补齐[^\n]*【红】|【红】[^\n]*共享仓库"); g.tap(P(407,257),12.0); g.at("pos",.3); g.shot()
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 4",4.5); g.shot()
    g.item("点「共享」页签","切页",r"界面页签：切 \+1 页"); g.tap(P(106.5,53),1.2); g.shot()   # 储藏箱加宽后页签左移 34.5 | after the stash widening the tabs move left by 34.5
    g.item("共享页","金币框 ≥ 2500000；两枚赦免之证在背包里（看截图）",None); g.shot()
    gs.append(g)
    # v2 局（产品落点，不关「进局落小站旁」）：线上缺陷的固定局——落点改到小站旁之后每个新建的极乐角色欠条永不划掉、每次进局再发两枚赦免之证并把金币补回 250 万。
    # 同 v 局建角到补齐；再保存退出、同一角色再进一局：不许再出现补齐行
    # v2 run (product spawn position, "spawn next to the town waypoint" not turned off): the fixed run for a bug in the released build -- after
    # the spawn position was moved next to the waypoint, the pending-grant record of every newly created Elysium character was never cleared, and
    # every run granted two more Tokens of Absolution and topped the gold back up to 2.5 million.
    # Same as the v run from character creation through the top-up; then save and exit and enter another run with the same character: the
    # top-up line must not appear again
    g=G("v2"); g.sect("ely2",0); g.t=3.5
    g.item("点「创建新角色」","进选职业页",r"ToCharacterCreate"); g.ui("CreateCharacter@CharacterSelectPanel",2.5); g.shot()
    g.item("点一个职业两下","选中职业",r"CharacterCreate Select"); g.tap(P(488,209),.8); g.tap(P(433,189),1.5); g.shot()
    g.item("点「创建」","进选择游戏模式页",r"OpenPanel CharacterOptionsPanel"); g.tap(P(334,343),2.8); g.shot()
    g.item("点「极乐世界」勾选框（只点一次）","勾上",r"极乐世界 勾上","late"); g.tap(P(70,236),1.2); g.shot()   # 勾选状态到下一次触摸才记日志 | the checked state is only logged at the next touch
    g.item("点「标准」","弹名字输入框",r"OpenPanel CharacterCreateNamePanel"); g.tap(P(167,193),2.5); g.shot()
    g.item("名字框留空点确定","填入随机名",r"随机名 → 名字框"); g.at("alert ok",2.0); g.shot()
    g.item("起名页点「创建」，进局后等 12 秒","记为极乐角色 → 共享仓库补齐 → 补齐后金币到数、角色身上赦免之证 2 枚 → 随存档落盘，四行依次出现、不带红",r"记为极乐世界角色[\s\S]*共享仓库补齐——[\s\S]*共享仓库补齐后——[^\n]*角色身上 2 枚[\s\S]*共享仓库补齐已随存档提交落盘",r"共享仓库补齐[^\n]*【红】|【红】[^\n]*共享仓库"); g.tap(P(407,257),12.0); g.at("pos",.3); g.shot()
    g.item("保存并退出","回选角页",r"FrontEndNavigation ToFrontEnd"); g.tap("@menu"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayoutGarden 3",5.0); g.shot()
    g.item("同一角色再进一局，等 12 秒","进局；不再补齐（没有「共享仓库补齐——」行）、不带红",lambda st,bd: ("不通：没进局" if "FrontEndNavigation ToGame" not in st else "不通：进的不是那个极乐角色" if "极乐世界：角色" not in st else "不通：第二局又补了一次" if "共享仓库补齐——" in st else "不通：有红" if re.search(r"【红】极乐世界",st) else "通：进局、没有补齐行")); g.tap(P(333,336),8.0); g.wait(12.0); g.at("pos",.3); g.shot()
    gs.append(g)
    g=G("b","DM_TEST_GOLD=1300 DM_TEST_WORLD_GO=near2:1:c154@650"); g.sect("npc",0); g.enter()
    g.item("点瓦瑞夫（第一次）","走过去，出讲话",r"OpenPanel ScrollSpeechPanel"); g.at("tap_unit 1 155 3",5.0); g.shot()   # 要先走过去，留够时间 | has to walk over first, leave enough time
    g.item("再点一下跳过讲话","出两行菜单（交谈 / 取消）",r"ClosePanel ScrollSpeechPanel"); g.at("tap_unit 1 155 2",1.8); g.shot()
    g.item("两行菜单点「交谈」","命中第 1 行",r"对话菜单 \+0 行"); g.at(f"tap_unit 1 155 2 +{MENU["talk2"]}",2.0); g.shot()
    g.item("讲话 / 话题页点菜单外","关闭",r"ClosePanel (NpcDialogPanel|ScrollSpeechPanel)"); g.tap(P(100,300),1.2); g.tap(P(100,300),1.2); g.shot()
    g.item("重开菜单后点「取消」","命中第 2 行并关菜单",r"对话菜单 \+1 行"); g.at("tap_unit 1 155 2",2.0); g.at(f"tap_unit 1 155 2 +{MENU["cancel2"]}",2.0); g.shot()
    g.item("重开菜单后点菜单外","关菜单、人不走",r"ClosePanel NpcDialogPanel"); g.at("tap_unit 1 155 2",2.0); g.tap(P(100,300),1.5); g.shot()
    g.arrive(5776,4433,10,80.0)   # 上限只是兜底（到位就继续）；开关按游戏拍数发令，和脚本时钟不同步，上限留宽 | the limit is only a fallback (continue as soon as in position); the switch issues commands by game ticks, not in sync with the script clock, so the limit is left wide
    g.item("点恰西","出三行菜单",r"OpenPanel NpcDialogPanel"); g.at("tap_unit 1 154 3",5.0); g.at("tap_unit 1 154 2",2.0); g.shot()   # 第二下跳过首次讲话 | the second tap skips the first-time speech
    g.item("三行菜单点「交易或修复」","进商人页",r"OpenPanel VendorPanelLayout"); g.at(f"tap_unit 1 154 2 +{MENU["trade3"]}",2.5); g.shot()
    g.sect("shop",0)   # 接着 npc 组开着的商人页走（买的东西放到背包第 6 列，别占 inv 组拖武器的落点；卖第二张卷轴，第一张留给 inv 组） | continues with the vendor page left open by the npc group (the bought item goes to inventory column 6, do not take the drop point where the inv group drags the weapon; sell the second scroll, the first is left for the inv group)
    g.item("货架上单击一件武器（身上拿着短剑）","出说明；有没有和身上那件并排对比（看截图）",None); g.tap(P(163,53),1.2); g.tap(P(102,85),1.5); g.shot()
    g.item("点「杂项」页签","切货架",r"界面页签：切 [+-]\d+ 页"); g.tap(P(270,53)); g.shot()
    g.item("货架第一件拖进背包（1200，身上 1300）","买下",r"买选中的货.*→ 1（"); g.dragp((102,85),(491,264)); g.shot()
    g.item("再拖一件 1200 的（钱不够）","走到原版买入口且身上钱少于价（提示与不成交看截图）",r"价 1200，身上 100 \+"); g.dragp((102,124),(454,264)); g.wait(.8); g.shot()
    g.item("提示框点「继续」","关提示",r"对话菜单 \+0 行|商人页结果提示（按返回收）"); g.tap(P(100,100)); g.shot()   # 商人页的单行结果提示现在按返回收（修复），日志是后一种 | the single-row result prompt on the vendor page is now dismissed by pressing Back (fix); the log is the latter kind
    g.item("背包里的第二张卷轴拖到货架","卖出",r"卖手上的物品.*→ 1"); g.dragp((562,286),(190,184)); g.shot()
    g.item("点右上角 X","关商人页",r"ClosePanel VendorPanelLayout"); g.tap(P(615,17)); g.shot()
    g.sect("inv"); g.tap("@menu")
    g.item("单击背包里的卷轴","命中物品格、出说明",r"物品区 grid"); g.tap(P(423,262)); g.shot()
    g.item("武器从装备位拖进背包","放进背包",r"拖放落点类 5"); g.dragp((257,119),(280,297)); g.shot()
    g.item("从背包拖回装备位","装上",r"拖放落点类 5"); g.dragp((280,270),(257,150)); g.shot()
    g.item("双击装备位上的武器","卸下",r"界面双击"); g.dbl(257,119); g.shot()
    g.item("卷轴快松到石板空白（一步拖过去马上松手）","按松手位置算：放回，不丢",r"拖放落点类 0[\s\S]*c2s 19[\s\S]*c2s 18",r"c2s 17"); g.fast((423,262),(195,300)); g.shot()
    g.item("卷轴快松到左侧黑网格","按松手位置算：丢到地上",r"拖放落点类 3[\s\S]*c2s 17"); g.fast((423,262),(95,240)); g.shot()
    g.item("长按背包空格","整理",r"整理："); g.hold(300,262); g.shot()
    g.item("点右上角 X","面板关闭",r"ClosePanel PlayerInventoryExpansionLayout"); g.tap(P(615,17)); g.shot()
    g.item("地上掉落物","名字压在物品上（看截图）",None); g.shot()
    g.item("在物品位置落点","命中名字标签或物品框（结果里写明是哪个），走到跟前捡回",r"类型 4 [^\n]*（(名字标签|命中框)[\s\S]*类型 4 id \d+ 到跟前[^\n]*交互 1"); g.at("tap_unit 4 16777215 2",3.0); g.shot()
    gs.append(g)
    g=G("c","DM_TEST_WORLD_GO=near2:1:c155@40 DM_UI_TAPS_AT=OpenPanel:DropGoldModal DM_DBG_FE_SEND=60:PanelManager:OpenPanel:SaveIndicatorPanel;75:PanelManager:OpenPanel:SaveIndicatorPanel;90:PanelManager:OpenPanel:SaveIndicatorPanel;105:PanelManager:OpenPanel:SaveIndicatorPanel;120:PanelManager:OpenPanel:SaveIndicatorPanel;135:PanelManager:OpenPanel:SaveIndicatorPanel;150:PanelManager:OpenPanel:SaveIndicatorPanel;185:PanelManager:ClosePanel:SaveIndicatorPanel"); g.sect("stash",0); g.enter(1); g.wait(3.0)
    # 名牌只在有东西指向 NPC 时显示：这一局让测试开关在进局后 40 拍指向瓦瑞夫，等 3 秒截图。不放在 b 局——从紧挨瓦瑞夫的位置出发去恰西，游戏寻路会「走不过去」取消
    # The name tag shows only when something points at an NPC: this run has the test switch point at Warriv 40 ticks after entering the run, then waits 3 seconds and takes a screenshot. Not placed in run b - setting out for Charsi from right next to Warriv, the game's pathfinding cancels with "cannot walk there"
    g.item("NPC 名牌位置","名牌在 NPC 头顶（看截图）",None); g.shot()
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 3",3.0); g.shot()
    g.item("背包里的宝石拖进箱子的空格","放进箱子",r"拖放落点类 5"); g.dragp((522.5,285),(82,235)); g.shot()   # 落点要是空格：拖到已有物品的格子上会换下那件留在手上 | the drop point must be an empty cell: dragging onto a cell that already has an item swaps that item out and leaves it in hand
    g.item("双击箱子里那颗宝石","移回背包（开箱时双击 = 按住 Y 脉冲，另看截图）",r"InputMessage ControllerActionBegin"); g.dbl(82,201); g.shot()
    g.item("点「共享」页签","切页",r"界面页签：切 \+1 页"); g.tap(P(106.5,53)); g.shot()
    g.item("点「个人」页签","切回",r"界面页签：切 -1 页"); g.tap(P(62.5,53)); g.shot()
    g.item("点取金币按钮","弹数量框",r"OpenPanel DropGoldModal"); g.tap(P(187.5,307),1.0); g.shot()
    g.item("数量框开着、存盘指示叠在上面时拖背包里的魔盒","框关掉、魔盒没被拖走、金币不变（看截图）；日志有存盘指示",r"OpenPanel SaveIndicatorPanel[\s\S]*ClosePanel DropGoldModal|ClosePanel DropGoldModal[\s\S]*OpenPanel SaveIndicatorPanel",r"拖放落点类 5[\s\S]*换下来|原版判定"); g.dragp((590.5,238),(161,275)); g.wait(1.2); g.shot()
    g.tap(P(187.5,307),1.0)
    g.item("数量框开着等 3 秒","数字不变（看截图）",None); g.wait(3.0); g.shot()
    g.item("数量框点「取消」","关框、金币不变",r"ClosePanel DropGoldModal"); g.tap(P(375,200)); g.shot()
    g.item("数量框开着点框外的「共享」页签","框关掉、不切页、金币不变（看截图）",r"ClosePanel DropGoldModal",r"界面页签：切"); g.tap(P(187.5,307),1.0); g.tap(P(106.5,53),1.2); g.shot()
    g.item("数量框开着拖底下的魔盒","框关掉、魔盒没被拖走（看截图）",r"ClosePanel DropGoldModal",r"拖放落点类 5"); g.tap(P(187.5,307),1.0); g.dragp((590.5,238),(161,275)); g.shot()
    g.item("数量框开着点箱子里的空格子","框关掉、金币不变",r"ClosePanel DropGoldModal"); g.tap(P(187.5,307),1.0); g.tap(P(82,235),1.2); g.shot()
    g.item("数量框开着双击背包里的魔盒","框关掉、魔盒没打开",r"ClosePanel DropGoldModal",r"OpenPanel HoradricCubeLayout"); g.tap(P(187.5,307),1.0); g.dbl(590.5,238); g.shot()
    g.item("数量框里输 100 点「好」","取出 100：箱子 296、身上 100（看截图）",r"ClosePanel DropGoldModal"); g.tap(P(187.5,307),1.0); g.tap(P(332,173),1.5); g.at("clear",.3); g.at("type 100",.5); g.at("alert ok",1.2); g.tap(P(290,200),1.5); g.shot()
    g.item("存入框开着点框外的页签","框关掉、不切页、金币不变（看截图）",r"ClosePanel DropGoldModal",r"界面页签：切"); g.tap(P(494.5,308),1.0); g.shot(); g.tap(P(106.5,53),1.2); g.shot()
    g.item("开着箱子双击背包里的魔盒","打开魔盒",r"OpenPanel HoradricCubeLayout"); g.dbl(590.5,238); g.shot()
    g.tap(P(654.5,17),1.2); g.tap(P(654.5,17),1.2); g.tap("@menu",1.4)
    g.item("只开物品栏点金币（丢金币框）再点框外","框关掉、金币不变（看截图）",r"OpenPanel DropGoldModal[\s\S]*ClosePanel DropGoldModal"); g.tap(P(333,309),1.0); g.shot(); g.tap(P(120,200),1.2); g.shot()
    g.tap(P(615,17),1.2)
    # 模态框外触摸的其余两类也放在这一局
    # The other two kinds of touches outside a modal box are also placed in this run
    g.tap("@menu",1.4); g.tap(P(442,17),1.5)
    g.item("技能树里长按已学技能","弹「重新绑定技能」框",r"OpenPanel ButtonBindingModal"); g.hold(261,90,1.3); g.shot()   # 圣骑「防御光环」页左上角那个已学技能（右上角那格没学，长按不出框） | the learned skill at the top-left of the paladin's 「防御光环」(Defensive Auras) page (the top-right cell is not learned, long-press shows no box)
    g.item("绑定框开着点框外","框关掉、留在技能树",r"技能绑定：已取消[^\n]*留在技能树"); g.tap(P(120,200),1.2); g.shot()
    g.tap(P(551,17),1.5); g.at("tap_ui Settings@PauseLayoutGarden 3",1.8); g.tap(P(485,195),1.2); g.tap(P(223,17),1.5)
    g.item("设置改了一项后点顶部页签弹出确认框，点框外","没有反应、框还在（看截图）",r"script\] down",r"ClosePanel ConfirmationModal|CancelChanges|AcceptChanges"); g.tap(P(333,330),1.2); g.shot()
    g.item("确认框点「应用」","保存生效",r"AcceptChanges"); g.tap(P(283,200),1.8); g.shot()
    gs.append(g)
    # d 局要一份专用存档（术士，箱子符文页有一叠 12 个符文）：仓库里不带，本机要先备好，目录名由 UIREG_STACK_SAVE 指定（见usage.md）
    # Run d needs a dedicated save (warlock, the stash rune page has a stack of 12 runes): it is not in the repository and must be prepared locally first, with the directory name given by UIREG_STACK_SAVE (see usage.md)
    g=G("d","DM_TEST_WORLD_GO=near2:2:c267@200",save=os.environ.get("UIREG_STACK_SAVE") or "save.pw_rune_stack"); g.sect("stack",0); g.t=3.5; g.tap(P(333,336),1.6); g.at("await pos"); g.t=max(g.t,13.0); g.at("note 进局",1.5)
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.arrive(5808,4443,3,24.0); g.at("tap_unit 2 267 3",3.0); g.shot()
    g.item("点「符文」页签","切到符文页",r"界面页签：切 [+-]\d+ 页"); g.tap(P(148,53)); g.shot()
    g.item("单击成叠的符文格","命中成叠格、出说明、不取",r"成叠格",r"取多件"); g.tap(P(201,81),2.0); g.shot()
    g.item("双击成叠格","取 1 件",r"取多件：.*取 1："); g.dbl(201,81); g.shot()
    g.item("长按成叠格","取 3 件",r"取多件：.*取 3："); g.hold(201,81); g.shot()
    g.item("从成叠格拖到背包","取 1 件",r"取多件：.*取 1："); g.dragp((201,81),(488.5,297)); g.shot()
    g.item("从成叠格拖到面板下方松手","不取",r"拖放落点|界面命中",r"取多件"); g.dragp((201,81),(333,372)); g.shot()
    gs.append(g)
    # sa 局：往个人页 / 共享页里存符文自动进高级仓库（珍宝匣存入，dm_pouch.c）。同 d 局的存档与开头：珍宝匣页符文那一叠 12 个，每次双击取 1 个到背包（落在背包右上角那格），
    # 再把它拖进个人页 / 共享页的格子，或开着箱子双击它。判据读宿主日志：个人页 = 落下后服务端代发原版「移到仓库」；共享页 = 收包处改道（放回背包原格再发）；双击 = 原版「移动到堆叠栏」
    # Run sa: runes stored into the personal page / shared page go into the advanced stash automatically (trove deposit, dm_pouch.c). Same save and opening as run d: the stack of 12 runes on the trove page, each double-tap takes 1 into the backpack (it lands in the top-right backpack cell),
    # then drag it into a cell of the personal page / shared page, or double-tap it with the stash open. The criteria read the host log: personal page = after the drop the server sends the original "move to stash" on its behalf; shared page = rerouted at packet receive (put back into its backpack cell and sent again); double-tap = the original "move to stacking tab"
    g=G("sa","DM_TEST_WORLD_GO=near2:2:c267@200 DM_C2S_LOG=pwc2s.jsonl",save=os.environ.get("UIREG_STACK_SAVE") or "save.pw_rune_stack"); g.sect("stashauto",0); g.t=3.5; g.tap(P(333,336),1.6); g.at("await pos"); g.t=max(g.t,13.0); g.at("note 进局",1.5)
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.arrive(5808,4443,3,24.0); g.at("tap_unit 2 267 3",3.0); g.shot()
    g.item("珍宝匣页双击符文那一叠","取 1 件到背包",r"取多件：.*取 1："); g.tap(P(148,53)); g.dbl(201,81); g.shot()
    g.item("切到个人页，把背包里那颗符文拖进个人页格子","落下后自动转进珍宝匣，个人页不留",r"珍宝匣：仓库页里新落下的物品[\s\S]*珍宝匣：物品 id \d+ 已从仓库页转进高级仓库",r"2 秒还在仓库页"); g.tap(P(62,53)); g.dragp((600.5,227),(81,237)); g.wait(2.5); g.shot()
    g.item("珍宝匣页再取 1 件","到背包（那一叠还是 12 个：上一步拖进个人页的那颗回来了）",r"叠数 12 取 1："); g.tap(P(148,53)); g.dbl(201,81); g.shot()
    g.item("切到共享页，把它拖进共享页格子","收包处改道：放回背包原格、下一拍进珍宝匣；共享页不留",r"珍宝匣：r01[^\n]*拖进共享页 → 改成放回背包格[\s\S]*珍宝匣：物品 id \d+ 已回到背包 → 原版移到仓库",r"留在背包"); g.tap(P(106,53)); g.dragp((600.5,227),(81,237)); g.wait(2.5); g.shot()
    g.item("珍宝匣页再取 1 件","到背包（那一叠还是 12 个：上一步拖进共享页的那颗也回来了）",r"叠数 12 取 1："); g.tap(P(148,53)); g.dbl(201,81); g.shot()
    g.item("开着箱子双击背包里的那颗符文","原版「移动到堆叠栏」：背包少 1 格（回到珍宝匣那一叠）",("inv",-1)); g.dbl(600.5,246.5); g.wait(2.0); g.shot()
    gs.append(g)
    # t 局：背包里带着魔盒时从符文页那一叠取件——双击 / 长按 / 拖三种手法。判据读截图的背包占格（魔盒本身占 4 格，取出的符文要多占格；占格增量 = 叠数减量，就没有东西进了魔盒），叠数认取件时读到的游戏值
    # t run: with the cube in the inventory, take items from that stack on the rune page - three gestures: double-tap / long-press / drag. The criterion reads the inventory cell occupancy from the screenshot (the cube itself takes 4 cells, and runes taken out must take additional cells; cell increase = stack count decrease means nothing went into the cube), and the stack count trusts the game value read at take time
    SV=os.environ.get("UIREG_STACK_SAVE") or "save.pw_rune_stack"
    g=G("t","DM_TEST_GIVE=box:2:1@60 DM_TEST_GIVE_INV=1"+" DM_TEST_WORLD_GO=near2:2:c267@200",save=SV); g.sect("cube",0); g.t=3.5; g.tap(P(333,336),1.6); g.at("await pos"); g.t=max(g.t,13.0); g.at("note 进局",1.5); g.wait(2.0)
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.arrive(5808,4443,3,24.0); g.at("tap_unit 2 267 3",3.0); g.shot()
    g.item("点「符文」页签","切到符文页（背包里有魔盒，看截图）",r"界面页签：切 [+-]\d+ 页"); g.tap(P(148,53)); g.shot()
    g.item("双击成叠格","背包多 1 格",("inv",1,"→ 成叠格")); g.dbl(201,81); g.shot()
    g.item("长按成叠格","背包多 3 格",("inv",3,"→ 成叠格")); g.hold(201,81); g.shot()
    g.item("从成叠格拖到背包","背包多 1 格",("inv",1,"→ 成叠格")); g.dragp((201,81),(488.5,297)); g.shot()
    g.item("再双击一次","取件时游戏里那一叠是 7 个（12 − 5，没有多扣也没有少扣）",r"叠数 7 取 1"); g.dbl(201,81); g.shot()
    g.item("双击背包里的一枚符文（背包带着魔盒）","背包少 1 格",("inv",-1)); g.dbl(580.9,285.5); g.shot()
    g.item("再双击那一叠","那枚进的是这一叠（取件时叠数回到 7）、没进魔盒；背包多 1 格",("inv",1,"叠数 7 取 1")); g.dbl(201,81); g.shot()
    gs.append(g)
    # u 局：背包塞到只剩一格（魔盒 4 格 + 8 顶软帽 32 格 + 1 颗宝石 + 存档自带 2 张卷轴 = 39 格；发 9 顶软帽，第一顶会自动戴到头上）再取：第一枚取得出，之后背包满了取不出、那一叠也不能少
    # u run: fill the inventory until only one cell is left (cube 4 cells + 8 caps 32 cells + 1 gem + the 2 scrolls that come with the save = 39 cells; 9 caps are given, the first is auto-equipped on the head) and then take: the first one comes out, after that the inventory is full and nothing comes out, and that stack must not decrease either
    g=G("u","DM_TEST_GIVE=box:2:1,cap:2:9,gcv:2:1@60 DM_TEST_GIVE_INV=1"+" DM_TEST_WORLD_GO=near2:2:c267@200",save=SV); g.sect("full",0); g.t=3.5; g.tap(P(333,336),1.6); g.at("await pos"); g.t=max(g.t,13.0); g.at("note 进局",1.5); g.wait(2.0)
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.arrive(5808,4443,3,24.0); g.at("tap_unit 2 267 3",3.0); g.shot()
    g.item("点「符文」页签","切到符文页（背包只剩 1 个空格，看截图）",r"界面页签：切 [+-]\d+ 页"); g.tap(P(148,53)); g.shot()
    g.item("双击成叠格","背包多 1 格（满了）",("inv",1,"→ 成叠格")); g.dbl(201,81); g.shot()
    g.item("背包满了再双击","背包占格不变",("inv",0,"→ 成叠格")); g.dbl(201,81); g.shot()
    g.item("背包满了长按","那一叠还是 11 个（没取出来的不能凭空少）",r"叠数 11 取"); g.hold(201,81); g.shot()
    gs.append(g)
    # s 局：玩家原路——背包里的符文放进箱子（个人页普通格、符文页成叠格）再取出来。标准存档 + 当场发两枚符文，不要专用存档，真机也能跑；判据读背包占格
    # s run: the player's original path - put runes from the inventory into the stash (normal cells on the personal page, stack cells on the rune page) and take them out again. Standard save + two runes handed out on the spot, no dedicated save needed, also runs on a real device; the criterion reads inventory cell occupancy
    g=G("s","DM_TEST_GIVE=r01:2:2@60 DM_TEST_GIVE_INV=1"+" DM_TEST_WORLD_GO=near2:2:c267@200"); g.sect("put",0); g.enter(); g.wait(2.0)
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.arrive(5808,4443,3,24.0); g.at("tap_unit 2 267 3",3.0); g.shot()
    # 珍宝匣存入之后：符文 / 宝石 / 材料落进个人页（或共享页）会自动转进珍宝匣对应那一格，个人页里不留——所以没有「从个人页拖回」这一步了
    # After depositing into the Trove: a rune / gem / material landing in the personal tab (or shared tab) is moved automatically into its Trove slot and does not stay in the personal tab, so there is no "drag back from the personal tab" step any more
    g.item("背包右上角的符文拖进个人页空格","自动转进珍宝匣（个人页不留，看截图）",r"珍宝匣：物品 id \d+ 已从仓库页转进高级仓库"); g.dragp((600.5,227),(81,237)); g.wait(1.5); g.clr(); g.shot()
    g.item("个人页开着双击背包里的卷轴（不可堆叠）","进箱子当前页：背包少 1 格（卷轴在个人页里，看截图）",("inv",-1)); g.dbl(600.5,266); g.clr(); g.shot()
    g.item("点「珍宝匣」页签","切到珍宝匣页（符文那一格已有 1 枚，看截图）",r"界面页签：切 [+-]\d+ 页"); g.tap(P(148,53)); g.shot()
    g.item("珍宝匣页开着双击背包里另一枚符文","进那一叠：背包少 1 格",("inv",-1)); g.dbl(600.5,246.5); g.clr(); g.shot()
    g.item("双击珍宝匣里那一叠","取出 1 枚：背包多 1 格；取件时那一叠是 2 个（自动转进去 1 + 双击进去 1）",("inv",1,"叠数 2 取 1")); g.dbl(201,81); g.clr(); g.shot()
    g.item("从那一叠拖到背包左上角空格","再取出 1 枚：背包多 1 格",("inv",1,"→ 成叠格")); g.dragp((201,81),(424.5,267)); g.clr(); g.shot()
    gs.append(g)
    # g 局：技能绑定（从 a 局技能段拆出来，守住单局 3 分钟）。先走出营地——绑完要点一下看能不能施放，城里放不了技能
    # g run: skill binding (split out of the skill section of run a, to keep a single run within 3 minutes). Walk out of the camp first - after binding, one tap checks whether it can be cast, and skills cannot be cast in town
    g=G("g"); g.sect("bind",0); g.enter()
    for x,y,r,to in [(5788,4475,4,10),(5812,4503,4,9),(5826,4522,4,8),(5840,4560,5,8)]: g.at(f"walk_to {x} {y} {r} {to} skip",to+.5)
    g.at("pos",.3)
    # 技能绑定的三条路径：点空位、换组后的第二组、技能树里长按。流程按页面反馈推进（开页、按目标键、关页），不再固定等帧
    # Three paths for skill binding: tap an empty slot, the second set after switching sets, long-press in the skill tree. The flow advances on page feedback (open page, press target key, close page), no longer waiting a fixed number of frames
    g.item("点空技能位 @skill4（第一组）","开技能树、等选技能",r"技能绑定：技能树已开"); g.tap("@skill4",2.0); g.shot()
    g.item("在技能树里点已学技能","绑到这个钮、面板自己关",r"技能绑定：已绑到键 0x[0-9a-f]+，关功能页"); g.tap(P(397,91),2.5); g.shot()
    g.item("点刚绑好的 @skill4","施放：法力下降",cast); g.tap("@skill4",2.2); g.shot()
    g.item("点换组键、点第二组的空位 @skill4、再点技能","绑到第二组这个钮、面板自己关",r"技能绑定：技能树已开[\s\S]*技能绑定：已绑到键 0x[0-9a-f]+，关功能页"); g.tap("@swap",1.0); g.tap("@skill4",2.0); g.tap(P(397,91),2.5); g.shot()
    g.tap("@swap",1.0)
    # 两排五列逐格点：每点一格（绑上、框关、留在技能树）就再长按开一次框截图，按上面的规则读各格——点的那格要有技能，别的格子不能新亮
    # Tap the two rows of five columns cell by cell: after each cell is tapped (bound, box closed, staying in the skill tree), long-press again to open the box and take a screenshot, and read each cell by the rule above - the tapped cell must have the skill, and no other cell may newly light up
    g.tap("@menu",1.4); g.tap(P(442,17),1.5)
    g.item("技能树里长按已学技能","弹「重新绑定技能」框",r"OpenPanel ButtonBindingModal"); g.hold(397,91,1.3); g.shot(); prev=g.items[-1][4]
    for r,y in enumerate(BIND_ROWS,1):
        for c,x in enumerate(BIND_COLS,1):
            g.item(f"绑定框里点第 {r} 排第 {c} 列，再开框看","技能落在画面上的这一格",("cells",prev,(r,c))); g.tap(P(x,y),1.6); g.hold(397,91,1.3); g.shot(); prev=g.items[-1][4]
    g.item("绑定框开着点框外","框关掉、不绑、留在技能树",r"技能绑定：已取消[^\n]*留在技能树"); g.tap(P(120,200),1.5); g.shot()
    g.tap(P(615,17),1.2)
    gs.append(g)
    # h 局：技能树「综合技能」页（普通技能 / 物品技能）。术士标准存档的匕首带「+1 瘴气弹」，卸掉它术士就只剩综合技能页有东西——这一页的图标原先不在可点控件表里，点不动、长按不开框
    # h run: the skill tree 「综合技能」(General Skills) page (normal skills / item skills). The dagger in the warlock standard save has 「+1 瘴气弹」(+1 Miasma Bolt); with it unequipped only the General Skills page has anything for the warlock - the icons on this page were previously not in the tappable widget table, so they could not be tapped and long-press did not open the box
    g=G("h"); g.sect("misc",0); g.enter(); g.tap("@menu",1.4)
    g.item("双击武器位卸下匕首","卸到背包",r"界面双击：身上的"); g.dbl(257,119); g.shot()
    g.tap(P(615,17),1.2)
    g.item("点空技能位 @skill4","开技能树、等选技能",r"技能绑定：技能树已开"); g.tap("@skill4",2.0); g.shot()
    g.item("在「综合技能」页点一个普通技能","绑上、面板自己关",r"技能绑定：已绑到键 0x80，关功能页"); g.tap(P(428,54),1.2); g.tap(P(275,114),2.5); g.shot()
    g.item("长按刚绑的那个技能开绑定框","框里它在第 1 排第 5 列（@skill4 那个钮）——读画面，不读日志",("cellhas",(1,5))); g.tap("@menu",1.4); g.tap(P(442,17),1.5); g.tap(P(428,54),1.2); g.hold(275,114,1.3); g.shot()
    g.tap(P(120,200),1.5)
    g.item("「综合技能」页长按另一个技能","开绑定框",r"OpenPanel ButtonBindingModal"); g.hold(301,114,1.3); g.shot()
    g.tap(P(120,200),1.5); g.tap(P(615,17),1.2)
    gs.append(g)
    # shl 局：暂停菜单「分享诊断日志」。测试开关让分享面板弹出 3 秒后自己收起（真机截不到系统弹层，只读日志）；导出的文件另拷一份到容器 Documents/sharelog_test.txt
    # shl run: pause menu "Share diagnostic log". A test switch lets the share sheet dismiss itself 3 seconds after it appears (system overlays cannot be captured on a device, only the log is read); the exported file is also copied to Documents/sharelog_test.txt in the container
    g=G("shl","DM_TEST_SHARELOG_CLOSE=3"); g.sect("sharelog",0); g.enter(); g.tap("@menu",1.4); g.tap(P(551,17))
    g.item("暂停菜单点「分享诊断日志」","两份日志打成一个文件（本局那份非空）、分享面板弹出",r"诊断日志：已打包 \S+（\d+ 字节：本局 [1-9]\d*、[^\n]*\n(?:[^\n]*\n){0,40}?[^\n]*诊断日志：分享面板已弹出"); g.at("tap_ui MessageLog@PauseLayoutGarden 3",2.5); g.shot()
    g.item("等 3 秒","分享面板到点自己收起",r"到点收起分享面板"); g.wait(3.0); g.shot()
    g.item("再点一次「分享诊断日志」","又能打包（上一次收起后没卡在忙状态）",r"诊断日志：已打包"); g.at("tap_ui MessageLog@PauseLayoutGarden 3",5.5); g.shot()
    gs.append(g)
    # i 局：雇佣兵（第一幕卡夏；第二幕格雷兹是同一个雇佣面板）。最小步骤来自一条真问题：雇佣名单里同一行连点几下、雇成之后面板栈的计数没清，摇杆和技能钮回不来
    # i run: hiring a mercenary (Act 1 Kashya; Act 2 Greiz is the same hire panel). The minimal steps come from a real problem: tapping the same row of the hire list several times, the panel stack count was not cleared after the hire succeeded, and the stick and skill buttons did not come back
    g=G("i","DM_TEST_GOLD=200000 DM_TEST_WORLD_GO=near2:1:c150@200"); g.sect("hire",0); g.enter(1)
    g.item("让人走到卡夏旁","游戏自己寻路走到",r"\[script\] await pos：区域 1",r"走不过去"); g.arrive(5838,4454,10,45.0)
    g.item("点卡夏","出菜单（交谈 / 复活 / 雇佣 / 取消，看截图）",r"OpenPanel NpcDialogPanel"); g.at("tap_unit 1 150 3",3.0); g.shot()
    # 这个圣骑的佣兵是死的，卡夏的菜单是四行：交谈 / 复活某某 / 雇佣 / 取消。先点第二行把佣兵复活（菜单随后重排成三行），同一位置才变成「雇佣」
    # This paladin's mercenary is dead, and Kashya's menu has four rows: Talk / Resurrect so-and-so / Hire / Cancel. First tap the second row to resurrect the mercenary (the menu is then rearranged into three rows), and only then does the same position become 「雇佣」(Hire)
    g.item("点菜单第二行「复活佣兵」","复活：发出复活包、菜单重排成三行（看截图）",r"c2s 52\b"); g.at(f"tap_unit 1 150 2 +{MENU['kashya2']}",2.5); g.shot()
    g.item("点菜单第二行「雇佣」","开雇佣名单",r"OpenPanel HireMenuPanel"); g.at(f"tap_unit 1 150 2 +{MENU['kashya2']}",2.5); g.shot()
    g.item("名单同一行连点 6 下","弹确认框（看截图）",None)
    for k in range(6): g.tap(P(333,114),0.4)
    g.wait(1.0); g.shot()
    g.item("确认框点「是」","雇成：发出雇佣包",r"c2s 36\b"); g.tap(P(286,203),2.5); g.shot()
    g.item("点面板外","雇佣名单与对话菜单都关掉",r"ClosePanel (HireMenuPanel|NpcDialogPanel)"); g.tap(P(100,300),1.5); g.tap(P(100,300),1.5); g.shot()
    g.item("推摇杆","人能走：坐标变了",moved); g.at("pos",.2); g.at("down 1 @joy",.3); g.at("move 1 @joy+60,0",1.5); g.at("up 1",.5); g.at("pos",.3)
    gs.append(g)
    # j 局：镶嵌。测试开关给 7 顶软帽、2 枚一号符文、1 颗宝石并当场进背包；种子必须固定（软帽带不带孔是随机的）。
    # 固定种子下的布局（667 宽基准）：头上自动戴的那顶 0 孔；背包软帽 (376,228) 1 孔、(318,267) 2 孔、(395,267) 2 孔；符文 (308,219)、(308,238)；拖放松手点 = 目标下方约 38 点。这些是拖放用的点（有余量）；读「格子空不空」要用格子正中心：符文两格是 (304,225)、(304,245)——偏几个点就压到网格线上，空格也会读成有东西。
    # 日志里没有「镶嵌成功」这样的字，判据看发出的命令包（0x28 = 镶嵌，0x1f = 交换，0x19 / 0x18 = 拿起 / 放下）加「符文还在不在原格」；镶完的名字与属性看截图里的说明
    # j run: socketing. The test switch gives 7 caps, 2 no. 1 runes and 1 gem, straight into the inventory; the seed must be fixed (whether a cap has sockets is random).
    # Layout under the fixed seed (667-wide base): the auto-equipped one on the head has 0 sockets; inventory caps (376,228) 1 socket, (318,267) 2 sockets, (395,267) 2 sockets; runes (308,219), (308,238); drag-and-drop release point = about 38 points below the target. These are points for drag-and-drop (with margin); to read "is the cell empty" use the exact cell center: the two rune cells are (304,225), (304,245) - a few points off lands on the grid line, and an empty cell also reads as having something.
    # The log has no text like "socketing succeeded"; the criterion looks at the command packets sent (0x28 = socket, 0x1f = swap, 0x19 / 0x18 = pick up / put down) plus "is the rune still in its original cell"; the name and attributes after socketing are checked in the description in the screenshot
    g=G("j","DM_TEST_GIVE=cap:2:7,r01:2:2,gcv:2:1@60 DM_TEST_GIVE_INV=1"); g.sect("socket",0); g.enter(); g.wait(2.0); g.tap("@menu",1.4)
    g.item("背包布局","软帽、符文、宝石都在上面写的位置（看截图）",None); g.shot()
    g.item("符文拖到 1 孔软帽上","镶进去：发镶嵌包、不是交换",r"c2s 28\b",r"c2s 1f\b"); g.dragp((308,238),(376,266)); g.shot()
    g.item("那枚符文原来的格子","空了",("spot","empty",(304,245))); g.shot()
    g.item("单击那顶软帽","说明里是「镶宝软帽」、带符文名（看截图）",None); g.tap(P(376,228),1.5); g.shot()
    g.item("另一枚符文拖到头部位（头上那顶没孔）","放不进：回原格",r"原版判定 0[\s\S]*放到被拖物品腾出的原位",r"c2s 28\b"); g.dragp((308,219),(333,107)); g.shot()
    g.item("那枚符文原来的格子","还在",("spot","filled",(304,225))); g.shot()
    g.item("把 2 孔软帽拖到头部位","换上",r"换下来的那件[^\n]*放到被拖物品腾出的原位",r"原版判定 0"); g.dragp((318,267),(333,107)); g.shot()
    g.item("符文拖到头部位（头上那顶有孔）","镶进身上的帽子：发镶嵌包",r"c2s 28\b",r"c2s 1f\b"); g.dragp((308,219),(333,107)); g.shot()
    g.item("那枚符文原来的格子","空了",("spot","empty",(304,225))); g.shot()
    gs.append(g)
    # k 局：符文之语「天底」——四号、三号符文按这个顺序镶进 2 孔软帽（顺序反了只是普通镶嵌，原版规则）
    # k run: runeword 「天底」(Nadir) - runes no. 4 and no. 3 are socketed into a 2-socket cap in this order (with the order reversed it is just ordinary socketing, the original game's rule)
    g=G("k","DM_TEST_GIVE=cap:2:7,r04:2:1,r03:2:1@60 DM_TEST_GIVE_INV=1"); g.sect("runeword",0); g.enter(); g.wait(2.0); g.tap("@menu",1.4)
    g.item("背包布局","软帽与两枚符文在位（看截图）",None); g.shot()
    g.item("第一枚符文拖到 2 孔软帽上","镶进去",r"c2s 28\b",r"c2s 1f\b"); g.dragp((308,238),(395,305)); g.shot()
    g.item("第二枚符文拖到同一顶软帽上","镶进去",r"c2s 28\b",r"c2s 1f\b"); g.dragp((308,219),(395,305)); g.shot()
    g.item("两枚符文原来的格子","都空了",("spot","empty",(304,245))); g.shot()
    g.item("（同上）上面那格","空了",("spot","empty",(304,225))); g.shot()
    g.item("单击那顶软帽","说明第一行是「天底」、带「+50% 增强防御」（旧译名缺省开着；关掉 item_notes 是原版的「强化防御」。看截图）",None); g.tap(P(395,267),1.5); g.shot()
    gs.append(g)
    # f 局：带佣兵的角色（标准存档第 4 行 88 级法师）——佣兵头像栏 HireablesPanel 常开在最上层，物品栏里的拖放不该因此被挡。
    # 背包（667 宽基准）：左上角 1×1 小护身符 (245,225)，第 7 列第 1 行是空格 (364,225)。
    # f run: a character with a mercenary (level 88 sorceress in row 4 of the standard save) - the mercenary portrait bar HireablesPanel is always open on the topmost layer, and drag-and-drop in the inventory panel must not be blocked because of it.
    # Inventory (667-wide base): 1×1 small charm at the top-left corner (245,225), column 7 row 1 is an empty cell (364,225).
    g=G("f"); g.sect("merc",0); g.enter(4); g.tap("@menu",1.4)
    g.item("带佣兵的角色开物品栏","佣兵头像栏在、物品栏开着（看截图）",None); g.shot()
    g.item("把背包里的小护身符拖到空格","拿得起、放得下；日志没有「不放行」",r"c2s 19[\s\S]*c2s 18"); g.dragp((245,225),(364,265)); g.shot()
    g.item("再拖回原来的格子","拿得起、放得下",r"c2s 19[\s\S]*c2s 18"); g.dragp((364,225),(245,265)); g.shot()
    # 佣兵页外框右上角的关闭 X：点了两个面板都关、回到游戏画面；「真关了」另用推摇杆人能走来证
    # The close X at the top-right corner of the mercenary page outer frame: tapping it closes both panels and returns to the game view; "really closed" is additionally proven by pushing the stick and the player being able to walk
    g.item("点页签条「佣兵」","开佣兵页，外框右上角有 X（看截图）",r"OpenPanel HirelingInventoryPanel"); g.tap(P(469,18),1.5); g.shot()
    g.item("点佣兵页右上角 X","佣兵页关掉、回到游戏画面、不留空外框（看截图）",r"ClosePanel HirelingInventoryPanel"); g.tap(P(615,17),1.5); g.shot()
    g.item("关掉后推摇杆","人能走：坐标变了（面板真关了）",moved); g.at("pos",.2); g.at("down 1 @joy",.3); g.at("move 1 @joy+60,0",1.5); g.at("up 1",.5); g.at("pos",.3)
    gs.append(g)
    # map 局（线上：设置选全屏无效、点成全屏后开装备栏 / 换图就回小地图）：触屏模式记住玩家要的地图档，面板开着先让开、关掉 / 换区域后回到玩家要的那档
    # map run (report: choosing full screen had no effect, and after switching to full screen, opening the inventory / changing area went back to the minimap): touch mode remembers the map the player wants, steps aside while a panel is open, and returns to it after the panel closes / after an area change
    g=G("map"); g.sect("map",0); g.enter(2)
    g.item("进局","在营地；缺省档是小地图（「读到 2」那行在进局前打，看截图）",r"pos 区域 1 "); g.at("pos",.3); g.shot()
    g.item("点区域名那一条","切全屏，记住全屏",r"地图：玩家点了 → 记住全屏地图"); g.tap("@mmname",1.5); g.shot()
    g.item("点底栏十字开物品栏","全屏地图先让成小地图",r"地图：→ 微缩放右（面板开着先让开"); g.tap("@menu",1.5); g.shot()
    g.item("点右上角 X 关物品栏","回到全屏地图",r"地图：→ 全屏（玩家要全屏"); g.tap(P(615,17),1.5); g.shot()
    g.item("走到出生点旁的箱子再出营地（换区域）","到了野外，仍是全屏地图（看截图）",r"pos 区域 2 "); g.at("walk_to 5808 4443 6 15 skip",15.5)
    for x,y,r,to in [(5788,4475,4,12),(5812,4503,4,9),(5826,4522,4,8),(5840,4560,5,8)]: g.at(f"walk_to {x} {y} {r} {to} skip",to+.5)
    g.wait(2.0); g.at("pos",.3); g.shot()
    g.item("全屏地图再点区域名那一条","回小地图，记住小地图",r"地图：玩家点了 → 记住小地图"); g.tap("@mmname",1.5); g.shot()
    g.item("开、关一次物品栏","关掉后仍是小地图",r"ClosePanel PlayerInventoryExpansionLayout",r"地图：→ 全屏"); g.tap("@menu",1.5); g.tap(P(615,17),1.5); g.shot()
    gs.append(g)
    # mapf 局：选项页把「透视地图尺寸」选成全屏（测试开关 DM_TEST_AUTOMAP 到帧改配置并调原版回调，同选项页那条路）→ 触屏下直接全屏地图，配置值不被改写
    # mapf run: "automap size" set to full screen on the options page (test switch DM_TEST_AUTOMAP changes the config at a frame and calls the original callback, the same path as the options page) → in touch mode the full-screen map shows directly and the config value is not rewritten
    g=G("mapf",env="DM_TEST_AUTOMAP=2400:0"); g.sect("mapf",0); g.enter(2); g.t=max(g.t,38.0)
    g.item("选项里选了全屏","直接全屏地图，不改回小地图档",r"地图：「透视地图尺寸」读到 0 → 触屏下缺省全屏地图",r"改回微缩地图放右"); g.at("pos",.3); g.wait(12.0); g.shot()   # 测试开关在帧 2400（约脚本第 46 秒）改档，这一项要把它框进来
    g.item("点底栏十字开物品栏","全屏地图先让成小地图",r"地图：→ 微缩放右（面板开着先让开"); g.tap("@menu",1.5); g.shot()
    g.item("点右上角 X 关物品栏","回到全屏地图",r"地图：→ 全屏（玩家要全屏"); g.tap(P(615,17),1.5); g.shot()
    g.item("全屏地图点空处","仍是全屏地图（点地面走路不再切回小地图，#186）",r"pos 区域 \d+",r"→ 回小地图|记住小地图"); g.tap(P(300,150)); g.at("pos",.3); g.shot()
    g.item("全屏地图再点区域名那一条","回小地图（小地图还在，点得回来）",r"地图：玩家点了 → 记住小地图"); g.tap("@mmname",1.5); g.shot()
    g.item("点区域名那一条","又切回全屏",r"地图：玩家点了 → 记住全屏地图"); g.tap("@mmname",1.5); g.shot()
    gs.append(g)
    # ms 局：佣兵加装备位（秘境最高通关 ≥ 10 层解锁，DM_TEST_MERC_SLOTS=1 当作已解锁）。带活佣兵的老角色（第 4 行 88 级法师：旧数据版本，走 base/ 那份物品栏表）。
    # 给腰带、手套进背包（这个角色背包只剩这点空位），测试开关替玩家发原版「从背包格给佣兵穿」的包；读佣兵身上的物品；保存退出、同一角色再进一局再读——两件还在各自的部位上。
    # 老角色的暂停菜单是 PauseLayout（不是 PauseLayoutGarden）
    # ms run: extra mercenary equipment slots (unlocked at highest rift level cleared ≥ 10; DM_TEST_MERC_SLOTS=1 treats them as unlocked). An old character with a living mercenary (row 4, the level 88 Sorceress: old data version, using the base/ copy of the inventory table).
    # A belt and gloves are given into the inventory (this character's inventory only has this much room left), and the test switch sends the original game's "equip the mercenary from an inventory cell" packet on the player's behalf; the items on the mercenary are read; save and exit, enter another run with the same character and read again -- both items are still at their own body locations.
    # The old character's pause menu is PauseLayout (not PauseLayoutGarden)
    def worn(st,bd): 
        m=re.findall(r"佣兵装备位：拍 \d+（定时）佣兵[^\n]*",st); 
        if not m: return "不通：没有佣兵读数（没有佣兵？）"
        miss=[k for k in ("lbl 部位 8","lgl 部位 10") if k not in m[-1]]
        return "通：腰带 8 / 手套 10 都在佣兵身上" if not miss else "不通：缺 "+"、".join(miss)
    g=G("ms","DM_TEST_GIVE=lbl:2:1,lgl:2:1@60 DM_TEST_GIVE_INV=1 DM_TEST_MERC_SLOTS=1 DM_TEST_MERC_EQUIP=200:lbl:8;220:lgl:10 DM_TEST_MERC_DUMP=180,300"); g.sect("mercslot",0); g.enter(4)
    g.item("进局后等 12 秒","两件穿到佣兵的新部位上：日志两次「发完物品模式 1」，读数里两件各在其位；不带红",lambda st,bd: ("不通：有红" if re.search(r"【红】",st) else "不通：没穿上（发完物品模式不是 1）" if len(re.findall(r"替玩家发 0x5b：[^\n]*发完物品模式 1",st))!=2 else worn(st,bd))); g.wait(12.0); g.at("pos",.3); g.tap("@menu",1.4); g.tap(P(469,18),1.5); g.shot()
    g.item("保存并退出","回选角页",r"FrontEndNavigation ToFrontEnd"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayout 3",5.0); g.shot()
    g.item("同一角色再进一局，等 14 秒","读档后两件还在佣兵的新部位上；不带红",lambda st,bd: ("不通：没进局" if "FrontEndNavigation ToGame" not in st else "不通：有红" if re.search(r"【红】",st) else worn(st,bd))); g.ui("ListItem3@CharacterSelectPanel",1.3); g.tap(P(333,336),1.6); g.tap(P(333,157),8.0); g.wait(14.0); g.at("pos",.3); g.shot()
    gs.append(g)
    # m 局：商人页开着时把物品拖到商人面板任意位置都卖出。现场：软帽 2 顶（第一顶自动戴上）、鉴定卷轴、魔盒当场进背包，走到恰西开「交易或修复」。
    # 判据读身上金币（游戏属性 14）与截图里的背包占格，不认发包；金币差另核 = 游戏给的卖价。松手点 = 目标下方 40 点
    # m run: with the vendor page open, dragging an item anywhere onto the vendor panel sells it. Setup: 2 caps (the first is auto-equipped), an identify scroll and the cube go into the inventory on the spot, walk to Charsi and open 「交易或修复」(trade/repair).
    # The criterion reads the gold carried (game stat 14) and the inventory cell occupancy in the screenshot, and does not accept packets sent; the gold difference is checked separately = the sell price given by the game. Release point = 40 points below the target
    g=G("m","DM_TEST_GIVE=cap:2:2,isc:2:1,box:2:1@60 DM_TEST_GIVE_INV=1 DM_TEST_WORLD_GO=near2:1:c154@200 DM_TEST_STAT_LOG=14"); g.sect("sell",0); g.enter(); g.arrive(5776,4433,10,60.0)
    g.at("tap_unit 1 154 3",5.0); g.at("tap_unit 1 154 2",2.0)
    g.item("恰西三行菜单点「交易或修复」","进商人页（背包布局看截图）",r"OpenPanel VendorPanelLayout"); g.at(f"tap_unit 1 154 2 +{MENU['trade3m']}",2.5); g.shot()   # 真全屏下菜单贴界面顶边、行的位置固定（三行的中心在视图 y 124 / 200 / 277），偏移 = 行中心 − 单位的屏幕 y，单位的屏幕位置随这一局的站位变：从出生点直接走来时恰西在 y 162（b 局先去过瓦瑞夫，站位不同，用 trade3） | in true fullscreen the menu sticks to the top edge of the interface and the row positions are fixed (the centers of the three rows are at view y 124 / 200 / 277), offset = row center − the unit's screen y, and the unit's screen position varies with the standing position in this run: walking straight from the spawn point Charsi is at y 162 (run b went to Warriv first, the standing position differs, so it uses trade3)
    g.item("背包里的卷轴拖到商人面板底板","卖出：金币增加、背包少 1 格",("inv",-1,gold('+'))); g.dragp((539,277),(203,340)); g.clr(); g.shot()
    g.item("软帽拖到页签条","卖出：金币增加、背包占格减少、页签没切",("inv",'-',gold('+',r"界面页签：切"))); g.dragp((550,230),(170,94)); g.clr(); g.shot()
    g.item("魔盒（不可卖）拖到有货的货架格","不成交：金币不变、背包占格不变；屏幕上有「此处无法交易物品」（看前一张截图）",("inv",0,gold(0))); g.dragp((397,230),(131,135)); g.at(f"shot {g.name}_tip.png",.3); g.clr(); g.shot()
    g.item("魔盒原来的格子","魔盒回到原格",("spot","filled",(386,227))); g.shot()
    g.item("另一张卷轴拖到货架空格","卖出：金币增加、背包少 1 格",("inv",-1,gold('+'))); g.dragp((558,277),(285,239)); g.clr(); g.shot()
    g.item("身上的匕首从武器位拖到商人面板底板","卖出：金币增加、背包占格不变、武器位空了（看截图）",("inv",0,gold('+'))); g.dragp((399,107),(203,340)); g.clr(); g.shot()
    g.item("点右上角 X","关商人页",r"ClosePanel VendorPanelLayout"); g.tap(P(615,17)); g.shot()
    gs.append(g)
    # n 局：货架长按 = 原版的大量购买。现场：回城书、钥匙 1 把当场进背包，30 级、身上 20 万金币，走到阿卡拉开「交易」（默认停在杂项页）。
    # 货架右列（667 基准）：回城卷轴 (284,140)、鉴定卷轴 (284,159)、钥匙 (284,177)、红药 (284,196)。长按 = 按 0.8 秒松手，之后留 3 秒（宿主替玩家按住 Y 约 1.3 秒）
    # n run: long-press on the shelf = the original game's bulk purchase. Setup: a tome of town portal and 1 key go into the inventory on the spot, level 30, carrying 200,000 gold, walk to Akara and open 「交易」(Trade) (stops on the misc page by default).
    # Right column of the shelf (667 base): town portal scroll (284,140), identify scroll (284,159), key (284,177), red potion (284,196). Long-press = press for 0.8 seconds and release, then leave 3 seconds (the host holds Y for the player for about 1.3 seconds)
    AK="DM_TEST_GIVE=tbk:2:1,key:2:1@60 DM_TEST_GIVE_INV=1 DM_TEST_STAT=12=30,14=200000 DM_TEST_WORLD_GO=near2:1:c148@200 DM_TEST_STAT_LOG=14"
    def akara(g):
        g.arrive(5874,4422,4,60.0); g.at("tap_unit 1 148 3",4.0); g.at("tap_unit 1 148 2",2.5); g.at("tap_unit 1 148 2",2.5); g.at("tap_unit 1 148 2",2.5)
        g.item("阿卡拉三行菜单点「交易」","进商人页、停在杂项页（背包布局看截图）",r"OpenPanel VendorPanelLayout"); g.at(f"tap_unit 1 148 2 +{MENU['akara2']}",2.5); g.shot()
    def lp(g,x,y): g.hold(x,y,.8); g.wait(2.0); g.clr(); g.shot()
    g=G("n",AK); g.sect("bulk",0); g.enter(); akara(g)
    g.item("货架上单击红药","出说明、不买：金币不变、背包占格不变",("inv",0,gold(0))); g.tap(P(284,196),1.5); g.clr(); g.shot()
    g.item("长按钥匙（背包里有 1 把）","买满进已有那一叠：金币减少、背包不多出新叠",("inv",0,gold('-'))); lp(g,284,177)
    g.item("单击背包里的钥匙","说明里数量是 12（满叠；看 key 那张截图）",None); g.tap(P(542.4,285.5),1.5); g.at(f"shot {g.name}_key.png",.3); g.clr(); g.shot()
    g.item("长按回城卷轴（背包里有回城书）","买满进书：金币减少、背包占格不变",("inv",0,gold('-'))); lp(g,284,140)
    g.item("长按鉴定卷轴（没有鉴定书）","只买 1 份：金币减少、背包多 1 格",("inv",1,gold('-'))); lp(g,284,159)
    g.item("长按红药（腰带满）","只买 1 瓶进背包：金币减少、背包多 1 格、腰带不变",("inv",1,both(gold('-'),belt(0)))); lp(g,284,196)
    g.item("双击红药","买 1 瓶：金币减少、背包多 1 格",("inv",1,gold('-'))); g.dbl(284,196); g.wait(1.0); g.clr(); g.shot()
    g.item("点右上角 X","关商人页",r"ClosePanel VendorPanelLayout"); g.tap(P(615,17)); g.shot()
    gs.append(g)
    # p 局：腰带有空槽时长按货架上的药水 = 补腰带（同上提交）。进局先喝掉腰带前两格
    # p run: with an empty belt slot, long-pressing a potion on the shelf = refill the belt (same commit as above). On entering the run first drink the first two belt slots
    g=G("p",AK); g.sect("belt",0); g.enter()
    g.item("喝掉腰带第 1、2 格","腰带 0/0/1/1",r"腰带 0/0/1/1"); g.tap("@belt1",1.0); g.tap("@belt2",2.0); g.shot()
    akara(g)
    g.item("长按红药（腰带有 2 个空槽）","两个空槽一次补满：腰带瓶数增加、金币减少、背包占格不变",("inv",0,both(gold('-'),belt('+')))); lp(g,284,196)
    g.item("再长按红药（腰带已补满）","买 1 瓶进背包：金币减少、背包多 1 格、腰带不变",("inv",1,both(gold('-'),belt(0)))); lp(g,284,196)
    g.item("点右上角 X","关商人页（腰带看截图）",r"ClosePanel VendorPanelLayout"); g.tap(P(615,17)); g.shot()
    gs.append(g)
    # ra 局：商人页「全部修理」（单件修理钮去掉、不补，交易页只留全部修理，挪到左格；右格是刷新货架）。
    # 现场：标准存档第 4 行 88 级法师，左手盾耐久 85/86（身上另有两件也不满：23/24、11/12），全部修理一次修完、不出确认框。
    # 判据：金币减少（三件的修理费合计，不止盾的 117）；盾耐久读 DM_TEST_DUR 的记录行——拍数写到一局跑不到的远处，只借它记数、不压耐久。这个法师见过恰西，点她第一下就出菜单
    # ra run: 「全部修理」(Repair All) on the vendor page (the single-item repair button is removed and not replaced, the trade page keeps only Repair All, moved to the left cell; the right cell is refresh shelf).
    # Setup: level 88 sorceress in row 4 of the standard save, left-hand shield durability 85/86 (two other equipped items are also not full: 23/24, 11/12), Repair All repairs everything in one go, with no confirmation box.
    # Criterion: gold decreases (the total repair cost of the three items, not just the shield's 117); shield durability is read from the DM_TEST_DUR record line - the tick number is set far beyond what one run can reach, borrowing it only to record the value, not to lower durability. This sorceress has met Charsi, so the first tap on her brings up the menu
    RE="DM_TEST_WORLD_GO=near2:1:c154@200 DM_TEST_STAT_LOG=14 DM_TEST_DUR=1@999999999"
    def charsi(g):
        g.enter(4); g.arrive(5776,4433,10,60.0); g.at("tap_unit 1 154 3",5.0)
        g.item("恰西三行菜单点「交易或修复」","进商人页",r"OpenPanel VendorPanelLayout"); g.at(f"tap_unit 1 154 2 +{MENU['trade3m']}",2.5); g.shot()
    g=G("ra",RE); g.sect("repall",0); charsi(g)
    g.item("交易页布局","左格全部修理、右格刷新货架、没有单件修理（看截图）",None); g.clr(); g.shot()
    g.item("点左格「全部修理」","全修：金币减少、盾耐久 86 / 86",both(gold('-'),dur(86))); g.tap(P(104,306),3.0); g.shot()
    g.item("再点一次全部修理","都满了：不再扣钱",gold(0)); g.tap(P(104,306),3.0); g.shot()
    g.item("点右上角 X","关商人页",r"ClosePanel VendorPanelLayout"); g.tap(P(615,17)); g.shot()
    gs.append(g)
    # q 局：佣兵页开着双击药水 = 喂佣兵；佣兵页关着双击药水 = 玩家自己喝。同一个 88 级法师（玩家生命 713/809 不满，佣兵 2142/2142 满）。
    # 判据：腰带瓶数、玩家生命读 dm_hud 行——药少了一瓶而不满血的玩家生命没动 = 药没给玩家。
    # 佣兵生命读 DM_TEST_MERC_HP 的记录行：进局第 80 拍把佣兵压到三成，喂药后要涨。不带这个开关的旧包上佣兵是满血、也没有读数，这一条报未判。换头盔造不出掉血——把「+60 生命」的头冠换下再换回，当前生命跟着回到 2142。
    # 超级治疗药水是持续回复：佣兵生命每 25 拍涨一截，要隔几秒读；所以后一步不判「佣兵不变」（前一瓶的药效还在涨）。原版只把生命 / 解毒 / 解冻类的药给佣兵，法力药仍是玩家喝——这一局只测红药
    # 佣兵页属性表的「生命」与左上角头像血条显示的是服务端上次通知的百分比：进局后第 250 拍才有第一次通知，之后每 30 拍轮询、差够 9 个点才发（日志有「上次发过的百分比 a → b」）。
    # 所以压血（拍 80）后要等过拍 250 再开页截图——早了看到的还是 2142 / 2142（两遍都截早了，误报成「面板不跟」）
    # 两项有连带：佣兵页那一步要是被玩家错喝了，玩家就满血，后一步「玩家自己喝」的生命读数不再涨，也会报不通
    # 别在这一局用 clr()：法师背包那一格是护身符，点了出说明框
    # q / bdm 验的是「手动」双击腰带喂佣兵：佣兵自动喂药缺省开之后，压到三成的佣兵会先被自动喂满，手动那一下读不出涨（两档实测 2142 → 2142）。
    # 这两局把自动喂药关掉只为把手动路径单独量出来；自动喂药本身由专项六格（人物 / 佣兵各三格）与缺省开着的其余各局守。
    # Case q: double-tap a potion with the mercenary page open = feed the mercenary; with the mercenary page closed = the player drinks. Same level-88 Sorceress (player Life 713/809 not full, mercenary 2142/2142 full).
    # Criteria: belt potion count and player Life are read from the dm_hud line: one potion gone while the not-full player's Life did not move = the potion did not go to the player.
    # Mercenary Life is read from the DM_TEST_MERC_HP log line: on tick 80 after entry the mercenary is pushed down to 30%, and must rise after feeding. On older builds without this switch the mercenary is at full Life and there is no readout, so this item reports undetermined. Swapping the helm cannot cause Life loss: taking off the "+60 Life" tiara and putting it back returns current Life to 2142.
    # Super Healing Potion heals over time: mercenary Life rises a step every 25 ticks and must be read a few seconds later; so the next step does not assert "mercenary unchanged" (the previous potion is still healing). The game only gives Healing / Antidote / Thawing potions to the mercenary, Mana potions are still drunk by the player; this case tests Healing potions only
    # The "Life" in the mercenary page stat table and the portrait Life bar at top left show the percentage last notified by the server: the first notice only comes on tick 250 after entry, then it polls every 30 ticks and sends only on a 9-point difference (the log has "last sent percentage a → b").
    # So after lowering Life (tick 80), wait past tick 250 before opening the page for a screenshot; too early still shows 2142 / 2142 (both early runs captured too soon and were misreported as "panel does not follow")
    # The two items are coupled: if the player wrongly drinks in the mercenary-page step, the player is at full Life, the Life reading of the later "player drinks" step no longer rises, and it also reports a failure
    # Do not use clr() in this case: that inventory cell of the Sorceress holds a charm, and tapping it brings up the tooltip
    # q / bdm verify "manual" double-tap belt feeding of the mercenary: with mercenary auto-feed on by default, a mercenary pushed down to 30% is auto-fed to full first, and the manual tap shows no rise (measured on both profiles 2142 → 2142).
    # These two cases turn auto-feed off only to measure the manual path in isolation; auto-feed itself is covered by the six dedicated cells (three each for character / mercenary) and by all the other cases where it stays on by default.
    g=G("q","DM_TEST_MERC_HP=30@80 DM_MERC_POT=0"); g.sect("mercpot",0); g.enter(4); g.wait(12.0); g.tap("@menu",1.4)
    g.item("点页签条「佣兵」","开佣兵页；属性表「生命」约 621 / 2142、左上角头像血条只剩三成（看截图）",r"OpenPanel HirelingInventoryPanel"); g.tap(P(469,18),1.5); g.shot()
    g.item("佣兵页开着双击腰带里的红药","喂佣兵：腰带少 1 瓶、玩家生命不变、佣兵生命读数涨；属性表「生命」也涨、左上角头像血条变长（对比上一张截图）",both(belt('-'),php(0),merc('+'))); g.dbl(486,197); g.wait(5.0); g.shot()
    g.tap(P(615,17),1.5); g.tap("@menu",1.4)
    g.item("佣兵页关着（只开物品栏）双击腰带里的红药","玩家自己喝：腰带少 1 瓶、玩家生命涨",both(belt('-'),php('+'))); g.dbl(346,198); g.wait(2.0); g.shot()
    g.item("点右上角 X","面板关闭",r"ClosePanel PlayerInventoryExpansionLayout"); g.tap(P(615,17)); g.shot()
    gs.append(g)
    # ---- 世界点选：出口砖与物件的可点范围 ----
    # 物件的可点范围 = 身体框 + 占地菱形，再并上游戏自己的选择框；点位取自 DM_WORLD_DUMP=1 的候选行（iPhone 比例 1334 宽像素 ÷ 2 = 667 基准）。
    # iPad 真全屏下世界铺满整窗、物件的屏幕位置不同，点位还没量：那一档这几局只做「进现场 + 点一下空地打出候选行」。
    # 存档副本要先放进 App 容器 Data 下：save.rg_act4（第四幕，小站全开）、save.rg_act4done（第四幕已通关）、save.rg_joejoe（19 级术士，鲁高因）。
    # ---- World tap selection: tappable range of exit tiles and objects ----
    # An object's tappable range = body box + footprint diamond, unioned with the game's own selection box; tap positions are taken from the candidate lines of DM_WORLD_DUMP=1 (iPhone ratio, 1334 pixels wide ÷ 2 = 667 base).
    # In iPad true fullscreen the world fills the whole window and objects' screen positions differ, and the tap positions have not been measured yet: in that mode these runs only do "get into the test setup + tap empty ground once to print the candidate lines".
    # Save copies must first be placed under Data in the App container: save.rg_act4 (Act 4, all waypoints open), save.rg_act4done (Act 4 completed), save.rg_joejoe (level 19 warlock, Lut Gholein).
    HP="DM_TEST_STAT=7=2000000000,6=2000000000 DM_TEST_POS_LOG=1"; PH=MODE=="iphone"
    NOHIT=r"世界目标：点 \([^\n]*没有目标，不发令"
    def probe(g): g.item("（这一档点位还没量）点左下角空地一下","日志打出候选行",None); g.at("pos",.2); g.tap(P(170,150),2.5); g.shot()
    # pk 局：营地箱子（标准存档术士，出生点不动）。箱子身体框中心 (237.5,127.5)、选择框中心 (235,138.5) 56.5×58
    # iPad 真全屏（世界铺满整窗，视图点 = 窗口点减上方 256）：身体框中心 (205.6,45.2)、选择框中心 (201.9,59.3) 75×77
    # pk run: camp stash (standard-save warlock, spawn point not moved). Stash body box center (237.5,127.5), selection box center (235,138.5) 56.5×58
    # iPad true fullscreen (the world fills the whole window, view points = window points minus the 256 above): body box center (205.6,45.2), selection box center (201.9,59.3) 75×77
    PK={"iphone":((198,138),(237,174),(211,115)),"ipadfull":((158,59),(205,103),(169,26))}.get(MODE)
    g=G("pk"); g.sect("pick",0); g.enter()
    if PK:
        g.item("点箱子选择框左边外面一点","不中：没有目标、不开箱",NOHIT,r"OpenPanel BankExpansionLayout"); g.tap(P(*PK[0]),2.0); g.shot()
        g.item("点箱子选择框下边外面一点","不中：没有目标、不开箱",NOHIT,r"OpenPanel BankExpansionLayout"); g.tap(P(*PK[1]),2.0); g.shot()
        g.item("点选择框左上角里面（身体框与占地菱形之外）","命中箱子：走过去开箱",r"OpenPanel BankExpansionLayout"); g.tap(P(*PK[2]),4.0); g.shot()
    else: probe(g)
    gs.append(g)
    # wp 局：进局落在城镇小站旁（进局与一键重开的出生位置移到小站旁；离小站一个安全距离、尸体同处）。产品缺省，不带 BASE 里的 DM_TOWNWP_OFF=1
    # wp run: entering a run lands next to the town waypoint (the spawn position for entering a run and for one-tap restart is moved next to the waypoint; a safe distance from the waypoint, with the corpse at the same place). Product default, without the DM_TOWNWP_OFF=1 from BASE
    g=G("wp","DM_TOWNWP_OFF=0"); g.sect("townwp",0)
    g.item("进局","进局那一拍就落在小站旁安全距离处（不再第 2 拍换关）；5 拍后人离小站横纵较大的那个 7–12 格（安全距离 10 格，找空格 ±2，转 45 度时横纵各约 7）",r"城镇小站落点：关卡 1 小站 \d+,\d+，进局落点 \d+,\d+[\s\S]*城镇小站落点：拍 \d+ 人在 \d+,\d+，小站 \d+,\d+（差 (?:-?(?:[7-9]|1[0-2]),-?\d+|-?\d+,-?(?:[7-9]|1[0-2]))）"); g.enter(); g.shot()
    g.item("点脚下的小站","开小站面板",r"OpenPanel WaypointsPanel"); g.at("tap_unit 2 119 5",4.0); g.shot()
    gs.append(g)
    # g1 / g2 / g3 / g4 局：第四幕红门（物件 566，和泰瑞尔说过话才出现；探路开关把人传到泰瑞尔旁并交互）。一局只能验一个命中点（点中就换幕到哈洛加斯，区域 109）。
    # iPhone 比例（1334 宽像素）：门身体框中心 (739,440) 补到 75 见方，选择框中心 (739,405) 124×255；凯恩 246 的身体框 (739,376) 75×119 正压在门的中段，泰瑞尔 367 在门左上 (667,291) 95×216。
    # iPad 真全屏（2732 宽像素，基准 x = 像素 ÷ 4.096，y = (像素 ÷ 2 − 256) ÷ 2.048）：门身体框中心 (1563,1201) 补到 205 见方，选择框中心 (1563,1105) 338×696；凯恩 (1563,1026) 205×325，泰瑞尔 (1366,795) 260×590。
    # 上段 = 选择框里、凯恩头顶之上；中段 = 选择框右缘那一条（凯恩与门身体框之外，只靠选择框）；下段 = 门身体框里、凯恩脚下；g4 点在凯恩身上（门的选择框也盖着）→ 该选凯恩，不换幕
    # g1 / g2 / g3 / g4 runs: the Act 4 failure portal (object 566, appears only after talking to Tyrael; the scout switch teleports the player next to Tyrael and interacts). One run can verify only one hit point (a hit changes act to Harrogath, area 109).
    # iPhone ratio (1334 pixels wide): portal body box center (739,440) padded to 75 square, selection box center (739,405) 124×255; the body box of Cain 246 (739,376) 75×119 sits right over the middle section of the portal, Tyrael 367 is at the upper left of the portal (667,291) 95×216.
    # iPad true fullscreen (2732 pixels wide, base x = pixels ÷ 4.096, y = (pixels ÷ 2 − 256) ÷ 2.048): portal body box center (1563,1201) padded to 205 square, selection box center (1563,1105) 338×696; Cain (1563,1026) 205×325, Tyrael (1366,795) 260×590.
    # Upper section = inside the selection box, above Cain's head; middle section = the strip at the right edge of the selection box (outside Cain and the portal body box, relying only on the selection box); lower section = inside the portal body box, below Cain's feet; g4 taps on Cain (the portal's selection box covers it too) → Cain should be selected, no act change
    GATE={"iphone":{"up":(369,150),"mid":(395,190),"low":(369,225),"out_up":(369,128),"out_dn":(369,276),"npc":(369,190)},
          "ipadfull":{"up":(390.6,70.3),"mid":(415.0,119.1),"low":(381.6,180.2),"out_up":(390.6,45.9),"out_dn":(381.6,241.2),"npc":(381.6,119.1)}}.get(MODE)
    for nm,key,what in (("g1","up","上段（只在选择框里）"),("g2","mid","中段右缘（只在选择框里）"),("g3","low","下段（身体框里）"),("g4","npc","中段凯恩身上")):
        g=G(nm,HP+" DM_TEST_WORLD_SEEK=120:1:c367:L103",save="save.rg_act4done"); g.sect("gate",0); g.enter(); g.wait(8.0)
        g.tap(P(170,150),1.2); g.tap(P(170,150),1.5)   # 探路和泰瑞尔交互后开着讲话面板（ScrollSpeechPanel）：头两下触摸被它吃掉（点位避开左下角的摇杆底座：底座固定后落在底座上会起摇杆、人被带走；第一下面板内、第二下「面板外」关掉），之后才点得到世界。这两下落在左下角空地 | after the pathfinding step interacts with Tyrael the speech panel (ScrollSpeechPanel) is open: it swallows the first two touches (placed away from the stick base at bottom left: with a fixed base a touch on it starts the stick and drags the character off; first one inside the panel, second one "outside the panel" closes it), and only then can the world be tapped. Both land on empty ground at bottom left
        if not GATE: probe(g)
        elif nm=="g4":
            g.item("点在凯恩身上（门的选择框也盖着这一点）","选凯恩：走过去开他的对话，不换幕",r"世界目标：点 \([^\n]*类型 1 cls 246[\s\S]*OpenPanel (NpcDialogPanel|ScrollSpeechPanel)",r"pos 区域 109 "); g.at("pos",.2); g.tap(P(*GATE[key]),6.0); g.at("pos",.3); g.shot()
        else:
            if nm=="g1":
                g.item("点红门选择框上边外面","不中：没有目标、人不动",NOHIT,r"pos 区域 109 "); g.at("pos",.2); g.tap(P(*GATE["out_up"]),2.0); g.at("pos",.3); g.shot()
                g.item("点红门选择框下边外面","不中：没有目标、人不动",NOHIT,r"pos 区域 109 "); g.tap(P(*GATE["out_dn"]),2.0); g.at("pos",.3); g.shot()
            g.item(f"点红门{what}","命中：换幕到哈洛加斯（区域 109）",r"世界目标：点 \([^\n]*cls 566[\s\S]*pos 区域 109 "); g.at("pos",.2); g.tap(P(*GATE[key]),7.0); g.at("pos",.3); g.shot()
        gs.append(g)
    # ex 局：出口砖旁有把守它的物件（崔凡克 → 憎恨囚牢，出口砖 64 由楼梯物件 386 把守）。探路到第四幕小站开页 → 第三幕页签 → 第 8 行崔凡克 → 探路把人传到出口砖旁（不交互），拍 900 再挪到 5153,1807 → 手指点出口砖。
    # DM_TEST_WARP_DIAG=1：服务端每收到一次「与出口砖交互」记一行（玩家模式、能否打断、出口可用、距离）；点了出口不进时这行说明是哪一项拒的
    # 判据：换关卡到囚牢一层（区域 100）。旁证两种日志都算对：游戏自己选中了楼梯当场交互，或交互出口砖 45 帧后仍在原关卡再改交互楼梯
    # ex run: an exit tile with an object guarding it beside it (Travincal → Durance of Hate, exit tile 64 guarded by stair object 386). Scout to the Act 4 waypoint and open its page → Act 3 tab → row 8 Travincal → the scout teleports the player next to the exit tile (no interaction), at tick 900 move again to 5153,1807 → tap the exit tile with a finger.
    # DM_TEST_WARP_DIAG=1: each time the server receives "interact with exit tile" it logs one line (player mode, interruptible or not, exit available, distance); when tapping the exit does not go in, this line tells which check refused it
    # Criterion: level change to Durance of Hate Level 1 (area 100). Either of two logs counts as corroboration: the game itself selected the stairs and interacted on the spot, or 45 frames after interacting with the exit tile it is still in the original level and then interacts with the stairs instead
    g=G("ex",HP+" DM_TEST_WARP_DIAG=1 DM_TEST_SEEK_NOACT=64 DM_TEST_WORLD_SEEK=150:2:c398:L103;0:5:c64:L83 DM_TEST_WORLD_XY=900:5153,1807",save="save.rg_act4"); g.sect("exit",0); g.enter(); g.wait(8.0)
    g.item("小站页点第三幕页签、第 8 行崔凡克","到崔凡克（区域 83）",r"pos 区域 83 "); g.tap(P(335,57),1.5); g.tap(P(333,280),6.0); g.at("pos",.3); g.shot()
    g.item("探路把人带到出口砖旁","还在崔凡克（看截图）",r"pos 区域 83 "); g.wait(26.0); g.at("pos",.3); g.shot()
    g.item("手指点出口砖","进憎恨囚牢一层（区域 100）",r"pos 区域 100 "); g.at("tap_unit 5 64 3",8.0); g.at("pos",.3); g.shot()
    gs.append(g)
    # tb 局：没有把守物件的出口不变（法师峡谷 → 古墓入口，出口砖 40；只对真墓 = 68 的种子成立，这份存档每局都是）。走到鲁高因小站开页约要 48 秒
    # tb run: an exit without a guarding object is unchanged (Canyon of the Magi → tomb entrance, exit tile 40; holds only for seeds where the true tomb = 68, which is every run with this save). Walking to the Lut Gholein waypoint and opening its page takes about 48 seconds
    g=G("tb",HP+" DM_TEST_WARP_DIAG=1 DM_TEST_WORLD_XY=100:5140,5170;350:5120,5135;600:5095,5105 DM_TEST_WORLD_GO=2:c156@1000 DM_TEST_SEEK_NOACT=40 DM_TEST_WORLD_SEEK=1450:5:c40:L46",save="save.rg_joejoe"); g.sect("tomb",0)
    g.t=3.5; g.tap(P(333,336),1.6); g.at("await pos"); g.t=max(g.t,13.0); g.at("note 进局",1.5)
    g.item("走到鲁高因小站，小站页点第 9 行法师峡谷","到法师峡谷（区域 46）",r"pos 区域 46 "); g.wait(44.0); g.shot(); g.tap(P(338,308),6.0); g.at("pos",.3); g.shot()
    g.item("探路把人带到古墓入口旁（逐间房传送，实测跳 59 次、约 14 秒）","还在法师峡谷（看截图）",r"pos 区域 46 "); g.wait(34.0); g.at("pos",.3); g.shot()
    g.item("手指点出口砖","进古墓（区域 68）；日志没有「把守它的物件」",r"pos 区域 68 ",r"把守它的物件"); g.at("tap_unit 5 40 3",8.0); g.at("pos",.3); g.shot()
    gs.append(g)
    # tp 局：回城门（物件 59）点门的中段。88 级法师：营地小站去冰冷之原 → 底栏回城卷轴钮开门 → 点门脚底往上 30 点 → 回营地
    # tp run: town portal (object 59), tap the middle section of the portal. Level 88 sorceress: camp waypoint to the Cold Plains → open the portal with the bottom bar town portal scroll button → tap 30 points above the foot of the portal → back to the camp
    g=G("tp","DM_TEST_WORLD_GO=2:c119@250 DM_TEST_POS_LOG=1"); g.sect("portal",0); g.enter(4); g.wait(10.0)
    g.item("营地小站页点第 2 行冰冷之原","到冰冷之原（区域 3）",r"pos 区域 3 "); g.tap(P(333,112),8.0); g.at("pos",.3); g.shot()
    g.item("点底栏回城卷轴钮","开出回城门（看截图）",None); g.tap("@portal",4.0); g.shot()
    g.item("点回城门中段（脚底往上 30 点）","回到营地（区域 1）",r"pos 区域 1 "); g.at(f"tap_unit 2 59 3 +{O(0,-30)}",7.0); g.at("pos",.3); g.shot()
    gs.append(g)
    if MODE=="ipadfull":   # 真全屏补测（贴顶信息层回屏幕真顶部）：小地图 / 地名在真顶部、点得中；各面板标题栏连在面板上 | true fullscreen extra tests (the top-anchored info layer goes back to the true top of the screen): the minimap / area name are at the true top and can be hit; each panel's title bar stays attached to its panel
        TOP=256   # 游戏视图上方那截的高（窗口点）；真顶部的东西在脚本坐标里 y 是负数 | height of the strip above the game view (window points); things at the true top have a negative y in script coordinates
        g=G("top","DM_TEST_GOLD=1300 DM_TEST_WORLD_GO=near2:1:c148@700 DM_TEST_WORLD_GO2=near2:2:c267@1900"); g.sect("top",0); g.enter()
        g.item("局内截图","小地图与地名在屏幕真顶部右上角、没被裁（看截图）",None); g.shot()
        g.item("点真顶部的区域名那一条","切全屏地图",r"点小地图那一块.*→ 全屏地图"); g.tap("@mmname"); g.shot()
        g.item("全屏地图再点区域名那一条","回小地图",r"点小地图那一块.*→ 回小地图"); g.tap("@mmname"); g.shot()
        g.item("开物品栏","标题栏连在面板上（看截图）",r"OpenPanel PlayerInventoryExpansionLayout"); g.tap("@menu"); g.shot()
        g.item("开设置页","标题栏连在面板上（看截图）",r"OpenPanel SettingsPanel"); g.tap(P(551,17)); g.at("tap_ui Settings@PauseLayoutGarden 3",1.8); g.shot()
        g.tap(P(420,345),1.8); g.tap(P(615,17),1.5)
        g.item("开箱子","标题栏连在面板上（看截图）",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 3",3.0); g.shot()
        g.tap(P(654.5,17),1.5)   # 储藏箱加宽后箱子开着时关闭钮在 660 | after the stash widening, the close button is at 660 when the stash is open
        # 阿卡拉在营地另一头：让测试开关从出生点把人带过去（游戏自己寻路），再点她。首次对话是连着两段讲话，说完对话就结束，要再点一次才出菜单
        # Akara is at the other end of the camp: let the test switch take the player there from the spawn point (the game pathfinds by itself), then tap her. The first conversation is two speeches in a row, and the conversation ends once they finish; one more tap is needed to bring up the menu
        g.item("从出生点走到阿卡拉旁","游戏自己寻路走到",r"\[script\] await pos：区域 1",r"走不过去"); g.arrive(5874,4422,4,60.0); g.shot()
        g.item("点阿卡拉、跳过两段讲话、再点她","出三行菜单（看截图）",r"OpenPanel NpcDialogPanel[\s\S]*OpenPanel NpcDialogPanel"); g.at("tap_unit 1 148 3",4.0); g.at("tap_unit 1 148 2",2.5); g.at("tap_unit 1 148 2",2.5); g.at("tap_unit 1 148 2",2.5); g.shot()
        g.item("阿卡拉菜单点最后一行「取消」","命中那一行并关菜单",r"对话菜单 \+2 行"); g.at(f"tap_unit 1 148 2 +{MENU['trade3']}",2.0); g.shot()
        # 出营地分两段：测试开关只认当前载入范围里的目标（在阿卡拉那头，出口和恰西都「没找到」），所以先让人寻路走回出生点旁的箱子，再用摇杆沿固定路线出营地。
        # 「阿卡拉 ↔ 恰西」那条远路由「紧挨瓦瑞夫 → 恰西」的原站位复跑覆盖
        # Leaving the camp takes two stages: the test switch only recognizes targets within the currently loaded range (over at Akara's end, the exit and Charsi are both 「没找到」(not found)), so first let the player pathfind back to the stash next to the spawn point, then leave the camp along a fixed route with the stick.
        # The long "Akara ↔ Charsi" route is covered by rerunning the original standing position "right next to Warriv → Charsi"
        g.item("从阿卡拉处走回箱子旁","游戏自己寻路走到，不在半路停",r"\[script\] await pos：区域 1",r"走不过去|没找到"); g.arrive(5808,4443,6,130.0); g.shot()
        for x,y,r,to in [(5788,4475,4,12),(5812,4503,4,9),(5826,4522,4,8),(5840,4560,5,8)]: g.at(f"walk_to {x} {y} {r} {to} skip",to+.5)
        g.item("野外","在区域 2",r"pos 区域 2 "); g.at("pos",.3); g.shot()
        g.item("野外点真顶部区域名那一条","切全屏地图；玩家标记压在玩家身上（看截图）",r"点小地图那一块.*→ 全屏地图"); g.tap("@mmname"); g.shot()
        g.item("野外全屏地图再点区域名那一条","回小地图",r"点小地图那一块.*→ 回小地图"); g.tap("@mmname"); g.shot()
        # 真全屏下功能页只占屏幕下方那一块，上方那一截是世界画面：面板开着时点上方 = 点面板外，面板关掉；不能被当成点了顶部页签
        # In true fullscreen the function page takes only the lower part of the screen, and the strip above it is the world view: with a panel open, tapping above = tapping outside the panel, and the panel closes; it must not be treated as a tap on the top tabs
        g.item("物品栏开着，点面板上方那一截","面板关掉、不切页签",r"ClosePanel PlayerInventoryExpansionLayout",r"界面页签：切"); g.tap("@menu",1.4); g.shot(); g.tap(f"683,{-TOP//2}",1.5); g.shot()
        gs.append(g)
    # ---- NPC 对话菜单逐行点：行用控件名点（tap_ui ListItem<N>@NpcDialogPanel），不用坐标——菜单的纵向位置跟 NPC 停下时的站位走，同一配置各局能差半行。
    # 点一行之后菜单会关或换成子面板，下一步先重开（tap_unit 再点一下 NPC）；面板外点两下 = 关（单项提示框 = 继续）
    # ---- Tap NPC dialog menu rows one by one: rows are tapped by widget name (tap_ui ListItem<N>@NpcDialogPanel), not by coordinates - the menu's vertical position follows where the NPC stands when it stops, and can differ by half a row between runs of the same configuration.
    # After a row is tapped the menu closes or is replaced by a sub-panel, so the next step reopens it first (tap_unit taps the NPC once more); two taps outside the panel = close (single-item prompt box = Continue)
    def row(g,cls,k,title,expect,res=None,reopen=True):
        if reopen: g.at(f"tap_unit 1 {cls} 2",2.2)
        g.item(title,expect,menu(k,res)); g.at(f"tap_ui ListItem{k}@NpcDialogPanel 3",2.5); g.shot()
    def away(g): g.tap(P(20,240),1.2); g.tap(P(20,240),1.2)
    # me1 局：圣骑（标准存档第 1 行）的恰西四行：交谈 / 交易或修复 / 灌魔 / 取消。灌魔现场：长剑、箭袋当场进背包。灌魔面板用外框右上角凹槽里的关闭钮关
    # me1 run: Charsi's four rows for the paladin (row 1 of the standard save): Talk / Trade or Repair / Imbue / Cancel. Imbue setup: a long sword and a quiver go into the inventory on the spot. The imbue panel is closed with the close button in the notch at the top-right corner of the outer frame
    g=G("me1","DM_TEST_GIVE=lsd:2:2,qui:2:2@60 DM_TEST_GIVE_INV=1 DM_TEST_WORLD_GO=near2:1:c154@200"); g.sect("menu4",0); g.enter(1); g.arrive(5776,4433,10,60.0)
    g.item("点恰西","出四行菜单（看截图）",r"OpenPanel NpcDialogPanel"); g.at("tap_unit 1 154 3",4.5); g.shot()
    row(g,154,0,"点第 1 行「交谈」","落在交谈",None,reopen=False); away(g)
    row(g,154,1,"点第 2 行「交易或修复」","进商人页",r"OpenPanel VendorPanelLayout"); g.tap(P(615,17),1.5)
    row(g,154,2,"点第 3 行「灌魔」","开灌魔面板（关闭钮在外框右上角凹槽里，看截图）",r"OpenPanel ImbueItemsPanel")
    g.item("空槽时点灌魔面板的关闭钮","灌魔面板、背包、外框都关（看截图）",r"ClosePanel ImbueItemsPanel",r"c2s 17\b"); g.at("tap_ui TouchClose@ImbueItemsPanel 3",2.0); g.shot()
    g.item("关掉后推摇杆","人能走：坐标变了",moved); g.at("pos",.2); g.at("down 1 @joy",.3); g.at("move 1 @joy+60,0",.5); g.at("up 1",.5); g.at("pos",.3)   # 只推半秒：走远了 iPad 真全屏下恰西会落到游戏视图上方那一截，tap_unit 认不到 | push for only half a second: after walking far, in iPad true fullscreen Charsi falls into the strip above the game view and tap_unit cannot find her
    g.at("tap_unit 1 154 3",5.0)   # 人走开了一点，先走回去点她 | the player has walked away a little; walk back first and tap her
    row(g,154,3,"点第 4 行「取消」","菜单关",r"ClosePanel NpcDialogPanel",reopen=False)
    g.item("重开菜单后点菜单外","菜单关",r"ClosePanel NpcDialogPanel"); g.at("tap_unit 1 154 2",2.2); g.tap(P(20,240),1.5); g.shot()
    gs.append(g)
    # me2 局：圣骑的卡夏四行：交谈 / 复活佣兵 / 雇佣 / 取消。圣骑金币 0，点「复活」出单项提示框「你没有足够的金币那么做 / 继续」——点一下要能关掉、之后菜单还能重开（这里出过卡死）
    # me2 run: Kashya's four rows for the paladin: Talk / Resurrect Mercenary / Hire / Cancel. The paladin has 0 gold; tapping 「复活」(Resurrect) shows the single-item prompt box 「你没有足够的金币那么做 / 继续」(You do not have enough gold for that / Continue) - one tap must be able to close it, and the menu must still reopen afterward (a hang happened here)
    g=G("me2","DM_TEST_WORLD_GO=near2:1:c150@200"); g.sect("menu4k",0); g.enter(1); g.arrive(5838,4454,10,45.0)
    g.item("点卡夏","出四行菜单（看截图）",r"OpenPanel NpcDialogPanel"); g.at("tap_unit 1 150 3",4.0); g.shot()
    row(g,150,0,"点第 1 行「交谈」","落在交谈",None,reopen=False); away(g)
    row(g,150,1,"点第 2 行「复活佣兵」（金币不够）","落在复活；出「你没有足够的金币那么做 / 继续」单项框（看截图）",None)
    g.item("单项提示框点一下","框关掉；随后再点卡夏菜单能重开（没卡死）",r"OpenPanel NpcDialogPanel"); g.tap(P(20,240),1.5); g.tap(P(20,240),1.5); g.at("tap_unit 1 150 2",2.5); g.shot()
    row(g,150,2,"点第 3 行「雇佣」","开雇佣名单",r"OpenPanel HireMenuPanel",reopen=False); away(g)
    row(g,150,3,"点第 4 行「取消」","菜单关",r"ClosePanel NpcDialogPanel")
    g.item("关掉后推摇杆","人能走：坐标变了",moved); g.at("pos",.2); g.at("down 1 @joy",.3); g.at("move 1 @joy+60,0",1.2); g.at("up 1",.5); g.at("pos",.3)
    gs.append(g)
    # me3 局：法师（第 4 行）的恰西三行：交谈 / 交易或修复 / 取消
    # me3 run: Charsi's three rows for the sorceress (row 4): Talk / Trade or Repair / Cancel
    g=G("me3","DM_TEST_WORLD_GO=near2:1:c154@200"); g.sect("menu3",0); g.enter(4); g.arrive(5776,4433,10,60.0)
    g.item("点恰西","出三行菜单（看截图）",r"OpenPanel NpcDialogPanel"); g.at("tap_unit 1 154 3",4.5); g.shot()
    row(g,154,0,"点第 1 行「交谈」","落在交谈",None,reopen=False); away(g)
    row(g,154,1,"点第 2 行「交易或修复」","进商人页",r"OpenPanel VendorPanelLayout"); g.tap(P(615,17),1.5)
    row(g,154,2,"点第 3 行「取消」","菜单关",r"ClosePanel NpcDialogPanel")
    gs.append(g)
    # me4 局：1 级术士（第 2 行）的卡夏：先有一段开场白（不是菜单），点面板外关掉后再点她才出两行菜单：交谈 / 取消
    # me4 run: Kashya for the level 1 warlock (row 2): first there is an opening speech (not a menu); after tapping outside the panel to close it, tapping her again brings up the two-row menu: Talk / Cancel
    g=G("me4","DM_TEST_WORLD_GO=near2:1:c150@200"); g.sect("menu2",0); g.enter(); g.arrive(5838,4454,10,45.0)
    g.item("点卡夏、关掉开场白、再点她","出两行菜单（看截图）",r"OpenPanel NpcDialogPanel"); g.at("tap_unit 1 150 3",4.0); away(g); g.at("tap_unit 1 150 2",2.5); g.shot()
    row(g,150,0,"点第 1 行「交谈」","落在交谈",None,reopen=False); away(g)
    row(g,150,1,"点第 2 行「取消」","菜单关",r"ClosePanel NpcDialogPanel")
    gs.append(g)
    # ---- 单件任务槽（灌魔 / 打孔 / 个性化 / 底座）：物品进槽、出槽读物品表状态日志的停留段，不读截图 ----
    # sl1 局：恰西灌魔（圣骑，标准存档第 1 行）。底材用皮手套 lgl——手套最多 0 孔；测试开关给的白装随机带孔，带孔的原版不让灌魔，用长剑这类会「时进时不进」。
    # 每种给 2 件，第一件自动穿上，第二件进背包：手套 2×2 在格 (8,2)（中心 (552.2,275.8)），布甲 qui 2×3 在格 (6,1)（中心 (513.1,266)），宝石在格 (5,3)。面板：槽 (188,197)，拖放松手点在目标下方 40。
    # 每个动作做完停 3 秒，读物品表里手套的停留段：背包 → 同格（拖偏）→ 槽 → 背包 → 槽 →（槽占用时别的物品不动）→ 背包（关面板）→ 槽 → 没了（灌魔把它换成新物品）
    # ---- Single-item quest slots (imbue / socketing / personalization / orifice): items entering and leaving the slot are read from the dwell segments of the item table state log, not from screenshots ----
    # sl1 run: Charsi imbue (paladin, row 1 of the standard save). The base item is leather gloves lgl - gloves have at most 0 sockets; white items given by the test switch get random sockets, the original game does not allow imbuing socketed ones, and something like a long sword would "sometimes go in, sometimes not".
    # Two of each are given, the first is auto-equipped, the second goes into the inventory: gloves 2×2 at cell (8,2) (center (552.2,275.8)), quilted armor qui 2×3 at cell (6,1) (center (513.1,266)), gem at cell (5,3). Panel: slot (188,197), the drag-and-drop release point is 40 below the target.
    # After each action pause 3 seconds and read the gloves' dwell segments in the item table: inventory → same cell (drag missed) → slot → inventory → slot → (other items do not move while the slot is occupied) → inventory (panel closed) → slot → gone (imbue replaces it with a new item)
    g=G("sl1","DM_STATE=pwstate.jsonl DM_TEST_GIVE=lgl:2:2,qui:2:2@60 DM_TEST_GIVE_INV=1 DM_TEST_WORLD_GO=near2:1:c154@200"); g.sect("slot",0); g.enter(1); g.arrive(5776,4433,10,60.0)
    def imbue(g): g.at("tap_unit 1 154 3",4.5); g.item("恰西菜单点「灌魔」","开灌魔面板",r"OpenPanel ImbueItemsPanel"); g.at("tap_ui ListItem2@NpcDialogPanel 3",3.0); g.shot()
    GL=(552.2,275.8); QU=(513.1,266); QU2=(464.2,246.5)
    imbue(g)
    g.item("手套从背包拖到面板底板（没对准槽）","回原格",("seg","lgl",1,0,"same")); g.dragp(GL,(100,330)); g.wait(2.0); g.shot()
    g.item("双击背包里的手套","进槽",("seg","lgl",2,4)); g.dbl(*GL); g.wait(2.0); g.shot()
    g.item("把槽里的手套拖回背包（松手在它原来那格上）","回背包",("seg","lgl",3,0)); g.dragp((188,197),(GL[0],GL[1]+40)); g.wait(2.0); g.shot()
    g.item("槽空着时把布甲拖到背包别的空位","照常能拖：布甲换了格",("seg","qui",1,0,"moved")); g.dragp(QU,(QU2[0],QU2[1]+40)); g.wait(2.0); g.shot()
    g.item("把手套从背包拖到槽上","进槽",("seg","lgl",4,4)); g.dragp(GL,(188,237)); g.wait(2.0); g.shot()
    g.item("槽里有东西时：把布甲拖到面板外、双击它、长按它","都不动：槽里那件一直在槽里，布甲在物品表里没有任何变化",("still","lgl",4,"qui")); g.dragp(QU2,(20,240)); g.wait(1.0); g.dbl(*QU2); g.wait(1.0); g.hold(QU2[0],QU2[1],.8); g.wait(1.0); g.shot()
    g.item("槽里有物品时点面板的关闭钮","面板关、手套回背包（不丢地上）",("seg","lgl",5,0,r"ClosePanel ImbueItemsPanel")); g.at("tap_ui TouchClose@ImbueItemsPanel 3",3.0); g.shot()
    imbue(g)
    g.item("双击背包里的宝石（不合格）","拒收：宝石始终在背包、没进过槽",("segnot",568,4)); g.dbl(483.75,285.5); g.wait(2.0); g.shot()
    g.item("双击手套再进槽","进槽",("seg","lgl",6,4)); g.dbl(*GL); g.wait(2.0); g.shot()
    g.item("点「灌魔」确认","灌魔完成：原来那件没了（被换成新物品）、面板关",("seg","lgl",7,-1,r"ClosePanel ImbueItemsPanel")); g.at("tap_ui ConfirmWithText@ImbueItemsPanel 3",3.0); g.shot()
    g.item("灌魔出的新物品","在背包里",("bag","lgl")); g.tap("@menu",1.5); g.shot()
    gs.append(g)
    # sl2 局：拉苏克打孔（探路稿，还没跑过）。88 级法师：营地小站去哈洛加斯 → 走到拉苏克（菜单：交谈 / 交易或修复 / 添加镶孔 / 取消，没有开场白）→ 交易页买货架第一件（要无孔白装；货架每局随种子定）→ 添加镶孔 → 双击背包里刚买的那件进槽 → 确认。
    # 买来的东西事先不知道类号，进没进槽读「物品表里有物品稳定地在手上」
    # sl2 run: Larzuk socketing (scouting draft, not yet run). Level 88 sorceress: camp waypoint to Harrogath → walk to Larzuk (menu: Talk / Trade or Repair / Add Sockets / Cancel, no opening speech) → on the trade page buy the first item on the shelf (needs an unsocketed white item; the shelf is fixed per run by the seed) → Add Sockets → double-tap the just-bought item in the inventory into the slot → confirm.
    # The class number of the bought item is not known in advance; whether it went into the slot is read from "the item table has an item stably in hand"
    g=G("sl2","DM_STATE=pwstate.jsonl DM_TEST_WORLD_GO=2:c119@250 DM_TEST_WORLD_GO2=1:c511@900 DM_TEST_STAT_LOG=14 DM_TEST_POS_LOG=1"); g.sect("socket2",0); g.enter(4); g.wait(8.0)
    g.item("营地小站页点第五幕页签、第 1 行哈洛加斯","到哈洛加斯（区域 109）",r"pos 区域 109 "); g.wait(4.0); g.tap(P(459,58),1.5); g.tap(P(337,83),8.0); g.at("pos",.3); g.shot()
    g.item("走到拉苏克出菜单，点「交易或修复」","进商人页",r"OpenPanel VendorPanelLayout"); g.wait(14.0); g.at("tap_ui ListItem1@NpcDialogPanel 8",3.0); g.shot()
    g.item("货架上双击第一件","买下：金币减少",gold('-')); g.dbl(109.5,102); g.wait(1.5); g.shot()
    g.tap(P(615,17),1.5); g.at("tap_unit 1 511 2",2.5)
    g.item("拉苏克菜单点「添加镶孔」","开打孔面板（看截图：槽、背包里刚买的那件在哪）",r"OpenPanel \w+"); g.at("tap_ui ListItem2@NpcDialogPanel 3",3.0); g.shot()
    g.item("双击背包里刚买的那件","进槽",("held",0)); g.dbl(512.6,246.2); g.wait(2.5); g.shot()
    g.item("点确认","打孔完成：那件离开槽、面板关（看截图里物品说明有孔）",("held",0,"ended")); g.tap(P(188,252),3.0); g.shot()
    gs.append(g)
    # sl3 局：安亚个性化（探路稿，同上还没跑过）。88 级法师到哈洛加斯后发一顶软帽（拍 600，那时已换幕），走到安亚 c512（先有两段开场白，点三下），菜单第 4 行个性化 → 双击软帽进槽 → 确认
    # sl3 run: Anya personalization (scouting draft, like the one above not yet run). The level 88 sorceress is given a cap after reaching Harrogath (tick 600, the act has already changed by then), walks to Anya c512 (two opening speeches first, tap three times), menu row 4 personalize → double-tap the cap into the slot → confirm
    g=G("sl3","DM_STATE=pwstate.jsonl DM_TEST_GIVE=cap:2:1@600 DM_TEST_GIVE_INV=1 DM_TEST_WORLD_GO=2:c119@250 DM_TEST_WORLD_GO2=1:c512@950 DM_TEST_STAT_LOG=14 DM_TEST_POS_LOG=1"); g.sect("personal",0); g.enter(4); g.wait(8.0)
    g.item("营地小站页点第五幕页签、第 1 行哈洛加斯","到哈洛加斯（区域 109）",r"pos 区域 109 "); g.wait(4.0); g.tap(P(459,58),1.5); g.tap(P(337,83),8.0); g.at("pos",.3); g.shot()
    g.item("走到安亚、关掉两段开场白、再点她出菜单，点第 4 行个性化","开个性化面板（看截图）",r"OpenPanel \w+"); g.wait(18.0); g.tap(P(20,240),1.5); g.tap(P(20,240),1.5); g.tap(P(20,240),1.5); g.at("tap_unit 1 512 3",3.0); g.at("tap_ui ListItem3@NpcDialogPanel 5",3.0); g.shot()
    g.item("双击背包里的软帽","进槽",("seg","cap",1,4)); g.dbl(513.1,236.75); g.wait(2.5); g.shot()
    g.item("槽里有物品时点关闭钮","面板关、软帽回背包",("seg","cap",2,0)); g.tap(P(615,17),3.0); g.shot()
    gs.append(g)
    # cb 局：魔盒页开着时双击 = 送进 / 送出魔盒。88 级法师（第 4 行），另给一顶软帽（法师头上有头盔，软帽留在背包的空位，格 (6,0)）。
    # 物品栏单页的背包格：第 1 列中心 x 245、列距 19.83、第 1 行 y 225；魔盒在格 (8,2)。魔盒页开着时右边背包是箱子那套格子：符文在格 (6,3)，软帽在格 (6,0)。判据读截图的背包占格（软帽 2×2 读不全，只判增减）
    # cb run: with the cube page open, double-tap = send into / out of the cube. Level 88 sorceress (row 4), given one extra cap (the sorceress has a helm on her head, so the cap stays in a free spot in the inventory, cell (6,0)).
    # Inventory cells on the single-page inventory panel: column 1 center x 245, column pitch 19.83, row 1 y 225; the cube is at cell (8,2). With the cube page open the inventory on the right uses the stash's grid: the rune is at cell (6,3), the cap at cell (6,0). The criterion reads the inventory cell occupancy from the screenshot (the 2×2 cap cannot be read fully, so only increase / decrease is judged)
    def notworn(s,before): return "不通：被穿上了（"+re.search(r"界面双击：[^\n]*装上",s).group(0)[:30]+"）" if re.search(r"界面双击：[^\n]*装上",s) else "通：没有被穿上"
    g=G("cb","DM_STATE=pwstate.jsonl DM_TEST_GIVE=cap:2:1@60 DM_TEST_GIVE_INV=1"); g.sect("cube2",0); g.enter(4); g.wait(2.0); g.tap("@menu",1.4)
    g.item("物品栏里双击背包里的魔盒","打开魔盒页",r"OpenPanel HoradricCubeLayout"); g.dbl(413.6,273.75); g.wait(1.0); g.shot()
    g.item("魔盒页开着双击背包里的符文","进魔盒：背包少 1 格",("inv",-1)); g.dbl(503.3,285.5); g.wait(1.5); g.shot()
    g.item("双击魔盒里的那枚符文","回背包：背包多 1 格",("inv",1)); g.dbl(205.6,179.6); g.wait(1.5); g.shot()
    g.item("魔盒页开着双击背包里的软帽（装备）","进魔盒（物品表里到了魔盒那一页）、没被穿上",("segpage","cap",1,3)); g.dbl(513.1,236.75); g.wait(2.0); g.shot()
    g.item("双击魔盒里的软帽","回背包",("segpage","cap",2,0)); g.dbl(178,183); g.wait(2.0); g.shot()   # 魔盒里它落在左下 2×2；法师的魔盒里原有一把钥匙在它右边，第一遍点偏点到了钥匙 | in the cube it lands in the bottom-left 2×2; the sorceress's cube already had a key to its right, and on the first pass the tap was off and hit the key
    g.tap(P(615,17),1.5); g.tap(P(615,17),1.2); g.tap("@menu",1.4)
    g.item("魔盒页没开时（只开物品栏）双击背包里的软帽","照旧穿上",r"界面双击：[^\n]*装上"); g.dbl(373.9,234.75); g.wait(1.5); g.shot()
    gs.append(g)
    # dz（古墓底座，主线卡点；走法同 touch/l_d2）：joejoe（save.rg_joejoe，身上带赫拉迪姆法杖）→ 鲁高因小站 → 法师峡谷 → 探路进真墓 → 交互底座 c152 开放置面板
    # dz (tomb orifice, main-quest chokepoint; same route as touch/l_d2): joejoe (save.rg_joejoe, carrying the Horadric Staff) → Lut Gholein waypoint → Canyon of the Magi → scout into the true tomb → interact with orifice c152 to open the placement panel
    g=G("dz",HP+" DM_STATE=pwstate.jsonl DM_TEST_WORLD_XY=100:5140,5170;350:5120,5135;600:5095,5105 DM_TEST_WORLD_GO=2:c156@1000 DM_TEST_WORLD_SEEK=1450:5:c39:L46;1200:2:c152:L67",save="save.rg_joejoe")   # 这份存档真墓 = 67（DM_TEST_LEVEL_INFO 探得），出口砖类号 = 67−28 = 39; g.sect("dz",0) | in this save the true tomb = 67 (found with DM_TEST_LEVEL_INFO), exit tile class number = 67−28 = 39; g.sect("dz",0)
    g.t=3.5; g.tap(P(333,336),1.6); g.at("await pos"); g.t=max(g.t,13.0); g.at("note 进局",1.5)
    g.item("走到鲁高因小站，小站页点第 9 行法师峡谷","到法师峡谷（区域 46）",r"pos 区域 46 "); g.wait(44.0); g.tap(P(338,308),6.0); g.at("pos",.3); g.shot()
    g.item("探路进真墓、交互底座","开底座放置面板（ImbueItemsPanel），在区域 67",r"OpenPanel ImbueItemsPanel"); g.t=max(g.t,200.0); g.at("pos",.3); g.shot()
    # 法杖在背包第 5 列（竖 4 格），中心 667 基准 (464,255)；底座面板确认钮 (188.5,252.5)（dz_01 截图）
    # The staff is in inventory column 5 (4 cells tall), center at 667 base (464,255); the orifice panel confirm button (188.5,252.5) (dz_01 screenshot)
    g.item("双击背包里的赫拉迪姆法杖","进底座槽（物品表里有物品稳定地在槽里）",("held",0)); g.dbl(464,255); g.wait(2.0); g.shot()
    g.item("点底座面板的确认钮","法杖插进底座：离开槽、面板关（看截图：底座上插着法杖）",("held",0,"ended",r"ClosePanel ImbueItemsPanel")); g.tap(P(188.5,252.5),4.0); g.shot()
    gs.append(g)
    # sb 局：储藏箱每个按钮逐个点（iPhone 档 + iPad 档都跑）。全按控件名点，跟坐标无关
    # sb run: tap every stash button one by one (run in both iPhone mode and iPad mode). All tapped by widget name, independent of coordinates
    g=G("sb","DM_TEST_GIVE=r01:2:1@60 DM_TEST_GIVE_INV=1 DM_TEST_GOLD=1300"+" DM_TEST_WORLD_GO=near2:2:c267@200"); g.sect("sbtn",0); g.enter(); g.wait(2.0)
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.arrive(5808,4443,3,24.0); g.at("tap_unit 2 267 3",3.0); g.shot()
    for k,nm in [(1,"共享"),(2,"珍宝匣"),(5,"编年史"),(2,"珍宝匣"),(0,"个人")]:   # 珍宝匣合页后页签条只剩四个：个人 / 共享 / 珍宝匣（原宝石页签 Image2）/ 编年史（Image5，左移两格）；材料 / 符文页签挪出屏不再点 | after the Trove merge the tab bar has only four tabs: Personal / Shared / Trove (the former gems tab Image2) / Chronicle (Image5, shifted left two slots); the materials / runes tabs are off screen and no longer tapped
        g.item(f"点页签 Image{k}（{nm}）","切页",r"界面页签：切 [+-]\d+ 页"); g.at(f"tap_ui Image{k}@BankExpansionLayout 3",1.5); g.shot()
    g.at("tap_ui Image1@BankExpansionLayout 3",1.5)
    g.item("共享页点右箭头","翻到第 2 页",r"SharedStashRight"); g.at("tap_ui TouchStashRight@BankExpansionLayout 3",1.5); g.shot()
    g.item("再点右箭头","翻到第 3 页",r"SharedStashRight"); g.at("tap_ui TouchStashRight@BankExpansionLayout 3",1.5); g.shot()
    g.item("点左箭头","回第 2 页",r"SharedStashLeft"); g.at("tap_ui TouchStashLeft@BankExpansionLayout 3",1.5); g.shot()
    g.item("再点左箭头","回第 1 页",r"SharedStashLeft"); g.at("tap_ui TouchStashLeft@BankExpansionLayout 3",1.5); g.shot()
    g.at("tap_ui Image0@BankExpansionLayout 3",1.5)
    g.item("点物品栏金币钮（存入）","弹数量框",r"OpenPanel DropGoldModal"); g.at("tap_ui gold_button@PlayerInventoryExpansionLayout 3",1.5); g.shot()
    g.item("数量框点取消","关框",r"ClosePanel DropGoldModal"); g.tap(P(375,200),1.5); g.shot()
    g.item("点右上角关闭 close","箱子关、加宽还原",r"ClosePanel BankExpansionLayout[\s\S]*储藏箱加宽：关|储藏箱加宽：关[\s\S]*ClosePanel BankExpansionLayout"); g.at("tap_ui close@BankExpansionLayout 3",2.0); g.shot()
    gs.append(g)
    # ---- 符文之语助手：rw1 长按开清单 / 点外关 / 同一落点再长按（不许整理背包）/ 一键合成；rw2 符文在符文页、箱子关着与开着 ----
    # 现场同 k 局（固定种子）：只开物品栏时背包软帽 (376,228) 1 孔、(318,267) 2 孔、(395,267) 2 孔；四号符文 (308,238)、三号 (308,219)。四号 + 三号 = 「天底」。
    # 储藏箱开着时（rw1 物品表实测）：两枚符文在背包格 (3,0) / (3,1)，2 孔软帽左上在 (7,2)；储藏箱视图背包格中心 x = 424.5 + 19.55 列、y = 227 + 19.5 行
    # ---- Runeword helper: rw1 long-press opens the list / tap outside closes / long-press again at the same spot (must not sort the inventory) / one-tap craft; rw2 runes on the rune page, stash closed and open ----
    # Same setup as run k (fixed seed): with only the inventory panel open, inventory caps (376,228) 1 socket, (318,267) 2 sockets, (395,267) 2 sockets; rune no. 4 (308,238), no. 3 (308,219). No. 4 + no. 3 = 「天底」(Nadir).
    # With the stash open (measured from the rw1 item table): the two runes are in inventory cells (3,0) / (3,1), the top-left of the 2-socket cap is at (7,2); inventory cell centers in the stash view are x = 424.5 + 19.55 × column, y = 227 + 19.5 × row
    RWE="DM_TEST_GIVE=cap:2:7,r04:2:1,r03:2:1@60 DM_TEST_GIVE_INV=1"
    LISTOK=r"符文之语：长按[^\n]*→ 清单 [1-9]\d* 行"
    def anyl(p): return lambda s,b: ("通：" if re.search(p,b+s) else "不通：")+"整局日志"
    # rw1：背包里符文齐——长按 1 孔软帽不开清单；长按 2 孔软帽开清单；点清单外关；再长按、点「一键合成」成型，两枚符文离开背包
    # rwlv 局：清单里的「需要等级」取底材的需求等级，不是物品等级（线上缺陷：塔盔需求 59、物品等级 79，清单整页显示 79）。
    # 现场：背包里 7 顶塔盔（uhm，白装孔数随机但逐局相同），长按其中带孔的一顶；2 孔塔盔的首行是「知识」（欧特 21 + 索尔 27）→ max(59, 27) = 59
    # rw1: all runes in the inventory - a long press on a 1-socket Cap does not open the list; a long press on a 2-socket Cap opens it; a tap outside the list closes it; long press again and tap "Craft" to make it, and the two runes leave the inventory
    # rwlv run: "required level" in the list takes the base item's required level, not its item level (reported defect: the Spired Helm requires 59 with item level 79, and the whole page of the list showed 79).
    # Scene: 7 Spired Helms in the inventory (uhm; socket counts of white items are random but the same every run), long press one with sockets; the first row for a 2-socket Spired Helm is Lore (Ort 21 + Sol 27) -> max(59, 27) = 59
    g=G("rwlv","DM_TEST_GIVE=uhm:2:7@60 DM_TEST_GIVE_INV=1"); g.sect("rwlv",0); g.enter(); g.wait(2.0); g.tap("@menu",1.4)
    g.item("长按背包里带孔的塔盔","弹清单，需要等级 59（看截图：每行都是 59）",r"长按 uhm[^\n]*需要等级 59",r"长按 uhm[^\n]*需要等级 79|【红】符文之语"); g.hold(395,267); g.wait(1.0); g.shot()
    g.item("点清单外","清单关掉",r"清单外 → 关清单|清单关闭"); g.tap(P(120,200),1.5); g.shot()
    gs.append(g)
    g=G("rw1",RWE+" DM_TEST_RW_LIST=75"); g.sect("rw1",0); g.enter(); g.wait(2.0); g.tap("@menu",1.4)
    g.item("背包布局","软帽、符文在 k 局同样的位置（看截图）",None); g.shot()
    g.item("长按 1 孔软帽","不开清单（1 孔帽子没有能做的符文之语）",None,LISTOK); g.hold(376,228); g.wait(1.0); g.shot()
    g.item("长按 2 孔软帽","弹清单：天底那一行有「一键合成」（看截图：名字、需要等级、符文编号与数量）",LISTOK); g.hold(395,267); g.wait(1.0); g.shot()
    g.item("点清单外（左边游戏画面）","清单关掉",r"清单外 → 关清单|清单关闭"); g.tap(P(120,200),1.5); g.shot()
    g.item("同一落点再长按 2 孔软帽、点第一行「一键合成」","再开清单（不是整理背包）、成型：两颗进孔、标志带 0x04000000",r"清单 [1-9]\d* 行[\s\S]*一键合成成型",r"【红】符文之语|整理背包"); g.hold(395,267); g.wait(1.0); g.at("tap_ui TouchRwGo0 3",4.0); g.shot()
    g.item("两枚符文原来的格子","都空了",("spot","empty",(304,245))); g.shot()
    g.item("单击那顶软帽","说明第一行是「天底」（看截图）",None); g.tap(P(395,267),1.5); g.shot()
    g.item("读回","物品表里有 1 件符文之语物品、孔里 2 颗",anyl(r"DM_TEST_RW_LIST：符文之语物品[^\n]*孔里 2 颗")); g.t=max(g.t,80.0); g.shot()
    gs.append(g)
    # rw2：符文放进储藏箱符文页——箱子关着时长按：清单写「符文页」、没有按钮；箱子开着时长按：有「一键合成」，点了先从符文页取件再镶，成型
    # rw2: runes placed in the stash rune page - long-press with the stash closed: the list says 「符文页」(rune page) and there is no button; long-press with the stash open: there is 「一键合成」(Craft), tapping it first takes the items from the rune page and then sockets them, formed
    g=G("rw2",RWE); g.sect("rw2",0); g.enter(); g.wait(2.0)
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 3",3.0); g.tap(P(148,53),1.5); g.shot()
    g.item("符文页开着双击背包里两枚符文","各进符文页自己那一格：背包少 2 格",None); g.clr(); g.shot(); g.dbl(483.2,227); g.wait(1.0); g.dbl(483.2,246.5); g.wait(1.0); g.clr(); g.shot()
    g.tap(P(654.5,17),1.5); g.tap("@menu",1.4)
    g.item("箱子关着、只开物品栏长按 2 孔软帽","清单写「符文页 1」、没有「一键合成」、没有红字（看截图）",LISTOK); g.hold(395,267); g.wait(1.0); g.shot()
    g.tap(P(120,200),1.2); g.tap(P(615,17),1.5)
    g.item("开箱子、长按物品栏里的 2 孔软帽","这次有「一键合成」",LISTOK); g.at("tap_unit 2 267 3",3.0); g.hold(561.4,266); g.wait(1.0); g.shot()
    g.item("点「一键合成」","先从符文页取件、再镶，成型",r"从符文页(?:取 1 颗|成叠拿起)[\s\S]*一键合成成型",r"【红】符文之语"); g.at("tap_ui TouchRwGo0 3",5.0); g.shot()
    gs.append(g)
    # ---- 腰带空格双击补满：物品栏腰带长按不动、空格双击补满所有空格（只认药水、卷轴不进）、有药格双击使用；bdm 带佣兵底栏双击喂佣兵 ----
    # 只开物品栏时腰带四格中心（667 基准）：(294,197) (320,197) (346,197) (371,197)（q 局第 3 格 346 同）
    # ---- Double-tap an empty belt slot to fill: long-pressing the belt in the inventory panel does nothing, double-tapping an empty slot fills all empty slots (potions only, scrolls do not go in), double-tapping a slot with a potion uses it; bdm with a mercenary, double-tap on the bottom bar feeds the mercenary ----
    # Centers of the four belt slots when only the inventory panel is open (667 base): (294,197) (320,197) (346,197) (371,197) (same as slot 3 at 346 in run q)
    BX=(294,320,346,371)
    g=G("bd","DM_TEST_GIVE=hp1:2:3@60 DM_TEST_GIVE_INV=1 DM_STATE=pwstate.jsonl DM_POT_PICK=0"); g.sect("bd",0); g.enter(); g.wait(2.0)   # DM_POT_PICK=0：关掉开背包自动补腰带，这一局验的是双击补满 | DM_POT_PICK=0: turns off auto-refilling the belt when the inventory opens; this run verifies double-tap fill
    g.item("底栏喝掉腰带第 1、2 格","腰带 0/0/1/1",r"腰带 0/0/1/1"); g.tap("@belt1",1.0); g.tap("@belt2",2.0); g.shot()
    g.tap("@menu",1.4)
    g.item("物品栏腰带空格（第 1 格）长按 1.2 秒","不动、不整理（看截图）",r"界面长按：腰带 → 不动",r"整理背包|一键补满"); g.hold(BX[0],197,1.2); g.wait(1.0); g.shot()
    g.item("腰带空格双击","一键补满：两个空格都满（1/1/1/1）、日志有「腰带补满：调了原版自动装入」",lambda s,b: ("通：" if all(re.search(x,s) for x in (r"腰带空格 → 一键补满",r"腰带补满：调了原版自动装入",r"腰带 1/1/1/1")) else "不通：")+"补满三行",r"整理背包"); g.dbl(BX[0],197); g.wait(1.5); g.shot()
    g.item("有药格（第 4 格）双击","照旧使用（喝）",r"界面双击：[^\n]*→ 使用"); g.dbl(BX[3],197); g.wait(1.5); g.shot()
    g.item("腰带空格吸附边缘（第 2 格偏向第 3 格 6 点）长按","仍按空格算：不动、不整理",r"界面长按：腰带 → 不动",r"整理背包"); g.hold(BX[1]+6,197,1.2); g.wait(1.0); g.shot()
    gs.append(g)
    # ba：开背包自动补腰带（只在开物品栏那一下补一次；空列按红红蓝蓝认领）。底栏喝掉第 1、2 格，背包里有 3 瓶小红，开物品栏 → 第 1、2 列各补 1 瓶
    # ba: opening the inventory auto-refills the belt (refills once, only at the moment the inventory panel opens; empty columns are claimed as red, red, blue, blue). Drink slots 1 and 2 from the bottom bar, the inventory has 3 minor healing potions, open the inventory panel → columns 1 and 2 each get 1 potion
    g=G("ba","DM_TEST_GIVE=hp1:2:3@60 DM_TEST_GIVE_INV=1"); g.sect("ba",0); g.enter(); g.wait(2.0)
    g.item("底栏喝掉腰带第 1、2 格","腰带 0/0/1/1",r"腰带 0/0/1/1"); g.tap("@belt1",1.0); g.tap("@belt2",2.0); g.shot()
    def ba_ok(s,b):
        m=re.search(r"开背包补腰带：开始.*?药水合计 (\d+)[\s\S]*?开背包补腰带：完成，放了 (\d+) 瓶.*?药水合计 (\d+)",s)
        if not m: return "不通：没有「开背包补腰带：开始 … 完成」两行"
        ok=m.group(2)=="2" and m.group(1)==m.group(3) and re.search(r"腰带 1/1/1/1",s)
        return ("通：" if ok else "不通：")+f"放了 {m.group(2)} 瓶、药水合计 {m.group(1)} → {m.group(3)}"+("" if re.search(r"腰带 1/1/1/1",s) else "；没见「腰带 1/1/1/1」")
    g.item("开物品栏","第 1、2 列各补 1 瓶小红，腰带 1/1/1/1，药水合计不变",ba_ok); g.tap("@menu",3.0); g.shot()
    gs.append(g)
    # bdm：带佣兵的 88 法师（佣兵生命压到三成）。底栏腰带点一下喝一瓶、点几下喝几瓶，都给自己——「双击 = 喂佣兵」已取消，喂佣兵只在佣兵页开着时双击腰带（q 局）
    # bdm: level-88 Sorceress with a mercenary (mercenary Life pushed down to 30%). One tap on the bottom-bar belt drinks one potion, several taps drink several, all for the player. "Double-tap = feed mercenary" has been removed; feeding the mercenary is only by double-tapping the belt with the mercenary page open (case q)
    g=G("bdm","DM_TEST_MERC_HP=30@80 DM_MERC_POT=0"); g.sect("bdm",0); g.enter(4); g.wait(12.0)
    g.item("底栏腰带第 1 格连点两下","都给自己喝：玩家生命涨、佣兵生命不变",both(php('+'),merc(0))); g.tap("@belt1",.25); g.tap("@belt1",5.0); g.shot()
    gs.append(g)
    # ---- 一键重开：回到前端按名字改选刚退出的角色再原版载入。BASE 钉了游戏时间，选角列表第一行永远是 joe——正好造出「刚退出的不在第一行」。每局连重开 3 次 ----
    # ---- One-tap restart: back at the front end, reselect the character that just exited by name, then load the original game's way. BASE pins the game time, so the first row of the character list is always joe - which exactly creates the case "the one that just exited is not in the first row". Each run restarts 3 times in a row ----
    def ms_ok(s,b):
        m=re.findall(r"一键重开：「([^」]+)」难度 (\d) 进局读取（从点按钮起 (\d+) ms）",s); c=re.findall(r"一键重开：进局后核对，当前角色「([^」]+)」，刚退出的是「([^」]+)」→ (\S+)",s)
        bad=re.search(r"OpenPanel (CharacterSelectPanel|MainMenuPanel|CharacterDifficultyModal)|【红】",s); sel=re.search(r"原版列表第 \d+ 项是「[^」]+」，选中槽原为 \d+ → (已改选它|本来就是它)",s)
        if not m: return "不通：没有「进局读取」行"
        if not c: return "不通：没有「进局后核对」行；"+f"读取 {m[-1][2]} ms"
        if not sel: return "不通：没有「原版列表第 N 项是…→ 已改选它 / 本来就是它」行"
        ok=int(m[-1][2])<=200 and c[-1][2].startswith("相符") and not bad
        return ("通：" if ok else "不通：")+f"{sel.group(1)}；「{m[-1][0]}」难度 {m[-1][1]}，点按钮到读取 {m[-1][2]} ms；核对 {c[-1][0]} / {c[-1][1]} → {c[-1][2]}"+(f"；出现「{bad.group(0)}」" if bad else "")
    def rs3(g,panel,want_diff):
        for k in range(3):
            g.tap("@menu"); g.tap(P(551,17),1.5)
            g.item(f"第 {k+1} 次一键重开","读取 ≤ 200 ms、进局后核对相符、难度 %d、不开选角页 / 主菜单"%want_diff,lambda s,b,d=want_diff: (lambda r: r if not r.startswith("通") or f"难度 {d}" in r else "不通："+r[2:])(ms_ok(s,b)))
            g.at(f"tap_ui TouchRestart@{panel} 3",9.0); g.shot()
    # rf1：76 级圣骑 joe 选地狱进局（选择难度框第 3 行），连重开 3 次
    # rf1: level 76 paladin joe enters the run choosing Hell (row 3 of the difficulty selection box), restart 3 times in a row
    g=G("rf1","DM_TEST_POS_LOG=1"); g.sect("rf1",0); g.ui("ListItem0@CharacterSelectPanel",1.3); g.tap(P(333,336),1.6); g.tap(P(333,203),1.0); g.at("await pos"); g.t=max(g.t,13.0); g.at("note 进局",1.5)
    rs3(g,"PauseLayoutGarden",2); gs.append(g)
    # rf2：88 级法师（带佣兵 → 经典版暂停页 PauseLayout），普通
    # rf2: level 88 sorceress (with a mercenary → classic pause page PauseLayout), Normal
    g=G("rf2","DM_TEST_POS_LOG=1"); g.sect("rf2",0); g.enter(4); g.wait(2.0); rs3(g,"PauseLayout",0); gs.append(g)
    # rf3：术士，iCloud 开着（本地目录当云端），普通
    # rf3: warlock, iCloud on (a local directory acts as the cloud), Normal
    g=G("rf3","DM_TEST_ICLOUD_DIR=icrF/saves DM_TEST_POS_LOG=1"); g.sect("rf3",0); g.enter(); g.wait(2.0); rs3(g,"PauseLayoutGarden",0)
    g.item("iCloud","每次退出都传一次（3 次「退出这一局回到前端，上传」）",lambda s,b: (lambda n: ("通：" if n>=3 else "不通：")+f"{n} 次")(len(re.findall(r"退出这一局回到前端，上传",b+s)))); g.shot()
    gs.append(g)
    # ---- 刷怪积分牌：kba 编年史「首领」页逐控件，kbb 击杀局（BASE 钉时间：首杀显示钉住的那个时刻）----
    # 编年史页签（iPhone 档 667 基准）：页签加宽 266 → 320 单位后，四个页签中心按「整排居中于 334、间距 (320+2)×0.2015 ≈ 64.9」算：237 / 302 / 366 / 431，y 55.5
    # （加宽前是 253 / 307 / 361 / 415，kbp 截图量）
    # ---- Kill board: kba checks the Chronicle "Bosses" page widget by widget, kbb is the kill run (BASE pins the time: the first kill shows the pinned
    # moment) ----
    # Chronicle tabs (iPhone profile, 667 baseline): after the tabs were widened 266 → 320 units, the four tab centers are computed as "row centered at 334,
    # pitch (320+2)×0.2015 ≈ 64.9": 237 / 302 / 366 / 431, y 55.5
    # (before widening they were 253 / 307 / 361 / 415, measured from regression-case screenshots)
    TABS={"暗金":237,"套装":302,"符文之语":366,"首领":431}
    def chron(g):
        g.tap("@menu"); g.tap(P(551,17),1.5)
        g.item("暂停 → 编年史","编年史开，四个页签（看截图）",r"OpenPanel ChroniclePanel"); g.at("tap_ui Chronicle@PauseLayoutGarden 3",2.0); g.shot()
    # kba：积分牌各控件逐个点（刷怪前的空牌）
    # kba: tap each kill board widget one by one (the empty board before any monster farming)
    g=G("kba"); g.sect("kba",0); g.enter(); chron(g)
    g.item("点第四页签「首领」","切到积分牌：左列五幕、三个勾选、排序下拉、左下总击杀（看截图）",r"界面页签：切"); g.tap(P(TABS["首领"],55.5),2.0); g.shot()
    for k in range(5):
        g.item(f"点第{'一二三四五'[k]}幕","切幕（看截图：右列换成这一幕的首领）",r"TouchKB_Act%d"%k); g.at(f"tap_ui TouchKB_Act{k} 3",1.5); g.shot()
    g.item("第五幕列表往上滑","长列表能滚（看截图）",None); g.at("tap_ui TouchKB_Act4 3",1.0); g.drag(P(460,250),P(460,110),.4); g.wait(1.0); g.shot()
    for k,nm in enumerate(("普通","噩梦","地狱")):
        g.item(f"勾「{nm}」","勾上（看截图）",r"TouchKB_D%d"%k); g.at(f"tap_ui TouchKB_D{k} 3",1.5); g.shot()
        g.item(f"再点「{nm}」","取消勾选",r"TouchKB_D%d"%k); g.at(f"tap_ui TouchKB_D{k} 3",1.5)
    for k,nm in enumerate(("按幕顺序","按次数","按最快")):
        g.item(f"排序下拉选「{nm}」","选中这项、不崩（看截图）",r"DropdownListWidgetMessage",r"CRASH|【红】"); g.at("tap_ui TouchKB_Sort 3",1.2); g.at(f"shot {g.name}_dd{k}.png",.3); g.tap(P(468,(86,95.5,105.5)[k]),1.5); g.shot()
    for nm in ("暗金","套装","符文之语"):
        g.item(f"切回「{nm}」页","原版页完好、积分牌不残留（看截图）",r"界面页签：切"); g.tap(P(TABS[nm],55.5),2.0); g.shot()
    g.item("关编年史再开","头一帧不闪积分牌（看截图：停在原版页）",r"OpenPanel ChroniclePanel"); g.tap(P(615,17),1.5); g.at("tap_ui Chronicle@PauseLayoutGarden 3",0.1); g.at(f"shot {g.name}_f1.png",1.5); g.shot()
    gs.append(g)
    # kbb：击杀局：冰冷之原刷安达莉尔、160 拍压成 1 点，测试开关带人走到怪旁，主钮连点；然后开积分牌看第一幕
    # kbb: kill run: spawn Andariel in the Cold Plains, at tick 160 pressed down to 1 point, the test switch walks the player next to the monster, tap the main button repeatedly; then open the kill board and look at Act 1
    g=G("kbb","DM_TEST_GOTO_LEVEL=60:2 DM_TEST_SPAWN=150:156:5,5 DM_TEST_SLAY=160 DM_TEST_WORLD_GO=near:monster@200 DM_TEST_STAT=7=2560000,6=2560000 DM_TEST_KILL_DROP=cap:7,lgl:5 DM_TEST_POS_LOG=1"); g.sect("kbb",0); g.enter(); g.t=max(g.t,16.0)
    g.item("首领出现","系统提示「安达莉尔已累计击杀 0 次」类（看截图）",r"刷怪积分牌：拍 \d+ 提示出现"); g.wait(2.0); g.shot()
    for k in range(16): g.at("down 1 @main",.12); g.at("up 1",.38)
    g.item("主钮连点 8 秒","击杀：日志「第 1 次」与「暗金 1、套装 1」，提示「第 1 次成功击杀」（看截图）",lambda s,b: ("通：" if re.search(r"刷怪积分牌：拍 \d+ \S+ 难度 0 第 1 次",b+s) and re.search(r"死后那一批掉落：暗金 1、套装 1",b+s) else "不通：")+"击杀与掉落两行（整局）"); g.wait(2.0); g.shot()
    g.wait(4.0); chron(g)
    g.item("点「首领」页签、第一幕","安达莉尔「击杀 1」「暗金 1 · 套装 1」、首杀时刻（看截图）",r"界面页签：切"); g.tap(P(TABS["首领"],55.5),2.0); g.at("tap_ui TouchKB_Act0 3",1.5); g.shot()
    g.item("宿主时区","日志「ToCalendarTime：首次换算」的偏移",lambda s,b: (lambda m: ("通：" if m else "未判：")+(m.group(0) if m else "没有这行"))(re.search(r"ToCalendarTime：首次换算[^\n]*",b+s)))
    gs.append(g)
    # ---- 看门狗：wd1 局后回前端发呆不误报判死；wd2 启动后不动仍判死。都带 DM_WATCH_STALL=15 ----
    # ---- Watchdog: wd1 idling at the front end after a run is not falsely declared dead; wd2 not moving after launch is still declared dead. Both use DM_WATCH_STALL=15 ----
    WD="DM_WATCH_STALL=15"
    def nodie(s,b):
        m=re.search(r"【红】看门狗[^\n]*",b+s); return ("不通："+m.group(0)[:90]) if m else "通：整局没有看门狗判死"
    g=G("wd1",WD); g.sect("wd1",0); g.enter(); g.wait(2.0)
    g.item("保存并退出","回选角页",r"FrontEndNavigation ToFrontEnd"); g.tap("@menu"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayoutGarden 3",5.0); g.shot()
    g.wait(30.0)
    g.item("前端停 30 秒后（看门狗 15 秒）","没有看门狗判死、进程还在",nodie); g.shot()
    g.item("再进局","进局",r"FrontEndNavigation ToGame"); g.ui("ListItem2@CharacterSelectPanel",1.3); g.tap(P(333,336),12.0); g.shot()
    gs.append(g)
    # wd2：项记号放在最前，判死行落在这一项的日志段里（判死后进程退出，后面的步骤本来就跑不到）
    # wd2: the item marker is placed at the very front, so the declared-dead line falls in this item's log segment (the process exits after being declared dead, so the later steps would never run anyway)
    g=G("wd2",WD); g.sect("wd2",0); g.t=3.5
    g.item("启动后什么都不点","按原设计判死：「【红】看门狗：第 N 秒起无进展」",r"【红】看门狗：第 \d+ 秒起无进展"); g.wait(40.0)
    gs.append(g)
    # ---- 个人仓库 2–5 页：pg1 88 法师开关翻走翻回、前后逐件比；pg2 术士触摸点个人页左右箭头、保存退出重进 ----
    # ---- Personal stash pages 2–5: pg1 level 88 sorceress, the test switch pages away and back, compare item by item before and after; pg2 warlock taps the personal page left/right arrows by touch, save, exit and re-enter ----
    def pglog(log,tick):   # DM_TEST_PG_LOG 在某拍打出的那批（物品码, 格, 孔里件数） | the batch printed by DM_TEST_PG_LOG at a given tick (item code, cell, number of items in sockets)
        rows=re.findall(r"个人仓库读数：拍 %d (\S+) 格 (\d+),(\d+) 孔里 (\d+) 件"%tick,log); return sorted(rows)
    def same(t0,t1):
        def f(s,b):
            a=pglog(b+s,t0); c=pglog(b+s,t1)
            if not a: return "未判：拍 %d 没有读数"%t0
            return ("通：" if a==c else "不通：")+f"拍 {t0} {len(a)} 件 / 拍 {t1} {len(c)} 件"+("，逐件（码、格、孔里件数）相同" if a==c else f"；不同：{sorted(set(a)^set(c))[:6]}")
        return f
    # pg1：88 法师（个人页满仓 90 件）——开关翻到第 2 页再翻回，前后逐件比
    # pg1: level 88 sorceress (personal page full with 90 items) - the switch flips to page 2 and back, compare item by item before and after
    g=G("pg1","DM_TEST_PG_LOG=150,700 DM_TEST_PG_GO=300:+1,500:-1"); g.sect("pg1",0); g.enter(4); g.t=max(g.t,24.0)
    g.item("翻到第 2 页（开关在拍 300 翻，早于本项记号，判整局）","「第 1 → 2 页，搬出 N 件」",lambda s,b: ("通：" if re.search(r"个人仓库：第 1 → 2 页，搬出 \d+ 件",b+s) and not re.search(r"【红】个人仓库",b+s) else "不通：")+"整局日志"); g.t=max(g.t,30.0); g.shot()
    g.item("翻回第 1 页","「第 2 → 1 页 … 放回 N 件」",r"个人仓库：第 2 → 1 页",r"【红】个人仓库"); g.t=max(g.t,38.0); g.shot()
    g.item("翻走翻回后","个人仓库格里逐件（码 / 格 / 孔里件数）与翻前相同",same(150,700)); g.t=max(g.t,44.0)
    gs.append(g)
    # pg2：术士，个人页上用触摸点左右箭头——卷轴双击进第 1 页 → 右箭头到第 2 页（空）→ 双击另一张卷轴进第 2 页 → 左箭头回第 1 页 → 保存退出重进 → 开箱看第 1 页、再翻到第 2 页
    # pg2: warlock, tap the left/right arrows by touch on the personal page - double-tap a scroll into page 1 → right arrow to page 2 (empty) → double-tap another scroll into page 2 → left arrow back to page 1 → save, exit and re-enter → open the stash and look at page 1, then flip to page 2
    g=G("pg2","DM_STATE=pwstate.jsonl"); g.sect("pg2",0); g.enter(); g.wait(2.0)
    g.item("点箱子","开储藏箱（个人页，底下页码「第 1/5 页」，看截图）",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 3",3.0); g.shot()
    g.item("个人页开着双击背包里的卷轴","进个人页第 1 页（看截图）",None); g.dbl(600.5,266); g.wait(1.5); g.clr(); g.shot()
    g.item("个人页点右箭头","翻到第 2 页：「个人仓库：第 1 → 2 页」、不发共享页翻页",r"个人仓库：第 1 → 2 页",r"SharedStashRight|【红】个人仓库"); g.at("tap_ui TouchStashRight@BankExpansionLayout 3",2.0); g.shot()
    g.item("第 2 页双击背包里另一张卷轴","进第 2 页（看截图）",None); g.dbl(600.5,285.5); g.wait(1.5); g.clr(); g.shot()
    g.item("点左箭头","回第 1 页",r"个人仓库：第 2 → 1 页",r"SharedStashLeft|【红】个人仓库"); g.at("tap_ui TouchStashLeft@BankExpansionLayout 3",2.0); g.shot()
    g.item("第 1 页再点左箭头","不回绕：不翻",lambda s,b: ("不通：翻了" if re.search(r"个人仓库：第 1 → \d+ 页",s) else "通：没翻")); g.at("tap_ui TouchStashLeft@BankExpansionLayout 3",2.0); g.shot()
    g.item("点「共享」页签再点右箭头","共享页照旧发 SharedStashRight、页码各显各的（看截图）",r"SharedStashRight"); g.at("tap_ui Image1@BankExpansionLayout 3",1.5); g.at("tap_ui TouchStashRight@BankExpansionLayout 3",2.0); g.shot()
    g.at("tap_ui Image0@BankExpansionLayout 3",1.5); g.tap(P(654.5,17),1.5)
    g.item("保存并退出","回选角页",r"FrontEndNavigation ToFrontEnd"); g.tap("@menu"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayoutGarden 3",5.0); g.shot()
    g.item("重进同一个角色","进局；载入个人仓库侧文件",r"个人仓库：\S+ 当前第 1 页，另外几页共 1 件"); g.ui("ListItem2@CharacterSelectPanel",1.3); g.tap(P(333,336),12.0); g.shot()
    g.item("开箱翻到第 2 页","第 2 页那张卷轴还在（看截图）",r"个人仓库：第 1 → 2 页"); g.at("tap_unit 2 267 3",3.0); g.at("tap_ui TouchStashRight@BankExpansionLayout 3",2.0); g.shot()
    gs.append(g)
    # vt1：60 级术士给一顶暗金软帽（背包右上 2×2，中心 591.9,236.3）→ 开箱 → 第六页签「编年史」(278.5,53) → 双击帽子存入 → 单击格子 (96.7,113.3) 出说明 → 拖回背包 → 搜「帽」→ 保存退出重进 → 再开第六页
    # vt1: give a level 60 warlock a unique cap (inventory top right 2×2, center 591.9,236.3) → open the stash → sixth tab 「编年史」(Chronicle) (278.5,53) → double-tap the cap to deposit it → single-tap the cell (96.7,113.3) to show the description → drag it back to the inventory → search 「帽」(cap) → save, exit and re-enter → open the sixth page again
    VE="DM_TEST_STAT=12=60 DM_TEST_GIVE=cap:7:1@60 DM_TEST_GIVE_INV=1 DM_KBD=帽"
    g=G("vt1",VE); g.sect("vt1",0); g.enter(); g.wait(2.0)
    g.item("点箱子","开储藏箱",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 3",3.0); g.shot()
    g.item("点第六页签「编年史」","切到编年史仓库页（看截图：暗金格、部位下拉、档次勾选、搜索）",r"编年史仓库：进第六页"); g.tap(P(181,53),1.5); g.shot()
    g.item("双击背包里的暗金帽子","存入：「→ 存进编年史仓库」「存入 …」",r"存进编年史仓库[\s\S]*编年史仓库：存入",r"【红】编年史仓库"); g.dbl(591.9,236.3); g.wait(2.0); g.dbl(591.9,236.3); g.wait(2.0); g.shot()   # 测试给的暗金未鉴定：第一下双击是免费鉴定，第二下才存入 | the unidentified unique given by the test: the first double-tap is a free identify, only the second one deposits it
    g.item("单击仓库格子","出这件的说明框（看截图）",None); g.tap(P(94.7,156.4),1.5); g.shot()
    g.item("说明框开着点「共享」页签","只收框、页签不切（vaultui2：框外任何地方只收框）",lambda s,b: ("不通：切了页签" if re.search(r"界面页签：切",s) else "通：没切页签")); g.tap(P(105.9,53),1.5); g.shot()
    g.item("从格子拖回背包","取出：「拖到背包格 … → 取出」「取出 … → 背包」",r"编年史仓库：[^\n]+ 拖到背包格[^\n]*→ 取出[\s\S]*取出 [^\n]*→ 背包",r"【红】编年史仓库"); g.dragp((94.7,156.4),(454.4,276.3)); g.wait(1.5); g.shot()
    g.item("再双击存回去","存入",r"编年史仓库：存入",r"【红】编年史仓库"); g.dbl(454.4,236.3); g.wait(2.0); g.shot()
    g.item("点搜索框（输入「帽」）","列表只剩帽子类（看截图）",None); g.tap(P(226,78),3.0); g.shot()
    g.tap(P(654.5,17),1.5)
    g.item("保存并退出","回选角页",r"FrontEndNavigation ToFrontEnd"); g.tap("@menu"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayoutGarden 3",5.0); g.shot()
    g.item("重进同一个角色","进局；编年史仓库读入 1 件",r"编年史仓库：\S+ 读入 1 件"); g.ui("ListItem2@CharacterSelectPanel",1.3); g.tap(P(333,336),12.0); g.shot()
    g.item("开箱第六页","那顶帽子还在格子里；普通 / 优异 / 精英三个框全勾，同第一次打开（看截图，对照 vt1_02）",r"编年史仓库：进第六页"); g.at("tap_unit 2 267 3",3.0); g.tap(P(181,53),1.5); g.shot()
    gs.append(g)
    # vf1：编年史独立页面修复回归（曾出现切到编年史页后装备不显示）。用一份真实玩家存档的副本（只在测试机容器里，不进仓库；第一行那个角色：个人页 3 件、共享页 1 满页、金币）——6 页签来回切；编年史页关箱再开；编年史页先关箱再退出、重进第一行
    # vf1: regression for the chronicle standalone page fix (equipment once did not show after switching to the chronicle page). Uses a copy of a real player save (only in the test device's container, not in the repository; the character in the first row: 3 items on the personal page, 1 full shared page, gold) - switch back and forth across the 6 tabs; on the chronicle page close the stash and reopen it; on the chronicle page close the stash first, then exit and re-enter the first row
    def ent(g): g.t=max(g.t,3.5); g.tap(P(333,336),1.6); g.tap(P(333,157),1.0); g.at("await pos"); g.t=max(g.t,13.0); g.at("note 进局",1.5); g.wait(2.0)
    g=G("vf1","DM_TEST_ITEM_DUMP=1500",save="save.sl_player"); g.sect("vf1",0); ent(g)
    g.item("开箱","个人页：3 件、金币框、页码（看截图）",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 3",3.0); g.clr(); g.shot()
    g.item("个人 → 编年史","编年史页",r"进第六页"); g.tap(P(181,53),1.5); g.shot()
    g.item("编年史 → 共享","共享页满页物品、金币（看截图）",r"界面页签"); g.tap(P(105.9,53),1.5); g.clr(); g.shot()
    g.item("共享 → 宝石","宝石页（看截图）",r"界面页签"); g.tap(P(148.8,53),1.5); g.clr(); g.shot()
    g.item("宝石 → 编年史","编年史页",r"进第六页"); g.tap(P(181,53),1.5); g.shot()
    g.item("编年史 → 个人","个人页 3 件、金币框、页码都在（看截图，对照第 1 张）",r"界面页签"); g.tap(P(62.5,53),1.5); g.clr(); g.shot()
    g.item("个人 → 共享","共享页满页物品还在（看截图，对照第 3 张）",r"界面页签"); g.tap(P(105.9,53),1.5); g.clr(); g.shot()
    g.tap(P(181,53),1.5)
    g.item("停在编年史页关箱","箱子关了",r"ClosePanel BankExpansionLayout|CloseAll|ClosePanel"); g.tap(P(654.5,16.7),1.5); g.shot()
    g.item("再开箱","开箱（看截图停在哪页）",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 3",3.0); g.clr(); g.shot()
    g.item("点个人","个人页 3 件都在（看截图）",None); g.tap(P(62.5,53),1.5); g.clr(); g.shot()
    g.item("点共享","共享页满页还在（看截图）",None); g.tap(P(105.9,53),1.5); g.clr(); g.shot()
    g.tap(P(181,53),1.5); g.tap(P(654.5,16.7),1.5)
    g.item("编年史页关箱后退出","回选角页",r"FrontEndNavigation ToFrontEnd"); g.tap("@menu"); g.tap(P(551,17)); g.at("tap_ui Exit@PauseLayoutGarden 3",5.0); g.shot()
    g.item("重进第一行","进局",None); g.t+=0.5; ent(g); g.shot()
    g.item("开箱","个人页 3 件都在（看截图）",r"OpenPanel BankExpansionLayout"); g.at("tap_unit 2 267 3",3.0); g.clr(); g.shot()
    g.item("点共享","共享页满页还在（看截图）",None); g.tap(P(105.9,53),1.5); g.clr(); g.shot()
    gs.append(g)
    # vf2：术士背包有魔盒——编年史页点 / 拖底部盖板那块，没有输入落到魔盒；两排部位页签、左右箭头、盖板
    # vf2: the warlock's inventory has the Horadric Cube - on the chronicle page tap / drag the bottom cover plate area, and no input lands on the cube; two rows of body-slot tabs, left/right arrows, cover plate
    g=G("vf2","DM_TEST_STAT=12=60 DM_TEST_GIVE=box:2:1@60 DM_TEST_GIVE_INV=1 DM_TEST_ITEM_DUMP=1400"+" DM_TEST_WORLD_GO=near2:2:c267@200"); g.sect("vf2",0); g.enter(); g.wait(2.0)
    g.item("开箱","个人页（背包里有魔盒，看截图）",r"OpenPanel BankExpansionLayout"); g.arrive(5808,4443,3,24.0); g.at("tap_unit 2 267 3",3.0); g.shot()
    g.item("点编年史","编年史页，没有魔盒容器 / 转化钮露出（看截图）",r"进第六页"); g.tap(P(181,53),1.5); g.shot()
    g.item("点原魔盒格那块","没打开魔盒、没有输入落到魔盒",r"界面命中",r"OpenPanel \S*Cube|魔盒|Transmute|转化"); g.tap(P(179.3,306.4),1.0); g.shot()
    g.item("从原魔盒格那块拖出去","同上，魔盒不动",r"界面命中",r"OpenPanel \S*Cube|魔盒|Transmute|转化"); g.drag(P(179.3,306.4),P(300,250)); g.shot()
    g.item("点部位页签「鞋子与腰带」","列表换成鞋子与腰带、选中帧带菱形（看截图）",None); g.tap(P(209.6,95.3),1.5); g.shot()
    g.item("点部位页签「法杖与法器」（第二排）","列表换成法杖与法器（看截图）",None); g.tap(P(279.1,117.2),1.5); g.shot()
    g.item("点部位页签「头盔」","头盔，第 1/4 页（看截图）",None); g.tap(P(70.5,95.3),1.5); g.shot()
    g.item("点右箭头","第 2/4 页（看截图）",None); g.tap(P(226.4,280.6),1.5); g.shot()
    g.item("点左箭头","回第 1/4 页（看截图）",None); g.tap(P(147.8,280.4),1.5); g.shot()
    g.item("点底部盖板","什么都不发、不冒原版说明（看截图）",lambda s,b: ("不通：" if re.search(r"OpenPanel TooltipsPanel|Cube|魔盒|转化",s) else "通：")+"盖板"); g.tap(P(179.3,306.4),1.5); g.shot()
    gs.append(g)
    # rwp：符文之语清单分页 / 详情——15 枚符文 + 两件布甲，开关直接开清单、显示 3 孔
    # rwp: runeword list paging / details - 15 runes + two quilted armors, the switch opens the list directly and shows 3 sockets
    RWOK=lambda s,b: ("通：" if re.search(r"排版自检",b+s) and not re.search(r"出底板 [1-9]|重叠 [1-9]|出框 [1-9]",b+s) else "不通：")+"排版自检（整局）"
    g=G("rwp","DM_TEST_GIVE=r01:2:1,r02:2:1,r03:2:1,r04:2:1,r05:2:1,r06:2:1,r07:2:1,r08:2:1,r09:2:1,r10:2:1,r11:2:1,r12:2:1,r13:2:1,r14:2:1,r15:2:1,qui:2:2@80 DM_TEST_GIVE_INV=1 DM_TEST_RW_OPEN=26:qui DM_TEST_RW_SOCKETS=3"); g.sect("rwp",0); g.enter(); g.wait(2.0); g.tap("@menu",1.4)
    g.t=max(g.t,45.0)
    g.item("清单打开（第 1 页）","第 1/2 页，每页 ≤ 8 条、条目间分隔线（看截图）",RWOK); g.shot()
    g.item("点下一页","「翻页 Next → 第 2/2 页（第 9–15 个）」",r"翻页 Next → 第 2/2 页（第 9–15 个）",r"【红】"); g.at("tap_ui TouchRwPgNext 3",2.0); g.shot()
    g.item("点第 14 条名字（谜团）","「详情「谜团」」→「详情自检 … 出框 0、重叠 0」（看截图）",r"详情「谜团」[\s\S]*详情自检「谜团」[^\n]*出框 0、重叠 0",r"【红】"); g.at("tap_ui TouchRwName13 3",2.0); g.shot()
    g.item("点返回","「收起详情」、回到清单",r"收起详情",r"【红】"); g.at("tap_ui TouchRwBack 3",2.0); g.shot()
    g.item("点上一页","「翻页 Prev → 第 1/2 页」",r"翻页 Prev → 第 1/2 页",r"【红】"); g.at("tap_ui TouchRwPgPrev 3",2.0); g.shot()
    g.item("整局排版自检","每条出底板 0、重叠 0",RWOK)
    gs.append(g)
    # rwd：详情里的一键合成——两枚符文 + 9 顶软帽，开关开第一顶能做的软帽的清单，点第 1 条名字进详情，点详情里的「一键合成」
    # rwd: one-tap craft inside the details - two runes + 9 caps, the switch opens the list for the first cap that can be made, tap the name of entry 1 to enter the details, tap 「一键合成」(Craft) in the details
    g=G("rwd","DM_TEST_GIVE=r04:2:1,r03:2:1,cap:2:9@80 DM_TEST_GIVE_INV=1 DM_TEST_RW_OPEN=26:cap"); g.sect("rwd",0); g.enter(); g.wait(2.0); g.tap("@menu",1.4)
    g.at("await ui TouchRwName0 40",40.0)   # 清单由 DM_TEST_RW_OPEN 按进程秒打开：等清单第 1 行名字出现就拨钟到下一步（按事件，40 秒是墙钟兜底） | the list is opened by DM_TEST_RW_OPEN on process seconds: once the name in row 1 of the list appears, advance the clock to the next step (by event, 40 seconds is the wall-clock fallback)
    g.item("点第 1 条名字进详情","详情（看截图）",r"详情「"); g.at("tap_ui TouchRwName0 3",2.0); g.shot()
    g.item("点详情里的一键合成","「一键合成成型」；宿主在结束那一刻读回：2 孔、孔里 2 颗、标志带 0x04000000",lambda s,b: ("通：" if re.search(r"一键合成成型[^\n]*标志 0x4",b+s) and re.search(r"成型读回（成型）[^\n]*2 孔、孔里 2 颗[^\n]*标志 0x4",b+s) else "不通：")+"成型与读回")
    g.at("await rw 25",0.1); g.at("tap_ui TouchRwDetGo 3",25.0); g.shot()   # 按事件收：await rw 等到宿主记完「成型读回」就拨钟到截图、随后「note 结束」；25 秒是墙钟兜底（原先按 DM_TEST_RW_LIST=44 进程秒读回，await pos 一跳就落在局外） | collected by event: await rw waits until the host has logged 「成型读回」(formed readback), then advances the clock to the screenshot, followed by 「note 结束」(note end); 25 seconds is the wall-clock fallback (it used to read back at DM_TEST_RW_LIST=44 process seconds, and one jump of await pos landed it outside the run)
    gs.append(g)
    # dc 局（补的覆盖缺口）：野外死亡 → 点原版死亡画面的「继续」→ 复活回城。整表原先没有任何一局走这条玩家路径，
    #   秘境图里死亡后点继续崩溃（线上出过）就漏在这里。这局是普通野外的基线；秘境里的死亡 → 继续另有专项。
    #   第 150 拍换到鲜血荒地（尸体留在野外）、第 450 拍 DM_TEST_KILL 走原版死亡流程；复活后人要在城里、生命回满、底栏读的是本人（喝一瓶腰带药读数跟着变）
    # dc run (fills a coverage gap): death in the wilderness → tap 「继续」(Continue) on the original game's death screen → revive back in town. The full table previously had no run at all that took this player path,
    #   and the crash on tapping Continue after dying in a rift map (seen in released builds) slipped through here. This run is the baseline for ordinary wilderness areas; death → Continue inside a rift has its own dedicated test.
    #   At tick 150 switch to the Blood Moor (the corpse stays in the wilderness), at tick 450 DM_TEST_KILL goes through the original game's death flow; after reviving the player must be in town, life back to full, and the bottom bar must read the player themselves (drink one belt potion and the reading changes with it)
    g=G("dc","DM_TEST_GOTO_LEVEL=150:2 DM_TEST_KILL=450 DM_TEST_POS_LOG=1"); g.sect("death",0); g.enter()
    g.item("第 150 拍换到鲜血荒地、第 450 拍死亡","换关、开始死亡模式、弹原版死亡画面",r"(?=[\s\S]*DM_TEST_GOTO_LEVEL：拍 \d+ 换到关卡 2)(?=[\s\S]*DM_TEST_KILL：拍 \d+ 对玩家)(?=[\s\S]*OpenPanel YouDiedModal)"); g.t=max(g.t,38.0); g.at("pos",.3); g.shot()
    g.item("点屏幕（原版「继续」）","死亡画面关、人复活在罗格营地（区域 1）、生命回满",lambda s,b: (lambda m: ("通：" if re.search(r"YouDiedModalMessage Continue",s) and re.search(r"ClosePanel YouDiedModal",s) and m and m[-1][0]=="1" and m[-1][1]==m[-1][2] and m[-1][1]!="0" else "不通：")+f"继续消息 {len(re.findall(r'YouDiedModalMessage Continue',s))} 条；最后位置读数 区域 {m[-1][0] if m else '?'} 生命 {m[-1][1]+'/'+m[-1][2] if m else '?'}")(re.findall(r"\[script\] pos 区域 (\d+) \(\d+,\d+\) 生命 (\d+)/(\d+)",s)))
    g.tap(P(333,200),2.0); g.tap(P(333,200),6.0); g.at("pos",.3); g.shot()
    g.item("点底栏腰带第 1 格","喝掉本人腰带上的一瓶：读数第 1 列变 0（读到的是复活后的本人，不是野外的尸体）",r"腰带 0/\d/\d/\d"); g.tap("@belt1",2.5); g.shot()
    gs.append(g)
    # od 局：老机型（不支持 BC 压缩的机型，走 ASTC 覆盖层）× 手柄 / 触屏来回切。的固定局（$DM_ROOT/run/tl/astc/oldpad.txt），进整表常驻、两档各一局：
    # 线上 10111427–10111636 所有 A16 及以前机型更新后卡在手柄界面进不了游戏，当时整表、冒烟、手柄专项没有一局带 DM_TEST_NO_BC，这条启动路径从没被执行过。
    # 前提两样，缺一样日志里就没有「boot: 用老机型补充包」、第一项判不通（= 前提没满足，这局无效，不算绿）：① 出包带老机型覆盖层（DM_ASTC_PACK=$DM_ROOT/out/astc_pack/current）；② 容器 Documents 里有「Legacy Device Pack」。
    # od run: a fixed run for older devices (devices without BC compression support, which use the ASTC overlay) × switching back and forth between
    # controller and touch ($DM_ROOT/run/tl/astc/oldpad.txt), a permanent member of the full regression table with one run per profile:
    # in released builds 10111427–10111636 every device with an A16 or earlier chip got stuck on the controller screen after updating and could not enter
    # the game; at the time not a single run in the full regression table, the smoke tests or the controller suite carried DM_TEST_NO_BC, so this startup
    # path had never been executed.
    # Two preconditions; if either is missing the log has no "boot: using the Legacy Device Pack" line (the App prints this line in Chinese; search the log for "boot:") and the first item fails (=
    # precondition not met, the run is invalid and does not count as green): (1) the package is built with the older-device overlay
    # (DM_ASTC_PACK=$DM_ROOT/out/astc_pack/current); (2) the container's Documents holds a "Legacy Device Pack" folder.
    def _od_xy(t): return re.findall(r"\[script\] pos 区域 \d+ \((\d+),(\d+)\)",t)
    g=G("od","DM_TEST_NO_BC=1"); g.sect("oldpad",0); g.enter()
    g.item("老机型开机进局","用老机型补充包、开机用触屏版、覆盖层带着布局标记",lambda s,b: (lambda t,bad: ("不通：" if bad else "通：")+("；".join(bad) if bad else "补充包在用、开机触屏版、标记在"))(b+s,[x for x,ok in (("没有「boot: 用老机型补充包」（包没带老机型覆盖层，或容器里没有补充包）","boot: 用老机型补充包" in b+s),("没有「开机用触屏版」","开机用触屏版" in b+s),("出现「覆盖层没有 ctrl_touch 标记」","覆盖层没有 ctrl_touch 标记" not in b+s)) if not ok])); g.at("pos",.5); g.shot()
    g.item("注入手柄摇杆","切到原版手柄版",r"切到原版手柄版",r"覆盖层没有 ctrl_touch 标记"); g.at("pad 0 0 32767",.8); g.at("pad 0",1.5); g.shot(); g.at("pos",.5)
    g.item("碰屏幕空地","切回触屏版",r"切到触屏版"); g.tap(P(520,120),3.0); g.shot(); g.at("pos",.5)
    g.item("触屏摇杆右推","人走了一段（前后两次位置不同）",lambda s,b: (lambda a,c: ("通：" if a and c and a[-1]!=c[-1] else "不通：")+f"推之前 {a[-1:]}，之后 {c[-1:]}")(_od_xy(b),_od_xy(s))); g.at("down 0 @joy",.3); g.at("move 0 @joy+40,0",1.2); g.at("up 0",1.0); g.at("pos",.5); g.shot()
    g.item("推摇杆切到手柄版，再按 ＋ 开物品栏、B 关","切到原版手柄版、物品栏开了又关（触屏模式下第一下手柄键只切界面、不执行，所以先推摇杆切过去）",r"(?=[\s\S]*切到原版手柄版)(?=[\s\S]*OpenPanel PlayerInventoryExpansionLayout)"); g.at("pad 0 0 32767",.6); g.at("pad 0",1.6); g.at("pad 0x400",.2); g.at("pad 0",1.5); g.shot(); g.at("pad 0x2",.2); g.at("pad 0",1.1)
    g.item("再碰屏幕空地","切回触屏版；整局没有「覆盖层没有 ctrl_touch 标记」",lambda s,b: ("通：" if "切到触屏版" in s and "覆盖层没有 ctrl_touch 标记" not in b+s else "不通：")+"这一步「切到触屏版」%d 行；整局缺标记 %d 行"%(s.count("切到触屏版"),(b+s).count("覆盖层没有 ctrl_touch 标记"))); g.tap(P(520,120),3.0); g.shot()
    gs.append(g)
    # pvb 局：手柄版商人页有刷新货架钮（一笔改动把它的覆盖件从生成清单里替掉过，整表没有一局看手柄版商人页，全通也没拦住）。
    # 进店同 ra 局（88 级法师点恰西），店开着注入右摇杆切手柄版看截图；iPhone 档再把光标挪到钮上按 A 判换货（光标比例是 16:9 画面的，别的档只看截图）。
    # Run pvb: the controller-style vendor page has the refresh-stock button (one change once replaced its overlay entry in the generated list; no run in the full table looked at the controller-style vendor page, so an all-green table did not catch it).
    # Enters the shop the same way as run ra (level-88 sorceress taps Charsi); with the shop open, a right-stick input switches to the controller style and the screenshot is checked; on the iPhone profile the cursor is then moved onto the button and A is pressed to check the stock changed (the cursor ratios are for a 16:9 picture, so other profiles only check the screenshot).
    g=G("pvb","DM_TEST_WORLD_GO=near2:1:c154@200 DM_TEST_GOLD=1300"); g.sect("padshop",0); charsi(g)
    g.item("店开着注入手柄右摇杆","切到手柄版、商人页还在，货架左下有刷新货架钮（看截图）",r"手柄：手柄有输入 → 手柄模式",r"CRASH|Terminating app"); g.at("pad 0 0 0 32767 0",.5); g.at("pad 0",2.5); g.at("padcur 0.414 0.790",1.5); g.shot()
    if MODE=="iphone":
        g.item("光标在刷新钮上按 A","货架换货",r"商人刷新：点了",r"CRASH|Terminating app"); g.at("pad 0x1",.2); g.at("pad 0",2.8); g.shot()
    gs.append(g)
    # psel0–3：手柄在角色列表里逐行选人，进局的必须是高亮的那一行（标准存档自上而下 joe / Warlock / joebao / 法师）。只用 pad 步：推右摇杆切手柄版 → X 开列表 → 十字键下 N 次 → A 选定 → A 进入游戏 → A 过难度。
    # psel0–3: the controller selects a character row by row in the character list, and the one that enters the run must be the highlighted row (standard save, top to bottom joe / Warlock / joebao / 法师). Only pad steps are used: push the right stick to switch to the controller layout → X opens the list → D-pad down N times → A selects → A enters the game → A passes the difficulty.
    def _ps(m,hold=.2,after=.3): g.at(f"pad {m}",hold); g.at("pad 0",after)
    for gn,n,nm in (("psel0",0,"joe"),("psel1",1,"Warlock"),("psel2",2,"joebao"),("psel3",3,"法师")):   # 局名写成元组里的字面量：并发分片的脚本靠读源里的局名枚举整表，拼出来的名字它认不到 | Run names are written as literals in the tuple: the concurrent sharding script enumerates the full table by reading run names from the source, and it cannot recognize names that are assembled
        g=G(gn); g.who=nm; g.padsel=True; g.sect(gn,0); g.t=5.0
        g.item(f"手柄选第 {n+1} 行「{nm}」并进局","主菜单角色牌是这一行的角色（看截图）；进局",r"\[script\] pos 区域 \d+")
        g.at("pad 0 0 0 32767 0",.5); g.at("pad 0",4.5); _ps("0x4",.2,1.6); [_ps("0x8000",.2,.5) for _ in range(n)]; g.shot(); _ps("0x1",.2,2.3); g.shot(); _ps("0x1",.2,2.8); _ps("0x1",.2,8.8); g.at("pos",.5); g.shot()
        gs.append(g)
    return gs
allrows=[]; T0=time.time(); notes=[]
for g in groups():
    if ONLY and g.run not in ONLY: continue
    if LETTERBOX and not ONLY and g.run in ("d","g","h","j","k","t","u","v","v2","m","n","p","ra","q","pk","g1","g2","g3","g4","ex","tb","tp"): continue   # 成叠取件、技能绑定是纯逻辑，留边档不跑（点名要跑仍可以） | stack pickup and skill binding are pure logic, not run in letterbox mode (can still be run when named explicitly)
    rows,wall,cap,red,tm=run(g); allrows+=rows; notes.append(f"局 {g.run}：墙钟 {wall:.0f} 秒（脚本上限 {cap}）；{tm}；日志【红】{red} 行"); print("# "+notes[-1],flush=True)
real=[r for r in allrows if not r[1].startswith("（整局）") or r[1].startswith("（整局）不该出现") or r[1].startswith("（整局）该出现")]   # 整局的禁词 / 必有词两行计入通与不通 | the two whole-run rows (forbidden words / required words) count toward pass and fail
ok=sum(r[3].startswith("通") for r in real); un=sum(r[3].startswith("未判") for r in real); bad=len(real)-ok-un
with open(f"{OUT}/表.md","w") as f:
    f.write(f"# 逐界面回归（{MODE}，{time.strftime('%m-%d %H:%M')}）\n\n共 {len(real)} 项：通 {ok}、不通 {bad}、未判 {un}；合计墙钟 {time.time()-T0:.0f} 秒\n\n"+"".join(f"- {n}\n" for n in notes)+"\n| 组 | 项 | 期望 | 真结果 | 截图 |\n|---|---|---|---|---|\n")
    for r in allrows: f.write("| "+" | ".join(r)+" |\n")
print(f"# 共 {len(real)} 项：通 {ok}、不通 {bad}、未判 {un}；合计墙钟 {time.time()-T0:.0f} 秒；表 {OUT}/表.md")
for r in real:
    if not r[3].startswith("通"): print("  "+" | ".join(r))
