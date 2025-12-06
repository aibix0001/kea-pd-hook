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
        
        // Check if this packet itself is a relay message
        if (msg_type == DHCPV6_RELAY_FORW || msg_type == DHCPV6_RELAY_REPL) {
            DEBUG_LOG("PD_WEBHOOK: This is a relay message, extracting peer-address");
            
            // For relay messages, try to get from remote address for relayed packets
            if (!query->getRemoteAddr().isV6Zero()) {
                peer_address = query->getRemoteAddr().toText();
                DEBUG_LOG("PD_WEBHOOK: Using remote address as peer: " << peer_address);
                
                if (peer_address.find("fe80::") == 0) {
                    return peer_address;
                }
            }
        } else {
            DEBUG_LOG("PD_WEBHOOK: Not a relay message, checking for embedded relay info");
            
            // Check if this is a regular message that contains relay information
            OptionPtr relay_msg_opt = query->getOption(D6O_RELAY_MSG);
            if (relay_msg_opt) {
                DEBUG_LOG("PD_WEBHOOK: Found RELAY_MSG option, parsing relay message");
                
                const std::vector<uint8_t>& relay_data = relay_msg_opt->getData();
                DEBUG_LOG("PD_WEBHOOK: RELAY_MSG data size: " << relay_data.size());
                
                if (relay_data.size() >= 34) { // Minimum size for relay message header
                    // Extract peer-address (bytes 18-33)
                    std::ostringstream peer_addr_stream;
                    peer_addr_stream << std::hex << std::setfill('0');
                    
                    for (int i = 18; i < 34; i++) {
                        if (i == 18) {
                            peer_addr_stream << std::hex << static_cast<unsigned int>(relay_data[i]);
                        } else if (i % 2 == 0) {
                            peer_addr_stream << ":" << std::setw(2) << static_cast<unsigned int>(relay_data[i]);
                        } else {
                            peer_addr_stream << std::setw(2) << static_cast<unsigned int>(relay_data[i]);
                        }
                    }
                    
                    peer_address = peer_addr_stream.str();
                    DEBUG_LOG("PD_WEBHOOK: Extracted peer-address from RELAY_MSG: " << peer_address);
                    
                    // Check if it's a link-local address
                    if (peer_address.find("fe80") == 0) {
                        return peer_address;
                    }
                }
            }
        }
        
        // Log packet addresses for comparison
        if (!query->getRemoteAddr().isV6Zero()) {
            DEBUG_LOG("PD_WEBHOOK: Packet remote address: " << query->getRemoteAddr().toText());
        }
        if (!query->getLocalAddr().isV6Zero()) {
            DEBUG_LOG("PD_WEBHOOK: Packet local address: " << query->getLocalAddr().toText());
        }
    }
    
    // Fallback: try to extract link-layer address from CLIENTID option
    OptionPtr clientid_opt = query->getOption(D6O_CLIENTID);
    if (clientid_opt) {
        const std::vector<uint8_t>& data = clientid_opt->getData();
        
        // Debug: print the raw DUID data
        if (g_cfg.debug) {
            std::cout << "PD_WEBHOOK: DUID raw data: ";
            for (size_t i = 0; i < data.size(); i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(data[i]);
            }
            std::cout << std::dec << std::endl;
        }
        
        // Client ID format varies by DUID type:
        // DUID-LLT: Type(2) + HWType(2) + Time(4) + MAC(6)
        // DUID-LL:  Type(2) + HWType(2) + MAC(6)
        // DUID-UUID: Type(2) + UUID(16)
        
        if (data.size() >= 8) {
            // Check DUID type (1 = DUID-LLT, 3 = DUID-LL, 4 = DUID-UUID)
            uint16_t duid_type = (static_cast<uint16_t>(data[0]) << 8) | data[1];
            DEBUG_LOG("PD_WEBHOOK: DUID type: " << duid_type);
            
            if (duid_type == 1 || duid_type == 3) {  // DUID-LLT or DUID-LL
                // Extract hardware type
                uint16_t hw_type = (static_cast<uint16_t>(data[2]) << 8) | data[3];
                DEBUG_LOG("PD_WEBHOOK: Hardware type: " << hw_type);
                
                // For Ethernet (type 1), extract 6-byte MAC
                if (hw_type == 1) {
                    size_t mac_offset = (duid_type == 1) ? 8 : 4; // DUID-LLT has 4-byte time field
                    
                    if (data.size() >= mac_offset + 6) {
                        // Extract MAC bytes
                        uint8_t mac[6];
                        for (int i = 0; i < 6; i++) {
                            mac[i] = data[mac_offset + i];
                        }
                        
                        DEBUG_LOG("PD_WEBHOOK: Extracted MAC: " 
                                << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(mac[0]) << ":"
                                << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(mac[1]) << ":"
                                << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(mac[2]) << ":"
                                << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(mac[3]) << ":"
                                << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(mac[4]) << ":"
                                << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(mac[5]));
                        
                        // Build EUI-64 identifier from MAC
                        std::ostringstream link_local;
                        link_local << "fe80::";
                        
                        // Convert MAC to EUI-64: flip 7th bit of first byte
                        mac[0] ^= 0x02;
                        
                        // Format as IPv6 address: xxxx:xxff:feXX:xxxx
                        link_local << std::hex << std::setfill('0');
                        link_local << std::setw(2) << static_cast<unsigned int>(mac[0]) << std::setw(2) << static_cast<unsigned int>(mac[1]);
                        link_local << ":" << std::setw(2) << static_cast<unsigned int>(mac[2]) << "ff";
                        link_local << ":fe" << std::setw(2) << static_cast<unsigned int>(mac[3]);
                        link_local << ":" << std::setw(2) << static_cast<unsigned int>(mac[4]) << std::setw(2) << static_cast<unsigned int>(mac[5]);
                        
                        std::string duid_link_local = link_local.str();
                        DEBUG_LOG("PD_WEBHOOK: Generated link-local from DUID: " << duid_link_local);
                        
                        // If we didn't get a good peer address, use the DUID-based one
                        if (peer_address.empty() || peer_address.find("fe80") != 0) {
                            return duid_link_local;
                        }
                    }
                }
            }
        }
    }
    
    // If we have a good peer address, use it
    if (!peer_address.empty()) {
        return peer_address;
    }
    
    // Final fallback: try to get from packet source
    if (query && !query->getRemoteAddr().isV6Zero()) {
        std::string remote_addr = query->getRemoteAddr().toText();
        if (remote_addr.find("fe80::") == 0) {
            return remote_addr;
        }
    }
    
    return "unknown";
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

