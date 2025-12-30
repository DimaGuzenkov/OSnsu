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

/* Клиент для тестирования HTTP-proxy */
int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Использование: %s <proxy_host> <proxy_port> <target_url>\n", argv[0]);
        fprintf(stderr, "Пример: %s localhost 8080 http://example.com/\n", argv[0]);
        return 1;
    }
    
    const char *proxy_host = argv[1];
    int proxy_port = atoi(argv[2]);
    const char *target_url = argv[3];
    
    printf("Подключаюсь к прокси %s:%d\n", proxy_host, proxy_port);
    printf("Запрашиваю URL: %s\n", target_url);
    
    // Создаем сокет
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Ошибка создания сокета");
        return 1;
    }
    
    // Получаем адрес прокси
    struct hostent *proxy = gethostbyname(proxy_host);
    if (!proxy) {
        fprintf(stderr, "Не удалось разрешить хост: %s\n", proxy_host);
        close(sock);
        return 1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr.s_addr, proxy->h_addr, proxy->h_length);
    addr.sin_port = htons(proxy_port);
    
    // Подключаемся к прокси
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Ошибка подключения к прокси");
        close(sock);
        return 1;
    }
    
    printf("Подключение к прокси установлено\n");
    
    // Формируем HTTP-запрос для прокси
    char request[2048];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: dummy\r\n"  // Host заголовок требуется, но прокси его заменит
             "User-Agent: Test-Client/1.0\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "\r\n",
             target_url);
    
    printf("Отправляю запрос:\n%s\n", request);
    
    // Отправляем запрос
    if (send(sock, request, strlen(request), 0) < 0) {
        perror("Ошибка отправки запроса");
        close(sock);
        return 1;
    }
    
    printf("Запрос отправлен, ожидаю ответ...\n");
    
    // Получаем ответ
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    int total_bytes = 0;
    int header_printed = 0;
    
    // Неблокирующий режим для таймаута
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        total_bytes += bytes_received;
        buffer[bytes_received] = '\0';
        
        // Выводим только первые 2000 байт для краткости
        if (total_bytes < 2000) {
            printf("%s", buffer);
        } else if (!header_printed) {
            // Выводим только заголовки
            char *header_end = strstr(buffer, "\r\n\r\n");
            if (header_end) {
                *header_end = '\0';
                printf("%s\r\n\r\n", buffer);
                header_printed = 1;
                printf("... (данные опущены, всего получено %d байт)\n", total_bytes);
            }
        }
    }
    
    if (bytes_received < 0) {
        perror("Ошибка при чтении ответа");
    }
    
    printf("\nВсего получено: %d байт\n", total_bytes);
    
    close(sock);
    return 0;
}