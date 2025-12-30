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

#define MAX_URL 1024
#define MAX_HOST 256
#define MAX_PORT 16
#define CHUNK_SZ 8192

#define CACHE_MAX_BYTES (20 * 1024 * 1024)

typedef struct cache_entry {
    char url[MAX_URL];

    char *data;
    size_t size;
    size_t capacity;

    int complete;
    int cacheable;
    size_t content_length;

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

static void log_client(const char *client_ip, int client_port, const char *message) {
    log_time();
    printf("[CLIENT %s:%d] %s\n", client_ip, client_port, message);
}

static void log_cache(const char *url, const char *operation, size_t size) {
    log_time();
    printf("[CACHE %s] %s", operation, url);
    if (size > 0) {
        printf(" (%zu bytes)", size);
    }
    printf("\n");
}

static void log_fetcher(const char *url, const char *message) {
    log_time();
    printf("[FETCHER %s] %s\n", url, message);
}

static void log_connection(const char *host, int port, const char *message) {
    log_time();
    printf("[CONNECTION %s:%d] %s\n", host, port, message);
}

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
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;

    const char *slash = strchr(p, '/');
    if (!slash) return -1;

    const char *colon = memchr(p, ':', slash - p);
    if (colon) {
        snprintf(host, MAX_HOST, "%.*s", (int)(colon - p), p);
        snprintf(port, MAX_PORT, "%.*s", (int)(slash - colon - 1), colon + 1);
    } else {
        snprintf(host, MAX_HOST, "%.*s", (int)(slash - p), p);
        strcpy(port, "80");
    }
    snprintf(path, MAX_URL, "%s", slash);
    return 0;
}

static void cache_remove_entry(cache_entry *e) {
    cache_entry **pp = &cache_table;
    while (*pp && *pp != e)
        pp = &(*pp)->next;
    if (*pp) *pp = e->next;

    pthread_mutex_lock(&cache_mem_lock);
    cache_used_bytes -= e->capacity;
    pthread_mutex_unlock(&cache_mem_lock);

    log_cache(e->url, "EVICTED", e->capacity);
    log_time();
    printf("[CACHE STATS] Cache usage: %zu/%zu bytes (%.1f%%)\n", 
           cache_used_bytes, CACHE_MAX_BYTES,
           (double)cache_used_bytes / CACHE_MAX_BYTES * 100);

    free(e->data);
    pthread_mutex_destroy(&e->lock);
    pthread_cond_destroy(&e->cond);
    free(e);
}

static void cache_evict_if_needed(size_t needed) {
    int evicted = 0;
    while (1) {
        pthread_mutex_lock(&cache_mem_lock);
        size_t available = CACHE_MAX_BYTES - cache_used_bytes;
        pthread_mutex_unlock(&cache_mem_lock);

        if (available >= needed)
            return;

        cache_entry *victim = NULL;

        pthread_mutex_lock(&cache_table_lock);
        for (cache_entry *e = cache_table; e; e = e->next) {
            if (e->complete && e->refcount == 0) {
                victim = e;
                break;
            }
        }

        if (!victim) {
            pthread_mutex_unlock(&cache_table_lock);
            if (!evicted) {
                log_time();
                printf("[CACHE WARNING] No evictable entries found, cache full!\n");
            }
            return;
        }

        cache_remove_entry(victim);
        pthread_mutex_unlock(&cache_table_lock);
        evicted++;
    }
}

static cache_entry *cache_lookup(const char *url) {
    pthread_mutex_lock(&cache_table_lock);

    cache_entry *e = cache_table;
    while (e) {
        if (strcmp(e->url, url) == 0) {
            e->refcount++;
            pthread_mutex_unlock(&cache_table_lock);
            log_cache(url, "HIT", e->size);
            return e;
        }
        e = e->next;
    }

    pthread_mutex_unlock(&cache_table_lock);
    log_cache(url, "MISS", 0);
    return NULL;
}

static int extract_content_length(char *headers, size_t headers_len) {
    // Ищем Content-Length в заголовках
    char *content_start = strstr(headers, "Content-Length:");
    if (!content_start) {
        content_start = strstr(headers, "Content-length:");
    }
    
    if (!content_start) {
        return -1;
    }
    
    // Пропускаем "Content-Length:" и пробелы
    char *value_start = content_start + 15;
    while (*value_start == ' ' || *value_start == '\t' || *value_start == ':') {
        value_start++;
    }
    
    // Конвертируем в число
    return atoi(value_start);
}

static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }
    
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(sock);
        return -1;
    }
    freeaddrinfo(res);
    
    return sock;
}