// Extract router's link-address from relay packet (16:31 offset)
static std::string
extractRouterLinkAddr(const Pkt6Ptr& query) {
    std::cout << "PD_WEBHOOK: >>> extractRouterLinkAddr() called" << std::endl;
    DEBUG_LOG("PD_WEBHOOK: >>> extractRouterLinkAddr() called");
    
    if (!query) {
        std::cout << "PD_WEBHOOK: query is null, returning 'unknown'" << std::endl;
        DEBUG_LOG("PD_WEBHOOK: query is null, returning 'unknown'");
        return "unknown";
    }
    
    DEBUG_LOG("PD_WEBHOOK: query->relay_info_.size() = " << query->relay_info_.size());
    
    // Check if Kea has stored relay information for this packet
    try {
        if (query->relay_info_.size() > 0) {
            DEBUG_LOG("PD_WEBHOOK: Found " << query->relay_info_.size() << " relay info entries");
            
            const auto& relay = query->relay_info_[0];
            
                // Try to access link_addr_ field from RelayInfo structure
                // Use pattern-based search similar to peer-address extraction
                try {
                    const uint8_t* relay_bytes = reinterpret_cast<const uint8_t*>(&relay);
                    
                    DEBUG_LOG("PD_WEBHOOK: Searching for router link-address pattern in RelayInfo structure");
                    
                    // Look for global unicast IPv6 addresses (not starting with fe80 or ff)
                    // Router link-address should be a global address, typically starting with 2xxx
                    for (size_t i = 0; i < sizeof(relay) - 1; i++) {
                        // Look for patterns that could be global unicast addresses
                        // Global unicast addresses start with 2xxx or 3xxx in hex
                        if ((relay_bytes[i] == 0x20 || relay_bytes[i] == 0x30 || relay_bytes[i] == 0x2) && 
                            i + 16 <= sizeof(relay)) {
                            
                            // Skip if this looks like the peer-address (fe80 pattern nearby)
                            bool is_near_fe80 = false;
                            for (int check = -10; check <= 10; check++) {
                                if (i + check >= 0 && i + check + 1 < sizeof(relay)) {
                                    if (relay_bytes[i + check] == 0xfe && relay_bytes[i + check + 1] == 0x80) {
                                        is_near_fe80 = true;
                                        break;
                                    }
                                }
                            }
                            
                            if (is_near_fe80) {
                                continue; // Skip, this is probably near the peer-address
                            }
                            
                            // Extract potential IPv6 address
                            std::ostringstream addr_stream;
                            addr_stream << std::hex << std::setfill('0');
                            
                            for (int j = 0; j < 16; j++) {
                                if (j == 0) {
                                    addr_stream << std::hex << static_cast<unsigned int>(relay_bytes[i + j]);
                                } else if (j % 2 == 0) {
                                    addr_stream << ":" << std::setw(2) << static_cast<unsigned int>(relay_bytes[i + j]);
                                } else {
                                    addr_stream << std::setw(2) << static_cast<unsigned int>(relay_bytes[i + j]);
                                }
                            }
                            
                            std::string extracted_addr = addr_stream.str();
                            DEBUG_LOG("PD_WEBHOOK: Found potential router link-address at offset " << i << ": " << extracted_addr);
                            
                            // Validate and format the address
                            try {
                                isc::asiolink::IOAddress addr(extracted_addr);
                                std::string formatted_addr = addr.toText();
                                
                                // Only accept global unicast addresses (not link-local, multicast, or unspecified)
                                if (formatted_addr.find("fe80::") != 0 && 
                                    formatted_addr != "::" && 
                                    formatted_addr.find("ff") != 0 &&
                                    formatted_addr.find("2001:") == 0) { // Look for 2001:/32 addresses
                                    DEBUG_LOG("PD_WEBHOOK: Using router link-address: " << formatted_addr);
                                    return formatted_addr;
                                }
                            } catch (...) {
                                // Invalid address, continue searching
                            }
                        }
                    }
                
                // Alternative approach: try to extract from RELAY_MSG option
                OptionPtr relay_msg_opt = query->getOption(D6O_RELAY_MSG);
                if (relay_msg_opt) {
                    DEBUG_LOG("PD_WEBHOOK: Found RELAY_MSG option, extracting link-address");
                    
                    const std::vector<uint8_t>& relay_data = relay_msg_opt->getData();
                    DEBUG_LOG("PD_WEBHOOK: RELAY_MSG data size: " << relay_data.size());
                    
                    if (relay_data.size() >= 34) { // Minimum size for link-address extraction (16+16)
                        // Extract link-address (bytes 16-31 in relay message header)
                        std::ostringstream link_addr_stream;
                        link_addr_stream << std::hex << std::setfill('0');
                        
                        for (int i = 16; i < 32; i++) {
                            if (i == 16) {
                                link_addr_stream << std::hex << static_cast<unsigned int>(relay_data[i]);
                            } else if (i % 2 == 0) {
                                link_addr_stream << ":" << std::setw(2) << static_cast<unsigned int>(relay_data[i]);
                            } else {
                                link_addr_stream << std::setw(2) << static_cast<unsigned int>(relay_data[i]);
                            }
                        }
                        
                        std::string link_address = link_addr_stream.str();
                        DEBUG_LOG("PD_WEBHOOK: Extracted link-address from RELAY_MSG: " << link_address);
                        
                        // Validate and format the address
                        try {
                            isc::asiolink::IOAddress addr(link_address);
                            std::string formatted_addr = addr.toText();
                            
                            // Skip link-local addresses (those are usually CPE addresses)
                            if (formatted_addr.find("fe80::") != 0 && 
                                formatted_addr != "::" && 
                                formatted_addr.find("ff") != 0) {
                                DEBUG_LOG("PD_WEBHOOK: Using router link-address from RELAY_MSG: " << formatted_addr);
                                return formatted_addr;
                            }
                        } catch (...) {
                            DEBUG_LOG("PD_WEBHOOK: Failed to parse link-address from RELAY_MSG");
                        }
                    }
                }
                
            } catch (...) {
                DEBUG_LOG("PD_WEBHOOK: Failed to extract link-address from RelayInfo");
            }
        }
    } catch (...) {
        DEBUG_LOG("PD_WEBHOOK: Failed to access relay_info for link-address extraction");
    }
    
    // Log packet addresses for debugging
    if (!query->getLocalAddr().isV6Zero()) {
        DEBUG_LOG("PD_WEBHOOK: Packet local address: " << query->getLocalAddr().toText());
    }
    if (!query->getRemoteAddr().isV6Zero()) {
        DEBUG_LOG("PD_WEBHOOK: Packet remote address: " << query->getRemoteAddr().toText());
    }
    
    // Fallback: try to use packet's local address (might be router address)
    if (!query->getLocalAddr().isV6Zero()) {
        std::string local_addr = query->getLocalAddr().toText();
        if (local_addr.find("fe80::") != 0 && local_addr != "::") {
            DEBUG_LOG("PD_WEBHOOK: Using packet local address as router link-address: " << local_addr);
            return local_addr;
        }
    }
    
    DEBUG_LOG("PD_WEBHOOK: Could not extract router link-address, returning 'unknown'");
    return "unknown";
}

