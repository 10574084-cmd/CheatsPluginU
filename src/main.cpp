#include <cctype>
#include <atomic>
#include <utility>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <coreinit/title.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <wups.h>
#include <wups/config_api.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <notifications/notifications.h>

#include "kernel.hpp"

WUPS_PLUGIN_NAME("CheatsPlugin");
WUPS_PLUGIN_DESCRIPTION("Loads per-title .cht/.txt cheats from the SD card with per-file toggles.");
WUPS_PLUGIN_VERSION("0.2.0");
WUPS_PLUGIN_AUTHOR("CheatsPlugin");
WUPS_PLUGIN_LICENSE("GPL-3.0-or-later");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("cheatsplugin");

static constexpr const char *CHEATS_ROOT = "sd:/wiiu/cheatsPlugin/";
static constexpr size_t MAX_CHEATS = 64;
static constexpr size_t MAX_WRITES_PER_FILE = 128;
static constexpr size_t MAX_FILE_BYTES = 64 * 1024;
static constexpr uint32_t MIN_GAME_ADDRESS = 0x01000000;
static constexpr uint32_t MAX_GAME_ADDRESS = 0x50000000;
static constexpr uint32_t WORKER_STACK_SIZE = 0x8000;

static constexpr const char *CFG_ENABLED = "enabled";
static constexpr const char *CFG_NOTIFICATIONS = "notifications";

struct MemoryWrite {
    uint32_t address;
    uint32_t value;
};

struct CheatFile {
    std::string name;
    std::string configId;
    std::vector<MemoryWrite> writes;
    bool enabled = true;
};

static std::vector<CheatFile> gCheats;
static std::atomic<bool> gCheatEnabled[MAX_CHEATS];
static bool gEnabled = true;
static bool gNotifications = true;
static volatile bool gWorkerRunning = false;
static bool gWorkerCreated = false;
static uint64_t gActiveTitleId = 0;
static OSThread gWorkerThread;
alignas(0x100) static uint8_t gWorkerStack[WORKER_STACK_SIZE];

static void Notify(const char *message) {
    if (!gNotifications) return;
    NotificationModuleHandle handle;
    if (NotificationModule_AddDynamicNotification(message, &handle) ==
        NOTIFICATION_MODULE_RESULT_SUCCESS) {
        NotificationModule_FinishDynamicNotification(handle, 4.0f);
    }
}

static bool IsSystemTitle(uint64_t titleId) {
    const uint32_t high = (uint32_t)(titleId >> 32);
    return high == 0x00050010 || high == 0x0005001B ||
           high == 0x00050030 || high == 0x0005000E;
}

static bool HasExtension(const char *name, const char *extension) {
    const size_t nameLen = std::strlen(name);
    const size_t extLen = std::strlen(extension);
    if (nameLen < extLen) return false;
    const char *tail = name + nameLen - extLen;
    for (size_t i = 0; i < extLen; ++i) {
        if (std::tolower((unsigned char)tail[i]) !=
            std::tolower((unsigned char)extension[i])) return false;
    }
    return true;
}

static char *Trim(char *text) {
    while (*text && std::isspace((unsigned char)*text)) ++text;
    size_t len = std::strlen(text);
    while (len && std::isspace((unsigned char)text[len - 1])) text[--len] = '\0';
    return text;
}

static bool ParseHex32(char *text, uint32_t *out) {
    text = Trim(text);
    if (!*text) return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (errno != 0 || end == text || value > 0xFFFFFFFFul) return false;
    end = Trim(end);
    if (*end != '\0') return false;
    *out = (uint32_t)value;
    return true;
}

static bool IsSafeGameAddress(uint32_t address) {
    return (address & 3u) == 0 && address >= MIN_GAME_ADDRESS &&
           address < MAX_GAME_ADDRESS;
}

// Stable, title-specific WUPS storage key based on folder and filename.
static std::string MakeCheatConfigId(const char *folder, const char *filename) {
    uint32_t hash = 2166136261u;
    const auto addText = [&hash](const char *text) {
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
            hash ^= *p;
            hash *= 16777619u;
        }
    };
    addText(folder);
    hash ^= 0xFFu;
    hash *= 16777619u;
    addText(filename);

    char id[24];
    std::snprintf(id, sizeof(id), "cheat_%08X", (unsigned)hash);
    return std::string(id);
}

