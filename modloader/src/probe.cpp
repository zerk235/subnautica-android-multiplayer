#include "probe.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "il2cpp_api.h"
#include "log.h"

namespace {

struct ProbeEntry {
    const char* namespaze;
    const char* klass;
    const char* method;
};

const ProbeEntry kTargets[] = {
    {"", "Player", nullptr},
    {"", "PlayerController", nullptr},
    {"", "PlayerAnimationController", nullptr},
    {"", "Inventory", nullptr},
    {"", "Inventory_Bundle", nullptr},
    {"", "Equipment", nullptr},
    {"", "World", nullptr},
    {"", "PDAData", nullptr},
    {"", "SeaMoth", nullptr},
    {"", "SeaMothMain", nullptr},
    {"", "Exosuit", nullptr},
    {"", "Cyclops", nullptr},
    {"", "Buildable", nullptr},
    {"", "BuildableVendingMachine", nullptr},
    {"", "CraftingManager", nullptr},
    {"", "TechManager", nullptr},
    {"", "Pickupable", nullptr},
    {"", "UserStorageMobile", nullptr},
    {"", "SyncingCloudSaves", nullptr},
    {"", "NetworkIdentity", nullptr},
    {"", "NetworkPlayer", nullptr},
    {"", "PhotonNetwork", nullptr},
};

void write_report_line(int fd, const char* text) {
    if (fd < 0) return;
    ssize_t ignored = write(fd, text, strlen(text));
    (void)ignored;
}

// il2cpp_class_from_name only reports types the runtime already materialised. Sweeping the
// metadata index of every loaded image resolves the rest and, as a bonus, records which
// assembly each target actually lives in.
struct SweepState {
    const ProbeEntry* targets;
    size_t target_count;
    const char* assembly;
    struct Il2CppClass** found;
    const char** owner;
    size_t* pending;
};

void sweep_visit(struct Il2CppClass* klass, void* ctx) {
    SweepState* state = static_cast<SweepState*>(ctx);
    if (!il2cpp_class_get_name) return;

    const char* name = il2cpp_class_get_name(klass);
    if (!name) return;

    for (size_t i = 0; i < state->target_count; i++) {
        if (state->found[i] || strcmp(name, state->targets[i].klass) != 0) continue;

        const char* want_ns = state->targets[i].namespaze;
        if (want_ns[0]) {
            const char* actual = il2cpp_class_get_namespace(klass);
            if (!actual || strcmp(actual, want_ns) != 0) continue;
        }

        state->found[i] = klass;
        state->owner[i] = state->assembly;
        (*state->pending)--;
        LOG_INFO("class %s found at %p in %s (metadata sweep)",
                 state->targets[i].klass, klass, state->assembly);
    }
}

int open_report() {
    static const char* kPaths[] = {
        "/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_log/compat.txt",
        "/data/data/com.unknownworlds.subnautica/files/mod_log/compat.txt",
    };
    for (const char* path : kPaths) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
        if (fd >= 0) {
            LOG_INFO("compat report: %s", path);
            return fd;
        }
    }
    return -1;
}

}  // namespace

void probe_game() {
    int report = open_report();

    struct Il2CppDomain* domain = il2cpp_domain_get();
    size_t count = 0;
    struct Il2CppAssembly** assemblies = domain ? il2cpp_domain_get_assemblies(domain, &count)
                                               : nullptr;
    if (!assemblies || !count) {
        LOG_ERROR("assembly list unavailable");
        if (report >= 0) close(report);
        return;
    }

    LOG_INFO("domain has %zu assemblies", count);

    // Gameplay types are spread over several assemblies, so every class is looked up
    // in all of them instead of only in Assembly-CSharp.dll.
    struct Il2CppImage** images = static_cast<struct Il2CppImage**>(
        calloc(count, sizeof(struct Il2CppImage*)));
    const char** names = static_cast<const char**>(calloc(count, sizeof(const char*)));
    if (!images || !names) {
        LOG_ERROR("out of memory while listing assemblies");
        free(images);
        free(names);
        if (report >= 0) close(report);
        return;
    }

    size_t usable = 0;
    for (size_t i = 0; i < count; i++) {
        struct Il2CppImage* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        const char* name = il2cpp_image_get_name(image);
        images[usable] = image;
        names[usable] = name ? name : "?";
        LOG_INFO("  assembly[%zu]: %s", usable, names[usable]);
        usable++;
    }

    // Build the class -> assembly map in a single sweep: the lookup below is O(classes)
    // per target otherwise, and there are two dozen targets.
    struct TargetIndex {
        struct Il2CppClass* klass[sizeof(kTargets) / sizeof(kTargets[0])];
        const char* owner[sizeof(kTargets) / sizeof(kTargets[0])];
    } found;
    memset(&found, 0, sizeof(found));

    for (const ProbeEntry& entry : kTargets) {
        found.klass[&entry - kTargets] = find_class_in_domain(entry.namespaze, entry.klass);
    }

    size_t pending = 0;
    for (size_t i = 0; i < sizeof(found.klass) / sizeof(found.klass[0]); i++) {
        if (!found.klass[i]) pending++;
    }

    if (pending) {
        LOG_INFO("%zu target(s) not materialised yet, sweeping metadata", pending);
        for (size_t i = 0; i < usable && pending; i++) {
            SweepState state{kTargets, sizeof(kTargets) / sizeof(kTargets[0]), names[i],
                             found.klass, found.owner, &pending};
            enumerate_image_classes(images[i], sweep_visit, &state);
        }
    }

    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); i++) {
        const ProbeEntry& entry = kTargets[i];
        struct Il2CppClass* klass = found.klass[i];
        const char* owner = found.owner[i] ? found.owner[i] : "?";

        char line[256];
        snprintf(line, sizeof(line), "%s%s%s%s%s%s%s\n", klass ? "OK   " : "MISS ",
                 entry.namespaze, entry.namespaze[0] ? "." : "", entry.klass,
                 entry.method ? entry.method : "", klass ? "  <- " : "",
                 klass ? owner : "");
        write_report_line(report, line);

        if (klass) {
            LOG_INFO("class %s found at %p in %s", entry.klass, klass, owner);
        } else {
            LOG_WARN("class %s%s%s NOT FOUND in any of %zu assemblies", entry.namespaze,
                     entry.namespaze[0] ? "." : "", entry.klass, usable);
        }
    }

    free(images);
    free(names);

    if (report >= 0) close(report);
    LOG_INFO("probe done, compat report written");
}
