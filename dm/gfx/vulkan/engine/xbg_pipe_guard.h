// 管线建立守护的簿记（纯逻辑，不碰 Vulkan 和文件）：xbg_game.cpp 的 pipeguard 读写记号文件，判定交给这里；宿主测试直接调用。
// The pipeline-build guard's bookkeeping (pure logic, no Vulkan or files): xbg_game.cpp's pipeguard reads and writes the marker
// files and leaves the verdicts to this header; the host test calls it directly.
#pragma once
#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <vector>

// 一条在建条目：这条管线里还没建成过的着色器键，去重排序后以空格相连；空串表示不用记号。
// An in-flight entry: the pipeline's shader keys not built successfully yet, unique and sorted, joined by spaces; empty means no marker.
inline std::vector<std::string> xbg_pipe_guard_split(const std::string &entry){
    std::vector<std::string> keys;size_t i=0;
    while(i<entry.size()){size_t j=entry.find(' ',i);if(j==std::string::npos)j=entry.size();if(j>i)keys.push_back(entry.substr(i,j-i));i=j+1;}
    return keys;
}
inline std::vector<std::string> xbg_pipe_guard_sorted(std::vector<std::string> keys){
    keys.erase(std::remove(keys.begin(),keys.end(),std::string()),keys.end());
    std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());return keys;
}
inline std::string xbg_pipe_guard_join(const std::vector<std::string> &keys){
    std::string e;for(const auto &k:xbg_pipe_guard_sorted(keys)){if(!e.empty())e+=' ';e+=k;}return e;
}
inline std::string xbg_pipe_guard_entry(const std::vector<std::string> &keys,const std::set<std::string> &proven){
    std::vector<std::string> left;for(const auto &k:keys)if(!proven.count(k))left.push_back(k);
    return xbg_pipe_guard_join(left);
}

