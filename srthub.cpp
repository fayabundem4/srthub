#include <arpa/inet.h>
#include <signal.h>
#include <srt/srt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#define MAX_CLIENTS 512
#define TS_PACKET_SIZE 1316
// #define TS_PACKET_SIZE 188
#define CLIENT_QUEUE_LEN 1024

volatile int running = 1;

void handle_sigint(int sig) { running = 0; }

typedef struct {
  SRTSOCKET sock;
  int head, tail, count;
  char packets[CLIENT_QUEUE_LEN][TS_PACKET_SIZE];
} Client;

void enqueue_packet(Client *c, const char *data) {
  if (c->count == CLIENT_QUEUE_LEN) {
    c->head = (c->head + 1) % CLIENT_QUEUE_LEN;
  } else {
    c->count++;
  }
  memcpy(c->packets[c->tail], data, TS_PACKET_SIZE);
  c->tail = (c->tail + 1) % CLIENT_QUEUE_LEN;
}

int dequeue_packet(Client *c, char *out) {
  if (c->count == 0)
    return 0;
  memcpy(out, c->packets[c->head], TS_PACKET_SIZE);
  c->head = (c->head + 1) % CLIENT_QUEUE_LEN;
  c->count--;
  return TS_PACKET_SIZE;
}

void remove_client(std::vector<Client> &clients, int idx) {
  if (clients[idx].sock != SRT_INVALID_SOCK) {
    srt_close(clients[idx].sock);
  }
  clients.erase(clients.begin() + idx);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    printf("Usage: %s <source_port> <client_port> <latency_ms>\n", argv[0]);
    return 1;
  }

  int src_port = atoi(argv[1]);
  int client_port = atoi(argv[2]);
  int latency = atoi(argv[3]);

  signal(SIGINT, handle_sigint);
  if (srt_startup() != 0) {
    printf("SRT startup failed\n");
    return 1;
  }

  // ===== Source socket setup =====
  SRTSOCKET src_listener = srt_create_socket();
  if (src_listener == SRT_INVALID_SOCK) {
    perror("src socket");
    srt_cleanup();
    return 1;
  }

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(src_port);
  sa.sin_addr.s_addr = INADDR_ANY;

  if (srt_bind(src_listener, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
    perror("src bind");
    srt_close(src_listener);
    srt_cleanup();
    return 1;
  }
  if (srt_listen(src_listener, 1) == SRT_ERROR) {
    perror("src listen");
    srt_close(src_listener);
    srt_cleanup();
    return 1;
  }
  printf("Waiting for source on port %d...\n", src_port);

  SRTSOCKET src_sock = srt_accept(src_listener, NULL, NULL);
  if (src_sock == SRT_INVALID_SOCK) {
    perror("src accept");
    srt_close(src_listener);
    srt_cleanup();
    return 1;
  }
  printf("Source connected\n");

  srt_setsockflag(src_sock, SRTO_LATENCY, &latency, sizeof(latency));
  int bufsize = 16 * 1024 * 1024;
  srt_setsockflag(src_sock, SRTO_RCVBUF, &bufsize, sizeof(bufsize));
  srt_setsockflag(src_sock, SRTO_SNDBUF, &bufsize, sizeof(bufsize));

  // ===== Client listener setup =====
  SRTSOCKET client_listener = srt_create_socket();
  if (client_listener == SRT_INVALID_SOCK) {
    perror("client socket");
    srt_close(src_sock);
    srt_close(src_listener);
    srt_cleanup();
    return 1;
  }
  sa.sin_port = htons(client_port);
  if (srt_bind(client_listener, (struct sockaddr *)&sa, sizeof(sa)) ==
      SRT_ERROR) {
    perror("client bind");
    srt_close(src_sock);
    srt_close(src_listener);
    srt_close(client_listener);
    srt_cleanup();
    return 1;
  }
  if (srt_listen(client_listener, MAX_CLIENTS) == SRT_ERROR) {
    perror("client listen");
    srt_close(src_sock);
    srt_close(src_listener);
    srt_close(client_listener);
    srt_cleanup();
    return 1;
  }
  printf("Listening for clients on port %d...\n", client_port);

  // ===== Create an epoll instance and register sockets =====
  int eid = srt_epoll_create();
  if (eid < 0) {
    perror("epoll create");
    srt_cleanup();
    return 1;
  }

  int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
  srt_epoll_add_usock(eid, src_sock, &events);
  srt_epoll_add_usock(eid, client_listener, &events);

  std::vector<Client> clients;

  // allocate space for up to MAX_CLIENTS + 2 (src + listener) sockets
  std::vector<SRTSOCKET> read_socks(MAX_CLIENTS + 2);
  std::vector<SRTSOCKET> write_socks(MAX_CLIENTS);
  std::vector<SYSSOCKET> lrfds(MAX_CLIENTS + 2), lwfds(MAX_CLIENTS);
  int rnum, wnum, lrnum, lwnum;

  char ts_packet[TS_PACKET_SIZE];

  char tmp_buf[TS_PACKET_SIZE * 10]; // buffer to store fragmented packets
  int tmp_len = 0;

  while (running) {
    rnum = read_socks.size();
    wnum = write_socks.size();
    lrnum = lrfds.size();
    lwnum = lwfds.size();

    int r =
        srt_epoll_wait(eid, read_socks.data(), &rnum, write_socks.data(), &wnum,
                       100, lrfds.data(), &lrnum, lwfds.data(), &lwnum);

    if (r < 0) {
      if (srt_getlasterror(NULL) != SRT_ETIMEOUT) {
        printf("Epoll wait error: %s\n", srt_getlasterror_str());
      }
      continue;
    }

    for (int i = 0; i < rnum; ++i) {
      SRTSOCKET current_sock = read_socks[i];

      // Handle new client connections
      if (current_sock == client_listener) {
        if (clients.size() < MAX_CLIENTS) {
          SRTSOCKET new_client = srt_accept(client_listener, NULL, NULL);
          if (new_client != SRT_INVALID_SOCK) {
            Client c;
            c.sock = new_client;
            c.head = 0;
            c.tail = 0;
            c.count = 0;
            srt_setsockflag(c.sock, SRTO_LATENCY, &latency, sizeof(latency));
            srt_setsockflag(c.sock, SRTO_SNDBUF, &bufsize, sizeof(bufsize));
            srt_setsockflag(c.sock, SRTO_RCVBUF, &bufsize, sizeof(bufsize));
            clients.push_back(c);

            // Add the new client socket to the epoll instance
            int client_events = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
            srt_epoll_add_usock(eid, new_client, &client_events);

            printf("New client connected: %zu\n", clients.size());
          }
        }
      }
      // Handle incoming data from the source
      else if (current_sock == src_sock) {
        char recv_buf[1500];
        int bytes_received =
            srt_recv(src_sock, tmp_buf + tmp_len, sizeof(tmp_buf) - tmp_len);
        if (bytes_received < 0) {
          int error = srt_getlasterror(NULL);
          if (error != SRT_EASYNCRCV) {
            printf("Source disconnected! Error: %s\n", srt_getlasterror_str());
            running = 0;
          }
        } else if (bytes_received > 0) {
          tmp_len += bytes_received;
          while (tmp_len >= TS_PACKET_SIZE) {
            // We have a full TS packet, process it
            memcpy(ts_packet, tmp_buf, TS_PACKET_SIZE);

            // Enqueue to all clients
            for (auto &client : clients) {
              enqueue_packet(&client, ts_packet);
            }

            // Shift remaining data to the beginning of the buffer
            memmove(tmp_buf, tmp_buf + TS_PACKET_SIZE,
                    tmp_len - TS_PACKET_SIZE);
            tmp_len -= TS_PACKET_SIZE;
          }
        }
      }
      // Handle client disconnections or errors
      else {
        for (size_t j = 0; j < clients.size(); ++j) {
          if (clients[j].sock == current_sock) {
            printf("Client disconnected. Removing.\n");
            srt_epoll_remove_usock(eid, current_sock);
            remove_client(clients, j);
            break;
          }
        }
      }
    }

    // 3) Send packets to clients
    for (size_t i = 0; i < clients.size(); ++i) {
      Client &c = clients[i];
      char send_buf[TS_PACKET_SIZE];
      int len;
      while ((len = dequeue_packet(&c, send_buf)) > 0) {
        int sent = srt_send(c.sock, send_buf, len);
        if (sent < 0) {
          int error = srt_getlasterror(NULL);
          if (error != SRT_EASYNCSND) {
            printf("Client disconnected or slow. Removing.\n");
            srt_epoll_remove_usock(eid, c.sock);
            remove_client(clients, i);
            i--;
            break;
          }
        }
      }
    }

    usleep(100);
  }

  // Cleanup
  for (auto &c : clients) {
    srt_close(c.sock);
  }
  srt_close(src_sock);
  srt_close(src_listener);
  srt_close(client_listener);
  srt_epoll_release(eid);
  srt_cleanup();
  printf("Hub terminated.\n");
  return 0;
}