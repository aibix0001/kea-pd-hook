# Kea DHCPv6 PD Hook - NetBox Integration Plan

## Project Overview

This document outlines the implementation plan for extending the Kea DHCPv6 PD hook to integrate with NetBox API for prefix assignment tracking.

## Requirements

### Current Environment
- **VyOS VM routers** with basic relay agents
- **Unknown CPE types** (maximum flexibility required)
- **Lab environment** (minimal validation needed)
- **Real-time processing** of each DHCPv6 assignment

### Data Flow
```
DHCPv6 Packet → Extract Data → NetBox API Request → Fire-and-Forget
                    ↓
Router IP (from relay) → Store in NetBox custom field
```

## Implementation Plan

### Phase 1: Data Extraction

**Data to Extract from DHCPv6 Packets:**
1. **CPE DUID** - From client identifier option (D6O_CLIENTID)
2. **CPE Link-Local Address** - From client identifier option
3. **Router IP** - From DHCPv6 relay message headers
4. **Assigned Prefix** - From lease (network + prefix length)
5. **IAID** - From lease for correlation

**Data Structure:**
```cpp
struct AssignmentData {
    std::string cpe_duid;           // Client DUID for tracking
    std::string cpe_link_local;      // CPE's link-local address  
    std::string router_relay_ip;     // Router's IP from DHCP exchange
    std::string prefix;              // Assigned prefix (e.g., "2001:db8:56::")
    int prefix_length;                // Prefix length (e.g., 56)
    uint32_t iaid;                 // Identity association ID
};
```

### Phase 2: NetBox Integration

**NetBox Custom Fields Required:**
- `cpe_duid` (string) - Client DUID for CPE tracking
- `cpe_link_local` (string) - CPE's link-local address
- `router_relay_ip` (string) - Router's IP from DHCP exchange

**API Request Strategy:**
- **POST to `/api/ipam/prefixes/`** with prefix data
- **Include custom fields** in the request payload
- **Fire-and-forget approach** - no response processing needed

**Request Payload Structure:**
```json
{
  "prefix": "2001:db8:56::/56",
  "status": "active", 
  "scope_type": "dcim.site",
  "scope_id": 1,
  "custom_fields": {
    "cpe_duid": "00:01:02:03:04:05",
    "cpe_link_local": "fe80::1234:56ff:fe78:9abc",
    "router_relay_ip": "2001:db8:53::1"
  },
  "description": "DHCPv6 PD assignment - IAID: 12345"
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
        "netbox-token": "your-api-token-here"
    }
}
```

### Phase 4: Implementation Functions

**Core Functions to Implement:**

1. **Data Extraction Functions**
   ```cpp
   // Extract CPE DUID from client identifier option
   std::string extractCPEduid(const OptionPtr& client_id);
   
   // Extract CPE link-local from client identifier option  
   std::string extractCPELinkLocal(const OptionPtr& client_id);
   
   // Extract router IP from DHCPv6 relay message
   std::string extractRouterIP(const Pkt6Ptr& packet);
   
   // Build assignment data structure
   AssignmentData extractAssignmentData(const Pkt6Ptr& query, const Lease6Ptr& lease);
   ```

2. **NetBox API Functions**
   ```cpp
   // Send NetBox API request
   bool sendNetBoxRequest(const AssignmentData& data);
   
   // Build JSON payload for NetBox
   std::string buildNetBoxPayload(const AssignmentData& data);
   ```

3. **Main Processing Logic**
   ```cpp
   // Process each PD assignment
   void processPdAssignment(const Lease6Ptr& lease, const Pkt6Ptr& query) {
       AssignmentData data = extractAssignmentData(query, lease);
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
- Malformed client identifier option
- NetBox API unavailability
- Network connectivity issues

### Phase 6: Testing Strategy

**Test Cases:**
1. **Normal Assignment** - All data present, successful NetBox update
2. **Missing DUID** - Handle gracefully, log error
3. **Network Issues** - NetBox unreachable, log error
4. **Multiple CPEs** - Process each assignment independently
5. **Invalid Data** - Malformed packets, error handling

## Implementation Notes

### Key Principles:
- **Simplicity** - Extract only what's needed
- **Reliability** - Don't fail DHCP operations
- **Flexibility** - Work with unknown CPE types
- **Fire-and-Forget** - No response processing required
- **Lab-Friendly** - Minimal validation, maximum logging

### Dependencies:
- Existing Kea hook framework
- libcurl for HTTP requests
- NetBox API access with token
- Custom fields created in NetBox

## Next Steps

1. Implement data extraction functions
2. Add NetBox API client code
3. Integrate into existing hook framework
4. Add configuration parameters
5. Test with various DHCPv6 scenarios
6. Deploy to lab environment

---

*This plan focuses on the core requirement: extract DHCPv6 data and send NetBox API request with fire-and-forget approach.*