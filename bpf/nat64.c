
/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "headers/vmlinux.h"
#include "headers/bpf_endian.h"
#include "headers/bpf_helpers.h"
#include "headers/bpf_tracing.h"

#define TC_ACT_OK  0
#define TC_ACT_SHOT		2

#define PACKET_HOST		0		/* To us		*/

#define ETH_P_IP   0x0800 /* Internet Protocol packet	*/
#define ETH_P_IPV6 0x86DD
#define ETH_HLEN	14		/* Total octets in header.	 */

// From kernel:include/net/ip.h
#define IP_DF 0x4000  // Flag: "Don't Fragment"

#define TCP_CSUM_OFF (ETH_HLEN + sizeof(struct iphdr) + offsetof(struct tcphdr, check))
#define UDP_CSUM_OFF (ETH_HLEN + sizeof(struct iphdr) + offsetof(struct udphdr, check))

// rfc6052
# define NAT64_PREFIX_0 0x64
# define NAT64_PREFIX_1 0xff
# define NAT64_PREFIX_2 0x9b
# define NAT64_PREFIX_3 0

/* Declare BPF maps */

// Helper forward declarations, so that we can have the most
// important functions code first
static __always_inline bool nat46_valid(const struct __sk_buff *skb);
static __always_inline bool nat64_valid(const struct __sk_buff *skb);

