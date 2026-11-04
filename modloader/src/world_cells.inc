// Opaque, already compressed game cell files. Never unzip remote ZIP contents.
// This private archive is deterministic and contains flat native cache names only.
constexpr size_t kMaxCellArchiveBytes=64u*1024u*1024u;
constexpr size_t kMaxCellEntries=4096;
struct WorldDiskStamp {
    std::string name;
    uint64_t size,device,inode;
    int64_t msec,mnsec,csec,cnsec;
    bool operator==(const WorldDiskStamp& b) const {
        return name==b.name && size==b.size && device==b.device && inode==b.inode &&
            msec==b.msec && mnsec==b.mnsec && csec==b.csec && cnsec==b.cnsec;
    }
};
bool world_cell_name(const std::string& name) {
    if(name.size()<23 || name.size()>120 || name.compare(0,18,"baked-batch-cells-")!=0 ||
       (name.compare(name.size()-4,4,".zip") && name.compare(name.size()-4,4,".bin"))) return false;
    for(char c:name) if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')) return false;
    return name.find("..") == std::string::npos;
}
WorldDiskStamp world_stamp(const std::string& name,const struct stat& st) {
    return {name,uint64_t(st.st_size),uint64_t(st.st_dev),uint64_t(st.st_ino),
        st.st_mtim.tv_sec,st.st_mtim.tv_nsec,st.st_ctim.tv_sec,st.st_ctim.tv_nsec};
}
bool world_list_cells(const char* slot,std::vector<WorldDiskStamp>* entries) {
    entries->clear();char path[600];snprintf(path,sizeof(path),"%s/%s/CellsCache",kSavesRoot,slot);
    struct stat st{};
    if(lstat(path,&st)!=0) return errno==ENOENT;
    if(!S_ISDIR(st.st_mode)) return false;
    DIR* dir=opendir(path);if(!dir) return false;
    bool ok=true;size_t bytes=8;
    while(auto* e=readdir(dir)) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        std::string name=e->d_name;
        if(!world_cell_name(name)||entries->size()>=kMaxCellEntries){ok=false;break;}
        char file[760];snprintf(file,sizeof(file),"%s/%s",path,name.c_str());
        if(lstat(file,&st)!=0||!S_ISREG(st.st_mode)||st.st_size<=0||
           uint64_t(st.st_size)>kMaxCellArchiveBytes){ok=false;break;}
        bytes+=12+name.size()+size_t(st.st_size);
        if(bytes>kMaxCellArchiveBytes){ok=false;break;}
        entries->push_back(world_stamp(name,st));
    }
    closedir(dir);
    std::sort(entries->begin(),entries->end(),[](const auto& a,const auto& b){return a.name<b.name;});
    return ok;
}
void world_put32(std::vector<uint8_t>& v,uint32_t n){for(int i=0;i<4;++i)v.push_back(uint8_t(n>>(8*i)));}
uint32_t world_get32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
bool world_capture_cells(const char* slot,uint8_t** out,uint32_t* size) {
    std::vector<WorldDiskStamp> before,after;
    if(!world_list_cells(slot,&before)) return false;
    std::vector<uint8_t> archive;world_put32(archive,0x31464357);world_put32(archive,uint32_t(before.size()));
    for(const auto& e:before) {
        char path[760];snprintf(path,sizeof(path),"%s/%s/CellsCache/%s",kSavesRoot,slot,e.name.c_str());
        uint8_t* data=nullptr;uint32_t length=0;
        if(!read_whole_file(path,&data,&length)||length!=e.size){free(data);return false;}
        world_put32(archive,uint32_t(e.name.size()));world_put32(archive,length);
        world_put32(archive,world_checksum(data,length));
        archive.insert(archive.end(),e.name.begin(),e.name.end());
        archive.insert(archive.end(),data,data+length);free(data);
    }
    if(!world_list_cells(slot,&after)||before!=after) return false;
    *out=static_cast<uint8_t*>(malloc(archive.size()));if(!*out) return false;
    memcpy(*out,archive.data(),archive.size());*size=uint32_t(archive.size());
    LOG_INFO("world: saved cell archive ready files=%zu bytes=%u",before.size(),*size);
    return true;
}
struct WorldCellEntry {std::string name;const uint8_t* data;uint32_t size;};
bool world_parse_cells(const uint8_t* data,size_t size,std::vector<WorldCellEntry>* entries) {
    entries->clear();
    if(!data||size<8||size>kMaxCellArchiveBytes||world_get32(data)!=0x31464357) return false;
    const uint32_t count=world_get32(data+4);if(count>kMaxCellEntries)return false;
    size_t at=8;std::string previous;
    for(uint32_t i=0;i<count;++i) {
        if(size-at<12)return false;
        uint32_t name_len=world_get32(data+at),bytes=world_get32(data+at+4),sum=world_get32(data+at+8);at+=12;
        if(name_len>120||name_len>size-at)return false;
        std::string name(reinterpret_cast<const char*>(data+at),name_len);at+=name_len;
        if(!world_cell_name(name)||name<=previous||!bytes||bytes>size-at||world_checksum(data+at,bytes)!=sum)return false;
        entries->push_back({name,data+at,bytes});previous=name;at+=bytes;
    }
    return at==size;
}
bool world_extract_cells(const char* staging,const uint8_t* data,size_t size) {
    std::vector<WorldCellEntry> entries;if(!world_parse_cells(data,size,&entries))return false;
    char directory[640];snprintf(directory,sizeof(directory),"%s/CellsCache",staging);
    if(mkdir(directory,0770)!=0)return false;
    for(const auto& e:entries) {
        char path[800];snprintf(path,sizeof(path),"%s/%s",directory,e.name.c_str());
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0660);if(fd<0)return false;
        size_t at=0;while(at<e.size){ssize_t n=write(fd,e.data+at,e.size-at);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return false;}at+=size_t(n);}
        bool ok=fsync(fd)==0;close(fd);if(!ok)return false;
    }
    LOG_INFO("world: restored saved cell files=%zu before slot commit",entries.size());
    return true;
}
bool world_disk_stamps(const char* slot,std::vector<WorldDiskStamp>* out) {
    if(!world_list_cells(slot,out))return false;
    for(int i=0;i<4;++i){char p[600];struct stat st{};snprintf(p,sizeof(p),"%s/%s/%s",kSavesRoot,slot,kWorldFileNames[i]);
        if(lstat(p,&st)!=0||!S_ISREG(st.st_mode)||st.st_size<=0||uint64_t(st.st_size)>8u*1024u*1024u)return false;
        out->push_back(world_stamp(kWorldFileNames[i],st));}
    return true;
}