static bool LoadCheatFile(const char *folder, const char *filename, CheatFile *cheat) {
    char path[320];
    const int written = std::snprintf(path, sizeof(path), "%s%s", folder, filename);
    if (written < 0 || (size_t)written >= sizeof(path)) return false;

    struct stat info {};
    if (stat(path, &info) != 0 || info.st_size <= 0 ||
        (size_t)info.st_size > MAX_FILE_BYTES) return false;

    FILE *file = std::fopen(path, "r");
    if (!file) return false;

    cheat->name = filename;
    cheat->writes.clear();
    char line[256];
    bool malformed = false;

    while (std::fgets(line, sizeof(line), file)) {
        char *content = Trim(line);
        if (!*content || *content == '#' || *content == ';') continue;

        char *comment = std::strchr(content, '#');
        if (comment) *comment = '\0';
        content = Trim(content);
        if (!*content || *content == ';') continue;

        char *equals = std::strchr(content, '=');
        if (!equals) {
            malformed = true;
            continue;
        }
        *equals = '\0';

        uint32_t address = 0, value = 0;
        if (!ParseHex32(content, &address) || !ParseHex32(equals + 1, &value) ||
            !IsSafeGameAddress(address)) {
            malformed = true;
            continue;
        }

        if (cheat->writes.size() >= MAX_WRITES_PER_FILE) {
            malformed = true;
            break;
        }
        cheat->writes.push_back({address, value});
    }

    std::fclose(file);
    if (cheat->writes.empty() || malformed) {
        cheat->writes.clear();
        return false;
    }

    cheat->configId = MakeCheatConfigId(folder, filename);
    cheat->enabled = true;
    const WUPSStorageError storageResult =
        WUPSStorageAPI_GetBool(nullptr, cheat->configId.c_str(), &cheat->enabled);
    if (storageResult == WUPS_STORAGE_ERROR_NOT_FOUND) {
        cheat->enabled = true;
        WUPSStorageAPI_StoreBool(nullptr, cheat->configId.c_str(), true);
    }
    return true;
}

static bool LoadCheatsForTitle(uint64_t titleId) {
    gCheats.clear();
    char folder[256];
    const int written = std::snprintf(folder, sizeof(folder), "%s%016llX/",
                                      CHEATS_ROOT, (unsigned long long)titleId);
    if (written < 0 || (size_t)written >= sizeof(folder)) return false;

    DIR *dir = opendir(folder);
    if (!dir) return false;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && gCheats.size() < MAX_CHEATS) {
        const char *name = entry->d_name;
        if (name[0] == '.' ||
            (!HasExtension(name, ".txt") && !HasExtension(name, ".cht"))) {
            continue;
        }

        CheatFile cheat;
        if (LoadCheatFile(folder, name, &cheat)) {
            const size_t index = gCheats.size();
            gCheatEnabled[index].store(cheat.enabled, std::memory_order_relaxed);
            gCheats.push_back(std::move(cheat));
        }
    }
    closedir(dir);
    return !gCheats.empty();
}

static size_t TotalWrites() {
    size_t total = 0;
    for (const auto &cheat : gCheats) total += cheat.writes.size();
    return total;
}

static void ApplyAllCheats() {
    for (size_t index = 0; index < gCheats.size(); ++index) {
        if (!gCheatEnabled[index].load(std::memory_order_relaxed)) continue;
        for (const auto &write : gCheats[index].writes) {
            CheatsPlugin_WriteU32(write.address, write.value);
        }
    }
}

