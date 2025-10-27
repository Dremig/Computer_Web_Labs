#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <queue>
#include <mutex>
#include <vector>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

int sock = -1;
int connected = 0;
pthread_t recv_thread;
int signal_pipe[2];  // For recv_thread to signal main for re-prompt

struct Message {
    uint8_t type;
    std::vector<char> data;
};

std::queue<Message> msg_queue;
std::mutex queue_mutex;

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

void print_menu_connected() {
    printf("请选择操作:\n2. 断开\n3. 获取时间\n4. 获取名字\n5. 获取列表\n6. 发消息\n7. 退出\n");
}

void print_menu_unconnected() {
    printf("请选择操作:\n1. 连接\n7. 退出\n");
}

void print_prompt() {
    printf("请输入选择: ");
    fflush(stdout);
}

void process_sync_response() {
    std::vector<Message> pending;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        while (!msg_queue.empty()) {
            pending.push_back(std::move(msg_queue.front()));
            msg_queue.pop();
        }
    }
    for (auto& m : pending) {
        char* buf = m.data.data();
        int plen = m.data.size();
        switch (m.type) {
            case 10: {  // 时间
                printf("Server time: %.*s\n", plen, buf);
                break;
            }
            case 11: {  // 名字
                printf("Server name: %.*s\n", plen, buf);
                break;
            }
            case 12: {  // 列表
                if (plen < 4) break;
                uint32_t num;
                memcpy(&num, buf, 4);
                num = ntohl(num);
                printf("Active connections:\n");
                int off = 4;
                for (uint32_t i = 0; i < num && off < plen; ++i) {
                    if (off + 8 > plen) break;
                    uint32_t cid;
                    memcpy(&cid, buf + off, 4); cid = ntohl(cid); off += 4;
                    uint16_t cport;
                    memcpy(&cport, buf + off, 2); cport = ntohs(cport); off += 2;
                    uint16_t ciplen;
                    memcpy(&ciplen, buf + off, 2); ciplen = ntohs(ciplen); off += 2;
                    char cip[16] = {0};
                    int ip_bytes = (ciplen > 15 ? 15 : ciplen);
                    if (off + ip_bytes <= plen) {
                        memcpy(cip, buf + off, ip_bytes);
                    }
                    off += ciplen;
                    printf("ID: %u, IP: %s, Port: %d\n", cid, cip, cport);
                }
                break;
            }
        }
    }
}

void print_async_message(uint8_t type, void* pay, uint16_t plen) {
    if (type != 13) return;
    char* buf = (char*)pay;
    int off = 0;
    if (plen < 12) return;
    uint32_t sid;
    memcpy(&sid, buf + off, 4); sid = ntohl(sid); off += 4;
    uint16_t sport;
    memcpy(&sport, buf + off, 2); sport = ntohs(sport); off += 2;
    uint16_t siplen;
    memcpy(&siplen, buf + off, 2); siplen = ntohs(siplen); off += 2;
    char sip[16] = {0};
    int sip_bytes = (siplen > 15 ? 15 : siplen);
    if (off + sip_bytes <= plen) {
        memcpy(sip, buf + off, sip_bytes);
    }
    off += siplen;
    if (off + 2 > plen) return;
    uint16_t mlen;
    memcpy(&mlen, buf + off, 2); mlen = ntohs(mlen); off += 2;
    char* msg = buf + off;
    int msg_bytes = (mlen > 255 ? 255 : mlen);
    printf("\nMessage from client %u (%s:%d): %.*s\n", sid, sip, sport, msg_bytes, msg);
    fflush(stdout);
    // Signal main to re-print menu/prompt
    char sig = 'm';
    write(signal_pipe[1], &sig, 1);
}

void* recv_thread_func(void* arg) {
    while (1) {
        uint8_t type;
        void* pay = NULL;
        uint16_t plen;
        if (recv_packet(sock, &type, &pay, &plen) < 0) break;
        if (type == 13) {
            // Async: print immediately and signal
            print_async_message(type, pay, plen);
        } else {
            // Sync: queue
            std::lock_guard<std::mutex> lock(queue_mutex);
            Message m;
            m.type = type;
            m.data.resize(plen);
            if (plen > 0) {
                memcpy(m.data.data(), pay, plen);
            }
            msg_queue.push(std::move(m));
        }
        free(pay);
    }
    return NULL;
}

