#include <algorithm>
#include <cctype>
#include <atomic>
#include <utility>
#include <mutex>
#include <sstream>
#include <curl/curl.h>
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
WUPS_PLUGIN_DESCRIPTION("Finds compatible online Wii U cheats by Title ID and loads local cheat files.");
WUPS_PLUGIN_VERSION("0.4.0");
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
static constexpr const char *CFG_ONLINE_SEARCH = "online_search";

static constexpr size_t MAX_ONLINE_CHEATS = 48;
static constexpr size_t MAX_HTTP_BYTES = 1024 * 1024;
static constexpr size_t MAX_DATABASE_INDEXES = 1024;
static constexpr size_t MAX_ONLINE_WRITES_PER_CHEAT = 128;
static constexpr const char *ONLINE_DB_BASE =
    "https://raw.githubusercontent.com/vincent-coding/uCodes/main/views/pages/cheats/";
static constexpr const char *ONLINE_DB_DIRECTORY_URL =
    "https://api.github.com/repos/vincent-coding/uCodes/contents/views/pages/cheats?ref=main";

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

struct OnlineCheat {
    std::string name;
    std::string configId;
    std::vector<MemoryWrite> writes;
};

static std::vector<CheatFile> gCheats;
static std::atomic<bool> gCheatEnabled[MAX_CHEATS];
static std::vector<OnlineCheat> gOnlineCheats;
static std::atomic<bool> gOnlineCheatEnabled[MAX_ONLINE_CHEATS];
static std::mutex gOnlineCheatsMutex;
static std::atomic<bool> gOnlineSearchEnabled{true};
static bool gCurlInitialized = false;
static bool gEnabled = true;
static bool gNotifications = true;
static std::atomic<bool> gWorkerRunning{false};
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


// Online search uses uCodes' public JSON database. It imports only simple
// gecko.poke32 writes; pointer loops, placeholders, conditionals and Cafe
// handler programs are rejected instead of being partially or incorrectly applied.
static size_t OnlineDownloadWrite(char *data, size_t size, size_t count, void *userdata) {
    if (!userdata || (size != 0 && count > static_cast<size_t>(-1) / size)) return 0;
    const size_t bytes = size * count;
    auto *output = static_cast<std::string *>(userdata);
    if (output->size() > MAX_HTTP_BYTES || bytes > MAX_HTTP_BYTES - output->size()) return 0;
    output->append(data, bytes);
    return bytes;
}

static bool DownloadOnlineText(const std::string &url, std::string *output) {
    if (!output || !gCurlInitialized) return false;
    output->clear();

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CheatsPluginU/0.4");
    struct curl_slist *headers = nullptr;
    if (url.find("https://api.github.com/") == 0) {
        headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
        if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnlineDownloadWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode result = curl_easy_perform(curl);
    long responseCode = 0;
    if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    return result == CURLE_OK && responseCode == 200;
}

static size_t FindJsonObjectEnd(const std::string &json, size_t start) {
    if (start >= json.size() || json[start] != '{') return std::string::npos;
    size_t depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = start; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

static bool ReadJsonString(const std::string &json, const char *key, std::string *output) {
    if (!output || !key) return false;
    const std::string marker = std::string("\"") + key + "\"";
    size_t position = json.find(marker);
    if (position == std::string::npos) return false;
    position += marker.size();
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) ++position;
    if (position >= json.size() || json[position++] != ':') return false;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) ++position;
    if (position >= json.size() || json[position++] != '"') return false;

    output->clear();
    while (position < json.size()) {
        const char c = json[position++];
        if (c == '"') return true;
        if (c != '\\') {
            output->push_back(c);
            continue;
        }
        if (position >= json.size()) return false;
        const char escaped = json[position++];
        switch (escaped) {
            case '"': output->push_back('"'); break;
            case '\\': output->push_back('\\'); break;
            case '/': output->push_back('/'); break;
            case 'b': output->push_back('\b'); break;
            case 'f': output->push_back('\f'); break;
            case 'n': output->push_back('\n'); break;
            case 'r': output->push_back('\r'); break;
            case 't': output->push_back('\t'); break;
            case 'u': {
                if (position + 4 > json.size()) return false;
                unsigned int codePoint = 0;
                for (unsigned int i = 0; i < 4; ++i) {
                    const char h = json[position++];
                    codePoint <<= 4;
                    if (h >= '0' && h <= '9') codePoint |= static_cast<unsigned int>(h - '0');
                    else if (h >= 'a' && h <= 'f') codePoint |= static_cast<unsigned int>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') codePoint |= static_cast<unsigned int>(h - 'A' + 10);
                    else return false;
                }
                output->push_back(codePoint < 0x80 ? static_cast<char>(codePoint) : '?');
                break;
            }
            default: return false;
        }
    }
    return false;
}