static void *fetcher_thread(void *arg) {
    cache_entry *e = arg;
    
    log_fetcher(e->url, "Starting fetch");

    char host[MAX_HOST], port[MAX_PORT], path[MAX_URL];
    if (parse_url(e->url, host, port, path) < 0) {
        log_fetcher(e->url, "Failed to parse URL");
        goto done;
    }

    log_connection(host, atoi(port), "Resolving address");

    int sock = connect_to_server(host, port);
    if (sock < 0) {
        log_connection(host, atoi(port), "Connection failed");
        goto done;
    }

    log_connection(host, atoi(port), "Connected, sending request");

    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    send_all(sock, req, strlen(req));

    char buf[CHUNK_SZ];
    ssize_t r;
    int headers_complete = 0;
    size_t header_len = 0;
    size_t total_received = 0;
    
    // Читаем ответ, находим заголовки
    while (!headers_complete && (r = recv(sock, buf, sizeof(buf), 0)) > 0) {
        char *header_end = memmem(buf, r, "\r\n\r\n", 4);
        if (header_end) {
            headers_complete = 1;
            header_len = header_end - buf + 4;
            
            // Анализируем заголовки
            char headers[CHUNK_SZ];
            if (header_len < sizeof(headers)) {
                memcpy(headers, buf, header_len);
                headers[header_len] = '\0';
                
                int content_length = extract_content_length(headers, header_len);
                if (content_length <= 0) {
                    log_fetcher(e->url, "No Content-Length header or invalid value, cannot cache");
                    e->cacheable = 0;
                    // Освобождаем сокет для прямого стриминга
                    close(sock);
                    goto done;
                }
                
                if (content_length > CACHE_MAX_BYTES) {
                    log_fetcher(e->url, "File too large for cache, streaming directly");
                    e->cacheable = 0;
                    close(sock);
                    goto done;
                }
                
                e->content_length = content_length;
                
                // Пытаемся выделить память для кэша
                cache_evict_if_needed(content_length);
                
                pthread_mutex_lock(&cache_mem_lock);
                if (cache_used_bytes + content_length <= CACHE_MAX_BYTES) {
                    e->data = malloc(content_length);
                    e->capacity = content_length;
                    cache_used_bytes += content_length;
                    e->cacheable = 1;
                    log_fetcher(e->url, "Memory allocated for caching");
                } else {
                    e->cacheable = 0;
                    log_fetcher(e->url, "Not enough cache space, streaming directly");
                }
                pthread_mutex_unlock(&cache_mem_lock);
                
                if (!e->cacheable) {
                    close(sock);
                    goto done;
                }
                
                // Копируем тело после заголовков
                size_t body_bytes = r - header_len;
                if (body_bytes > 0) {
                    pthread_mutex_lock(&e->lock);
                    memcpy(e->data, buf + header_len, body_bytes);
                    e->size = body_bytes;
                    total_received = body_bytes;
                    pthread_cond_broadcast(&e->cond);
                    pthread_mutex_unlock(&e->lock);
                }
                
                // Если получили весь файл сразу
                if (body_bytes == content_length) {
                    pthread_mutex_lock(&e->lock);
                    e->complete = 1;
                    pthread_cond_broadcast(&e->cond);
                    pthread_mutex_unlock(&e->lock);
                    close(sock);
                    log_cache(e->url, "STORED", e->size);
                    log_fetcher(e->url, "Fetch completed in first chunk");
                    return NULL;
                }
            }
        }
    }
    
    // Продолжаем чтение если файл кэшируемый
    if (e->cacheable) {
        while ((r = recv(sock, buf, sizeof(buf), 0)) > 0) {
            pthread_mutex_lock(&e->lock);
            
            if (e->size + r <= e->capacity) {
                memcpy(e->data + e->size, buf, r);
                e->size += r;
                total_received += r;
                
                if (total_received % (CHUNK_SZ * 10) == 0) {
                    log_fetcher(e->url, "Received chunk");
                }
                
                pthread_cond_broadcast(&e->cond);
                pthread_mutex_unlock(&e->lock);
                
                if (e->size == e->content_length) {
                    pthread_mutex_lock(&e->lock);
                    e->complete = 1;
                    pthread_cond_broadcast(&e->cond);
                    pthread_mutex_unlock(e->lock);
                    break;
                }
            } else {
                log_fetcher(e->url, "Error: Received more data than expected");
                e->cacheable = 0;
                pthread_cond_broadcast(&e->cond);
                pthread_mutex_unlock(&e->lock);
                break;
            }
        }
    }

    close(sock);

    if (e->cacheable && e->size == e->content_length) {
        pthread_mutex_lock(&e->lock);
        e->complete = 1;
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
        log_cache(e->url, "STORED", e->size);
        log_fetcher(e->url, "Fetch completed and cached successfully");
    } else if (e->cacheable) {
        log_fetcher(e->url, "Error: Incomplete data received, not caching");
        pthread_mutex_lock(&cache_mem_lock);
        cache_used_bytes -= e->capacity;
        pthread_mutex_unlock(&cache_mem_lock);
        free(e->data);
        e->data = NULL;
        e->size = 0;
        e->capacity = 0;
        e->cacheable = 0;
        pthread_mutex_lock(&e->lock);
        e->complete = 1;
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
    }

done:
    pthread_mutex_lock(&e->lock);
    if (!e->complete) {
        e->complete = 1;
    }
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
    return NULL;
}

