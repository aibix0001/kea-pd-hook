# Kea DHCPv6 PD Hook - NetBox Integration Plan

## Project Overview

This document outlines the simplified implementation plan for a Kea DHCPv6 PD hook that sends IA-PD assignment data to NetBox API.

## Requirements

### Core Requirement
- **Hook on IA-PD assignments** and construct API call to NetBox
- **Fire-and-forget approach** - no response processing needed
- **Minimal complexity** - extract only required data

### Current Environment
- **VyOS VM routers** with basic relay agents
- **Unknown CPE types** (maximum flexibility required)
- **Lab environment** (minimal validation needed)

### Data Flow
```
DHCPv6 IA-PD Assignment → Extract Data → Single NetBox API POST → Fire-and-Forget
```

## Implementation Plan

### Phase 1: Data Extraction

**Data to Extract from DHCPv6 IA-PD Leases:**
1. **Client DUID** - From client identifier option (D6O_CLIENTID)
2. **Assigned Prefix** - From lease (network + prefix length)
3. **IAID** - From lease for correlation
4. **CPE Link-Local Address** - From client identifier option or packet source
5. **Router IP** - From DHCPv6 relay message headers or packet source

**Data Structure:**
```cpp
struct PdAssignmentData {
    std::string client_duid;        // Client DUID (hex encoded)
    std::string prefix;              // Assigned prefix (e.g., "2001:db8:56::")
    int prefix_length;               // Prefix length (e.g., 56)
    uint32_t iaid;                   // Identity association ID
    std::string cpe_link_local;      // CPE's link-local address
    std::string router_ip;           // Router's IP address
};
```

### Phase 2: NetBox Integration

**NetBox Custom Fields Required:**
- `client_duid` (string) - Client DUID for tracking
- `iaid` (integer) - Identity Association ID
- `cpe_link_local` (string) - CPE's link-local address for route construction
- `router_ip` (string) - Router's IP address for route construction

**API Request Strategy:**
- **Single POST to `/api/ipam/prefixes/`** with prefix data
- **Include custom fields** in the request payload
- **Fire-and-forget approach** - no response processing needed
- **No device management** - just prefix creation with routing data

**Request Payload Structure:**
```json
{
  "prefix": "2001:db8:56::/56",
  "status": "active",
  "description": "DHCPv6 PD assignment - IAID: 12345",
  "custom_fields": {
    "client_duid": "0001000123456789",
    "iaid": 12345,
    "cpe_link_local": "fe80::1234:56ff:fe78:9abc",
    "router_ip": "2001:db8:53::1"
  }
}
```

### Phase 3: Configuration

**Hook Configuration Parameters:**
```json
{
    "library": "/path/to/libpd_webhook.so",
    "parameters": {
        "netbox-enabled": true,
        "netbox-url": "https://netbox.example.com",
        "netbox-token": "your-api-token-here",
        "timeout-ms": 2000
    }
}
```

### Phase 4: Implementation Functions

**Core Functions to Implement:**

1. **Data Extraction Functions**
   ```cpp
   // Extract client DUID from CLIENTID option
   std::string extractClientDuid(const Pkt6Ptr& query);
   
   // Extract CPE link-local address from packet source or CLIENTID
   std::string extractCpeLinkLocal(const Pkt6Ptr& query);
   
   // Extract router IP from relay headers or packet source
   std::string extractRouterIp(const Pkt6Ptr& query);
   
   // Build assignment data structure from lease and packet
   PdAssignmentData buildAssignmentData(const Pkt6Ptr& query, const Lease6Ptr& lease);
   ```

2. **NetBox API Functions**
   ```cpp
   // Send single POST request to NetBox
   void sendNetBoxRequest(const PdAssignmentData& data);
   
   // Build JSON payload for NetBox prefix creation
   std::string buildNetBoxPayload(const PdAssignmentData& data);
   ```

3. **Main Processing Logic**
   ```cpp
   // Process each IA-PD assignment
   void processPdAssignment(const Lease6Ptr& lease, const Pkt6Ptr& query) {
       PdAssignmentData data = buildAssignmentData(query, lease);
       sendNetBoxRequest(data);
   }
   ```

### Phase 5: Error Handling

**Error Handling Strategy:**
- **Log to Kea logger** on any failures
- **Continue processing** (don't block DHCP operations)
- **Minimal validation** (lab environment)
- **No retries** (fire-and-forget approach)

**Error Scenarios:**
- Missing DUID in DHCP packet
- Missing CPE link-local address
- Missing router IP address
- NetBox API unavailability
- Network connectivity issues
- Invalid lease data

### Phase 6: Testing Strategy

**Test Cases:**
1. **Normal IA-PD Assignment** - All data present, successful NetBox prefix creation
2. **Missing DUID** - Handle gracefully, log error, still create prefix
3. **Network Issues** - NetBox unreachable, log error, continue DHCP operations
4. **Multiple IA-PDs** - Process each assignment independently
5. **Invalid Lease Data** - Error handling for malformed packets

## Implementation Notes

### Key Principles:
- **Minimal Scope** - Only IA-PD hook + NetBox API call
- **Simplicity** - Extract only required data (DUID, prefix, IAID, CPE link-local, router IP)
- **Reliability** - Don't fail DHCP operations
- **Fire-and-Forget** - No response processing, no device management
- **Lab-Friendly** - Minimal validation, maximum logging
- **Route Construction Ready** - Include all data needed for later route building

### Dependencies:
- Existing Kea hook framework
- libcurl for HTTP requests
- NetBox API access with token
- Custom fields created in NetBox (client_duid, iaid, cpe_link_local, router_ip)

## Next Steps

1. Simplify existing code to remove device management
2. Implement basic data extraction functions
3. Add single NetBox API POST function
4. Update configuration parameters
5. Test with IA-PD assignments
6. Deploy to lab environment

---

*This updated plan focuses on the core requirement with route construction support: hook IA-PD assignments, extract prefix/length + CPE link-local + router IP, and send NetBox API request with fire-and-forget approach.*