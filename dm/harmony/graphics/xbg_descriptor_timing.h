#pragma once
#include "xbg_cpu_timing.h"
// Same Descriptor root exclusive clock; child main scopes remain in original parts.
// Caller holds graphics recursive mutex. No allocation/I/O in measured subscopes.
struct XbgDescriptorTiming {
    enum Kind { Setup, BufferBind, ImageLookup, ImageTransition, ImageView, SamplerBind, ElementRest, Count };
    struct Counter { uint64_t calls=0, ns=0; } counters[Count];
    struct Scope;
    Scope *top=nullptr;
    uint64_t reads=0; bool invalid=false;
    uint64_t position(XbgCpuTiming::Scope *root) {
        uint64_t now=root->recorder->clock();
        if(reads==UINT64_MAX)invalid=true;else ++reads;
        if(root->recorder->top!=root || now<root->start || root->children>now-root->start){invalid=true;return 0;}
        return now-root->start-root->children;
    }
    struct Scope {
        XbgDescriptorTiming *owner; XbgCpuTiming::Scope *root;
        Scope *parent=nullptr; Kind kind; uint64_t begin=0, children=0;
        Scope(XbgDescriptorTiming *o,XbgCpuTiming::Scope *r,Kind k):owner(o),root(r),kind(k){
            if(owner && root && root->recorder){begin=owner->position(root);parent=owner->top;owner->top=this;}else owner=nullptr;
        }
        ~Scope(){if(!owner)return;uint64_t end=owner->position(root),elapsed=end>=begin?end-begin:0;
            if(end<begin || children>elapsed)owner->invalid=true;
            auto &c=owner->counters[kind];uint64_t exclusive=children<=elapsed?elapsed-children:0;
            if(c.calls==UINT64_MAX || c.ns>UINT64_MAX-exclusive)owner->invalid=true;
            else{++c.calls;c.ns+=exclusive;}
            if(parent){if(parent->children>UINT64_MAX-elapsed)owner->invalid=true;else parent->children+=elapsed;}owner->top=parent;
        }
        Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    };
    uint64_t sum() {uint64_t n=0;for(const auto &c:counters){if(n>UINT64_MAX-c.ns){invalid=true;return 0;}n+=c.ns;}return n;}
};
