# Kea DHCPv6 PD Hook

A C++ hook library for Kea DHCPv6 server that monitors DHCPv6 Prefix Delegation (IA_PD) assignments and sends webhook notifications to NetBox for automated IPAM management.

## Purpose

This hook library provides real-time visibility for DHCPv6 Prefix Delegation events by:
- Monitoring IA_PD assignments in Kea DHCPv6
- Sending JSON webhook notifications with lease details to NetBox
- Automatically creating devices and prefixes in NetBox for IPAM management
- Supporting configurable debug output for troubleshooting
- Providing lease information including prefix, IAID, and lifetimes

## Features

- **DHCPv6 PD Monitoring**: Tracks prefix delegation assignments
- **NetBox Integration**: Automatically creates devices and prefixes in NetBox IPAM
- **Webhook Notifications**: Sends JSON payloads with lease details to NetBox API
- **Configurable Debug Output**: Toggle verbose logging for troubleshooting
- **Lease Information**: Includes prefix, IAID, subnet ID, and lifetimes
- **Device Naming**: Supports router naming by DUID prefix or IAID

## Building

### Prerequisites

- Kea DHCP development libraries (`isc-kea-dev`)
- libcurl development headers (`libcurl4-openssl-dev`)
- JSON parsing library (`libjsoncpp-dev`)
- C++17 compatible compiler (g++ recommended)

### Build Commands

#### Using CMake (Recommended)

```bash
# Create build directory
mkdir build && cd build

# Configure build
cmake ..

# Build the library
make

# Install to Kea hooks directory
sudo make install

# Restart Kea DHCPv6 service
sudo systemctl restart isc-kea-dhcp6-server
```

#### Manual Build with g++

```bash
# Build the hook library
g++ -shared -fPIC -std=c++17 -I/usr/include/kea -o libpd_webhook.so pd_webhook.cc -lcurl -ljsoncpp

# Build with debug symbols
g++ -shared -fPIC -std=c++17 -g -I/usr/include/kea -o libpd_webhook.so pd_webhook.cc -lcurl -ljsoncpp

# Install to Kea hooks directory
sudo cp libpd_webhook.so /usr/lib/x86_64-linux-gnu/kea/hooks/

# Restart Kea DHCPv6 service
sudo systemctl restart isc-kea-dhcp6-server

# Clean build artifacts
rm -f libpd_webhook.so *.o
```

## Configuration

Add the hook to your `kea-dhcp6.conf`:

```json
{
    "Dhcp6": {
        "hooks-libraries": [
            {
                "library": "/usr/lib/x86_64-linux-gnu/kea/hooks/libpd_webhook.so",
                "parameters": {
                    "netbox-url": "https://your-netbox.example.com/api",
                    "netbox-token": "your-netbox-api-token",
                    "timeout-ms": 2000,
                    "debug": false
                }
            }
        ]
    }
}
```

### Configuration Parameters

- **netbox-url**: NetBox API base URL (e.g., https://your-netbox.example.com/api)
- **netbox-token**: NetBox API token with write permissions
- **timeout-ms**: HTTP request timeout in milliseconds (default: 2000)
- **debug**: Enable verbose debug logging for troubleshooting (boolean, default: false)

### Production Deployment

For production use, set `"debug": false` to minimize log output.

### Debug Mode

For troubleshooting, enable debug output with `"debug": true`.

## NetBox Integration

This hook integrates with NetBox (v3.x+) to automatically manage IPAM data:

- **Device Creation**: Automatically creates router devices in NetBox when new DHCPv6 clients are detected
- **Prefix Management**: Creates and updates IPv6 prefixes in NetBox IPAM
- **Device Naming**: Uses DUID prefix or IAID for device naming (format: `router-{duid_prefix}` or `router-{iaid}`)
- **API Requirements**: Requires NetBox API token with write permissions to devices, prefixes, and IP addresses

### NetBox API Compatibility

- Supports NetBox REST API v3.x+
- Tested with NetBox 3.6+ installations
- Requires appropriate site and role configurations in NetBox for device and prefix creation

## Testing

```bash
# Validate configuration syntax
kea-dhcp6 -t kea-dhcp6.conf

# Test with verbose output
kea-dhcp6 -c kea-dhcp6.conf -v

# Monitor logs in real-time
journalctl -u isc-kea-dhcp6-server -f
```

## Development

This project follows Kea hook library conventions and uses C++17 standards.

## Hook Points

- **leases6_committed**: Triggered when DHCPv6 leases are committed (initial assignments)

## License

[Add your license information here]