#ifdef __KERNEL__

#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/skbuff.h>
#include <linux/version.h>
#include <net/route.h>
#include <net/ip.h>

#include "packet.h"
#include "security.h"
#include "interfaces_table.h"
#include "security_table.h"

static const char* const superman_packet_type_desc[] = {
	"UNKNOWN",
	"SUPERMAN_DISCOVERY_REQUEST_TYPE",
	"SUPERMAN_CERTIFICATE_REQUEST_TYPE",
	"SUPERMAN_CERTIFICATE_EXCHANGE_TYPE",
	"SUPERMAN_CERTIFICATE_EXCHANGE_WITH_BROADCAST_KEY_TYPE",
	"SUPERMAN_AUTHENTICATED_SK_REQUEST_TYPE",
	"SUPERMAN_AUTHENTICATED_SK_RESPONSE_TYPE",
	"SUPERMAN_SK_INVALIDATE_TYPE",
	"SUPERMAN_BROADCAST_KEY_EXCHANGE_TYPE",
	"SUPERMAN_DATA_PACKET"
};

inline const char* lookup_superman_packet_type_desc(__u8 type)
{
	if(type >= 0 && type <= SUPERMAN_MAX_TYPE)
		return superman_packet_type_desc[type];
	else
		return superman_packet_type_desc[SUPERMAN_MAX_TYPE+1];
}

static inline u_int8_t _encode_ip_protocol(u_int8_t protocol)
{
	return protocol + SUPERMAN_MAX_TYPE;
}

static inline u_int8_t _decode_ip_protocol(u_int8_t superman_protocol)
{
	return superman_protocol - SUPERMAN_MAX_TYPE;
}

/*
struct net_device* lookup_dst(uint32_t addr)
{
	//struct rtable* r = ip_route_output(&init_net, addr, 0, 0);
	struct net_device* dev = ip_dev_find(&init_net, addr);
	
}
*/

inline bool is_superman_packet(struct sk_buff* skb)
{
	// Does this IPv4 packet contain superman payload?
	struct iphdr* iph = (struct iphdr*)skb_network_header(skb);
	return (iph->protocol == SUPERMAN_PROTOCOL_NUM);
}

inline struct superman_header* get_superman_header(struct sk_buff *skb)
{
	return (struct superman_header*)skb_transport_header(skb);
}

unsigned int send_superman_packet(struct superman_packet_info* spi, bool result)
{
	// printk(KERN_INFO "SUPERMAN: Packet (send_superman_packet)...\n");

	if(result)
	{
		struct flowi4 fl;
		struct rtable* rt;
		struct dst_entry* dst;

		printk(KERN_INFO "SUPERMAN: Packet (send_superman_packet) - (%u) %s, %d bytes sending to %u.%u.%u.%u.\n", spi->shdr->type, lookup_superman_packet_type_desc(spi->shdr->type), spi->skb->len, 0x0ff & spi->iph->daddr, 0x0ff & (spi->iph->daddr >> 8), 0x0ff & (spi->iph->daddr >> 16), 0x0ff & (spi->iph->daddr >> 24));

		ip_send_check(spi->iph);

		flowi4_init_output(&fl, spi->skb->dev->ifindex, 0, 0, RT_SCOPE_UNIVERSE, 0, 0, spi->iph->daddr, spi->iph->saddr, 0, 0);
		rt = ip_route_output_key(dev_net(spi->skb->dev), &fl);
		if(IS_ERR(rt))
		{
			printk(KERN_INFO "SUPERMAN: Packet (send_superman_packet) - Routing failed.\n");
			printk(KERN_INFO "if: %d, src: %u.%u.%u.%u, dst: %u.%u.%u.%u\n", spi->skb->dev->ifindex, 0x0ff & spi->iph->saddr, 0x0ff & (spi->iph->saddr >> 8), 0x0ff & (spi->iph->saddr >> 16), 0x0ff & (spi->iph->saddr >> 24), 0x0ff & spi->iph->daddr, 0x0ff & (spi->iph->daddr >> 8), 0x0ff & (spi->iph->daddr >> 16), 0x0ff & (spi->iph->daddr >> 24));
		}
		else
		{
			spi->skb->protocol = htons(ETH_P_IP);
			skb_dst_set(spi->skb, &rt->dst);
			dst = skb_dst(spi->skb);
			spi->skb->dev = dst->dev;

			// printk(KERN_INFO "SUPERMAN: Packet (send_superman_packet) - \t\tSending...\n");
			spi->result = NF_ACCEPT;
			dst->output(NULL, spi->skb);
		}
	}
	else
		printk(KERN_INFO "SUPERMAN: Packet (send_superman_packet) - \t\tFailed crypto.\n");


	return FreeSupermanPacketInfo(spi);
}

unsigned int hash_then_send_superman_packet(struct superman_packet_info* spi, bool result)
{
	if(result)
	{
		return AddP2PSecurity(spi, send_superman_packet);
	}
	else
		printk(KERN_INFO "SUPERMAN: Packet (hash_then_send_superman_packet) - \t\tFailed crypto.\n");

	return FreeSupermanPacketInfo(spi);
}

