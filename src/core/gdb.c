#include "core/gdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  static void close_sock(int s) { closesocket(s); }
  #define SOCK_NONBLOCK_ARG u_long
  static void sock_nonblock(int s) { u_long m = 1; ioctlsocket(s, FIONBIO, &m); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #include <fcntl.h>
  static void close_sock(int s) { close(s); }
  static void sock_nonblock(int s) { int f = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, f | O_NONBLOCK); }
#endif

/* GDB RSP packet format: $<payload>#<csum_hex_2>
   csum = sum of payload bytes mod 256. */

static int gdb_send(gdb_t* g, const char* payload) {
    char buf[2048];
    int n = (int)strlen(payload);
    u8 cs = 0;
    for (int i = 0; i < n; ++i) cs = (u8)(cs + (u8)payload[i]);
    int len = snprintf(buf, sizeof(buf), "$%s#%02x", payload, cs);
    /* Wait for '+' ack, but be tolerant. */
    if (send(g->conn, buf, len, 0) < 0) return -1;
    char ack;
    recv(g->conn, &ack, 1, 0);
    return 0;
}

static int gdb_recv_packet(gdb_t* g, char* out, int max) {
    /* Parse: optional '+', then $...#cs */
    char ch;
    int got;
    while ((got = recv(g->conn, &ch, 1, 0)) == 1 && ch != '$') {
        if (ch == 0x03) { /* Ctrl-C interrupt */
            strcpy(out, "INT"); return 3;
        }
    }
    if (got != 1) return -1;
    int n = 0;
    while ((got = recv(g->conn, &ch, 1, 0)) == 1 && ch != '#' && n < max - 1) {
        out[n++] = ch;
    }
    out[n] = 0;
    /* swallow 2-byte checksum */
    char dummy[2];
    recv(g->conn, dummy, 2, 0);
    /* Send ack */
    send(g->conn, "+", 1, 0);
    return n;
}

static const char* hex_chars = "0123456789abcdef";
static void hex32_le(u32 v, char* out) {
    /* GDB expects little-endian for register values. */
    for (int i = 0; i < 4; ++i) {
        u8 b = (u8)((v >> (i * 8)) & 0xFF);
        out[i*2]   = hex_chars[b >> 4];
        out[i*2+1] = hex_chars[b & 0xF];
    }
}

static u32 hex_to_u32(const char* s, int n) {
    u32 v = 0;
    for (int i = 0; i < n; ++i) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (u32)(c - 'A' + 10);
    }
    return v;
}

bool gdb_listen(gdb_t* g, int port) {
#ifdef _WIN32
    static bool wsa_init = false;
    if (!wsa_init) { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); wsa_init = true; }
#endif
    *g = (gdb_t){0};
    g->sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (g->sock < 0) return false;
    int on = 1;
    setsockopt(g->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g->sock, (struct sockaddr*)&a, sizeof(a)) < 0) return false;
    if (listen(g->sock, 1) < 0) return false;
    fprintf(stderr, "[gdb] listening on :%d (target remote :%d)\n", port, port);
    socklen_t sl = sizeof(a);
    g->conn = (int)accept(g->sock, (struct sockaddr*)&a, &sl);
    if (g->conn < 0) return false;
    fprintf(stderr, "[gdb] client connected\n");
    g->active = true;
    g->halted_for_gdb = true; /* start halted */
    return true;
}

void gdb_close(gdb_t* g) {
    if (g->conn) close_sock(g->conn);
    if (g->sock) close_sock(g->sock);
    g->active = false;
}

bool gdb_should_stop(gdb_t* g, cpu_t* c) {
    if (!g->active) return false;
    if (g->stepping) return true;
    for (int i = 0; i < g->bp_n; ++i) {
        if (g->bp[i] == c->r[REG_PC]) return true;
    }
    return false;
}

/* Return all 17 GDB registers (r0-r15 + xpsr) as hex string. */
static void send_regs(gdb_t* g, cpu_t* c) {
    char buf[17 * 8 + 1];
    for (int i = 0; i < 16; ++i) hex32_le(c->r[i], buf + i * 8);
    u32 xpsr = (c->apsr & 0xF8000000u) | c->ipsr | c->epsr;
    hex32_le(xpsr, buf + 16 * 8);
    buf[17 * 8] = 0;
    gdb_send(g, buf);
}

static void send_mem(gdb_t* g, bus_t* b, addr_t addr, u32 len) {
    char out[2048];
    if (len * 2 + 1 > sizeof(out)) { gdb_send(g, "E22"); return; }
    int p = 0;
    for (u32 i = 0; i < len; ++i) {
        u32 v = 0;
        if (!bus_read(b, addr + i, 1, &v)) { gdb_send(g, "E14"); return; }
        out[p++] = hex_chars[(v >> 4) & 0xF];
        out[p++] = hex_chars[v & 0xF];
    }
    out[p] = 0;
    gdb_send(g, out);
}

static void write_mem(gdb_t* g, bus_t* b, addr_t addr, u32 len, const char* hex) {
    for (u32 i = 0; i < len; ++i) {
        u32 v = hex_to_u32(hex + i * 2, 2);
        if (!bus_write(b, addr + i, 1, v)) { gdb_send(g, "E14"); return; }
    }
    gdb_send(g, "OK");
}