// 跳过组：管线含齐一组里的全部键就不建，它的绘制或派发直接跳过。单键组即「含这个着色器的都跳过」（计算着色器只会是这种）。
// 多键组是坐实时分不清哪个有罪：以后哪个键在别的管线里建成了，就从组里去掉，剩一个键时就确定了。
// Skip groups: a pipeline holding every key of a group is not built, and its draws or dispatches are skipped. A one-key group means
// "everything with this shader" (a compute shader can only be that). A multi-key group is a confirmed crash whose culprit could not
// be told apart: a key that later builds in another pipeline leaves the group, and one remaining key settles it.
struct xbg_pipe_guard_state {
    std::vector<std::vector<std::string>> groups;
    std::vector<std::string> suspects;
};
inline bool xbg_pipe_guard_has_group(const xbg_pipe_guard_state &st,const std::vector<std::string> &g){
    for(const auto &x:st.groups)if(x==g)return true;
    return false;
}
// 记一组；与现有组有交集时，交集另成一组（两次坐实里都在的键最可疑）。返回是否有新组。
// Record a group; where it intersects an existing group, the intersection becomes a group of its own (a key present in both
// confirmed crashes is the likeliest culprit). Returns whether anything new was added.
inline bool xbg_pipe_guard_record(xbg_pipe_guard_state &st,const std::vector<std::string> &keys){
    std::vector<std::string> g=xbg_pipe_guard_sorted(keys);if(g.empty())return false;
    std::vector<std::vector<std::string>> add;
    for(const auto &x:st.groups){
        std::vector<std::string> both;std::set_intersection(x.begin(),x.end(),g.begin(),g.end(),std::back_inserter(both));
        if(!both.empty() && both!=x && both!=g)add.push_back(both);
    }
    add.push_back(g);bool added=false;
    for(const auto &a:add)if(!xbg_pipe_guard_has_group(st,a)){st.groups.push_back(a);added=true;}
    return added;
}
inline bool xbg_pipe_guard_skipped(const xbg_pipe_guard_state &st,const std::vector<std::string> &keys){
    for(const auto &g:st.groups){
        bool all=true;for(const auto &k:g)if(std::find(keys.begin(),keys.end(),k)==keys.end()){all=false;break;}
        if(all)return true;
    }
    return false;
}
// 开机判定。inflight：上次死时在建的条目；proven：上次建成过的键；st.suspects：上上次留下的嫌疑。
// 条目里上次建成的键去掉（它在别处建成了，不是元凶）；剩下的键里在嫌疑中出现过的坐实，成为一组；没出现过的成为新嫌疑。
// 键全都建成过的条目不是着色器编译器的崩溃，不理。要连着两次启动死在同一处才坐实：一次可能只是进程在那几毫秒里被系统杀掉，
// 所以上次没有死在建管线上（没有在建条目）时，嫌疑清空。有嫌疑的那次启动，没建成过的管线一条一条地建（见 xbg_game.cpp 的
// pipeguard::begin），死时在建的条目只有一条，元凶就是它，不会冤枉同时在别的线程上建的管线。现有的组也按 proven 缩小，缩空即删
// （组里的键都在别处建成过，没有哪个是元凶）。
// Boot verdict. inflight: the entries in flight when the last boot died; proven: the keys it built; st.suspects: what the boot
// before it left. Keys the last boot built leave an entry (they built elsewhere, so they are not the culprit); the remaining keys
// that appear in a suspect are confirmed as a group, and an entry with none becomes a new suspect. An entry whose keys all built
// elsewhere is not a shader-compiler crash and is left alone. It takes two boots in a row dying at the same place: once may just be
// the process being killed during those milliseconds, so when the last boot did not die building a pipeline (no in-flight entries),
// the suspects are cleared. A boot with suspects builds its not-yet-built pipelines one at a time (pipeguard::begin in
// xbg_game.cpp), so it dies with one entry in flight: that one is the culprit, and pipelines building on other threads at the same
// moment are not blamed. Existing groups shrink by proven as well, and a group shrunk to nothing is dropped (every key in it built
// elsewhere, so none is the culprit).
struct xbg_pipe_guard_verdict {std::vector<std::vector<std::string>> confirmed;bool groups_changed=false,suspects_changed=false;};
inline xbg_pipe_guard_verdict xbg_pipe_guard_boot(xbg_pipe_guard_state &st,const std::vector<std::string> &inflight,const std::set<std::string> &proven){
    xbg_pipe_guard_verdict v;
    for(auto &g:st.groups){
        size_t n=g.size();g.erase(std::remove_if(g.begin(),g.end(),[&](const std::string &k){return proven.count(k)!=0;}),g.end());
        if(g.size()!=n)v.groups_changed=true;
    }
    std::vector<std::vector<std::string>> kept;
    for(const auto &g:st.groups)if(!g.empty() && std::find(kept.begin(),kept.end(),g)==kept.end())kept.push_back(g);
    if(kept.size()!=st.groups.size())v.groups_changed=true;
    st.groups=kept;
    if(inflight.empty()){if(!st.suspects.empty()){st.suspects.clear();v.suspects_changed=true;}return v;}
    std::set<std::string> suspected;for(const auto &s:st.suspects)for(const auto &k:xbg_pipe_guard_split(s))suspected.insert(k);
    std::vector<std::string> next;
    for(const auto &raw:inflight){
        std::vector<std::string> keys=xbg_pipe_guard_split(xbg_pipe_guard_entry(xbg_pipe_guard_split(raw),proven));if(keys.empty())continue;
        std::vector<std::string> hit;for(const auto &k:keys)if(suspected.count(k))hit.push_back(k);
        if(!hit.empty()){if(xbg_pipe_guard_record(st,hit))v.groups_changed=true;v.confirmed.push_back(hit);}
        else{std::string e=xbg_pipe_guard_join(keys);if(std::find(next.begin(),next.end(),e)==next.end())next.push_back(e);}
    }
    v.suspects_changed=true;st.suspects=next;return v;
}