bool EncapsulatePacket(struct superman_packet_info* spi)
{
	uint32_t iph_len;
	// printk(KERN_INFO "SUPERMAN: Packet - \tEncapsulating packet...\n");

	// printk(KERN_INFO "SUPERMAN: Packet - \tPacket before encapsulation...\n");
	// dump_packet(spi->skb);

	// Don't do this if we already have a header.
	if(!spi->shdr)
	{
		// Make sure we have enough headroom.
		if(skb_headroom(spi->skb) < SUPERMAN_HEADER_LEN)
		{
			struct sk_buff* nskb;

			// printk(KERN_INFO "SUPERMAN: Packet - \t\tExpanding the packet to increase headroom...\n");

			nskb = skb_copy_expand(spi->skb, SUPERMAN_HEADER_LEN, skb_tailroom(spi->skb), GFP_ATOMIC);
			if(nskb == NULL)
			{
				printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
				return false;
			}

			// Set old owner
			if (spi->skb->sk != NULL)
				skb_set_owner_w(nskb, spi->skb->sk);

			// Clean up the old one
			kfree_skb(spi->skb);
			spi->skb = nskb;

			// printk(KERN_INFO "SUPERMAN: Packet - \t\tPacket after expansion...\n");
			// dump_packet(spi->skb);
		}
		skb_linearize(spi->skb);

		// Determine the IP header length
		iph_len = ((struct iphdr*)skb_network_header(spi->skb))->ihl << 2;

		// Allocate some of the headroom to our new header
		skb_push(spi->skb, SUPERMAN_HEADER_LEN);

		// printk(KERN_INFO "SUPERMAN: Packet - \tPacket after skb_push...\n");
		// dump_packet(spi->skb);

		// Move the IP header to the start
		memmove(spi->skb->data, spi->skb->data + SUPERMAN_HEADER_LEN, iph_len);

		// Grab the new IP header reference
		skb_reset_network_header(spi->skb);
		skb_set_transport_header(spi->skb, iph_len);
		spi->iph = (struct iphdr*)skb_network_header(spi->skb);

		// Fill in the superman header
		spi->shdr = (struct superman_header*)skb_transport_header(spi->skb);
		spi->shdr->type = SUPERMAN_MAX_TYPE + spi->iph->protocol;					// We're preparing a superman packet.
		spi->shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(spi->addr)));			// This will be a unique counter value for each packet, cycling round.
		spi->shdr->payload_len = htons(spi->skb->len - iph_len - SUPERMAN_HEADER_LEN);	// The payload length.	
		spi->shdr->last_node = htonl(0);

		// Update the IP header
		spi->iph->protocol = SUPERMAN_PROTOCOL_NUM;							// Our SUPERMAN protocol number
		spi->iph->tot_len = htons(ntohs(spi->iph->tot_len) + SUPERMAN_HEADER_LEN);		// Update the IP packet length
		spi->iph->check = 0;										// No checksum yet
		ip_send_check(spi->iph);

		// printk(KERN_INFO "SUPERMAN: Packet - \tPacket after encapsulation...\n");
		// dump_packet(spi->skb);

	}

	return true;
}

bool DecapsulatePacket(struct superman_packet_info* spi)
{
	uint32_t iph_len;

	// printk(KERN_INFO "SUPERMAN: Packet - \tDecapsulating packet...\n");

	// printk(KERN_INFO "SUPERMAN: Packet - \tPacket before decapsulation...\n");
	// dump_packet(spi->skb);

	// Determine the IP header length
	iph_len = ((struct iphdr*)skb_network_header(spi->skb))->ihl << 2;

	// Update the IP header
	spi->iph->protocol = spi->shdr->type - SUPERMAN_MAX_TYPE;				// Our SUPERMAN protocol number
	spi->iph->tot_len = htons(ntohs(spi->iph->tot_len) - SUPERMAN_HEADER_LEN);	// Update the IP packet length
	spi->iph->check = 0;									// No checksum yet

	// Move the IP header inward to sit next to the payload
	memmove(spi->skb->data + SUPERMAN_HEADER_LEN, spi->skb->data, iph_len);

	// Remove the space at the start of the data, back into the headroom
	skb_pull(spi->skb, SUPERMAN_HEADER_LEN);

	// Update our pointers
	skb_reset_network_header(spi->skb);
	skb_set_transport_header(spi->skb, iph_len);
	spi->iph = (struct iphdr*)skb_network_header(spi->skb);
	spi->shdr = NULL;
	ip_send_check(spi->iph);

	// printk(KERN_INFO "SUPERMAN: Packet - \tPacket after decapsulation...\n");
	// dump_packet(spi->skb);

	return true;
}