// Create an IPv4 packets using as destination address the last 4 bytes the
// dst IPv6 address with the NAT64 prefix.
// Use as source address the last digit of the soucre address with the 169.254.64.x prefix
// Assume there are less than 254 pods always in the node and that range is empty
SEC("tc/nat64")
int nat64(struct __sk_buff* skb)
{
	void *data = (void *)(long)skb->data;
	const void *data_end = (void *)(long)skb->data_end;
	const struct ethhdr *const eth = data;
	const struct ipv6hdr *const ip6 = (void *)(eth + 1);

	bpf_printk("NAT64: starting");

	// Forward packet if we can't handle it.
	if (!nat64_valid(skb)) {
		bpf_printk("NAT64 packet forwarded: not valid for nat46");
		return TC_ACT_OK;
	}

	bpf_printk("NAT64 IPv6 packet: saddr: %pI6, daddr: %pI6", &ip6->saddr, &ip6->daddr);

	// Save L2 header we got from the input packet before any packet
	// modifications. We will copy it later to the output packet.
	struct ethhdr old_eth;
	old_eth = *eth;
	// Replace the ethertype for a correct one for IPv4 packet.
	old_eth.h_proto = bpf_htons(ETH_P_IP);

	// Build source ip, last byte of the ipv6 address plus the prefix.
	// 169.254.64.xxx
	__u32 new_src = bpf_htonl(0xA9FE4000 + (bpf_ntohl(ip6->saddr.in6_u.u6_addr32[3]) & 0x000000FF));

	// Extract IPv4 address from the last 4 bytes of IPv6 address.
	__u32 new_dst = ip6->daddr.in6_u.u6_addr32[3];

	// Crafting IPv4 packet out of IPv6 start here. Most of it can be
	// derived from IPv6 packet rather easily. Replacing addresses
	// is the least trivial part.
	__be16 tot_len = bpf_htons(bpf_ntohs(ip6->payload_len) + sizeof(struct iphdr));
	struct iphdr ip4 = {
		.version = 4,                                           // u4
		.ihl = sizeof(struct iphdr) / sizeof(__u32),            // u4
		.tos = (ip6->priority << 4) + (ip6->flow_lbl[0] >> 4),  // u8
		.tot_len = tot_len,                                     // u16
		.id = 0,                                                // u16
		.check = 0,                                             // u16
		.frag_off = 0,                                          // u16
	};

	// For whatever cursed reason, verifier is unhappy if these are part
	// of initializer list above, so I guess we need to set values
	// separately.
	ip4.ttl = ip6->hop_limit;
	ip4.protocol = ip6->nexthdr;
	ip4.saddr = new_src;
	ip4.daddr = new_dst;

	// https://mailarchive.ietf.org/arch/msg/behave/JfxCt1fGT66pEtfXKuEDJ8rdd7w/
	if (bpf_ntohs(ip4.tot_len) > 1280)
		ip4.frag_off = bpf_htons(IP_DF);

	// Calculate the IPv4 one's complement checksum of the IPv4 header.
	__wsum sum4 = 0;
	for (int i = 0; i < sizeof(struct iphdr) / sizeof(__u16); ++i) {
		sum4 += ((__u16*)&ip4)[i];
	}
	// Note that sum4 is guaranteed to be non-zero by virtue of ip.version == 4
	sum4 = (sum4 & 0xFFFF) + (sum4 >> 16);  // collapse u32 into range 1 .. 0x1FFFE
	sum4 = (sum4 & 0xFFFF) + (sum4 >> 16);  // collapse any potential carry into u16
	ip4.check = (__u16)~sum4;                // sum4 cannot be zero, so this is never 0xFFFF

	// Calculate L4 checksum difference before any helpers that modify
	// packet's data are called, because verifier will invalidate
	// all packet pointers.
	__u64 l4_csum_diff = 0;
	switch (ip4.protocol) {
		case IPPROTO_UDP:
		case IPPROTO_TCP:
			// Both UDP and TCP use pseudo header for checksum
			// calculation, see https://www.rfc-editor.org/rfc/rfc2460.html#section-8.1.

			// This is non-trivial, so some background on TCP/UDP
			// checksum calculation. Checksum is calculated over
			// pseudo-header, which contains some bits from L3
			// header, and L4 payload. L4 payload does not change
			// between input IPv6 packet and output IPv4 packet, but
			// pseudo-header does indeed change. We could feed
			// bpf_csum_diff with the entire pseudo-headers both from
			// input and output packets and calculate checksum
			// difference this way, but we can afford to be a
			// bit smarter here.
			//
			// TCP / UDP pseudo-header for IPv4
			// (see https://www.rfc-editor.org/rfc/rfc793.html#section-3.1)
			// and for IPv6
			// (see https://www.rfc-editor.org/rfc/rfc2460.html#section-8.1)
			// contain the same information for TCP / UDP (protocol
			// is 6 for TCP, 17 for UDP for both IPv4 and IPv6), but
			// structure of pseudo-header differs - fields are
			// ordered differently and have different sizes. For checksum
			// calculation, this does not matter - all bytes of
			// pseudo-header apart from IP addresses contribute the
			// same value to checksum (first step of calculation is
			// summing all bytes, zeroes does not matter),
			// meaning we only need to run bpf_csum_diff over IP
			// addresses instead of the entire pseudo-header.
			//
			// Last neat piece of info that makes it a one-liner is that both
			// ipv6hdr and iphdr structs have src and dst addresses
			// next to each other in memory. That means we can
			// calculate checksum difference with one bpf_csum_diff
			// call using 2 * size of IP address.
			l4_csum_diff = bpf_csum_diff((void *)&(ip6->saddr), 2*sizeof(struct in6_addr), (void *)&(ip4.saddr), 2*sizeof(__u32), 0);
	}

	// Packet mutations begin - point of no return, but if this first modification fails
	// the packet is probably still pristine, so let clatd handle it.
	// This also takes care of resizing socket buffer to handle different IP
	// header size.
	if (bpf_skb_change_proto(skb, bpf_htons(ETH_P_IP), 0)) {
		bpf_printk("NAT64 packet forwarded: bpf_skb_change_proto failed");
		return TC_ACT_OK;
	}

	// Update L4 checksum using the checksum difference we calculated before.
	int ret = 0;
	switch (ip4.protocol) {
		case IPPROTO_UDP:
			ret = bpf_l4_csum_replace(skb, UDP_CSUM_OFF, 0, l4_csum_diff, BPF_F_PSEUDO_HDR);
			break;
		case IPPROTO_TCP:
			ret = bpf_l4_csum_replace(skb, TCP_CSUM_OFF, 0, l4_csum_diff, BPF_F_PSEUDO_HDR);
			break;
	}

	// If true, updating packet's UDP / TCP checksum failed.
	if (ret < 0) {
		bpf_printk("NAT64 packet dropped: L4 checksum update failed");
		return TC_ACT_SHOT;
	}

	// bpf_skb_change_proto() invalidates all pointers - reload them.
	data = (void*)(long)skb->data;
	data_end = (void*)(long)skb->data_end;
	// I cannot think of any valid way for this error condition to trigger, however I do
	// believe the explicit check is required to keep the in kernel ebpf verifier happy.
	if (data + sizeof(struct ethhdr) + sizeof(struct iphdr) > data_end)
		return TC_ACT_SHOT;

	// Copy over the old ethernet header with updated ethertype.
	ret = bpf_skb_store_bytes(skb, 0, &old_eth, sizeof(struct ethhdr), 0);
	if (ret < 0) {
		bpf_printk("NAT64 packet dropped: copy eth header");
		return TC_ACT_SHOT;
	}
	// Copy over the new ipv4 header.
	ret = bpf_skb_store_bytes(skb, sizeof(struct ethhdr), &ip4, sizeof(struct iphdr), 0);
	if (ret < 0) {
		bpf_printk("NAT64 packet dropped: copy ipv4 header");
	}

	bpf_printk("NAT64 IPv4 packet: saddr: %pI4, daddr: %pI4", &ip4.saddr, &ip4.daddr);
	return bpf_redirect(skb->ifindex, BPF_F_INGRESS);
}

