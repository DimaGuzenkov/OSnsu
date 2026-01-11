#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/time.h>
#include <http_parser.h>

#define MAX_URL 1024
#define MAX_HOST 256
#define MAX_PORT 16
#define CHUNK_SZ 8192
#define HEADER_BUFFER_SIZE 4096

#define CACHE_MAX_BYTES (20 * 1024 * 1024)

// Структура для хранения информации о HTTP-ответе
typedef struct {
    int status_code;
    size_t content_length;
    int headers_complete;
    int is_chunked;
    int keep_alive;
} http_response_info;

// Структура для парсинга
typedef struct {
    http_response_info info;
    char *headers;
    size_t headers_len;
    size_t body_received;
} parse_context;

// Структура записи кэша
typedef struct cache_entry {
    char url[MAX_URL];

    // HTTP заголовки и данные
    char *headers;
    size_t headers_len;
    char *data;
    size_t size;
    size_t capacity;

    int complete;
    int cacheable;
    int refcount;

    pthread_mutex_t lock;
    pthread_cond_t cond;

    struct cache_entry *next;
} cache_entry;

/* ================= GLOBAL CACHE ================= */
static cache_entry *cache_table = NULL;
static pthread_mutex_t cache_table_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t cache_used_bytes = 0;
static pthread_mutex_t cache_mem_lock = PTHREAD_MUTEX_INITIALIZER;

/* ================= LOGGING HELPERS ================= */
static void log_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    char buffer[26];
    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    printf("[%s.%03ld] ", buffer, tv.tv_usec / 1000);
}

static void log_message(const char *category, const char *message) {
    log_time();
    printf("[%s] %s\n", category, message);
}

/* ================= HTTP PARSER CALLBACKS ================= */
static int on_status(http_parser *parser, const char *at, size_t length) {
    parse_context *ctx = (parse_context *)parser->data;
    // Можно сохранить статус сообщение если нужно
    return 0;
}

static int on_header_field(http_parser *parser, const char *at, size_t length) {
    parse_context *ctx = (parse_context *)parser->data;
    // Добавляем заголовок в буфер
    if (ctx->headers) {
        size_t new_len = ctx->headers_len + length;
        char *new_headers = realloc(ctx->headers, new_len + 1);
        if (new_headers) {
            ctx->headers = new_headers;
            memcpy(ctx->headers + ctx->headers_len, at, length);
            ctx->headers_len = new_len;
            ctx->headers[ctx->headers_len] = '\0';
        }
    }
    return 0;
}

static int on_header_value(http_parser *parser, const char *at, size_t length) {
    parse_context *ctx = (parse_context *)parser->data;
    // Добавляем значение заголовка в буфер
    if (ctx->headers) {
        size_t new_len = ctx->headers_len + length;
        char *new_headers = realloc(ctx->headers, new_len + 1);
        if (new_headers) {
            ctx->headers = new_headers;
            memcpy(ctx->headers + ctx->headers_len, at, length);
            ctx->headers_len = new_len;
            ctx->headers[ctx->headers_len] = '\0';
        }
    }
    return 0;
}

static int on_headers_complete(http_parser *parser) {
    parse_context *ctx = (parse_context *)parser->data;
    ctx->info.status_code = parser->status_code;
    ctx->info.content_length = parser->content_length;
    ctx->info.headers_complete = 1;
    ctx->info.is_chunked = (parser->flags & F_CHUNKED);
    ctx->info.keep_alive = http_should_keep_alive(parser);
    
    log_message("PARSER", "Headers complete");
    return 0;
}

static int on_body(http_parser *parser, const char *at, size_t length) {
    parse_context *ctx = (parse_context *)parser->data;
    ctx->body_received += length;
    return 0;
}

static int on_message_complete(http_parser *parser) {
    parse_context *ctx = (parse_context *)parser->data;
    log_message("PARSER", "Message complete");
    return 0;
}