void SendDiscoveryRequestPacket(uint32_t sk_len, unsigned char* sk)
{
	struct net_device *dev;

	// printk(KERN_INFO "SUPERMAN: Packet - \tSend Discovery Request...\n");
	
	INTERFACE_ITERATOR_START(dev)

	struct sk_buff* tx_sk;
	struct superman_header* shdr;
	struct iphdr* iph;
	void* payload;
	struct superman_packet_info* spi;

	// printk(KERN_INFO "SUPERMAN: Packet - \tSend Discovery Request on %s...\n", dev->name);

	// Allocate a new packet
	tx_sk = alloc_skb(sizeof(struct iphdr) + SUPERMAN_HEADER_LEN + sk_len, GFP_KERNEL);
	if(tx_sk == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
		continue;
	}
	tx_sk->dev = dev;
	tx_sk->pkt_type = PACKET_OUTGOING | PACKET_BROADCAST;				// Its outgoing.
	tx_sk->ip_summed = CHECKSUM_NONE;						// No need to checksum.

	// Reserve space for the IP and SUPERMAN headers
	skb_reserve(tx_sk, sizeof(struct iphdr) + SUPERMAN_HEADER_LEN);

	// Payload goes here.

	//   2 bytes  |     sk_len
	// -----------------------------
	// |  sk len  |       sk       |
	// -----------------------------

	payload = skb_put(tx_sk, sk_len);
	memcpy(payload, sk, sk_len);

	// Setup the superman header
	shdr = (struct superman_header*) skb_push(tx_sk, SUPERMAN_HEADER_LEN);
	skb_reset_transport_header(tx_sk);
	shdr->type = SUPERMAN_DISCOVERY_REQUEST_TYPE;					// We're preparing a discovery request packet.
	shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(INADDR_BROADCAST)));		// This will be a unique counter value for each packet, cycling round.
	shdr->payload_len = htons(sk_len);						// A discovery request contains an SK.

	// Setup the IP header
	iph = (struct iphdr*) skb_push(tx_sk, sizeof(struct iphdr));
	skb_reset_network_header(tx_sk);
	iph->version = 4;								// IPv4 only, for now.
	iph->ihl = 5;									// Number of 32-bit words in the header (min 5)
	iph->tos = 0;									// Was TOS, now DSCP (Differentiated Services Code Point) - not required.
	iph->tot_len = htons(tx_sk->len);						// Total length of the packet
	iph->frag_off = htons(IP_DF);							// Fragment Offset - this packet is not fragmented
	iph->id = htons(0);								// The identifier is supposed to be a unique value during such that it does not repeat within the maximum datagram lifetime (MDL)
	iph->ttl = 64;									// A recommended value (in seconds)
	iph->protocol = SUPERMAN_PROTOCOL_NUM;						// Our SUPERMAN protocol number
	iph->check = 0;									// No checksum yet
	iph->saddr = inet_select_addr(dev, htonl(INADDR_BROADCAST), RT_SCOPE_UNIVERSE);	// Grab the most appropriate address.
	//iph->daddr = ((iph->saddr & 0x00FFFFFF) + 0xFF000000),			// Broadcast the message to all on the subnet
	iph->daddr = htonl(INADDR_BROADCAST);						// Broadcast the message to all on the subnet

	spi = MallocSupermanPacketInfo(NULL, tx_sk, NULL);
	send_superman_packet(spi, true);

	INTERFACE_ITERATOR_END
	// printk(KERN_INFO "SUPERMAN: Packet - \t... Send Discovery Request done.\n");
}

void SendCertificateRequestPacket(uint32_t addr, uint32_t sk_len, unsigned char* sk)
{
	struct security_table_entry* ste;
	struct net_device *dev;
	struct in_addr;
	struct sk_buff* tx_sk;
	struct superman_header* shdr;
	struct iphdr* iph;
	void* payload;
	struct superman_packet_info* spi;

	// printk(KERN_INFO "SUPERMAN: Packet - \tSend Certificate Request to %u.%u.%u.%u...\n", 0x0ff & addr, 0x0ff & (addr >> 8), 0x0ff & (addr >> 16), 0x0ff & (addr >> 24));

	// Grab some information about the interface.
	if(!GetSecurityTableEntry(addr, &ste))
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tNo device for address %d.%d.%d.%d.\n", 0x0ff & addr, 0x0ff & (addr >> 8), 0x0ff & (addr >> 16), 0x0ff & (addr >> 24));
		return;
	}

	// Grab a device reference. We must dereference later (dev_put).
	dev = dev_get_by_index(&init_net, ste->ifindex);
	if(dev == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tNo device for interface %i.\n", ste->ifindex);
		return;
	}

	// Allocate a new packet
	tx_sk = alloc_skb(sizeof(struct iphdr) + SUPERMAN_HEADER_LEN + sk_len, GFP_KERNEL);
	if(tx_sk == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
		return;
	}
	tx_sk->dev = dev;
	tx_sk->pkt_type = PACKET_OUTGOING;						// Its outgoing.
	tx_sk->ip_summed = CHECKSUM_NONE;						// No need to checksum.

	// Reserve space for the IP and SUPERMAN headers
	skb_reserve(tx_sk, sizeof(struct iphdr) + SUPERMAN_HEADER_LEN);

	// Payload goes here.

	//   2 bytes  |     sk_len
	// -----------------------------
	// |  sk len  |       sk       |
	// -----------------------------

	payload = skb_put(tx_sk, sk_len);
	memcpy(payload, sk, sk_len);

	// Setup the superman header
	shdr = (struct superman_header*) skb_push(tx_sk, SUPERMAN_HEADER_LEN);
	skb_reset_transport_header(tx_sk);
	shdr->type = SUPERMAN_CERTIFICATE_REQUEST_TYPE;					// We're preparing a certificate request packet.
	shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(addr)));		// This will be a unique counter value for each packet, cycling round.
	shdr->payload_len = htons(sk_len);						// A certificate request contains an SK.

	// Setup the IP header
	iph = (struct iphdr*) skb_push(tx_sk, sizeof(struct iphdr));
	skb_reset_network_header(tx_sk);
	iph->version = 4;								// IPv4 only, for now.
	iph->ihl = 5;									// Number of 32-bit words in the header (min 5)
	iph->tos = 0;									// Was TOS, now DSCP (Differentiated Services Code Point) - not required.
	iph->tot_len = htons(tx_sk->len);						// Total length of the packet
	iph->frag_off = htons(IP_DF);							// Fragment Offset - this packet is not fragmented
	iph->id = htons(0);								// The identifier is supposed to be a unique value during such that it does not repeat within the maximum datagram lifetime (MDL)
	iph->ttl = 64;									// A recommended value (in seconds)
	iph->protocol = SUPERMAN_PROTOCOL_NUM;						// Our SUPERMAN protocol number
	iph->check = 0;									// No checksum yet
	iph->saddr = inet_select_addr(dev, addr, RT_SCOPE_UNIVERSE);			// Grab the most appropriate address.
	iph->daddr = addr;								// Broadcast the message to all on the subnet

	spi = MallocSupermanPacketInfo(NULL, tx_sk, NULL);
	send_superman_packet(spi, true);
	
	// Dereference the device.
	dev_put(dev);
	// printk(KERN_INFO "SUPERMAN: Packet - \t... Send Certificate Request done.\n");
}

