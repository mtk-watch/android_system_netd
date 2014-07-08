/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RouteController.h"

#include "Fwmark.h"
#include "UidRanges.h"

#define LOG_TAG "Netd"
#include "log/log.h"
#include "logwrap/logwrap.h"
#include "resolv_netid.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/fib_rules.h>
#include <map>
#include <net/if.h>
#include <sys/stat.h>

namespace {

// BEGIN CONSTANTS --------------------------------------------------------------------------------

const uint32_t RULE_PRIORITY_VPN_OVERRIDE_SYSTEM = 10000;
// const uint32_t RULE_PRIORITY_VPN_OVERRIDE_LOCAL  = 11000;
const uint32_t RULE_PRIORITY_SECURE_VPN          = 12000;
const uint32_t RULE_PRIORITY_EXPLICIT_NETWORK    = 13000;
const uint32_t RULE_PRIORITY_OUTPUT_INTERFACE    = 14000;
const uint32_t RULE_PRIORITY_LEGACY_SYSTEM       = 15000;
const uint32_t RULE_PRIORITY_LEGACY_NETWORK      = 16000;
// const uint32_t RULE_PRIORITY_LOCAL_NETWORK       = 17000;
// const uint32_t RULE_PRIORITY_TETHERING           = 18000;
const uint32_t RULE_PRIORITY_IMPLICIT_NETWORK    = 19000;
// const uint32_t RULE_PRIORITY_BYPASSABLE_VPN      = 20000;
// const uint32_t RULE_PRIORITY_VPN_FALLTHROUGH     = 21000;
const uint32_t RULE_PRIORITY_DEFAULT_NETWORK     = 22000;
const uint32_t RULE_PRIORITY_DIRECTLY_CONNECTED  = 23000;
const uint32_t RULE_PRIORITY_UNREACHABLE         = 24000;

const uint32_t ROUTE_TABLE_LEGACY_NETWORK = 98;
const uint32_t ROUTE_TABLE_LEGACY_SYSTEM  = 99;

const char* const ROUTE_TABLE_NAME_LEGACY_NETWORK = "legacy_network";
const char* const ROUTE_TABLE_NAME_LEGACY_SYSTEM  = "legacy_system";

// TODO: These values aren't defined by the Linux kernel, because our UID routing changes are not
// upstream (yet?), so we can't just pick them up from kernel headers. When (if?) the changes make
// it upstream, we'll remove this and rely on the kernel header values. For now, add a static assert
// that will warn us if upstream has given these values some other meaning.
const uint16_t FRA_UID_START = 18;
const uint16_t FRA_UID_END   = 19;
static_assert(FRA_UID_START > FRA_MAX,
             "Android-specific FRA_UID_{START,END} values also assigned in Linux uapi. "
             "Check that these values match what the kernel does and then update this assertion.");

const uint16_t NETLINK_REQUEST_FLAGS = NLM_F_REQUEST | NLM_F_ACK;
const uint16_t NETLINK_CREATE_REQUEST_FLAGS = NETLINK_REQUEST_FLAGS | NLM_F_CREATE | NLM_F_EXCL;

const sockaddr_nl NETLINK_ADDRESS = {AF_NETLINK, 0, 0, 0};

const uint8_t AF_FAMILIES[] = {AF_INET, AF_INET6};

const char* const IP_VERSIONS[] = {"-4", "-6"};

const uid_t UID_ROOT = 0;
const char* const OIF_NONE = NULL;
const bool ACTION_ADD = true;
const bool ACTION_DEL = false;
const bool MODIFY_NON_UID_BASED_RULES = true;

const char* const RT_TABLES_PATH = "/data/misc/net/rt_tables";

// Avoids "non-constant-expression cannot be narrowed from type 'unsigned int' to 'unsigned short'"
// warnings when using RTA_LENGTH(x) inside static initializers (even when x is already uint16_t).
constexpr uint16_t U16_RTA_LENGTH(uint16_t x) {
    return RTA_LENGTH(x);
}

// These are practically const, but can't be declared so, because they are used to initialize
// non-const pointers ("void* iov_base") in iovec arrays.
rtattr FRATTR_PRIORITY  = { U16_RTA_LENGTH(sizeof(uint32_t)), FRA_PRIORITY };
rtattr FRATTR_TABLE     = { U16_RTA_LENGTH(sizeof(uint32_t)), FRA_TABLE };
rtattr FRATTR_FWMARK    = { U16_RTA_LENGTH(sizeof(uint32_t)), FRA_FWMARK };
rtattr FRATTR_FWMASK    = { U16_RTA_LENGTH(sizeof(uint32_t)), FRA_FWMASK };
rtattr FRATTR_UID_START = { U16_RTA_LENGTH(sizeof(uid_t)),    FRA_UID_START };
rtattr FRATTR_UID_END   = { U16_RTA_LENGTH(sizeof(uid_t)),    FRA_UID_END };

rtattr RTATTR_TABLE     = { U16_RTA_LENGTH(sizeof(uint32_t)), RTA_TABLE };
rtattr RTATTR_OIF       = { U16_RTA_LENGTH(sizeof(uint32_t)), RTA_OIF };

uint8_t PADDING_BUFFER[RTA_ALIGNTO] = {0, 0, 0, 0};

// END CONSTANTS ----------------------------------------------------------------------------------

// No locks needed because RouteController is accessed only from one thread (in CommandListener).
std::map<std::string, uint32_t> interfaceToTable;

uint32_t getRouteTableForInterface(const char* interface) {
    uint32_t index = if_nametoindex(interface);
    if (index) {
        index += RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX;
        interfaceToTable[interface] = index;
        return index;
    }
    // If the interface goes away if_nametoindex() will return 0 but we still need to know
    // the index so we can remove the rules and routes.
    auto iter = interfaceToTable.find(interface);
    if (iter == interfaceToTable.end()) {
        ALOGE("cannot find interface %s", interface);
        return RT_TABLE_UNSPEC;
    }
    return iter->second;
}

void addTableName(uint32_t table, const std::string& name, std::string* contents) {
    char tableString[UINT32_STRLEN];
    snprintf(tableString, sizeof(tableString), "%u", table);
    *contents += tableString;
    *contents += " ";
    *contents += name;
    *contents += "\n";
}

// Doesn't return success/failure as the file is optional; it's okay if we fail to update it.
void updateTableNamesFile() {
    std::string contents;
    addTableName(ROUTE_TABLE_LEGACY_NETWORK, ROUTE_TABLE_NAME_LEGACY_NETWORK, &contents);
    addTableName(ROUTE_TABLE_LEGACY_SYSTEM,  ROUTE_TABLE_NAME_LEGACY_SYSTEM,  &contents);
    for (const auto& entry : interfaceToTable) {
        addTableName(entry.second, entry.first, &contents);
    }

    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  // mode 0644, rw-r--r--
    int fd = open(RT_TABLES_PATH, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd == -1) {
        ALOGE("failed to create %s (%s)", RT_TABLES_PATH, strerror(errno));
        return;
    }
    // File creation is affected by umask, so make sure the right mode bits are set.
    if (fchmod(fd, mode) == -1) {
        ALOGE("failed to chmod %s to mode 0%o (%s)", RT_TABLES_PATH, mode, strerror(errno));
    }
    ssize_t bytesWritten = write(fd, contents.data(), contents.size());
    if (bytesWritten != static_cast<ssize_t>(contents.size())) {
        ALOGE("failed to write to %s (%zd vs %zu bytes) (%s)", RT_TABLES_PATH, bytesWritten,
              contents.size(), strerror(errno));
    }
    close(fd);
}

// Sends a netlink request and expects an ack.
// |iov| is an array of struct iovec that contains the netlink message payload.
// The netlink header is generated by this function based on |action| and |flags|.
// Returns -errno if there was an error or if the kernel reported an error.
WARN_UNUSED_RESULT int sendNetlinkRequest(uint16_t action, uint16_t flags, iovec* iov, int iovlen) {
    nlmsghdr nlmsg = {
        .nlmsg_type = action,
        .nlmsg_flags = flags,
    };
    iov[0].iov_base = &nlmsg;
    iov[0].iov_len = sizeof(nlmsg);
    for (int i = 0; i < iovlen; ++i) {
        nlmsg.nlmsg_len += iov[i].iov_len;
    }

    int ret;
    struct {
        nlmsghdr msg;
        nlmsgerr err;
    } response;

    int sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    if (sock != -1 &&
            connect(sock, reinterpret_cast<const sockaddr*>(&NETLINK_ADDRESS),
                    sizeof(NETLINK_ADDRESS)) != -1 &&
            writev(sock, iov, iovlen) != -1 &&
            (ret = recv(sock, &response, sizeof(response), 0)) != -1) {
        if (ret == sizeof(response)) {
            ret = response.err.error;  // Netlink errors are negative errno.
            if (ret) {
                ALOGE("netlink response contains error (%s)", strerror(-ret));
            }
        } else {
            ALOGE("bad netlink response message size (%d != %zu)", ret, sizeof(response));
            ret = -EBADMSG;
        }
    } else {
        ALOGE("netlink socket/connect/writev/recv failed (%s)", strerror(errno));
        ret = -errno;
    }

    if (sock != -1) {
        close(sock);
    }

    return ret;
}

// Adds or removes a routing rule for IPv4 and IPv6.
//
// + If |table| is non-zero, the rule points at the specified routing table. Otherwise, the rule
//   returns ENETUNREACH.
// + If |mask| is non-zero, the rule matches the specified fwmark and mask. Otherwise, |fwmark| is
//   ignored.
// + If |interface| is non-NULL, the rule matches the specified outgoing interface.
//
// Returns 0 on success or negative errno on failure.
WARN_UNUSED_RESULT int modifyIpRule(uint16_t action, uint32_t priority, uint32_t table,
                                    uint32_t fwmark, uint32_t mask, const char* interface,
                                    uid_t uidStart, uid_t uidEnd) {
    // Ensure that if you set a bit in the fwmark, it's not being ignored by the mask.
    if (fwmark & ~mask) {
        ALOGE("mask 0x%x does not select all the bits set in fwmark 0x%x", mask, fwmark);
        return -ERANGE;
    }

    // The interface name must include exactly one terminating NULL and be properly padded, or older
    // kernels will refuse to delete rules.
    uint16_t paddingLength = 0;
    size_t interfaceLength = 0;
    char oifname[IFNAMSIZ];
    if (interface != OIF_NONE) {
        interfaceLength = strlcpy(oifname, interface, IFNAMSIZ) + 1;
        if (interfaceLength > IFNAMSIZ) {
            ALOGE("interface name too long (%zu > %u)", interfaceLength, IFNAMSIZ);
            return -ENAMETOOLONG;
        }
        paddingLength = RTA_SPACE(interfaceLength) - RTA_LENGTH(interfaceLength);
    }

    // Either both start and end UID must be specified, or neither.
    if ((uidStart == INVALID_UID) != (uidEnd == INVALID_UID)) {
        ALOGE("incompatible start and end UIDs (%u vs %u)", uidStart, uidEnd);
        return -EUSERS;
    }
    bool isUidRule = (uidStart != INVALID_UID);

    // Assemble a rule request and put it in an array of iovec structures.
    fib_rule_hdr rule = {
        .action = static_cast<uint8_t>(table != RT_TABLE_UNSPEC ? FR_ACT_TO_TBL :
                                                                  FR_ACT_UNREACHABLE),
    };

    rtattr fraOifname = { U16_RTA_LENGTH(interfaceLength), FRA_OIFNAME };

    iovec iov[] = {
        { NULL,              0 },
        { &rule,             sizeof(rule) },
        { &FRATTR_PRIORITY,  sizeof(FRATTR_PRIORITY) },
        { &priority,         sizeof(priority) },
        { &FRATTR_TABLE,     table != RT_TABLE_UNSPEC ? sizeof(FRATTR_TABLE) : 0 },
        { &table,            table != RT_TABLE_UNSPEC ? sizeof(table) : 0 },
        { &FRATTR_FWMARK,    mask ? sizeof(FRATTR_FWMARK) : 0 },
        { &fwmark,           mask ? sizeof(fwmark) : 0 },
        { &FRATTR_FWMASK,    mask ? sizeof(FRATTR_FWMASK) : 0 },
        { &mask,             mask ? sizeof(mask) : 0 },
        { &FRATTR_UID_START, isUidRule ? sizeof(FRATTR_UID_START) : 0 },
        { &uidStart,         isUidRule ? sizeof(uidStart) : 0 },
        { &FRATTR_UID_END,   isUidRule ? sizeof(FRATTR_UID_END) : 0 },
        { &uidEnd,           isUidRule ? sizeof(uidEnd) : 0 },
        { &fraOifname,       interface != OIF_NONE ? sizeof(fraOifname) : 0 },
        { oifname,           interfaceLength },
        { PADDING_BUFFER,    paddingLength },
    };

    uint16_t flags = (action == RTM_NEWRULE) ? NETLINK_CREATE_REQUEST_FLAGS : NETLINK_REQUEST_FLAGS;
    for (size_t i = 0; i < ARRAY_SIZE(AF_FAMILIES); ++i) {
        rule.family = AF_FAMILIES[i];
        if (int ret = sendNetlinkRequest(action, flags, iov, ARRAY_SIZE(iov))) {
            return ret;
        }
    }

    return 0;
}

// Adds or deletes an IPv4 or IPv6 route.
// Returns 0 on success or negative errno on failure.
WARN_UNUSED_RESULT int modifyIpRoute(uint16_t action, uint32_t table, const char* interface,
                                     const char* destination, const char* nexthop) {
    // At least the destination must be non-null.
    if (!destination) {
        ALOGE("null destination");
        return -EFAULT;
    }

    // Parse the prefix.
    uint8_t rawAddress[sizeof(in6_addr)];
    uint8_t family;
    uint8_t prefixLength;
    int rawLength = parsePrefix(destination, &family, rawAddress, sizeof(rawAddress),
                                &prefixLength);
    if (rawLength < 0) {
        ALOGE("parsePrefix failed for destination %s (%s)", destination, strerror(-rawLength));
        return rawLength;
    }

    if (static_cast<size_t>(rawLength) > sizeof(rawAddress)) {
        ALOGE("impossible! address too long (%d vs %zu)", rawLength, sizeof(rawAddress));
        return -ENOBUFS;  // Cannot happen; parsePrefix only supports IPv4 and IPv6.
    }

    // If an interface was specified, find the ifindex.
    uint32_t ifindex;
    if (interface != OIF_NONE) {
        ifindex = if_nametoindex(interface);
        if (!ifindex) {
            ALOGE("cannot find interface %s", interface);
            return -ENODEV;
        }
    }

    // If a nexthop was specified, parse it as the same family as the prefix.
    uint8_t rawNexthop[sizeof(in6_addr)];
    if (nexthop && inet_pton(family, nexthop, rawNexthop) <= 0) {
        ALOGE("inet_pton failed for nexthop %s", nexthop);
        return -EINVAL;
    }

    // Assemble a rtmsg and put it in an array of iovec structures.
    rtmsg route = {
        .rtm_protocol = RTPROT_STATIC,
        .rtm_type = RTN_UNICAST,
        .rtm_family = family,
        .rtm_dst_len = prefixLength,
    };

    rtattr rtaDst     = { U16_RTA_LENGTH(rawLength), RTA_DST };
    rtattr rtaGateway = { U16_RTA_LENGTH(rawLength), RTA_GATEWAY };

    iovec iov[] = {
        { NULL,          0 },
        { &route,        sizeof(route) },
        { &RTATTR_TABLE, sizeof(RTATTR_TABLE) },
        { &table,        sizeof(table) },
        { &rtaDst,       sizeof(rtaDst) },
        { rawAddress,    static_cast<size_t>(rawLength) },
        { &RTATTR_OIF,   interface != OIF_NONE ? sizeof(RTATTR_OIF) : 0 },
        { &ifindex,      interface != OIF_NONE ? sizeof(ifindex) : 0 },
        { &rtaGateway,   nexthop ? sizeof(rtaGateway) : 0 },
        { rawNexthop,    nexthop ? static_cast<size_t>(rawLength) : 0 },
    };

    uint16_t flags = (action == RTM_NEWROUTE) ? NETLINK_CREATE_REQUEST_FLAGS :
                                                NETLINK_REQUEST_FLAGS;
    return sendNetlinkRequest(action, flags, iov, ARRAY_SIZE(iov));
}

// Add rules to allow legacy routes added through the requestRouteToHost() API.
WARN_UNUSED_RESULT int AddLegacyRouteRules() {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;

    // Rules to allow legacy routes to override the default network.
    if (int ret = modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_LEGACY_SYSTEM, ROUTE_TABLE_LEGACY_SYSTEM,
                               fwmark.intValue, mask.intValue, OIF_NONE, INVALID_UID,
                               INVALID_UID)) {
        return ret;
    }
    if (int ret = modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_LEGACY_NETWORK,
                               ROUTE_TABLE_LEGACY_NETWORK, fwmark.intValue,
                               mask.intValue, OIF_NONE, INVALID_UID, INVALID_UID)) {
        return ret;
    }

    fwmark.permission = PERMISSION_SYSTEM;
    mask.permission = PERMISSION_SYSTEM;

    // A rule to allow legacy routes from system apps to override VPNs.
    return modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_VPN_OVERRIDE_SYSTEM, ROUTE_TABLE_LEGACY_SYSTEM,
                        fwmark.intValue, mask.intValue, OIF_NONE, INVALID_UID, INVALID_UID);
}

