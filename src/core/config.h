#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace MR {

struct PathMapping
{
    std::string win;
    std::string mac;
    std::string lin;
    bool enabled = true;
    std::string label;
};

inline void to_json(nlohmann::json& j, const PathMapping& m)
{
    j = nlohmann::json{{"win", m.win}, {"mac", m.mac}, {"lin", m.lin},
                        {"enabled", m.enabled}, {"label", m.label}};
}

inline void from_json(const nlohmann::json& j, PathMapping& m)
{
    if (j.contains("win")) j.at("win").get_to(m.win);
    if (j.contains("mac")) j.at("mac").get_to(m.mac);
    if (j.contains("lin")) j.at("lin").get_to(m.lin);
    if (j.contains("enabled")) j.at("enabled").get_to(m.enabled);
    if (j.contains("label")) j.at("label").get_to(m.label);
}

struct Config
{
    // Sync root path (shared filesystem mount point)
    std::string sync_root;


    // HTTP mesh port
    uint16_t http_port = 8420;

    // IP override (empty = auto-detect)
    std::string ip_override;

    // Node tags (for job targeting)
    std::vector<std::string> tags;

    // Agent settings
    bool auto_start_agent = true;

    // UDP multicast
    bool udp_enabled = true;
    uint16_t udp_port = 4243;

    // UI preferences
    bool show_notifications = true;

    // Rendering
    bool staging_enabled = false;

    // (rndr_dual_mode / rndr_dcc_gate / rndr_block_processes were removed
    // in 0.5.6. The RNDR client is now supervised by an external companion
    // that watches the node-state registry key — see SECURITY.md "Local
    // integration". Unknown keys in an existing config.json are ignored.)

    // Persisted node state
    bool node_stopped = false;

    // Cross-platform path mappings (synced from Tauri UI)
    std::vector<PathMapping> path_mappings;
};

// JSON serialization
inline void to_json(nlohmann::json& j, const Config& c)
{
    j = nlohmann::json{
        {"sync_root", c.sync_root},
        {"http_port", c.http_port},
        {"ip_override", c.ip_override},
        {"tags", c.tags},
        {"auto_start_agent", c.auto_start_agent},
        {"udp_enabled", c.udp_enabled},
        {"udp_port", c.udp_port},
        {"show_notifications", c.show_notifications},
        {"staging_enabled", c.staging_enabled},
        {"node_stopped", c.node_stopped},
        {"path_mappings", c.path_mappings},
    };
}

inline void from_json(const nlohmann::json& j, Config& c)
{
    if (j.contains("sync_root"))         j.at("sync_root").get_to(c.sync_root);
    if (j.contains("http_port"))         c.http_port = j.at("http_port").get<uint16_t>();
    if (j.contains("ip_override"))       j.at("ip_override").get_to(c.ip_override);
    if (j.contains("tags"))              j.at("tags").get_to(c.tags);
    if (j.contains("auto_start_agent"))  j.at("auto_start_agent").get_to(c.auto_start_agent);
    if (j.contains("udp_enabled"))       j.at("udp_enabled").get_to(c.udp_enabled);
    if (j.contains("udp_port"))          c.udp_port = j.at("udp_port").get<uint16_t>();
    if (j.contains("show_notifications")) j.at("show_notifications").get_to(c.show_notifications);
    if (j.contains("staging_enabled"))    j.at("staging_enabled").get_to(c.staging_enabled);
    if (j.contains("node_stopped"))       j.at("node_stopped").get_to(c.node_stopped);
    if (j.contains("path_mappings"))     j.at("path_mappings").get_to(c.path_mappings);
}

// --- Constants ---
constexpr uint32_t PROTOCOL_VERSION = 2;

#ifndef APP_VERSION
#define APP_VERSION "0.5.11"
#endif

} // namespace MR