/* ================= HELPER FUNCTIONS ================= */
static int send_all(int fd, const void *buf, size_t len) {
    size_t off = 0;
    const char *p = buf;
    while (off < len) {
        ssize_t s = send(fd, p + off, len - off, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)s;
    }
    return 0;
}

static int parse_url(const char *url, char *host, char *port, char *path) {
    // Парсим URL в формате http://host:port/path
    if (strncmp(url, "http://", 7) != 0) {
        return -1;
    }
    
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    if (!slash) {
        // Нет пути, используем "/"
        strcpy(path, "/");
        slash = p + strlen(p);
    } else {
        strcpy(path, slash);
    }
    
    const char *colon = memchr(p, ':', slash - p);
    if (colon) {
        // Есть порт
        size_t host_len = colon - p;
        if (host_len >= MAX_HOST) return -1;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
        
        size_t port_len = slash - colon - 1;
        if (port_len >= MAX_PORT) return -1;
        strncpy(port, colon + 1, port_len);
        port[port_len] = '\0';
    } else {
        // Нет порта, используем 80
        size_t host_len = slash - p;
        if (host_len >= MAX_HOST) return -1;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
        strcpy(port, "80");
    }
    
    return 0;
}

/* ================= CACHE MANAGEMENT ================= */
static void cache_remove_entry(cache_entry *e) {
    cache_entry **pp = &cache_table;
    while (*pp && *pp != e) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = e->next;
    }

    pthread_mutex_lock(&cache_mem_lock);
    cache_used_bytes -= e->capacity;
    pthread_mutex_unlock(&cache_mem_lock);

    log_message("CACHE", "Entry evicted");
    
    free(e->headers);
    free(e->data);
    pthread_mutex_destroy(&e->lock);
    pthread_cond_destroy(&e->cond);
    free(e);
}

static void cache_evict_if_needed(size_t needed) {
    int attempts = 0;
    while (attempts < 10) { // Ограничим попытки
        pthread_mutex_lock(&cache_mem_lock);
        size_t available = CACHE_MAX_BYTES - cache_used_bytes;
        pthread_mutex_unlock(&cache_mem_lock);

        if (available >= needed) {
            return;
        }

        cache_entry *victim = NULL;
        pthread_mutex_lock(&cache_table_lock);
        
        cache_entry *e = cache_table;
        while (e) {
            if (e->complete && e->refcount == 0) {
                victim = e;
                break;
            }
            e = e->next;
        }

        if (victim) {
            cache_remove_entry(victim);
            pthread_mutex_unlock(&cache_table_lock);
        } else {
            pthread_mutex_unlock(&cache_table_lock);
            log_message("CACHE", "No evictable entries found");
            break;
        }
        
        attempts++;
    }
}

static cache_entry *cache_lookup(const char *url) {
    pthread_mutex_lock(&cache_table_lock);

    cache_entry *e = cache_table;
    while (e) {
        if (strcmp(e->url, url) == 0) {
            e->refcount++;
            pthread_mutex_unlock(&cache_table_lock);
            
            if (e->complete) {
                if (e->cacheable) {
                    log_message("CACHE", "Hit (cached)");
                } else {
                    log_message("CACHE", "Hit (not cacheable)");
                }
            } else {
                log_message("CACHE", "Hit (in progress)");
            }
            return e;
        }
        e = e->next;
    }

    pthread_mutex_unlock(&cache_table_lock);
    log_message("CACHE", "Miss");
    return NULL;
}

/* ================= CONNECTION HELPERS ================= */
static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        log_message("CONNECTION", "getaddrinfo failed");
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }
    
    // Устанавливаем таймауты
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(sock);
        return -1;
    }
    
    freeaddrinfo(res);
    return sock;
}

