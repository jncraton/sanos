//
// arp.c
//
// Copyright (c) 2001 Michael Ringgaard. All rights reserved.
//
// Address Resolution Protocol (ARP)
//

#include <net/net.h>

#define HWTYPE_ETHERNET 1

#define ARP_REQUEST     1
#define ARP_REPLY       2

#define ARP_MAXAGE      120         // 120 * 10 seconds = 20 minutes

#pragma pack(push)
#pragma pack(1)

struct arp_hdr 
{
  struct eth_hdr ethhdr;           // Ethernet header
  unsigned short hwtype;           // Hardware type
  unsigned short proto;            // Protocol type
  unsigned short _hwlen_protolen;  // Protocol address length
  unsigned short opcode;           // Opcode
  struct eth_addr shwaddr;         // Source hardware address
  struct ip_addr sipaddr;          // Source protocol address
  struct eth_addr dhwaddr;         // Target hardware address
  struct ip_addr dipaddr;          // Target protocol address
};

struct ethip_hdr 
{
  struct eth_hdr eth;
  struct ip_hdr ip;
};

#pragma pack(pop)

#define ARPH_HWLEN(hdr) (NTOHS((hdr)->_hwlen_protolen) >> 8)
#define ARPH_PROTOLEN(hdr) (NTOHS((hdr)->_hwlen_protolen) & 0xFF)

#define ARPH_HWLEN_SET(hdr, len) (hdr)->_hwlen_protolen = HTONS(ARPH_PROTOLEN(hdr) | ((len) << 8))
#define ARPH_PROTOLEN_SET(hdr, len) (hdr)->_hwlen_protolen = HTONS((len) | (ARPH_HWLEN(hdr) << 8))

struct arp_entry 
{
  struct ip_addr ipaddr;
  struct eth_addr ethaddr;
  int ctime;
};

static struct arp_entry arp_table[ARP_TABLE_SIZE];
int ctime;

void arp_init()
{
  int i;
  
  for (i = 0; i < ARP_TABLE_SIZE; ++i) ip_addr_set(&(arp_table[i].ipaddr), IP_ADDR_ANY);
}

void arp_tmr()
{
  int i;
  
  ctime++;
  for (i = 0; i < ARP_TABLE_SIZE; ++i)
  {
    if (!ip_addr_isany(&arp_table[i].ipaddr) && ctime - arp_table[i].ctime >= ARP_MAXAGE) 
    {
      kprintf("arp_timer: expired entry %d\n", i);
      ip_addr_set(&(arp_table[i].ipaddr), IP_ADDR_ANY);
    }
  }  
}

static void add_arp_entry(struct ip_addr *ipaddr, struct eth_addr *ethaddr)
{
  int i, j, k;
  int maxtime;
  
  // Walk through the ARP mapping table and try to find an entry to
  // update. If none is found, the IP -> MAC address mapping is
  // inserted in the ARP table.
  for (i = 0; i < ARP_TABLE_SIZE; i++)
  {
    // Only check those entries that are actually in use.
    if (!ip_addr_isany(&arp_table[i].ipaddr))
    {
      // Check if the source IP address of the incoming packet matches
      // the IP address in this ARP table entry.
      if (ip_addr_cmp(ipaddr, &arp_table[i].ipaddr)) 
      {
	// An old entry found, update this and return.
	for (k = 0; k < 6; ++k) arp_table[i].ethaddr.addr[k] = ethaddr->addr[k];
	arp_table[i].ctime = 0;
	return;
      }
    }
  }

  // If we get here, no existing ARP table entry was found, so we create one.
  /// First, we try to find an unused entry in the ARP table.
  for (i = 0; i < ARP_TABLE_SIZE; i++)
  {
    if (ip_addr_isany(&arp_table[i].ipaddr)) break;
  }

  // If no unused entry is found, we try to find the oldest entry and throw it away
  if (i == ARP_TABLE_SIZE) 
  {
    maxtime = 0;
    j = 0;
    for (i = 0; i < ARP_TABLE_SIZE; ++i)
    {
      if(ctime - arp_table[i].ctime > maxtime) 
      {
	maxtime = ctime - arp_table[i].ctime;
	j = i;
      }
    }
    i = j;
  }

  // Now, i is the ARP table entry which we will fill with the new information.
  ip_addr_set(&arp_table[i].ipaddr, ipaddr);
  for(k = 0; k < 6; ++k) arp_table[i].ethaddr.addr[k] = ethaddr->addr[k];
  arp_table[i].ctime = ctime;
}