void SendCertificateExchangePacket(uint32_t addr, uint32_t certificate_len, unsigned char* certificate)
{
	struct security_table_entry* ste;
	struct net_device *dev;
	struct in_addr;
	struct sk_buff* tx_sk;
	struct superman_header* shdr;
	struct iphdr* iph;
	void* payload;
	struct superman_packet_info* spi;

	// printk(KERN_INFO "SUPERMAN: Packet - \tSend Certificate Exchange to %u.%u.%u.%u...\n", 0x0ff & addr, 0x0ff & (addr >> 8), 0x0ff & (addr >> 16), 0x0ff & (addr >> 24));

	// Grab some information about the interface.
	if(!GetSecurityTableEntry(addr, &ste))
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tNo device for address %d.%d.%d.%d.\n", 0x0ff & addr, 0x0ff & (addr >> 8), 0x0ff & (addr >> 16), 0x0ff & (addr >> 24));
		return;
	}

	// Grab a device reference. We must dereference later (dev_put).
	dev = dev_get_by_index(&init_net, ste->ifindex);
	if(dev == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tNo device for interface %i.\n", ste->ifindex);
		return;
	}

	// Allocate a new packet
	tx_sk = alloc_skb(sizeof(struct iphdr) + SUPERMAN_HEADER_LEN + sizeof(__be16) + certificate_len, GFP_KERNEL);
	if(tx_sk == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
		return;
	}
	tx_sk->dev = dev;
	tx_sk->pkt_type = PACKET_OUTGOING;						// Its outgoing.
	tx_sk->ip_summed = CHECKSUM_NONE;						// No need to checksum.

	// Reserve space for the IP and SUPERMAN headers
	skb_reserve(tx_sk, sizeof(struct iphdr) + SUPERMAN_HEADER_LEN);

	// Payload goes here.

	//   2 bytes  |    cert_len
	// -----------------------------
	// | Cert len |      Cert      |
	// -----------------------------

	payload = skb_put(tx_sk, certificate_len + sizeof(__be16));
	*((__be16*)payload) = htons(certificate_len);
	memcpy(payload + sizeof(__be16), certificate, certificate_len);

	// Setup the superman header
	shdr = (struct superman_header*) skb_push(tx_sk, SUPERMAN_HEADER_LEN);
	skb_reset_transport_header(tx_sk);
	shdr->type = SUPERMAN_CERTIFICATE_EXCHANGE_TYPE;				// We're preparing a certificate exchange packet.
	shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(addr)));		// This will be a unique counter value for each packet, cycling round.
	shdr->payload_len = htons(certificate_len + sizeof(__be16));			// A certificate exchange contains a certificate.

	// Setup the IP header
	iph = (struct iphdr*) skb_push(tx_sk, sizeof(struct iphdr));
	skb_reset_network_header(tx_sk);
	iph->version = 4;								// IPv4 only, for now.
	iph->ihl = 5;									// Number of 32-bit words in the header (min 5)
	iph->tos = 0;									// Was TOS, now DSCP (Differentiated Services Code Point) - not required.
	iph->tot_len = htons(tx_sk->len);						// Total length of the packet
	iph->frag_off = htons(IP_DF);							// Fragment Offset - this packet is not fragmented
	iph->id = htons(0);								// The identifier is supposed to be a unique value during such that it does not repeat within the maximum datagram lifetime (MDL)
	iph->ttl = 64;									// A recommended value (in seconds)
	iph->protocol = SUPERMAN_PROTOCOL_NUM;						// Our SUPERMAN protocol number
	iph->check = 0;									// No checksum yet
	iph->saddr = inet_select_addr(dev, addr, RT_SCOPE_UNIVERSE);			// Grab the most appropriate address.
	iph->daddr = addr;								// Broadcast the message to all on the subnet

	spi = MallocSupermanPacketInfo(NULL, tx_sk, NULL);
	//send_superman_packet(spi, true);
	AddE2ESecurity(spi, hash_then_send_superman_packet);

	// Dereference the device.
	dev_put(dev);
	// printk(KERN_INFO "SUPERMAN: Packet - \t... Send Certificate Exchange done.\n");
}

