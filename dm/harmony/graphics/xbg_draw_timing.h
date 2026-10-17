#pragma once
#include "xbg_cpu_timing.h"
// Root Draw exclusive progress: nested main parts are deducted at both endpoints.
// Serialized by graphics mutex; default-off scopes/counters never read clock.
struct XbgDrawTiming {
    enum Kind { Writeback, VertexPrepare, Count };
    enum Flag { BadClock=1, Overflow=2, Stack=4, Coverage=8 };
    struct Counter { uint64_t calls=0, ns=0; } counters[Count];
    struct Scope;
    Scope *top=nullptr;
    uint64_t reads=0,storage_ranges=0,images_visited=0,writeback_bytes=0,index_expand_calls=0,index_expand_bytes=0;
    uint32_t flags=0;
    void add(uint64_t &value,uint64_t amount){if(amount>UINT64_MAX-value)flags|=Overflow;else value+=amount;}
    uint64_t position(XbgCpuTiming::Scope *root){
        const uint64_t now=root->recorder->clock();add(reads,1);
        if(root->kind!=XbgCpuTiming::Draw || root->recorder->top!=root){flags|=Stack;return 0;}
        if(now<root->start || root->children>now-root->start){flags|=BadClock;return 0;}
        return now-root->start-root->children;
    }
    struct Scope {
        XbgDrawTiming *owner;XbgCpuTiming::Scope *root;Scope *parent=nullptr;
        Kind kind;uint64_t begin=0,children=0;
        Scope(XbgDrawTiming *o,XbgCpuTiming::Scope *r,Kind k):owner(o),root(r),kind(k){
            if(owner && root && root->recorder){parent=owner->top;begin=owner->position(root);owner->top=this;}else owner=nullptr;
        }
        ~Scope(){
            if(!owner)return;
            if(owner->top!=this)owner->flags|=Stack;
            uint64_t end=owner->position(root),elapsed=0,exclusive=0;
            if(end<begin)owner->flags|=BadClock;else elapsed=end-begin;
            if(children>elapsed)owner->flags|=BadClock;else exclusive=elapsed-children;
            auto &c=owner->counters[kind];owner->add(c.calls,1);owner->add(c.ns,exclusive);
            if(parent)owner->add(parent->children,elapsed);
            owner->top=parent;
        }
        Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    };
    bool in(Kind kind)const{return top && top->kind==kind;}
    void storage(uint64_t bytes){if(in(Writeback)){add(storage_ranges,1);add(writeback_bytes,bytes);}}
    void image(){if(in(Writeback))add(images_visited,1);}
    void index(uint64_t count){if(in(VertexPrepare)){add(index_expand_calls,1);if(count>UINT64_MAX/2)flags|=Overflow;else add(index_expand_bytes,count*2);}}
    uint64_t sum(){uint64_t value=0;for(const auto &c:counters)add(value,c.ns);return value;}
    uint64_t finish(uint64_t total){
        const uint64_t tagged=sum();if(top)flags|=Stack;
        uint64_t calls=0;for(const auto &c:counters)add(calls,c.calls);
        if(calls>UINT64_MAX/2)flags|=Overflow;else if(reads!=calls*2)flags|=Coverage;
        if(tagged>total)flags|=Coverage;
        return tagged;
    }
};