// Build an IPv6 packet from an IPv4 packet
// destination address is pod prefix plus last digit from 169.254.64.x
// source address is the IPv4 src address embedded on the well known NAT64 prefix
SEC("tc/nat46")
static __always_inline int nat46(struct __sk_buff *skb)
{
	void *data = (void *)(long)skb->data;
	const void *data_end = (void *)(long)skb->data_end;
	const struct ethhdr *const eth = data;
	const struct iphdr *const ip4 = (void *)(eth + 1);

	bpf_printk("NAT46 IPv4 packet: saddr: %pI4, daddr: %pI4", &ip4->saddr, &ip4->daddr);

	// Forward packet if we can't handle it.
	if (!nat46_valid(skb)) {
		bpf_printk("NAT46 packet forwarded: not valid for nat46");
		return TC_ACT_OK;
	}

	// Build dest ip, last byte of the ipv6 address plus the pod prefix
	// pod_prefix::xxx.
	 __u32 dst_addr = bpf_ntohl(ip4->daddr) & 0x000000FF;

	struct ipv6hdr ip6 = {
		.version = 6,                                            // __u8:4
		.priority = ip4->tos >> 4,                               // __u8:4
		.flow_lbl = {(ip4->tos & 0xF) << 4, 0, 0},               // __u8[3]
		.payload_len = bpf_htons(bpf_ntohs(ip4->tot_len) - 20),  // __be16
		.hop_limit = ip4->ttl,                                   // __u8
	};
	ip6.nexthdr = ip4->protocol;                                     // __u8
	if (ip4->protocol== IPPROTO_ICMP)
		ip6.nexthdr = IPPROTO_ICMPV6;
	ip6.saddr.in6_u.u6_addr32[0] = bpf_htonl(0x0064ff9b);
	ip6.saddr.in6_u.u6_addr32[1] = 0;
	ip6.saddr.in6_u.u6_addr32[2] = 0;
	ip6.saddr.in6_u.u6_addr32[3] = ip4->saddr;
	ip6.daddr.in6_u.u6_addr32[0] = bpf_htonl(0xfd000010);  // containers subnet
	ip6.daddr.in6_u.u6_addr32[1] = bpf_htonl(0x02440001);  // containers subnet
	ip6.daddr.in6_u.u6_addr32[2] = 0;
	ip6.daddr.in6_u.u6_addr32[3] = bpf_htonl(dst_addr);

	// Calculate L4 checksum difference before any helpers that modify
	// packet's data are called, because verifier will invalidate
	// all packet pointers.
	__u64 l4_csum_diff = 0;
	// Move declarations outside of switch case since declaration
	// as first instruction in case is illegal in C.
	const struct udphdr *uh;
	switch (ip6.nexthdr) {
		case IPPROTO_UDP:
			uh = (const struct udphdr *)(ip4 + 1);
			// If IPv4/UDP checksum is 0 then fallback to clatd so it can calculate the
			// checksum. Otherwise the network or more likely the NAT64 gateway might
			// drop the packet because in most cases IPv6/UDP packets with a zero checksum
			// are invalid. See RFC 6935.
			if (!uh->check) {
				bpf_printk("NAT46 packet forwarded: UDP checksum is 0");
				return TC_ACT_OK;
			}
			// If checksum is not 0, recalculate with bpf_csum_diff.
			// Fallthrough to the next case.
		case IPPROTO_TCP:
			// See comment for nat64 direction to see reasoning behind this.
			l4_csum_diff = bpf_csum_diff((void *)&(ip4->saddr), 2*sizeof(__u32), (void *)&(ip6.saddr), 2*sizeof(struct in6_addr), 0);
	}

	// Save L2 header we got from the input packet before any packet
	// modifications. We will copy it later to the output packet.
	struct ethhdr old_eth;
	old_eth = *eth;
	// Replace the ethertype for a correct one for IPv6 packet.
	old_eth.h_proto = bpf_htons(ETH_P_IPV6);

	// Calculate the IPv6 16-bit one's complement checksum of the IPv6 header.
	__wsum sum6 = 0;
	// We'll end up with a non-zero sum due to ip6.version == 6
	for (uint i = 0; i < sizeof(ip6) / sizeof(__u16); ++i)
		sum6 += ((__u16 *)&ip6)[i];

	// Packet mutations begin - point of no return, but if this first modification fails
	// the packet is probably still pristine, so let clatd handle it.
	// This also takes care of resizing socket buffer to handle different IP
	// header size.
	if (bpf_skb_change_proto(skb, bpf_htons(ETH_P_IPV6), 0)) {
		bpf_printk("NAT46 packet forwarded: bpf_skb_change_proto failed");
		return TC_ACT_OK;
	}

	bpf_csum_update(skb, sum6);

	// Update L4 checksum using the checksum difference we calculated before.
	int ret = 0;
	switch (ip6.nexthdr) {
		case IPPROTO_UDP:
//			ret = bpf_l4_csum_replace(skb, UDP_CSUM_OFF, 0, l4_csum_diff, BPF_F_PSEUDO_HDR);
			break;
		case IPPROTO_TCP:
			ret = bpf_l4_csum_replace(skb, TCP_CSUM_OFF, 0, l4_csum_diff, BPF_F_PSEUDO_HDR);
	}

	// If true, updating packet's UDP / TCP checksum failed.
	if (ret < 0) {
		bpf_printk("NAT46 packet dropped: L4 checksum update failed");
		return TC_ACT_SHOT;
	}

	// bpf_skb_change_proto() invalidates all pointers - reload them.
	data = (void *)(long)skb->data;
	data_end = (void *)(long)skb->data_end;

	// I cannot think of any valid way for this error condition to trigger, however I do
	// believe the explicit check is required to keep the in kernel ebpf verifier happy.
	if (data + sizeof(struct ethhdr) + sizeof(struct ipv6hdr) > data_end)
		return TC_ACT_SHOT;

	// Copy over the old ethernet header with updated ethertype.
	ret = bpf_skb_store_bytes(skb, 0, &old_eth, sizeof(struct ethhdr), 0);
	if (ret < 0) {
		bpf_printk("NAT46 packet dropped: copy eth header");
		return TC_ACT_SHOT;
	}
	// Copy over the new ipv6 header.
	// This takes care of updating the skb->csum field for a CHECKSUM_COMPLETE packet.
	ret = bpf_skb_store_bytes(skb, sizeof(struct ethhdr), &ip6, sizeof(struct ipv6hdr), 0);
	if (ret < 0) {
		bpf_printk("NAT46 packet dropped: copy ipv6 header + csum recompute");
		return TC_ACT_SHOT;
	}

	bpf_printk("NAT46 IPv6 packet: saddr: %pI6, daddr: %pI6", &ip6.saddr, &ip6.daddr);
	return bpf_redirect(skb->ifindex, BPF_F_INGRESS);
}

