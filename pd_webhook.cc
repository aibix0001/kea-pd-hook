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

// Simple runtime configuration for this library.
struct WebhookConfig {
    std::string url;
    long timeout_ms{2000};
    bool enabled{false};
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

} // extern "C"