// Add a new rule to look up the 'main' table, with the same selectors as the "default network"
// rule, but with a lower priority. Since the default network rule points to a table with a default
// route, the rule we're adding will never be used for normal routing lookups. However, the kernel
// may fall-through to it to find directly-connected routes when it validates that a nexthop (in a
// route being added) is reachable.
WARN_UNUSED_RESULT int AddDirectlyConnectedRule() {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = NETID_UNSET;
    mask.netId = FWMARK_NET_ID_MASK;

    return modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_DIRECTLY_CONNECTED, RT_TABLE_MAIN,
                        fwmark.intValue, mask.intValue, OIF_NONE, UID_ROOT, UID_ROOT);
}

// Add a rule to preempt the pre-defined "from all lookup main" rule. Packets that reach this rule
// will be null-routed, and won't fall-through to the main table.
WARN_UNUSED_RESULT int AddUnreachableRule() {
    return modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_UNREACHABLE, RT_TABLE_UNSPEC, MARK_UNSET,
                        MARK_UNSET, OIF_NONE, INVALID_UID, INVALID_UID);
}

// An iptables rule to mark incoming packets on a network with the netId of the network.
//
// This is so that the kernel can:
// + Use the right fwmark for (and thus correctly route) replies (e.g.: TCP RST, ICMP errors, ping
//   replies, SYN-ACKs, etc).
// + Mark sockets that accept connections from this interface so that the connection stays on the
//   same interface.
WARN_UNUSED_RESULT int modifyIncomingPacketMark(unsigned netId, const char* interface,
                                                Permission permission, bool add) {
    Fwmark fwmark;

    fwmark.netId = netId;
    fwmark.explicitlySelected = true;
    fwmark.protectedFromVpn = true;
    fwmark.permission = permission;

    char markString[UINT32_HEX_STRLEN];
    snprintf(markString, sizeof(markString), "0x%x", fwmark.intValue);

    if (execIptables(V4V6, "-t", "mangle", add ? "-A" : "-D", "INPUT", "-i", interface, "-j",
                     "MARK", "--set-mark", markString, NULL)) {
        ALOGE("failed to change iptables rule that sets incoming packet mark");
        return -EREMOTEIO;
    }

    return 0;
}