void SendCertificateExchangeWithBroadcastKeyPacket(uint32_t addr, uint32_t certificate_len, unsigned char* certificate, uint32_t broadcast_key_len, unsigned char* broadcast_key)
{
	struct security_table_entry* ste;
	struct net_device *dev;
	struct in_addr;
	struct sk_buff* tx_sk;
	struct superman_header* shdr;
	struct iphdr* iph;
	void* payload;
	struct superman_packet_info* spi;

	// printk(KERN_INFO "SUPERMAN: Packet - \tSend Certificate Exchange With Broadcast Key to %u.%u.%u.%u...\n", 0x0ff & addr, 0x0ff & (addr >> 8), 0x0ff & (addr >> 16), 0x0ff & (addr >> 24));
	// printk(KERN_INFO "SUPERMAN: Packet - \tCertificate len: %u, Broadcast Key len: %u.\n", certificate_len, broadcast_key_len);

	// Grab some information about the interface.
	if(!GetSecurityTableEntry(addr, &ste))
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tNo device for address %d.%d.%d.%d.\n", 0x0ff & addr, 0x0ff & (addr >> 8), 0x0ff & (addr >> 16), 0x0ff & (addr >> 24));
		return;
	}

	// Grab a device reference. We must dereference later (dev_put).
	dev = dev_get_by_index(&init_net, ste->ifindex);
	if(dev == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tNo device for interface %i.\n", ste->ifindex);
		return;
	}

	// Allocate a new packet
	tx_sk = alloc_skb(sizeof(struct iphdr) + SUPERMAN_HEADER_LEN + sizeof(__be16) + certificate_len + sizeof(__be16) + broadcast_key_len, GFP_KERNEL);
	if(tx_sk == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
		return;
	}
	tx_sk->dev = dev;
	tx_sk->pkt_type = PACKET_OUTGOING;						// Its outgoing.
	tx_sk->ip_summed = CHECKSUM_NONE;						// No need to checksum.

	// Reserve space for the IP and SUPERMAN headers
	skb_reserve(tx_sk, sizeof(struct iphdr) + SUPERMAN_HEADER_LEN);

	// Payload goes here.

	//   2 bytes  |    cert_len    | 2 bytes  | broadcast_key_len
	// ------------------------------------------------------------
	// | Cert len |      Cert      | bkey len |        bkey       |
	// ------------------------------------------------------------

	payload = skb_put(tx_sk, certificate_len + sizeof(__be16) + broadcast_key_len + sizeof(__be16));
	*((__be16*)payload) = htons(certificate_len);
	memcpy(payload + sizeof(__be16), certificate, certificate_len);
	*((__be16*)(payload + sizeof(__be16) + certificate_len)) = htons(broadcast_key_len);
	memcpy(payload + sizeof(__be16) + certificate_len + sizeof(__be16), broadcast_key, broadcast_key_len);

	// Setup the superman header
	shdr = (struct superman_header*) skb_push(tx_sk, SUPERMAN_HEADER_LEN);
	skb_reset_transport_header(tx_sk);
	shdr->type = SUPERMAN_CERTIFICATE_EXCHANGE_WITH_BROADCAST_KEY_TYPE;		// We're preparing a certificate exchange with broadcast key packet.
	shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(addr)));		// This will be a unique counter value for each packet, cycling round.
	shdr->payload_len = htons(certificate_len + sizeof(__be16) + broadcast_key_len + sizeof(__be16));	// A certificate exchange with broadcast key contains a certificate and broadcast key.

	// Setup the IP header
	iph = (struct iphdr*) skb_push(tx_sk, sizeof(struct iphdr));
	skb_reset_network_header(tx_sk);
	iph->version = 4;								// IPv4 only, for now.
	iph->ihl = 5;									// Number of 32-bit words in the header (min 5)
	iph->tos = 0;									// Was TOS, now DSCP (Differentiated Services Code Point) - not required.
	iph->tot_len = htons(tx_sk->len);						// Total length of the packet
	iph->frag_off = htons(IP_DF);							// Fragment Offset - this packet is not fragmented
	iph->id = htons(0);								// The identifier is supposed to be a unique value during such that it does not repeat within the maximum datagram lifetime (MDL)
	iph->ttl = 64;									// A recommended value (in seconds)
	iph->protocol = SUPERMAN_PROTOCOL_NUM;						// Our SUPERMAN protocol number
	iph->check = 0;									// No checksum yet
	iph->saddr = inet_select_addr(dev, addr, RT_SCOPE_UNIVERSE);			// Grab the most appropriate address.
	iph->daddr = addr;								// Broadcast the message to all on the subnet

	spi = MallocSupermanPacketInfo(NULL, tx_sk, NULL);
	AddE2ESecurity(spi, hash_then_send_superman_packet);

	// Dereference the device.
	dev_put(dev);
	// printk(KERN_INFO "SUPERMAN: Packet - \t... Send Certificate Exchange With Broadcast Key done.\n");
}

