#include "rawudp.hh"
#include <sys/socket.h>
#include <netinet/in.h>
#include "misc.hh"
#include "iputils.hh"
#include <sys/ioctl.h>
#include <net/if.h>
#include "dnsparser.hh"
#include "statbag.hh"
#define __FAVOR_BSD
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <linux/if_packet.h>
#include <net/ethernet.h> /* the L2 protocols */
#include "dnsrecords.hh"

StatBag S;

int getindex(int s, const std::string& interface)
{
  struct ifreq ifr;
  memcpy(ifr.ifr_name,interface.c_str(), interface.size());
  ifr.ifr_name[interface.size()]=0;
  
  if (ioctl(s,SIOCGIFINDEX,&ifr)==-1) {
    unixDie("getting interface index");
  }
  return ifr.ifr_ifindex;
}

RawUDPListener::RawUDPListener(int port, const std::string& interface)
{
  d_socket = socket(AF_PACKET, SOCK_DGRAM,htons(ETH_P_ALL) );
  if(d_socket < 0)
    unixDie("opening packet socket");
  if(!interface.empty()) {
    
    struct sockaddr_ll iface;
    memset(&iface, 0, sizeof(iface));
    iface.sll_family = AF_PACKET;
    iface.sll_protocol = htons(ETH_P_IP);
    iface.sll_ifindex = getindex(d_socket, interface);
    cout<<"Index for "<<interface<<" is "<<iface.sll_ifindex<<endl;
    if(bind(d_socket, (struct sockaddr*)&iface, sizeof(iface)) < 0)
      unixDie("binding to interface");
    
    //    if(setsockopt(d_socket, SOL_SOCKET, SO_BINDTODEVICE, interface.c_str(), interface.size()) < 0)
    //  unixDie("Could not bind to interface");
  }
}

bool RawUDPListener::getPacket(std::string* packet, struct sockaddr_ll* addr)
{
  char buffer[1500];
  *addr={0};

  socklen_t remlen = sizeof(*addr);
  int res=recvfrom(d_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)addr, &remlen);
  if(res < 0)
    return false;
  packet->assign(buffer, res);
  return true;
}

bool RawUDPListener::getPacket(ComboAddress* src, ComboAddress* dst, struct sockaddr_ll* addr, std::string* payload, std::string* whole)
{
  string packet;
  if(!getPacket(&packet, addr))
    return false;
  
  const struct ip *iphdr = (const struct ip*)packet.c_str();
  const struct udphdr *udphdr= (const struct udphdr*)(packet.c_str() + 4 * iphdr->ip_hl);

  src->sin4.sin_family = AF_INET;
  src->sin4.sin_addr = iphdr->ip_src;
  src->sin4.sin_port = udphdr->uh_sport;

  dst->sin4.sin_family = AF_INET;
  dst->sin4.sin_addr = iphdr->ip_dst;
  dst->sin4.sin_port = udphdr->uh_dport;

  auto startpos = 4*iphdr->ip_hl + sizeof(struct udphdr);
  payload->assign(packet.c_str() + startpos, packet.size()-startpos);

  if(whole)
    *whole = packet;
  return true;
}

void RawUDPListener::sendPacket(const std::string& ippacket, const std::string& interface, const std::string& mac)
{
  struct sockaddr_ll addr={0};
  addr.sll_family=AF_PACKET;
  addr.sll_ifindex=getindex(d_socket, interface);
  addr.sll_halen=ETHER_ADDR_LEN;
  addr.sll_protocol=htons(ETH_P_IP);
  memcpy(&addr.sll_addr, mac.c_str(), 6);
  sendPacket(ippacket, addr); 

}


void RawUDPListener::sendPacket(const std::string& ippacket, const struct sockaddr_ll& addr)
{
  if(sendto(d_socket, ippacket.c_str(), ippacket.size(), 0, (struct sockaddr*) &addr, sizeof(addr)) < 0)
    unixDie("sending packet");
}


uint16_t ip_checksum(const void* vdata,size_t length) {
    // Cast the data pointer to one that can be indexed.
    const char* data=(char*)vdata;

    // Initialise the accumulator.
    uint32_t acc=0xffff;

    // Handle complete 16-bit blocks.
    for (size_t i=0;i+1<length;i+=2) {
        uint16_t word;
        memcpy(&word,data+i,2);
        acc+=ntohs(word);
        if (acc>0xffff) {
            acc-=0xffff;
        }
    }

    // Handle any partial block at the end of the data.
    if (length&1) {
        uint16_t word=0;
        memcpy(&word,data+length-1,1);
        acc+=ntohs(word);
        if (acc>0xffff) {
            acc-=0xffff;
        }
    }

    // Return the checksum in network byte order.
    return htons(~acc);
}


