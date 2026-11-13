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
    Authority reactor;
    assert(reactor.initialize({{"fuel-a",101,{}},{"fuel-b",102,{}},{"fuel-c",103,{}}}));
    Message fuel_take=take;
    fuel_take.locker="reactor-1";
    fuel_take.item={"fuel-a",101,{}};
    assert(reactor.begin(11,fuel_take).kind==Grant);
    std::vector<Message> burned;
    assert(!reactor.record_consumption(fuel_take.locker,{},&burned)&&reactor.revision==1);
    assert(reactor.finish(11,fuel_take,false).kind==Reject);
    assert(!reactor.record_consumption(fuel_take.locker,{{"fuel-c",104,{}}},&burned));
    assert(!reactor.record_consumption(fuel_take.locker,{{"new-fuel",103,{}}},&burned));
    assert(reactor.items.size()==3&&reactor.revision==1);
    assert(reactor.record_consumption(fuel_take.locker,{{"fuel-c",103,{}}},&burned));
    assert(burned.size()==2&&reactor.items.size()==1&&reactor.revision==3);
    assert(burned[0].kind==Done&&!burned[0].deposit&&burned[0].request==0&&
           burned[0].item.id=="fuel-a"&&burned[0].revision==2);
    assert(burned[1].item.id=="fuel-b"&&burned[1].revision==3);
    std::vector<uint8_t> burn_wire;
    Message burn_copy;
    assert(encode(burned[0],&burn_wire)&&decode(burn_wire,&burn_copy));
    assert(burn_copy.item.id=="fuel-a"&&burn_copy.locker=="reactor-1"&&burn_copy.revision==2);
    assert(reactor.record_consumption(fuel_take.locker,{{"fuel-c",103,{}}},&burned)&&burned.empty());
    fuel_take.request=2;fuel_take.revision=3;
    assert(reactor.begin(11,fuel_take).kind==Reject);
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
