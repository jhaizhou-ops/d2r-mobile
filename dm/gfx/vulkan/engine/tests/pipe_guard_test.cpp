// 直接调用生产的管线守护簿记：开机判定、跳过组、嫌疑的交集与缩小；只有字符串运算，不需要 Vulkan 或 GPU。
// 编译运行：g++ -std=c++17 -Wall -Wextra dm/gfx/vulkan/engine/tests/pipe_guard_test.cpp -o /tmp/pipe_guard_test && /tmp/pipe_guard_test
// Call the production pipeline-guard bookkeeping directly: boot verdicts, skip groups, suspect intersections and shrinking; string
// work only, no Vulkan or GPU.
// Build and run: g++ -std=c++17 -Wall -Wextra dm/gfx/vulkan/engine/tests/pipe_guard_test.cpp -o /tmp/pipe_guard_test && /tmp/pipe_guard_test
#include "../xbg_pipe_guard.h"
#include <stdio.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)
using Keys=std::vector<std::string>;
using Entries=std::vector<std::string>;
using Proven=std::set<std::string>;

int main(){
    // 条目：去重、排序、去掉已建成的；空条目表示不用记号。
    // Entries: unique, sorted, without proven keys; an empty entry means no marker.
    CHECK(xbg_pipe_guard_entry({"b","a","b"},{})=="a b");
    CHECK(xbg_pipe_guard_entry({"b","a"},{"a"})=="b");
    CHECK(xbg_pipe_guard_entry({"a"},{"a"}).empty());
    CHECK(xbg_pipe_guard_entry({"",""},{}).empty());
    CHECK(xbg_pipe_guard_split(" a  b ")==Keys({"a","b"}));
    CHECK(xbg_pipe_guard_join({"c","a","c"})=="a c");

    // 计算着色器那种单键条目：第一次死只是嫌疑，第二次死在同一处才坐实；不含它的管线照常建。
    // The compute shader's one-key entry: the first death is a suspect, the second at the same place confirms it; pipelines without it build.
    {
        xbg_pipe_guard_state st;
        auto v=xbg_pipe_guard_boot(st,{"K"},{});
        CHECK(v.confirmed.empty() && st.groups.empty() && st.suspects==Entries({"K"}) && v.suspects_changed && !v.groups_changed);
        v=xbg_pipe_guard_boot(st,{"K"},{});
        CHECK(v.confirmed.size()==1 && v.groups_changed && st.groups==std::vector<Keys>({{"K"}}) && st.suspects.empty());
        CHECK(xbg_pipe_guard_skipped(st,{"K"}));
        CHECK(xbg_pipe_guard_skipped(st,{"V","K"}));
        CHECK(!xbg_pipe_guard_skipped(st,{"Z"}));
    }
    // 要连着两次：中间一次没死在建管线上，嫌疑清空，得重新数（被系统杀掉的那一次不算数）。
    // It takes two in a row: a boot in between that did not die building a pipeline clears the suspects, and counting starts over
    // (the one kill by the system does not count).
    {
        xbg_pipe_guard_state st;xbg_pipe_guard_boot(st,{"K"},{});
        auto v=xbg_pipe_guard_boot(st,{},{});
        CHECK(v.suspects_changed && st.suspects.empty() && v.confirmed.empty());
        v=xbg_pipe_guard_boot(st,{},{});
        CHECK(!v.suspects_changed && !v.groups_changed);
        v=xbg_pipe_guard_boot(st,{"K"},{});
        CHECK(v.confirmed.empty() && st.suspects==Entries({"K"}));
        v=xbg_pipe_guard_boot(st,{"K"},{});
        CHECK(v.confirmed.size()==1 && xbg_pipe_guard_skipped(st,{"K"}));
    }
    // 两条线程同时在建时死了：两条都成嫌疑；下一次一条一条地建，死时只有元凶在建，坐实的是它在嫌疑里出现过的键。
    // Two threads building at the death: both become suspects; the next boot builds one at a time, only the culprit is in flight
    // when it dies, and what is confirmed is its keys that appear among the suspects.
    {
        xbg_pipe_guard_state st;
        xbg_pipe_guard_boot(st,{"P","V"},{});
        CHECK(st.suspects==Entries({"P","V"}));
        auto v=xbg_pipe_guard_boot(st,{"P V"},{});
        CHECK(v.confirmed==std::vector<Keys>({{"P","V"}}) && st.suspects.empty());
        v=xbg_pipe_guard_boot(st,{},{"V"});
        CHECK(st.groups==std::vector<Keys>({{"P"}}));
    }
    // 图形管线：顶点与像素两个键都没建成过时条目是两个键；上次建成过的键不算。
    // A graphics pipeline: the entry holds both keys when neither has built; keys the last boot built do not count.
    {
        xbg_pipe_guard_state st;
        auto v=xbg_pipe_guard_boot(st,{"P V"},{"V"});
        CHECK(st.suspects==Entries({"P"}) && v.confirmed.empty());
        v=xbg_pipe_guard_boot(st,{"V P"},{"V"});
        CHECK(v.confirmed==std::vector<Keys>({{"P"}}) && st.groups==std::vector<Keys>({{"P"}}));
        CHECK(xbg_pipe_guard_skipped(st,{"V","P"}) && xbg_pipe_guard_skipped(st,{"W","P"}) && !xbg_pipe_guard_skipped(st,{"V","Q"}));
    }
    // 分不清时整组跳过（只跳含齐两个键的管线），以后哪个键在别处建成了就从组里去掉。
    // When the culprit cannot be told apart the whole group is skipped (only pipelines holding both keys); a key that later builds elsewhere leaves the group.
    {
        xbg_pipe_guard_state st;
        xbg_pipe_guard_boot(st,{"P V"},{});
        auto v=xbg_pipe_guard_boot(st,{"P V"},{});
        CHECK(v.confirmed==std::vector<Keys>({{"P","V"}}) && st.groups==std::vector<Keys>({{"P","V"}}));
        CHECK(xbg_pipe_guard_skipped(st,{"V","P"}) && !xbg_pipe_guard_skipped(st,{"V","Q"}) && !xbg_pipe_guard_skipped(st,{"W","P"}));
        v=xbg_pipe_guard_boot(st,{},{"V"});
        CHECK(v.groups_changed && st.groups==std::vector<Keys>({{"P"}}) && xbg_pipe_guard_skipped(st,{"W","P"}));
        v=xbg_pipe_guard_boot(st,{},{"P"});
        CHECK(v.groups_changed && st.groups.empty());
    }
    // 嫌疑与条目只有部分重合时，重合的部分坐实。
    // When a suspect and an entry only partly overlap, the overlap is what gets confirmed.
    {
        xbg_pipe_guard_state st;
        xbg_pipe_guard_boot(st,{"P V"},{});
        auto v=xbg_pipe_guard_boot(st,{"P W"},{});
        CHECK(v.confirmed==std::vector<Keys>({{"P"}}) && st.groups==std::vector<Keys>({{"P"}}) && st.suspects.empty());
    }
    // 几条线程同时在建：两次都在建的那条坐实，其余成为新嫌疑。
    // Several threads building at once: the entry in flight both times is confirmed, the others become the new suspects.
    {
        xbg_pipe_guard_state st;
        xbg_pipe_guard_boot(st,{"A B","C D"},{});
        auto v=xbg_pipe_guard_boot(st,{"E F","A B"},{});
        CHECK(v.confirmed==std::vector<Keys>({{"A","B"}}) && st.groups==std::vector<Keys>({{"A","B"}}) && st.suspects==Entries({"E F"}));
    }
    // 新组与旧组有交集时，交集另成一组。
    // A new group intersecting an old one adds the intersection as a group.
    {
        xbg_pipe_guard_state st;
        CHECK(xbg_pipe_guard_record(st,{"B","A"}) && st.groups==std::vector<Keys>({{"A","B"}}));
        CHECK(!xbg_pipe_guard_record(st,{"A","B"}));
        CHECK(xbg_pipe_guard_record(st,{"D","B"}) && st.groups.size()==3 && xbg_pipe_guard_has_group(st,{"B"}));
        CHECK(xbg_pipe_guard_skipped(st,{"B","Z"}));
        CHECK(!xbg_pipe_guard_record(st,{}));
    }
    // 键全都建成过的条目不是编译器崩溃，不记。
    // An entry whose keys all built elsewhere is not a compiler crash and is not recorded.
    {
        xbg_pipe_guard_state st;
        auto v=xbg_pipe_guard_boot(st,{"P V"},{"P","V"});
        CHECK(v.confirmed.empty() && st.suspects.empty() && st.groups.empty());
    }
    if(failures){printf("%d failures\n",failures);return 1;}
    printf("pipe guard OK\n");return 0;
}