static int CheatWorker(int, const char **) {
    OSSleepTicks(OSMillisecondsToTicks(1000));
    while (gWorkerRunning && OSGetTitleID() == gActiveTitleId) {
        ApplyAllCheats();
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    gWorkerRunning = false;
    return 0;
}

static void StopWorker() {
    if (!gWorkerCreated) return;
    gWorkerRunning = false;
    int result = 0;
    OSJoinThread(&gWorkerThread, &result);
    gWorkerCreated = false;
}

static bool StartWorker() {
    gWorkerRunning = true;
    if (!OSCreateThread(&gWorkerThread, CheatWorker, 0, nullptr,
                        gWorkerStack + sizeof(gWorkerStack), sizeof(gWorkerStack),
                        20, OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        gWorkerRunning = false;
        return false;
    }
    gWorkerCreated = true;
    OSSetThreadName(&gWorkerThread, "CheatsPlugin");
    OSResumeThread(&gWorkerThread);
    return true;
}

static WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle root);
static void ConfigMenuClosedCallback();

INITIALIZE_PLUGIN() {
    NotificationModule_InitLibrary();

    WUPSConfigAPIOptionsV1 configOptions = {.name = "CheatsPlugin"};
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);

    WUPSStorageError result = WUPSStorageAPI_GetBool(nullptr, CFG_ENABLED, &gEnabled);
    if (result == WUPS_STORAGE_ERROR_NOT_FOUND) {
        WUPSStorageAPI_StoreBool(nullptr, CFG_ENABLED, gEnabled);
    }
    result = WUPSStorageAPI_GetBool(nullptr, CFG_NOTIFICATIONS, &gNotifications);
    if (result == WUPS_STORAGE_ERROR_NOT_FOUND) {
        WUPSStorageAPI_StoreBool(nullptr, CFG_NOTIFICATIONS, gNotifications);
    }
    WUPSStorageAPI_SaveStorage(false);
}

DEINITIALIZE_PLUGIN() {
    StopWorker();
    NotificationModule_DeInitLibrary();
}

ON_APPLICATION_START() {
    StopWorker();
    gCheats.clear();
    gActiveTitleId = OSGetTitleID();

    if (!gEnabled || IsSystemTitle(gActiveTitleId)) return;
    if (!LoadCheatsForTitle(gActiveTitleId)) {
        Notify("CheatsPlugin: no valid .cht/.txt files for this game");
        return;
    }

    char message[100];
    std::snprintf(message, sizeof(message), "CheatsPlugin: %u cheats, %u writes",
                  (unsigned)gCheats.size(), (unsigned)TotalWrites());
    Notify(message);

    if (!StartWorker()) {
        ApplyAllCheats();
        Notify("CheatsPlugin: worker failed; cheats applied once");
    }
}

ON_APPLICATION_ENDS() {
    StopWorker();
    gCheats.clear();
    gActiveTitleId = 0;
}

ON_APPLICATION_REQUESTS_EXIT() {
}

static void EnabledChanged(ConfigItemBoolean *, bool value) {
    gEnabled = value;
    WUPSStorageAPI_StoreBool(nullptr, CFG_ENABLED, value);
}

static void NotificationsChanged(ConfigItemBoolean *, bool value) {
    gNotifications = value;
    WUPSStorageAPI_StoreBool(nullptr, CFG_NOTIFICATIONS, value);
}

static void CheatFileChanged(ConfigItemBoolean *item, bool value) {
    if (!item || !item->identifier) return;

    for (size_t index = 0; index < gCheats.size(); ++index) {
        if (gCheats[index].configId != item->identifier) continue;

        gCheatEnabled[index].store(value, std::memory_order_relaxed);
        WUPSStorageAPI_StoreBool(nullptr, item->identifier, value);

        char message[128];
        std::snprintf(message, sizeof(message), "%s: %s",
                      gCheats[index].name.c_str(), value ? "enabled" : "disabled");
        Notify(message);
        return;
    }
}

static WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle root) {
    WUPSConfigCategoryHandle category;
    WUPSConfigAPICreateCategoryOptionsV1 categoryOptions = {.name = "Settings"};
    if (WUPSConfigAPI_Category_Create(categoryOptions, &category) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemBoolean_AddToCategory(category, CFG_ENABLED,
            "Enable CheatsPlugin (next game launch)", true, gEnabled, &EnabledChanged) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemBoolean_AddToCategory(category, CFG_NOTIFICATIONS,
            "Notifications", true, gNotifications, &NotificationsChanged) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // Add one switch for each valid cheat file loaded for the active title.
    for (size_t index = 0; index < gCheats.size(); ++index) {
        if (WUPSConfigItemBoolean_AddToCategory(
                category, gCheats[index].configId.c_str(), gCheats[index].name.c_str(),
                true, gCheatEnabled[index].load(std::memory_order_relaxed),
                &CheatFileChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    if (WUPSConfigAPI_Category_AddCategory(root, category) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

static void ConfigMenuClosedCallback() {
    WUPSStorageAPI_SaveStorage(false);
}
