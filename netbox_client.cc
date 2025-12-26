#include "netbox_client.h"
#include <curl/curl.h>
#include <jsoncpp/json/json.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <ctime>
#include <iomanip>

// Forward declarations for Kea types
namespace isc {
namespace dhcp {
    class Lease6;
    typedef std::shared_ptr<Lease6> Lease6Ptr;
}
}

// Forward declarations (defined in pd_webhook.cc)
enum class ErrorCode;
struct WebhookConfig;
extern WebhookConfig g_cfg;

// Error logging macro
#define ERROR_LOG(msg) do { \
    g_cfg.last_error_msg = msg; \
    std::cerr << "[ERROR] " << msg << std::endl; \
} while(0)

// Debug logging macro
#define DEBUG_LOG(msg) do { if (g_cfg.debug) std::cout << msg << std::endl; } while(0)

// Hex encode helper for DUID, etc.
static std::string
toHex(const std::vector<uint8_t>& data) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (auto byte : data) {
        os << std::setw(2) << static_cast<int>(byte);
    }
    return os.str();
}

// Safe integer parsing
static int safeParseInt(const std::string& str, int default_val = -1) {
    try {
        size_t pos = 0;
        int val = std::stoi(str, &pos);
        if (pos != str.size()) {
            return default_val;  // Trailing characters
        }
        return val;
    } catch (const std::exception& e) {
        DEBUG_LOG("PD_WEBHOOK: Failed to parse int: " << e.what());
        return default_val;
    }
}

// NetBox client implementation
class NetBoxClient : public INetBoxClient {
public:
    NetBoxClient() = default;
    ~NetBoxClient() override = default;

    // HTTP request methods
    std::string postWebhook(const std::string& payload) override {
        if (!g_cfg.enabled || g_cfg.url.empty()) {
            return "";
        }

        DEBUG_LOG("PD_WEBHOOK: Sending webhook payload: " << payload);

        std::string response = netboxHttpRequest("POST", g_cfg.url, payload);

        if (response.empty()) {
            ERROR_LOG("PD_WEBHOOK: Webhook request failed");
        } else {
            DEBUG_LOG("PD_WEBHOOK: Webhook response: " << response);
        }

        return response;
    }

