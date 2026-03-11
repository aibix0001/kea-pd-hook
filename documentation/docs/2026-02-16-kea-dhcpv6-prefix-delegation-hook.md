---
title: "Kea DHCPv6 Prefix Delegation Hook"
date: 2026-02-16
author: aibix
status: active
migrated_from: bookstack
bookstack_id: 108
bookstack_url: https://bookstack.lab.aibix.io/books/networks/page/kea-dhcpv6-prefix-delegation-hook
related_issues: []
related_mrs: []
---

# Kea DHCPv6 Prefix Delegation Hook

A C++ hook library for the Kea DHCPv6 server that monitors DHCPv6 Prefix Delegation (IA_PD) assignments and sends webhook notifications to NetBox for automated IPAM management.

## What It Is

This hook library plugs into ISC Kea's DHCPv6 server to intercept prefix delegation events in real time. When a DHCPv6 client receives a prefix delegation (IA_PD), the hook fires a webhook and automatically creates or updates devices and prefixes in NetBox. The result is that every prefix delegation assignment is reflected in NetBox IPAM without manual intervention.

## Why It Exists

In an IPv6 network using prefix delegation, routers request /48 or /56 prefixes from the DHCPv6 server. Without automation, tracking which prefix went to which router requires manually checking lease files or logs. This hook closes that gap by pushing assignment data directly into NetBox the moment a lease is committed, keeping IPAM in sync with the network's actual state.

## Architecture

### Hook Points

The library hooks into Kea's `leases6_committed` event, which fires when DHCPv6 leases (including prefix delegations) are committed. It also implements `lease6_recover` for handling recovered leases. When triggered, it extracts prefix details, client DUID, relay information, and link-local addresses, then pushes this data to both a generic webhook endpoint and the NetBox API.

### NetBox Integration

The hook communicates with NetBox's REST API (v3.x+) to:

- **Create devices** — Automatically registers router devices when new DHCPv6 clients appear (named by DUID prefix or IAID, e.g., `router-{duid_prefix}`)
- **Manage prefixes** — Creates IPv6 prefixes in NetBox IPAM and updates their status (active/deprecated) based on lease lifecycle
- **Track custom fields** — Stores DUID, IAID, valid/preferred lifetimes, and CPE link-local addresses as custom fields on the prefix

### Key Components

- `pd_webhook.cc` — Main hook implementation (~900 lines of C++17), handles all Kea hook points, JSON construction, and HTTP communication
- `netbox_client.h` / `netbox_client.cc` — Extracted NetBox API client interface (foundation for testability refactoring)
- `CMakeLists.txt` — CMake build configuration targeting Kea's hook library directory

### Configuration

The hook is configured in `kea-dhcp6.conf` under the `hooks-libraries` section with parameters for the NetBox URL, API token, HTTP timeout, and debug mode. A typical configuration specifies the library path at `/usr/lib/x86_64-linux-gnu/kea/hooks/libpd_webhook.so`.

### Build Requirements

- Kea DHCP development libraries (`isc-kea-dev`)
- libcurl for HTTP requests
- jsoncpp for JSON construction and parsing
- C++17 compatible compiler

## Development Status

The project has undergone a structured code maturity improvement process. Completed work includes replacing manual JSON construction with jsoncpp, removing unsafe raw memory scanning for CPE address extraction, implementing structured error handling, safe number parsing, and lease recovery. Remaining work includes configurable log levels, magic string constants, full testability refactoring, and documentation updates. Estimated remaining effort is around 9 hours.