static bool ReadJsonBool(const std::string &json, const char *key, bool *value) {
    if (!value || !key) return false;
    const std::string marker = std::string("\"") + key + "\"";
    size_t position = json.find(marker);
    if (position == std::string::npos) return false;
    position += marker.size();
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) ++position;
    if (position >= json.size() || json[position++] != ':') return false;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) ++position;
    if (json.compare(position, 4, "true") == 0) {
        *value = true;
        return true;
    }
    if (json.compare(position, 5, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool ParseJsonHex32(std::string text, uint32_t *output) {
    if (!output) return false;
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    size_t endPos = text.size();
    while (endPos > begin && std::isspace(static_cast<unsigned char>(text[endPos - 1]))) --endPos;
    text = text.substr(begin, endPos - begin);
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return false;
    if (text.size() > 10) return false;
    for (size_t i = 2; i < text.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text.c_str(), &end, 16);
    if (errno != 0 || !end || *end != '\0' || value > 0xFFFFFFFFul) return false;
    *output = static_cast<uint32_t>(value);
    return true;
}

static bool ParseGeckoPoke32Line(const std::string &rawLine, MemoryWrite *write) {
    if (!write) return false;
    size_t begin = 0;
    while (begin < rawLine.size() && std::isspace(static_cast<unsigned char>(rawLine[begin]))) ++begin;
    size_t endPos = rawLine.size();
    while (endPos > begin && std::isspace(static_cast<unsigned char>(rawLine[endPos - 1]))) --endPos;
    const std::string line = rawLine.substr(begin, endPos - begin);
    const std::string prefix = "gecko.poke32(";
    if (line.compare(0, prefix.size(), prefix) != 0 || line.size() <= prefix.size() ||
        line.back() != ')') return false;

    const std::string args = line.substr(prefix.size(), line.size() - prefix.size() - 1);
    const size_t comma = args.find(',');
    if (comma == std::string::npos || args.find(',', comma + 1) != std::string::npos) return false;
    uint32_t address = 0, value = 0;
    if (!ParseJsonHex32(args.substr(0, comma), &address) ||
        !ParseJsonHex32(args.substr(comma + 1), &value) ||
        !IsSafeGameAddress(address)) return false;
    write->address = address;
    write->value = value;
    return true;
}

static bool ParseUgeckoCode(const std::string &codeText, std::vector<MemoryWrite> *writes) {
    if (!writes) return false;
    writes->clear();
    std::istringstream input(codeText);
    std::string line;
    while (std::getline(input, line)) {
        size_t begin = 0;
        while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin]))) ++begin;
        if (begin == line.size() || line[begin] == '#') continue;

        MemoryWrite write{};
        if (!ParseGeckoPoke32Line(line, &write)) {
            writes->clear();
            return false;
        }
        if (writes->size() >= MAX_ONLINE_WRITES_PER_CHEAT) {
            writes->clear();
            return false;
        }
        writes->push_back(write);
    }
    return !writes->empty();
}

static std::string CompactHex(const std::string &text) {
    std::string result;
    for (unsigned char c : text) {
        if (std::isxdigit(c)) result.push_back(static_cast<char>(std::toupper(c)));
    }
    return result;
}

static bool OnlineTitleIdMatches(const std::string &infoJson, uint64_t titleId) {
    char titleIdText[17];
    std::snprintf(titleIdText, sizeof(titleIdText), "%016llX",
                  static_cast<unsigned long long>(titleId));
    std::string declaredTitleIds;
    if (!ReadJsonString(infoJson, "titleid", &declaredTitleIds)) return false;
    return CompactHex(declaredTitleIds).find(titleIdText) != std::string::npos;
}

