//----------------------------------------------------------------------------
/// \file   pcap.hpp
/// \author Serge Aleynikov
//----------------------------------------------------------------------------
/// \brief Support for PCAP file format writing.
//----------------------------------------------------------------------------
// Copyright (c) 2010 Serge Aleynikov <saleyn@gmail.com>
// Created: 2010-10-21
//----------------------------------------------------------------------------
/*
***** BEGIN LICENSE BLOCK *****

This file is part of the utxx open-source project.

Copyright (C) 2010 Serge Aleynikov <saleyn@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

***** END LICENSE BLOCK *****
*/
#ifndef _UTXX_PCAP_HPP_
#define _UTXX_PCAP_HPP_

#include <utxx/error.hpp>
#include <utxx/endian.hpp>
#include <boost/cstdint.hpp>
#include <boost/assert.hpp>
#include <boost/static_assert.hpp>
#include <stdio.h>
#include <arpa/inet.h>

namespace utxx {

namespace detail {
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/if_ether.h>
}

/**
 * PCAP file reader/writer
 */
struct pcap {
    struct file_header {
        uint32_t magic_number;   /* magic number */
        uint16_t version_major;  /* major version number */
        uint16_t version_minor;  /* minor version number */
        int32_t  thiszone;       /* GMT to local correction */
        uint32_t sigfigs;        /* accuracy of timestamps */
        uint32_t snaplen;        /* max length of captured packets, in octets */
        uint32_t network;        /* data link type */
    };

    struct packet_header {
        uint32_t ts_sec;         /* timestamp seconds */
        uint32_t ts_usec;        /* timestamp microseconds */
        uint32_t incl_len;       /* number of octets of packet saved in file */
        uint32_t orig_len;       /* actual length of packet */
    };

    struct udp_frame {
        detail::ethhdr eth;
        detail::iphdr  ip;
        detail::udphdr udp;

        uint8_t  protocol() const { return ip.protocol;       }
        uint32_t src_ip()   const { return ntohl(ip.saddr);   }
        uint32_t dst_ip()   const { return ntohl(ip.daddr);   }
        uint16_t src_port() const { return ntohs(udp.source); }
        uint16_t dst_port() const { return ntohs(udp.dest);   }

        std::string src() const {
            char buf[32];
            return std::string(fmt(buf, src_ip(), src_port()));
        }
        std::string dst() const {
            char buf[32];
            return std::string(fmt(buf, dst_ip(), dst_port()));
        }

    private:
        static const char* fmt(char* buf, uint32_t ip, uint16_t port) {
            sprintf(buf, "%u.%u.%u.%u:%d",
                ip >> 24 & 0xFF, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF, port);
            return buf;
        }
    } __attribute__ ((packed));

    BOOST_STATIC_ASSERT(sizeof(detail::ethhdr) == 14);
    BOOST_STATIC_ASSERT(sizeof(detail::iphdr)  == 20);
    BOOST_STATIC_ASSERT(sizeof(detail::udphdr) ==  8);
    BOOST_STATIC_ASSERT(sizeof(udp_frame)      == 42);

    pcap() : m_big_endian(true), m_file(NULL), m_own_handle(false) {}

    long open_read(const std::string& a_filename, bool a_is_pipe = false) {
        return open(a_filename.c_str(), a_is_pipe ? "r" : "rb", a_is_pipe);
    }

    long open_write(const std::string& a_filename, bool a_is_pipe = false) {
        long sz = open(a_filename.c_str(), a_is_pipe ? "w" : "wb+", a_is_pipe);
        if (sz < 0)
            return sz;
        write_file_header();
        return sz;
    }

    long popen(const char* a_filename, const std::string& a_mode) {
        return open(a_filename, a_mode, true);
    }

    long open(const char* a_filename, const std::string& a_mode) {
        return open(a_filename, a_mode, false);
    }

    ~pcap() {
        close();
    }

    static bool is_pcap_header(const char* buf, size_t sz) {
        return sz >= 4 &&
             ( *reinterpret_cast<const uint32_t*>(buf) == 0xa1b2c3d4
            || *reinterpret_cast<const uint32_t*>(buf) == 0xd4c3b2a1);
    }

