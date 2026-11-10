// One shared GUID, opt-in on BOTH devices, 60 seconds. No spawning or save writes.
constexpr const char* kMotionProbeRequest="/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_log/creature_motion.request";
struct CreatureMotionProbe {
    bool armed=false,host=false,selected=false,streaming=false,ending=false;
    bool gap_profile=false,gap_active=false,gap_done=false,stale_notice=false,last_written_valid=false;
    uint64_t nonce=0;
    creature_motion::Id session{};
    uint32_t epoch=0,authority=0,peer=0,sequence=0,received=0,applied=0,reenabled=0;
    double armed_at=0,started=0,next_send=0,next_report=0,next_select=0,end_until=0;
    float maximum_error=0;
    float maximum_frame_drift=0,maximum_stale_drift=0;
    uint32_t stale_frames=0;
    double next_hint=0,maximum_age=0;
    Vector3 last_written{};
    creature_motion::Message request;
    creature_motion::Receiver receiver;
} g_creature_motion_probe;

void motion_notify(const char* text) {
    LOG_INFO("motion probe: notice=%s",text);
    if(g_contents_api.notify){void* s=il2cpp_string_new(text);g_contents_api.notify.call(s,g_contents_api.notify.method);}
}
void motion_hint(const LocalState& local) {
    auto& m=g_creature_motion_probe;if(build_now()<m.next_hint)return;m.next_hint=build_now()+3;
    auto& p=g_creature_probe;Vector3 pos=g_get_position.call(g_get_transform.call(p.fish,g_get_transform.method),g_get_position.method);
    float dx=pos.x-local.x,dy=pos.y-local.y,dz=pos.z-local.z;
    const auto direction=creature_motion::bearing(dx,dz,local.view_yaw);
    const char* side=direction==creature_motion::Bearing::Ahead?"впереди":direction==creature_motion::Bearing::Right?"справа":direction==creature_motion::Bearing::Behind?"сзади":"слева";
    const char* height=dy>2?", выше":dy<-2?", ниже":"";
    char text[256];snprintf(text,sizeof(text),"SNMP: тестовая рыба — %.0f м, %s%s",sqrtf(dx*dx+dy*dy+dz*dz),side,height);
    motion_notify(text);
}

