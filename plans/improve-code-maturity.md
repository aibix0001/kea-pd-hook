# Code Maturity Improvement Plan

## Overview

This plan addresses code maturity issues in `pd_webhook.cc` without focusing on security (lab project context). The main issues include:
- Fragile manual JSON construction/parsing
- Undefined behavior in CPE address extraction
- Silent error handling
- Incomplete functionality (TODO items)
- Code quality and maintainability concerns

---

## Phase 1: Fix JSON Handling (High Impact)

### 1.1 Replace Manual JSON Construction with jsoncpp

**Files**: `pd_webhook.cc`

**Current Problems**:
- Lines 214-226, 280-292, 614-644: Manual `std::ostringstream` JSON building
- No escaping of special characters in DUIDs, prefixes, or network addresses
- Claims to be "safe" without any validation

**Changes Required**:
1. Add `#include <json/json.h>` at top of file (jsoncpp already linked in CMakeLists.txt)
2. Replace all JSON building with `Json::Value` objects:
   - Use `Json::Value root(Json::objectValue);`
   - Set fields: `root["prefix"] = l->addr_.toText();`
   - For arrays: `root["leases"].append(lease_obj);`
3. Convert to string using `Json::StreamWriterBuilder` or `Json::FastWriter`
4. Apply to:
   - `createPrefix()` - lines 274-309
   - `updatePrefix()` - lines 206-243
   - `updateExpiredPrefix()` - lines 247-270
   - `notifyPdAssigned()` - lines 614-646
   - `notifyPdExpired()` - lines 681-696

**Example**:
```cpp
Json::Value payload;
payload["prefix"] = data.prefix + "/" + std::to_string(data.prefix_length);
payload["status"] = "active";
payload["description"] = "DHCPv6 PD assignment - IAID: " + std::to_string(data.iaid);

Json::Value custom_fields;
custom_fields["dhcpv6_client_duid"] = data.client_duid;
custom_fields["dhcpv6_iaid"] = static_cast<int>(data.iaid);
// ... more fields
payload["custom_fields"] = custom_fields;

Json::StreamWriterBuilder builder;
builder["indentation"] = "";
std::string payload_str = Json::writeString(builder, payload);
```

**Benefit**: Eliminates JSON syntax errors, handles special characters automatically, makes code maintainable

---

### 1.2 Replace String Parsing with jsoncpp

**Files**: `pd_webhook.cc`

**Current Problems**:
- Lines 180-202 (`findPrefixId`): Uses substring searches like `response.find("\"count\":0")`
- Fragile JSON parsing with `std::stoi` that can throw exceptions
- No error handling for malformed JSON

**Changes Required**:
1. Add `#include <json/json.h>` at top
2. Rewrite `findPrefixId()`:
   ```cpp
   Json::Value root;
   Json::CharReaderBuilder builder;
   Json::CharReader* reader = builder.newCharReader();
   std::string errors;
   bool success = reader->parse(response.c_str(), response.c_str() + response.size(), &root, &errors);
   delete reader;

   if (!success || !root.isMember("results") || !root["results"].isArray()) {
       return -1;
   }

   if (root["results"].size() == 0) {
       return -1;
   }

   return root["results"][0]["id"].asInt();
   ```
3. Add error logging for parse failures

**Benefit**: Reliable parsing, no false positives from substring matches, proper error handling

---

## Phase 2: Fix CPE Address Extraction (Critical)

### 2.1 Remove Raw Memory Scanning

**Files**: `pd_webhook.cc`

**Current Problems**:
- Lines 376-434: Raw memory dump of `RelayInfo` structure
- Scanning bytes for `fe80` pattern is undefined behavior
- Compiler optimizations and structure padding can break this
- Already have proper access to `relay.peeraddr_` at line 503

**Changes Required**:
1. Delete entire hex scan logic (lines 376-434)
2. Simplify `extractCpeLinkLocal()` to:
   ```cpp
   static std::string extractCpeLinkLocal(const Pkt6Ptr& query) {
       if (!query || query->relay_info_.empty()) {
           return "";
       }
       const Pkt6::RelayInfo& relay = query->relay_info_[0];
       return relay.peeraddr_.toText();
   }
   ```
3. Remove `hexDump()` function (lines 66-83) if only used by CPE extraction
4. Remove manual hex dump debug logging (lines 380-393)

**Benefit**: Eliminates undefined behavior, works reliably across compilers/optimizations, reduces code complexity