static __always_inline bool
nat64_valid(const struct __sk_buff *skb) {
	const void *data = (void *)(long)skb->data;
	const void *data_end = (void *)(long)skb->data_end;

	// Require ethernet dst mac address to be our unicast address.
	if (skb->pkt_type != PACKET_HOST)
		return false;

	// Must be meta-ethernet IPv6 frame.
	if (skb->protocol != bpf_htons(ETH_P_IPV6))
		return false;

	// Must have (ethernet and) ipv6 header.
	if (data + sizeof(struct ethhdr) + sizeof(struct ipv6hdr) > data_end)
		return false;

	const struct ethhdr *eth = data;

	// Ethertype - if present - must be IPv6.
	if (eth->h_proto != bpf_htons(ETH_P_IPV6))
		return false;

	const struct ipv6hdr *ip6 = (void *)(eth + 1);

	// IP version must be 6.
	if (ip6->version != 6)
		return false;

	// Maximum IPv6 payload length that can be translated to IPv4.
	if (bpf_ntohs(ip6->payload_len) > 0xFFFF - sizeof(struct iphdr))
		return false;

	// Must be L4 protocol we can support.
	// TODO: Add support for ICMP.
	// TODO: Check what's with IPPROTO_GRE, IPPROTO_ESP, I'm not even sure
	//       what those are.
	switch (ip6->nexthdr) {
		case IPPROTO_TCP:
		case IPPROTO_UDP:
			break;
		default:  // Do not know how to handle anything else.
			return false;
	}

	return true;
}

