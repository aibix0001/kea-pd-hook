#include <cc/data.h>

#include <dhcp/pkt6.h>
#include <dhcpsrv/lease.h>
#include <dhcp/option.h>
#include <dhcp/option6_ia.h>
#include <dhcp/std_option_defs.h>
#include <dhcp/dhcp6.h>
#include <hooks/callout_handle.h>
#include <hooks/hooks.h>
#include <hooks/library_handle.h>

#include <curl/curl.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace isc;
using namespace isc::data;
using namespace isc::dhcp;
using namespace isc::hooks;

// Data structure for PD assignment information
struct PdAssignmentData {
    std::string client_duid;        // Client DUID (hex encoded)
    std::string prefix;              // Assigned prefix (e.g., "2001:db8:56::")
    int prefix_length;               // Prefix length (e.g., 56)
    uint32_t iaid;                   // Identity association ID
    std::string cpe_link_local;      // CPE's link-local address
    std::string router_ip;           // Router's IP address
};

// Simple runtime configuration for this library.
struct WebhookConfig {
    std::string url;
    long timeout_ms{2000};
    bool enabled{false};
    
    // NetBox API configuration
    std::string netbox_url;
    std::string netbox_token;
    bool netbox_enabled{false};
};

static WebhookConfig g_cfg;

// Hex encode helper for DUID, etc.
static std::string
toHex(const std::vector<uint8_t>& data) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (auto b : data) {
        os << std::setw(2) << static_cast<unsigned int>(b);
    }
    return os.str();
}

// Post JSON payload to the configured webhook URL.
static void
postWebhook(const std::string& body) {
    if (!g_cfg.enabled || g_cfg.url.empty()) {
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, g_cfg.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, g_cfg.timeout_ms);

    // Errors are intentionally ignored; this library is notification-only.
    (void)curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// Make HTTP request to NetBox API and return response
static std::string
netboxHttpRequest(const std::string& method, const std::string& endpoint, const std::string& data) {
    if (!g_cfg.netbox_enabled || g_cfg.netbox_url.empty() || g_cfg.netbox_token.empty()) {
        return "";
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return "";
    }

    std::string response;
    std::string full_url = g_cfg.netbox_url;
    if (full_url.back() != '/') {
        full_url += "/";
    }
    full_url += "api/" + endpoint;

    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Token " + g_cfg.netbox_token;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, g_cfg.timeout_ms);

    // Set up response callback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    // Set request method
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
    } else if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return "";
    }

    return response;
}

// Check if prefix exists in NetBox and return its ID
static int
findPrefixId(const std::string& prefix, int prefix_length) {
    std::string search_url = "ipam/prefixes/?prefix=" + prefix + "/" + std::to_string(prefix_length);
    std::string response = netboxHttpRequest("GET", search_url, "");
    
    if (response.empty()) {
        return -1;
    }
    
    // Check if prefix exists
    if (response.find("\"count\":0") == std::string::npos) {
        // Prefix exists, extract ID
        size_t id_pos = response.find("\"id\":");
        if (id_pos != std::string::npos) {
            id_pos += 5;
            size_t comma_pos = response.find(",", id_pos);
            if (comma_pos != std::string::npos) {
                return std::stoi(response.substr(id_pos, comma_pos - id_pos));
            }
        }
    }
    
    return -1;
}

// Update existing prefix with new data
static bool
updatePrefix(int prefix_id, const PdAssignmentData& data) {
    std::string endpoint = "ipam/prefixes/" + std::to_string(prefix_id) + "/";
    
    std::ostringstream payload;
    payload << "{";
    payload << "\"status\":\"active\",";
    payload << "\"description\":\"DHCPv6 PD assignment - IAID: " << data.iaid << "\",";
    payload << "\"custom_fields\":{";
    payload << "\"client_duid\":\"" << data.client_duid << "\",";
    payload << "\"iaid\":" << data.iaid << ",";
    payload << "\"cpe_link_local\":\"" << data.cpe_link_local << "\",";
    payload << "\"router_ip\":\"" << data.router_ip << "\"";
    payload << "}";
    payload << "}";
    
    std::string response = netboxHttpRequest("PATCH", endpoint, payload.str());
    
    if (response.empty()) {
        return false;
    }
    
    // Check if update was successful
    if (response.find("\"id\":") != std::string::npos) {
        return true;
    }
    
    return false;
}