---

## Phase 3: Error Handling Improvements

### 3.1 Add Structured Error Reporting

**Files**: `pd_webhook.cc`

**Current Problems**:
- Lines 108-109: `(void)curl_easy_perform(curl);` - errors intentionally ignored
- Lines 167-169: No error checking on NetBox requests
- No visibility into failures for troubleshooting

**Changes Required**:
1. Create error code enum:
   ```cpp
   enum class ErrorCode {
       NONE,
       CURL_INIT_FAILED,
       HTTP_REQUEST_FAILED,
       JSON_PARSE_FAILED,
       INVALID_RESPONSE
   };
   ```
2. Add field to `WebhookConfig`:
   ```cpp
   struct WebhookConfig {
       // ... existing fields ...
       ErrorCode last_error{ErrorCode::NONE};
       std::string last_error_msg;
   };
   ```
3. Add error logging macro:
   ```cpp
   #define ERROR_LOG(msg) do { \
       g_cfg.last_error_msg = msg; \
       std::cerr << "[ERROR] " << msg << std::endl; \
   } while(0)
   ```
4. Replace silent failures with error logging:
   ```cpp
   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK) {
       ERROR_LOG("HTTP request failed: " + std::string(curl_easy_strerror(res)));
       return "";
   }
   ```

**Benefit**: Visibility into failures, easier debugging, better diagnostics

---

### 3.2 Safe Number Parsing

**Files**: `pd_webhook.cc`

**Current Problems**:
- Line 196: `std::stoi()` can throw `std::invalid_argument` or `std::out_of_range`
- No bounds checking for ID values
- No validation of prefix lengths

**Changes Required**:
1. Replace `std::stoi()` with safe parsing:
   ```cpp
   static int safeParseInt(const std::string& str, int default_val = -1) {
       try {
           size_t pos = 0;
           int val = std::stoi(str, &pos);
           if (pos != str.size()) {
               return default_val;  // Trailing characters
           }
           return val;
       } catch (const std::exception& e) {
           DEBUG_LOG("PD_WEBHOOK: Failed to parse int: " << e.what());
           return default_val;
       }
   }
   ```
2. Add bounds checking for prefix lengths (0-128)
3. Add validation for NetBox IDs (positive integers)

**Benefit**: Prevents exceptions on malformed data, more robust error handling

---

## Phase 4: Complete Functionality

### 4.1 Implement Lease Recovery

**Files**: `pd_webhook.cc`

**Current Problems**:
- Lines 809-843: `lease6_recover()` has TODO comment
- Function logs but doesn't actually do anything
- Incomplete feature implementation

**Changes Required**:
1. Implement `notifyPdRecovered()` function:
   ```cpp
   static void notifyPdRecovered(const Lease6Ptr& lease) {
       if (!lease || lease->type_ != Lease::TYPE_PD) {
           return;
       }

       PdAssignmentData data;
       data.client_duid = toHex(lease->duid_->getDuid());
       data.prefix = lease->addr_.toText();
       data.prefix_length = lease->prefixlen_;
       data.iaid = lease->iaid_;
       data.cpe_link_local = "";
       data.router_ip = "";
       data.router_link_addr = "";

       // Re-activate in NetBox (update status to "active")
       int existing_prefix_id = findPrefixId(data.prefix, data.prefix_length);
       if (existing_prefix_id > 0) {
           updatePrefix(existing_prefix_id, data, lease->valid_lft_, lease->preferred_lft_, "active");
       } else {
           // If not found, create new prefix
           createPrefix(data, lease->valid_lft_, lease->preferred_lft_);
       }
   }
   ```
2. Call `notifyPdRecovered()` in `lease6_recover()` hook
3. Add test cases for recovery scenarios

**Benefit**: Complete feature set, handles all lease lifecycle events

---

### 4.2 Add Webhook URL Support

**Files**: `pd_webhook.cc`, `README.md`

**Current Problems**:
- Configuration mentions `webhook-url` parameter (line 855) but it's not fully utilized
- No webhook URL parameter in WebhookConfig structure
- Documentation mentions webhook but implementation focuses on NetBox

**Changes Required**:
1. Verify webhook functionality is working:
   - Check `g_cfg.enabled` logic (line 885): only checks if URL is not empty
   - Ensure webhook URL is parsed from config (line 855-858)
