#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define LOG_INFO(...) ((void)0)
struct BuildGridPoint {int32_t x,y,z;};
using ManagedFn=void(*)(void*,void*);
struct Method {void* code;};
template<class Fn> struct Entry {
    Fn call=nullptr; void* method=nullptr;
    void bind(Method* m){method=m;call=m?reinterpret_cast<Fn>(m->code):nullptr;}
    explicit operator bool()const{return call!=nullptr;}
};
int base_class,handle_class,token;
void noop(void*,void*){}
uint8_t empty(void* base,void* ignore,void* method){
    assert(base==&token && ignore==nullptr && method!=nullptr);return 1;
}
Method standard{reinterpret_cast<void*>(&noop)},is_empty{reinterpret_cast<void*>(&empty)};
bool dependencies=true,missing_empty=false;
bool build_resolve_corridor_api(){return dependencies;}
auto g_object_destroy=&noop;
struct {void* base=&base_class;} g_corridor_api;
void* build_class(const char* name){return !strcmp(name,"BaseDeconstructable")?&handle_class:nullptr;}
const void* il2cpp_class_get_type(void* c){return c;}
void* il2cpp_type_get_object(const void* t){return const_cast<void*>(t);}
void* build_field_on(void* c,const char*,int){return c;}
Method* find_method(void* cls,const char* name,int count){
    if(cls==&handle_class) return !strcmp(name,"DeconstructionAllowed")&&count==1?&standard:nullptr;
    assert(cls==&base_class);
    if(!strcmp(name,"IsEmpty")) return count==1&&!missing_empty?&is_empty:nullptr;
    if(count!=0) return nullptr;
    for(const char* n:{"FixRoomFloors","FixCorridorLinks","RebuildGeometry","OnPreDestroy"}) if(!strcmp(name,n)) return &standard;
    return nullptr;
}
Method* build_typed_method(void* cls,const char* name,int count,const char* type){
    if(cls==&base_class && !strcmp(name,"ClearCell") && count==1 && !strcmp(type,"Int3")) return &standard;
    if(!strcmp(name,"IsEmpty") && !strcmp(type,"BaseGhost")) return find_method(cls,name,count);
    return nullptr;
}
#include "modloader/src/base_corridor_removal_api.inc"
int main(){
    assert(build_resolve_corridor_removal());
    assert(g_corridor_removal_api.is_empty.call(&token,nullptr,g_corridor_removal_api.is_empty.method)==1);
    g_corridor_removal_api={};missing_empty=true;
    assert(!build_resolve_corridor_removal());
    g_corridor_removal_api={};missing_empty=false;dependencies=false;
    assert(!build_resolve_corridor_removal());
    puts("actual removal resolver: Base.IsEmpty(BaseGhost), null ignore argument, missing-method/dependency refusal passed");
}
