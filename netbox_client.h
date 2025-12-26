#ifndef NETBOX_CLIENT_H
#define NETBOX_CLIENT_H

#include <string>
#include <vector>
#include <memory>

// Forward declarations
struct PdAssignmentData;

// Forward declarations for Kea types (typedefs defined elsewhere)
namespace isc {
namespace dhcp {
    class Pkt6;
    class Lease6;
}
}

// NetBox client interface for HTTP operations
class INetBoxClient {
public:
    virtual ~INetBoxClient() = default;

    // HTTP request methods
    virtual std::string postWebhook(const std::string& payload) = 0;
    virtual std::string netboxHttpRequest(const std::string& method, const std::string& endpoint, const std::string& payload = "") = 0;

    // NetBox API operations
    virtual int findPrefixId(const std::string& prefix, int prefix_length) = 0;
    virtual bool createPrefix(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft) = 0;
    virtual bool updatePrefix(int prefix_id, const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft, const std::string& status = "active") = 0;
    virtual bool updateExpiredPrefix(int prefix_id, const PdAssignmentData& data) = 0;

    // High-level operations
    virtual void sendNetBoxRequest(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft) = 0;
};

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

// Factory function to create NetBox client
std::unique_ptr<INetBoxClient> createNetBoxClient();

#endif // NETBOX_CLIENT_H