static __always_inline bool
nat46_valid(const struct __sk_buff *skb) {
	const void *data = (void *)(long)skb->data;
	const void *data_end = (void *)(long)skb->data_end;

	// Must be meta-ethernet IPv4 frame.
	if (skb->protocol != bpf_htons(ETH_P_IP))
		return false;

	// Must have IPv4 header.
	if (data + sizeof(struct ethhdr) + sizeof(struct iphdr) > data_end)
		return false;

	const struct ethhdr *eth = data;

	// Ethertype - if present - must be IPv4.
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return false;

	const struct iphdr *ip4 = (void *)(eth + 1);

	// IP version must be 4.
	if (ip4->version != 4)
		return false;

	// We cannot handle IP options, just standard 20 byte == 5 dword minimal IPv4 header.
	if (ip4->ihl != 5)
		return false;

	// Maximum IPv4 payload length that can be translated to IPv6.
	if (bpf_htons(ip4->tot_len) > 0xFFFF - sizeof(struct ipv6hdr))
		return false;

	// Calculate the IPv4 one's complement checksum of the IPv4 header.
	__wsum sum4 = 0;
	for (uint i = 0; i < sizeof(*ip4) / sizeof(__u16); ++i)
		sum4 += ((__u16 *)ip4)[i];

	// Note that sum4 is guaranteed to be non-zero by virtue of ip4->version == 4
	sum4 = (sum4 & 0xFFFF) + (sum4 >> 16);  // collapse u32 into range 1 .. 0x1FFFE
	sum4 = (sum4 & 0xFFFF) + (sum4 >> 16);  // collapse any potential carry into u16

	// For a correct checksum we should get *a* zero, but sum4 must be positive, ie 0xFFFF
	if (sum4 != 0xFFFF)
		return TC_ACT_OK;

	// Minimum IPv4 total length is the size of the header
	if (bpf_ntohs(ip4->tot_len) < sizeof(*ip4))
		return TC_ACT_OK;

	// We are incapable of dealing with IPv4 fragments
	if (ip4->frag_off & ~bpf_htons(IP_DF))
		return TC_ACT_OK;

	// Must be L4 protocol we can support.
	// TODO: Add support for ICMPv6.
	// TODO: Check what's with IPPROTO_GRE, IPPROTO_ESP, I'm not even sure
	//       what those are.
	switch (ip4->protocol) {
		case IPPROTO_TCP:
			// Must have TCP header.
			if (data + sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct tcphdr) > data_end)
				return false;
			break;
		case IPPROTO_UDP:
			if (data + sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) > data_end)
				return false;
			const struct udphdr *uh = (const struct udphdr *)(ip4 + 1);
			// If IPv4/UDP checksum is 0 then fallback to clatd so it can calculate the
			// checksum.  Otherwise the network or more likely the NAT64 gateway might
			// drop the packet because in most cases IPv6/UDP packets with a zero checksum
			// are invalid. See RFC 6935.
			// TODO: calculate checksum via bpf_csum_diff().
			if (!uh->check)
				return false;

			break;
		default:  // do not know how to handle anything else
			return false;
	}

	return true;
}

char __license[] SEC("license") = "GPL";