// Build assignment data structure from lease and packet
static PdAssignmentData
buildAssignmentData(const Pkt6Ptr& query, const Lease6Ptr& lease) {
    std::cout << "PD_WEBHOOK: >>> buildAssignmentData() called" << std::endl;
    DEBUG_LOG("PD_WEBHOOK: >>> buildAssignmentData() called");
    PdAssignmentData data;
    
    data.client_duid = extractClientDuid(query);
    data.prefix = lease->addr_.toText();
    data.prefix_length = static_cast<int>(lease->prefixlen_);
    data.iaid = lease->iaid_;
    data.cpe_link_local = extractCpeLinkLocal(query);
    data.router_ip = extractRouterIp(query);
    data.router_link_addr = extractRouterLinkAddr(query);
    
    DEBUG_LOG("PD_WEBHOOK: buildAssignmentData - router_link_addr = '" << data.router_link_addr << "'");
    
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
    
    DEBUG_LOG("PD_WEBHOOK: found " << pd_leases.size() << " PD leases");

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
        DEBUG_LOG("PD_WEBHOOK: NetBox enabled, processing " << pd_leases.size() << " PD leases");
        for (const auto& l : pd_leases) {
            PdAssignmentData data = buildAssignmentData(query6, l);
            DEBUG_LOG("PD_WEBHOOK: Sending NetBox request for prefix " << data.prefix << "/" << data.prefix_length);
            sendNetBoxRequest(data, l->valid_lft_, l->preferred_lft_);
        }
    } else {
        DEBUG_LOG("PD_WEBHOOK: NetBox disabled");
    }
}

