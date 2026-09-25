#include "guest/guest_console.h"
#include "guest/guest_interrupts.h"
#include "guest/network_stack.h"
#include "guest/virtio_net.h"

#include <stddef.h>
#include <stdint.h>

#ifndef TEST_DNS_SERVER_IP
#define TEST_DNS_SERVER_IP NET_IPV4(1, 1, 1, 1)
#endif

#define DNS_SERVER_IP ((uint32_t)TEST_DNS_SERVER_IP)
#define DNS_SERVER_PORT UINT16_C(53)
#define DNS_CLIENT_PORT UINT16_C(4000)
#define DNS_HEADER_SIZE 12U

#define DNS_FLAG_QR        UINT16_C(0x8000)
#define DNS_FLAG_OPCODE    UINT16_C(0x7800)
#define DNS_FLAG_TRUNCATED UINT16_C(0x0200)
#define DNS_FLAG_RCODE     UINT16_C(0x000f)

/* RFC 1035 query: ID 0x1234, recursion desired, example.com IN A. */
static const uint8_t dns_query[] = {
    0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm', 0x00,
    0x00, 0x01, 0x00, 0x01,
};

static uint16_t read_be16(const uint8_t* bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint8_t lowercase_ascii(uint8_t value) {
    if (value >= 'A' && value <= 'Z') {
        return (uint8_t)(value + ('a' - 'A'));
    }
    return value;
}

/* Check that the response repeats this test's example.com IN A question. */
static int dns_question_matches(const uint8_t* message,
                                size_t length,
                                size_t* answer_offset) {
    const size_t question_length = sizeof(dns_query) - DNS_HEADER_SIZE;
    if (length < DNS_HEADER_SIZE ||
        length - DNS_HEADER_SIZE < question_length) {
        return 0;
    }

    for (size_t i = 0; i < question_length; ++i) {
        if (lowercase_ascii(message[DNS_HEADER_SIZE + i]) !=
            lowercase_ascii(dns_query[DNS_HEADER_SIZE + i])) {
            return 0;
        }
    }

    *answer_offset = DNS_HEADER_SIZE + question_length;
    return 1;
}

/* We only need to advance through names, including compressed answer names. */
static int skip_dns_name(const uint8_t* message,
                         size_t length,
                         size_t* offset) {
    while (*offset < length) {
        const uint8_t label = message[(*offset)++];
        if (label == 0) return 1;
        if ((label & UINT8_C(0xc0)) == UINT8_C(0xc0)) {
            if (*offset == length) return 0;
            ++*offset;
            return 1;
        }
        if ((label & UINT8_C(0xc0)) != 0 || label > length - *offset) {
            return 0;
        }
        *offset += label;
    }
    return 0;
}

static int dns_answer_ipv4(const uint8_t* message,
                           size_t length,
                           uint8_t address[4]) {
    if (length < DNS_HEADER_SIZE ||
        read_be16(message) != UINT16_C(0x1234)) {
        return 0;
    }

    const uint16_t flags = read_be16(message + 2);
    if ((flags & DNS_FLAG_QR) == 0 ||
        (flags & DNS_FLAG_OPCODE) != 0 ||
        (flags & DNS_FLAG_TRUNCATED) != 0 ||
        (flags & DNS_FLAG_RCODE) != 0 ||
        read_be16(message + 4) != 1) {
        return 0;
    }

    size_t offset = 0;
    if (!dns_question_matches(message, length, &offset)) return 0;

    const uint16_t answer_count = read_be16(message + 6);
    for (uint16_t i = 0; i < answer_count; ++i) {
        if (!skip_dns_name(message, length, &offset) ||
            length - offset < 10) {
            return 0;
        }
        const uint16_t type = read_be16(message + offset);
        const uint16_t class_code = read_be16(message + offset + 2);
        const uint16_t data_length = read_be16(message + offset + 8);
        offset += 10;
        if (data_length > length - offset) return 0;
        if (type == 1 && class_code == 1 && data_length == 4) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                address[byte] = message[offset + byte];
            }
            return 1;
        }
        offset += data_length;
    }
    return 0;
}

static void print_ipv4(const uint8_t address[4]) {
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned value = address[i];
        if (i != 0) putc('.');
        if (value >= 100) putc('0' + value / 100);
        if (value >= 10) putc('0' + (value / 10) % 10);
        putc('0' + value % 10);
    }
}

static void __attribute__((noreturn)) halt_forever(void) {
    asm volatile("cli" ::: "memory");
    for (;;) asm volatile("hlt");
}

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void) {
    uint8_t reply[512];

    puts("Public DNS query demo started\n");
    guest_interrupts_init();
    if (net_init() != NET_OK) {
        puts("Virtio-net initialization failed\n");
        halt_forever();
    }
    puts("Virtio-net initialized; resolving TAP gateway ARP\n");
    guest_interrupts_enable();

    for (;;) {
        const int status = net_udp_send(DNS_SERVER_IP,
                                        DNS_CLIENT_PORT,
                                        DNS_SERVER_PORT,
                                        dns_query,
                                        sizeof(dns_query));
        if (status == NET_OK) break;
        if (status != NET_WOULD_BLOCK) {
            puts("DNS request could not be queued\n");
            halt_forever();
        }
        (void)net_udp_receive(NULL, NULL, reply, sizeof(reply));
        asm volatile("pause" ::: "memory");
    }
    puts("DNS query for example.com sent to resolver\n");

    for (;;) {
        uint32_t source_ip = 0;
        uint16_t source_port = 0;
        const int length = net_udp_receive(&source_ip,
                                           &source_port,
                                           reply,
                                           sizeof(reply));
        if (length >= 0 && source_ip == DNS_SERVER_IP &&
            source_port == DNS_SERVER_PORT) {
            uint8_t address[4];
            if (dns_answer_ipv4(reply, (size_t)length, address)) {
                puts("Public DNS reply: example.com = ");
                print_ipv4(address);
                putc('\n');
                halt_forever();
            }
            puts("DNS response has no valid A answer\n");
            halt_forever();
        }
        virtio_net_wait_for_receive();
    }
}