// Create new prefix in NetBox
static bool
createPrefix(const PdAssignmentData& data) {
    std::ostringstream payload;
    payload << "{";
    payload << "\"prefix\":\"" << data.prefix << "/" << data.prefix_length << "\",";
    payload << "\"status\":\"active\",";
    payload << "\"description\":\"DHCPv6 PD assignment - IAID: " << data.iaid << "\",";
    payload << "\"custom_fields\":{";
    payload << "\"client_duid\":\"" << data.client_duid << "\",";
    payload << "\"iaid\":" << data.iaid << ",";
    payload << "\"cpe_link_local\":\"" << data.cpe_link_local << "\",";
    payload << "\"router_ip\":\"" << data.router_ip << "\"";
    payload << "}";
    payload << "}";
    
    std::string response = netboxHttpRequest("POST", "ipam/prefixes/", payload.str());
    
    if (response.empty()) {
        return false;
    }
    
    // Check if creation was successful
    if (response.find("\"id\":") != std::string::npos) {
        return true;
    }
    
    return false;
}

// Send request to NetBox API with check-then-create-or-update logic
static void
sendNetBoxRequest(const PdAssignmentData& data) {
    if (!g_cfg.netbox_enabled || g_cfg.netbox_url.empty() || g_cfg.netbox_token.empty()) {
        return;
    }

    // Check if prefix already exists
    int existing_prefix_id = findPrefixId(data.prefix, data.prefix_length);
    
    if (existing_prefix_id > 0) {
        // Update existing prefix
        updatePrefix(existing_prefix_id, data);
    } else {
        // Create new prefix
        createPrefix(data);
    }
}

// Extract client DUID from CLIENTID option
static std::string
extractClientDuid(const Pkt6Ptr& query) {
    OptionPtr clientid_opt = query->getOption(D6O_CLIENTID);
    if (clientid_opt) {
        const std::vector<uint8_t>& d = clientid_opt->getData();
        return toHex(d);
    }
    return "";
}

// Extract CPE link-local address from packet source or CLIENTID
static std::string
extractCpeLinkLocal(const Pkt6Ptr& query) {
    // Try to get from packet source first
    if (query && !query->getRemoteAddr().isV6Zero()) {
        std::string remote_addr = query->getRemoteAddr().toText();
        // Check if it's a link-local address (starts with fe80::)
        if (remote_addr.find("fe80::") == 0) {
            return remote_addr;
        }
    }
    
    // Fallback: try to construct from CLIENTID if available
    OptionPtr clientid_opt = query->getOption(D6O_CLIENTID);
    if (clientid_opt) {
        const std::vector<uint8_t>& d = clientid_opt->getData();
        if (d.size() >= 6) {
            // Simple fallback: construct a dummy link-local address
            // In practice, this should be extracted from the packet properly
            return "fe80::" + toHex(d).substr(0, 4);
        }
    }
    
    return "fe80::unknown";
}

// Extract router IP from relay headers or packet source
static std::string
extractRouterIp(const Pkt6Ptr& query) {
    // Try to get from relay agent information option
    OptionPtr relay_opt = query->getOption(D6O_RELAY_MSG);
    if (relay_opt) {
        // For relayed messages, try to get the relay agent address
        // This is a simplified approach - in practice you'd parse the relay message
        if (!query->getRemoteAddr().isV6Zero()) {
            return query->getRemoteAddr().toText();
        }
    }
    
    // Fallback: use packet source if it's not link-local
    if (query && !query->getRemoteAddr().isV6Zero()) {
        std::string remote_addr = query->getRemoteAddr().toText();
        if (remote_addr.find("fe80::") != 0) {
            return remote_addr;
        }
    }
    
    return "unknown";
}

// Build assignment data structure from lease and packet
static PdAssignmentData
buildAssignmentData(const Pkt6Ptr& query, const Lease6Ptr& lease) {
    PdAssignmentData data;
    
    data.client_duid = extractClientDuid(query);
    data.prefix = lease->addr_.toText();
    data.prefix_length = static_cast<int>(lease->prefixlen_);
    data.iaid = lease->iaid_;
    data.cpe_link_local = extractCpeLinkLocal(query);
    data.router_ip = extractRouterIp(query);
    
    return data;
}

