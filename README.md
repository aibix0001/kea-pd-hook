# Kea DHCPv6 PD Hook

A C++ hook library for the Kea DHCPv6 server that sends external API requests when an IA_PD (Prefix Delegation) is assigned to a client.

## Overview

This hook library integrates with Kea DHCPv6 to monitor prefix delegation assignments and trigger webhook notifications to external systems. It's designed to provide real-time visibility into DHCPv6 prefix delegation events.

## Features

- Monitors IA_PD (Prefix Delegation) assignments in Kea DHCPv6
- Sends HTTP POST requests to configurable webhook endpoints
- JSON payload format with assignment details
- Configurable timeout and retry logic
- Comprehensive logging through Kea's logging system

## Building

### Prerequisites

- Kea DHCP development libraries (`libkea-*`)
- libcurl development headers
- jsoncpp development headers
- C++17 compatible compiler (g++ recommended)

### Build Commands

```bash
# Build the hook library
g++ -shared -fPIC -std=c++17 -o libpd_webhook.so pd_webhook.cc -I/usr/include/kea -lcurl -ljsoncpp

# Build with debug symbols
g++ -shared -fPIC -std=c++17 -g -o libpd_webhook.so pd_webhook.cc -I/usr/include/kea -lcurl -ljsoncpp

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
                "library": "/path/to/libpd_webhook.so",
                "parameters": {
                    "webhook-url": "https://your-api.example.com/dhcp6-events",
                    "timeout": 5000,
                    "retry-count": 3
                }
            }
        ]
    }
}
```

## Testing

```bash
# Validate configuration
kea-dhcp6 -t kea-dhcp6.conf

# Run Kea with the hook
kea-dhcp6 -c kea-dhcp6.conf
```

## Development

This project follows Kea hook library conventions and uses C++17 standards. See `AGENTS.md` for detailed development guidelines and build commands.

## License

[Add your license information here]