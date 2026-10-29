/**
 * @file lwip_compat.h
 * @brief Compatibility layer for lwIP API changes between versions
 *
 * ESP-IDF v5.3+ uses lwIP 2.2.0 which changed ip_addr_t structure
 */

#ifndef LWIP_COMPAT_H
#define LWIP_COMPAT_H

#include "lwip/ip_addr.h"

// Detect lwIP version
#if LWIP_IPV4 && LWIP_IPV6
    // Dual stack - newer lwIP uses u_addr union
    #define LWIP_USES_U_ADDR 1
#else
    #define LWIP_USES_U_ADDR 0
#endif

// Compatibility macros for IP address access
#if LWIP_USES_U_ADDR
    // New lwIP (v5.3+): ip_addr_t has u_addr union
    #define IP_ADDR_GET_IP4_U32(ipaddr)     ((ipaddr)->u_addr.ip4.addr)
    #define IP_ADDR_SET_IP4_U32(ipaddr, val) do { (ipaddr)->u_addr.ip4.addr = (val); (ipaddr)->type = IPADDR_TYPE_V4; } while(0)
    #define IP_ADDR_TO_IP4(ipaddr)          (&((ipaddr)->u_addr.ip4))
    #define IP_ADDR_TO_IP4_CONST(ipaddr)    ((const ip4_addr_t*)(&((ipaddr)->u_addr.ip4)))

    // Wrapper for ip_addr_netcmp that converts ip_addr_t* mask to ip4_addr_t*
    #define IP_ADDR_NETCMP_COMPAT(addr1, addr2, mask) \
        ip_addr_netcmp(addr1, addr2, IP_ADDR_TO_IP4_CONST(mask))
#else
    // Old lwIP: ip_addr_t has direct addr field
    #define IP_ADDR_GET_IP4_U32(ipaddr)     ((ipaddr)->addr)
    #define IP_ADDR_SET_IP4_U32(ipaddr, val) ((ipaddr)->addr = (val))
    #define IP_ADDR_TO_IP4(ipaddr)          ((ip4_addr_t*)(ipaddr))
    #define IP_ADDR_TO_IP4_CONST(ipaddr)    ((const ip4_addr_t*)(ipaddr))

    // Old lwIP doesn't need special handling
    #define IP_ADDR_NETCMP_COMPAT(addr1, addr2, mask) \
        ip_addr_netcmp(addr1, addr2, mask)
#endif

#endif /* LWIP_COMPAT_H */