// Build a minimal JSON payload for PD leases and send it.
static void
notifyPdAssigned(const Pkt6Ptr& query6,
                 const Pkt6Ptr& response6,
                 const Lease6CollectionPtr& leases6)
{
    if (!leases6 || leases6->empty()) {
        return;
    }

    // Collect PD leases only.
    std::vector<Lease6Ptr> pd_leases;
    for (const auto& l : *leases6) {
        if (!l) {
            continue;
        }
        if (l->type_ == Lease::TYPE_PD) {
            pd_leases.push_back(l);
        }
    }

    if (pd_leases.empty()) {
        return;
    }

    // Send original webhook notification if configured
    if (g_cfg.enabled && !g_cfg.url.empty()) {
        // Client DUID (from CLIENTID option, no parsing – just hex).
        std::string client_duid_hex;
        OptionPtr clientid_opt = query6->getOption(D6O_CLIENTID);
        if (clientid_opt) {
            const std::vector<uint8_t>& d = clientid_opt->getData();
            client_duid_hex = toHex(d);
        }

        // Build JSON manually. All fields we use are safe without escaping.
        std::ostringstream os;
        os << "{";
        os << "\"event\":\"pd_assigned\",";
        os << "\"msg_type\":" << static_cast<unsigned int>(query6->getType()) << ",";
        os << "\"reply_type\":" << static_cast<unsigned int>(response6->getType()) << ",";
        os << "\"client_duid\":\"" << client_duid_hex << "\",";
        os << "\"leases\":[";

        bool first = true;
        for (const auto& l : pd_leases) {
            if (!first) {
                os << ",";
            }
            first = false;

            os << "{";
            os << "\"prefix\":\"" << l->addr_.toText() << "\",";
            os << "\"prefix_length\":" << static_cast<unsigned int>(l->prefixlen_) << ",";
            os << "\"iaid\":" << l->iaid_ << ",";
            os << "\"subnet_id\":" << l->subnet_id_ << ",";
            os << "\"preferred_lft\":" << l->preferred_lft_ << ",";
            os << "\"valid_lft\":" << l->valid_lft_;
            os << "}";
        }

        os << "]";
        os << "}";

        postWebhook(os.str());
    }

    // NetBox integration - simplified fire-and-forget approach
    if (g_cfg.netbox_enabled) {
        for (const auto& l : pd_leases) {
            PdAssignmentData data = buildAssignmentData(query6, l);
            sendNetBoxRequest(data);
        }
    }
}

// Hook callout: leases6_committed
extern "C" {

int
leases6_committed(CalloutHandle& handle) {
    try {
        if (!g_cfg.enabled) {
            return (0);
        }

        Pkt6Ptr query6;
        Pkt6Ptr response6;
        Lease6CollectionPtr leases6;
        Lease6CollectionPtr deleted_leases6;

        handle.getArgument("query6", query6);
        handle.getArgument("response6", response6);
        handle.getArgument("leases6", leases6);
        handle.getArgument("deleted_leases6", deleted_leases6);

        if (!query6 || !response6 || !leases6) {
            return (0);
        }

        // Only trigger on REQUEST, and optionally SOLICIT+Rapid Commit.
        uint8_t msg_type = query6->getType();
        bool is_request = (msg_type == DHCPV6_REQUEST);

        bool is_rapid_commit =
            (msg_type == DHCPV6_SOLICIT) &&
            (query6->getOption(D6O_RAPID_COMMIT) != nullptr);

        if (!is_request && !is_rapid_commit) {
            return (0);
        }

        notifyPdAssigned(query6, response6, leases6);

    } catch (...) {
        // Do not throw into Kea; errors are silently ignored here.
    }

    return (0);
}

// Library load hook: read configuration parameters.
int
load(LibraryHandle& handle) {
    // Default.
    g_cfg = WebhookConfig();

    // Library parameters from kea config: hooks-libraries[].parameters
    ConstElementPtr params = handle.getParameters();
    if (params && params->getType() == Element::map) {
        // Original webhook configuration
        ConstElementPtr url_el = params->get("webhook-url");
        if (url_el && url_el->getType() == Element::string) {
            g_cfg.url = url_el->stringValue();
        }

        ConstElementPtr timeout_el = params->get("timeout-ms");
        if (timeout_el && timeout_el->getType() == Element::integer) {
            long t = static_cast<long>(timeout_el->intValue());
            if (t > 0) {
                g_cfg.timeout_ms = t;
            }
        }
        
        // NetBox configuration
        ConstElementPtr netbox_url_el = params->get("netbox-url");
        if (netbox_url_el && netbox_url_el->getType() == Element::string) {
            g_cfg.netbox_url = netbox_url_el->stringValue();
        }
        
        ConstElementPtr netbox_token_el = params->get("netbox-token");
        if (netbox_token_el && netbox_token_el->getType() == Element::string) {
            g_cfg.netbox_token = netbox_token_el->stringValue();
        }
        
        ConstElementPtr netbox_enabled_el = params->get("netbox-enabled");
        if (netbox_enabled_el && netbox_enabled_el->getType() == Element::boolean) {
            g_cfg.netbox_enabled = netbox_enabled_el->boolValue();
        }
    }

    g_cfg.enabled = !g_cfg.url.empty();
    g_cfg.netbox_enabled = g_cfg.netbox_enabled && !g_cfg.netbox_url.empty() && !g_cfg.netbox_token.empty();

    // Initialize libcurl once.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    return (0);
}

// Library unload hook.
int
unload() {
    curl_global_cleanup();
    g_cfg = WebhookConfig();
    return (0);
}

// Declare that this library is MT-safe (if you are sure).
int
multi_threading_compatible() {
    return (1);
}

} // extern "C"