void SendAuthenticatedSKResponsePacket(uint32_t originaddr, uint32_t targetaddr, uint32_t sk_len, unsigned char* sk)
{
	struct sk_buff* tx_sk;
	struct iphdr* iph;
	struct superman_header* shdr;
	void* payload;
	struct rtable* rt;
	struct dst_entry* dst;
	uint32_t ouraddr;
	struct flowi4 fl4;

	// NOTE: Responses are sent from the target to the origin

	printk(KERN_INFO "SUPERMAN: Packet - \tSendAuthenticatedSKResponsePacket\n");
	printk(KERN_INFO "SUPERMAN: Packet - \tDoing ip_route_output_key on addr %d.%d.%d.%d...\n", 0x0ff & originaddr, 0x0ff & (originaddr >> 8), 0x0ff & (originaddr >> 16), 0x0ff & (originaddr >> 24));

	memset(&fl4, 0, sizeof(fl4));
	fl4.daddr = originaddr;
	fl4.flowi4_flags = 0x08;
 	rt = ip_route_output_key(&init_net, &fl4);
 	if (IS_ERR(rt))
	{
		printk(KERN_INFO "SUPERMAN: Packet - \tip_route_output_key error!\n");
		return;
	}

	dst = &rt->dst;
	if(dst == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \tdst == NULL!\n");
		return;
	}

	printk(KERN_INFO "SUPERMAN: Packet - \tDoing inet_select_addr...\n");
	ouraddr = inet_select_addr(dst->dev, originaddr, RT_SCOPE_UNIVERSE);
	if(originaddr == 0)
		originaddr = ouraddr;

	printk(KERN_INFO "SUPERMAN: Packet - \tSending SK Response, target: %d.%d.%d.%d, origin %d.%d.%d.%d.\n", 0x0ff & targetaddr, 0x0ff & (targetaddr >> 8), 0x0ff & (targetaddr >> 16), 0x0ff & (targetaddr >> 24), 0x0ff & originaddr, 0x0ff & (originaddr >> 8), 0x0ff & (originaddr >> 16), 0x0ff & (originaddr >> 24));

	// Allocate a new packet
	tx_sk = alloc_skb(sizeof(struct iphdr) + SUPERMAN_HEADER_LEN + SK_RESPONSE_PAYLOAD_LEN(sk_len), GFP_KERNEL);
	if(tx_sk == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
		return;
	}
	skb_dst_set(tx_sk, dst);
	tx_sk->pkt_type = PACKET_OUTGOING;				// Its outgoing.
	tx_sk->ip_summed = CHECKSUM_NONE;				// No need to checksum.


	// Reserve space for the IP and SUPERMAN headers
	skb_reserve(tx_sk, sizeof(struct iphdr) + SUPERMAN_HEADER_LEN);

	// Payload goes here.

	//  4 bytes | 4 bytes  | 2 bytes | sk_len    
	// -----------------------------------------
	//   saddr  |  daddr   |  sk_len |  sk   
	// -----------------------------------------

	//printk(KERN_INFO "SUPERMAN: Packet - \tReserving %lu bytes...\n", SK_RESPONSE_PAYLOAD_LEN(sk_len));
	payload = skb_put(tx_sk, SK_RESPONSE_PAYLOAD_LEN(sk_len));
	{	
		struct sk_response_payload* p = (struct sk_response_payload*)payload;
		p->originaddr = htonl(originaddr);
		p->targetaddr = htonl(targetaddr);
		p->sk_len = htons(sk_len);
		memcpy(p->sk, sk, sk_len);
	}
	//printk(KERN_INFO "SUPERMAN: Packet - \tPayload:\n");
	//dump_bytes(payload, SK_RESPONSE_PAYLOAD_LEN(sk_len));

	// Setup the superman header
	shdr = (struct superman_header*) skb_push(tx_sk, SUPERMAN_HEADER_LEN);
	skb_reset_transport_header(tx_sk);
	shdr->type = SUPERMAN_AUTHENTICATED_SK_RESPONSE_TYPE;		// We're preparing an SK response packet.
	shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(daddr)));		// This will be a unique counter value for each packet, cycling round.
	shdr->payload_len = htons(SK_RESPONSE_PAYLOAD_LEN(sk_len));						// An SK request contains the address information capture along the route, used for the return journey.

	// Setup the IP header
	iph = (struct iphdr*) skb_push(tx_sk, sizeof(struct iphdr));
	skb_reset_network_header(tx_sk);
	iph->version = 4;								// IPv4 only, for now.
	iph->ihl = 5;									// Number of 32-bit words in the header (min 5)
	iph->tos = 0;									// Was TOS, now DSCP (Differentiated Services Code Point) - not required.
	iph->tot_len = htons(tx_sk->len);						// Total length of the packet
	iph->frag_off = htons(IP_DF);							// Fragment Offset - this packet is not fragmented
	iph->id = htons(0);								// The identifier is supposed to be a unique value during such that it does not repeat within the maximum datagram lifetime (MDL)
	iph->ttl = 64;									// A recommended value (in seconds)
	iph->protocol = SUPERMAN_PROTOCOL_NUM;						// Our SUPERMAN protocol number
	iph->check = 0;									// No checksum yet
	iph->saddr = ouraddr;								// Grab the most appropriate address.
	iph->daddr = originaddr;							// We're targeting the destination.

	// Unlike other packet types, we need this one to be routed properly
	// and we'll be added superman on as it goes through netfilter.
	tx_sk->protocol = htons(ETH_P_IP);
	tx_sk->sk = NULL;
	ip_send_check(iph);
	ip_local_out(tx_sk);

/*
	printk(KERN_INFO "SUPERMAN: Packet - \tDumping stack before nf_hook.\n");
	dump_stack();	

	{
		int nfresult = NF_REPEAT;
		while(nfresult == NF_REPEAT)
			nfresult = nf_hook(NFPROTO_IPV4, NF_INET_LOCAL_OUT, NULL, tx_sk, NULL, dst->dev, dst_output_sk);

		printk(KERN_INFO "SUPERMAN: Packet - \tDumping stack after nf_hook, result: %d.\n", nfresult);
		dump_stack();	

		if(nfresult == NF_ACCEPT)
			dst->output(NULL, tx_sk);
		else
		{
			printk(KERN_INFO "SUPERMAN: Packet - \tnf_hook result: %d.\n", nfresult);
			if(nfresult == NF_DROP)
				kfree_skb(tx_sk);
			return;
		}
	}
*/

	printk(KERN_INFO "SUPERMAN: Packet - \tSK Response sent.\n");
}