// A rule to route traffic based on an explicitly chosen network.
//
// Supports apps that use the multinetwork APIs to restrict their traffic to a network.
//
// Even though we check permissions at the time we set a netId into the fwmark of a socket, we need
// to check it again in the rules here, because a network's permissions may have been updated via
// modifyNetworkPermission().
WARN_UNUSED_RESULT int modifyExplicitNetworkRule(unsigned netId, uint32_t table,
                                                 Permission permission, uid_t uidStart,
                                                 uid_t uidEnd, bool add) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.explicitlySelected = true;
    mask.explicitlySelected = true;

    fwmark.permission = permission;
    mask.permission = permission;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_EXPLICIT_NETWORK, table,
                        fwmark.intValue, mask.intValue, OIF_NONE, uidStart, uidEnd);
}

// A rule to route traffic based on a chosen outgoing interface.
//
// Supports apps that use SO_BINDTODEVICE or IP_PKTINFO options and the kernel that already knows
// the outgoing interface (typically for link-local communications).
WARN_UNUSED_RESULT int modifyOutputInterfaceRule(const char* interface, uint32_t table,
                                                 Permission permission, uid_t uidStart,
                                                 uid_t uidEnd, bool add) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.permission = permission;
    mask.permission = permission;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_OUTPUT_INTERFACE, table,
                        fwmark.intValue, mask.intValue, interface, uidStart, uidEnd);
}

