# Kea DHCPv6 PD Hook Development Guide

## Build Commands
```bash
# Build the hook library
g++ -shared -fPIC -std=c++17 -I/usr/include/kea -o libpd_webhook.so pd_webhook.cc -lcurl -ljsoncpp

# Build with debug symbols
g++ -shared -fPIC -std=c++17 -g -I/usr/include/kea -o libpd_webhook.so pd_webhook.cc -lcurl -ljsoncpp

# Clean build artifacts
rm -f libpd_webhook.so *.o
```

## Testing Commands
```bash
# Run Kea with hook (requires Kea installation)
kea-dhcp6 -c kea-dhcp6.conf

# Test with configuration validation
kea-dhcp6 -t kea-dhcp6.conf
```

## Code Style Guidelines
- Use C++17 standard
- Follow Kea hook library conventions
- Include proper error handling for all external API calls
- Use RAII for resource management
- Prefix custom classes with `PdWebhook`
- Use camelCase for function names, PascalCase for classes
- Include comprehensive logging using Kea's logger
- Handle HTTP requests asynchronously where possible
- Validate all JSON data before processing
- Use const correctness throughout codebase

## Dependencies
- isc-kea-dev (Kea development headers)
- libcurl4-openssl-dev (HTTP client library)
- libjsoncpp-dev (JSON parsing library)
- libboost-all-dev (Boost libraries required by Kea)

## NetBox Integration
- Supports NetBox REST API v3.x+
- Creates devices and prefixes automatically
- Requires NetBox API token with write permissions
- Device naming: router-{duid_prefix} or router-{iaid}