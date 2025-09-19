#include <arpa/inet.h>
#include <signal.h>
#include <srt/srt.h>
#include <stdio.h>

#define MAX_CLIENTS 11000
#define TS_PACKET_SIZE 1316
#define CLIENT_QUEUE_LEN 1024

volatile int running = 1;

void handle_sigint(int unused) { running = 0; }

typedef struct {
  SRTSOCKET sock;
  int head, tail, count;
  char packets[CLIENT_QUEUE_LEN][TS_PACKET_SIZE];
  int active;
} Client;

void remove_client(Client clients[], int idx, int free_list[], int *free_list_count) {
  if (clients[idx].sock != SRT_INVALID_SOCK) {
    srt_close(clients[idx].sock);
  }
  clients[idx].sock = SRT_INVALID_SOCK;
  clients[idx].active = 0;
  free_list[(*free_list_count)++] = idx;
}


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

int main(int argc, char **argv) {
  if (argc != 3) {
    printf("Usage: %s <source_port> <client_port>\n", argv[0]);
    return 1;
  }

  int src_port = atoi(argv[1]);
  int client_port = atoi(argv[2]);

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

  Client* clients = (Client*)malloc(sizeof(Client)*MAX_CLIENTS);
  int free_list[MAX_CLIENTS];
  int free_list_count = MAX_CLIENTS;
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    clients[i].sock = SRT_INVALID_SOCK;
    clients[i].active = 0;
    free_list[i] = i;
  }


  // allocate space for up to MAX_CLIENTS + 2 (src + listener) sockets
  SRTSOCKET read_socks[MAX_CLIENTS + 2];
  SRTSOCKET write_socks[MAX_CLIENTS];
  SYSSOCKET lrfds[MAX_CLIENTS + 2], lwfds[MAX_CLIENTS];
  int rnum, wnum, lrnum, lwnum;

  char ts_packet[TS_PACKET_SIZE];

  char tmp_buf[TS_PACKET_SIZE * 10]; // buffer to store fragmented packets
  int tmp_len = 0;

  while (running) {
    rnum = MAX_CLIENTS + 2;
    wnum = MAX_CLIENTS;
    lrnum = MAX_CLIENTS + 2;
    lwnum = MAX_CLIENTS;

    int r =
      srt_epoll_wait(eid, read_socks, &rnum, write_socks, &wnum,
               100, lrfds, &lrnum, lwfds, &lwnum);

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
        if (free_list_count > 0) {
          SRTSOCKET new_client = srt_accept(client_listener, NULL, NULL);
          if (new_client != SRT_INVALID_SOCK) {
            int idx = free_list[--free_list_count];
            clients[idx].sock = new_client;
            clients[idx].head = 0;
            clients[idx].tail = 0;
            clients[idx].count = 0;
            clients[idx].active = 1;
            srt_setsockflag(clients[idx].sock, SRTO_SNDBUF, &bufsize, sizeof(bufsize));
            srt_setsockflag(clients[idx].sock, SRTO_RCVBUF, &bufsize, sizeof(bufsize));

            // Add the new client socket to the epoll instance
            int client_events = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
            srt_epoll_add_usock(eid, new_client, &client_events);

            printf("New client connected: %d\n", idx);
          }
        }
      }
      // Handle incoming data from the source
      else if (current_sock == src_sock) {
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
            for (int j = 0; j < MAX_CLIENTS; ++j) {
              if (clients[j].active) {
                enqueue_packet(&clients[j], ts_packet);
              }
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
        for (int j = 0; j < MAX_CLIENTS; ++j) {
          if (clients[j].active && clients[j].sock == current_sock) {
            printf("Client %d disconnected. Removing.\n", j);
            srt_epoll_remove_usock(eid, current_sock);
            remove_client(clients, j, free_list, &free_list_count);
            break;
          }
        }
      }
    }

    // 3) Send packets to clients
    for (int i = 0; i < MAX_CLIENTS; ++i) {
      if (!clients[i].active) continue;
      char send_buf[TS_PACKET_SIZE];
      int len;
      while ((len = dequeue_packet(&clients[i], send_buf)) > 0) {
        int sent = srt_send(clients[i].sock, send_buf, len);
        if (sent < 0) {
          int error = srt_getlasterror(NULL);
          if (error != SRT_EASYNCSND) {
            printf("Client disconnected or slow. Removing.\n");
            srt_epoll_remove_usock(eid, clients[i].sock);
            remove_client(clients, i, free_list, &free_list_count);
            break;
          }
        }
      }
    }

    usleep(1000);
  }

  // Cleanup
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (clients[i].active) {
      srt_close(clients[i].sock);
    }
  }

  free(clients);
  srt_close(src_sock);
  srt_close(src_listener);
  srt_close(client_listener);
  srt_epoll_release(eid);
  srt_cleanup();
  printf("Hub terminated.\n");
  return 0;
}