static std::string MakeOnlineConfigId(uint64_t titleId, const std::string &name, size_t ordinal) {
    uint32_t hash = 2166136261u;
    const auto addByte = [&hash](unsigned char byte) {
        hash ^= byte;
        hash *= 16777619u;
    };
    for (int shift = 56; shift >= 0; shift -= 8) addByte(static_cast<unsigned char>(titleId >> shift));
    for (unsigned char c : name) addByte(c);
    for (int shift = 24; shift >= 0; shift -= 8) addByte(static_cast<unsigned char>(ordinal >> shift));
    char id[24];
    std::snprintf(id, sizeof(id), "online_%08X", static_cast<unsigned>(hash));
    return std::string(id);
}

static void ParseOnlineCodesJson(const std::string &codesJson, uint64_t titleId,
                                 std::vector<OnlineCheat> *output) {
    if (!output) return;
    output->clear();

    size_t cursor = 0;
    while (output->size() < MAX_ONLINE_CHEATS) {
        const size_t objectStart = codesJson.find('{', cursor);
        if (objectStart == std::string::npos) break;
        const size_t objectEnd = FindJsonObjectEnd(codesJson, objectStart);
        if (objectEnd == std::string::npos) break;

        const std::string object = codesJson.substr(objectStart, objectEnd - objectStart + 1);
        cursor = objectEnd + 1;

        bool displayed = true;
        if (ReadJsonBool(object, "display", &displayed) && !displayed) continue;

        std::string name;
        if (!ReadJsonString(object, "title", &name) || name.empty()) continue;

        const size_t ugeckoKey = object.find("\"ugecko\"");
        if (ugeckoKey == std::string::npos) continue;
        const size_t ugeckoStart = object.find('{', ugeckoKey);
        if (ugeckoStart == std::string::npos) continue;
        const size_t ugeckoEnd = FindJsonObjectEnd(object, ugeckoStart);
        if (ugeckoEnd == std::string::npos) continue;
        const std::string ugeckoObject = object.substr(ugeckoStart, ugeckoEnd - ugeckoStart + 1);

        bool enabledByDatabase = false;
        if (!ReadJsonBool(ugeckoObject, "enabled", &enabledByDatabase) || !enabledByDatabase) continue;

        std::string codeText;
        if (!ReadJsonString(ugeckoObject, "code", &codeText)) continue;

        std::vector<MemoryWrite> writes;
        if (!ParseUgeckoCode(codeText, &writes)) continue;

        OnlineCheat cheat;
        cheat.name = "Online: " + name;
        cheat.configId = MakeOnlineConfigId(titleId, name, output->size());
        cheat.writes = std::move(writes);
        output->push_back(std::move(cheat));
    }
}