// A rule to route traffic based on the chosen network.
//
// This is for sockets that have not explicitly requested a particular network, but have been
// bound to one when they called connect(). This ensures that sockets connected on a particular
// network stay on that network even if the default network changes.
WARN_UNUSED_RESULT int modifyImplicitNetworkRule(unsigned netId, uint32_t table,
                                                 Permission permission, bool add) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;

    fwmark.permission = permission;
    mask.permission = permission;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_IMPLICIT_NETWORK, table,
                        fwmark.intValue, mask.intValue, OIF_NONE, INVALID_UID, INVALID_UID);
}

// A rule to route all traffic from a given set of UIDs to go over the VPN.
//
// Notice that this rule doesn't use the netId. I.e., no matter what netId the user's socket may
// have, if they are subject to this VPN, their traffic has to go through it. Allows the traffic to
// bypass the VPN if the protectedFromVpn bit is set.
WARN_UNUSED_RESULT int modifyVpnUidRangeRule(uint32_t table, uid_t uidStart, uid_t uidEnd,
                                             bool add) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.protectedFromVpn = false;
    mask.protectedFromVpn = true;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_SECURE_VPN, table,
                        fwmark.intValue, mask.intValue, OIF_NONE, uidStart, uidEnd);
}

// A rule to allow system apps to send traffic over this VPN even if they are not part of the target
// set of UIDs.
//
// This is needed for DnsProxyListener to correctly resolve a request for a user who is in the
// target set, but where the DnsProxyListener itself is not.
WARN_UNUSED_RESULT int modifyVpnSystemPermissionRule(unsigned netId, uint32_t table, bool add) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.permission = PERMISSION_SYSTEM;
    mask.permission = PERMISSION_SYSTEM;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_SECURE_VPN, table,
                        fwmark.intValue, mask.intValue, OIF_NONE, INVALID_UID, INVALID_UID);
}

