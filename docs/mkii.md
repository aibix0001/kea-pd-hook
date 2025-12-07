# MKII Implementation Plan for PD Webhook Elegance Improvements

## Overview
This plan addresses the elegance issues identified in `pd_webhook.cc` analysis. Focus areas include JSON handling, relay parsing simplification, code organization, and error handling improvements for the webhook notification functionality.

## 1. JSON Handling Refactor
**Issue**: Manual string concatenation for JSON payloads is error-prone and verbose.

**Solution**: Utilize jsoncpp library for structured JSON building.

**Steps**:
- Add `#include <json/json.h>` to includes
- Create helper function:
  - `Json::Value buildWebhookPayload(const Pkt6Ptr& query, const Pkt6Ptr& response, const std::vector<Lease6Ptr>& pd_leases)`
- Replace manual string building in `notifyPdAssigned`
- Test JSON output matches current format

**Dependencies**: Ensure jsoncpp is properly linked (already in build command)

## 2. Error Handling and Logging Improvements
**Issue**: Empty catch blocks and extensive debug logging.

**Solution**: Add structured error logging while maintaining Kea compatibility.

**Steps**:
- Define error codes enum: `enum PdWebhookError { WEBHOOK_REQUEST_FAILED, JSON_BUILD_FAILED }`
- Create logging helper: `void logError(PdWebhookError err, const std::string& details)`
- Replace empty catches with specific error logging
- Make debug logging configurable per function or reduce verbosity
- Use Kea's logger if available, fallback to std::cout

## 3. RAII for Resource Management
**Issue**: Manual CURL cleanup.

**Solution**: Implement RAII wrapper for CURL handles.

**Steps**:
- Create `CurlHandle` class with constructor/destructor
- Replace direct `curl_easy_init`/`curl_easy_cleanup` calls
- Ensure exception safety in HTTP functions

## 4. Function Splitting and Organization
**Issue**: `notifyPdAssigned` and `extractCpeLinkLocal` are monolithic.

**Solution**: Break into smaller, focused functions.

**Steps**:
- Split `notifyPdAssigned`:
  - `void sendWebhookNotification(const Pkt6Ptr& query, const Pkt6Ptr& response, const std::vector<Lease6Ptr>& pd_leases)`
- Ensure each function has single responsibility

## Implementation Order
1. JSON handling refactor (foundation for others)
2. Error handling improvements
3. RAII implementation
4. Function splitting

## Risks and Considerations
- Kea API changes: Monitor for Kea API changes
- JSON format changes: Must maintain backward compatibility with webhook consumers
- Performance: JSON library usage should not significantly impact latency
- Memory: RAII should prevent leaks in error paths

## Success Criteria
- Code compiles without warnings
- All existing functionality preserved
- Functions are more readable and maintainable
- Error conditions are properly logged
- No orphaned code or unused dependencies