static bool ParseDatabaseIndexName(const std::string &name, unsigned int *output) {
    if (!output || name.empty()) return false;
    for (unsigned char c : name) {
        if (!std::isdigit(c)) return false;
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long value = std::strtoul(name.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || value > 1000000ul) return false;
    *output = static_cast<unsigned int>(value);
    return true;
}

// Download the live directory listing instead of assuming game indexes are 0..3.
// New numeric folders in the upstream uCodes catalog are discovered automatically over Wi-Fi.
static bool DiscoverOnlineDatabaseIndexes(std::vector<unsigned int> *indexes) {
    if (!indexes) return false;
    indexes->clear();

    std::string listing;
    if (!DownloadOnlineText(ONLINE_DB_DIRECTORY_URL, &listing)) return false;

    size_t cursor = 0;
    while (indexes->size() < MAX_DATABASE_INDEXES) {
        const size_t objectStart = listing.find('{', cursor);
        if (objectStart == std::string::npos) break;
        const size_t objectEnd = FindJsonObjectEnd(listing, objectStart);
        if (objectEnd == std::string::npos) break;

        const std::string object = listing.substr(objectStart, objectEnd - objectStart + 1);
        cursor = objectEnd + 1;

        std::string name, type;
        if (!ReadJsonString(object, "name", &name) ||
            !ReadJsonString(object, "type", &type) || type != "dir") continue;

        unsigned int index = 0;
        if (ParseDatabaseIndexName(name, &index)) indexes->push_back(index);
    }

    std::sort(indexes->begin(), indexes->end());
    indexes->erase(std::unique(indexes->begin(), indexes->end()), indexes->end());
    return !indexes->empty();
}

static bool SearchOnlineCheats(uint64_t titleId) {
    std::vector<unsigned int> indexes;
    if (!DiscoverOnlineDatabaseIndexes(&indexes)) {
        Notify("CheatsPlugin: cannot download the online catalog; check Wi-Fi and CURLWrapperModule");
        return false;
    }

    size_t infoFetched = 0;
    bool matchedDatabase = false;
    unsigned int matchedDatabaseIndex = 0;

    // Inspect every discoverable numeric game folder from the online catalog.
    // The match remains an exact 64-bit Title ID check; we never guess by game name.
    for (const unsigned int index : indexes) {
        char url[240];
        std::snprintf(url, sizeof(url), "%s%u/info.json", ONLINE_DB_BASE, index);
        std::string infoJson;
        if (!DownloadOnlineText(url, &infoJson)) continue;
        ++infoFetched;
        if (OnlineTitleIdMatches(infoJson, titleId)) {
            matchedDatabaseIndex = index;
            matchedDatabase = true;
            break;
        }
    }

    if (!matchedDatabase) {
        if (infoFetched == 0) {
            Notify("CheatsPlugin: catalog loaded, but game metadata could not be downloaded");
        } else {
            char message[128];
            std::snprintf(message, sizeof(message),
                          "CheatsPlugin: no Title ID match in %u catalog entries",
                          static_cast<unsigned>(infoFetched));
            Notify(message);
        }
        return false;
    }

    char codesUrl[240];
    std::snprintf(codesUrl, sizeof(codesUrl), "%s%u/codes.json",
                  ONLINE_DB_BASE, matchedDatabaseIndex);
    std::string codesJson;
    if (!DownloadOnlineText(codesUrl, &codesJson)) {
        Notify("CheatsPlugin: found the game, but could not download its code list");
        return false;
    }

    std::vector<OnlineCheat> downloaded;
    ParseOnlineCodesJson(codesJson, titleId, &downloaded);
    {
        std::lock_guard<std::mutex> lock(gOnlineCheatsMutex);
        gOnlineCheats = std::move(downloaded);
        for (size_t i = 0; i < MAX_ONLINE_CHEATS; ++i) {
            bool storedEnabled = false;
            if (i < gOnlineCheats.size()) {
                const WUPSStorageError storageResult =
                    WUPSStorageAPI_GetBool(nullptr, gOnlineCheats[i].configId.c_str(), &storedEnabled);
                if (storageResult == WUPS_STORAGE_ERROR_NOT_FOUND) {
                    storedEnabled = false;
                    WUPSStorageAPI_StoreBool(nullptr, gOnlineCheats[i].configId.c_str(), false);
                } else if (storageResult != WUPS_STORAGE_ERROR_SUCCESS) {
                    storedEnabled = false;
                }
            }
            gOnlineCheatEnabled[i].store(storedEnabled, std::memory_order_relaxed);
        }
    }

    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(gOnlineCheatsMutex);
        count = gOnlineCheats.size();
    }
    if (count == 0) {
        Notify("CheatsPlugin: game found, but no fixed-value poke32 cheats were compatible");
        return false;
    }

    char message[100];
    std::snprintf(message, sizeof(message), "CheatsPlugin: found %u compatible online cheats",
                  static_cast<unsigned>(count));
    Notify(message);
    return true;
}

static void ApplyAllOnlineCheats() {
    std::lock_guard<std::mutex> lock(gOnlineCheatsMutex);
    for (size_t index = 0; index < gOnlineCheats.size(); ++index) {
        if (!gOnlineCheatEnabled[index].load(std::memory_order_relaxed)) continue;
        for (const auto &write : gOnlineCheats[index].writes) {
            CheatsPlugin_WriteU32(write.address, write.value);
        }
    }
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
    if (gWorkerRunning.load(std::memory_order_relaxed) &&
        gOnlineSearchEnabled.load(std::memory_order_relaxed) &&
        OSGetTitleID() == gActiveTitleId) {
        SearchOnlineCheats(gActiveTitleId);
    }

    while (gWorkerRunning.load(std::memory_order_relaxed) && OSGetTitleID() == gActiveTitleId) {
        ApplyAllCheats();
        ApplyAllOnlineCheats();
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    gWorkerRunning.store(false, std::memory_order_relaxed);
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

    gCurlInitialized = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);

    WUPSStorageError result = WUPSStorageAPI_GetBool(nullptr, CFG_ENABLED, &gEnabled);
    if (result == WUPS_STORAGE_ERROR_NOT_FOUND) {
        WUPSStorageAPI_StoreBool(nullptr, CFG_ENABLED, gEnabled);
    }
    result = WUPSStorageAPI_GetBool(nullptr, CFG_NOTIFICATIONS, &gNotifications);
    if (result == WUPS_STORAGE_ERROR_NOT_FOUND) {
        WUPSStorageAPI_StoreBool(nullptr, CFG_NOTIFICATIONS, gNotifications);
    }

    bool onlineSearch = true;
    result = WUPSStorageAPI_GetBool(nullptr, CFG_ONLINE_SEARCH, &onlineSearch);
    if (result == WUPS_STORAGE_ERROR_NOT_FOUND) {
        WUPSStorageAPI_StoreBool(nullptr, CFG_ONLINE_SEARCH, onlineSearch);
    } else if (result != WUPS_STORAGE_ERROR_SUCCESS) {
        onlineSearch = true;
    }
    gOnlineSearchEnabled.store(onlineSearch, std::memory_order_relaxed);
    WUPSStorageAPI_SaveStorage(false);
}

