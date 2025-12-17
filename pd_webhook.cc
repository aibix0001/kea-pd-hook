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
#include <ctime>

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
    std::string router_link_addr;    // Router's link-address from relay packet
};

// Simple runtime configuration for this library.
struct WebhookConfig {
    std::string url;
    long timeout_ms{2000};
    bool enabled{false};
    bool debug{false};

    // NetBox API configuration
    std::string netbox_url;
    std::string netbox_token;
    bool netbox_enabled{false};
};

static WebhookConfig g_cfg;

// Debug logging macro
#define DEBUG_LOG(msg) do { if (g_cfg.debug) std::cout << msg << std::endl; } while(0)

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

// Hex dump helper in the specified format
static std::string
hexDump(const uint8_t* data, size_t len, size_t offset = 0) {
    std::ostringstream os;
    for (size_t i = 0; i < len; i += 16) {
        // Offset column (4 hex digits, no leading 0x)
        os << std::hex << std::setw(4) << std::setfill('0') << (offset + i) << "   ";
        
        // Hex bytes with spaces between each byte
        for (size_t j = 0; j < 16 && i + j < len; ++j) {
            os << std::hex << std::setw(2) << std::setfill('0') 
               << static_cast<unsigned int>(data[i + j]) << " ";
        }
        
        os << "\n";
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
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

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
updatePrefix(int prefix_id, const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft) {
    std::string endpoint = "ipam/prefixes/" + std::to_string(prefix_id) + "/";

    // Calculate expiration timestamp (current time + valid lifetime)
    time_t now = time(nullptr);
    time_t expires_at = now + valid_lft;

    std::ostringstream payload;
    payload << "{";
    payload << "\"status\":\"active\",";
    payload << "\"description\":\"DHCPv6 PD assignment - IAID: " << data.iaid << "\",";
    payload << "\"custom_fields\":{";
    payload << "\"dhcpv6_client_duid\":\"" << data.client_duid << "\",";
    payload << "\"dhcpv6_iaid\":" << data.iaid << ",";
    payload << "\"dhcpv6_cpe_link_local\":\"" << data.cpe_link_local << "\",";
    payload << "\"dhcpv6_router_ip\":\"" << data.router_ip << "\",";
    payload << "\"dhcpv6_router_link_addr\":\"" << data.router_link_addr << "\",";
    payload << "\"dhcpv6_leasetime\":" << expires_at;
    payload << "}";
    payload << "}";

    std::string payload_str = payload.str();
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

// Create new prefix in NetBox
static bool
createPrefix(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft) {
    // Calculate expiration timestamp (current time + valid lifetime)
    time_t now = time(nullptr);
    time_t expires_at = now + valid_lft;

    std::ostringstream payload;
    payload << "{";
    payload << "\"prefix\":\"" << data.prefix << "/" << data.prefix_length << "\",";
    payload << "\"status\":\"active\",";
    payload << "\"description\":\"DHCPv6 PD assignment - IAID: " << data.iaid << "\",";
    payload << "\"custom_fields\":{";
    payload << "\"dhcpv6_client_duid\":\"" << data.client_duid << "\",";
    payload << "\"dhcpv6_iaid\":" << data.iaid << ",";
    payload << "\"dhcpv6_cpe_link_local\":\"" << data.cpe_link_local << "\",";
    payload << "\"dhcpv6_router_ip\":\"" << data.router_ip << "\",";
    payload << "\"dhcpv6_router_link_addr\":\"" << data.router_link_addr << "\",";
    payload << "\"dhcpv6_leasetime\":" << expires_at;
    payload << "}";
    payload << "}";

    std::string payload_str = payload.str();
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

// Send request to NetBox API with check-then-create-or-update logic
static void
sendNetBoxRequest(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft) {
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
        updatePrefix(existing_prefix_id, data, valid_lft, preferred_lft);
    } else {
        // Create new prefix
        createPrefix(data, valid_lft, preferred_lft);
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

// Extract CPE link-local address from DHCPv6 relay message peer-address
static std::string
extractCpeLinkLocal(const Pkt6Ptr& query) {
    DEBUG_LOG("PD_WEBHOOK: >>> extractCpeLinkLocal() called");
    std::string peer_address = "";

    // First, try to extract peer-address from relay-forward message
    if (query) {
        DEBUG_LOG("PD_WEBHOOK: extractCpeLinkLocal - query is valid");

        // Check if this packet itself is a relay message (RELAY-FORWARD or RELAY-REPLY)
        uint8_t msg_type = query->getType();
        DEBUG_LOG("PD_WEBHOOK: Message type: " << static_cast<int>(msg_type)
                  << " (REQUEST=" << static_cast<int>(DHCPV6_REQUEST)
                  << ", RELAY_FORW=" << static_cast<int>(DHCPV6_RELAY_FORW)
                  << ", RELAY_REPL=" << static_cast<int>(DHCPV6_RELAY_REPL) << ")");

        // Check if Kea has stored relay information for this packet
        // Kea typically stores relay info in the packet's metadata
        try {
            // Check if the packet has relay information stored
            if (query->relay_info_.size() > 0) {
                DEBUG_LOG("PD_WEBHOOK: Found " << query->relay_info_.size() << " relay info entries");

                // Try to access the correct fields in RelayInfo structure
                const auto& relay = query->relay_info_[0];

                // Try to access peer_addr_ field (this should contain the CPE's link-local address)
                try {
                    // Use direct field access - let's try the correct field names
                    // Based on Kea source, RelayInfo should have: msg_type_, hop_count_, link_addr_, peer_addr_
                    if (sizeof(relay) >= 64) { // Rough check that structure has expected fields
                        DEBUG_LOG("PD_WEBHOOK: RelayInfo structure appears to have expected fields");

                        // Dump entire RelayInfo structure to see what Kea actually stores
                        const uint8_t* relay_bytes = reinterpret_cast<const uint8_t*>(&relay);

                        DEBUG_LOG("PD_WEBHOOK: Full RelayInfo dump (" << sizeof(relay) << " bytes):");
                        for (size_t i = 0; i < sizeof(relay); i++) {
                            if (g_cfg.debug) {
                                std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(relay_bytes[i]);
                                if ((i + 1) % 16 == 0) {
                                    std::cout << std::endl;
                                } else {
                                    std::cout << " ";
                                }
                            }
                        }
                        if (g_cfg.debug) std::cout << std::dec << std::endl;

                        // Look for fe80 pattern (start of link-local address)
                        for (size_t i = 0; i < sizeof(relay) - 1; i++) {
                            if (relay_bytes[i] == 0xfe && relay_bytes[i+1] == 0x80) {
                                DEBUG_LOG("PD_WEBHOOK: Found fe80 pattern at offset " << i);

                                // Extract 16 bytes from this position
                                if (i + 16 <= sizeof(relay)) {
                                    std::ostringstream peer_stream;
                                    peer_stream << std::hex << std::setfill('0');

                                    for (int j = 0; j < 16; j++) {
                                        if (j == 0) {
                                            peer_stream << static_cast<unsigned int>(relay_bytes[i + j]);
                                        } else if (j % 2 == 0) {
                                            peer_stream << ":" << std::setw(2) << static_cast<unsigned int>(relay_bytes[i + j]);
                                        } else {
                                            peer_stream << std::setw(2) << static_cast<unsigned int>(relay_bytes[i + j]);
                                        }
                                    }

                                    std::string extracted_peer = peer_stream.str();
                                    DEBUG_LOG("PD_WEBHOOK: Extracted peer address: " << extracted_peer);

                                    if (extracted_peer.find("fe80") == 0) {
                                        // Use Kea's IOAddress to properly compress IPv6 address
                                        try {
                                            isc::asiolink::IOAddress addr(extracted_peer);
                                            peer_address = addr.toText();
                                            DEBUG_LOG("PD_WEBHOOK: Using compressed peer address: " << peer_address);
                                            return peer_address;
                                        } catch (...) {
                                            // Fallback to original address if compression fails
                                            peer_address = extracted_peer;
                                            DEBUG_LOG("PD_WEBHOOK: Using uncompressed peer address: " << peer_address);
                                            return peer_address;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {
                    DEBUG_LOG("PD_WEBHOOK: Failed to extract peer address from RelayInfo");
                }

                // If we found a peer address, return it
                if (!peer_address.empty() && peer_address.find("fe80::") == 0) {
                    return peer_address;
                }
            }
        } catch (...) {
            DEBUG_LOG("PD_WEBHOOK: Failed to access relay_info");
        }
        
        // If we found a peer address, return it
        if (!peer_address.empty() && peer_address.find("fe80::") == 0) {
            return peer_address;
        }
    }
    
    // Return empty string if no link-local address found
    return "";
}

// Extract router IP from relay information
static std::string
extractRouterIp(const Pkt6Ptr& query) {
    if (!query || query->relay_info_.empty()) {
        return "";
    }
    
    // Return the remote address (source of the relay packet)
    return query->getRemoteAddr().toText();
}

// Extract router link address from relay information
static std::string
extractRouterLinkAddr(const Pkt6Ptr& query) {
    if (!query || query->relay_info_.empty()) {
        return "";
    }
    
    // Return the link address from the first relay
    const Pkt6::RelayInfo& relay = query->relay_info_[0];
    return relay.linkaddr_.toText();
}

// Dump relay information for debugging
static void
dumpRelayInfo(const Pkt6Ptr& query) {
    if (!query) {
        return;
    }
    
    // Check if this is a relayed message
    if (query->relay_info_.empty()) {
        DEBUG_LOG("PD_WEBHOOK: No relay information (direct message)");
        return;
    }
    
    DEBUG_LOG("PD_WEBHOOK: Relay information - " << query->relay_info_.size() << " relay(s):");
    
    for (size_t i = 0; i < query->relay_info_.size(); ++i) {
        const Pkt6::RelayInfo& relay = query->relay_info_[i];
        DEBUG_LOG("PD_WEBHOOK:   Relay " << i << ":");
        DEBUG_LOG("PD_WEBHOOK:     msg_type: " << static_cast<unsigned int>(relay.msg_type_));
        DEBUG_LOG("PD_WEBHOOK:     hop_count: " << static_cast<unsigned int>(relay.hop_count_));
        DEBUG_LOG("PD_WEBHOOK:     link_addr: " << relay.linkaddr_.toText());
        DEBUG_LOG("PD_WEBHOOK:     peer_addr: " << relay.peeraddr_.toText());
        
        // Dump interface-id option if present
        auto interface_id_it = relay.options_.find(D6O_INTERFACE_ID);
        if (interface_id_it != relay.options_.end()) {
            const std::vector<uint8_t>& data = interface_id_it->second->getData();
            DEBUG_LOG("PD_WEBHOOK:     interface-id: " << toHex(data));
        }
        
        // Dump relay-message option if present
        auto relay_msg_it = relay.options_.find(D6O_RELAY_MSG);
        if (relay_msg_it != relay.options_.end()) {
            const std::vector<uint8_t>& relay_msg_data = relay_msg_it->second->getData();
            DEBUG_LOG("PD_WEBHOOK:     relay-msg: present (" << relay_msg_data.size() << " bytes)");
            
            // Hex dump the relay message data (contains encapsulated DHCPv6 message)
            if (g_cfg.debug && !relay_msg_data.empty()) {
                DEBUG_LOG("PD_WEBHOOK:     relay_msg hex dump:");
                std::string hexdump = hexDump(relay_msg_data.data(), relay_msg_data.size());
                std::istringstream iss(hexdump);
                std::string line;
                while (std::getline(iss, line)) {
                    DEBUG_LOG("PD_WEBHOOK:       " << line);
                }
            }
        } else {
            DEBUG_LOG("PD_WEBHOOK:     relay-msg: not found in relay options");
        }
        
        // Dump all options in this relay level
        DEBUG_LOG("PD_WEBHOOK:     options count: " << relay.options_.size());
        for (const auto& opt_pair : relay.options_) {
            const std::vector<uint8_t>& opt_data = opt_pair.second->getData();
            DEBUG_LOG("PD_WEBHOOK:       option " << static_cast<unsigned int>(opt_pair.first) 
                     << ": " << opt_data.size() << " bytes");
            
            // Hex dump the option data if debug is enabled
            if (g_cfg.debug && !opt_data.empty()) {
                std::string hexdump = hexDump(opt_data.data(), opt_data.size());
                std::istringstream iss(hexdump);
                std::string line;
                while (std::getline(iss, line)) {
                    DEBUG_LOG("PD_WEBHOOK:         " << line);
                }
            }
        }
    }
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
    DEBUG_LOG("PD_WEBHOOK: Processing " << leases6->size() << " total leases");
    
    for (const auto& l : *leases6) {
        if (!l) {
            continue;
        }
        DEBUG_LOG("PD_WEBHOOK: Lease type: " << static_cast<int>(l->type_) 
                  << " (NA=" << Lease::TYPE_NA 
                  << ", TA=" << Lease::TYPE_TA 
                  << ", PD=" << Lease::TYPE_PD << ")"
                  << " address: " << l->addr_.toText());
        
        if (l->type_ == Lease::TYPE_PD) {
            pd_leases.push_back(l);
        }
    }

    if (pd_leases.empty()) {
        DEBUG_LOG("PD_WEBHOOK: No PD leases found, returning");
        return;
    }
    
    DEBUG_LOG("PD_WEBHOOK: found " << pd_leases.size() << " PD leases");

    // Send webhook notification if configured
    if (g_cfg.enabled && !g_cfg.url.empty()) {
        // Client DUID (from CLIENTID option, no parsing – just hex).
        std::string client_duid_hex;
        OptionPtr clientid_opt = query6->getOption(D6O_CLIENTID);
        if (clientid_opt) {
            const std::vector<uint8_t>& d = clientid_opt->getData();
            client_duid_hex = toHex(d);
        }

        // Extract relay information for webhook
        std::string link_addr = "";
        std::string peer_addr = "";
        if (!query6->relay_info_.empty()) {
            const Pkt6::RelayInfo& relay = query6->relay_info_[0];
            link_addr = relay.linkaddr_.toText();
            peer_addr = relay.peeraddr_.toText();
            DEBUG_LOG("PD_WEBHOOK: Found relay info - link_addr: " << link_addr << ", peer_addr: " << peer_addr);
        } else {
            DEBUG_LOG("PD_WEBHOOK: No relay information found (direct message)");
        }

        // Build JSON manually. All fields we use are safe without escaping.
        std::ostringstream os;
        os << "{";
        os << "\"event\":\"pd_assigned\",";
        os << "\"msg_type\":" << static_cast<unsigned int>(query6->getType()) << ",";
        os << "\"reply_type\":" << static_cast<unsigned int>(response6->getType()) << ",";
        os << "\"client_duid\":\"" << client_duid_hex << "\",";
        os << "\"link_addr\":\"" << link_addr << "\",";
        os << "\"peer_addr\":\"" << peer_addr << "\",";
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

    // Send NetBox requests for each PD lease
    for (const auto& l : pd_leases) {
        PdAssignmentData data;
        data.client_duid = extractClientDuid(query6);
        data.prefix = l->addr_.toText();
        data.prefix_length = l->prefixlen_;
        data.iaid = l->iaid_;
        data.cpe_link_local = extractCpeLinkLocal(query6);
        data.router_ip = extractRouterIp(query6);
        data.router_link_addr = extractRouterLinkAddr(query6);

        DEBUG_LOG("PD_WEBHOOK: Sending NetBox request for prefix " << data.prefix << "/" << data.prefix_length
                  << " (IAID=" << data.iaid << ", CPE=" << data.cpe_link_local
                  << ", Router=" << data.router_ip << ", LinkAddr=" << data.router_link_addr << ")");

        sendNetBoxRequest(data, l->valid_lft_, l->preferred_lft_);
    }
}

// Hook callout: leases6_committed
extern "C" {

int
leases6_committed(CalloutHandle& handle) {
    try {
        // Always log that hook was called
        DEBUG_LOG("PD_WEBHOOK: leases6_committed called");
        
        if (!g_cfg.enabled) {
            DEBUG_LOG("PD_WEBHOOK: hook disabled, returning");
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

        // Trigger on REQUEST, RENEW (for testing with short leases), and SOLICIT+Rapid Commit.
        uint8_t msg_type = query6->getType();
        bool is_request = (msg_type == DHCPV6_REQUEST);
        bool is_renew = (msg_type == DHCPV6_RENEW);

        bool is_rapid_commit =
            (msg_type == DHCPV6_SOLICIT) &&
            (query6->getOption(D6O_RAPID_COMMIT) != nullptr);

        if (!is_request && !is_renew && !is_rapid_commit) {
            DEBUG_LOG("PD_WEBHOOK: Skipping message type: " << static_cast<unsigned int>(msg_type) << " (REQUEST=3, SOLICIT=1, RENEW=5, RELAY_FORW=12, RELAY_REPL=13)");
            return (0);
        }

        // Dump relay information for debugging
        dumpRelayInfo(query6);
        
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
        // Webhook configuration
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
        
        ConstElementPtr debug_el = params->get("debug");
        if (debug_el && debug_el->getType() == Element::boolean) {
            g_cfg.debug = debug_el->boolValue();
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
    }

    g_cfg.enabled = !g_cfg.url.empty();
    g_cfg.netbox_enabled = !g_cfg.netbox_url.empty() && !g_cfg.netbox_token.empty();

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

// Return the version number.
int
version() {
    return (KEA_HOOKS_VERSION);
}

} // extern "C"
