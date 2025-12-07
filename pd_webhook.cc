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

// Simple runtime configuration for this library.
struct WebhookConfig {
    std::string url;
    long timeout_ms{2000};
    bool enabled{false};
    bool debug{false};
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
        
        // Also try to dump the raw packet data if available
        // Since Kea decapsulates relay messages, let's dump the processed query packet
        // Build a representation of what the original relay packet might have looked like
        if (g_cfg.debug) {
            DEBUG_LOG("PD_WEBHOOK:     building relay packet representation:");
            
            std::vector<uint8_t> relay_packet;
            
            // Relay header: msg_type (1 byte) + hop_count (1 byte)
            relay_packet.push_back(relay.msg_type_);
            relay_packet.push_back(relay.hop_count_);
            
            // Link address (16 bytes for IPv6) - simplified
            const std::string& link_str = relay.linkaddr_.toText();
            // For hex dump purposes, we'll add a simplified representation
            for (int i = 0; i < 16; i++) {
                relay_packet.push_back(0x20); // placeholder
            }
            
            // Peer address (16 bytes for IPv6) - simplified  
            for (int i = 0; i < 16; i++) {
                relay_packet.push_back(0xfe); // placeholder
            }
            
            // Add all relay options
            for (const auto& opt_pair : relay.options_) {
                uint16_t opt_code = opt_pair.first;
                const std::vector<uint8_t>& opt_data = opt_pair.second->getData();
                uint16_t opt_len = opt_data.size();
                
                // Option code (2 bytes, network order)
                relay_packet.push_back((opt_code >> 8) & 0xFF);
                relay_packet.push_back(opt_code & 0xFF);
                // Option length (2 bytes, network order)
                relay_packet.push_back((opt_len >> 8) & 0xFF);
                relay_packet.push_back(opt_len & 0xFF);
                // Option data
                relay_packet.insert(relay_packet.end(), opt_data.begin(), opt_data.end());
            }
            
            // Add the encapsulated DHCPv6 message (simplified representation)
            // Message type: query->getType()
            relay_packet.push_back(query->getType());
            // Transaction ID (3 bytes)
            uint32_t transid = query->getTransid();
            relay_packet.push_back((transid >> 16) & 0xFF);
            relay_packet.push_back((transid >> 8) & 0xFF);
            relay_packet.push_back(transid & 0xFF);
            
            DEBUG_LOG("PD_WEBHOOK:     relay packet representation:");
            std::string hexdump = hexDump(relay_packet.data(), relay_packet.size());
            std::istringstream iss(hexdump);
            std::string line;
            while (std::getline(iss, line)) {
                DEBUG_LOG("PD_WEBHOOK:       " << line);
            }
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
        std::string relay_src_addr = "";
        if (!query6->relay_info_.empty()) {
            const Pkt6::RelayInfo& relay = query6->relay_info_[0];
            link_addr = relay.linkaddr_.toText();
            peer_addr = relay.peeraddr_.toText();
            // Get the actual source address of the relay packet (relay agent's IP)
            relay_src_addr = query6->getRemoteAddr().toText();
            DEBUG_LOG("PD_WEBHOOK: Found relay info - link_addr: " << link_addr << ", peer_addr: " << peer_addr << ", relay_src_addr: " << relay_src_addr);
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
        os << "\"relay_src_addr\":\"" << relay_src_addr << "\",";
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
    }

    g_cfg.enabled = !g_cfg.url.empty();

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
