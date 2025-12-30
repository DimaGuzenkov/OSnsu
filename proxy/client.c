#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define BUFFER_SIZE 8192

/* Простая функция для отправки всех данных */
static int send_all(int sock, const void *buf, size_t len) {
    size_t sent = 0;
    const char *ptr = buf;
    
    while (sent < len) {
        ssize_t n = send(sock, ptr + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += n;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <proxy_host> <proxy_port> <url>\n", argv[0]);
        fprintf(stderr, "Example: %s localhost 8080 http://example.com/\n", argv[0]);
        return 1;
    }
    
    const char *proxy_host = argv[1];
    const char *proxy_port_str = argv[2];
    const char *url = argv[3];
    
    int proxy_port = atoi(proxy_port_str);
    if (proxy_port <= 0 || proxy_port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", proxy_port_str);
        return 1;
    }
    
    printf("Connecting to proxy %s:%d\n", proxy_host, proxy_port);
    printf("Requesting URL: %s\n", url);
    
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    // Get proxy address
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      // Use IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP
    
    int ret = getaddrinfo(proxy_host, proxy_port_str, &hints, &result);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        close(sock);
        return 1;
    }
    
    // Connect to proxy
    if (connect(sock, result->ai_addr, result->ai_addrlen) < 0) {
        perror("connect");
        freeaddrinfo(result);
        close(sock);
        return 1;
    }
    
    freeaddrinfo(result);
    printf("Connected to proxy\n");
    
    // Build HTTP request for proxy
    char request[2048];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: dummy\r\n"
             "User-Agent: Test-Client/1.0\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "\r\n",
             url);
    
    printf("Sending request:\n%s\n", request);
    
    // Send request
    if (send_all(sock, request, strlen(request)) < 0) {
        perror("send");
        close(sock);
        return 1;
    }
    
    printf("Request sent, waiting for response...\n");
    
    // Receive response
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    int total_bytes = 0;
    int header_printed = 0;
    
    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    // Read all data
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        total_bytes += bytes_received;
        buffer[bytes_received] = '\0';
        
        // Print only first 2000 bytes for brevity
        if (total_bytes < 2000) {
            printf("%s", buffer);
        } else if (!header_printed) {
            // Print only headers
            char *header_end = strstr(buffer, "\r\n\r\n");
            if (header_end) {
                *header_end = '\0';
                printf("%s\r\n\r\n", buffer);
                header_printed = 1;
                printf("... (data truncated, total %d bytes received)\n", total_bytes);
            }
        }
    }
    
    if (bytes_received < 0) {
        perror("recv");
    }
    
    printf("\nTotal received: %d bytes\n", total_bytes);
    
    close(sock);
    return 0;
}