    static int set_file_header(char* buf, size_t sz) {
        BOOST_ASSERT(sz >= sizeof(file_header));
        file_header* p = reinterpret_cast<file_header*>(buf);
        p->magic_number     = 0xa1b2c3d4;
        p->version_major    = 2;
        p->version_minor    = 4;
        p->thiszone         = 0;
        p->sigfigs          = 0;
        p->snaplen          = 65535;
        p->network          = 1;
        return sizeof(file_header);
    }

    static void init_udp_frame(udp_frame& frame)
    {
        memset(&frame, 0, sizeof(udp_frame));
        frame.eth.h_proto = htons(ETH_P_IP);
        frame.ip.version  = IPVERSION;
        frame.ip.protocol = IPPROTO_UDP;
        frame.ip.ihl      = 5;    // 32-bit words
        //frame.ip.frag_off = 0;
        //frame.ip.tos      = 0;    // No special type-of-service
        //frame.ip.id       = 0;    // No flow id (since no frags)
        frame.ip.ttl      = 64;   // Linux default time-to-live
        //frame.ip.check    = 0;    // Zero the checksum
        //frame.udp.check   = 0;    // Zero the checksum
    }

    template <size_t N>
    static int set_packet_header(char (&buf)[N], const struct timeval& tv, size_t len) {
        return set_packet_header(buf, N, tv, len);
    }

    static int set_packet_header(char* a_buf, size_t a_size,
        const struct timeval& tv, size_t len)
    {
        assert(a_size >= sizeof(packet_header));
        packet_header* p = reinterpret_cast<packet_header*>(a_buf);
        int sz      = len + sizeof(udp_frame);
        p->ts_sec   = tv.tv_sec;
        p->ts_usec  = tv.tv_usec;
        p->incl_len = sz;
        p->orig_len = sz;
        return sizeof(packet_header);
    }

    int read_file_header() {
        char buf[sizeof(file_header)];
        const char* p = buf;
        size_t n = read(buf, sizeof(buf));
        if (n < sizeof(buf))
            return -1;
        return read_file_header(p, sizeof(buf));
    }

    // For use with externally open files
    /// @return size of consumed file reader
    static int read_file_header(pcap& file, const char*& buf, size_t sz) {
        auto begin = buf;
        if (sz < sizeof(file_header) || !is_pcap_header(buf, sz))
            return -1;

        const uint8_t* p = (const uint8_t*)buf;
        file.m_big_endian = *p++ == 0xa1
                         && *p++ == 0xb2
                         && *p++ == 0xc3
                         && *p++ == 0xd4;
        if (!file.m_big_endian) {
            file.m_file_header = *reinterpret_cast<const file_header*>(buf);
            buf += sizeof(file_header);
        } else {
            file.m_file_header.magic_number  = get32be(buf);
            file.m_file_header.version_major = get16be(buf);
            file.m_file_header.version_minor = get16be(buf);
            file.m_file_header.thiszone      = (int32_t)get32be(buf);
            file.m_file_header.sigfigs       = get32be(buf);
            file.m_file_header.snaplen       = get32be(buf);
            file.m_file_header.network       = get32be(buf);
        }
        return buf - begin;
    }

    int read_file_header(const char*& buf, size_t sz) {
        return read_file_header(*this, buf, sz);
    }

    // For use with externally open files
    /// @param file is the PCAP file instance
    /// @param buf  holds the PCAP data to be read
    /// @param sz   is the size of the data in \a buf
    /// @return size of next packet
    static int read_packet_header(pcap& file, const char*& buf, size_t sz) {
        if (sz < sizeof(packet_header))
            return -1;
        if (!file.m_big_endian) {
            file.m_pkt_header = *reinterpret_cast<const packet_header*>(buf);
            buf += sizeof(packet_header);
        } else {
            file.m_pkt_header.ts_sec   = get32be(buf);
            file.m_pkt_header.ts_usec  = get32be(buf);
            file.m_pkt_header.incl_len = get32be(buf);
            file.m_pkt_header.orig_len = get32be(buf);
        }
        return file.m_pkt_header.incl_len;
    }

