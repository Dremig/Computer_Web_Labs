#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <netdb.h>

#define MAX_CLIENTS 100
#define PORT 6221

struct Client {
    int id;
    char ip[16];
    int port;
    int sock;
};

struct Client client_list[MAX_CLIENTS];
int num_clients = 0;
int next_id = 1;
pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

void send_packet(int s, uint8_t type, const void* payload, uint16_t payload_len) {
    uint16_t total_len = htons(3 + payload_len);
    char header[3];
    header[0] = type;
    memcpy(header + 1, &total_len, 2);
    send(s, header, 3, 0);
    if (payload_len > 0) send(s, payload, payload_len, 0);
}

int recv_packet(int s, uint8_t* type, void** payload, uint16_t* payload_len) {
    char h[3];
    int n = recv(s, h, 3, 0);
    if (n < 3) return -1;
    *type = h[0];
    uint16_t total_len;
    memcpy(&total_len, h + 1, 2);
    total_len = ntohs(total_len);
    *payload_len = total_len - 3;
    *payload = malloc(*payload_len);
    n = recv(s, *payload, *payload_len, 0);
    if (n != (int)*payload_len) {
        free(*payload);
        return -1;
    }
    return 0;
}

void* handle_client(void* arg) {
    int index = (int)(intptr_t)arg;
    int sock = client_list[index].sock;
    int my_id = client_list[index].id;
    while (1) {
        uint8_t type;
        void* pay = NULL;
        uint16_t plen;
        if (recv_packet(sock, &type, &pay, &plen) < 0) break;
        char* data = (char*)pay;  // Cast to char* for safe arithmetic in C++
        switch (type) {
            case 1: {  // time
                time_t t = time(NULL);
                char time_str[32];
                strcpy(time_str, ctime(&t));
                time_str[strlen(time_str) - 1] = 0;  
                send_packet(sock, 10, time_str, strlen(time_str));
                free(pay);
                break;
            }
            case 2: {  // name
                char hostname[256];
                gethostname(hostname, sizeof(hostname));
                send_packet(sock, 11, hostname, strlen(hostname));
                free(pay);
                break;
            }
            case 3: {  // list
                pthread_mutex_lock(&list_mutex);
                uint32_t num_net = htonl(num_clients);
                char payload_buf[4096];
                int off = 4;
                memcpy(payload_buf, &num_net, 4);
                for (int i = 0; i < num_clients; i++) {
                    uint32_t id_net = htonl(client_list[i].id);
                    uint16_t port_net = htons(client_list[i].port);
                    uint16_t ip_len_net = htons(strlen(client_list[i].ip));
                    memcpy(payload_buf + off, &id_net, 4); off += 4;
                    memcpy(payload_buf + off, &port_net, 2); off += 2;
                    memcpy(payload_buf + off, &ip_len_net, 2); off += 2;
                    memcpy(payload_buf + off, client_list[i].ip, ntohs(ip_len_net)); off += ntohs(ip_len_net);
                }
                send_packet(sock, 12, payload_buf, off);
                pthread_mutex_unlock(&list_mutex);
                free(pay);
                break;
            }
            case 4: {  // message
                if (plen < 6) {
                    free(pay);
                    break;
                }
                uint32_t target_id;
                memcpy(&target_id, data, 4);
                target_id = ntohl(target_id);
                uint16_t msg_len_net;
                memcpy(&msg_len_net, data + 4, 2);
                uint16_t msg_len = ntohs(msg_len_net);
                if (plen != 6 + msg_len) {
                    free(pay);
                    break;
                }
                char* msg = data + 6;
                pthread_mutex_lock(&list_mutex);
                int target_sock = -1;
                for (int i = 0; i < num_clients; i++) {
                    if (client_list[i].id == target_id) {
                        target_sock = client_list[i].sock;
                        break;
                    }
                }
                if (target_sock != -1) {
                    char fpay[1024];
                    int foff = 0;
                    uint32_t sid_net = htonl(my_id);
                    memcpy(fpay + foff, &sid_net, 4); foff += 4;
                    uint16_t sp_net = htons(client_list[index].port);
                    memcpy(fpay + foff, &sp_net, 2); foff += 2;
                    uint16_t si_len_net = htons(strlen(client_list[index].ip));
                    memcpy(fpay + foff, &si_len_net, 2); foff += 2;
                    memcpy(fpay + foff, client_list[index].ip, ntohs(si_len_net)); foff += ntohs(si_len_net);
                    uint16_t m_len_net = htons(msg_len);
                    memcpy(fpay + foff, &m_len_net, 2); foff += 2;
                    memcpy(fpay + foff, msg, msg_len); foff += msg_len;
                    send_packet(target_sock, 13, fpay, foff);
                }
                pthread_mutex_unlock(&list_mutex);
                free(pay);
                break;
            }
            default:
                free(pay);
                break;
        }
    }
    // remove a client from the list
    pthread_mutex_lock(&list_mutex);
    for (int i = index; i < num_clients - 1; i++) {
        client_list[i] = client_list[i + 1];
    }
    num_clients--;
    pthread_mutex_unlock(&list_mutex);
    close(sock);
    return NULL;
}

int main() {
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_sock, 5) < 0) {
        perror("listen");
        return 1;
    }
    printf("服务器启动，监听端口: %d\n", PORT);
    printf("等待客户端连接...\n");
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &len);
        if (client_sock < 0) continue;
        pthread_mutex_lock(&list_mutex);
        if (num_clients < MAX_CLIENTS) {
            client_list[num_clients].id = next_id++;
            strcpy(client_list[num_clients].ip, inet_ntoa(client_addr.sin_addr));
            client_list[num_clients].port = ntohs(client_addr.sin_port);
            client_list[num_clients].sock = client_sock;
            int index = num_clients;
            num_clients++;
            pthread_mutex_unlock(&list_mutex);
            pthread_t thread;
            pthread_create(&thread, NULL, handle_client, (void*)(intptr_t)index);
            printf("新客户端连接: ID=%d, IP=%s, Port=%d\n", client_list[index].id, client_list[index].ip, client_list[index].port);
        } else {
            pthread_mutex_unlock(&list_mutex);
            close(client_sock);
        }
    }
    close(listen_sock);
    pthread_mutex_destroy(&list_mutex);
    return 0;
}