WARN_UNUSED_RESULT int modifyPhysicalNetwork(unsigned netId, const char* interface,
                                             Permission permission, bool add) {
    uint32_t table = getRouteTableForInterface(interface);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    if (int ret = modifyIncomingPacketMark(netId, interface, permission, add)) {
        return ret;
    }
    if (int ret = modifyExplicitNetworkRule(netId, table, permission, INVALID_UID, INVALID_UID,
                                            add)) {
        return ret;
    }
    if (int ret = modifyOutputInterfaceRule(interface, table, permission, INVALID_UID, INVALID_UID,
                                            add)) {
        return ret;
    }
    return modifyImplicitNetworkRule(netId, table, permission, add);
}

WARN_UNUSED_RESULT int modifyVirtualNetwork(unsigned netId, const char* interface,
                                            const UidRanges& uidRanges, bool add,
                                            bool modifyNonUidBasedRules) {
    uint32_t table = getRouteTableForInterface(interface);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    for (const UidRanges::Range& range : uidRanges.getRanges()) {
        if (int ret = modifyExplicitNetworkRule(netId, table, PERMISSION_NONE, range.first,
                                                range.second, add)) {
            return ret;
        }
        if (int ret = modifyOutputInterfaceRule(interface, table, PERMISSION_NONE, range.first,
                                                range.second, add)) {
            return ret;
        }
        if (int ret = modifyVpnUidRangeRule(table, range.first, range.second, add)) {
            return ret;
        }
    }

    if (modifyNonUidBasedRules) {
        if (int ret = modifyIncomingPacketMark(netId, interface, PERMISSION_NONE, add)) {
            return ret;
        }
        if (int ret = modifyExplicitNetworkRule(netId, table, PERMISSION_NONE, UID_ROOT, UID_ROOT,
                                                add)) {
            return ret;
        }
        return modifyVpnSystemPermissionRule(netId, table, add);
    }

    return 0;
}