void gdb_serve(gdb_t* g, cpu_t* c, bus_t* b) {
    if (!g->active) return;
    /* Send stop reply first if just halted. */
    gdb_send(g, "S05"); /* SIGTRAP */
    char pkt[2048];
    while (1) {
        int n = gdb_recv_packet(g, pkt, sizeof(pkt));
        if (n < 0) { gdb_close(g); return; }
        char cmd = pkt[0];
        switch (cmd) {
            case 'q':
                if (strncmp(pkt, "qSupported", 10) == 0)
                    gdb_send(g, "PacketSize=1000");
                else if (strncmp(pkt, "qAttached", 9) == 0)
                    gdb_send(g, "1");
                else if (strncmp(pkt, "qC", 2) == 0)
                    gdb_send(g, "QC1");
                else if (strncmp(pkt, "qfThreadInfo", 12) == 0)
                    gdb_send(g, "m1");
                else if (strncmp(pkt, "qsThreadInfo", 12) == 0)
                    gdb_send(g, "l");
                else
                    gdb_send(g, "");
                break;
            case 'H': /* set thread */
                gdb_send(g, "OK");
                break;
            case '?':
                gdb_send(g, "S05");
                break;
            case 'g':
                send_regs(g, c);
                break;
            case 'G': {
                /* Restore all registers from hex */
                int off = 1;
                for (int i = 0; i < 16; ++i) {
                    u32 v = 0;
                    for (int j = 0; j < 4; ++j) {
                        v |= hex_to_u32(pkt + off + j * 2, 2) << (j * 8);
                    }
                    c->r[i] = v;
                    off += 8;
                }
                gdb_send(g, "OK");
                break;
            }
            case 'p': {
                int reg = (int)hex_to_u32(pkt + 1, (int)strlen(pkt) - 1);
                char out[9];
                u32 v = (reg < 16) ? c->r[reg] :
                        (reg == 16) ? ((c->apsr & 0xF8000000u) | c->ipsr | c->epsr) : 0;
                hex32_le(v, out); out[8] = 0;
                gdb_send(g, out);
                break;
            }
            case 'P': {
                /* P<reg>=<hex> */
                char* eq = strchr(pkt, '=');
                if (!eq) { gdb_send(g, "E22"); break; }
                int reg = (int)hex_to_u32(pkt + 1, (int)(eq - pkt - 1));
                u32 v = 0;
                for (int j = 0; j < 4; ++j)
                    v |= hex_to_u32(eq + 1 + j * 2, 2) << (j * 8);
                if (reg < 16) c->r[reg] = v;
                gdb_send(g, "OK");
                break;
            }
            case 'm': {
                /* m<addr>,<len> */
                char* comma = strchr(pkt, ',');
                if (!comma) { gdb_send(g, "E22"); break; }
                u32 addr = hex_to_u32(pkt + 1, (int)(comma - pkt - 1));
                u32 len = hex_to_u32(comma + 1, (int)strlen(comma + 1));
                send_mem(g, b, addr, len);
                break;
            }
            case 'M': {
                /* M<addr>,<len>:<hex> */
                char* comma = strchr(pkt, ',');
                char* colon = strchr(pkt, ':');
                if (!comma || !colon) { gdb_send(g, "E22"); break; }
                u32 addr = hex_to_u32(pkt + 1, (int)(comma - pkt - 1));
                u32 len = hex_to_u32(comma + 1, (int)(colon - comma - 1));
                write_mem(g, b, addr, len, colon + 1);
                break;
            }
            case 'Z': /* set breakpoint: Z0,<addr>,<kind> */
                if (pkt[1] == '0') {
                    char* comma = strchr(pkt + 3, ',');
                    if (!comma) { gdb_send(g, "E22"); break; }
                    u32 addr = hex_to_u32(pkt + 3, (int)(comma - pkt - 3));
                    if (g->bp_n < 32) {
                        g->bp[g->bp_n++] = addr;
                        gdb_send(g, "OK");
                    } else gdb_send(g, "E16");
                } else gdb_send(g, "");
                break;
            case 'z': /* remove breakpoint */
                if (pkt[1] == '0') {
                    char* comma = strchr(pkt + 3, ',');
                    if (!comma) { gdb_send(g, "E22"); break; }
                    u32 addr = hex_to_u32(pkt + 3, (int)(comma - pkt - 3));
                    for (int i = 0; i < g->bp_n; ++i) {
                        if (g->bp[i] == addr) {
                            g->bp[i] = g->bp[--g->bp_n];
                            break;
                        }
                    }
                    gdb_send(g, "OK");
                } else gdb_send(g, "");
                break;
            case 'c':
                g->stepping = false;
                g->halted_for_gdb = false;
                return;
            case 's':
                g->stepping = true;
                g->halted_for_gdb = false;
                return;
            case 'k':
                gdb_close(g);
                exit(0);
            default:
                gdb_send(g, ""); /* unsupported */
                break;
        }
    }
}
