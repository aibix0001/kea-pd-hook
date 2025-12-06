# Kea DHCPv6 PD Hook

A C++ hook library for Kea DHCPv6 server that monitors DHCPv6 Prefix Delegation (IA_PD) assignments and integrates with NetBox for automated network infrastructure management.

## Purpose

This hook library provides real-time visibility and automation for DHCPv6 Prefix Delegation events by:
- Monitoring IA_PD assignments and renewals in Kea DHCPv6
- Extracting accurate CPE link-local addresses from DHCPv6 relay messages
- Automatically creating/updating prefix and device records in NetBox
- Providing lease expiration timestamps for infrastructure automation

## Features

- **DHCPv6 PD Monitoring**: Tracks prefix delegation assignments and renewals
- **NetBox Integration**: Automatic prefix and device management via NetBox API v3.x+
- **Peer Address Extraction**: Extracts CPE link-local addresses from DHCPv6 relay-forward messages
- **Lease Expiration Tracking**: Calculates and stores lease expiration timestamps
- **Configurable Debug Output**: Toggle verbose logging for troubleshooting
- **IPv6 Address Compression**: Proper IPv6 address formatting using Kea's built-in functions
- **Dual Hook Points**: Supports both initial assignments (`leases6_committed`) and renewals (`lease6_renew`)

## Building

### Prerequisites

- Kea DHCP development libraries (`isc-kea-dev`)
- libcurl development headers (`libcurl4-openssl-dev`)
- jsoncpp development headers (`libjsoncpp-dev`)
- C++17 compatible compiler (g++ recommended)

### Build Commands

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
                    "webhook-url": "https://your-api.example.com/dhcp6-events",
                    "timeout-ms": 2000,
                    "netbox-enabled": true,
                    "netbox-url": "https://netbox.example.com",
                    "netbox-token": "your-netbox-api-token",
                    "debug": false
                }
            }
        ]
    }
}
```

### Configuration Parameters

- **webhook-url**: Original webhook endpoint for DHCPv6 events (optional)
- **timeout-ms**: HTTP request timeout in milliseconds (default: 2000)
- **netbox-enabled**: Enable/disable NetBox API integration (boolean, default: false)
- **netbox-url**: Base URL of your NetBox instance (string)
- **netbox-token**: NetBox API authentication token with write permissions (string)
- **debug**: Enable verbose debug logging for troubleshooting (boolean, default: false)

### Required NetBox Custom Fields

The hook requires the following custom fields to be created in your NetBox instance:

- **dhcpv6_client_duid**: Client DUID in hex format (text)
- **dhcpv6_iaid**: Identity Association ID (number)  
- **dhcpv6_cpe_link_local**: CPE's link-local IPv6 address (text)
- **dhcpv6_router_ip**: Router's IPv6 address (text)
- **dhcpv6_leasetime**: Lease expiration Unix timestamp (number)

### NetBox Data Model

The hook will:
1. **Extract CPE link-local address** from DHCPv6 relay-forward message peer-address field
2. **Calculate lease expiration** as Unix timestamp (current time + valid lifetime)
3. **Create/update prefix objects** with proper custom field population
4. **Support lease renewals** through `lease6_renew` hook point

Address extraction priority:
1. **Peer address from relay message** (most accurate)
2. **EUI-64 from DUID MAC address** (fallback)
3. **Packet source address** (final fallback)

## Usage

### Production Deployment

For production use, set `"debug": false` to minimize log output:

```json
{
    "Dhcp6": {
        "hooks-libraries": [
            {
                "library": "/usr/lib/x86_64-linux-gnu/kea/hooks/libpd_webhook.so",
                "parameters": {
                    "netbox-enabled": true,
                    "netbox-url": "https://netbox.example.com",
                    "netbox-token": "your-production-token",
                    "debug": false
                }
            }
        ]
    }
}
```

### Debug Mode

For troubleshooting, enable debug output with `"debug": true`:

```json
"debug": true
```

Debug output includes:
- DHCPv6 message type analysis
- Relay information extraction details
- Peer address extraction process
- NetBox API request/response details
- DUID parsing and MAC extraction

## Testing

```bash
# Validate configuration syntax
kea-dhcp6 -t kea-dhcp6.conf

# Test with verbose output
kea-dhcp6 -c kea-dhcp6.conf -v

# Monitor logs in real-time
journalctl -u isc-kea-dhcp6-server -f
```

## Router Configuration Server

This project includes a Flask server (`router_config_server.py`) that receives webhook data from NetBox and automatically configures VyOS routers with DHCPv6 prefix delegation routes.

### Architecture

The solution is split into two parts:

1. **NetBox Script** (`netbox-scripts/extract_prefix_data_webhook.py`):
   - Runs on NetBox server
   - Extracts prefix custom fields from NetBox
   - Sends webhook to configuration server
   - Updates `automation_was_here` flag on success

2. **Router Configuration Server** (`router_config_server.py`):
   - Receives webhook data from NetBox
   - Validates lease information
   - Connects to VyOS router via SSH
   - Configures static routes
   - Maintains configuration history

### Setup

```bash
# Install Python dependencies
uv add flask paramiko requests

# Start the router configuration server
uv run python router_config_server.py
```

### Testing

```bash
# Test the server
uv run python test_router_client.py

# Manual test
curl -X POST http://localhost:5000/configure-router \
  -H "Content-Type: application/json" \
  -d '{
    "prefix": "2001:db8:56::/56",
    "router_ip": "192.168.1.100",
    "cpe_link_local": "fe80::1234:5678:9abc:def0",
    "leasetime": 1734076800
  }'
```

### Complete Workflow

1. **Kea Hook** creates NetBox prefix with DHCPv6 data
2. **NetBox Script** extracts data and sends webhook
3. **Router Server** configures the VyOS router
4. **NetBox Script** updates `automation_was_here` flag

This creates a complete automated workflow from DHCPv6 PD assignment to router configuration.

## Development

This project follows Kea hook library conventions and uses C++17 standards. See `AGENTS.md` for detailed development guidelines and build commands.

## Hook Points

- **leases6_committed**: Triggered when DHCPv6 leases are committed (initial assignments)
- **lease6_renew**: Triggered when DHCPv6 leases are renewed (lease extensions)

## License

[Add your license information here]