void wait_for_sync_response() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        bool has_sync = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            has_sync = !msg_queue.empty();
        }
        if (has_sync) {
            process_sync_response();
            usleep(10000);
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!msg_queue.empty()) {
                    process_sync_response();
                }
            }
            break;
        }
        usleep(50000);
    }
}

int read_choice(bool is_connected) {
    if (is_connected) {
        print_menu_connected();
    } else {
        print_menu_unconnected();
    }
    print_prompt();
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int max_fd = STDIN_FILENO + 1;
        if (is_connected) {
            FD_SET(signal_pipe[0], &fds);
            max_fd = (signal_pipe[0] > STDIN_FILENO ? signal_pipe[0] : STDIN_FILENO) + 1;
        }
        struct timeval tv = {0, 100000};  // 100ms poll
        int ret = select(max_fd, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            if (is_connected && FD_ISSET(signal_pipe[0], &fds)) {
                char dummy;
                read(signal_pipe[0], &dummy, 1);
                printf("\n");  // Newline after message
                if (is_connected) {
                    print_menu_connected();
                } else {
                    print_menu_unconnected();
                }
                print_prompt();
                fflush(stdout);
            }
            if (FD_ISSET(STDIN_FILENO, &fds)) {
                char line[10];
                if (fgets(line, sizeof(line), stdin) != NULL) {
                    line[strcspn(line, "\n")] = 0;
                    int choice = atoi(line);
                    return choice;
                }
            }
        } else if (ret < 0 && errno != EINTR) {
            perror("select");
            return -1;
        }
        // Poll for sync responses occasionally
        if (is_connected) {
            process_sync_response();
        }
    }
    return -1;
}

int main() {
    if (pipe(signal_pipe) < 0) {
        perror("pipe");
        return 1;
    }
    fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int choice;
    while (1) {
        process_sync_response();
        choice = read_choice(connected);
        if (choice < 0) continue;
        if (!connected) {
            if (choice == 1) {
                char ip[16];
                int port;
                printf("服务器 IP: "); 
                scanf("%s", ip);
                int c; while ((c = getchar()) != '\n' && c != EOF);
                printf("端口: "); 
                scanf("%d", &port);
                while ((c = getchar()) != '\n' && c != EOF);
                struct sockaddr_in server_addr;
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(port);
                server_addr.sin_addr.s_addr = inet_addr(ip);
                if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                    connected = 1;
                    pthread_create(&recv_thread, &attr, recv_thread_func, NULL);
                    printf("连接成功。\n");
                    process_sync_response();
                } else {
                    printf("连接失败。\n");
                }
            } else if (choice == 7) {
                break;
            }
        } else {
            if (choice == 2) {
                close(sock);
                connected = 0;
                process_sync_response();
                sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) {
                    perror("socket");
                    return 1;
                }
            } else if (choice == 3) {
                send_packet(sock, 1, NULL, 0);
                wait_for_sync_response();
            } else if (choice == 4) {
                send_packet(sock, 2, NULL, 0);
                wait_for_sync_response();
            } else if (choice == 5) {
                send_packet(sock, 3, NULL, 0);
                wait_for_sync_response();
            } else if (choice == 6) {
                int tid;
                printf("目标 ID: ");
                scanf("%d", &tid);
                int cc; while ((cc = getchar()) != '\n' && cc != EOF);
                char msg[256];
                printf("消息: ");
                if (fgets(msg, sizeof(msg), stdin) != NULL) {
                    msg[strcspn(msg, "\n")] = 0;
                    uint32_t tid_net = htonl(tid);
                    uint16_t mlen = strlen(msg);
                    uint16_t mlen_net = htons(mlen);
                    char* pay = (char*)malloc(6 + mlen);
                    memcpy(pay, &tid_net, 4);
                    memcpy(pay + 4, &mlen_net, 2);
                    memcpy(pay + 6, msg, mlen);
                    send_packet(sock, 4, pay, 6 + mlen);
                    free(pay);
                    printf("消息已发送。\n");
                }
            } else if (choice == 7) {
                close(sock);
                connected = 0;
                process_sync_response();
                break;
            }
        }
    }
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    if (sock >= 0) close(sock);
    pthread_attr_destroy(&attr);
    return 0;
}