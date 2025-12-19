// Mock dhcp6/dhcp6.h
#ifndef MOCK_DHCP6_DHCP6_H
#define MOCK_DHCP6_DHCP6_H

#include <cstdint>
#include <vector>

namespace isc {
namespace dhcp {

enum DHCPv6MessageType {
    DHCPV6_SOLICIT = 1,
    DHCPV6_REQUEST = 3,
    DHCPV6_RENEW = 5,
    DHCPV6_REBIND = 6,
    DHCPV6_REPLY = 7,
    DHCPV6_RELEASE = 8,
    DHCPV6_DECLINE = 9,
    DHCPV6_RECONFIGURE = 10,
    DHCPV6_INFORMATION_REQUEST = 11,
    DHCPV6_RELAY_FORW = 12,
    DHCPV6_RELAY_REPL = 13
};

enum D6OptionType {
    D6O_CLIENTID = 1,
    D6O_SERVERID = 2,
    D6O_RAPID_COMMIT = 14,
    D6O_IA_PD = 25
};

}
}

#endif