    int read_packet_header(const char*& buf, size_t sz) {
        return read_packet_header(*this, buf, sz);
    }

    int parse_udp_frame(const char*&buf, size_t sz) {
        if (sz < 42)
            return -2;

        memcpy(&m_frame, buf, sizeof(m_frame)); buf += sizeof(m_frame);

        return m_frame.ip.protocol != IPPROTO_UDP ? -1 : 42;
    }

    /// @param a_mask is an IP address mask in network byte order.
    bool match_dst_ip(uint32_t a_ip_mask, uint16_t a_port = 0) {
        uint8_t b = a_ip_mask >> 24 & 0xFF;
        if (b != 0 && (b != (m_frame.ip.daddr >> 24 & 0xFF)))
            return false;
        b = a_ip_mask >> 16 & 0xFF;
        if (b != 0 && (b != (m_frame.ip.daddr >> 16 & 0xFF)))
            return false;
        b = a_ip_mask >> 8 & 0xFF;
        if (b != 0 && (b != (m_frame.ip.daddr >> 8 & 0xFF)))
            return false;
        b = a_ip_mask & 0xFF;
        if (b != 0 && (b != (m_frame.ip.daddr & 0xFF)))
            return false;
        if (a_port != 0 && (a_port != m_frame.udp.dest))
            return false;
        return true;
    }

    size_t read(char* buf, size_t sz) {
        return fread(buf, 1, sz, m_file);
    }

    int write_file_header() {
        char buf[sizeof(file_header)];
        int n = set_file_header(buf, sizeof(buf));
        return fwrite(buf, 1, n, m_file);
    }

    int write_packet_header(
        const struct timeval& a_timestamp, size_t a_packet_size)
    {
        char buf[sizeof(packet_header)];
        int n = set_packet_header(buf, a_timestamp, a_packet_size);
        return fwrite(buf, 1, n, m_file);
    }

    int write_packet_header(const packet_header& a_header) {
        return fwrite(&a_header, 1, sizeof(packet_header), m_file);
    }

    int write_udp_frame(const udp_frame& a_frame) {
        return fwrite(&a_frame, 1, sizeof(udp_frame), m_file);
    }

    int write(const char* buf, size_t sz) {
        return fwrite(buf, 1, sz, m_file);
    }

    bool     is_open()              const { return m_file != NULL; }
    uint64_t tell()                 const { return m_file ? ftell(m_file) : 0; }

    const file_header&   header()   const { return m_file_header; }
    const packet_header& packet()   const { return m_pkt_header;  }
    const udp_frame&     frame()    const { return m_frame;       }

    void set_handle(FILE* a_handle) {
        close();
        m_own_handle = false;
        m_file = a_handle;
    }

private:
    void close() {
        if (m_file && m_own_handle) {
            m_is_pipe ? pclose(m_file) : fclose(m_file);
            m_is_pipe = false;
            m_file    = NULL;
        }
    }

    /// @return 0 if opening a pipe or stdin
    long open(const char* a_filename, const std::string& a_mode, bool a_is_pipe) {
        close();
        bool use_stdin = !a_is_pipe
                      && (!strcmp(a_filename, "-") || !strcmp(a_filename, "/dev/stdin"));
        if (use_stdin)
            a_filename = "/dev/stdin";
        if (a_is_pipe)
            m_file = ::popen(a_filename, a_mode.c_str());
        else
            m_file = ::fopen(a_filename, a_mode.c_str());
        if (!m_file)
            return -1;
        m_is_pipe    = a_is_pipe;
        m_own_handle = true;
        long sz;
        if (a_is_pipe || use_stdin)
            sz = 0;
        else {
            if (fseek(m_file, 0, SEEK_END) < 0) return -1;
            sz = ftell(m_file);
            if (fseek(m_file, 0, SEEK_SET) < 0) return -1;
        }
        return (m_file == NULL) ? -1 : sz;
    }

    udp_frame     m_frame;
    file_header   m_file_header;
    packet_header m_pkt_header;
    bool          m_big_endian;
    FILE*         m_file;
    bool          m_own_handle;
    bool          m_is_pipe;
};

} // namespace utxx

#endif // _UTXX_PCAP_HPP_