void SendAuthenticatedSKRequestPacket(uint32_t originaddr, uint32_t targetaddr)
{
	struct sk_buff* tx_sk;
	struct iphdr* iph;
	struct superman_header* shdr;
	void* payload;
	struct rtable* rt;
	struct dst_entry* dst;
	uint32_t ouraddr;
	struct flowi4 fl4;

	// NOTE: Requests are sent from the origin to the target

	printk(KERN_INFO "SUPERMAN: Packet - \tSendAuthenticatedSKRequestPacket\n");
	printk(KERN_INFO "SUPERMAN: Packet - \tDoing ip_route_output_key on addr %d.%d.%d.%d...\n", 0x0ff & targetaddr, 0x0ff & (targetaddr >> 8), 0x0ff & (targetaddr >> 16), 0x0ff & (targetaddr >> 24));

	memset(&fl4, 0, sizeof(fl4));
	fl4.daddr = targetaddr;
	fl4.flowi4_flags = 0x08;
 	rt = ip_route_output_key(&init_net, &fl4);
 	if (IS_ERR(rt))
	{
		printk(KERN_INFO "SUPERMAN: Packet - \tip_route_output_key error!\n");
		return;
	}

	dst = &rt->dst;
	if(dst == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \tdst == NULL!\n");
		return;
	}

	printk(KERN_INFO "SUPERMAN: Packet - \tDoing inet_select_addr...\n");
	ouraddr = inet_select_addr(dst->dev, targetaddr, RT_SCOPE_UNIVERSE);
	if(originaddr == 0)
		originaddr = ouraddr;

	printk(KERN_INFO "SUPERMAN: Packet - \tSending SK Request, target: %d.%d.%d.%d, origin: %d.%d.%d.%d.\n", 0x0ff & targetaddr, 0x0ff & (targetaddr >> 8), 0x0ff & (targetaddr >> 16), 0x0ff & (targetaddr >> 24), 0x0ff & originaddr, 0x0ff & (originaddr >> 8), 0x0ff & (originaddr >> 16), 0x0ff & (originaddr >> 24));

	// Allocate a new packet
	tx_sk = alloc_skb(sizeof(struct iphdr) + SUPERMAN_HEADER_LEN + SK_REQUEST_PAYLOAD_LEN, GFP_KERNEL);
	if(tx_sk == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
		return;
	}
	skb_dst_set(tx_sk, dst);
	tx_sk->pkt_type = PACKET_OUTGOING;				// Its outgoing.
	tx_sk->ip_summed = CHECKSUM_NONE;				// No need to checksum.

	// Reserve space for the IP and SUPERMAN headers
	skb_reserve(tx_sk, sizeof(struct iphdr) + SUPERMAN_HEADER_LEN);

	// Payload goes here.

	//   4 bytes  |    4 bytes
	// --------------------------
	//   saddr    |    daddr
	// --------------------------

	payload = skb_put(tx_sk, SK_REQUEST_PAYLOAD_LEN);
	{
		struct sk_request_payload* p = (struct sk_request_payload*)payload;
		p->originaddr = htonl(originaddr);
		p->targetaddr = htonl(targetaddr);
	}
	//*((uint32_t*)payload) = htonl(saddr);
	//*((uint32_t*)(payload + sizeof(uint32_t))) = htonl(daddr);

	// Setup the superman header
	shdr = (struct superman_header*) skb_push(tx_sk, SUPERMAN_HEADER_LEN);
	skb_reset_transport_header(tx_sk);
	shdr->type = SUPERMAN_AUTHENTICATED_SK_REQUEST_TYPE;		// We're preparing an SK request packet.
	shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(daddr)));		// This will be a unique counter value for each packet, cycling round.
	shdr->payload_len = htons(SK_REQUEST_PAYLOAD_LEN);						// An SK request contains the address information capture along the route, used for the return journey.

	// Setup the IP header
	iph = (struct iphdr*) skb_push(tx_sk, sizeof(struct iphdr));
	skb_reset_network_header(tx_sk);
	iph->version = 4;								// IPv4 only, for now.
	iph->ihl = 5;									// Number of 32-bit words in the header (min 5)
	iph->tos = 0;									// Was TOS, now DSCP (Differentiated Services Code Point) - not required.
	iph->tot_len = htons(tx_sk->len);						// Total length of the packet
	iph->frag_off = htons(IP_DF);							// Fragment Offset - this packet is not fragmented
	iph->id = htons(0);								// The identifier is supposed to be a unique value during such that it does not repeat within the maximum datagram lifetime (MDL)
	iph->ttl = 64;									// A recommended value (in seconds)
	iph->protocol = SUPERMAN_PROTOCOL_NUM;						// Our SUPERMAN protocol number
	iph->check = 0;									// No checksum yet
	iph->saddr = ouraddr;								// Grab the most appropriate address.
	iph->daddr = targetaddr;							// We're targeting the destination.

	// Unlike other packet types, we need this one to be routed properly
	// and we'll be added superman on as it goes through netfilter.
	tx_sk->protocol = htons(ETH_P_IP);
	tx_sk->sk = NULL;
	ip_send_check(iph);
	ip_local_out(tx_sk);

	printk(KERN_INFO "SUPERMAN: Packet - \t SK Request sent.\n");
}