bool motion_identity(void* fish,creature_motion::Id* id,creature_motion::Id* prefab) {
    std::string text;if(!contents_id(fish,&text)||!g_unique_get_class_id)return false;
    void* uid=build_uid(fish);if(!uid)return false;
    guid_from_string(text.c_str(),id->data());
    return build_class_bytes(g_unique_get_class_id.call(uid,g_unique_get_class_id.method),prefab->data())&&
        creature_motion::nonzero(*id)&&creature_motion::nonzero(*prefab);
}
void* motion_find_exact(const creature_motion::Message& request) {
    auto& p=g_creature_probe;void* list=g_find_objects_all.call(p.fish_type,false,g_find_objects_all.method);
    if(!list)return nullptr;
    void* selected=nullptr;int matches=0;
    int count=g_array_get_length.call(list,g_array_get_length.method);
    for(int i=0;i<count&&i<4096;++i){
        void* fish=g_array_get_value.call(list,i,g_array_get_value.method);if(!probe_exists(fish))continue;
        creature_motion::Id id{},prefab{};if(!motion_identity(fish,&id,&prefab)||id!=request.fish)continue;
        ++matches;if(prefab!=request.prefab)continue;
        void* go=g_get_game_object.call(fish,g_get_game_object.method);if(!go||object_in_local_inventory(go))continue;
        void* live=g_get_component.call(go,p.live_type,g_get_component.method);
        if(!probe_exists(live)||!p.alive.call(live,p.alive.method))continue;
        void* park=g_get_component.call(go,p.waterpark_type,g_get_component.method);void* owner=nullptr;
        if(park)il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(park),p.waterpark_field,&owner);
        if(!owner)selected=fish;
    }
    return matches==1?selected:nullptr;
}
creature_motion::Message motion_reply(creature_motion::Kind kind) {
    auto& m=g_creature_motion_probe;auto out=m.request;
    out.kind=kind;out.sender=net_local_id();out.target=m.peer;out.sequence=m.sequence;
    return out;
}
void motion_finish(const char* reason) {
    auto& m=g_creature_motion_probe;
    LOG_INFO("motion probe: finished role=%s id=%s reason=%s rx=%u applied=%u reenabled=%u max_apply_error=%.5f max_frame_drift=%.5f max_stale_drift=%.5f stale_frames=%u max_age=%.3f",
        m.host?"host":"guest",g_creature_probe.id,reason,m.received,m.applied,m.reenabled,m.maximum_error,m.maximum_frame_drift,m.maximum_stale_drift,m.stale_frames,m.maximum_age);
    if(m.streaming||m.ending)motion_notify("SNMP: проверка рыбы завершена");
    probe_finish(reason);m.armed=false;m.selected=false;m.streaming=false;m.ending=false;
}
bool motion_current_context() {
    auto& m=g_creature_motion_probe;creature_motion::Id session{};uint32_t epoch=0;
    return net_geometry_session(session.data(),&epoch)&&session==m.session&&epoch==m.epoch&&
        net_session_host_id()==m.authority&&g_local_is_host==m.host;
}
bool motion_read_arm() {
    static double next=0;double now=build_now();if(now<next)return false;next=now+1;
    FILE* file=fopen(kMotionProbeRequest,"rb");if(!file)return false;
    char command[80]{};size_t n=fread(command,1,sizeof(command)-1,file);fclose(file);unlink(kMotionProbeRequest);
    constexpr char prefix[]="motion-boomerang-60s ";
    constexpr char gap_prefix[]="motion-boomerang-gap ";
    static_assert(sizeof(prefix)==sizeof(gap_prefix));
    bool gap=n==sizeof(gap_prefix)-1+16&&!memcmp(command,gap_prefix,sizeof(gap_prefix)-1);
    if(n!=sizeof(prefix)-1+16||(!gap&&memcmp(command,prefix,sizeof(prefix)-1)))return false;
    uint64_t nonce=0;
    for(size_t i=sizeof(prefix)-1;i<n;++i){char c=command[i];unsigned digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;if(digit>15)return false;nonce=(nonce<<4)|digit;}
    if(!nonce||g_creature_probe.active)return false;
    g_creature_probe.id[0]=0;
    auto& m=g_creature_motion_probe;m=CreatureMotionProbe{};
    if(!net_geometry_session(m.session.data(),&m.epoch))return false;
    m.authority=net_session_host_id();if(!m.authority)return false;
    m.armed=true;m.host=g_local_is_host;m.nonce=nonce;m.armed_at=now;
    m.gap_profile=gap;
    LOG_INFO("motion probe: armed role=%s nonce=%016llx authority=%08x epoch=%u; exact GUID only; finite 60s",
        m.host?"host":"guest",static_cast<unsigned long long>(nonce),m.authority,m.epoch);
    LOG_INFO("motion probe: profile=%s",gap?"5s-pose-gap":"continuous");
    return true;
}
void motion_freeze_guest() {
    auto& p=g_creature_probe;
    p.freeze_position=g_get_position.call(g_get_transform.call(p.fish,g_get_transform.method),g_get_position.method);
    p.allow.call(p.fish,false,p.allow.method);p.frozen=true;
    for(int i=0;i<p.count;++i)if(probe_exists(p.saved[i].component))g_behaviour_set_enabled.call(p.saved[i].component,false,g_behaviour_set_enabled.method);
    g_rb_set_kinematic.call(p.body,true,g_rb_set_kinematic.method);
    for(int i=0;i<p.count;++i)probe_indices(p.saved[i].component,"network-frozen");
    LOG_INFO("motion probe: guest bound id=%s native_AI_suppressed=1 components=%d",p.id,p.count);
}
void motion_host_end(const char* reason) {
    auto& m=g_creature_motion_probe;
    LOG_INFO("motion probe: host stream ended reason=%s seq=%u",reason,m.sequence);
    probe_finish(reason);m.streaming=false;m.ending=true;m.end_until=build_now()+5;m.next_send=0;
}
bool tick_creature_motion_probe(const LocalState& local) try {
    namespace cm=creature_motion;auto& m=g_creature_motion_probe;auto& p=g_creature_probe;double now=build_now();
    if(m.armed&&(!local.valid||!g_role_known||!contents_world_ready()||!world_operations_ready()||!motion_current_context())){
        motion_finish("world/role/context changed");return false;
    }
    if(!local.valid||!g_role_known||!contents_world_ready()||!world_operations_ready()||!probe_resolve())return false;
    static bool ready_logged=false;if(!ready_logged){ready_logged=true;LOG_INFO("motion probe: ready protocol=29; explicit paired arm required");}
    if(!m.armed)motion_read_arm();
    cm::Message incoming;
    for(int n=0;n<16&&net_creature_probe_take(&incoming);++n){
        if(!m.armed||incoming.nonce!=m.nonce||incoming.epoch!=m.epoch||incoming.session!=m.session||incoming.host!=m.authority)continue;
        if(m.host){
            if(incoming.kind!=cm::Request||m.ending)continue;
            if(m.selected){if(!cm::same_run(m.request,incoming)||incoming.sender!=m.peer)continue;}
            else {
                // Only an exact active wild Boomerang is eligible. Never choose a nearby substitute.
                void* fish=motion_find_exact(incoming);m.request=incoming;m.peer=incoming.sender;
                if(!fish||!probe_bind(fish,0)){
                    auto refused=motion_reply(cm::Refuse);net_creature_probe_send(refused);
                    LOG_WARN("motion probe: exact GUID/class missing or unavailable; refusing without spawn");
                    motion_finish("exact host fish unavailable");break;
                }
                m.selected=true;m.streaming=true;m.started=now;
                LOG_INFO("motion probe: host matched exact id=%s prefab=%02x%02x peer=%08x; native AI retained",p.id,incoming.prefab[0],incoming.prefab[1],m.peer);
            }
        }else {
            if(!m.selected||incoming.sender!=m.authority||!cm::same_run(m.request,incoming))continue;
            if(incoming.kind==cm::Refuse){motion_finish("host refused exact identity");break;}
            if(incoming.kind==cm::End){motion_finish("explicit host end");break;}
            if(incoming.kind!=cm::Pose||!m.receiver.accept(incoming,now))continue;
            if(m.stale_notice){m.stale_notice=false;LOG_INFO("motion probe: guest resumed seq=%u after_age=%.3f; AI stays suppressed",incoming.sequence,m.maximum_age);motion_notify("SNMP: движение рыбы снова приходит");}
            if(!m.streaming){motion_freeze_guest();m.streaming=true;m.started=now;}
            ++m.received;
            LOG_INFO("motion probe: rx seq=%u id=%s pos=%.3f,%.3f,%.3f host_ms=%u",incoming.sequence,p.id,incoming.position[0],incoming.position[1],incoming.position[2],incoming.elapsed_ms);
        }
    }
    if(!m.armed)return false;
    if(m.ending){
        if(now>=m.end_until){motion_finish("host end repeated for 5s");return false;}
        if(now>=m.next_send){m.next_send=now+.2;auto end=motion_reply(cm::End);end.elapsed_ms=60000;net_creature_probe_send(end);}
        return true;
    }
    if(now-m.armed_at>120&&!m.selected){motion_finish("no fish/request before arm expiry");return false;}
    if(!m.host&&!m.selected&&now>=m.next_select){
        m.next_select=now+1;
        if(probe_select(local)){
            m.request=cm::Message{};m.request.host=m.authority;m.request.sender=net_local_id();m.request.target=m.authority;
            m.request.nonce=m.nonce;m.request.session=m.session;m.request.epoch=m.epoch;
            if(!motion_identity(p.fish,&m.request.fish,&m.request.prefab)){motion_finish("guest identity unavailable");return false;}
            m.receiver.reset(m.request);m.peer=m.authority;m.selected=true;m.next_send=0;m.started=now;
            LOG_INFO("motion probe: guest selected exact id=%s; requesting host match before suppression",p.id);
        }
    }
    if(!m.selected)return true;
    if(!probe_exists(p.fish)||!probe_exists(p.body)){
        if(m.host)motion_host_end("host fish streamed out/destroyed");else motion_finish("guest fish streamed out/destroyed");return m.armed;
    }
    if(m.streaming)motion_hint(local);
    if(m.host){
        if(now-m.started>=cm::kRunSeconds){motion_host_end("60s complete");return true;}
        if(cm::controlled_gap(m.gap_profile,now-m.started)){
            if(!m.gap_active){m.gap_active=true;LOG_INFO("motion probe: host controlled gap start elapsed=%.3f seq=%u",now-m.started,m.sequence);motion_notify("SNMP: проверяем паузу движения на 5 секунд");}
            return true;
        }
        if(m.gap_active&&!m.gap_done){m.gap_active=false;m.gap_done=true;LOG_INFO("motion probe: host controlled gap end elapsed=%.3f seq=%u",now-m.started,m.sequence);motion_notify("SNMP: передача движения возобновлена");}
        if(now>=m.next_send){
            m.next_send=now+.1;auto pose=motion_reply(cm::Pose);pose.sequence=++m.sequence;
            pose.elapsed_ms=uint32_t((now-m.started)*1000);
            void* tr=g_get_transform.call(p.fish,g_get_transform.method);Vector3 pos=g_get_position.call(tr,g_get_position.method);Quaternion rot=g_get_rotation.call(tr,g_get_rotation.method);
            pose.position={pos.x,pos.y,pos.z};pose.rotation={rot.x,rot.y,rot.z,rot.w};
            if(net_creature_probe_send(pose))LOG_INFO("motion probe: tx seq=%u id=%s pos=%.3f,%.3f,%.3f host_ms=%u",pose.sequence,p.id,pos.x,pos.y,pos.z,pose.elapsed_ms);
        }
    }else if(!m.streaming){
        if(now-m.started>=20){motion_finish("host match timed out; native AI never suppressed");return false;}
        if(now>=m.next_send){m.next_send=now+.5;net_creature_probe_send(m.request);}
    }else {
        if(now-m.started>=cm::kGuestDeadline){motion_finish("finite local test deadline; not a packet-silence fallback");return false;}
        // Packet silence keeps the selected fish suppressed and holds its last host pose.
        for(int i=0;i<p.count;++i)if(probe_exists(p.saved[i].component)&&g_behaviour_get_enabled.call(p.saved[i].component,g_behaviour_get_enabled.method)){
            ++m.reenabled;g_behaviour_set_enabled.call(p.saved[i].component,false,g_behaviour_set_enabled.method);
        }
        if(!g_rb_get_kinematic.call(p.body,g_rb_get_kinematic.method)){++m.reenabled;g_rb_set_kinematic.call(p.body,true,g_rb_set_kinematic.method);}
        cm::Message pose;
        if(m.receiver.sample(now,&pose)){
            void* tr=g_get_transform.call(p.fish,g_get_transform.method);Vector3 pos{pose.position[0],pose.position[1],pose.position[2]};Quaternion rot{pose.rotation[0],pose.rotation[1],pose.rotation[2],pose.rotation[3]};
            const double age=m.receiver.age(now);m.maximum_age=std::max(m.maximum_age,age);
            float frame_drift=0;
            if(m.last_written_valid){Vector3 before=g_get_position.call(tr,g_get_position.method);frame_drift=sqrtf((before.x-m.last_written.x)*(before.x-m.last_written.x)+(before.y-m.last_written.y)*(before.y-m.last_written.y)+(before.z-m.last_written.z)*(before.z-m.last_written.z));m.maximum_frame_drift=std::max(m.maximum_frame_drift,frame_drift);}
            if(age>.5){++m.stale_frames;m.maximum_stale_drift=std::max(m.maximum_stale_drift,frame_drift);
                if(!m.stale_notice){m.stale_notice=true;LOG_INFO("motion probe: guest holding stale pose seq=%u age=%.3f; AI stays suppressed",pose.sequence,age);motion_notify("SNMP: пауза передачи — рыба ждёт хоста");}}
            g_build_set_position.call(tr,pos,g_build_set_position.method);g_set_rotation.call(tr,rot,g_set_rotation.method);
            Vector3 read=g_get_position.call(tr,g_get_position.method);float error=sqrtf((read.x-pos.x)*(read.x-pos.x)+(read.y-pos.y)*(read.y-pos.y)+(read.z-pos.z)*(read.z-pos.z));
            m.last_written=read;m.last_written_valid=true;
            m.maximum_error=std::max(m.maximum_error,error);++m.applied;
            if(now>=m.next_report){m.next_report=now+1;LOG_INFO("motion probe: apply seq=%u id=%s pos=%.3f,%.3f,%.3f age=%.3f stale=%u enabled_corrections=%u error=%.5f frame_drift=%.5f max_stale_drift=%.5f",pose.sequence,p.id,read.x,read.y,read.z,age,unsigned(age>.5),m.reenabled,error,frame_drift,m.maximum_stale_drift);}
        }
    }
    return true;
} catch(...){motion_finish("native exception");return false;}
