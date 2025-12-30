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

/* ================= DIRECT FETCH FUNCTION ================= */

static int direct_fetch_to_client(const char *url, int client_fd) {
    log_fetcher(url, "Starting direct fetch to client");

    char host[MAX_HOST], port[MAX_PORT], path[MAX_URL];
    if (parse_url(url, host, port, path) < 0) {
        log_fetcher(url, "Failed to parse URL");
        return -1;
    }

    log_connection(host, atoi(port), "Resolving address for direct fetch");

    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        log_connection(host, atoi(port), "DNS resolution failed");
        return -1;
    }

    log_connection(host, atoi(port), "Creating socket for direct fetch");

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        log_connection(host, atoi(port), "Socket creation failed");
        freeaddrinfo(res);
        return -1;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        log_connection(host, atoi(port), "Connection failed");
        freeaddrinfo(res);
        close(sock);
        return -1;
    }
    freeaddrinfo(res);

    log_connection(host, atoi(port), "Connected, sending request");

    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    
    if (send_all(sock, req, strlen(req)) < 0) {
        log_connection(host, atoi(port), "Request send failed");
        close(sock);
        return -1;
    }

    char buf[CHUNK_SZ];
    ssize_t r;
    size_t total_received = 0;

    log_fetcher(url, "Starting to stream data to client");

    while ((r = recv(sock, buf, sizeof(buf), 0)) > 0) {
        total_received += r;
        
        if (send_all(client_fd, buf, r) < 0) {
            log_fetcher(url, "Failed to send data to client");
            close(sock);
            return -1;
        }
        
        if (total_received % (CHUNK_SZ * 10) == 0) {
            log_fetcher(url, "Streamed chunk to client");
        }
    }

    close(sock);
    
    if (r < 0) {
        log_fetcher(url, "Error reading from server");
        return -1;
    }

    log_fetcher(url, "Direct fetch completed successfully");
    return 0;
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

    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        log_connection(host, atoi(port), "DNS resolution failed");
        goto done;
    }

    log_connection(host, atoi(port), "Creating socket");

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        log_connection(host, atoi(port), "Connection failed");
        freeaddrinfo(res);
        close(sock);
        goto done;
    }
    freeaddrinfo(res);

    log_connection(host, atoi(port), "Connected, sending request");

    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    send_all(sock, req, strlen(req));

    char buf[CHUNK_SZ];
    ssize_t r;
    size_t total_received = 0;

    while ((r = recv(sock, buf, sizeof(buf), 0)) > 0) {
        pthread_mutex_lock(&e->lock);

        if (e->cacheable) {
            if (e->size + r > e->capacity) {
                size_t newcap = e->capacity ? e->capacity * 2 : 16384;
                while (newcap < e->size + r)
                    newcap *= 2;

                log_fetcher(e->url, "Cache needs expansion");
                cache_evict_if_needed(newcap - e->capacity);

                pthread_mutex_lock(&cache_mem_lock);
                if (cache_used_bytes + (newcap - e->capacity) <= CACHE_MAX_BYTES) {
                    cache_used_bytes += (newcap - e->capacity);
                    e->data = realloc(e->data, newcap);
                    e->capacity = newcap;
                    log_fetcher(e->url, "Cache expanded successfully");
                } else {
                    /* Не хватило места в кэше - отключаем кэширование и освобождаем память */
                    e->cacheable = 0;
                    if (e->data) {
                        cache_used_bytes -= e->capacity;
                        free(e->data);
                        e->data = NULL;
                        e->size = 0;
                        e->capacity = 0;
                    }
                    log_fetcher(e->url, "Cache full, disabling caching for this entry");
                }
                pthread_mutex_unlock(&cache_mem_lock);
            }

            if (e->cacheable) {
                memcpy(e->data + e->size, buf, r);
                e->size += r;
                total_received += r;
                
                if (total_received % (CHUNK_SZ * 10) == 0) {
                    log_fetcher(e->url, "Received chunk");
                }
            }
        }

        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
    }

    close(sock);

done:
    pthread_mutex_lock(&e->lock);
    e->complete = 1;
    
    if (e->cacheable && e->data) {
        log_cache(e->url, "STORED", e->size);
        log_fetcher(e->url, "Fetch completed successfully");
    } else {
        log_fetcher(e->url, "Fetch completed (not cached)");
    }
    
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
    return NULL;
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
    sscanf(req, "%15s %1023s", method, url);
    
    log_client(client_ip, client_port, url);

    cache_entry *e = cache_lookup(url);
    if (!e) {
        /* Проверяем, есть ли место в кэше для нового элемента */
        pthread_mutex_lock(&cache_mem_lock);
        int cache_has_space = (cache_used_bytes < CACHE_MAX_BYTES);
        pthread_mutex_unlock(&cache_mem_lock);
        
        if (!cache_has_space) {
            log_client(client_ip, client_port, "Cache full, using direct fetch");
            direct_fetch_to_client(url, cfd);
            close(cfd);
            return NULL;
        }
        
        log_client(client_ip, client_port, "Creating new cache entry");
        e = calloc(1, sizeof(*e));
        strcpy(e->url, url);
        e->cacheable = 1;
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

    size_t offset = 0;
    size_t total_sent = 0;
    while (1) {
        pthread_mutex_lock(&e->lock);
        
        /* Если кэширование отключено и данных нет, нужно перейти на прямую пересылку */
        if (!e->cacheable && !e->complete && offset == 0) {
            pthread_mutex_unlock(&e->lock);
            
            log_client(client_ip, client_port, "Switching to direct fetch (cache disabled)");
            
            /* Уменьшаем счетчик ссылок перед переходом на прямую пересылку */
            pthread_mutex_lock(&e->lock);
            e->refcount--;
            pthread_mutex_unlock(&e->lock);
            
            /* Выполняем прямую пересылку */
            direct_fetch_to_client(url, cfd);
            close(cfd);
            return NULL;
        }
        
        while (!e->complete && offset >= e->size)
            pthread_cond_wait(&e->cond, &e->lock);

        size_t avail = e->size - offset;
        int done = e->complete && offset >= e->size;
        pthread_mutex_unlock(&e->lock);

        if (avail > 0) {
            if (send_all(cfd, e->data + offset, avail) < 0)
                break;
            offset += avail;
            total_sent += avail;
            
            if (total_sent % (CHUNK_SZ * 5) == 0) {
                log_client(client_ip, client_port, "Sending data...");
            }
        }
        if (done)
            break;
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

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    bind(s, (struct sockaddr*)&sa, sizeof(sa));
    listen(s, 128);

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
        pthread_create(&th, NULL, client_thread, (void*)(intptr_t)cfd);
        pthread_detach(th);
    }
}