int main()
{
  reportAllTypes();
  RawUDPListener rul(53, "nonbt");
  string payload, packet;
  ComboAddress src, dst;
//  string mac("\x00\x0d\xb9\x3f\x80\x18", 6);
  string mac("\xb8\x27\xeb\x13\x0d\x73", 6);


  int recsock = socket(AF_INET, SOCK_DGRAM, 0);
  if(recsock < 0)
    unixDie("Making socket to talk to recursor");
    
  ComboAddress recursor("172.16.1.3:53");
  if(connect(recsock, (struct sockaddr*)&recursor, recursor.getSocklen()) < 0)
    unixDie("Connecting to recursor");
  
  for(;;) {
    struct sockaddr_ll macsrc;
    if(rul.getPacket(&src, &dst, &macsrc, &payload, &packet)) {
      cout<<"Got "<<payload.size()<<" bytes from "<<src.toStringWithPort()<<" to "<<dst.toStringWithPort()<<endl;
      cout<<makeHexDump(payload)<<endl;
      if(dst.sin4.sin_port != htons(53))
        cout<<"NOT DNS QUERY: ";
      else {
        MOADNSParser mdp(payload);
        cout<<"Query for "<<mdp.d_qname<<" | "<<DNSRecordContent::NumberToType(mdp.d_qtype)<<endl;
        cout<<"Mac: "<<makeHexDump(string((const char*)&macsrc.sll_addr, 6))<<", ifi: "<<macsrc.sll_ifindex<<endl;

        if(send(recsock, payload.c_str(), payload.size(), 0) < 0)
          unixDie("Sending query to recursor");
        char verdict[1500];
        int len=recv(recsock, verdict, sizeof(verdict), 0);
        if(len < 0)
          unixDie("Receiving answer from recursor");

        cout<<"Actual recursor said: "<<endl;        
        MOADNSParser rep(string(verdict, len));
        bool blocked=false;        
        for(const auto& a : rep.d_answers) {
          cout << a.first.d_name <<" " <<DNSRecordContent::NumberToType(a.first.d_type)<<" "<<a.first.d_content->getZoneRepresentation()<<endl;
          if(a.first.d_type == QType::A && a.first.d_content->getZoneRepresentation()=="172.16.1.3")
            blocked = true;
        }

        // send query to our normal nameserver, see what it does
        // if we get non-blocked answer, send on to internet
        // if we get blocked answer, send back blocked answer
        

        
        if(blocked) {
          cout<<"Query was blocked"<<endl;
          // now we need to pretend we are '1.2.3.4', the evil nameserver
          char p[packet.size()+1500];
          memcpy(p, packet.c_str(), packet.size());
          struct ip *iphdr = (struct ip*)p;
          struct udphdr *udphdr= (struct udphdr*)(p + 4 * iphdr->ip_hl);

          auto tmp1 = iphdr->ip_src;
          iphdr->ip_src= iphdr->ip_dst;
          iphdr->ip_dst = tmp1;
          
          auto tmp2 = udphdr->uh_sport;
          udphdr->uh_sport = udphdr->uh_dport;
          udphdr->uh_dport = tmp2;
          
          struct dnsheader* dh = (struct dnsheader*)verdict;
          dh->id = mdp.d_header.id;
          
          auto startpos = 4*iphdr->ip_hl + sizeof(struct udphdr);
          memcpy(p+startpos, verdict, len);

          iphdr->ip_len = htons(4*iphdr->ip_hl + sizeof(struct udphdr) + len);
          udphdr->uh_ulen = htons(sizeof(struct udphdr) + len);
          
          iphdr->ip_sum = 0;
          iphdr->ip_sum = ip_checksum(p, 4*iphdr->ip_hl); 
          
          udphdr->uh_sum = 0;
//          udphdr->uh_sum = ip_checksum(p+4*iphdr->ip_hl, ntohs(udphdr->uh_ulen));
          
          rul.sendPacket(string(p, startpos + len), macsrc);
          
        }
        else {
          // pass it on
          cout<<"Sending on query.."<<endl;
          rul.sendPacket(packet, "eth0", mac);
        }
      }
      

    }
    else
      cout<<"Got error"<<endl;
  }
}
