# Kea DHCPv6 PD Hook

A C++ hook library for Kea DHCPv6 server that monitors DHCPv6 Prefix Delegation (IA_PD) assignments and sends webhook notifications.

## Purpose

This hook library provides real-time visibility for DHCPv6 Prefix Delegation events by:
- Monitoring IA_PD assignments in Kea DHCPv6
- Sending JSON webhook notifications with lease details
- Supporting configurable debug output for troubleshooting
- Providing lease information including prefix, IAID, and lifetimes

## Features

- **DHCPv6 PD Monitoring**: Tracks prefix delegation assignments
- **Webhook Notifications**: Sends JSON payloads with lease details
- **Configurable Debug Output**: Toggle verbose logging for troubleshooting
- **Lease Information**: Includes prefix, IAID, subnet ID, and lifetimes

## Building

### Prerequisites

- Kea DHCP development libraries (`isc-kea-dev`)
- libcurl development headers (`libcurl4-openssl-dev`)
- C++17 compatible compiler (g++ recommended)

### Build Commands

```bash
# Build the hook library
g++ -shared -fPIC -std=c++17 -I/usr/include/kea -o libpd_webhook.so pd_webhook.cc -lcurl

# Build with debug symbols
g++ -shared -fPIC -std=c++17 -g -I/usr/include/kea -o libpd_webhook.so pd_webhook.cc -lcurl

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
                    "debug": false
                }
            }
        ]
    }
}
```

### Configuration Parameters

- **webhook-url**: Webhook endpoint for DHCPv6 events
- **timeout-ms**: HTTP request timeout in milliseconds (default: 2000)
- **debug**: Enable verbose debug logging for troubleshooting (boolean, default: false)

### Production Deployment

For production use, set `"debug": false` to minimize log output.

### Debug Mode

For troubleshooting, enable debug output with `"debug": true`.

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

This project follows Kea hook library conventions and uses C++17 standards. See `AGENTS.md` for detailed development guidelines and build commands.

## Hook Points

- **leases6_committed**: Triggered when DHCPv6 leases are committed (initial assignments)

## License

[Add your license information here]