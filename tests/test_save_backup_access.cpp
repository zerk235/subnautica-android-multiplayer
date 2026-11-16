#include "modloader/src/save_backup_access.h"
#include <cassert>
#include <cstdlib>
#include <fstream>

int main() {
    using namespace save_backup_access;
    assert(readable_mode(0770, true, 1078) == 02770);
    assert(readable_mode(0600, false, 1078) == 0640);
    assert(readable_mode(0770, true, 12345) == 0775);
    assert(readable_mode(0660, false, 12345) == 0664);
    assert((readable_mode(0600, false, 12345) & 0222) == 0200);
    char name[] = "/data/local/tmp/snmp-access-XXXXXX";
    const char* root = mkdtemp(name); assert(root);
    std::string dir = std::string(root) + "/.snmod_backup_slotA";
    std::string file = dir + "/data.bin";
    assert(!mkdir(dir.c_str(), 0700));
    { std::ofstream out(file); out << "saved progress"; }
    assert(!chmod(file.c_str(), 0600));
    auto result = repair(root);
    assert(!result.errors && result.changed == 3 && result.checked == 3);
    struct stat info{}; assert(!stat(file.c_str(), &info));
    assert((info.st_mode & 0777) == 0604 && info.st_uid == geteuid());
    std::ifstream in(file); std::string data; std::getline(in, data); assert(data == "saved progress");
    assert(!repair(root).changed); // Idempotent: no continual mode churn.
    std::string outside = std::string(root) + "-outside";
    { std::ofstream out(outside); out << "private"; }
    assert(!chmod(outside.c_str(), 0600));
    std::string link = dir + "/link";
    assert(!symlink(outside.c_str(), link.c_str()));
    result = repair(root); assert(result.errors == 1 && result.error == ELOOP);
    assert(!stat(outside.c_str(), &info) && (info.st_mode & 0777) == 0600);
    assert(!unlink(link.c_str()) && !unlink(file.c_str()) && !rmdir(dir.c_str()));
    assert(!rmdir(root) && !unlink(outside.c_str()));
}