WARN_UNUSED_RESULT int modifyDefaultNetwork(uint16_t action, const char* interface,
                                            Permission permission) {
    uint32_t table = getRouteTableForInterface(interface);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = NETID_UNSET;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.permission = permission;
    mask.permission = permission;

    return modifyIpRule(action, RULE_PRIORITY_DEFAULT_NETWORK, table, fwmark.intValue,
                        mask.intValue, OIF_NONE, INVALID_UID, INVALID_UID);
}

// Adds or removes an IPv4 or IPv6 route to the specified table and, if it's a directly-connected
// route, to the main table as well.
// Returns 0 on success or negative errno on failure.
WARN_UNUSED_RESULT int modifyRoute(uint16_t action, const char* interface, const char* destination,
                                   const char* nexthop, RouteController::TableType tableType) {
    uint32_t table;
    switch (tableType) {
        case RouteController::INTERFACE: {
            table = getRouteTableForInterface(interface);
            if (table == RT_TABLE_UNSPEC) {
                return -ESRCH;
            }
            break;
        }
        case RouteController::LEGACY_NETWORK: {
            table = ROUTE_TABLE_LEGACY_NETWORK;
            break;
        }
        case RouteController::LEGACY_SYSTEM: {
            table = ROUTE_TABLE_LEGACY_SYSTEM;
            break;
        }
    }

    int ret = modifyIpRoute(action, table, interface, destination, nexthop);
    // We allow apps to call requestRouteToHost() multiple times with the same route, so ignore
    // EEXIST failures when adding routes to legacy tables.
    if (ret && !(action == RTM_NEWROUTE && ret == -EEXIST &&
                 tableType != RouteController::INTERFACE)) {
        return ret;
    }

    // If there's no nexthop, this is a directly connected route. Add it to the main table also, to
    // let the kernel find it when validating nexthops when global routes are added.
    if (!nexthop) {
        ret = modifyIpRoute(action, RT_TABLE_MAIN, interface, destination, NULL);
        // A failure with action == ADD && errno == EEXIST means that the route already exists in
        // the main table, perhaps because the kernel added it automatically as part of adding the
        // IP address to the interface. Ignore this, but complain about everything else.
        if (ret && !(action == RTM_NEWROUTE && ret == -EEXIST)) {
            return ret;
        }
    }

    return 0;
}

