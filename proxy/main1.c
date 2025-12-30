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

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    int complete;
} client_buffer;

static void process_http_response(int sock, cache_entry *e, int client_fd) {
    char buf[CHUNK_SZ];
    ssize_t r;
    int headers_complete = 0;
    size_t content_length = 0;
    int chunked = 0;
    char *body_start = NULL;
    size_t header_len = 0;
    
    client_buffer client_buf = {0};
    
    while ((r = recv(sock, buf, sizeof(buf), 0)) > 0) {
        if (!headers_complete) {
            char *header_end = memmem(buf, r, "\r\n\r\n", 4);
            if (header_end) {
                headers_complete = 1;
                header_len = header_end - buf + 4;
                body_start = buf + header_len;
                
                char headers[CHUNK_SZ];
                memcpy(headers, buf, header_len);
                headers[header_len] = '\0';
                
                char *line = strtok(headers, "\r\n");
                while (line) {
                    if (strncasecmp(line, "Content-Length:", 15) == 0) {
                        content_length = atol(line + 15);
                        break;
                    }
                    line = strtok(NULL, "\r\n");
                }
                
                if (content_length == 0) {
                    log_fetcher(e->url, "No Content-Length header, cannot cache");
                    e->cacheable = 0;
                } else if (content_length > CACHE_MAX_BYTES) {
                    log_fetcher(e->url, "File too large for cache, streaming directly");
                    e->cacheable = 0;
                } else {
                    log_fetcher(e->url, "Content-Length found, attempting to cache");
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
                }
                
                size_t body_bytes = r - header_len;
                
                if (e->cacheable && body_bytes > 0) {
                    memcpy(e->data, body_start, body_bytes);
                    e->size = body_bytes;
                }
                
                if (!e->cacheable) {
                    if (body_bytes > 0) {
                        client_buf.data = malloc(body_bytes);
                        memcpy(client_buf.data, body_start, body_bytes);
                        client_buf.size = body_bytes;
                        client_buf.capacity = body_bytes;
                        send_all(client_fd, client_buf.data, client_buf.size);
                        free(client_buf.data);
                        client_buf.data = NULL;
                        client_buf.size = 0;
                    }
                }
            } else {
                if (!e->cacheable) {
                    send_all(client_fd, buf, r);
                }
            }
        } else {
            if (e->cacheable) {
                if (e->size + r <= e->capacity) {
                    memcpy(e->data + e->size, buf, r);
                    e->size += r;
                } else {
                    log_fetcher(e->url, "Error: Received more data than expected");
                    e->cacheable = 0;
                    free(e->data);
                    e->data = NULL;
                    e->size = 0;
                    e->capacity = 0;
                    send_all(client_fd, buf, r);
                }
            } else {
                send_all(client_fd, buf, r);
            }
        }
    }
    
    if (e->cacheable && e->size == content_length) {
        e->complete = 1;
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
    } else {
        log_fetcher(e->url, "Fetch completed (not cached)");
    }
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
    
    int client_fd = -1;
    
    process_http_response(sock, e, client_fd);

    close(sock);

done:
    pthread_mutex_lock(&e->lock);
    e->complete = 1;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
    return NULL;
}

static void send_data_to_client(int client_fd, cache_entry *e) {
    const char *hdr = "HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n";
    send_all(client_fd, hdr, strlen(hdr));
    
    if (e->cacheable && e->data) {
        send_all(client_fd, e->data, e->size);
    } else {
        pthread_mutex_lock(&e->lock);
        while (!e->complete || e->size == 0) {
            pthread_cond_wait(&e->cond, &e->lock);
        }
        
        if (e->data) {
            send_all(client_fd, e->data, e->size);
        }
        pthread_mutex_unlock(&e->lock);
    }
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

    send_data_to_client(cfd, e);

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