2. Test webhook notifications independently from NetBox
3. Document webhook payload format in README
4. Add example webhook endpoint configuration

**Benefit**: Full feature parity with documentation

---

## Phase 5: Code Quality Improvements

### 5.1 Refactor for Testability

**Files**: `pd_webhook.cc`, `netbox_client.h` (new), `netbox_client.cc` (new)

**Current Problems**:
- All code in single file (915 lines)
- HTTP functions tightly coupled with hook logic
- No ability to mock network calls for testing

**Changes Required**:
1. Extract HTTP functions to new files:
   - Create `netbox_client.h` with interface
   - Create `netbox_client.cc` with implementations
   - Move functions: `postWebhook()`, `netboxHttpRequest()`, `findPrefixId()`, `createPrefix()`, `updatePrefix()`, `updateExpiredPrefix()`, `sendNetBoxRequest()`
2. Create interface for mocking:
   ```cpp
   class INetBoxClient {
   public:
       virtual ~INetBoxClient() = default;
       virtual int findPrefixId(const std::string& prefix, int prefix_length) = 0;
       virtual bool createPrefix(const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft) = 0;
       virtual bool updatePrefix(int prefix_id, const PdAssignmentData& data, uint32_t valid_lft, uint32_t preferred_lft, const std::string& status) = 0;
       virtual bool updateExpiredPrefix(int prefix_id, const PdAssignmentData& data) = 0;
   };
   ```
3. Update CMakeLists.txt to compile new files

**Benefit**: Enables unit testing, improves maintainability, reduces coupling

---

### 5.2 Add Logging Improvements

**Files**: `pd_webhook.cc`

**Current Problems**:
- Single `DEBUG_LOG` macro for all logging
- No log levels (INFO, WARNING, ERROR)
- Verbose hex dump in production builds
- No timestamps on log messages

**Changes Required**:
1. Create log level enum:
   ```cpp
   enum class LogLevel {
       ERROR,
       WARNING,
       INFO,
       DEBUG
   };
   ```
2. Add configurable log level to WebhookConfig:
   ```cpp
   LogLevel log_level{LogLevel::WARNING};
   ```
3. Create logging macros:
   ```cpp
   #define LOG(level, msg) do { \
       if (static_cast<int>(level) <= static_cast<int>(g_cfg.log_level)) { \
           std::time_t now = std::time(nullptr); \
           char buf[64]; \
           std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now)); \
           std::cout << "[" << buf << "] [" << #level << "] " << msg << std::endl; \
       } \
   } while(0)

   #define LOG_ERROR(msg)   LOG(LogLevel::ERROR, msg)
   #define LOG_WARNING(msg) LOG(LogLevel::WARNING, msg)
   #define LOG_INFO(msg)    LOG(LogLevel::INFO, msg)
   #define LOG_DEBUG(msg)   LOG(LogLevel::DEBUG, msg)
   ```
4. Remove or conditionally compile verbose hex dumps
5. Update config parsing to support `log-level` parameter

**Benefit**: Professional logging, easier troubleshooting, configurable verbosity

---

### 5.3 Constants and Magic Numbers

**Files**: `pd_webhook.cc`

**Current Problems**:
- Magic strings ("active", "deprecated") scattered throughout code
- Timeout limits hardcoded
- No namespace for configuration keys

**Changes Required**:
1. Create namespace for constants:
   ```cpp
   namespace Config {
       constexpr const char* NETBOX_URL = "netbox-url";
       constexpr const char* NETBOX_TOKEN = "netbox-token";
       constexpr const char* WEBHOOK_URL = "webhook-url";
       constexpr const char* TIMEOUT_MS = "timeout-ms";
       constexpr const char* DEBUG = "debug";
       constexpr const char* LOG_LEVEL = "log-level";

       constexpr long DEFAULT_TIMEOUT_MS = 2000L;
       constexpr long MIN_TIMEOUT_MS = 100L;
       constexpr long MAX_TIMEOUT_MS = 30000L;
   }

   namespace NetBox {
       constexpr const char* STATUS_ACTIVE = "active";
       constexpr const char* STATUS_DEPRECATED = "deprecated";
       constexpr const char* PREFIX_ENDPOINT = "ipam/prefixes/";
       constexpr const char* AUTH_HEADER_PREFIX = "Authorization: Token ";
   }

   namespace Events {
       constexpr const char* PD_ASSIGNED = "pd_assigned";
       constexpr const char* PD_EXPIRED = "pd_expired";
   }
   ```