static void stream_directly(int client_fd, const char *url) {
    char host[MAX_HOST], port[MAX_PORT], path[MAX_URL];
    if (parse_url(url, host, port, path) < 0) {
        const char *err = "HTTP/1.0 400 Bad Request\r\nConnection: close\r\n\r\n";
        send_all(client_fd, err, strlen(err));
        return;
    }

    int sock = connect_to_server(host, port);
    if (sock < 0) {
        const char *err = "HTTP/1.0 500 Internal Server Error\r\nConnection: close\r\n\r\n";
        send_all(client_fd, err, strlen(err));
        return;
    }

    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    send_all(sock, req, strlen(req));

    char buf[CHUNK_SZ];
    ssize_t r;
    int headers_complete = 0;
    size_t header_len = 0;
    
    // Отправляем клиенту свои заголовки
    const char *hdr = "HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n";
    send_all(client_fd, hdr, strlen(hdr));
    
    while ((r = recv(sock, buf, sizeof(buf), 0)) > 0) {
        if (!headers_complete) {
            char *header_end = memmem(buf, r, "\r\n\r\n", 4);
            if (header_end) {
                headers_complete = 1;
                header_len = header_end - buf + 4;
                
                // Отправляем только тело (после заголовков)
                size_t body_bytes = r - header_len;
                if (body_bytes > 0) {
                    send_all(client_fd, buf + header_len, body_bytes);
                }
            }
        } else {
            send_all(client_fd, buf, r);
        }
    }

    close(sock);
}

static void *client_thread(void *arg) {
    int cfd = (intptr_t)arg;
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    getpeername(cfd, (struct sockaddr*)&client_addr, &client_len);
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);
    
    log_client(client_ip, client_port, "New connection");

    char req[4096];
    ssize_t rr = recv(cfd, req, sizeof(req) - 1, 0);
    if (rr <= 0) {
        log_client(client_ip, client_port, "Connection closed");
        close(cfd);
        return NULL;
    }
    req[rr] = 0;

    char method[16], url[MAX_URL];
    if (sscanf(req, "%15s %1023s", method, url) != 2) {
        log_client(client_ip, client_port, "Invalid request");
        close(cfd);
        return NULL;
    }
    
    log_client(client_ip, client_port, url);

    cache_entry *e = cache_lookup(url);
    if (!e) {
        log_client(client_ip, client_port, "Creating new cache entry");
        e = calloc(1, sizeof(*e));
        strcpy(e->url, url);
        e->cacheable = 1;  // Пытаемся кэшировать по умолчанию
        e->refcount = 1;
        pthread_mutex_init(&e->lock, NULL);
        pthread_cond_init(&e->cond, NULL);

        pthread_mutex_lock(&cache_table_lock);
        e->next = cache_table;
        cache_table = e;
        
        log_time();
        printf("[CACHE STATS] Total cache entries: ");
        int count = 0;
        cache_entry *temp = cache_table;
        while (temp) {
            count++;
            temp = temp->next;
        }
        printf("%d\n", count);
        
        pthread_mutex_unlock(&cache_table_lock);

        pthread_t th;
        pthread_create(&th, NULL, fetcher_thread, e);
        pthread_detach(th);
    } else {
        log_client(client_ip, client_port, "Using cached entry");
    }

    pthread_mutex_lock(&e->lock);
    
    while (!e->complete) {
        pthread_cond_wait(&e->cond, &e->lock);
    }
    
    int cacheable = e->cacheable;
    char *cached_data = e->data;
    size_t cached_size = e->size;
    pthread_mutex_unlock(&e->lock);
    
    if (cacheable && cached_data && cached_size > 0) {
        const char *hdr = "HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n";
        send_all(cfd, hdr, strlen(hdr));
        send_all(cfd, cached_data, cached_size);
    } else {
        log_client(client_ip, client_port, "Streaming directly (not cached)");
        stream_directly(cfd, url);
    }

    log_client(client_ip, client_port, "Request completed");
    close(cfd);

    pthread_mutex_lock(&e->lock);
    e->refcount--;
    log_cache(e->url, "REFCOUNT DECREMENT", e->refcount);
    pthread_mutex_unlock(&e->lock);

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
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

    log_time();
    printf("[SERVER] Starting proxy server on port %d\n", port);
    log_time();
    printf("[CACHE STATS] Cache limit: %zu bytes\n", CACHE_MAX_BYTES);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(s, (struct sockaddr*)&client_addr, &client_len);
        
        if (cfd < 0) {
            log_time();
            printf("[SERVER ERROR] Accept failed\n");
            continue;
        }
        
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, (void*)(intptr_t)cfd) != 0) {
            log_time();
            printf("[SERVER ERROR] Failed to create thread\n");
            close(cfd);
            continue;
        }
        pthread_detach(th);
    }
}