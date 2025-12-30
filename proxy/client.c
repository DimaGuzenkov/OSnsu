#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Использование: %s <proxy_ip> <proxy_port> <url> [output_file]\n", argv[0]);
        printf("Пример: %s 127.0.0.1 8080 http://example.com/ output.html\n", argv[0]);
        return 1;
    }
    
    const char *proxy_ip = argv[1];
    int proxy_port = atoi(argv[2]);
    const char *url = argv[3];
    const char *output_file = (argc > 4) ? argv[4] : "output.html";
    
    printf("Proxy: %s:%d\n", proxy_ip, proxy_port);
    printf("URL: %s\n", url);
    printf("Output: %s\n", output_file);
    
    // Создаем сокет
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    // Настраиваем адрес прокси
    struct sockaddr_in proxy_addr;
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port = htons(proxy_port);
    
    if (inet_pton(AF_INET, proxy_ip, &proxy_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return 1;
    }
    
    // Подключаемся к прокси
    if (connect(sock, (struct sockaddr*)&proxy_addr, sizeof(proxy_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    
    printf("Connected to proxy\n");
    
    // Формируем HTTP-запрос
    char request[1024];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: dummy\r\n"
             "User-Agent: Simple-Client/1.0\r\n"
             "Connection: close\r\n"
             "\r\n",
             url);
    
    // Отправляем запрос
    if (send(sock, request, strlen(request), 0) < 0) {
        perror("send");
        close(sock);
        return 1;
    }
    
    printf("Request sent\n");
    
    // Открываем файл для записи
    FILE *fp = fopen(output_file, "wb");
    if (!fp) {
        perror("fopen");
        close(sock);
        return 1;
    }
    
    // Получаем и сохраняем ответ
    char buffer[4096];
    int total_bytes = 0;
    int bytes;
    
    while ((bytes = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytes, fp);
        total_bytes += bytes;
        printf("Received: %d bytes\n", bytes);
    }
    
    if (bytes < 0) {
        perror("recv");
    }
    
    printf("Total saved: %d bytes to %s\n", total_bytes, output_file);
    
    // Закрываем всё
    fclose(fp);
    close(sock);
    
    return 0;
}