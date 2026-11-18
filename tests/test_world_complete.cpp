#define SNMOD_WORLD_TEST_ROOT "/data/local/tmp/snmod_world_complete_tests"
#include "modloader/src/world.cpp"
#include <cassert>
#include <fstream>
void mod_logf(const char*,const char*,...) {}
void put(const std::string& path,const std::string& bytes){std::ofstream f(path,std::ios::binary);f.write(bytes.data(),bytes.size());f.close();assert(f.good());}
std::string get(const std::string& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
std::string root=SNMOD_WORLD_TEST_ROOT;
void native_slot(const char* slot){
    std::string p=root+"/"+slot;assert(mkdir(p.c_str(),0770)==0);assert(mkdir((p+"/CellsCache").c_str(),0770)==0);
    put(p+"/gameinfo.json","{\"session\":\"af261965-e826-4b78-bc75-bf84c7083245\",\"gameMode\":3,\"changeSet\":83315}");
    put(p+"/global-objects.bin","identifiers");put(p+"/scene-objects.bin",std::string(4207,'s'));put(p+"/screenshot.jpg","picture");
    put(p+"/CellsCache/baked-batch-cells-12-grp0.zip",std::string(2067,'a'));
    put(p+"/CellsCache/baked-batch-cells-11-grp0.zip",std::string(1031,'b'));
}
WorldManifest manifest(const WorldPayload& p,const char* slot){
    WorldManifest m{};snprintf(m.slot,sizeof(m.slot),"%s",slot);m.file_count=kWorldFileCount;m.total_bytes=uint32_t(p.total_bytes);
    for(int i=0;i<kWorldFileCount;++i){snprintf(m.files[i].name,sizeof(m.files[i].name),"%s",p.files[i].name);m.files[i].size=p.files[i].size;m.files[i].checksum=p.files[i].checksum;}
    return m;
}
void receive(const WorldPayload& p,const WorldManifest& m,bool corrupt=false){
    assert(world_begin_receive(m));
    for(int i=kWorldFileCount-1;i>=0;--i){
        const auto& f=p.files[i];
        for(int chunk=int(chunks_for(f.size))-1;chunk>=0;--chunk){uint32_t at=uint32_t(chunk)*kWorldChunkPayload;size_t n=std::min<size_t>(kWorldChunkPayload,f.size-at);
            assert(world_receive_chunk(i,at,f.data+at,n));assert(world_receive_chunk(i,at,f.data+at,n));}
    }
    if(corrupt)g_recv[4][g_recv_size[4]-1]^=1;
}

guest_progress::View as_view(std::string_view value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
void text_field(gp::Bytes& bytes, unsigned tag, gp::View value) {
    gp::put_varint(bytes, (tag << 3) | 2);
    gp::put_blob(bytes, value);
}
std::string personal_scene(uint8_t item, uint8_t blueprint, uint8_t location, const char* world) {
    gp::Bytes game_object, tree, envelope, entry, outer, file;
    text_field(game_object, 4, as_view("Player"));
    gp::put_blob(tree, gp::view(game_object));
    const uint8_t count[]{8, 2};
    gp::put_blob(tree, {count, sizeof(count)});
    for (const char* name : {"Player", "Inventory"}) {
        gp::Bytes header;
        text_field(header, 1, as_view(name));
        gp::put_blob(tree, gp::view(header));
        if (strcmp(name, "Player") == 0) {
            const uint8_t player[]{16, location, 40, blueprint};
            gp::put_blob(tree, {player, sizeof(player)});
        } else {
            const uint8_t inventory[]{8, 6, 18, 1, item};
            gp::put_blob(tree, {inventory, sizeof(inventory)});
        }
    }
    text_field(envelope, 3, gp::view(tree));
    text_field(entry, 1, as_view("Player"));
    text_field(entry, 2, gp::view(envelope));
    outer = {8, 1};
    text_field(outer, 2, gp::view(entry));
    text_field(outer, 17, as_view(world)); // Unrelated scene data survives.
    gp::put_blob(file, gp::view(outer));
    return std::string(reinterpret_cast<const char*>(file.data()), file.size());
}

void test_guest_refresh() {
    native_slot("profile_source");
    const std::string host = personal_scene(1, 2, 1, "host wreck is open");
    put(root + "/profile_source/scene-objects.bin", host);
    WorldPayload payload{};
    assert(world_disk_capture("profile_source", &payload));
    auto session = manifest(payload, "profile_guest");
    receive(payload, session);
    assert(world_write_slot(session));
    assert(!world_guest_profile_applied("profile_guest"));
    WorldGuestSaveStamp before;
    assert(!world_guest_save_stamp("profile_source", payload.content_guid, &before));
    assert(world_guest_save_stamp("profile_guest", payload.content_guid, &before));
    assert(!world_guest_checkpoint("profile_guest", before)); // no completed save
    const std::string guest_path = root + "/profile_guest/";
    put(guest_path + "gameinfo.json", "{\"session\":\"\",\"dateTicks\":1,\"changeSet\":83315}");
    assert(!world_guest_checkpoint("profile_guest", before)); // metadata alone is not a new save
    put(guest_path + "scene-objects.bin", personal_scene(3, 4, 0, "guest wreck is closed"));
    put(guest_path + "gameinfo.json", "{\"session\":\"0f261965-e826-4b78-bc75-bf84c7083245\"}");
    assert(!world_guest_checkpoint("profile_guest", before)); // another world cannot replace the profile
    put(guest_path + "gameinfo.json", "{\"session\":\"\",\"dateTicks\":2,\"changeSet\":83315}");
    assert(world_guest_checkpoint("profile_guest", before));
    assert(world_guest_save_stamp("profile_guest", payload.content_guid, &before));
    put(guest_path + "scene-objects.bin", personal_scene(6, 7, 0, "guest reloads the wreck"));
    put(guest_path + "gameinfo.json", "{\"session\":\"\",\"dateTicks\":30,\"changeSet\":83315}");
    assert(world_guest_checkpoint("profile_guest", before));
    receive(payload, session);
    assert(world_guest_receive_has_profile() && world_write_slot(session));
    const std::string combined = get(guest_path + "scene-objects.bin");
    assert(combined == personal_scene(6, 7, 1, "host wreck is open"));
    assert(world_guest_profile_applied("profile_guest"));
    assert(get(root + "/profile_source/scene-objects.bin") == host);

    char profile_path[560];
    guest_profile_path(before.world, profile_path);
    const std::string profile = get(profile_path);
    put(profile_path, "torn profile");
    receive(payload, session);
    assert(!world_write_slot(session));
    assert(get(guest_path + "scene-objects.bin") == combined);
    put(profile_path, profile);
    world_payload_free(&payload);
    world_reset_receive();

    put(root + "/profile_source/gameinfo.json",
        "{\"session\":\"0f261965-e826-4b78-bc75-bf84c7083245\"}");
    assert(world_disk_capture("profile_source", &payload));
    session = manifest(payload, "another_world");
    receive(payload, session);
    assert(!world_guest_receive_has_profile() && world_write_slot(session));
    assert(get(root + "/another_world/scene-objects.bin") == host);
    world_payload_free(&payload);
    puts("PASS: guest inventory and personal progress survive host refresh; corrupt and unrelated profiles are isolated");
}

void test_native_samples(const char* host_path, const char* guest_path) {
    const std::string host = get(host_path), guest = get(guest_path);
    gp::Bytes expected, actual, updated;
    assert(gp::extract(as_view(guest), expected));
    assert(gp::overlay(as_view(host), gp::view(expected), updated));
    assert(gp::extract(gp::view(updated), actual) && actual == expected);
    gp::Components host_parts, updated_parts;
    assert(gp::locate(as_view(host), host_parts) && gp::locate(gp::view(updated), updated_parts));
    gp::Bytes host_world, updated_world;
    assert(gp::filter_player(host_parts.player, false, host_world));
    assert(gp::filter_player(updated_parts.player, false, updated_world));
    assert(host_world == updated_world);
    puts("PASS: native host and guest save samples preserve host world-facing player fields");
}

int main(int argc, char** argv){
    if (argc == 3) {
        test_native_samples(argv[1], argv[2]);
        return 0;
    }
    assert(root=="/data/local/tmp/snmod_world_complete_tests");remove_tree_best_effort(root.c_str());assert(mkdir(root.c_str(),0770)==0);
    native_slot("source");native_slot("local");native_slot("identical");
    WorldPayload p{};assert(world_disk_ready("source")&&world_disk_capture("source",&p));
    assert(p.valid&&p.file_count==5&&p.content_guid_valid);std::vector<WorldCellEntry> entries;
    assert(world_parse_cells(p.files[4].data,p.files[4].size,&entries)&&entries.size()==2);
    assert(entries[0].name=="baked-batch-cells-11-grp0.zip");
    auto m=manifest(p,"guest");receive(p,m,true);assert(!world_receive_verify(m));
    receive(p,m);assert(world_receive_verify(m));
    auto wrong=m;wrong.files[0].checksum++;assert(!world_write_slot(wrong));
    assert(world_write_slot(m)&&world_slot_is_host_received("guest"));
    struct stat copied_access{};
    assert(!stat((root+"/guest/CellsCache/"+entries[0].name).c_str(),&copied_access));
    assert((copied_access.st_gid==save_backup_access::kExternalDataGroup && (copied_access.st_mode&S_IRGRP)) ||
           (copied_access.st_mode&S_IROTH)); // Received cells remain readable for the next Shizuku backup.
    assert(world_slot_exists("guest",m)==kWorldSlotMatches);
    assert(get(root+"/guest/CellsCache/"+entries[0].name)==get(root+"/source/CellsCache/"+entries[0].name));
    assert(access((root+"/guest/snmod-cells.bin").c_str(),F_OK)!=0); // archive is not a game save file
    // Role swap: identical bytes do not imply that a native save is ready for
    // guest autoload. Explicit adoption must retain the native directory and
    // commit the host-received marker even when every manifest file matches.
    auto identical=manifest(p,"identical");
    assert(world_slot_exists("identical",identical)==kWorldSlotMatches);
    assert(!world_slot_is_host_received("identical"));
    receive(p,identical);assert(!world_write_slot(identical));
    receive(p,identical);assert(world_write_slot(identical,true));
    assert(world_slot_is_host_received("identical"));
    assert(world_slot_exists("identical",identical)==kWorldSlotMatches);
    assert(get(root+"/.snmod_backup_identical/CellsCache/"+entries[0].name)==get(root+"/source/CellsCache/"+entries[0].name));
    WorldLoadFields fields{};assert(world_read_load_fields("guest",&fields)&&fields.game_mode==3);
    // Change only cells: old four native files stay byte-identical. Must refresh.
    put(root+"/source/CellsCache/baked-batch-cells-12-grp0.zip","new built room and dome");
    assert(unlink((root+"/source/CellsCache/baked-batch-cells-11-grp0.zip").c_str())==0);
    WorldPayload fresh{};assert(world_disk_capture("source",&fresh));auto newer=manifest(fresh,"guest");
    assert(world_slot_exists("guest",newer)==kWorldSlotConflict);
    receive(fresh,newer);assert(world_write_slot(newer));
    assert(get(root+"/guest/CellsCache/baked-batch-cells-12-grp0.zip")=="new built room and dome");
    assert(access((root+"/guest/CellsCache/baked-batch-cells-11-grp0.zip").c_str(),F_OK)!=0);
    assert(get(root+"/.snmod_backup_guest/CellsCache/baked-batch-cells-11-grp0.zip")==std::string(1031,'b'));
    assert(world_slot_exists("guest",newer)==kWorldSlotMatches);
    // Never overwrite a native local save just because the host uses that slot name.
    auto local=manifest(fresh,"local");receive(fresh,local);assert(!world_write_slot(local));
    assert(get(root+"/local/CellsCache/baked-batch-cells-12-grp0.zip")==std::string(2067,'a'));
    receive(fresh,local);assert(world_write_slot(local,true)); // explicit guest: retain former host's native save
    assert(get(root+"/.snmod_backup_local/CellsCache/baked-batch-cells-12-grp0.zip")==std::string(2067,'a'));
    assert(world_slot_is_host_received("local"));
    put(root+"/source/screenshot.jpg","another host save");
    WorldPayload latest{};assert(world_disk_capture("source",&latest));auto again=manifest(latest,"local");
    receive(latest,again);assert(world_write_slot(again)); // later ordinary refresh must not erase the native original
    bool native_kept=false;DIR* backups=opendir(root.c_str());assert(backups);
    while(auto* entry=readdir(backups)) if(!strncmp(entry->d_name,".snmod_kept_local_",18)) {
        native_kept=get(root+"/"+entry->d_name+"/CellsCache/baked-batch-cells-12-grp0.zip")==std::string(2067,'a');
    }
    closedir(backups);assert(native_kept);world_payload_free(&latest);
    // Checksums alone cannot authorize a malicious archive path.
    std::vector<uint8_t> malformed(p.files[4].data,p.files[4].data+p.files[4].size);
    malformed[20]='/';assert(!world_parse_cells(malformed.data(),malformed.size(),&entries));
    WorldPayload bad=fresh;bad.files[4].data=malformed.data();bad.files[4].size=uint32_t(malformed.size());bad.files[4].checksum=world_checksum(malformed.data(),malformed.size());
    auto evil=manifest(bad,"unsafe");receive(bad,evil);assert(!world_write_slot(evil));
    assert(access((root+"/unsafe").c_str(),F_OK)!=0);assert(access((root+"/.snmod_stage_unsafe").c_str(),F_OK)!=0);
    assert(!world_parse_cells(p.files[4].data,p.files[4].size-1,&entries));
    auto huge=newer;huge.files[4].size=uint32_t(kMaxCellArchiveBytes+1);assert(!world_begin_receive(huge));
    auto duplicate=newer;strcpy(duplicate.files[4].name,duplicate.files[0].name);assert(!world_begin_receive(duplicate));
    // Disk capture refuses symlink entries and never exports host marker/backups.
    assert(symlink("/etc/hosts",(root+"/source/CellsCache/baked-batch-cells-20.bin").c_str())==0);
    WorldPayload refused{};assert(!world_disk_capture("source",&refused)&&!refused.valid);
    assert(unlink((root+"/source/CellsCache/baked-batch-cells-20.bin").c_str())==0);
    world_payload_free(&p);world_payload_free(&fresh);world_reset_receive();
    test_guest_refresh();
    assert(remove_tree_best_effort(root.c_str()));
    puts("actual full world transfer: native files+saved cells, unordered/duplicate chunks, corruption rejection, first install, cell-only refresh/deletions, prior backup, local-save protection, archive path/size/duplicate/symlink guards, cleanup and exact load metadata passed");
}