// Returns 0 on success or negative errno on failure.
WARN_UNUSED_RESULT int flushRoutes(const char* interface) {
    uint32_t table = getRouteTableForInterface(interface);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    char tableString[UINT32_STRLEN];
    snprintf(tableString, sizeof(tableString), "%u", table);

    for (size_t i = 0; i < ARRAY_SIZE(IP_VERSIONS); ++i) {
        const char* argv[] = {
            IP_PATH,
            IP_VERSIONS[i],
            "route",
            "flush",
            "table",
            tableString,
        };
        if (android_fork_execvp(ARRAY_SIZE(argv), const_cast<char**>(argv), NULL, false, false)) {
            ALOGE("failed to flush routes");
            return -EREMOTEIO;
        }
    }

    interfaceToTable.erase(interface);
    return 0;
}

}  // namespace

int RouteController::Init() {
    if (int ret = AddDirectlyConnectedRule()) {
        return ret;
    }
    if (int ret = AddLegacyRouteRules()) {
        return ret;
    }
    // TODO: Enable once we are sure everything works.
    if (false) {
        if (int ret = AddUnreachableRule()) {
            return ret;
        }
    }
    updateTableNamesFile();
    return 0;
}

int RouteController::addInterfaceToPhysicalNetwork(unsigned netId, const char* interface,
                                                   Permission permission) {
    if (int ret = modifyPhysicalNetwork(netId, interface, permission, ACTION_ADD)) {
        return ret;
    }
    updateTableNamesFile();
    return 0;
}

