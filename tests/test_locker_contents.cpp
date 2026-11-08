#include "modloader/src/locker_contents.h"
#include <cassert>
using namespace locker_contents;
int main(){
    Authority a;assert(a.initialize({{"titanium-1",16,{1,2,3}}}));
    Message take;take.kind=Request;take.locker="locker-1";take.request=1;take.revision=1;take.item={"titanium-1",16,{}};
    assert(a.begin(11,take).kind==Grant);
    assert(a.begin(11,take).kind==Grant); // retransmit cannot reserve twice
    assert(a.begin(22,take).kind==Reject); // two players, one item
    auto spoof=take;spoof.item.id="other-item";assert(a.finish(22,spoof,true).kind==Reject);
    assert(a.items.size()==1&&a.reserved); // disconnect does not unlock
    auto done=a.finish(11,spoof,true);assert(done.kind==Done&&done.item.id=="titanium-1"&&a.items.empty()&&a.revision==2);
    assert(a.finish(11,take,true).revision==2&&a.begin(11,take).kind==Done);
    auto deposit=take;deposit.request=2;deposit.revision=1;deposit.deposit=true;deposit.item={"new-resource",16,{7,9}};
    assert(a.begin(11,deposit).kind==Reject); // stale snapshot
    deposit.request=3;deposit.revision=2;assert(a.begin(11,deposit).kind==Grant);
    assert(a.finish(11,deposit,false).kind==Reject&&a.items.empty()&&a.revision==2); // full destination/native failure
    deposit.request=4;assert(a.begin(11,deposit).kind==Grant);assert(a.finish(11,deposit,true).revision==3);
    assert(a.items.at("new-resource").data==deposit.item.data);
    deposit.request=5;deposit.revision=3;assert(a.begin(22,deposit).kind==Reject); // duplicate identity
    Authority full;std::vector<Item> capacity;
    for(size_t j=0;j<kMaxItems;++j)capacity.push_back({"resource-"+std::to_string(j),16,{}});
    assert(full.initialize(capacity));deposit.item.id="overflow";deposit.request=6;deposit.revision=1;
    assert(full.begin(22,deposit).kind==Reject&&full.items.size()==kMaxItems);
    auto duplicate=capacity;duplicate.back().id=duplicate.front().id;assert(!full.initialize(duplicate));
    a.reserved=true;a.pending={{11,42},deposit};
    auto cancel=deposit;cancel.request=42;
    assert(a.finish(22,cancel,false).kind==Reject&&a.reserved); // only owner may cancel
    assert(a.finish(11,cancel,false).kind==Reject&&!a.reserved);
    Message m=deposit;m.kind=Snapshot;m.items.push_back({"stateful-tool",502,std::vector<uint8_t>(kMaxItemBytes,42)});
    std::vector<uint8_t> b;assert(encode(m,&b));Message decoded;assert(decode(b,&decoded)&&decoded.items[0].data==m.items[0].data);
    auto bad=b;bad.push_back(0);assert(!decode(bad,&decoded));bad=b;bad[2]=2;assert(!decode(bad,&decoded));
    for(size_t n=0;n<b.size();n+=137){bad.assign(b.begin(),b.begin()+n);assert(!decode(bad,&decoded));}
    Assembly rx;std::vector<uint8_t> frame,result;size_t i=0;
    while(fragment(b,123, i++,300,&frame))assert(rx.append(frame.data(),frame.size(),300,&result));
    assert(result==b);
    assert(fragment(b,125,1,300,&frame));assert(!rx.append(frame.data(),frame.size(),300,&result));
    assert(fragment(b,125,0,300,&frame));frame.back()^=1;assert(rx.append(frame.data(),frame.size(),300,&result));
    i=1;result.clear();while(fragment(b,125,i++,300,&frame))rx.append(frame.data(),frame.size(),300,&result);assert(result.empty());
}
