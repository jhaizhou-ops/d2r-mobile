#pragma once
// One state per process, protected by Request::mutex. Runtime startup is one-shot.
struct XblKeyboardWaitState {
    bool pending=false,complete=false,closed=false;
    int id=0,result=-1;
    int begin() {
        if(closed)return 0;
        if(pending)return -1;
        pending=true;complete=false;++id;result=-1;return 2;
    }
    bool accepts(int requestId) const {return pending && !complete && id==requestId;}
    bool answer(int requestId,int value) {
        if(!accepts(requestId))return false;
        result=value;complete=true;return true;
    }
    bool close(bool runtimeStarted=true) {
        // Ability recreation before the first startup must not poison the future first request.
        if(!runtimeStarted)return false;
        closed=true;
        if(!pending || complete)return false;
        result=0;complete=true;return true;
    }
    template<class Wait> int waitResult(Wait wait) {
        const bool done=wait([this]{return complete;});
        pending=false;return done?result:-1;
    }
};