// Hook callout: lease6_renew - handle lease renewals to update expiration time
extern "C" {

int
lease6_renew(CalloutHandle& handle) {
    try {
        DEBUG_LOG("PD_WEBHOOK: lease6_renew called");
        
        if (!g_cfg.netbox_enabled) {
            DEBUG_LOG("PD_WEBHOOK: NetBox disabled, returning");
            return (0);
        }

        Lease6Ptr lease6;
        Pkt6Ptr query6;

        handle.getArgument("lease6", lease6);
        handle.getArgument("query6", query6);

        if (!lease6 || !query6) {
            return (0);
        }

        // Only process PD leases
        if (lease6->type_ != Lease::TYPE_PD) {
            return (0);
        }

        DEBUG_LOG("PD_WEBHOOK: Processing PD lease renewal for " << lease6->addr_.toText() 
                  << "/" << static_cast<int>(lease6->prefixlen_));

        // Build assignment data
        DEBUG_LOG("PD_WEBHOOK: About to call buildAssignmentData for lease renewal");
        PdAssignmentData data = buildAssignmentData(query6, lease6);
        
        // Update NetBox with new lease expiration time
        sendNetBoxRequest(data, lease6->valid_lft_, lease6->preferred_lft_);

    } catch (...) {
        // Do not throw into Kea; errors are silently ignored here.
    }

    return (0);
}

// Hook callout: leases6_committed
int
leases6_committed(CalloutHandle& handle) {
    try {
        // Always log that hook was called
        DEBUG_LOG("PD_WEBHOOK: leases6_committed called");
        
        DEBUG_LOG("PD_WEBHOOK: g_cfg.enabled=" << g_cfg.enabled << ", g_cfg.netbox_enabled=" << g_cfg.netbox_enabled);
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
        
        ConstElementPtr debug_el = params->get("debug");
        if (debug_el && debug_el->getType() == Element::boolean) {
            g_cfg.debug = debug_el->boolValue();
        }
    }

    g_cfg.enabled = !g_cfg.url.empty() || g_cfg.netbox_enabled;
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

// Return the version number.
int
version() {
    return (KEA_HOOKS_VERSION);
}

} // extern "C"