void arp_ip_input(struct netif *netif, struct pbuf *p)
{
  struct ethip_hdr *hdr;
  
  hdr = p->payload;
  
  // Only insert/update an entry if the source IP address of the
  // incoming IP packet comes from a host on the local network.
  if (!ip_addr_maskcmp(&(hdr->ip.src), &(netif->ip_addr), &(netif->netmask))) return;

  add_arp_entry(&(hdr->ip.src), &(hdr->eth.src));
}

struct pbuf *arp_arp_input(struct netif *netif, struct eth_addr *ethaddr, struct pbuf *p)
{
  struct arp_hdr *hdr;
  int i;
  
  if (p->tot_len < sizeof(struct arp_hdr)) 
  {
    pbuf_free(p);
    return NULL;
  }

  hdr = p->payload;
  
  switch(htons(hdr->opcode)) 
  {
    case ARP_REQUEST:
      // ARP request. If it asked for our address, we send out a reply
      if (ip_addr_cmp(&(hdr->dipaddr), &(netif->ip_addr))) 
      {
	hdr->opcode = htons(ARP_REPLY);

	ip_addr_set(&(hdr->dipaddr), &(hdr->sipaddr));
	ip_addr_set(&(hdr->sipaddr), &(netif->ip_addr));

	for (i = 0; i < 6; i++)
	{
	  hdr->dhwaddr.addr[i] = hdr->shwaddr.addr[i];
	  hdr->shwaddr.addr[i] = ethaddr->addr[i];
	  hdr->ethhdr.dest.addr[i] = hdr->dhwaddr.addr[i];
	  hdr->ethhdr.src.addr[i] = ethaddr->addr[i];
	}

	hdr->hwtype = htons(HWTYPE_ETHERNET);
	ARPH_HWLEN_SET(hdr, 6);
      
	hdr->proto = htons(ETHTYPE_IP);
	ARPH_PROTOLEN_SET(hdr, sizeof(struct ip_addr));      
      
	hdr->ethhdr.type = htons(ETHTYPE_ARP);      
	return p;
      }
      break;

    case ARP_REPLY:    
      // ARP reply. We insert or update the ARP table.
      if (ip_addr_cmp(&(hdr->dipaddr), &(netif->ip_addr))) 
      {
	add_arp_entry(&(hdr->sipaddr), &(hdr->shwaddr));
	dhcp_arp_reply(&hdr->sipaddr);
      }
      break;

    default:
      break;
  }

  pbuf_free(p);
  return NULL;
}

struct eth_addr *arp_lookup(struct ip_addr *ipaddr)
{
  int i;
  
  for (i = 0; i < ARP_TABLE_SIZE; ++i) 
  {
    if (ip_addr_cmp(ipaddr, &arp_table[i].ipaddr)) return &arp_table[i].ethaddr;
  }
  return NULL;  
}

struct pbuf *arp_query(struct netif *netif, struct eth_addr *ethaddr, struct ip_addr *ipaddr)
{
  struct arp_hdr *hdr;
  struct pbuf *p;
  int i;

  p = pbuf_alloc(PBUF_LINK, sizeof(struct arp_hdr), PBUF_RW);
  if (p == NULL) return NULL;

  hdr = p->payload;
  hdr->opcode = htons(ARP_REQUEST);

  for (i = 0; i < 6; ++i) 
  {
    hdr->dhwaddr.addr[i] = 0x00;
    hdr->shwaddr.addr[i] = ethaddr->addr[i];
  }
  
  ip_addr_set(&(hdr->dipaddr), ipaddr);
  ip_addr_set(&(hdr->sipaddr), &(netif->ip_addr));

  hdr->hwtype = htons(HWTYPE_ETHERNET);
  ARPH_HWLEN_SET(hdr, 6);

  hdr->proto = htons(ETHTYPE_IP);
  ARPH_PROTOLEN_SET(hdr, sizeof(struct ip_addr));

  for (i = 0; i < 6; ++i) 
  {
    hdr->ethhdr.dest.addr[i] = 0xFF;
    hdr->ethhdr.src.addr[i] = ethaddr->addr[i];
  }
  
  hdr->ethhdr.type = htons(ETHTYPE_ARP);      
  return p;
}