int RouteController::removeInterfaceFromPhysicalNetwork(unsigned netId, const char* interface,
                                                        Permission permission) {
    if (int ret = modifyPhysicalNetwork(netId, interface, permission, ACTION_DEL)) {
        return ret;
    }
    if (int ret = flushRoutes(interface)) {
        return ret;
    }
    updateTableNamesFile();
    return 0;
}

int RouteController::addInterfaceToVirtualNetwork(unsigned netId, const char* interface,
                                                  const UidRanges& uidRanges) {
    if (int ret = modifyVirtualNetwork(netId, interface, uidRanges, ACTION_ADD,
                                       MODIFY_NON_UID_BASED_RULES)) {
        return ret;
    }
    updateTableNamesFile();
    return 0;
}

int RouteController::removeInterfaceFromVirtualNetwork(unsigned netId, const char* interface,
                                                       const UidRanges& uidRanges) {
    if (int ret = modifyVirtualNetwork(netId, interface, uidRanges, ACTION_DEL,
                                       MODIFY_NON_UID_BASED_RULES)) {
        return ret;
    }
    if (int ret = flushRoutes(interface)) {
        return ret;
    }
    updateTableNamesFile();
    return 0;
}

int RouteController::modifyPhysicalNetworkPermission(unsigned netId, const char* interface,
                                                     Permission oldPermission,
                                                     Permission newPermission) {
    // Add the new rules before deleting the old ones, to avoid race conditions.
    if (int ret = modifyPhysicalNetwork(netId, interface, newPermission, ACTION_ADD)) {
        return ret;
    }
    return modifyPhysicalNetwork(netId, interface, oldPermission, ACTION_DEL);
}

int RouteController::addUsersToVirtualNetwork(unsigned netId, const char* interface,
                                              const UidRanges& uidRanges) {
    return modifyVirtualNetwork(netId, interface, uidRanges, ACTION_ADD,
                                !MODIFY_NON_UID_BASED_RULES);
}

int RouteController::removeUsersFromVirtualNetwork(unsigned netId, const char* interface,
                                                   const UidRanges& uidRanges) {
    return modifyVirtualNetwork(netId, interface, uidRanges, ACTION_DEL,
                                !MODIFY_NON_UID_BASED_RULES);
}

int RouteController::addInterfaceToDefaultNetwork(const char* interface, Permission permission) {
    return modifyDefaultNetwork(RTM_NEWRULE, interface, permission);
}

int RouteController::removeInterfaceFromDefaultNetwork(const char* interface,
                                                       Permission permission) {
    return modifyDefaultNetwork(RTM_DELRULE, interface, permission);
}

int RouteController::addRoute(const char* interface, const char* destination, const char* nexthop,
                              TableType tableType) {
    return modifyRoute(RTM_NEWROUTE, interface, destination, nexthop, tableType);
}

int RouteController::removeRoute(const char* interface, const char* destination,
                                 const char* nexthop, TableType tableType) {
    return modifyRoute(RTM_DELROUTE, interface, destination, nexthop, tableType);
}