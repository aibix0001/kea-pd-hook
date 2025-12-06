# MKII Implementation Plan for PD Webhook Elegance Improvements

## Overview
This plan addresses the elegance issues identified in `pd_webhook.cc` analysis. Focus areas include JSON handling, relay parsing simplification, code organization, and error handling improvements.

## 1. JSON Handling Refactor
**Issue**: Manual string concatenation for JSON payloads is error-prone and verbose.

**Solution**: Utilize jsoncpp library for structured JSON building.

**Steps**:
- Add `#include <json/json.h>` to includes
- Create helper functions:
  - `Json::Value buildWebhookPayload(const Pkt6Ptr& query, const Pkt6Ptr& response, const std::vector<Lease6Ptr>& pd_leases)`
  - `Json::Value buildNetBoxPayload(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft)`
- Replace manual string building in `notifyPdAssigned`, `updatePrefix`, and `createPrefix`
- Test JSON output matches current format

**Dependencies**: Ensure jsoncpp is properly linked (already in build command)

## 2. Simplify Peer-Address Extraction
**Issue**: `extractCpeLinkLocal` is overly complex with manual byte parsing.

**Investigation Complete**: Kea's RelayInfo structure exposes `peeraddr_` field as `isc::asiolink::IOAddress`.

**Steps**:
- Replace manual byte parsing with direct access to `relay.peeraddr_`
- Simplify `extractCpeLinkLocal` to use `relay.peeraddr_.toText()` for link-local addresses
- Keep fallback parsing for cases where relay info is not available
- Refactor into cleaner helper functions:
  - `std::string extractPeerAddressFromRelayInfo(const RelayInfo& relay)`
  - `std::string parsePeerAddressFromRelayMsgOption(const std::vector<uint8_t>& relay_data)`
  - `std::string generateLinkLocalFromDuid(const std::vector<uint8_t>& duid_data)`
- Remove hacky `reinterpret_cast` and manual byte manipulation
- Reduce debug logging verbosity in extraction functions

## 3. Extract Common NetBox JSON Logic
**Issue**: `updatePrefix` and `createPrefix` duplicate payload building.

**Solution**: Create shared payload builder.

**Steps**:
- Create `Json::Value buildNetBoxPrefixPayload(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft, bool is_update)`
- Modify `updatePrefix` and `createPrefix` to use shared builder
- Ensure timestamp calculation and custom_fields are consistent

## 4. Improve Error Handling and Logging
**Issue**: Empty catch blocks and extensive debug logging.

**Solution**: Add structured error logging while maintaining Kea compatibility.

**Steps**:
- Define error codes enum: `enum PdWebhookError { NETBOX_REQUEST_FAILED, JSON_BUILD_FAILED, RELAY_PARSE_FAILED }`
- Create logging helper: `void logError(PdWebhookError err, const std::string& details)`
- Replace empty catches with specific error logging
- Make debug logging configurable per function or reduce verbosity
- Use Kea's logger if available, fallback to std::cout

## 5. RAII for Resource Management
**Issue**: Manual CURL cleanup.

**Solution**: Implement RAII wrapper for CURL handles.

**Steps**:
- Create `CurlHandle` class with constructor/destructor
- Replace direct `curl_easy_init`/`curl_easy_cleanup` calls
- Ensure exception safety in HTTP functions

## 6. Function Splitting and Organization
**Issue**: `notifyPdAssigned` and `extractCpeLinkLocal` are monolithic.

**Solution**: Break into smaller, focused functions.

**Steps**:
- Split `notifyPdAssigned`:
  - `void sendWebhookNotification(const Pkt6Ptr& query, const Pkt6Ptr& response, const std::vector<Lease6Ptr>& pd_leases)`
  - `void sendNetBoxNotifications(const Pkt6Ptr& query, const std::vector<Lease6Ptr>& pd_leases)`
- Split `extractCpeLinkLocal` into sub-functions as noted above
- Ensure each function has single responsibility

## 7. Dependency Cleanup
**Issue**: jsoncpp linked but not used in current code.

**Solution**: Verify usage after refactor.

**Steps**:
- After implementing JSON refactor, check if jsoncpp is actually used
- If not needed, remove from build command and dependencies
- Update AGENTS.md accordingly

## 8. Testing and Validation
**Steps**:
- Build with new code: `g++ -shared -fPIC -std=c++17 -I/usr/include/kea -o libpd_webhook.so pd_webhook.cc -lcurl -ljsoncpp`
- Test configuration loading and basic functionality
- Validate JSON output format matches expectations
- Test relay parsing with various packet types
- Run Kea validation: `kea-dhcp6 -t kea-dhcp6.conf`

## Implementation Order
1. JSON handling refactor (foundation for others)
2. Peer-address extraction investigation and simplification
3. Common NetBox logic extraction
4. Error handling improvements
5. RAII implementation
6. Function splitting
7. Dependency cleanup
8. Testing and validation

## Risks and Considerations
- Kea API changes: RelayInfo.peeraddr_ is a public field, but monitor for Kea API changes
- JSON format changes: Must maintain backward compatibility with webhook consumers
- Performance: JSON library usage should not significantly impact latency
- Memory: RAII should prevent leaks in error paths

## Success Criteria
- Code compiles without warnings
- All existing functionality preserved
- Functions are more readable and maintainable
- Relay parsing is more robust and less complex
- Error conditions are properly logged
- No orphaned code or unused dependencies