/* ================= FETCHER THREAD ================= */
static void *fetcher_thread(void *arg) {
    cache_entry *e = arg;
    
    char host[MAX_HOST], port[MAX_PORT], path[MAX_URL];
    if (parse_url(e->url, host, port, path) < 0) {
        log_message("FETCHER", "Failed to parse URL");
        pthread_mutex_lock(&e->lock);
        e->complete = 1;
        e->cacheable = 0;
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
        return NULL;
    }

    // Подключаемся к серверу
    int sock = connect_to_server(host, port);
    if (sock < 0) {
        log_message("FETCHER", "Connection failed");
        pthread_mutex_lock(&e->lock);
        e->complete = 1;
        e->cacheable = 0;
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
        return NULL;
    }

    // Отправляем запрос
    char request[1024];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);
    
    if (send_all(sock, request, strlen(request)) < 0) {
        log_message("FETCHER", "Failed to send request");
        close(sock);
        pthread_mutex_lock(&e->lock);
        e->complete = 1;
        e->cacheable = 0;
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
        return NULL;
    }

    // Парсим ответ с помощью http-parser
    http_parser_settings settings = {0};
    settings.on_status = on_status;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = on_message_complete;
    
    http_parser parser;
    http_parser_init(&parser, HTTP_RESPONSE);
    
    parse_context ctx = {0};
    ctx.headers = malloc(1);
    ctx.headers[0] = '\0';
    parser.data = &ctx;
    
    // Читаем данные и парсим
    char buffer[CHUNK_SZ];
    ssize_t bytes_read;
    int parse_result = 0;
    
    while (!ctx.info.headers_complete && 
           (bytes_read = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        
        size_t parsed = http_parser_execute(&parser, &settings, buffer, bytes_read);
        if (parsed != bytes_read) {
            log_message("FETCHER", "HTTP parsing error");
            parse_result = -1;
            break;
        }
        
        if (ctx.info.headers_complete) {
            // Проверяем, можно ли кэшировать
            if (ctx.info.status_code != 200) {
                e->cacheable = 0;
                log_message("FETCHER", "Non-200 status code");
            } else if (ctx.info.is_chunked) {
                e->cacheable = 0;
                log_message("FETCHER", "Chunked encoding not supported for caching");
            } else if (ctx.info.content_length == 0) {
                e->cacheable = 0;
                log_message("FETCHER", "No content length or zero length");
            } else if (ctx.info.content_length > CACHE_MAX_BYTES) {
                e->cacheable = 0;
                log_message("FETCHER", "Content too large for cache");
            } else {
                // Пытаемся выделить память
                size_t total_size = ctx.info.content_length;
                cache_evict_if_needed(total_size);
                
                pthread_mutex_lock(&cache_mem_lock);
                if (cache_used_bytes + total_size <= CACHE_MAX_BYTES) {
                    e->data = malloc(total_size);
                    if (e->data) {
                        e->capacity = total_size;
                        e->size = 0;
                        cache_used_bytes += total_size;
                        e->cacheable = 1;
                        log_message("FETCHER", "Memory allocated for caching");
                    } else {
                        e->cacheable = 0;
                        log_message("FETCHER", "Memory allocation failed");
                    }
                } else {
                    e->cacheable = 0;
                    log_message("FETCHER", "Not enough cache space");
                }
                pthread_mutex_unlock(&cache_mem_lock);
            }
            
            break;
        }
    }
    
    // Если не можем кэшировать, закрываем соединение
    if (!e->cacheable || parse_result < 0) {
        close(sock);
        free(ctx.headers);
        pthread_mutex_lock(&e->lock);
        e->complete = 1;
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
        return NULL;
    }
    
    // Если можем кэшировать, продолжаем чтение
    size_t total_read = 0;
    while (total_read < ctx.info.content_length && 
           (bytes_read = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        
        pthread_mutex_lock(&e->lock);
        
        size_t to_copy = bytes_read;
        if (total_read + to_copy > e->capacity) {
            to_copy = e->capacity - total_read;
        }
        
        if (to_copy > 0) {
            memcpy(e->data + total_read, buffer, to_copy);
            total_read += to_copy;
            e->size = total_read;
        }
        
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
    }
    
    close(sock);
    free(ctx.headers);
    
    // Проверяем, все ли данные получены
    pthread_mutex_lock(&e->lock);
    if (total_read == ctx.info.content_length) {
        e->complete = 1;
        log_message("FETCHER", "Fetch completed successfully");
    } else {
        // Не все данные получены, освобождаем память
        pthread_mutex_lock(&cache_mem_lock);
        cache_used_bytes -= e->capacity;
        pthread_mutex_unlock(&cache_mem_lock);
        
        free(e->data);
        e->data = NULL;
        e->size = 0;
        e->capacity = 0;
        e->cacheable = 0;
        e->complete = 1;
        log_message("FETCHER", "Incomplete data received");
    }
    
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
    
    return NULL;
}

/* ================= CLIENT THREAD ================= */
static void *client_thread(void *arg) {
    int cfd = (intptr_t)arg;
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    getpeername(cfd, (struct sockaddr*)&client_addr, &client_len);
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);
    
    log_message("CLIENT", "New connection");
    
    // Читаем запрос
    char req[4096];
    ssize_t rr = recv(cfd, req, sizeof(req) - 1, 0);
    if (rr <= 0) {
        log_message("CLIENT", "Connection closed");
        close(cfd);
        return NULL;
    }
    req[rr] = 0;
    
    // Парсим запрос (очень просто)
    char method[16], url[MAX_URL];
    if (sscanf(req, "%15s %1023s", method, url) != 2) {
        log_message("CLIENT", "Invalid request");
        close(cfd);
        return NULL;
    }
    
    log_message("CLIENT", url);
    
    // Ищем в кэше
    cache_entry *e = cache_lookup(url);
    
    if (!e) {
        log_message("CLIENT", "Creating new cache entry");
        e = calloc(1, sizeof(*e));
        strncpy(e->url, url, MAX_URL - 1);
        e->url[MAX_URL - 1] = '\0';
        e->cacheable = 1;
        e->refcount = 1;
        pthread_mutex_init(&e->lock, NULL);
        pthread_cond_init(&e->cond, NULL);
        
        pthread_mutex_lock(&cache_table_lock);
        e->next = cache_table;
        cache_table = e;
        pthread_mutex_unlock(&cache_table_lock);
        
        pthread_t th;
        pthread_create(&th, NULL, fetcher_thread, e);
        pthread_detach(th);
    }
    
    // Ждем завершения загрузки
    pthread_mutex_lock(&e->lock);
    while (!e->complete) {
        pthread_cond_wait(&e->cond, &e->lock);
    }
    
    int cacheable = e->cacheable;
    char *cached_data = e->data;
    size_t cached_size = e->size;
    pthread_mutex_unlock(&e->lock);
    
    if (cacheable && cached_data && cached_size > 0) {
        // Отправляем данные из кэша
        send_all(cfd, cached_data, cached_size);
        log_message("CLIENT", "Sent from cache");
    } else {
        // Прямой стриминг
        log_message("CLIENT", "Direct streaming (not cacheable)");
        // В реальности нужно реализовать прямой стриминг
        const char *error_msg = "HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        send_all(cfd, error_msg, strlen(error_msg));
    }
    
    close(cfd);
    
    // Уменьшаем счетчик ссылок
    pthread_mutex_lock(&e->lock);
    e->refcount--;
    pthread_mutex_unlock(&e->lock);
    
    return NULL;
}

/* ================= MAIN ================= */
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }
    
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("bind");
        close(s);
        return 1;
    }
    
    if (listen(s, 128) < 0) {
        perror("listen");
        close(s);
        return 1;
    }
    
    log_message("SERVER", "Proxy server started");
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(s, (struct sockaddr*)&client_addr, &client_len);
        
        if (cfd < 0) {
            log_message("SERVER", "Accept failed");
            continue;
        }
        
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, (void*)(intptr_t)cfd) != 0) {
            log_message("SERVER", "Failed to create thread");
            close(cfd);
            continue;
        }
        pthread_detach(th);
    }
    
    close(s);
    return 0;
}