DEINITIALIZE_PLUGIN() {
    StopWorker();
    if (gCurlInitialized) {
        curl_global_cleanup();
        gCurlInitialized = false;
    }
    NotificationModule_DeInitLibrary();
}

ON_APPLICATION_START() {
    StopWorker();
    gCheats.clear();
    {
        std::lock_guard<std::mutex> lock(gOnlineCheatsMutex);
        gOnlineCheats.clear();
    }
    for (size_t i = 0; i < MAX_ONLINE_CHEATS; ++i) {
        gOnlineCheatEnabled[i].store(false, std::memory_order_relaxed);
    }
    gActiveTitleId = OSGetTitleID();

    if (!gEnabled || IsSystemTitle(gActiveTitleId)) return;
    const bool hasLocalCheats = LoadCheatsForTitle(gActiveTitleId);
    if (hasLocalCheats) {
        char message[100];
        std::snprintf(message, sizeof(message), "CheatsPlugin: %u local files, %u writes",
                      (unsigned)gCheats.size(), (unsigned)TotalWrites());
        Notify(message);
    }

    if (!hasLocalCheats && !gOnlineSearchEnabled.load(std::memory_order_relaxed)) {
        Notify("CheatsPlugin: no local cheats and online search is disabled");
        return;
    }

    if (!StartWorker()) {
        if (hasLocalCheats) ApplyAllCheats();
        Notify("CheatsPlugin: worker failed; local cheats were applied once");
    }
}

ON_APPLICATION_ENDS() {
    StopWorker();
    gCheats.clear();
    {
        std::lock_guard<std::mutex> lock(gOnlineCheatsMutex);
        gOnlineCheats.clear();
    }
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

static void OnlineSearchChanged(ConfigItemBoolean *, bool value) {
    gOnlineSearchEnabled.store(value, std::memory_order_relaxed);
    WUPSStorageAPI_StoreBool(nullptr, CFG_ONLINE_SEARCH, value);
    Notify("CheatsPlugin: online-search setting applies on the next game launch");
}

static void OnlineCheatChanged(ConfigItemBoolean *item, bool value) {
    if (!item || !item->identifier) return;
    std::lock_guard<std::mutex> lock(gOnlineCheatsMutex);
    for (size_t index = 0; index < gOnlineCheats.size(); ++index) {
        if (gOnlineCheats[index].configId != item->identifier) continue;
        gOnlineCheatEnabled[index].store(value, std::memory_order_relaxed);
        WUPSStorageAPI_StoreBool(nullptr, item->identifier, value);
        Notify(value ? "CheatsPlugin: online cheat enabled" : "CheatsPlugin: online cheat disabled");
        return;
    }
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

    if (WUPSConfigItemBoolean_AddToCategory(category, CFG_ONLINE_SEARCH,
            "Search online cheats on game launch", true,
            gOnlineSearchEnabled.load(std::memory_order_relaxed), &OnlineSearchChanged) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    {
        std::lock_guard<std::mutex> lock(gOnlineCheatsMutex);
        for (size_t index = 0; index < gOnlineCheats.size(); ++index) {
            if (WUPSConfigItemBoolean_AddToCategory(
                    category, gOnlineCheats[index].configId.c_str(),
                    gOnlineCheats[index].name.c_str(), false,
                    gOnlineCheatEnabled[index].load(std::memory_order_relaxed),
                    &OnlineCheatChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
                return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
            }
        }
    }

    // Add one switch for each valid local cheat file loaded for the active title.
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