void SendInvalidateSKPacket(uint32_t addr)
{
	struct net_device *dev;

	INTERFACE_ITERATOR_START(dev)

	struct in_addr;
	struct sk_buff* tx_sk;
	struct superman_header* shdr;
	struct iphdr* iph;
	void* payload;
	struct superman_packet_info* spi;

	// printk(KERN_INFO "SUPERMAN: Packet - \tSend SK Invalidate...\n");

	// Allocate a new packet
	tx_sk = alloc_skb(sizeof(struct iphdr) + SUPERMAN_HEADER_LEN + sizeof(addr), GFP_KERNEL);
	if(tx_sk == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
		continue;
	}
	tx_sk->dev = dev;
	tx_sk->pkt_type = PACKET_OUTGOING | PACKET_BROADCAST;				// Its outgoing.
	tx_sk->ip_summed = CHECKSUM_NONE;						// No need to checksum.

	// Reserve space for the IP and SUPERMAN headers
	skb_reserve(tx_sk, sizeof(struct iphdr) + SUPERMAN_HEADER_LEN);

	// Payload goes here.

	// ----------------
	// |  IP Address  |
	// ----------------

	payload = skb_put(tx_sk, sizeof(addr));
	*((__be32*)payload) = htonl(addr);

	// Setup the superman header
	shdr = (struct superman_header*) skb_push(tx_sk, SUPERMAN_HEADER_LEN);
	skb_reset_transport_header(tx_sk);
	shdr->type = SUPERMAN_SK_INVALIDATE_TYPE;					// We're preparing an SK invalidate packet.
	shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(INADDR_BROADCAST)));		// This will be a unique counter value for each packet, cycling round.
	shdr->payload_len = htons(sizeof(addr));					// An SK invalidate contains an address.

	// Setup the IP header
	iph = (struct iphdr*) skb_push(tx_sk, sizeof(struct iphdr));
	skb_reset_network_header(tx_sk);
	iph->version = 4;								// IPv4 only, for now.
	iph->ihl = 5;									// Number of 32-bit words in the header (min 5)
	iph->tos = 0;									// Was TOS, now DSCP (Differentiated Services Code Point) - not required.
	iph->tot_len = htons(tx_sk->len);						// Total length of the packet
	iph->frag_off = htons(IP_DF);							// Fragment Offset - this packet is not fragmented
	iph->id = htons(0);								// The identifier is supposed to be a unique value during such that it does not repeat within the maximum datagram lifetime (MDL)
	iph->ttl = 64;									// A recommended value (in seconds)
	iph->protocol = SUPERMAN_PROTOCOL_NUM;						// Our SUPERMAN protocol number
	iph->check = 0;									// No checksum yet
	iph->saddr = inet_select_addr(dev, htonl(INADDR_BROADCAST), RT_SCOPE_UNIVERSE);	// Grab the most appropriate address.
	iph->daddr = htonl(INADDR_BROADCAST);						// Broadcast the message to all on the subnet

	spi = MallocSupermanPacketInfo(NULL, tx_sk, NULL);
	AddE2ESecurity(spi, hash_then_send_superman_packet);

	INTERFACE_ITERATOR_END
	// printk(KERN_INFO "SUPERMAN: Packet - \t... Send SK Invalidate done.\n");
}


void SendBroadcastKeyExchange(uint32_t broadcast_key_len, unsigned char* broadcast_key)
{
	struct net_device *dev;

	INTERFACE_ITERATOR_START(dev)

	struct in_addr;
	struct sk_buff* tx_sk;
	struct superman_header* shdr;
	struct iphdr* iph;
	void* payload;
	struct superman_packet_info* spi;

	// printk(KERN_INFO "SUPERMAN: Packet - \tSend SK Invalidate...\n");

	// Allocate a new packet
	tx_sk = alloc_skb(sizeof(struct iphdr) + SUPERMAN_HEADER_LEN + sizeof(__be16) + broadcast_key_len, GFP_KERNEL);
	if(tx_sk == NULL)
	{
		printk(KERN_INFO "SUPERMAN: Packet - \t\tFailed to allocate a new skb.");
		continue;
	}
	tx_sk->dev = dev;
	tx_sk->pkt_type = PACKET_OUTGOING | PACKET_BROADCAST;				// Its outgoing.
	tx_sk->ip_summed = CHECKSUM_NONE;						// No need to checksum.

	// Reserve space for the IP and SUPERMAN headers
	skb_reserve(tx_sk, sizeof(struct iphdr) + SUPERMAN_HEADER_LEN);

	// Payload goes here.

	// ------------------------------
	// |  BKey len | BKey           |
	// ------------------------------

	payload = skb_put(tx_sk, broadcast_key_len + sizeof(__be16));
	*((__be16*)payload) = htons(broadcast_key_len);
	memcpy(payload + sizeof(__be16), broadcast_key, broadcast_key_len);

	// Setup the superman header
	shdr = (struct superman_header*) skb_push(tx_sk, SUPERMAN_HEADER_LEN);
	skb_reset_transport_header(tx_sk);
	shdr->type = SUPERMAN_SK_INVALIDATE_TYPE;					// We're preparing a broadcast key exchange packet.
	shdr->timestamp = 0; // htons(GetNextTimestampFromSecurityTableEntry(htonl(INADDR_BROADCAST)));		// This will be a unique counter value for each packet, cycling round.
	shdr->payload_len = htons(sizeof(__be16) + broadcast_key_len);			// A broadcast key exchange packet contains a broadcast key.

	// Setup the IP header
	iph = (struct iphdr*) skb_push(tx_sk, sizeof(struct iphdr));
	skb_reset_network_header(tx_sk);
	iph->version = 4;								// IPv4 only, for now.
	iph->ihl = 5;									// Number of 32-bit words in the header (min 5)
	iph->tos = 0;									// Was TOS, now DSCP (Differentiated Services Code Point) - not required.
	iph->tot_len = htons(tx_sk->len);						// Total length of the packet
	iph->frag_off = htons(IP_DF);							// Fragment Offset - this packet is not fragmented
	iph->id = htons(0);								// The identifier is supposed to be a unique value during such that it does not repeat within the maximum datagram lifetime (MDL)
	iph->ttl = 64;									// A recommended value (in seconds)
	iph->protocol = SUPERMAN_PROTOCOL_NUM;						// Our SUPERMAN protocol number
	iph->check = 0;									// No checksum yet
	iph->saddr = inet_select_addr(dev, htonl(INADDR_BROADCAST), RT_SCOPE_UNIVERSE);	// Grab the most appropriate address.
	iph->daddr = htonl(INADDR_BROADCAST);						// Broadcast the message to all on the subnet

	spi = MallocSupermanPacketInfo(NULL, tx_sk, NULL);
	AddE2ESecurity(spi, hash_then_send_superman_packet);

	INTERFACE_ITERATOR_END
	// printk(KERN_INFO "SUPERMAN: Packet - \t... Send Broadcast Key Exchange done.\n");
}

#endif