2. Replace all magic strings/numbers with constants
3. Add timeout validation in config parsing

**Benefit**: Self-documenting code, easier to modify, prevents typos

---

## Phase 6: Documentation

### 6.1 Update README

**Files**: `README.md`

**Changes Required**:
1. Add troubleshooting section:
   - Common errors and solutions
   - How to check NetBox connection
   - Debug mode usage
   - Log file locations
2. Document all configuration parameters:
   - `webhook-url`: Webhook endpoint for notifications
   - `netbox-url`: NetBox API base URL
   - `netbox-token`: NetBox API token
   - `timeout-ms`: HTTP timeout (100-30000ms)
   - `debug`: Enable debug logging
   - `log-level`: Set log verbosity (error/warning/info/debug)
3. Add example webhook payloads:
   - PD assigned event
   - PD expired event
4. Document NetBox field mapping:
   - Custom fields used
   - Status transitions
   - Description format
5. Add development guide:
   - How to build
   - How to test
   - Code structure overview

---

### 6.2 Code Comments

**Files**: `pd_webhook.cc`

**Changes Required**:
1. Add function-level documentation:
   ```cpp
   /**
    * Extract the CPE's link-local address from relay information.
    *
    * @param query The DHCPv6 packet containing relay information
    * @return Link-local IPv6 address as string, or empty string if not found
    */
   static std::string extractCpeLinkLocal(const Pkt6Ptr& query);
   ```
2. Document NetBox API interactions:
   - API endpoints used
   - Expected request/response formats
   - HTTP methods
3. Explain hook call sequence:
   - When each hook is called
   - What data is available
   - Side effects
4. Document data structures:
   - `PdAssignmentData` field descriptions
   - `WebhookConfig` field descriptions

**Benefit**: Better maintainability, easier onboarding for new developers

---

## Implementation Order

| Priority | Phase | Estimated Effort | Risk | Dependencies |
|----------|-------|------------------|------|--------------|
| 1 | 2.1 (CPE extraction) | 1-2 hours | Low | None |
| 2 | 1.1, 1.2 (JSON handling) | 3-4 hours | Low | None |
| 3 | 3.2 (Safe parsing) | 1 hour | Low | None |
| 4 | 4.1 (Recovery) | 2-3 hours | Medium | Phase 1.1 |
| 5 | 3.1, 5.3 (Error handling & constants) | 2 hours | Low | Phase 3.2 |
| 6 | 5.2 (Logging) | 2 hours | Low | None |
| 7 | 6.1, 6.2 (Documentation) | 2-3 hours | Low | All phases |
| 8 | 5.1 (Refactoring) | 4-6 hours | Medium | Phases 1, 2, 3 |

**Total estimated effort: 17-23 hours**

---

## Questions Before Implementation

1. **Webhook feature**: Should the webhook URL be a separate endpoint from NetBox, or is NetBox integration the primary feature? (Currently configured separately but not fully tested)

2. **Thread safety**: The code declares MT-safe (line 904) but uses global mutable state. Should we:
   - Make it truly thread-safe (add mutexes)?
   - Remove the claim and document as single-threaded?
   - Leave as-is for lab project context?

3. **Lease recovery**: Should recovered leases in NetBox:
   - Re-activate to "active" status?
   - Add a note to description indicating recovery?
   - Do nothing (just log)?

4. **Error severity**: Should network errors in the hook:
   - Cause the DHCP transaction to fail?
   - Continue silently (current behavior)?
   - Log warning but continue?

5. **Hex dump removal**: Should `hexDump()` function be:
   - Removed entirely (only used by CPE extraction)?
   - Kept for potential future debugging use?
   - Made conditional on debug level?

---

## Success Criteria

After completing all phases, the code will be considered mature when:

- [ ] All JSON construction/parsing uses jsoncpp
- [ ] No raw memory manipulation or undefined behavior
- [ ] All exceptions are caught and logged
- [ ] All TODO items are resolved
- [ ] Comprehensive error logging is in place
- [ ] Code is organized into logical modules
- [ ] Documentation is complete and up-to-date
- [ ] All magic numbers are replaced with named constants
- [ ] Logging uses proper severity levels
- [ ] Code passes static analysis (cppcheck/clang-tidy) without warnings
- [ ] Unit tests exist for core functionality (after Phase 5.1 refactoring)
