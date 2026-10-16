#!/usr/bin/env bash
# 同步面审计：数一数"我们 vs 上游"改了哪些文件、动上游文件的数量有没有涨。
# 每次迭代收尾跑一遍；规矩见 docs/android-isolation-analysis.md「硬性规矩」。
set -u
cd "$(git rev-parse --show-toplevel)"

UP=upstream/main
git rev-parse --verify "$UP" >/dev/null 2>&1 || { echo "【红】找不到 $UP（先 git fetch upstream）"; exit 1; }
M=$(git merge-base HEAD "$UP")

echo "=== 同步面审计（merge-base: $(git rev-parse --short $M)）==="
echo

new=$(git diff --name-status $M..HEAD | grep -c '^A')
mod=0; shared=0
while read -r st f; do
  [ "$st" = "M" ] || continue
  mod=$((mod+1))
  git cat-file -e "$UP:$f" 2>/dev/null && shared=$((shared+1))
done < <(git diff --name-status $M..HEAD)

total=$((new+mod))
echo "改动文件总数 : $total"
echo "  新建文件   : $new   (不同步冲突，越多越好)"
echo "  改动文件   : $mod   (其中 $shared 个是上游也有的文件 = 冲突面)"
echo

echo "=== 冲突面明细（上游也有的文件；按改动行数排序）==="
printf "%-52s %8s %8s  %s\n" "文件" "+行" "-行" "形态"
git diff --numstat $M..HEAD | while read -r a d f; do
  [ "$f" = "" ] && continue
  git cat-file -e "$UP:$f" 2>/dev/null || continue
  guards=$(git diff $M..HEAD -- "$f" | grep -cE '^[+-].*(ifdef|ifndef) __ANDROID__')
  marks=$(grep -c "ANDROID-BEGIN" "$f" 2>/dev/null || true)
  shape="未标"
  [ "$guards" -gt 0 ] && shape="双分支(guard×$guards)"
  [ "${marks:-0}" -gt 0 ] && shape="区段标记($marks)+guard×$guards"
  case "$f" in *.py) shape="工具(纯追加)" ;; esac
  printf "%-52s %8s %8s  %s\n" "$f" "$a" "$d" "$shape"
done

echo
echo "=== 规矩提醒 ==="
echo "· 改动文件(冲突面)数量只许降或持平；涨了要在提交说明里写原因"
echo "· 新代码进 dm/android/ 或新文件；动上游文件用：双分支 / 小钩子 / ANDROID-BEGIN 区段"
echo "· 枚举编号严禁中间插；改编号必须同树重新生成游戏模块"
echo "· 策略表（唯一权威）：docs/android-isolation-analysis.md 文末"