    std::string netboxHttpRequest(const std::string& method, const std::string& endpoint, const std::string& payload) override {
        if (!g_cfg.netbox_enabled) {
            return "";
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            ERROR_LOG("Failed to initialize CURL");
            return "";
        }

        std::string url = g_cfg.netbox_url + endpoint;
        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, g_cfg.timeout_ms);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        // Set headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("Authorization: Token " + g_cfg.netbox_token).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        if (!payload.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        }

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            ERROR_LOG("HTTP request failed: " + std::string(curl_easy_strerror(res)));
            return "";
        }

        return response;
    }

    // NetBox API operations
    int findPrefixId(const std::string& prefix, int prefix_length) override {
        std::string search_url = "ipam/prefixes/?prefix=" + prefix + "/" + std::to_string(prefix_length);
        std::string response = netboxHttpRequest("GET", search_url, "");

        if (response.empty()) {
            return -1;
        }

        Json::Value root;
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        std::string errors;
        bool success = reader->parse(response.c_str(), response.c_str() + response.size(), &root, &errors);
        delete reader;

        if (!success || !root.isMember("results") || !root["results"].isArray()) {
            DEBUG_LOG("PD_WEBHOOK: Failed to parse NetBox response: " << errors);
            return -1;
        }

        if (root["results"].size() == 0) {
            return -1;
        }

        return safeParseInt(root["results"][0]["id"].asString(), -1);
    }

    bool createPrefix(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft) override {
        // Calculate expiration timestamp (current time + valid lifetime)
        time_t now = time(nullptr);
        time_t expires_at = now + valid_lft;

        Json::Value payload;
        payload["prefix"] = data.prefix + "/" + std::to_string(data.prefix_length);
        payload["status"] = "active";
        payload["description"] = "DHCPv6 PD assignment - IAID: " + std::to_string(data.iaid);

        Json::Value custom_fields;
        custom_fields["dhcpv6_client_duid"] = data.client_duid;
        custom_fields["dhcpv6_iaid"] = static_cast<int>(data.iaid);
        custom_fields["dhcpv6_cpe_link_local"] = data.cpe_link_local;
        custom_fields["dhcpv6_router_ip"] = data.router_ip;
        custom_fields["dhcpv6_router_link_addr"] = data.router_link_addr;
        custom_fields["dhcpv6_leasetime"] = static_cast<int>(expires_at);
        payload["custom_fields"] = custom_fields;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string payload_str = Json::writeString(builder, payload);
        DEBUG_LOG("PD_WEBHOOK: createPrefix payload: " << payload_str);

        std::string response = netboxHttpRequest("POST", "ipam/prefixes/", payload_str);

        if (response.empty()) {
            return false;
        }

        // Check if creation was successful
        if (response.find("\"id\":") != std::string::npos) {
            return true;
        }

        return false;
    }

    bool updatePrefix(int prefix_id, const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft, const std::string& status) override {
        std::string endpoint = "ipam/prefixes/" + std::to_string(prefix_id) + "/";

        // Calculate expiration timestamp (current time + valid lifetime)
        time_t now = time(nullptr);
        time_t expires_at = now + valid_lft;

        Json::Value payload;
        payload["status"] = status;
        payload["description"] = "DHCPv6 PD assignment - IAID: " + std::to_string(data.iaid);

        Json::Value custom_fields;
        custom_fields["dhcpv6_client_duid"] = data.client_duid;
        custom_fields["dhcpv6_iaid"] = static_cast<int>(data.iaid);
        custom_fields["dhcpv6_cpe_link_local"] = data.cpe_link_local;
        custom_fields["dhcpv6_router_ip"] = data.router_ip;
        custom_fields["dhcpv6_router_link_addr"] = data.router_link_addr;
        custom_fields["dhcpv6_leasetime"] = static_cast<int>(expires_at);
        payload["custom_fields"] = custom_fields;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string payload_str = Json::writeString(builder, payload);
        DEBUG_LOG("PD_WEBHOOK: updatePrefix payload: " << payload_str);

        std::string response = netboxHttpRequest("PATCH", endpoint, payload_str);

        if (response.empty()) {
            return false;
        }

        // Check if update was successful
        if (response.find("\"id\":") != std::string::npos) {
            return true;
        }

        return false;
    }

    bool updateExpiredPrefix(int prefix_id, const PdAssignmentData& data) override {
        std::string endpoint = "ipam/prefixes/" + std::to_string(prefix_id) + "/";

        Json::Value payload;
        payload["status"] = "deprecated";  // Just mark as deprecated/expired

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string payload_str = Json::writeString(builder, payload);
        DEBUG_LOG("PD_WEBHOOK: updateExpiredPrefix payload: " << payload_str);

        std::string response = netboxHttpRequest("PATCH", endpoint, payload_str);

        if (response.empty()) {
            return false;
        }

        // Check if update was successful
        if (response.find("\"id\":") != std::string::npos) {
            return true;
        }

        return false;
    }

    // High-level operations
    void sendNetBoxRequest(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft) override {
        DEBUG_LOG("PD_WEBHOOK: sendNetBoxRequest called for prefix " << data.prefix << "/" << data.prefix_length
                  << " (valid_lft=" << valid_lft << ", preferred_lft=" << preferred_lft << ")");

        if (!g_cfg.netbox_enabled || g_cfg.netbox_url.empty() || g_cfg.netbox_token.empty()) {
            DEBUG_LOG("PD_WEBHOOK: NetBox not properly configured");
            return;
        }

        // Check if prefix already exists
        int existing_prefix_id = findPrefixId(data.prefix, data.prefix_length);

        if (existing_prefix_id > 0) {
            // Update existing prefix
            updatePrefix(existing_prefix_id, data, valid_lft, preferred_lft, "active");
        } else {
            // Create new prefix
            createPrefix(data, valid_lft, preferred_lft);
        }
    }

private:
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t totalSize = size * nmemb;
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), totalSize);
        return totalSize;
    }
};

// Factory function
std::unique_ptr<INetBoxClient> createNetBoxClient() {
    return std::make_unique<NetBoxClient>();
}